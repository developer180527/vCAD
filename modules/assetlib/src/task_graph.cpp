#include "assetlib/task_graph.h"
#include "cook/env.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <queue>
#include <thread>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#elif defined(__APPLE__)
    #include <pthread/qos.h>
    #include <sys/sysctl.h>
#elif defined(__linux__)
    #include <unistd.h>
    #include <pthread.h>
#endif

namespace assetlib {

namespace {

// Total physical RAM in bytes (0 if unknown).
size_t physicalRamBytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (::GlobalMemoryStatusEx(&ms) && ms.ullTotalPhys > 0)
        return (size_t)ms.ullTotalPhys;
#elif defined(__APPLE__)
    int64_t mem = 0; size_t len = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0 && mem > 0)
        return (size_t)mem;
#elif defined(__linux__)
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long psize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && psize > 0) return (size_t)pages * (size_t)psize;
#endif
    return 0;
}

// Drop the calling (cook worker) thread to a background/utility QoS so the OS
// scheduler keeps it BELOW foreground work and clocks the cores down before
// the fans spin up — the cook should be invisible, not a space heater.
void demoteToBackground() {
#if defined(_WIN32)
    // THREAD_MODE_BACKGROUND_BEGIN also demotes I/O priority, which is the
    // closer match to QOS_CLASS_UTILITY than a plain priority drop — a cook is
    // as disk-hungry as it is CPU-hungry. Falls back to lowest priority on the
    // (documented) failure when the thread is already in background mode.
    if (!::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN))
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif defined(__linux__)
    nice(10);   // best-effort; ignored if unsupported
#endif
}

// Admits work against a byte budget instead of a fixed thread count, so a
// burst of 8K textures / high-poly meshes serializes rather than OOM-ing. A
// task larger than the whole budget is allowed to run ALONE (used==0) so it
// can never deadlock waiting for space that will never exist.
struct MemGovernor {
    std::mutex              m;
    std::condition_variable cv;
    size_t                  budget;
    size_t                  used = 0;
    explicit MemGovernor(size_t b) : budget(b ? b : (size_t)1 << 30) {}

    void acquire(size_t need) {
        need = std::min(need, budget);
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return used == 0 || used + need <= budget; });
        used += need;
    }
    void release(size_t need) {
        need = std::min(need, budget);
        {
            std::lock_guard<std::mutex> lk(m);
            // Saturate rather than wrap. An unbalanced acquire/release pair
            // would otherwise underflow `used` to a huge value and wedge the
            // scheduler FOREVER (every admission test fails) — a silent hang
            // is the worst possible failure mode here, so clamp loudly.
            if (need > used) {
                std::fprintf(stderr, "[TaskGraph] BUG: memory release of %zu "
                             "with only %zu reserved — clamping\n", need, used);
                // The clamp keeps a RELEASE build scheduling; it does not make
                // the imbalance correct. Anything reaching here is an
                // acquire/release bug in the graph, so fail loudly where a
                // developer will see it rather than shipping a papered-over
                // scheduler.
                assert(!"MemGovernor: unbalanced release — acquire/release "
                        "pairing bug in the task graph");
                used = 0;
            } else {
                used -= need;
            }
        }
        cv.notify_all();
    }
};

} // namespace

int TaskGraph::add(std::string name, size_t estBytes, WorkFn work, DoneFn done) {
    Node n;
    n.name     = std::move(name);
    n.estBytes = estBytes;
    n.work     = std::move(work);
    n.done     = std::move(done);
    m_nodes.push_back(std::move(n));
    return (int)m_nodes.size() - 1;
}

void TaskGraph::addEdge(int before, int after) {
    if (before < 0 || after < 0 || before == after ||
        before >= (int)m_nodes.size() || after >= (int)m_nodes.size()) return;
    m_nodes[before].dependents.push_back(after);
    m_nodes[after].unmet++;
    ++m_edges;
}

size_t TaskGraph::run(const Options& opts) {
    const size_t total = m_nodes.size();
    if (total == 0) return 0;

    // ── Governance (same three levers as the old flat pool) ───────────────
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    int workers = opts.workers > 0 ? opts.workers
        : (int)envLong("COOK_THREADS", (long)std::max(1u, hw > 2 ? hw - 2 : 1u));
    workers = std::max(1, std::min(workers, (int)total));

    size_t budget = opts.memBudget;
    if (budget == 0) {
        const size_t ram = physicalRamBytes();
        const size_t autoBudget = ram ? ram * 3 / 5 : ((size_t)4 << 30); // 60% RAM
        budget = (size_t)envLong("COOK_MEM_BUDGET_MB",
                                 (long)(autoBudget >> 20)) << 20;
    }
    MemGovernor gov(budget);

    std::printf("[TaskGraph] %zu task(s), %zu edge(s), %d worker(s), "
                "mem budget %zu MB\n", total, m_edges, workers, budget >> 20);

    // ── Shared state (all under one mutex; work runs outside it) ──────────
    std::mutex              mtx;
    std::condition_variable cvWork;   // workers: ready-heap non-empty / stop
    std::condition_variable cvDrain;  // drain: completions / state change
    // Max-heap on estBytes → longest-processing-time-first dispatch.
    std::priority_queue<std::pair<size_t,int>> ready;
    std::vector<int> finishedQ;
    int    inFlight = 0;
    bool   stop     = false;

    for (int i = 0; i < (int)total; ++i)
        if (m_nodes[i].unmet == 0)
            ready.push({m_nodes[i].estBytes, i});

    auto workerLoop = [&] {
        demoteToBackground();
        for (;;) {
            int idx = -1;
            {
                std::unique_lock<std::mutex> lk(mtx);
                cvWork.wait(lk, [&] { return stop || !ready.empty(); });
                if (stop) return;
                idx = ready.top().second;
                ready.pop();
                ++inFlight;
            }
            Node& n = m_nodes[idx];
            gov.acquire(n.estBytes);
            // Work bodies carry their own error handling (dispatchCook nets
            // exceptions; scene tasks return bool) — this last-resort catch
            // only keeps the worker alive if one slips through.
            try { if (n.work) n.work(); } catch (...) {}
            gov.release(n.estBytes);
            {
                std::lock_guard<std::mutex> lk(mtx);
                --inFlight;
                finishedQ.push_back(idx);
            }
            cvDrain.notify_one();
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (int t = 0; t < workers; ++t) pool.emplace_back(workerLoop);

    // Names the ACTUAL cycle (A -> B -> C -> A), not just the set of stuck
    // tasks — "these 40 tasks are unreachable" tells you nothing about which
    // edge to delete. Once nothing is ready and nothing is in flight, every
    // undrained node has an undrained predecessor, so walking predecessors
    // from any stuck node is guaranteed to close a loop within N steps.
    auto reportCycle = [this] {
        const int n = (int)m_nodes.size();
        std::vector<std::vector<int>> preds(n);
        for (int i = 0; i < n; ++i)
            for (int d : m_nodes[i].dependents) preds[d].push_back(i);

        int start = -1;
        for (int i = 0; i < n; ++i) if (m_nodes[i].unmet > 0) { start = i; break; }
        if (start < 0) return;

        std::vector<int> path, posInPath(n, -1);
        int cur = start;
        while (cur >= 0 && posInPath[cur] < 0) {
            posInPath[cur] = (int)path.size();
            path.push_back(cur);
            int next = -1;
            for (int p : preds[cur])
                if (m_nodes[p].unmet > 0) { next = p; break; }
            cur = next;
        }
        if (cur < 0) {   // no loop found (shouldn't happen) — list the stuck set
            for (int i = 0; i < n; ++i)
                if (m_nodes[i].unmet > 0)
                    std::fprintf(stderr, "[TaskGraph]   stuck: %s\n",
                                 m_nodes[i].name.c_str());
            return;
        }
        // path[posInPath[cur]..] is the loop, in predecessor order — reverse
        // it to print in the direction the edges actually point.
        std::vector<int> cyc(path.begin() + posInPath[cur], path.end());
        std::reverse(cyc.begin(), cyc.end());
        std::string s;
        for (int i : cyc) { s += m_nodes[i].name; s += " -> "; }
        s += m_nodes[cyc.front()].name;          // close the loop
        std::fprintf(stderr, "[TaskGraph]   cycle: %s\n", s.c_str());
    };

    // ── Drain (caller thread): done() callbacks + dependency release ──────
    size_t drained   = 0;
    bool   cancelled = false;
    for (;;) {
        int idx = -1;
        {
            std::unique_lock<std::mutex> lk(mtx);
            // Timed wait so cancellation is noticed even while every worker
            // is deep inside a long cook.
            cvDrain.wait_for(lk, std::chrono::milliseconds(100),
                             [&] { return !finishedQ.empty(); });

            if (!cancelled && opts.shouldContinue && !opts.shouldContinue()) {
                // Stop dispatching; in-flight tasks still land and drain.
                cancelled = true;
                stop      = true;
                ready     = {};
                cvWork.notify_all();
            }
            if (!finishedQ.empty()) {
                idx = finishedQ.back();
                finishedQ.pop_back();
            } else if (cancelled) {
                if (inFlight == 0) break;          // everything landed
                continue;
            } else if (ready.empty() && inFlight == 0) {
                if (drained == total) break;       // all done
                // Nothing running, nothing ready, tasks remain → cycle.
                std::fprintf(stderr, "[TaskGraph] dependency cycle — %zu "
                             "task(s) unreachable\n", total - drained);
                reportCycle();
                break;
            } else {
                continue;                          // spurious/timeout wakeup
            }
        }

        Node& n = m_nodes[idx];
        try { if (n.done) n.done(); } catch (...) {
            std::fprintf(stderr, "[TaskGraph] done() threw for %s\n",
                         n.name.c_str());
        }
        ++drained;

        {
            std::lock_guard<std::mutex> lk(mtx);
            for (int dep : n.dependents)
                if (--m_nodes[dep].unmet == 0 && !stop)
                    ready.push({m_nodes[dep].estBytes, dep});
        }
        cvWork.notify_all();
    }

    {
        std::lock_guard<std::mutex> lk(mtx);
        stop = true;
    }
    cvWork.notify_all();
    for (auto& th : pool) th.join();
    return drained;
}

} // namespace assetlib
