import SwiftUI

/// The screen the app opens on: a sidebar, a grid of projects, and a way to make a new one.
///
/// # Why a home screen at all, when the desktop shell has none
///
/// The desktop opens into a document because it has a filesystem, a window title bar and a File
/// menu. iPad has none of those in a form a user reaches for, so the app itself has to be the file
/// browser. Shapr3D, Fusion for iPad and Onshape all resolve this the same way — a sidebar of
/// collections on the left, a grid of thumbnails on the right, one prominent New button — which
/// makes it the convention rather than a design decision to relitigate.
///
/// # NavigationSplitView, not a custom layout
///
/// It gives the iPad-native behaviours for free and, more to the point, correctly: the sidebar
/// collapses in Slide Over and portrait, comes back on a swipe, and keeps the system's own
/// animation. A hand-rolled two-pane HStack looks identical in a screenshot and is wrong in every
/// one of those cases.
struct HomeView: View {
    @EnvironmentObject private var store: ProjectStore
    @EnvironmentObject private var settings: AppSettings
    @State private var section: Section? = .projects
    @State private var open: Project?
    @State private var renaming: Project?
    @State private var renameText: String = ""
    @State private var confirmingDelete: Project?
    @State private var showSettings = false

    enum Section: String, CaseIterable, Identifiable {
        case projects
        case recent

        var id: String { rawValue }
        var title: String {
            switch self {
            case .projects: return "All Projects"
            case .recent: return "Recent"
            }
        }
        var symbol: String {
            switch self {
            case .projects: return "square.grid.2x2"
            case .recent: return "clock"
            }
        }
    }

    var body: some View {
        NavigationSplitView {
            sidebar
        } detail: {
            grid
        }
        .tint(Palette.accent)
        .fullScreenCover(item: $open) { project in
            ProjectView(project: project)
                .environmentObject(store)
                .environmentObject(settings)
        }
        .sheet(isPresented: $showSettings) {
            SettingsView().environmentObject(settings)
        }
        .alert("Rename Project", isPresented: renamingBinding, presenting: renaming) { project in
            // The text lives here rather than in a child view because an alert's buttons are the
            // only things it reliably renders — a helper view holding its own @State would have to
            // seed that state from something the alert does not run.
            TextField("Name", text: $renameText)
            Button("Rename") { store.rename(project, to: renameText) }
            Button("Cancel", role: .cancel) {}
        }
        .confirmationDialog(
            "Delete this project?", isPresented: deletingBinding, presenting: confirmingDelete
        ) { project in
            // Destructive and unrecoverable — there is no trash for an app container — so it is a
            // confirmation, and the button says what it deletes rather than "OK".
            Button("Delete \(project.name)", role: .destructive) { store.delete(project) }
            Button("Cancel", role: .cancel) {}
        }
    }

    // MARK: - Sidebar

    private var sidebar: some View {
        List(selection: $section) {
            ForEach(Section.allCases) { item in
                Label(item.title, systemImage: item.symbol).tag(item)
            }
        }
        .listStyle(.sidebar)
        .scrollContentBackground(.hidden)
        .background(Palette.sidebar)
        .navigationTitle("vCAD")
        // Settings at the FOOT of the sidebar, not in the projects toolbar: it belongs to the app
        // rather than to whichever collection happens to be selected, and the foot is where every
        // iPad app the user already owns keeps it.
        .safeAreaInset(edge: .bottom) {
            VStack(spacing: 0) {
                Divider()
                Button {
                    showSettings = true
                } label: {
                    Label("Settings", systemImage: "gearshape")
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.horizontal, 20)
                        .padding(.vertical, 12)
                        .contentShape(Rectangle())
                }
                .tint(Palette.text)
            }
            .background(Palette.sidebar)
        }
    }

    // MARK: - Detail

    private var shown: [Project] {
        switch section ?? .projects {
        case .projects: return store.projects
        case .recent:
            let week = Date().addingTimeInterval(-7 * 24 * 3600)
            return store.projects.filter { $0.modified >= week }
        }
    }

    private var grid: some View {
        ScrollView {
            if shown.isEmpty {
                empty
            } else {
                LazyVGrid(
                    columns: [GridItem(.adaptive(minimum: 200, maximum: 260), spacing: 20)],
                    spacing: 20
                ) {
                    ForEach(shown) { project in
                        ProjectTile(project: project)
                            .onTapGesture { openProject(project) }
                            .contextMenu {
                                Button("Rename") {
                                    renameText = project.name
                                    renaming = project
                                }
                                Button("Delete", role: .destructive) { confirmingDelete = project }
                            }
                    }
                }
                .padding(20)
            }
        }
        .background(Palette.canvas)
        .navigationTitle((section ?? .projects).title)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button {
                    openProject(store.create())
                } label: {
                    Label("New", systemImage: "plus")
                }
            }
        }
    }

    private var empty: some View {
        VStack(spacing: 14) {
            Image(systemName: "cube")
                .font(.system(size: 44, weight: .light))
                .foregroundStyle(Palette.disabledText)
            Text("No projects yet")
                .font(.title3)
                .foregroundStyle(Palette.text)
            Text("New starts a part on the XY plane.")
                .font(.callout)
                .foregroundStyle(Palette.secondaryText)
            Button("New Project") { openProject(store.create()) }
                .buttonStyle(.borderedProminent)
                .padding(.top, 4)
        }
        .frame(maxWidth: .infinity)
        .padding(.top, 120)
    }

    private func openProject(_ project: Project) {
        store.touch(project)
        open = project
    }

    // Presenting an optional through a bool binding: `alert(isPresented:presenting:)` wants both,
    // and deriving the bool keeps the optional as the single source of truth.
    private var renamingBinding: Binding<Bool> {
        Binding(get: { renaming != nil }, set: { if !$0 { renaming = nil } })
    }
    private var deletingBinding: Binding<Bool> {
        Binding(get: { confirmingDelete != nil }, set: { if !$0 { confirmingDelete = nil } })
    }
}

/// One project in the grid.
///
/// The thumbnail is a placeholder until the viewport can render one: a tile that lies about a
/// model's contents is worse than a tile that shows a glyph, and a real thumbnail means rendering
/// offscreen, which is the next milestone's work rather than this one's.
private struct ProjectTile: View {
    let project: Project

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            ZStack {
                Palette.surface
                Image(systemName: "cube.transparent")
                    .font(.system(size: 40, weight: .ultraLight))
                    .foregroundStyle(Palette.disabledText)
            }
            .frame(height: 140)

            VStack(alignment: .leading, spacing: 2) {
                Text(project.name)
                    .font(.subheadline.weight(.medium))
                    .foregroundStyle(Palette.text)
                    .lineLimit(1)
                Text(project.modified.formatted(date: .abbreviated, time: .shortened))
                    .font(.caption)
                    .foregroundStyle(Palette.secondaryText)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(10)
            .background(Palette.chrome)
        }
        .clipShape(RoundedRectangle(cornerRadius: 6))
        .overlay(
            RoundedRectangle(cornerRadius: 6).stroke(Palette.hairline, lineWidth: 1)
        )
    }
}
