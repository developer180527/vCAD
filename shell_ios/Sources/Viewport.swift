import SwiftUI

/// The viewport, as SwiftUI sees it.
///
/// A thin wrapper on purpose. `CadViewportView` owns the document, the camera and the GPU surface,
/// and this type exists only to place it in a layout and hand a few commands back — everything else
/// would be a second copy of state that already exists in `app/`.
struct ViewportView: UIViewRepresentable {
    /// Handed back on creation so the surrounding screen can drive the view: run a command, fit,
    /// undo. A binding rather than a return value because SwiftUI may rebuild this struct at will
    /// and the UIView must survive that.
    @Binding var handle: CadViewportView?
    let onStatus: (String) -> Void
    let onDocumentChanged: () -> Void
    /// Whether the renderer came up, and why not if it did not.
    ///
    /// Pushed rather than polled. `CadViewportView.attached` read inside a SwiftUI body is
    /// evaluated exactly once — before the view has been laid out, so necessarily before the
    /// renderer can have started — and never again unless some other state happens to change.
    let onStarted: (Bool, String) -> Void
    /// Offered a tap before selection sees it, in view points. Returns true when it was consumed —
    /// which is how "the next tap chooses the sketch plane" works without a mode inside the bridge.
    let onPlaneTap: (CGFloat, CGFloat) -> Bool

    func makeUIView(context: Context) -> CadViewportView {
        let view = CadViewportView(frame: .zero)
        view.onStatus = { onStatus($0) }
        view.onDocumentChanged = { onDocumentChanged() }
        view.onStarted = { ok, error in onStarted(ok, error) }
        view.onTap = { x, y in onPlaneTap(x, y) }
        // `start()` is NOT called here. A view has no size until it is laid out, and a renderer
        // brought up against a zero-sized drawable initialises into nothing. The view starts itself
        // from `layoutSubviews`, which is also what makes rotation and Slide Over work.
        DispatchQueue.main.async { handle = view }
        return view
    }

    func updateUIView(_ view: CadViewportView, context: Context) {}

    /// Tears the renderer down when the screen goes away.
    ///
    /// Not optional politeness: bgfx is a process-wide singleton, so a viewport still holding a
    /// context when the next project opens makes THAT project's renderer fail to start. Leaving it
    /// to deallocation does not work either — the callbacks above capture state that holds the
    /// view, so nothing would ever release it.
    static func dismantleUIView(_ view: CadViewportView, coordinator: ()) {
        view.shutdown()
    }
}

/// What the shell shows when the renderer could not start.
///
/// Its own view, because "blank" is the one thing this must never be: a viewport that fails to
/// initialise and a viewport that succeeded and drew nothing look identical, and telling them apart
/// afterwards has cost days on the desktop side.
struct ViewportFailure: View {
    let reason: String

    var body: some View {
        VStack(spacing: 8) {
            Image(systemName: "exclamationmark.triangle")
                .font(.system(size: 40, weight: .light))
                .foregroundStyle(Palette.secondaryText)
            Text("The renderer could not start")
                .font(.headline)
                .foregroundStyle(Palette.text)
            Text(reason)
                .font(.callout)
                .foregroundStyle(Palette.secondaryText)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 40)
        }
    }
}
