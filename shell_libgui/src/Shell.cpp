#include "Shell.h"

#include "cad/render/UiPass.h"
#include "cad/units/Units.h"

#include <libgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

extern "C" const char cad_libgui_theme[];

namespace cad::shell_libgui {

namespace {

// Tab ids. Stable, because the dock layout is saved by them.
constexpr std::uint64_t kTabBrowser = 1;
constexpr std::uint64_t kTabViewport = 2;

/// What libgui hands back as texture_index for the viewport. The UiPass resolves it to the scene's
/// texture each frame, because that texture is recreated whenever the viewport resizes.
constexpr std::uint64_t kSceneTexture = 1;
/// The ribbon's icon atlas, as a host texture.
constexpr std::uint64_t kIconTexture = 2;


}  // namespace

Shell::Shell(std::span<const std::uint8_t> font) : font_(font), main_(gui::Surface::create(font)) {
    dock_ = libgui_dock_new();
    // One window for now. Tearing a panel out into its own OS window is proven (scratchpad) and is
    // a later slice: a torn-off viewport needs a second swap chain in the UiPass, which it has.
    libgui_dock_set_in_app_floating(dock_, 1);
    const std::uint64_t root = libgui_dock_split(dock_, 0, 0.22f, libgui_dock_leaf(dock_, kTabBrowser),
                                                 libgui_dock_leaf(dock_, kTabViewport));
    libgui_dock_set_root(dock_, 0, root);
    controller_.onStatus([this](const std::string& s) { message_ = s; });
    applyTheme();
}

Shell::~Shell() {
    uiPass_.reset();  // before the Controller's backend goes: it owns bgfx
    libgui_dock_free(dock_);
}

kernel::Result<void> Shell::attach(void* nativeView, std::uint32_t pixelWidth, std::uint32_t pixelHeight,
                                   double scale) {
    if (!main_) return kernel::Error{kernel::ErrorCode::Internal, "The interface could not start.", gui::Surface::lastError()};
    if (auto r = controller_.attachCompositedRenderer(nativeView, pixelWidth, pixelHeight, scale); !r) return r;
    auto* backend = controller_.renderer();
    if (backend == nullptr || controller_.sceneTexture() == render::TextureId::None)
        return kernel::Error{kernel::ErrorCode::Internal, "The renderer did not attach to the window.",
                             "no scene texture in composited mode"};
    auto pass = render::UiPass::create(*backend, gui::Surface::vertexLayout(), gui::Surface::vertexStride());
    if (!pass) return pass.error();
    uiPass_ = std::move(pass.value());
    // 64 px cells: a large icon is 32 logical px, so this is sharp at 2x and filters down for 1x.
    icons_ = IconAtlas::build(64);
    iconTexture_ = uiPass_->createImage(icons_.rgba, icons_.width, icons_.height);
    uiPass_->setHostTextures(
        [](std::uint64_t texture, void* user) -> render::TextureId {
            auto* self = static_cast<Shell*>(user);
            if (texture == kSceneTexture) return self->controller_.sceneTexture();
            if (texture == kIconTexture) return self->iconTexture_;
            return render::TextureId::None;
        },
        this);
    surfaceWidth_ = pixelWidth;
    surfaceHeight_ = pixelHeight;
    controller_.setViewportBackground(255, 255, 255);  // Inventor's white graphics window
    return {};
}

kernel::Result<void> Shell::open(const std::string& path) {
    auto r = controller_.loadFrom(path);
    if (r) {
        path_ = path;
        controller_.fitView();
    }
    return r;
}

void Shell::runCommand(const std::string& id) {
    for (const auto& c : controller_.commands()) {
        if (c.id != id) continue;
        if (c.enabled && !c.enabled(controller_.context())) return;
        c.invoke();
        return;
    }
}

void Shell::tick(float w, float h, float scale, float dt, float originX, float originY, Buttons held,
                 float pointerX, float pointerY) {
    if (!uiPass_) return;
    scale_ = scale;
    held_ = held;
    const auto pw = std::uint32_t(std::lround(w * scale));
    const auto ph = std::uint32_t(std::lround(h * scale));
    if (pw != surfaceWidth_ || ph != surfaceHeight_) {
        controller_.resizeSurface(pw, ph);  // the window's swap chain, which the interface fills
        surfaceWidth_ = pw;
        surfaceHeight_ = ph;
    }

    libgui_dock_set_pointer(dock_, pointerX, pointerY, held.primary ? 1 : 0);
    libgui_dock_set_surface_frame(dock_, 0, originX, originY, scale);
    libgui_dock_update(dock_);

    LibguiTabViewer viewer{};
    viewer.title = [](std::uint64_t tab, char* buf, std::uint64_t cap, void*) {
        std::snprintf(buf, cap, "%s", tab == kTabBrowser ? "Model" : "Viewport");
    };
    viewer.ui = [](LibguiUi* ui, std::uint64_t tab, void* self) { static_cast<Shell*>(self)->panel(ui, tab); };
    viewer.scroll = [](std::uint64_t tab, void*) -> std::uint8_t { return tab != kTabViewport; };
    viewer.user = this;

    // The ribbon on top, the dock taking the rest -- the Qt shell's arrangement, from the same data.
    const auto build = [&](LibguiUi* ui) {
        LibguiLayout column{};
        column.axis = 1;
        column.width = {LIBGUI_SIZE_GROW, 1};
        column.height = {LIBGUI_SIZE_GROW, 1};
        libgui_open_container(ui, libgui_id_from_name("shell.root"), column, LibguiFrame{});
        ribbon(ui);
        LibguiLayout rest{};
        rest.axis = 1;
        rest.width = {LIBGUI_SIZE_GROW, 1};
        rest.height = {LIBGUI_SIZE_GROW, 1};
        libgui_open_container(ui, libgui_id_from_name("shell.dock"), rest, LibguiFrame{});
        libgui_dock_show(dock_, ui, 0, &viewer);
        libgui_close_container(ui);
        libgui_close_container(ui);
        properties(ui);
    };
    if (!main_->frame(w, h, scale, dt, build)) {
        std::fprintf(stderr, "libgui refused the frame: %s\n", gui::Surface::lastError().c_str());
        return;
    }
    // Order within the frame: the viewport's size is known once the interface is built, the scene
    // target is resized to it, and only then is the interface submitted -- so the texture it samples
    // is this frame's, at this frame's size. The scene's views run before the interface's.
    uiPass_->submit(main_->output(), nullptr, pw, ph);
    controller_.presentFrame();
    syncTitle();
    if (pendingAction_) {
        auto action = std::move(pendingAction_);
        pendingAction_ = nullptr;
        action();
    }
}

void Shell::panel(LibguiUi* ui, std::uint64_t tab) {
    if (tab == kTabBrowser) browser(ui);
    else viewport(ui);
}

void Shell::browser(LibguiUi* ui) {
    libgui_heading(ui, "Part1");

    const auto items = controller_.tree();
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        std::string label = it.label;
        if (!it.error.empty()) label += "  (" + it.error + ")";
        const std::uint64_t depth = it.group == app::TreeGroup::Origin ? 1 : 0;
        const LibguiTreeResponse r =
            libgui_tree_row(ui, 0x7000'0000ull + std::uint64_t(it.id.value), depth, 0, label.c_str(), it.selected ? 1 : 0);
        if (r.response.clicked) {
            const bool additive = r.response.modifiers.shift || r.response.modifiers.ctrl || r.response.modifiers.logo;
            controller_.select(it.id, additive);
        }
    }
    if (!message_.empty()) {
        libgui_separator(ui);
        libgui_label_muted(ui, message_.c_str());
    }
}

void Shell::viewport(LibguiUi* ui) {
    const LibguiResponse r = libgui_viewport(ui, "scene", kSceneTexture);
    viewX_ = r.rect.x;
    viewY_ = r.rect.y;
    viewW_ = r.rect.w;
    viewH_ = r.rect.h;

    // The scene's target follows the widget, in physical pixels. The rect is a frame old, like every
    // response's, so a resize lags one frame -- invisible, and what libgui documents.
    const auto vw = std::uint32_t(std::max(1.0f, std::round(r.rect.w * scale_)));
    const auto vh = std::uint32_t(std::max(1.0f, std::round(r.rect.h * scale_)));
    if (r.rect.w > 0 && (vw != viewportWidth_ || vh != viewportHeight_)) {
        controller_.setViewportSize(vw, vh);
        viewportWidth_ = vw;
        viewportHeight_ = vh;
    }

    using Input = app::ViewportInput;
    input_.setScale(scale_);
    input_.setSize(r.rect.w, r.rect.h);
    Input::Pointer p{r.mouse_pos.x - r.rect.x, r.mouse_pos.y - r.rect.y, r.modifiers.shift != 0, r.modifiers.ctrl != 0,
                     r.modifiers.alt != 0};
    const auto take = [this](const Input::Outcome& out) {
        if (!out.message.empty()) message_ = out.message;
    };
    const auto heldNow = [this](Input::Button b) {
        return b == Input::Button::Primary ? held_.primary : b == Input::Button::Middle ? held_.middle : held_.secondary;
    };

    // libgui reports a press on the frame it happens and a click (or double click) on the frame of
    // release; the host reports which buttons are physically held. Together they make the press /
    // move / release stream every toolkit's viewport speaks, and app::ViewportInput takes it from
    // there.
    if (!gesture_ && (r.pressed || r.middle_pressed || r.secondary_pressed)) {
        gestureButton_ = r.pressed ? Input::Button::Primary : r.middle_pressed ? Input::Button::Middle : Input::Button::Secondary;
        gesture_ = true;
        doubleSeen_ = false;
        take(input_.press(gestureButton_, p));
    }
    const bool moved = p.x != lastX_ || p.y != lastY_;
    lastX_ = p.x;
    lastY_ = p.y;
    if (gesture_) {
        if (r.double_clicked) doubleSeen_ = true;
        if (moved) take(input_.move(p));
        if (!heldNow(gestureButton_)) {
            // libgui's order: the double click comes with the second release, so it goes first and
            // ViewportInput swallows the release after it.
            if (doubleSeen_) take(input_.doubleClick(gestureButton_, p));
            take(input_.release(gestureButton_, p));
            gesture_ = false;
        }
    } else if (r.hovered && moved) {
        take(input_.move(p));  // hover, or the sketch rubber band
    }

    if (r.scroll.y != 0 && r.hovered) take(input_.wheel(r.scroll.y));
    if (wasHovered_ && !r.hovered && !gesture_) take(input_.leave());
    wasHovered_ = r.hovered != 0;
}

bool Shell::commandEnabled(const std::string& id) const {
    // The shell's own actions; everything else is the model's answer, shared with every shell.
    if (id == "shell.orbit") return true;
    if (id == "shell.import") return host_.chooseFile != nullptr;
    if (id.rfind("shell.", 0) == 0) return false;  // Parameters: a dialog this shell lacks yet
    return app::ribbonItemState(controller_, id).enabled;
}

namespace {

// Inventor's two button shapes, in logical px.
constexpr float kLargeIcon = 32, kSmallIcon = 16;
constexpr float kLargeHeight = 66, kSmallHeight = 22;
constexpr float kFontSize = 12, kLineHeight = 14;
// Three small buttons stacked fill exactly one large button's height.
static_assert(kSmallHeight * 3 == kLargeHeight);

constexpr LibguiColor rgb(std::uint32_t hex, float a = 1) {
    return {float((hex >> 16) & 0xff) / 255.f, float((hex >> 8) & 0xff) / 255.f, float(hex & 0xff) / 255.f, a};
}

std::vector<std::string> lines(const std::string& label) {
    std::vector<std::string> out;
    for (std::size_t at = 0;;) {
        const std::size_t nl = label.find('\n', at);
        out.push_back(label.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
        if (nl == std::string::npos) return out;
        at = nl + 1;
    }
}

/// What a ribbon button's paint callback needs, alive until libgui drops it at the end of the frame.
struct ButtonPaint {
    Shell* shell = nullptr;
    std::vector<std::string> lines;
    const IconUv* icon = nullptr;
    bool large = false, enabled = false, hovered = false, pressed = false, menu = false, checked = false;
};

void paintButton(LibguiPainter* p, LibguiRect r, void* user) {
    const auto& b = *static_cast<ButtonPaint*>(user);
    // A lit mode (the active sketch tool, Slice) keeps the pressed look, as Inventor's does.
    if (b.checked || (b.enabled && (b.hovered || b.pressed)))
        libgui_painter_rect_bordered(p, r, rgb(b.pressed || b.checked ? 0xcce4f7 : 0xe5f1fb), 2, 1,
                                     rgb(b.pressed || b.checked ? 0x99c9ef : 0xcce4f7));
    const LibguiColor ink = b.enabled ? rgb(0x1e1e1e) : rgb(0xa0a0a0);
    const LibguiColor tint{1, 1, 1, b.enabled ? 1.0f : 0.35f};

    for (const auto& line : b.lines) {  // measured for the next frame's layout
        float w = 0, h = 0;
        libgui_painter_measure(p, kFontSize, line.c_str(), &w, &h);
        b.shell->textWidth_[line] = w;
    }
    if (b.large) {
        const LibguiRect icon{r.x + (r.w - kLargeIcon) / 2, r.y + 3, kLargeIcon, kLargeIcon};
        if (b.icon) libgui_painter_image_alpha(p, icon, kIconTexture, b.icon->u0, b.icon->v0, b.icon->u1, b.icon->v1, 0, tint,
                                              LIBGUI_IMAGE_PREMULTIPLIED);
        float y = r.y + 3 + kLargeIcon + 2;
        for (const auto& line : b.lines) {
            libgui_painter_text_centered(p, LibguiRect{r.x, y, r.w, kLineHeight}, kFontSize, ink, line.c_str());
            y += kLineHeight;
        }
        if (b.menu) libgui_painter_chevron(p, LibguiRect{r.x + r.w / 2 - 4, y, 8, 6}, 6, 1, ink);
        return;
    }
    const LibguiRect icon{r.x + 3, r.y + (r.h - kSmallIcon) / 2, kSmallIcon, kSmallIcon};
    if (b.icon) libgui_painter_image_alpha(p, icon, kIconTexture, b.icon->u0, b.icon->v0, b.icon->u1, b.icon->v1, 0, tint,
                                              LIBGUI_IMAGE_PREMULTIPLIED);
    const float textX = r.x + 3 + kSmallIcon + 5;
    libgui_painter_text_left(p, LibguiRect{textX, r.y, r.w - (textX - r.x), r.h}, kFontSize, ink,
                             b.lines.empty() ? "" : b.lines.front().c_str());
    if (b.menu) libgui_painter_chevron(p, LibguiRect{r.x + r.w - 11, r.y + (r.h - 6) / 2, 8, 6}, 6, 1, ink);
}

/// A thin vertical rule between ribbon sections.
void paintRule(LibguiPainter* p, LibguiRect r, void*) {
    LibguiRect line{};
    libgui_painter_hairline(p, r.x + r.w / 2, r.y + 4, 1, r.h - 8, &line);
    libgui_painter_rect(p, line, rgb(0xd4d4d4), 0);
}

LibguiLayout fixed(float w, float h) {
    LibguiLayout l{};
    l.width = {LIBGUI_SIZE_FIXED, w};
    l.height = {LIBGUI_SIZE_FIXED, h};
    return l;
}

LibguiLayout fit(std::uint8_t axis, float gap) {
    LibguiLayout l{};
    l.axis = axis;
    l.width = {LIBGUI_SIZE_FIT, 0};
    l.height = {LIBGUI_SIZE_FIT, 0};
    l.gap = gap;
    return l;
}

}  // namespace

void Shell::ribbon(LibguiUi* ui) {
    // Whichever environment the model is in: the sketch collapses the ribbon to one tab.
    const auto& tabs = app::ribbonFor(controller_);
    if (ribbonTab_ >= tabs.size()) ribbonTab_ = 0;

    std::vector<const char*> names;
    names.reserve(tabs.size());
    for (const auto& t : tabs) names.push_back(t.label.c_str());
    LibguiLayout strip = fit(0, 0);
    strip.width = {LIBGUI_SIZE_GROW, 1};
    strip.align_cross = 1;
    libgui_open_container(ui, libgui_id_from_name("ribbon.strip"), strip, LibguiFrame{});
    fileMenu(ui);
    LibguiLayout tabsBox = fit(0, 0);
    tabsBox.width = {LIBGUI_SIZE_GROW, 1};
    libgui_open_container(ui, libgui_id_from_name("ribbon.tabs.box"), tabsBox, LibguiFrame{});
    libgui_segmented(ui, "ribbon.tabs", &ribbonTab_, names.data(), names.size());
    libgui_close_container(ui);
    libgui_close_container(ui);

    LibguiLayout row = fit(0, 0);
    row.width = {LIBGUI_SIZE_GROW, 1};
    row.pad_left = 4;
    row.pad_top = 3;
    row.pad_bottom = 2;
    libgui_open_container(ui, libgui_id_from_name("ribbon.row"), row, LibguiFrame{});
    const auto& sections = tabs[ribbonTab_].sections;
    for (std::size_t i = 0; i < sections.size(); ++i) {
        if (i > 0)
            libgui_add_leaf(ui, libgui_id_from_name((sections[i].id + ".rule").c_str()), fixed(9, kLargeHeight + 18), 0,
                            LibguiPaintFn{paintRule, nullptr, nullptr});
        ribbonSection(ui, sections[i]);
    }
    libgui_close_container(ui);
    libgui_separator(ui);
}

void Shell::ribbonSection(LibguiUi* ui, const app::RibbonSection& section) {
    LibguiLayout col = fit(1, 1);
    col.align_cross = 1;  // the caption centres under the buttons, as Inventor's does
    libgui_open_container(ui, libgui_id_from_name(section.id.c_str()), col, LibguiFrame{});

    // Inventor's columns: a large button is a column of its own; small ones stack three to a
    // column, in the order the data lists them -- which is why RibbonLayout.cpp reads down columns.
    LibguiLayout buttons = fit(0, 2);
    buttons.height = {LIBGUI_SIZE_FIXED, kLargeHeight};
    libgui_open_container(ui, libgui_id_from_name((section.id + ".buttons").c_str()), buttons, LibguiFrame{});
    const auto& items = section.items;
    for (std::size_t i = 0; i < items.size();) {
        if (items[i].size == app::ItemSize::Large) {
            ribbonItem(ui, items[i++]);
            continue;
        }
        libgui_open_container(ui, libgui_id_from_name((section.id + ".col" + std::to_string(i)).c_str()), fit(1, 0),
                              LibguiFrame{});
        for (int n = 0; n < 3 && i < items.size() && items[i].size == app::ItemSize::Small; ++n) ribbonItem(ui, items[i++]);
        libgui_close_container(ui);
    }
    libgui_close_container(ui);
    libgui_label_muted(ui, section.label.c_str());
    libgui_close_container(ui);
}

void Shell::ribbonItem(LibguiUi* ui, const app::RibbonItem& item) {
    auto* paint = new ButtonPaint;
    paint->shell = this;
    paint->lines = lines(item.label);
    paint->icon = icons_.find(item.iconName);
    paint->large = item.size == app::ItemSize::Large;
    paint->enabled = commandEnabled(item.id);
    paint->checked = item.checkable && app::ribbonItemState(controller_, item.id).checked;
    paint->menu = !item.menu.empty();

    float textW = 0;
    for (const auto& line : paint->lines) {
        const auto it = textWidth_.find(line);
        textW = std::max(textW, it != textWidth_.end() ? it->second : float(line.size()) * 6.6f);
    }
    const float w = paint->large ? std::max(kLargeIcon + 10, std::ceil(textW) + 10)
                                 : 3 + kSmallIcon + 5 + std::ceil(textW) + (paint->menu ? 16 : 6);
    const float h = paint->large ? kLargeHeight : kSmallHeight;

    // Keyed on the command, so the same button on two tabs keeps one identity.
    const std::uint64_t key =
        libgui_id_from_name(((item.id.empty() ? "planned." + item.label : item.id) + "#btn").c_str());
    libgui_add_leaf(ui, key, fixed(w, h), 1, LibguiPaintFn{paintButton, [](void* u) { delete static_cast<ButtonPaint*>(u); }, paint});
    const LibguiResponse r = libgui_interact(ui, key);
    paint->hovered = r.hovered != 0;
    paint->pressed = r.active != 0;

    std::string tip = item.label;
    for (char& c : tip) if (c == '\n') c = ' ';
    if (!item.shortcut.empty()) tip += " (" + item.shortcut + ")";
    if (item.id.empty()) tip += " -- not implemented yet";
    libgui_tooltip(ui, key, tip.c_str());

    const bool clicked = paint->enabled && r.clicked;
    if (item.menu.empty()) {
        if (!clicked) return;
        if (item.id == "shell.orbit") controller_.setOrbitMode(!controller_.orbitMode());
        else if (item.id == "shell.import") pendingAction_ = [this] { importFile(); };
        else if (item.opensPanel) beginPanel(item.id);
        else if (!item.valueLabel.empty()) {
            // Asked in the Properties panel, then run with the answer.
            if (!controller_.activeCommand().empty()) controller_.cancelCommand();
            askId_ = item.id;
            askTitle_ = tip;
            askLabel_ = item.valueLabel;
            char buf[64];
            std::snprintf(buf, sizeof buf, "%g mm", item.valueDefault);
            askText_ = buf;
            panelError_.clear();
        } else if (!app::runRibbonItem(controller_, item.id)) {
            runCommand(item.id);  // a command the ribbon data does not know, e.g. a plugin's
        }
        return;
    }
    // Combine: one slot, three modes on a drop-down.
    const std::uint64_t popup = key ^ 0x9e3779b97f4a7c15ull;
    if (clicked) libgui_open_popup(ui, popup, r.rect);
    if (libgui_open_popup_body(ui, popup, 120)) {
        for (const auto& id : item.menu) {
            std::string text = id;
            for (const auto& c : controller_.commands())
                if (c.id == id) text = c.label;
            const std::uint8_t on = libgui_open_enabled(ui, commandEnabled(id) ? 1 : 0);
            if (libgui_menu_item(ui, text.c_str()).clicked) {
                runCommand(id);
                libgui_close_popup(ui, popup);
            }
            libgui_close_enabled(ui, on);
        }
        libgui_close_popup_body(ui);
    }
}

namespace {

struct FilePaint {
    bool hovered = false, open = false;
};

/// Inventor's File tab: the one coloured tab on the strip, opening the application menu.
void paintFileTab(LibguiPainter* p, LibguiRect r, void* user) {
    const auto& f = *static_cast<FilePaint*>(user);
    libgui_painter_rect(p, r, rgb(f.open ? 0xd9831a : f.hovered ? 0xf5a943 : 0xf29a2e), 0);
    libgui_painter_text_centered(p, r, kFontSize, rgb(0xffffff), "File");
}

/// The filters a dialog shows for a list of formats: one "all of these" entry first, then each.
std::vector<FileFilter> filtersFor(const std::vector<app::Controller::ExportFormat>& formats, const std::string& all) {
    std::vector<FileFilter> out;
    std::string every;
    for (const auto& f : formats) {
        std::string ext;
        for (const auto& e : f.extensions) {
            const std::string bare = !e.empty() && e.front() == '.' ? e.substr(1) : e;
            ext += (ext.empty() ? "" : ";") + bare;
        }
        if (ext.empty()) continue;
        every += (every.empty() ? "" : ";") + ext;
        out.push_back(FileFilter{f.solids ? f.displayName : f.displayName + " (mesh only, no solid geometry)", ext});
    }
    if (out.size() > 1) out.insert(out.begin(), FileFilter{all, every});
    return out;
}

/// The native part document. Only parts: this shell has one Controller and no Session yet.
const std::vector<FileFilter>& partFilters() {
    static const std::vector<FileFilter> filters{{"vCAD part", "vpart"}};
    return filters;
}

std::string fileName(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

void Shell::setHost(HostServices host) {
    host_ = std::move(host);
    shownTitle_.clear();
    syncTitle();
}

void Shell::alert(const std::string& title, const std::string& message) {
    if (host_.ask) host_.ask(title, message, {"OK"});
    else std::fprintf(stderr, "%s: %s\n", title.c_str(), message.c_str());
}

void Shell::syncTitle() {
    // The asterisk is the platform-neutral dirty marker, as shell_qt uses.
    std::string title = "vCAD - " + (path_.empty() ? std::string("Part1") : fileName(path_));
    if (controller_.modified()) title += "*";
    if (title == shownTitle_ || !host_.setTitle) return;
    shownTitle_ = title;
    host_.setTitle(title);
}

void Shell::confirmDiscard(std::function<void()> then) {
    if (!controller_.modified() || !host_.ask) {
        then();
        return;
    }
    const std::string name = path_.empty() ? "Part1" : fileName(path_);
    switch (host_.ask("Unsaved changes", "Save changes to " + name + "?", {"Save", "Don't Save", "Cancel"})) {
        // Save may itself be cancelled at the file dialog, and that must cancel this too --
        // otherwise "Save" quietly behaves as "Don't Save".
        case 0: saveDocument(false, std::move(then)); break;
        case 1: then(); break;
        default: break;
    }
}

void Shell::requestClose(std::function<void()> close) { confirmDiscard(std::move(close)); }

void Shell::openDocument() {
    if (!host_.chooseFile) return;
    confirmDiscard([this] {
        host_.chooseFile(false, "Open", partFilters(), "", [this](const std::string& path) {
            if (path.empty()) return;
            if (auto r = open(path); !r) {
                alert("Could not open", r.error().message);
                return;
            }
            message_ = "Opened " + fileName(path);
        });
    });
}

void Shell::saveDocument(bool saveAs, std::function<void()> then) {
    const auto write = [this, then](const std::string& path) {
        if (auto r = controller_.saveTo(path); !r) {
            alert("Could not save", r.error().message);
            return;
        }
        path_ = path;
        message_ = "Saved " + fileName(path);
        if (then) then();
    };
    if (!saveAs && !path_.empty()) {
        write(path_);
        return;
    }
    if (!host_.chooseFile) return;
    const std::string suggested = path_.empty() ? "Part1.vpart" : fileName(path_);
    host_.chooseFile(true, "Save As", partFilters(), suggested, [write](const std::string& path) {
        if (path.empty()) return;  // cancelled: nothing saved, and `then` does not run
        write(path);
    });
}

void Shell::importFile() {
    if (!host_.chooseFile) return;
    const auto filters = filtersFor(app::Controller::importFormats(), "CAD files");
    host_.chooseFile(false, "Import", filters, "", [this](const std::string& path) {
        if (path.empty()) return;
        if (auto r = controller_.importFile(path); !r) {
            alert("Import failed", r.error().message);
            return;
        }
        controller_.fitView();
        message_ = "Imported " + fileName(path);
    });
}

void Shell::exportFile() {
    if (!host_.chooseFile) return;
    const auto filters = filtersFor(app::Controller::exportFormats(), "All export formats");
    if (filters.empty()) {
        alert("Export", "No export formats are available in this build.");
        return;
    }
    std::string stem = path_.empty() ? "Part1" : fileName(path_);
    if (const auto dot = stem.rfind('.'); dot != std::string::npos) stem.resize(dot);
    host_.chooseFile(true, "Export", filters, stem, [this](const std::string& path) {
        if (path.empty()) return;
        // The Controller's refusal reaches message_ through its status callback; shown as it is,
        // so there is one wording for why an export failed.
        if (!controller_.exportDocument(path)) alert("Could not export", message_);
        else message_ = "Exported " + fileName(path);
    });
}

void Shell::sketchOn(const std::string& plane) {
    const auto which = plane == "XZ" ? sketch::Plane::XZ : plane == "YZ" ? sketch::Plane::YZ : sketch::Plane::XY;
    controller_.beginSketchOn(which);
}

void Shell::fileAction(const std::string& name) {
    if (name == "open") pendingAction_ = [this] { openDocument(); };
    else if (name == "save") pendingAction_ = [this] { saveDocument(false); };
    else if (name == "saveas") pendingAction_ = [this] { saveDocument(true); };
    else if (name == "import") pendingAction_ = [this] { importFile(); };
    else if (name == "export") pendingAction_ = [this] { exportFile(); };
}

void Shell::fileMenu(LibguiUi* ui) {
    const std::uint64_t key = libgui_id_from_name("file.tab");
    const std::uint64_t popup = libgui_id_from_name("file.menu");
    auto* paint = new FilePaint;
    LibguiLayout l{};
    l.width = {LIBGUI_SIZE_FIXED, 52};
    l.height = {LIBGUI_SIZE_FIXED, 24};
    libgui_add_leaf(ui, key, l, 1, LibguiPaintFn{paintFileTab, [](void* u) { delete static_cast<FilePaint*>(u); }, paint});
    const LibguiResponse r = libgui_interact(ui, key);
    paint->hovered = r.hovered != 0;
    paint->open = libgui_popup_open(ui, popup) != 0;
    if (r.clicked) libgui_open_popup(ui, popup, r.rect);
    if (!libgui_open_popup_body(ui, popup, 180)) return;
    // Each action closes the menu BEFORE it runs: a native dialog is modal, and a menu left open
    // under it would still be open, and still eating clicks, when the dialog returns.
    std::function<void()> chosen;
    if (libgui_menu_item(ui, "Open...").clicked) chosen = [this] { openDocument(); };
    libgui_menu_separator(ui);
    if (libgui_menu_item(ui, "Save").clicked) chosen = [this] { saveDocument(false); };
    if (libgui_menu_item(ui, "Save As...").clicked) chosen = [this] { saveDocument(true); };
    libgui_menu_separator(ui);
    if (libgui_menu_item(ui, "Import...").clicked) chosen = [this] { importFile(); };
    if (libgui_menu_item(ui, "Export...").clicked) chosen = [this] { exportFile(); };
    libgui_close_popup_body(ui);
    if (chosen) {
        libgui_close_popup(ui, popup);
        pendingAction_ = std::move(chosen);
    }
}

void Shell::applyTheme() {
    std::string toml = cad_libgui_theme;
    if (const char* path = std::getenv("VCAD_THEME"); path != nullptr && *path != '\0') {
        std::ifstream in(path);
        if (in) {
            std::ostringstream text;
            text << in.rdbuf();
            toml = text.str();
        } else {
            std::fprintf(stderr, "VCAD_THEME: cannot read %s; using the built-in theme\n", path);
        }
    }
    // A rejected theme leaves libgui's default in place and says which key, rather than half-applying.
    if (libgui_set_theme_toml(main_->raw(), toml.c_str()) != 0)
        std::fprintf(stderr, "theme rejected: %s\n", gui::Surface::lastError().c_str());
}

void Shell::beginPanel(const std::string& id) {
    if (!controller_.activeCommand().empty()) controller_.cancelCommand();
    // A command with no parameters runs directly, as the app documents for beginCommand.
    // pickAfter: the panel opens with nothing selected and asks for it, as Inventor's does.
    if (!controller_.beginCommand(id, /*pickAfter=*/true)) runCommand(id);
}

void Shell::askValue(LibguiUi* ui) {
    LibguiFrame frame{};
    frame.fill = {0.961f, 0.961f, 0.961f, 1};
    frame.border = {0.749f, 0.749f, 0.749f, 1};
    frame.border_width = 1;
    frame.shadow = 1;
    frame.clip = 1;
    const LibguiRect rect{viewX_ + 8, viewY_ + 8, 270, 136 + (panelError_.empty() ? 0.0f : 22.0f)};
    libgui_open_layer(ui, libgui_id_from_name("ask.layer"), LIBGUI_LAYER_WINDOW, rect, frame);
    LibguiLayout body{};
    body.axis = 1;
    body.width = {LIBGUI_SIZE_GROW, 1};
    body.height = {LIBGUI_SIZE_GROW, 1};
    body.pad_left = body.pad_right = 8;
    body.pad_top = body.pad_bottom = 6;
    body.gap = 4;
    libgui_open_container(ui, libgui_id_from_name("ask.body"), body, LibguiFrame{});
    libgui_heading(ui, "Properties");
    libgui_section(ui, askTitle_.c_str());
    char buf[128];
    std::snprintf(buf, sizeof buf, "%s", askText_.c_str());
    std::uint64_t len = 0;
    libgui_label(ui, askLabel_.c_str());
    const LibguiTextResponse t = libgui_text_input(ui, "ask.value", buf, sizeof buf, "", &len);
    if (t.changed) askText_.assign(buf, std::min<std::uint64_t>(len, sizeof buf - 1));
    if (!panelError_.empty()) libgui_label_muted(ui, panelError_.c_str());
    LibguiLayout row{};
    row.axis = 0;
    row.width = {LIBGUI_SIZE_GROW, 1};
    row.height = {LIBGUI_SIZE_FIT, 0};
    row.gap = 6;
    libgui_open_container(ui, libgui_id_from_name("ask.buttons"), row, LibguiFrame{});
    const bool ok = libgui_button_primary(ui, "OK").clicked || t.submitted;
    const bool cancel = libgui_button(ui, "Cancel").clicked || t.cancelled;
    libgui_close_container(ui);
    libgui_close_container(ui);
    libgui_close_layer(ui);

    if (cancel) {
        askId_.clear();
        return;
    }
    if (!ok) return;
    // The unit grammar is the app's: "5", "5 mm" and "0.2 in" all mean what they say.
    const auto parsed = units::parseLength(askText_, controller_.preferences().displayUnits);
    if (!parsed) {
        panelError_ = "Cannot read \"" + askText_ + "\" as a length";
        return;
    }
    if (app::runRibbonItem(controller_, askId_, parsed.value().base())) askId_.clear();
    else panelError_ = message_.empty() ? "That did not apply to the selection." : message_;
}

void Shell::properties(LibguiUi* ui) {
    if (!askId_.empty()) {
        askValue(ui);
        return;
    }
    const std::string& active = controller_.activeCommand();
    if (active.empty()) {
        panelCommand_.clear();
        return;
    }
    if (panelCommand_ != active) {  // seed the fields from the app's current values
        panelCommand_ = active;
        panelError_.clear();
        fields_.clear();
        for (const auto& p : controller_.commandParameters()) fields_.emplace_back(p.name, p.value);
    }
    std::string title = active;
    for (const auto& c : controller_.commands())
        if (c.id == active) title = c.label;

    // Inventor floats Properties over the graphics window's top-left corner; so does this.
    constexpr float kWidth = 270;
    const float rows = float(controller_.commandParameters().size());
    const app::CommandSelection picking = controller_.commandSelection();
    const float pickRows = picking.needed ? (picking.satisfied ? 28.0f : 50.0f) : 0.0f;
    const LibguiRect rect{viewX_ + 8, viewY_ + 8, kWidth, 80 + rows * 28 + pickRows + (panelError_.empty() ? 0 : 22)};
    LibguiFrame frame{};
    frame.fill = {0.961f, 0.961f, 0.961f, 1};
    frame.border = {0.749f, 0.749f, 0.749f, 1};
    frame.border_width = 1;
    frame.shadow = 1;
    frame.clip = 1;
    libgui_open_layer(ui, libgui_id_from_name("properties.layer"), LIBGUI_LAYER_WINDOW, rect, frame);

    LibguiLayout body{};
    body.axis = 1;
    body.width = {LIBGUI_SIZE_GROW, 1};
    body.height = {LIBGUI_SIZE_GROW, 1};
    body.pad_left = body.pad_right = 8;
    body.pad_top = body.pad_bottom = 6;
    body.gap = 4;
    libgui_open_container(ui, libgui_id_from_name("properties.body"), body, LibguiFrame{});
    libgui_heading(ui, "Properties");
    libgui_section(ui, title.c_str());

    // The pick field, Inventor's "Profiles: 1 Profile". Picking happens in the viewport, which stays
    // live while the panel is open; this shows what OK will act on, and turns red until it fits.
    const app::CommandSelection pick = controller_.commandSelection();
    if (pick.needed) {
        LibguiLayout row{};
        row.axis = 0;
        row.width = {LIBGUI_SIZE_GROW, 1};
        row.height = {LIBGUI_SIZE_FIT, 0};
        row.gap = 6;
        row.align_cross = 1;
        libgui_open_container(ui, libgui_id_from_name("properties.pick.row"), row, LibguiFrame{});
        LibguiLayout caption{};
        caption.width = {LIBGUI_SIZE_FIXED, 90};
        caption.height = {LIBGUI_SIZE_FIT, 0};
        libgui_open_container(ui, libgui_id_from_name("properties.pick.cap"), caption, LibguiFrame{});
        libgui_label(ui, pick.label.c_str());
        libgui_close_container(ui);
        struct PickPaint {
            std::string text;
            bool ok;
        };
        LibguiLayout field{};
        field.width = {LIBGUI_SIZE_GROW, 1};
        field.height = {LIBGUI_SIZE_FIXED, 22};
        libgui_add_leaf(ui, libgui_id_from_name("properties.pick"), field, 0,
                        LibguiPaintFn{[](LibguiPainter* p, LibguiRect r, void* u) {
                                          const auto& f = *static_cast<PickPaint*>(u);
                                          libgui_painter_rect_bordered(p, r, rgb(f.ok ? 0xffffff : 0xfdecea), 0, 1,
                                                                       rgb(f.ok ? 0xc0c0c0 : 0xd9453a));
                                          libgui_painter_text_left(p, LibguiRect{r.x + 6, r.y, r.w - 12, r.h},
                                                                   kFontSize, rgb(0x1e1e1e), f.text.c_str());
                                      },
                                      [](void* u) { delete static_cast<PickPaint*>(u); },
                                      new PickPaint{pick.picked, pick.satisfied}});
        libgui_close_container(ui);
        if (!pick.satisfied) libgui_label_muted(ui, pick.prompt.c_str());
    }

    bool submit = false;
    const auto& params = controller_.commandParameters();
    for (std::size_t i = 0; i < fields_.size() && i < params.size(); ++i) {
        LibguiLayout row{};
        row.axis = 0;
        row.width = {LIBGUI_SIZE_GROW, 1};
        row.height = {LIBGUI_SIZE_FIT, 0};
        row.gap = 6;
        row.align_cross = 1;
        libgui_open_container(ui, libgui_id_from_name(("properties.row." + fields_[i].first).c_str()), row, LibguiFrame{});
        LibguiLayout caption{};
        caption.axis = 0;
        caption.width = {LIBGUI_SIZE_FIXED, 90};
        caption.height = {LIBGUI_SIZE_FIT, 0};
        libgui_open_container(ui, libgui_id_from_name(("properties.cap." + fields_[i].first).c_str()), caption, LibguiFrame{});
        libgui_label(ui, params[i].label.c_str());
        libgui_close_container(ui);

        auto& text = fields_[i].second;
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s", text.c_str());
        std::uint64_t len = 0;
        const LibguiTextResponse r =
            libgui_text_input(ui, ("properties." + fields_[i].first).c_str(), buf, sizeof buf, "", &len);
        if (r.changed) text.assign(buf, std::min<std::uint64_t>(len, sizeof buf - 1));
        if (r.submitted) submit = true;
        libgui_close_container(ui);
    }
    if (!panelError_.empty()) libgui_label_muted(ui, panelError_.c_str());

    LibguiLayout buttons{};
    buttons.axis = 0;
    buttons.width = {LIBGUI_SIZE_GROW, 1};
    buttons.height = {LIBGUI_SIZE_FIT, 0};
    buttons.gap = 6;
    libgui_open_container(ui, libgui_id_from_name("properties.buttons"), buttons, LibguiFrame{});
    if (libgui_button_primary(ui, "OK").clicked) submit = true;
    const bool cancel = libgui_button(ui, "Cancel").clicked;
    libgui_close_container(ui);
    libgui_close_container(ui);
    libgui_close_layer(ui);

    if (cancel) {
        controller_.cancelCommand();
        return;
    }
    if (!submit) return;
    // Every field goes back as text; the app parses units. One that will not parse keeps the panel
    // open and names the field, rather than committing half the user's values.
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        if (!controller_.setCommandParameter(fields_[i].first, fields_[i].second)) {
            panelError_ = "Cannot read " + params[i].label + ": \"" + fields_[i].second + "\"";
            return;
        }
    }
    if (!controller_.commitCommand()) panelError_ = message_.empty() ? "The command could not run." : message_;
}

}  // namespace cad::shell_libgui
