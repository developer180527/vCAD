# CI triplet overlays

Release-only builds of third-party dependencies.

vcpkg builds every port twice by default — debug and release. For OCCT that is 30–60 minutes
of compilation each, and CI never links the debug copy: the test matrix builds
`RelWithDebInfo`, and the Linux sanitizer job builds our own code with `-fsanitize` while
linking release dependencies (which is exactly what you want — sanitizer coverage inside
OCCT is not the signal we are after).

Halving the cold-build time is the difference between a workflow people wait for and one
they ignore.

**Used by CI only.** Local development keeps the default triplets so you can configure a
Debug build without a CRT mismatch on Windows — mixing `/MDd` application code with `/MD`
dependencies is an ABI error, not a warning. Pass them explicitly if you want the same
behaviour locally:

    -DVCPKG_OVERLAY_TRIPLETS=cmake/triplets
