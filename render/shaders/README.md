# Shaders

bgfx shader dialect, compiled by `shaderc` at build time (see `cmake/CadShaders.cmake`).

## `varying.def.sc` must contain no comments

shaderc's parser for this file **silently drops the first declaration if the file begins with a
comment.** Nothing warns. The symptom is a compile error deep in the *generated* HLSL saying
`'a_position' : unknown variable`, pointing at a line you did not write, while every other
attribute survives — so it reads as a bgfx bug rather than a stray `//`.

Cost an isolation session to find. Keep this file bare declarations only; the explanations live
here and in `common.sh`.

## Attribute conventions

| Attribute | Semantic | Why |
|---|---|---|
| `a_position` | POSITION | |
| `a_normal` | NORMAL | |
| `a_color1` | COLOR1 | The element index. bgfx has no integer vertex attributes, so it travels as four *unnormalised* uint8 channels and is reassembled in `common.sh::unpackU32`. |
| `i_data0..3` | TEXCOORD7..4 | Instance data. The semantics and their descending order are fixed by bgfx, not chosen. `i_data0..2` are the 4x3 affine rows; `i_data3` carries colour, `elementBase` and `instanceId`. |

## Passes

- `vs_shaded` + `fs_shaded` — the surface pass. Two-sided lighting plus a dim fill from below:
  CAD surfaces are routinely seen from behind (inside a pocket, through a section cut) and a
  one-sided model renders those black, which users read as a hole in the part.
- `vs_edge` + `fs_edge` — line primitives with a clip-space depth bias. Without the bias they
  z-fight the surfaces they lie exactly on, giving the stippled outlines that make a viewport
  look cheap. Depth-test but no depth-write, so biased lines cannot corrupt depth for later draws.
- `vs_shaded` + `fs_pick` — the same vertex shader, writing element ids as RGBA8 for readback.
  Reusing the vertex shader is deliberate: a pick pass that transforms geometry differently from
  the view pass is a pick that lies.
