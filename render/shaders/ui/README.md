# UI shaders

A port of libgui's reference shader, `crates/libgui_shaders/shaders/ui.wgsl`, for the vertex+index
form libgui emits with `libgui_enable_mesh`. Drawn by `render/src/BgfxUiPass.cpp`.

- The vertex stage only maps logical pixels to clip space. Every other value the fragment stage
  reads arrives already interpolated in the vertex, so this file has almost nothing to get wrong.
- The fragment stage is `fs_main` transliterated. Keep the branches in the same order as the
  reference so a diff between the two stays readable.
- Output is premultiplied alpha; blend with ONE, INV_SRC_ALPHA.
- `varying.def.sc` has its own directory because `cad_compile_shaders` uses one per directory, and
  it must contain no comments (see `../README.md`).

libgui ships a conformance kit (`libgui_conformance_*`, `libgui_reference_pixels`) that renders
every primitive on the CPU. Comparing this port against it is the test that matters, and is not
wired yet.
