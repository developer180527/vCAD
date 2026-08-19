import Foundation

/// A project, as the home screen knows it.
///
/// # Why this is metadata and not a document
///
/// What a vCAD document *contains* — features, sketches, the naming map — is `core/`'s, and it is
/// read and written by C++ that already exists and is already tested. Duplicating any part of that
/// shape in Swift would create a second definition of the same thing, and the two would disagree
/// the first time a feature was added on the desktop side.
///
/// So this holds only what a *file browser* needs: what it is called, where it is, and when it was
/// touched. Opening it hands the URL to the shared layer and gets a session back.
struct Project: Identifiable, Codable, Hashable {
    let id: UUID
    var name: String
    /// Last modification, kept in the index rather than stat'd per row: a grid of fifty tiles
    /// would otherwise hit the filesystem fifty times on every redraw, which on a scroll is every
    /// frame.
    var modified: Date

    /// The document's file name inside the projects directory. Derived from the id rather than the
    /// name, because two projects may share a name and renaming one must not move its file.
    var fileName: String { "\(id.uuidString).vcad" }

    init(id: UUID = UUID(), name: String, modified: Date = Date()) {
        self.id = id
        self.name = name
        self.modified = modified
    }
}
