import Foundation

/// The list of projects, and the only thing that writes it.
///
/// # Why an index file rather than a directory listing
///
/// A directory listing is the obvious choice and it is wrong here: the document file's name is a
/// UUID (see `Project.fileName`), so the display name has to live somewhere, and a listing cannot
/// hold it. Storing the name *inside* each document would mean opening every document — parsing
/// OCCT shapes — to draw a grid of tiles.
///
/// The index is therefore a cache with an authority problem, and the rule that keeps it honest is:
/// **the index is authoritative for the name, the filesystem is authoritative for existence.**
/// `load()` drops index entries whose file is gone, which is what makes a document deleted from the
/// Files app disappear here rather than becoming a tile that fails to open.
@MainActor
final class ProjectStore: ObservableObject {
    @Published private(set) var projects: [Project] = []

    /// Where documents live. `Documents/` and not a private container, so the Files app can see
    /// them — an iPad CAD app whose files cannot be handed to a slicer is not a 3D-printing client.
    let directory: URL

    private var indexURL: URL { directory.appendingPathComponent("index.json") }

    init(directory: URL? = nil) {
        self.directory =
            directory
            ?? FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        try? FileManager.default.createDirectory(
            at: self.directory, withIntermediateDirectories: true)
        load()
    }

    func load() {
        guard let data = try? Data(contentsOf: indexURL),
            let decoded = try? JSONDecoder().decode([Project].self, from: data)
        else {
            projects = []
            return
        }
        // Existence is the filesystem's call, not the index's. A brand-new project has no file
        // until it is first saved, so a missing file is only stale when the index claims it is old
        // enough to have been written — hence the check is on the file, and creation writes one
        // immediately (see `create`).
        projects = decoded.filter {
            FileManager.default.fileExists(atPath: url(for: $0).path)
        }
        sort()
    }

    func url(for project: Project) -> URL {
        directory.appendingPathComponent(project.fileName)
    }

    @discardableResult
    func create(name: String = "Untitled") -> Project {
        let project = Project(name: uniqueName(from: name))
        // Touch the file at creation so `load()`'s existence filter cannot eat a project the user
        // made and has not yet drawn anything in.
        FileManager.default.createFile(atPath: url(for: project).path, contents: Data())
        projects.append(project)
        sort()
        save()
        return project
    }

    func rename(_ project: Project, to name: String) {
        guard let i = projects.firstIndex(of: project) else { return }
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }
        projects[i].name = trimmed
        projects[i].modified = Date()
        sort()
        save()
    }

    /// Deletes the document as well as the index entry. Called only from a confirmed action in the
    /// UI: this is not recoverable, there being no trash on iOS for an app's own container.
    func delete(_ project: Project) {
        try? FileManager.default.removeItem(at: url(for: project))
        projects.removeAll { $0.id == project.id }
        save()
    }

    func touch(_ project: Project) {
        guard let i = projects.firstIndex(of: project) else { return }
        projects[i].modified = Date()
        sort()
        save()
    }

    /// "Untitled", "Untitled 2", "Untitled 3" — the same convention as the desktop shell, so a
    /// project made on one and opened on the other is not surprising.
    private func uniqueName(from base: String) -> String {
        let taken = Set(projects.map(\.name))
        guard taken.contains(base) else { return base }
        var n = 2
        while taken.contains("\(base) \(n)") { n += 1 }
        return "\(base) \(n)"
    }

    private func sort() {
        projects.sort { $0.modified > $1.modified }
    }

    private func save() {
        guard let data = try? JSONEncoder().encode(projects) else { return }
        try? data.write(to: indexURL, options: .atomic)
    }
}
