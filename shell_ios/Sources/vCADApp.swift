import SwiftUI

/// vCAD on iPad.
///
/// # What this shell is and is not
///
/// It is a *view layer*. Every rule about what a click means, what a sketch is, which face a
/// selection resolves to, and what a feature computes lives in `app/` and `core/` — the same C++
/// that the desktop shell drives, now proven to run on this device. That split is the whole reason
/// a second shell is affordable at all: `docs/design/IPAD_UX.md` measured five of eight concerns as
/// shared, and the two shells disagreeing about a model rule is the failure it exists to prevent.
///
/// So the rule for everything in this directory: **if a behaviour would have to be reimplemented in
/// the Android shell to work the same way, it does not belong here.** It belongs in `app/`.
///
/// # Stylus-agnostic, deliberately
///
/// Shapr3D's interaction model is the reference, but Apple Pencil is never *required*. A finger and
/// a third-party stylus must be able to do everything; Pencil adds precision and hover, which is a
/// genuine advantage to design around rather than a floor to stand on. Requiring Pencil would also
/// make the eventual Android shell a different product rather than the same one.
@main
struct vCADApp: App {
    /// One store for the session, injected rather than reached for globally: a preview and a test
    /// need to hand it a different one, and a singleton makes that impossible.
    @StateObject private var projects = ProjectStore()
    @StateObject private var settings = AppSettings()

    var body: some Scene {
        WindowGroup {
            HomeView()
                .environmentObject(projects)
                .environmentObject(settings)
                // Paper White is a LIGHT scheme, and saying so is not decoration.
                //
                // Every background in this shell is a hard-coded Paper White value, but text,
                // separators and control tints are the SYSTEM's, which resolve against the device's
                // appearance. On an iPad in dark mode that produced white labels on a white
                // sidebar — the app looked empty rather than broken, which is worse.
                //
                // When a dark theme lands it is an ADDITION: this becomes the theme's own scheme,
                // never a per-view patch.
                .preferredColorScheme(.light)
        }
    }
}
