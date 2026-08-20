import SwiftUI

/// An open project: the modelling workspace, arranged as Shapr3D arranges it.
///
/// # The layout, and where each piece comes from
///
/// ```
///   ┌─────────────────────────────────────────────────────────────┐
///   │  [rail]        ⌂ Untitled ⌄  Share  ⋯              [rail]   │  document chip, centre top
///   │  [rail]              Line/Arc ⌄                    [rail]   │  contextual tool, when in one
///   │  [rail]                                            [rail]   │
///   │                    the model, everything else                │
///   │  [rail]                                            [rail]   │
///   │  ↺ ↻                                                        │  undo/redo, bottom left
///   └─────────────────────────────────────────────────────────────┘
/// ```
///
/// Tools on the left, view and inspection on the right, document identity floating at the top
/// centre, undo/redo at the bottom corner, and the entire middle left to the model. See `Rail.swift`
/// for why rails rather than a ribbon.
///
/// # Every button here is a real command
///
/// The rails do not hold their own list of tools. Each item names an id in `app/`'s command
/// catalogue — the same catalogue the Qt ribbon is built from — and asks that catalogue whether it
/// is available right now. Nothing in this file decides what Extrude does or when it applies.
///
/// The consequence is visible and intended: a feature that exists in `core/` but has no command yet
/// (Revolve, Hole) is ABSENT from the rail rather than present and dead. Adding it is a change to
/// the shared layer, where both shells gain it at once.
struct ProjectView: View {
    let project: Project
    @EnvironmentObject private var store: ProjectStore
    @EnvironmentObject private var settings: AppSettings
    @Environment(\.dismiss) private var dismiss
    @Environment(\.scenePhase) private var scenePhase

    @State private var showTree = false
    @State private var renaming = false
    @State private var renameText = ""
    /// Set while a rail is pressed and held — Shapr3D's momentary label reveal.
    @State private var revealingLabels = false

    /// The live viewport, once UIKit has made one. Nil for the first instant of the screen's life,
    /// which is why every rail item that needs it is disabled until it arrives.
    @State private var viewport: CadViewportView?
    @State private var status = ""
    @State private var treeRows: [[String: String]] = []
    /// Command id → whether the catalogue says it is available right now.
    @State private var enabled: [String: Bool] = [:]
    @State private var diagnostics: [String: String] = [:]
    @State private var showDiagnostics = false
    @State private var statusToken = 0
    /// nil = the renderer has not answered yet. Not `false`: see the overlay below.
    @State private var rendererStarted: Bool?
    @State private var rendererError = ""
    /// Whether a sketch is open, and whether the next tap picks the plane to open it on.
    @State private var sketching = false
    @State private var choosingPlane = false

    private var labels: Bool { settings.toolLabels || revealingLabels }

    var body: some View {
        ZStack {
            modelSurface

            HStack(alignment: .top, spacing: 0) {
                leftRail
                Spacer(minLength: 0)
                rightRail
            }
            .padding(.horizontal, 12)
            .padding(.top, 12)
            // The reveal gesture is on the RAILS, not the screen.
            //
            // It was on the whole ZStack first, which is wrong the moment the viewport is live: a
            // screen-wide long press competes with the one-finger drag that orbits the model, and
            // the model would stutter or stop rotating whenever a finger paused.
            .simultaneousGesture(
                LongPressGesture(minimumDuration: 0.35)
                    .onChanged { _ in revealingLabels = true }
                    .onEnded { _ in revealingLabels = false }
            )

            VStack {
                documentChip
                Spacer()
            }
            .padding(.top, 12)

            VStack {
                Spacer()
                HStack(alignment: .bottom) {
                    historyButtons
                    Spacer()
                }
                // Centred on the SCREEN, not in what is left over beside the undo pair. As a third
                // element of the row it drifted whenever the buttons changed width; an overlay is
                // centred on the row itself and cannot.
                .overlay(alignment: .bottom) { statusLine }
            }
            .padding(12)

            if showDiagnostics {
                VStack {
                    Spacer()
                    HStack {
                        Spacer()
                        diagnosticsPanel
                    }
                }
                .padding(12)
            }

            if showTree {
                HStack {
                    Spacer()
                    treePanel
                }
                .padding(.trailing, 74)
                .padding(.vertical, 12)
                .transition(.move(edge: .trailing).combined(with: .opacity))
            }
        }
        .alert("Rename Project", isPresented: $renaming) {
            TextField("Name", text: $renameText)
            Button("Rename") { store.rename(project, to: renameText) }
            Button("Cancel", role: .cancel) {}
        }
        // The document is opened once the view exists, not in `init`: the file is read by the C++
        // side, and there is no C++ side until UIKit has made the view.
        .onChange(of: viewport == nil) { _, _ in openDocument() }
        // Saving on the way out covers the home button; this covers everything else. iPadOS can
        // suspend and then terminate an app without ever returning to it, and a modeller that
        // loses an afternoon's work to a swipe up is not one anybody keeps using.
        .onChange(of: scenePhase) { _, phase in
            guard phase != .active else { return }
            _ = viewport?.saveDocument(atPath: store.url(for: project).path)
        }
        .statusBarHidden(false)
    }

    // MARK: - The model

    private var modelSurface: some View {
        ViewportView(
            handle: $viewport,
            onStatus: { showStatus($0) },
            onDocumentChanged: {
                treeRows = viewport?.tree() ?? []
                diagnostics = viewport?.diagnostics() ?? [:]
            },
            onStarted: { ok, error in
                rendererStarted = ok
                rendererError = error
                diagnostics = viewport?.diagnostics() ?? [:]
            },
            onPlaneTap: { x, y in
                guard choosingPlane else { return false }
                if viewport?.beginSketch(at: CGPoint(x: x, y: y)) == true {
                    choosingPlane = false
                    sketching = true
                    status = stylusHint
                }
                return true
            }
        )
        .ignoresSafeArea()
        // The failure overlay, which is the whole point of `attached` being exposed. Without it a
        // renderer that never started looks exactly like a renderer that started and drew nothing —
        // and no amount of staring at the screen distinguishes them.
        // Shown only once the renderer has REPORTED a failure. `rendererStarted` is nil until then,
        // which is the state this was missing: "not started yet" and "could not start" are
        // different things, and showing the banner for the first is how a working viewport gets
        // reported as broken.
        .overlay {
            if rendererStarted == false {
                ViewportFailure(
                    reason: rendererError.isEmpty
                        ? "The renderer reported no reason." : rendererError)
            }
        }
    }

    /// What the renderer says it is doing. Shown on demand rather than always, because it is a
    /// diagnostic and not part of the product — but shown at all, because "it does not render" has
    /// four causes and they are indistinguishable by looking.
    private var diagnosticsPanel: some View {
        VStack(alignment: .leading, spacing: 4) {
            ForEach(diagnostics.keys.sorted(), id: \.self) { key in
                HStack(spacing: 8) {
                    Text(key)
                        .foregroundStyle(Palette.secondaryText)
                    Spacer()
                    Text(diagnostics[key] ?? "")
                        .foregroundStyle(Palette.text)
                }
                .font(.caption.monospaced())
            }
        }
        .padding(10)
        .frame(width: 240)
        .background(Palette.chrome, in: RoundedRectangle(cornerRadius: 10))
        .overlay(RoundedRectangle(cornerRadius: 10).stroke(Palette.hairline, lineWidth: 1))
    }

    /// What draws, in this session, on this device.
    ///
    /// Said out loud because the rule is invisible otherwise: a user with a stylus never discovers
    /// that a finger would have drawn, and a user without one must not be left wondering why
    /// nothing happens. It is a statement of fact, not an instruction — the mode was decided by
    /// which instrument they picked up.
    private var stylusHint: String {
        (viewport?.stylusSeen == true)
            ? "Draw with the stylus · one finger orbits · two pan"
            : "Draw with one finger · two fingers pan · pinch to zoom"
    }

    /// One finger orbits, two pan, pinch zooms — and the status line says so, once, until the user
    /// does something. The gestures themselves live in the bridge; this is only the caption.
    private var statusLine: some View {
        Text(status.isEmpty ? "Drag to orbit · two fingers to pan · pinch to zoom" : status)
            .font(.caption)
            .foregroundStyle(Palette.secondaryText)
            .padding(.horizontal, 12)
            .padding(.vertical, 6)
            .background(Palette.chrome, in: Capsule())
            .overlay(Capsule().stroke(Palette.hairline, lineWidth: 1))
    }

    // MARK: - Document chip

    /// Who you are and where you are, floating at the top centre. Shapr3D's home button is the way
    /// out of a document, and it is a *home* glyph rather than a back chevron because a document is
    /// a place rather than a page in a stack.
    private var documentChip: some View {
        HStack(spacing: 10) {
            Button {
                closeDocument()
            } label: {
                Image(systemName: "house")
                    .font(.system(size: 16))
                    .frame(width: 34, height: 34)
            }
            .tint(Palette.accent)

            Menu {
                Button("Rename…") {
                    renameText = project.name
                    renaming = true
                }
                Button("Export…") {}.disabled(true)
                Divider()
                Button("Close") { closeDocument() }
            } label: {
                HStack(spacing: 5) {
                    Text(project.name)
                        .font(.subheadline.weight(.medium))
                        .foregroundStyle(Palette.text)
                    Image(systemName: "chevron.down")
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundStyle(Palette.secondaryText)
                }
            }

            Button("Share") {}
                .font(.subheadline.weight(.medium))
                .buttonStyle(.borderedProminent)
                .tint(Palette.accent)
                .controlSize(.small)
                .disabled(true)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(Palette.chrome, in: Capsule())
        .overlay(Capsule().stroke(Palette.hairline, lineWidth: 1))
    }

    /// Shows a message and takes it away again.
    ///
    /// A status line that keeps the last thing that happened is lying by the time you read it —
    /// "Added Box" was still on screen several commands later. The token guards against an earlier
    /// message's timer wiping a newer one.
    private func showStatus(_ text: String) {
        status = text
        statusToken &+= 1
        let mine = statusToken
        DispatchQueue.main.asyncAfter(deadline: .now() + 4) {
            if statusToken == mine { status = "" }
        }
    }

    // MARK: - Commands

    /// A rail item backed by a catalogue command.
    ///
    /// Enablement is the catalogue's answer, not a guess made here: `CommandContext` knows whether
    /// anything is selected, whether the document is empty and whether there is anything to undo,
    /// and a shell that re-derived that would eventually disagree with the command it is enabling.
    /// A button that is enabled and then fails is worse than one that is greyed out.
    private func command(_ title: String, _ symbol: String, _ id: String) -> RailItem {
        let on = enabled[id] ?? false
        return RailItem(
            title, symbol,
            action: on
                ? {
                    viewport?.runCommand(id)
                    refreshCommandState()
                } : nil)
    }

    private func refreshCommandState() {
        guard let viewport else { return }
        var next: [String: Bool] = [:]
        for entry in viewport.commands() {
            guard let id = entry["id"] else { continue }
            next[id] = entry["enabled"] == "1"
        }
        enabled = next
        treeRows = viewport.tree()
        diagnostics = viewport.diagnostics()
    }

    // MARK: - Document

    /// Opens the project's file, if it has one yet.
    ///
    /// A newly created project is a zero-byte placeholder — `ProjectStore` writes it so the index's
    /// existence filter cannot eat a project the user just made — and handing an empty file to the
    /// document reader is an error, not an empty document. Hence the size check rather than a
    /// try-and-ignore, which would also swallow a genuinely corrupt file.
    private func openDocument() {
        guard let viewport else { return }
        let url = store.url(for: project)
        let attributes = try? FileManager.default.attributesOfItem(atPath: url.path)
        let size = (attributes?[.size] as? Int) ?? 0
        if size > 0 {
            if !viewport.openDocument(atPath: url.path) {
                status = "This project could not be opened: \(viewport.lastError)"
            }
        }
        refreshCommandState()
    }

    /// Saves on the way out. Explicit rather than continuous: writing the document after every
    /// command would make each one cost a serialisation, and there is no autosave contract yet for
    /// the two shells to share.
    private func closeDocument() {
        if let viewport, !viewport.saveDocument(atPath: store.url(for: project).path) {
            status = "This project could not be saved: \(viewport.lastError)"
            return
        }
        store.touch(project)
        dismiss()
    }

    // MARK: - Rails

    /// Tools, left edge.
    ///
    /// Every item here names a command id from `app/`'s catalogue — the SAME catalogue the Qt
    /// ribbon is built from. Nothing in this shell decides what "Extrude" does or when it is
    /// available; it asks. That is the difference between two shells over one application and two
    /// applications that resemble each other.
    ///
    /// So the rail can only offer what the catalogue has. Revolve and Hole compute correctly in
    /// `core/features` but have no command yet, and are therefore absent rather than present and
    /// dead — adding them is a change to `app/`, where both shells get them at once.
    private var leftRail: some View {
        // Aligned to its own edge and NOT given a fixed width: the label column grows into the
        // canvas when labels are shown, and a fixed width would clip it — which is what put
        // "Display Modes" half off the right side of the screen.
        VStack(alignment: .leading, spacing: 12) {
            RailGroup(
                items: [
                    command("Box", "cube", "feature.box"),
                    command("Cylinder", "cylinder", "feature.cylinder"),
                    RailItem(
                        "Sketch", "pencil.and.outline", selected: sketching || choosingPlane,
                        action: viewport == nil
                            ? nil
                            : {
                                if sketching {
                                    viewport?.finishSketch()
                                    sketching = false
                                } else {
                                    // Pick the surface, THEN draw — the order every CAD
                                    // application uses. The next tap chooses it.
                                    choosingPlane = true
                                    status = "Tap a flat face to sketch on"
                                }
                            }),
                    command("Extrude", "arrow.up.square", "feature.extrude"),
                ], showLabels: labels, edge: .leading)

            RailGroup(
                items: [
                    command("Join", "square.on.square", "feature.fuse"),
                    command("Cut", "square.on.square.dashed", "feature.cut"),
                    command("Intersect", "square.on.square.intersection.dashed", "feature.common"),
                    command("Fillet", "circle.bottomrighthalf.checkered", "feature.fillet"),
                    command("Chamfer", "triangle", "feature.chamfer"),
                ], showLabels: labels, edge: .leading)

            RailGroup(
                items: [
                    command("Delete", "trash", "edit.delete")
                ], showLabels: labels, edge: .leading)

            Spacer(minLength: 0)
        }
    }

    /// View and inspection, right edge. Nothing here changes the model, which is the line Shapr3D
    /// draws between its two rails and the reason a user can reach for one side without thinking.
    private var rightRail: some View {
        VStack(alignment: .trailing, spacing: 12) {
            RailGroup(
                items: [
                    RailItem(
                        "Fit", "viewfinder", action: viewport == nil ? nil : { viewport?.fitCamera() }
                    ),
                    command("Orthographic", "cube.transparent", "view.ortho"),
                ], showLabels: labels, edge: .trailing)

            RailGroup(
                items: [
                    RailItem(
                        "Items", "list.bullet.indent", selected: showTree,
                        action: { withAnimation(.easeOut(duration: 0.18)) { showTree.toggle() } }),
                    command("Roll Back", "clock.arrow.circlepath", "edit.rollback"),
                    command("Roll Forward", "clock.arrow.2.circlepath", "edit.rollforward"),
                    RailItem(
                        "Renderer", "waveform.path.ecg", selected: showDiagnostics,
                        action: {
                            diagnostics = viewport?.diagnostics() ?? [:]
                            showDiagnostics.toggle()
                        }),
                ], showLabels: labels, edge: .trailing)

            Spacer(minLength: 0)
        }
    }

    /// Undo and redo, bottom left, where Shapr3D puts them: away from every other control, because
    /// they are the two commands a user reaches for without looking.
    private var historyButtons: some View {
        HStack(spacing: 8) {
            ForEach(
                [
                    command("Undo", "arrow.uturn.backward", "edit.undo"),
                    command("Redo", "arrow.uturn.forward", "edit.redo"),
                ]
            ) { item in
                Button {
                    item.action?()
                } label: {
                    Image(systemName: item.symbol)
                        .font(.system(size: 16))
                        .frame(width: 42, height: 42)
                        .foregroundStyle(item.action == nil ? Palette.disabledText : Palette.text)
                        .background(Palette.chrome, in: Circle())
                        .overlay(Circle().stroke(Palette.hairline, lineWidth: 1))
                }
                .disabled(item.action == nil)
                .accessibilityLabel(item.title)
            }
        }
    }

    // MARK: - Items panel

    /// The model tree, as a pull-out rather than a permanent dock — IPAD_UX.md, "Feature tree". It
    /// is present because editing history is the entire reason to use a parametric modeller, and
    /// hidden by default because a permanent dock costs more screen than it returns on a tablet.
    ///
    /// Its contents come from `Controller::tree()`, which already returns data rather than widgets.
    private var treePanel: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text("Items")
                .font(.caption.weight(.semibold))
                .foregroundStyle(Palette.secondaryText)
                .padding(.horizontal, 12)
                .padding(.vertical, 9)
            Divider()
            if treeRows.isEmpty {
                Text("Nothing yet")
                    .font(.callout)
                    .foregroundStyle(Palette.disabledText)
                    .padding(12)
            } else {
                ScrollView {
                    VStack(alignment: .leading, spacing: 0) {
                        ForEach(Array(treeRows.enumerated()), id: \.offset) { _, row in
                            HStack(spacing: 8) {
                                Text(row["label"] ?? "")
                                    .font(.callout)
                                    .foregroundStyle(Palette.text)
                                Spacer()
                                if !(row["error"] ?? "").isEmpty {
                                    Image(systemName: "exclamationmark.triangle.fill")
                                        .font(.caption)
                                        .foregroundStyle(.orange)
                                }
                            }
                            .padding(.horizontal, 12)
                            .padding(.vertical, 7)
                        }
                    }
                }
            }
            Spacer()
        }
        .frame(width: 240)
        .background(Palette.chrome, in: RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(Palette.hairline, lineWidth: 1))
        .padding(.top, 58)
    }
}
