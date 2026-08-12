#pragma once
// Internal to the cook-dispatch TUs. Not a public header.
//
// dispatchCook() picks a mode; the two worker_*.cpp files implement the isolated
// child on their own platform; result_file.cpp owns the sidecar protocol both of
// them converge on. These declarations are what let those three files be three
// files instead of one 460-line block where half the code is invisible to your
// compiler.
#include "assetlib/cooker.h"

#include <filesystem>
#include <string>

namespace assetlib {

// Parse and VALIDATE a finished worker's sidecar result. `exitDesc` describes how
// the child ended, for the diagnostic when it wrote nothing usable. Deletes the
// file. Platform-independent: both spawn paths end here.
CookResult finishFromResultFile(const std::filesystem::path& resultPath,
                                const CookContext& ctx,
                                const std::string& exitDesc);

// Per-task memory cap in MB: 2x the cooker's estimate with a 1 GB floor, or
// COOK_TASK_MEM_CAP_MB verbatim.
long taskMemCapMb(ICooker& cooker, const CookContext& ctx);

#if !defined(_WIN32)
// Distinct from the worker's own codes (64 = bad usage, 65 = could not write a
// result) so "the binary is missing or not executable" is diagnosable rather
// than looking like a cook that crashed.
constexpr int kExecFailedExit = 66;
#endif

} // namespace assetlib
