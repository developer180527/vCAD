# 0006 — Acceptance tests are written in Rust, against the C ABI

Status: accepted (Aug 2026)

## Decision
New tests go in Rust, driving the core through `cad_plugin_abi.h`. C++ (Catch2) is retained
only for internals with no ABI representation.

## Why
The C ABI is the surface plugins, the SwiftUI iPad shell, and every future language binding
will use. A suite that only calls C++ directly will not notice the day that surface breaks,
and the people who find out will be third parties.

Driving acceptance from another language means an ABI regression fails our own tests first;
that the tests cannot cheat, because Rust genuinely cannot reach into a C++ type, so anything
they need must be properly exposed; and that `tests-rs/cad/` doubles as the reference binding.
If a test is awkward to write there, the ABI is wrong.

## Consequences
- The session half of the ABI had to be built at M2 rather than M6. That is a benefit: it is
  validated by real use two milestones before anyone depends on it.
- Two build systems. Mitigated by an explicit contract — CMake first, cargo second, never
  cargo-invokes-cmake — and one entry point, `tools/run-tests.sh`.
- FFI declarations are hand-written rather than bindgen-generated, so an accidental header
  change is a compile error instead of a silent regeneration. Header and `cad-sys` change in
  the same commit; `CAD_ABI_VERSION_MINOR` goes up.

## Rejected
- **Catch2 everywhere.** Cheaper, but leaves the ABI untested until a plugin author finds the
  bug.
- **bindgen.** Convenient, but it hides exactly the changes we want to be loud, and it puts
  libclang in the CI image.
- **cargo invokes cmake.** Makes the Rust suite hostage to a 40-minute OCCT build and turns
  C++ build errors into incomprehensible cargo failures.
