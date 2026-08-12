#include "cad/app/Session.h"

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

void Session::noteRecent(const std::filesystem::path& path) {
    if (path.empty()) return;
    recent_.erase(std::remove(recent_.begin(), recent_.end(), path), recent_.end());
    recent_.insert(recent_.begin(), path);
    if (recent_.size() > 12) recent_.resize(12);
}

}  // namespace cad::app
