#pragma once
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace assetlib {

// ── TaskGraph — cost-weighted DAG scheduler for cook work ────────────────────
// Replaces the flat parallel-for: cook work is a DAG of typed, cost-weighted
// tasks (a 4-vertex OBJ and an 8K BC7 are not the same unit of work, and a
// scene depends on the assets it references). What the graph buys:
//
//   • Cost-aware dispatch — the ready queue is a max-heap on estBytes, so the
//     longest tasks start FIRST (LPT scheduling): an 8K texture begins at
//     t=0 instead of becoming the lone straggler after everything else.
//   • Dependencies — a task runs only after every edge into it completed;
//     scenes cook the moment THEIR assets land, not after all assets.
//   • Two lanes — work() runs on the QoS-demoted worker pool; done() runs
//     serialized on the caller (drain) thread, which is what lets registry
//     commits keep their single-connection discipline while cooks overlap.
//   • Governance — the same memory-budget admission as before (estBytes is
//     both the reservation and the scheduling weight).
//   • Cancellation + liveness — shouldContinue stops dispatch (in-flight
//     tasks still drain); a dependency cycle is detected and reported
//     instead of deadlocking.
class TaskGraph {
public:
    using WorkFn = std::function<void()>;   // worker lane (parallel)
    using DoneFn = std::function<void()>;   // drain lane (caller thread)

    struct Options {
        int    workers   = 0;   // 0 → cores-2 (COOK_THREADS overrides)
        size_t memBudget = 0;   // 0 → 60% RAM (COOK_MEM_BUDGET_MB overrides)
        std::function<bool()> shouldContinue;   // false → stop dispatching
    };

    // estBytes: memory-admission reservation AND scheduling weight.
    // Returns the task id used by addEdge.
    int  add(std::string name, size_t estBytes, WorkFn work, DoneFn done = {});
    // `after` waits for `before`. Ids must come from add().
    void addEdge(int before, int after);

    size_t taskCount() const { return m_nodes.size(); }
    size_t edgeCount() const { return m_edges; }

    // Blocks until every reachable task drained (or cancellation). The
    // calling thread becomes the drain. Returns how many tasks RAN; less
    // than taskCount() means cancellation or a reported cycle.
    size_t run(const Options& opts);

private:
    struct Node {
        std::string name;
        size_t      estBytes = 0;
        WorkFn      work;
        DoneFn      done;
        std::vector<int> dependents;
        int         unmet = 0;
    };
    std::vector<Node> m_nodes;
    size_t            m_edges = 0;
};

} // namespace assetlib
