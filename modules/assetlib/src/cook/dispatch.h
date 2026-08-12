#pragma once
#include "assetlib/cooker.h"
#include <filesystem>
#include <functional>

// Internal: HOW a cook is executed — isolated child process or in-process.
// Orthogonal to WHAT gets cooked and WHERE the output is cached, which is
// the pipeline's job. Not a public header.
namespace assetlib {

// Polled to abort a cook in progress. Called from cook worker threads, so it
// MUST be thread-safe (the pipeline passes an atomic read). Empty = no
// cancellation. A cancelled cook returns CookResult{cancelled=true}, which
// the pipeline deliberately does NOT record — see CookResult::cancelled.
using CancelFn = std::function<bool()>;

// Cook in an isolated `engine_cook_worker` child process. A corrupt asset
// that SIGSEGVs Assimp kills the child, not us; the child also self-imposes
// a hard setrlimit memory cap. Crash, timeout, and a missing/garbled result
// all come back as an ordinary per-asset failure. On cancellation the child
// is SIGKILLed rather than waited out — otherwise quitting the editor blocks
// for however long an 8K BC7 bake still needs.
// POSIX only — returns a failure result on Windows.
CookResult cookInWorkerProcess(const std::filesystem::path& workerExe,
                               ICooker& cooker, const CookContext& ctx,
                               const CancelFn& isCancelled = {});

// Cook on this thread behind an exception net. Exceptions from third-party
// parsers become per-asset failures instead of std::terminate; SIGNALS still
// kill the host — which is precisely what the worker process is for.
// (In-process cooks cannot be interrupted once started; cancellation is only
// honoured before the cooker is entered.)
CookResult cookInProcess(ICooker& cooker, const CookContext& ctx);

// THE dispatch seam: worker process when `workerExe` is set (and supported),
// in-process otherwise. Every cook in the pipeline goes through here.
CookResult dispatchCook(const std::filesystem::path& workerExe,
                        ICooker& cooker, const CookContext& ctx,
                        const CancelFn& isCancelled = {});

} // namespace assetlib
