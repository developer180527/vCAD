import SwiftUI

/// Paper White, on iPad.
///
/// # These numbers are not choices
///
/// They are the desktop shell's, copied literally from `modules/proshell/src/Theme.cpp`, which the
/// proshell probe asserts against by string (`#f0efed`, `#cfcdc9`, `#0a6cc4`). Paper White is
/// frozen; a second shell inventing its own greys would make "the light theme" mean two different
/// things depending on which device you opened the file on.
///
/// Two shells cannot share a Qt stylesheet, so this is a copy — the one thing a copy must do is
/// say what it is a copy OF, so the next person changing one knows to change the other. A dark
/// theme, when it comes, is an ADDITION here as it is there, never an edit to these values.
enum Palette {
    /// Toolbars, sidebars, anything that is chrome rather than content.
    static let chrome = Color(hex: 0xF0EFED)
    /// The sidebar sits one step darker than the chrome, as the desktop's tab strip does.
    static let sidebar = Color(hex: 0xE9E8E5)
    /// Document background — the grid of tiles, and later the viewport's clear colour.
    static let canvas = Color(hex: 0xFAFAF9)
    static let surface = Color(hex: 0xFFFFFF)
    /// One-pixel dividers. Never a shadow: the desktop shell draws hairlines and shadows would
    /// read as a different product.
    static let hairline = Color(hex: 0xCFCDC9)
    static let accent = Color(hex: 0x0A6CC4)
    /// Selection and hover washes, in that order.
    static let selection = Color(hex: 0xCDE3F7)
    static let hover = Color(hex: 0xD7E5F3)
    static let text = Color(hex: 0x1F2124)
    static let secondaryText = Color(hex: 0x6C7075)
    static let disabledText = Color(hex: 0xA8ABAF)
}

extension Color {
    /// 0xRRGGBB, so the constants above can be read against the stylesheet they came from without
    /// mentally converting anything.
    init(hex: UInt32) {
        self.init(
            .sRGB,
            red: Double((hex >> 16) & 0xFF) / 255.0,
            green: Double((hex >> 8) & 0xFF) / 255.0,
            blue: Double(hex & 0xFF) / 255.0,
            opacity: 1.0)
    }
}
