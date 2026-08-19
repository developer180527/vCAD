import SwiftUI

/// The floating tool rails, which are the whole of Shapr3D's chrome.
///
/// # What is being copied, and why it is not a ribbon
///
/// Shapr3D puts every command in slim vertical rails pinned to the left and right edges, floating
/// OVER the model rather than reserving space beside it, grouped into small rounded capsules with
/// gaps between groups. Nothing spans the top or bottom edge except the document chip.
///
/// The reasons this is right for a tablet, and not merely different from the desktop ribbon:
///
/// - **The model gets the whole screen.** A ribbon reserves a horizontal band across the widest
///   dimension of a device held in landscape — the most expensive strip of pixels there is.
/// - **The edges are where the hands already are.** A tablet is held at its sides; a command at the
///   left edge is under the thumb, and a command in a top ribbon is a reach across the drawing.
/// - **The rails stay clear of the stylus.** Tools on the left, view controls on the right, and the
///   middle — where a right hand rests while drawing — left empty.
///
/// # Labels are a COLUMN, not an overlay
///
/// This was got wrong once and the mistake is worth recording, because it looked plausible in code
/// and was obviously broken on the device: labels were drawn as an overlay on each button and
/// offset sideways, which put them on top of the icons in the next rail and off the screen edge on
/// the right-hand side.
///
/// Shapr3D draws them as a second column — one chip per row, each aligned with its own icon, beside
/// the capsule rather than inside it. So that is what this builds: the capsule background sits
/// behind the ICON column only, at a fixed width, and the label chips live in the same rows in the
/// layout. Being in the layout is the point — the chips reserve their own space instead of landing
/// wherever an offset happens to put them.
struct RailItem: Identifiable {
    let id = UUID()
    let title: String
    let symbol: String
    /// Nil means declared but not yet wired. Such an item draws greyed and does not respond —
    /// a button that silently does nothing is a bug report waiting to be filed.
    let action: (() -> Void)?
    var selected: Bool = false

    init(_ title: String, _ symbol: String, selected: Bool = false, action: (() -> Void)? = nil) {
        self.title = title
        self.symbol = symbol
        self.selected = selected
        self.action = action
    }
}

/// Which edge a rail is pinned to. It decides which side the label column opens towards, and
/// nothing else — a rail is otherwise the same object on either side.
enum RailEdge {
    case leading, trailing
}

/// One capsule of related commands, with its label column.
struct RailGroup: View {
    let items: [RailItem]
    let showLabels: Bool
    let edge: RailEdge

    /// The capsule's width and each row's height are fixed and shared with the label column, which
    /// is what keeps a chip beside its own icon. Hard-coded together on purpose: derived from a
    /// font metric they would drift apart the first time a symbol changed weight.
    private static let iconWidth: CGFloat = 46
    private static let rowHeight: CGFloat = 42
    private static let rowSpacing: CGFloat = 2

    var body: some View {
        HStack(spacing: 8) {
            if edge == .trailing && showLabels { labelColumn }
            iconColumn
            if edge == .leading && showLabels { labelColumn }
        }
    }

    private var iconColumn: some View {
        VStack(spacing: Self.rowSpacing) {
            ForEach(items) { item in
                Button {
                    item.action?()
                } label: {
                    Image(systemName: item.symbol)
                        .font(.system(size: 17, weight: .regular))
                        .frame(width: Self.iconWidth, height: Self.rowHeight)
                        .foregroundStyle(colour(item))
                        .background(
                            item.selected ? Palette.selection : Color.clear,
                            in: RoundedRectangle(cornerRadius: 8))
                }
                .disabled(item.action == nil)
                .accessibilityLabel(item.title)
            }
        }
        .padding(.vertical, 5)
        .frame(width: Self.iconWidth + 8)
        .background(Palette.chrome, in: RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(Palette.hairline, lineWidth: 1))
    }

    private var labelColumn: some View {
        // Alignment lives on the STACK, never `maxWidth: .infinity` on the chips: inside an HStack
        // an infinite-width child eats all the remaining space and shoves the icon capsule off its
        // edge. The column is exactly as wide as its widest chip.
        VStack(alignment: edge == .leading ? .leading : .trailing, spacing: Self.rowSpacing) {
            ForEach(items) { item in
                Text(item.title)
                    .font(.footnote.weight(.medium))
                    .foregroundStyle(colour(item))
                    .lineLimit(1)
                    .fixedSize()
                    .padding(.horizontal, 10)
                    .frame(height: Self.rowHeight - 8, alignment: .center)
                    .background(Palette.chrome, in: RoundedRectangle(cornerRadius: 8))
                    .overlay(RoundedRectangle(cornerRadius: 8).stroke(Palette.hairline, lineWidth: 1))
                    .frame(height: Self.rowHeight)
            }
        }
        // The padding matches the icon column's, so row N of one lines up with row N of the other.
        .padding(.vertical, 5)
        // Not hit-testable: a tap on a label should reach the model behind it rather than the
        // button, because a label is a caption, not a second copy of the control.
        .allowsHitTesting(false)
    }

    private func colour(_ item: RailItem) -> Color {
        if item.action == nil { return Palette.disabledText }
        return item.selected ? Palette.accent : Palette.text
    }
}
