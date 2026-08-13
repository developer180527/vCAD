#pragma once

// N open documents, plus which one is active. The application, as distinct from one document.
//
// Controller used to be the application; it is now one open document (ADR 0009). This split is
// worth making before the second document kind exists, because "one document forever" leaks into
// every signature and is miserable to undo later.

#include "cad/app/Controller.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace cad::app {

/// The four Inventor document kinds. Declared in full even though only Part is implemented: the
/// shape of the app should be visible and honest from the start rather than appearing later and
/// moving everything.
enum class DocumentKind : std::uint8_t {
    Part,           ///< features on one solid body — implemented
    Assembly,       ///< references to other documents with placement and joints
    Drawing,        ///< 2D sheets with projected views; paper space, not model space
    Presentation,   ///< exploded views and animation
};

const char* toString(DocumentKind) noexcept;
const char* fileExtension(DocumentKind) noexcept;
[[nodiscard]] bool implemented(DocumentKind) noexcept;

/// One open document. Home is NOT one of these — see Session::homeActive.
struct OpenDocument {
    DocumentKind kind = DocumentKind::Part;
    std::string title;                       ///< "Part1", or the file stem once saved
    std::filesystem::path path;              ///< empty until saved
    bool modified = false;
    std::unique_ptr<Controller> controller;  ///< null for kinds not yet implemented
};

/// Search paths and the shared cache. Minimal, and both parts earn their place: search paths are
/// what make an assembly's references resolve after a file moves, and the shared cache path is
/// what makes "open this project" also mean "use the team's DDC" (ADR 0004).
struct Project {
    std::string name;
    std::filesystem::path root;
    std::vector<std::filesystem::path> searchPaths;
    std::filesystem::path sharedCache;
    [[nodiscard]] bool loaded() const noexcept { return !root.empty(); }
};

class Session {
public:
    Session();
    ~Session();

    /// Creates a document and makes it active. Kinds that are not implemented still open, with a
    /// null controller, so the UI can say so plainly instead of the command silently doing
    /// nothing.
    std::size_t create(DocumentKind);
    void close(std::size_t index);

    /// Home is index -1, conceptually: `homeActive()` rather than a document at index 0, so
    /// every "for each open document" loop needs no special case.
    void activateHome();
    void activate(std::size_t index);
    [[nodiscard]] bool homeActive() const noexcept { return homeActive_; }
    [[nodiscard]] std::size_t activeIndex() const noexcept { return active_; }

    [[nodiscard]] const std::vector<OpenDocument>& documents() const noexcept { return documents_; }
    [[nodiscard]] std::size_t count() const noexcept { return documents_.size(); }

    /// Null when Home is active or the active kind has no controller yet. Callers must check;
    /// that is the price of declaring kinds before implementing them, and it is cheaper than
    /// pretending a Drawing has a 3D controller.
    [[nodiscard]] Controller* active() noexcept;
    [[nodiscard]] const Controller* active() const noexcept;

    [[nodiscard]] Project& project() noexcept { return project_; }

    /// Most recently opened files, for the Home page. Persisted by the shell for now.
    [[nodiscard]] const std::vector<std::filesystem::path>& recent() const noexcept {
        return recent_;
    }
    void noteRecent(const std::filesystem::path&);

    /// Saves the active document, remembering the path so a later Save needs no dialog.
    ///
    /// The shell supplies the path — Session must not open a file dialog, for the same reason
    /// Controller must not: this header compiles for the SwiftUI shell too.
    kernel::Result<void> saveActive(const std::filesystem::path&);

    /// Opens a document into a NEW tab and makes it active. Returns its index.
    ///
    /// A new tab rather than reusing the active one: Inventor opens documents alongside each
    /// other, and silently replacing what the user was looking at is how unsaved work disappears.
    /// The kind comes from the file's own header, not from its extension, so a renamed file still
    /// opens as what it actually is.
    kernel::Result<std::size_t> openDocument(const std::filesystem::path&);

    /// Path of the active document, empty if it has never been saved. Also empty for Home.
    [[nodiscard]] std::filesystem::path activePath() const;

    /// Whether the active document has unsaved changes. False for Home, which has no file.
    [[nodiscard]] bool activeModified() const;

    void onChanged(std::function<void()>);

private:
    void notify();

    std::vector<OpenDocument> documents_;
    std::vector<std::filesystem::path> recent_;
    Project project_;
    std::size_t active_ = 0;
    bool homeActive_ = true;
    std::size_t nextPartNumber_ = 1;
    std::function<void()> changed_;
};

}  // namespace cad::app
