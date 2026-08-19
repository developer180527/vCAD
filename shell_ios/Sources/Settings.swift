import SwiftUI

/// Preferences, and the one place that persists them.
///
/// # Why these live in Swift rather than in `app/`
///
/// A tension, and worth stating: `Preferences` already exists in `app/include/cad/app/Model.h`, and
/// duplicating it is exactly the mistake IPAD_UX.md warns about. The split used here is by
/// AUDIENCE, not convenience — settings that change what the MODEL does (display units, auto look
/// at sketch) are the shared layer's and will be read from it the moment the bridge exists; settings
/// that change what this SHELL looks like (appearance, tool labels) are this shell's and have no
/// counterpart on the desktop.
///
/// Until the bridge lands, the model-facing ones are stored here and marked below, so the wiring
/// step has a list rather than a search.
@MainActor
final class AppSettings: ObservableObject {
    // MARK: Shell-facing — this shell's, permanently.

    /// The colour scheme. Paper White is the only one implemented, and the enum has one case for
    /// that reason: an option that does nothing is a lie the user has to discover by tapping it.
    /// A dark theme is an addition here, not an edit to Paper White's values.
    @AppStorage("appearance") var appearance: Appearance = .paperWhite

    /// Whether the tool rails show their names beside their glyphs.
    ///
    /// Shapr3D shows labels while a rail is touched and hides them at rest, which is right for an
    /// expert and hostile to someone opening the app for the first time. Both are available: this
    /// pins them on, and a press-and-hold reveals them either way.
    @AppStorage("toolLabels") var toolLabels: Bool = true

    // MARK: Model-facing — these move to `app/`'s Preferences when the bridge lands.

    /// The units dimensions are shown and typed in. `units::UnitSystem` on the C++ side.
    @AppStorage("displayUnits") var displayUnits: Units = .millimetre

    /// Rotate to face the sketch plane on entering a sketch. Fusion's "Auto Look At Sketch";
    /// SolidWorks made the same option default-on in 2021 (MODELLING_UX.md §2).
    @AppStorage("autoLookAtSketch") var autoLookAtSketch: Bool = true

    /// Draw the grid on the sketch plane.
    @AppStorage("showGrid") var showGrid: Bool = true

    enum Appearance: String, CaseIterable, Identifiable {
        case paperWhite
        var id: String { rawValue }
        var title: String { "Paper White" }
    }

    enum Units: String, CaseIterable, Identifiable {
        case millimetre, centimetre, inch
        var id: String { rawValue }
        var title: String {
            switch self {
            case .millimetre: return "Millimetres"
            case .centimetre: return "Centimetres"
            case .inch: return "Inches"
            }
        }
        var suffix: String {
            switch self {
            case .millimetre: return "mm"
            case .centimetre: return "cm"
            case .inch: return "in"
            }
        }
    }
}

/// The settings sheet, opened from the home screen.
///
/// A sheet and not a screen: settings are a detour, and a detour that replaces the whole view makes
/// the user find their way back. Every iPad app the user already owns does this the same way.
struct SettingsView: View {
    @EnvironmentObject private var settings: AppSettings
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Form {
                Section("Appearance") {
                    Picker("Theme", selection: $settings.appearance) {
                        ForEach(AppSettings.Appearance.allCases) { Text($0.title).tag($0) }
                    }
                    Toggle("Show tool labels", isOn: $settings.toolLabels)
                }

                Section {
                    Picker("Units", selection: $settings.displayUnits) {
                        ForEach(AppSettings.Units.allCases) { Text($0.title).tag($0) }
                    }
                    Toggle("Show grid", isOn: $settings.showGrid)
                    Toggle("Look at sketch plane", isOn: $settings.autoLookAtSketch)
                } header: {
                    Text("Modelling")
                } footer: {
                    // Said out loud rather than left to be discovered: these are stored and will be
                    // read by the shared layer when the viewport is wired, and until then they
                    // change nothing visible.
                    Text("Applies once the viewport is connected.")
                }

                Section("About") {
                    LabeledContent("Version", value: Bundle.version)
                    LabeledContent("Renderer", value: "bgfx / Metal")
                }
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
        .tint(Palette.accent)
    }
}

extension Bundle {
    static var version: String {
        let short = main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "0"
        let build = main.infoDictionary?["CFBundleVersion"] as? String ?? "0"
        return short == build ? short : "\(short) (\(build))"
    }
}
