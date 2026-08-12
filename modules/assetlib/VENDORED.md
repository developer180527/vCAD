# assetlib — vendored copy

Content-addressed derived-data cache. Provides the L1/L2 tiers behind
`cad::recompute::DdcCache` (see [ADR 0004](../../docs/decisions/0004-ddc-recompute.md)).

## Provenance

| | |
|---|---|
| Upstream | https://github.com/developer180527/engine — `modules/assetlib` |
| Synced from | `7a696d8` ("assetlib: include \<atomic\> in fs_util.cpp") |
| Synced on | 2026-08-11 |

**Update this table on every sync.** A vendored copy with no recorded provenance cannot be
diffed against upstream, which makes the next sync a manual read of both trees.

## Why vendored rather than a submodule

assetlib lives inside a much larger repository. A submodule of that repository meant cloning
26 MB of game engine to build 10 MB of one directory, and every build line reading
`modules/engine/...` — which looks, reasonably, like CI is compiling a game engine. (It was
not: only assetlib's 18 translation units were ever built.)

## The cost, stated plainly

Vendoring means changes no longer flow automatically in either direction, and this copy
proved it on arrival: the pasted tree predated the `<atomic>` fix that had just landed
upstream, and would have re-broken the Linux build. The fix was re-applied by hand.

So:

- **A fix made here must be pushed upstream too**, or the next sync silently reverts it.
- **A fix made upstream must be pulled here**, or Linux CI finds it the hard way.

To sync:

```bash
git clone --depth 1 https://github.com/developer180527/engine /tmp/engine
diff -ru modules/assetlib /tmp/engine/modules/assetlib
```

## The better long-term answer

Split assetlib into its own repository. It is already standalone by construction — zero
engine dependencies, BLAKE3 and SQLite vendored inside it — and it is genuinely reusable
beyond both projects. That would make it a normal dependency for both consumers instead of a
copy in one and a subdirectory in the other, and would give it CI of its own.

That matters concretely: the `<atomic>` bug existed because **assetlib has no gcc coverage**.
It builds on macOS (libc++) and Windows (MSVC STL), both of which provide `<atomic>`
transitively; libstdc++ does not. A downstream project's Linux job found it.

## Known latent issues, not fixed here

Two public headers are not self-contained. They compile today only because another include
happens to pull the needed header in first, which is one reordering away from breaking:

| Header | Uses | Missing |
|---|---|---|
| `include/assetlib/asset_registry.h` | `std::unordered_map` | `<unordered_map>` |
| `include/assetlib/cook_pipeline.h` | `std::unique_ptr` | `<memory>` |

Fix upstream first, then sync, so the two copies do not diverge further.
