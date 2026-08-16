// A real vCAD plugin, built as a real shared library, loaded through the real loader.
//
// The point of this file is that it is NOT a fake. Every plugin test until now registered a
// feature by calling the host vtable directly from inside the test process, which exercises the
// contract but not the loader: no manifest was read, nothing was dlopen'd, no descriptor was
// checked against anything, and `cad_plugin_main` was never the entry point.
//
// It is deliberately minimal. It registers one feature type that makes a cube from a `size`
// parameter -- enough to prove that a separately compiled library can reach the host, register,
// and have its compute called during a recompute, which is the whole claim of the plugin system.

#include "cad/abi/cad_plugin_abi.h"

#include <cstring>

namespace {

const CadHost* g_host = nullptr;

/// Compute: a cube of `size`, named by the host.
CadStatus computeCube(void* /*plugin_ctx*/, CadComputeCtx cc) {
    if (g_host == nullptr) return CAD_ERR_INTERNAL;

    double size = 0.0;
    const CadStatus read = g_host->compute_param_real(g_host->host_ctx, cc, "size", &size);
    if (read != CAD_OK) return read;

    if (!(size > 0.0)) {
        // Through compute_fail so the model tree shows a sentence rather than a code -- the
        // contract's §7.2 argument, exercised from a real plugin for the first time.
        g_host->compute_fail(g_host->host_ctx, cc, "The cube size must be greater than zero.",
                             "demo plugin: size <= 0");
        return CAD_ERR_INVALID_INPUT;
    }

    CadShape shape = 0;
    const CadStatus built = g_host->make_box(g_host->host_ctx, size, size, size, &shape);
    if (built != CAD_OK) return built;

    return g_host->compute_set_output(g_host->host_ctx, cc, shape);
}

CadStatus initialize(const CadHost* host) {
    if (host == nullptr) return CAD_ERR_INVALID_INPUT;

    // 4.5: check struct_size before touching anything past the header. A host older than this
    // plugin's header would leave later entries absent, and this is where a plugin finds that out
    // rather than by calling a null pointer.
    if (host->struct_size < sizeof(CadHost)) {
        // Not fatal in general -- a plugin may work with fewer entries -- but this one needs
        // make_box and the compute accessors, so it declines rather than crash later.
        return CAD_ERR_UNSUPPORTED;
    }
    g_host = host;

    CadFeatureDesc feature;
    std::memset(&feature, 0, sizeof(feature));
    feature.struct_size = sizeof(feature);
    feature.struct_version = 1;
    feature.type_name = "com.vcad.demo.Cube";
    feature.compute_version = 1;
    feature.param_schema_version = 1;
    feature.plugin_ctx = nullptr;
    feature.compute = &computeCube;

    return host->register_feature(host->host_ctx, &feature, nullptr, 0);
}

void shutdown(void) { g_host = nullptr; }

const CadPluginDesc g_desc = {
    /* struct_size    */ sizeof(CadPluginDesc),
    /* struct_version */ 1,
    /* abi_major      */ CAD_ABI_VERSION_MAJOR,
    /* abi_minor      */ CAD_ABI_VERSION_MINOR,
    /* id             */ "com.vcad.demo",
    /* display_name   */ "vCAD Demo Plugin",
    /* semver         */ "1.0.0",
    /* required_caps  */ 0,
    /* min_host_minor */ 0,
    /* initialize     */ &initialize,
    /* shutdown       */ &shutdown,
};

}  // namespace

extern "C" CAD_API const CadPluginDesc* cad_plugin_main(void) { return &g_desc; }
