# 0002 — Renderer is bgfx, not Diligent Engine

Status: accepted (Aug 2026) — **reverses the initial plan**

## Decision
bgfx (BSD-2-Clause).

## Context
DiligentCore is Apache-2.0 for D3D11/D3D12/Vulkan/OpenGL, but its **Metal backend is
available only under a commercial licence** (contact Diligent Graphics). Metal is mandatory
on macOS desktop *and* on the future iPad build — our two most important platforms. Adopting
Diligent would put a paid dependency on both before there is any revenue.

bgfx ships Metal with no strings and its macOS/iOS Metal support is mature.

## Consequences
- We give up Diligent's more modern explicit-API abstraction. Acceptable: a CAD viewport is
  not bound by draw-call submission overhead the way a game renderer is.
- Verify empirically in spike 0.3 before M3 commits.
- Revisit only against a concrete wall bgfx cannot clear, and price the Metal licence then.
