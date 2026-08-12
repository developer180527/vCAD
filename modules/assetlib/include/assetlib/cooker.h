#pragma once
#include "uuid.h"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// The COOKER CONTRACT — everything needed to implement or invoke a cooker,
// with no dependency on the pipeline that drives them. Cooker implementations
// and the out-of-process worker include this; only the orchestrator needs
// <assetlib/cook_pipeline.h>.
namespace assetlib {

// ── Cook context ──────────────────────────────────────────────────────────────
// Passed to ICooker::cook(). Cooker reads from sourcePath, writes to outputPath,
// and records any assets it depends on via addDependency().
// THREADING CONTRACT: one CookContext belongs to ONE cook on ONE thread.
// The pipeline runs different assets concurrently, never the same context.
// addDependency/addOutput are therefore NOT synchronized — they append to
// plain vectors owned by the scheduling task. A cooker that parallelizes its
// OWN work (parallel mip encode, threaded mesh optimization) must funnel
// these calls back to its cook() thread, or serialize them itself.
struct CookContext {
    UUID                     uuid;
    std::filesystem::path    sourcePath;
    std::filesystem::path    outputPath;
    std::function<void(const UUID&)> addDependency;
    // A cooker producing files BEYOND outputPath (the mesh cooker writes
    // sibling .ctex blobs for embedded textures) MUST report each one here,
    // or those files won't travel with the DDC record and a cache hit on
    // another machine materializes a mesh whose textures don't exist.
    std::function<void(const std::filesystem::path&)> addOutput;
};

struct CookResult {
    bool        success    = false;
    bool        skipped    = false; // cooker can't handle this type — not an error
    // Aborted mid-cook because the host is shutting down — NOT a verdict on
    // the asset. The pipeline must leave the registry record untouched: a
    // cancelled cook recorded as Failed would carry the current DDC key, and
    // staleness reads "same key + Failed" as "already tried, don't retry" —
    // the asset would never cook again until its source changed.
    bool        cancelled  = false;
    std::string error;
    // Populated by the pipeline after a successful cook:
    std::string cookedPath;
};

// ── Cooker interface ──────────────────────────────────────────────────────────
// Implement this for each source format. The pipeline calls cook() when it
// detects a source file is stale (hash changed or never cooked).
class ICooker {
public:
    virtual ~ICooker() = default;
    // Which source file extensions does this cooker handle? e.g. {".fbx", ".obj"}
    virtual std::vector<std::string> extensions() const = 0;
    virtual CookResult               cook(const CookContext& ctx) = 0;

    // ── DDC identity — these three, plus the source bytes, ARE the cache key.
    // Stable id, never reused across cooker kinds (it namespaces the key).
    virtual const char* id() const = 0;
    // Bump whenever cook() output changes for identical input: format bumps,
    // encoder swaps, bug fixes. Only THIS cooker's outputs re-cook — a
    // texture-cooker bump never invalidates a single cooked mesh.
    virtual uint32_t    version() const = 0;
    // Everything else that alters output for the same source bytes: env
    // quality knobs, per-asset flags derived from the path (a filename-based
    // normal-map heuristic changes the encode!). MUST be deterministic for a
    // given (environment, source path). Default: no extra settings.
    virtual std::string settingsFingerprint(const CookContext& ctx) const {
        (void)ctx; return {};
    }

    // Extra FILES this cook reads besides ctx.sourcePath, whose CONTENT changes
    // the output. The pipeline hashes each one into the DDC key, so editing any
    // of them re-cooks this asset and nothing else.
    //
    // This exists because `CookContext::addDependency` takes a UUID and cookers
    // have no registry lookup — so a cooker whose extra input is a plain FILE
    // (a shader's .sc stage sources, the .shader manifest a material resolves
    // against) had no way to declare it, and every such cooker hand-rolled
    // blake3File() into settingsFingerprint instead. That worked, and it was an
    // unenforceable convention: nothing could tell a cooker that forgot from one
    // that had no extra inputs. Declaring paths here makes the pipeline do the
    // hashing, uniformly, and makes the omission testable — see
    // cook_deps_test.cpp, which perturbs every declared input of every
    // registered cooker and requires the key to move.
    //
    // Called BEFORE the cook (it participates in the key), so it must derive its
    // answer from the source file alone. Paths that do not exist are ignored —
    // a missing include is the cook's error to report, not a keying failure.
    // MUST be deterministic for a given (environment, source path).
    virtual std::vector<std::filesystem::path>
    declaredInputs(const CookContext& ctx) const { (void)ctx; return {}; }

    // Estimated peak heap footprint, in bytes, of cooking this one asset.
    // The scheduler admits work against a memory budget (not a fixed thread
    // count) so a burst of 8K textures or high-poly meshes serializes instead
    // of OOM-ing the machine — heavy tasks run few-at-a-time, cheap ones pack.
    // Default: a generous multiple of source size; a cooker that can cheaply
    // predict better (a texture peeking its header dimensions) should override.
    virtual size_t estimatePeakBytes(const CookContext& ctx) const {
        std::error_code ec;
        const auto sz = std::filesystem::file_size(ctx.sourcePath, ec);
        return ec ? ((size_t)64 << 20) : (size_t)sz * 10 + ((size_t)16 << 20);
    }

    // The sibling files that belong to an ALREADY-materialized `primary` —
    // the same set a fresh cook would report through addOutput(). Used only to
    // BACK-FILL the DDC from outputs that exist on disk but were never
    // ingested (see CookPipeline::backfillDdc): with no cook running there is
    // no CookContext to collect addOutput calls, so the cooker must be able to
    // re-derive its own output set.
    //
    // MUST be exact. Guessing by glob (`<uuid>_t*.ctex`) would sweep up stale
    // siblings left by an older cooker version — nothing deletes them — and a
    // manifest would then depend on this machine's .cache history rather than
    // on the inputs, breaking the determinism invariant that lets two machines
    // share a cache. Read the real output instead, as MeshCooker does.
    //
    // Default: none — correct for every single-output cooker.
    virtual void enumerateOutputs(const std::filesystem::path& primary,
                                  std::vector<std::filesystem::path>& out) const {
        (void)primary; (void)out;
    }
};

} // namespace assetlib
