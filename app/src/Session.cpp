#include "cad/app/Session.h"

#include "cad/io/DocumentStore.h"

#include <algorithm>

namespace cad::app {

const char* toString(DocumentKind kind) noexcept {
    switch (kind) {
        case DocumentKind::Part:         return "Part";
        case DocumentKind::Assembly:     return "Assembly";
        case DocumentKind::Drawing:      return "Drawing";
        case DocumentKind::Presentation: return "Presentation";
    }
    return "Part";
}

const char* fileExtension(DocumentKind kind) noexcept {
    // Our own extensions rather than Inventor's .ipt/.iam: claiming their extensions would imply
    // a compatibility promise we cannot keep.
    switch (kind) {
        case DocumentKind::Part:         return ".vpart";
        case DocumentKind::Assembly:     return ".vasm";
        case DocumentKind::Drawing:      return ".vdrw";
        case DocumentKind::Presentation: return ".vpres";
    }
    return ".vpart";
}

bool implemented(DocumentKind kind) noexcept {
    // Only Part. The rest are declared so the New panel and the ribbon can show the real shape of
    // the application, and so a user is told plainly rather than clicking something inert.
    return kind == DocumentKind::Part;
}

Session::Session() = default;
Session::~Session() = default;

void Session::onChanged(std::function<void()> fn) { changed_ = std::move(fn); }
void Session::notify() { if (changed_) changed_(); }

std::size_t Session::create(DocumentKind kind) {
    OpenDocument doc;
    doc.kind = kind;
    doc.title = std::string(toString(kind)) + std::to_string(nextPartNumber_++);
    if (implemented(kind)) doc.controller = std::make_unique<Controller>();
    documents_.push_back(std::move(doc));

    active_ = documents_.size() - 1;
    homeActive_ = false;
    notify();
    return active_;
}

void Session::close(std::size_t index) {
    if (index >= documents_.size()) return;
    documents_.erase(documents_.begin() + static_cast<std::ptrdiff_t>(index));

    if (documents_.empty()) {
        activateHome();
        return;
    }
    // Stay on a neighbour rather than jumping home: closing one of several documents should not
    // throw the user out of their work.
    active_ = std::min(index, documents_.size() - 1);
    notify();
}

void Session::activateHome() {
    homeActive_ = true;
    notify();
}

void Session::activate(std::size_t index) {
    if (index >= documents_.size()) return;
    active_ = index;
    homeActive_ = false;
    notify();
}

Controller* Session::active() noexcept {
    if (homeActive_ || active_ >= documents_.size()) return nullptr;
    return documents_[active_].controller.get();
}

const Controller* Session::active() const noexcept {
    if (homeActive_ || active_ >= documents_.size()) return nullptr;
    return documents_[active_].controller.get();
}

std::filesystem::path Session::activePath() const {
    if (homeActive_ || active_ >= documents_.size()) return {};
    return documents_[active_].path;
}

bool Session::activeModified() const {
    if (homeActive_ || active_ >= documents_.size()) return false;
    const auto& doc = documents_[active_];
    return doc.controller != nullptr && doc.controller->modified();
}

kernel::Result<void> Session::saveActive(const std::filesystem::path& path) {
    if (homeActive_ || active_ >= documents_.size()) {
        return kernel::Error{kernel::ErrorCode::InvalidInput, "There is no document to save."};
    }
    auto& doc = documents_[active_];
    if (doc.controller == nullptr) {
        return kernel::Error{kernel::ErrorCode::Unsupported,
                             std::string(toString(doc.kind))
                                 + " documents cannot be saved yet."};
    }

    auto r = doc.controller->saveTo(path, toString(doc.kind));
    if (!r) return r;

    doc.path = path;
    doc.modified = false;
    // The title follows the file once there is one. Before that it is "Part1"; after Save As it
    // should read as the file the user chose, which is what every other application does.
    doc.title = path.stem().string();
    noteRecent(path);
    notify();
    return {};
}

kernel::Result<std::size_t> Session::openDocument(const std::filesystem::path& path) {
    // Read the header first. The KIND comes from the file, not from the extension — a .vpart that
    // is really an assembly must not open as a part, and refusing here is cheaper than discovering
    // it after building a controller for the wrong kind.
    auto info = io::readDocumentInfo(path);
    if (!info) return info.error();

    DocumentKind kind = DocumentKind::Part;
    const std::string& name = info.value().kind;
    if (name == "Assembly") kind = DocumentKind::Assembly;
    else if (name == "Drawing") kind = DocumentKind::Drawing;
    else if (name == "Presentation") kind = DocumentKind::Presentation;

    if (!implemented(kind)) {
        return kernel::Error{kernel::ErrorCode::Unsupported,
                             name + " documents cannot be opened yet.", path.string()};
    }

    // Load into a controller BEFORE it becomes a tab. A file that fails to read must not leave an
    // empty document open for the user to close — nothing visible should change on failure.
    auto controller = std::make_unique<Controller>();
    if (auto r = controller->loadFrom(path); !r) return r.error();

    OpenDocument doc;
    doc.kind = kind;
    doc.title = path.stem().string();
    doc.path = path;
    doc.modified = false;
    doc.controller = std::move(controller);
    documents_.push_back(std::move(doc));

    active_ = documents_.size() - 1;
    homeActive_ = false;
    noteRecent(path);
    notify();
    return active_;
}

void Session::noteRecent(const std::filesystem::path& path) {
    if (path.empty()) return;
    recent_.erase(std::remove(recent_.begin(), recent_.end(), path), recent_.end());
    recent_.insert(recent_.begin(), path);
    if (recent_.size() > 12) recent_.resize(12);
}

}  // namespace cad::app
