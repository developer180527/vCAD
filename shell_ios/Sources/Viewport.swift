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

    func makeUIView(context: Context) -> CadViewportView {
        let view = CadViewportView(frame: .zero)
        view.onStatus = { onStatus($0) }
        view.onDocumentChanged = { onDocumentChanged() }
        // `start()` is NOT called here. A view has no size until it is laid out, and a renderer
        // brought up against a zero-sized drawable initialises into nothing. The view starts itself
        // from `layoutSubviews`, which is also what makes rotation and Slide Over work.
        DispatchQueue.main.async { handle = view }
        return view
    }

    func updateUIView(_ view: CadViewportView, context: Context) {}
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
