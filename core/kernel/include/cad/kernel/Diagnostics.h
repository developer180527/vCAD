#pragma once

/// Routing OCCT's own diagnostics into our log.
///
/// OCCT writes through its `Message::DefaultMessenger()`, which by default prints to `std::cout`.
/// That output is genuinely useful — healing reports what it repaired, STEP reports what it could
/// not read — and today it goes to a terminal nobody is watching, unfiltered and interleaved with
/// nothing. In a GUI build it is simply lost.
namespace cad::kernel {

/// Replaces OCCT's printers with one that forwards to `cad::log` under the ThirdParty category.
///
/// Idempotent, and safe to call before anything else: it only touches OCCT's messenger. Call it
/// once at startup, from whichever shell is running — it lives here rather than in a shell because
/// it needs OCCT headers, and no shell may include those.
void routeOcctDiagnosticsToLog();

}  // namespace cad::kernel
