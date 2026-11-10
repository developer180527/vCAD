// The host for vCAD's libgui shell: windows, events and a frame clock, through SDL3.
//
// libgui never creates windows or reads the OS; the application does (libgui's MANUAL, section 4).
// This file is that half. It hands Shell a native view and events in the window's logical pixels,
// top-left origin, and knows nothing of libgui or bgfx. SDL3 rather than a hand-written host per
// platform: windows, DPI, input, IME, clipboard and native file dialogs, from one C API, and it runs
// vCAD's loop instead of owning it.

#include "Shell.h"
#include "cad/render/Renderer.h"

#include <SDL3/SDL.h>

#if defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <mutex>
#include <functional>

extern "C" const unsigned char cad_libgui_font[];
extern "C" const unsigned long cad_libgui_font_size;

using cad::shell_libgui::Shell;
namespace gui = cad::shell_libgui::gui;

namespace {

gui::Key keyFor(SDL_Keycode k, char32_t& ch) {
    ch = 0;
    switch (k) {
        case SDLK_LEFT: return gui::Key::Left;
        case SDLK_RIGHT: return gui::Key::Right;
        case SDLK_UP: return gui::Key::Up;
        case SDLK_DOWN: return gui::Key::Down;
        case SDLK_HOME: return gui::Key::Home;
        case SDLK_END: return gui::Key::End;
        case SDLK_PAGEUP: return gui::Key::PageUp;
        case SDLK_PAGEDOWN: return gui::Key::PageDown;
        case SDLK_BACKSPACE: return gui::Key::Backspace;
        case SDLK_DELETE: return gui::Key::Delete;
        case SDLK_RETURN: case SDLK_KP_ENTER: return gui::Key::Enter;
        case SDLK_TAB: return gui::Key::Tab;
        case SDLK_ESCAPE: return gui::Key::Escape;
        case SDLK_SPACE: return gui::Key::Space;
        default: break;
    }
    if (k >= SDLK_F1 && k <= SDLK_F12) {
        ch = char32_t(k - SDLK_F1 + 1);
        return gui::Key::F;
    }
    // SDL keycodes are layout-aware: the character the key produces, so Cmd+S is S on any layout.
    if (k >= SDLK_A && k <= SDLK_Z) {
        ch = char32_t(k);
        return gui::Key::Letter;
    }
    if (k >= SDLK_0 && k <= SDLK_9) {
        ch = char32_t(k);
        return gui::Key::Digit;
    }
    return gui::Key::None;
}

gui::Modifiers modifiersOf(SDL_Keymod m) {
    return {(m & SDL_KMOD_SHIFT) != 0, (m & SDL_KMOD_CTRL) != 0, (m & SDL_KMOD_ALT) != 0, (m & SDL_KMOD_GUI) != 0};
}

struct Host {
    SDL_Window* window = nullptr;
    SDL_MetalView metalView = nullptr;
    std::unique_ptr<Shell> shell;
    Uint64 last = 0;
    long frames = 0;
    long maxFrames = 0;  // VCAD_FRAMES: quit after this many, for a scripted check; 0 = run
    bool running = true;

    // File dialog results. SDL may call a dialog's callback on another thread, and the shell is
    // single-threaded, so the result is queued here and run by the main loop.
    std::mutex mainMutex;
    std::vector<std::function<void()>> onMain;

    void post(std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(mainMutex);
        onMain.push_back(std::move(fn));
    }
    void runPosted() {
        std::vector<std::function<void()>> ready;
        {
            std::lock_guard<std::mutex> lock(mainMutex);
            ready.swap(onMain);
        }
        for (auto& fn : ready) fn();
    }

    cad::shell_libgui::HostServices services();

    void tick() {
        const Uint64 now = SDL_GetTicksNS();
        const float dt = last ? float(double(now - last) / 1e9) : 1.0f / 60.0f;
        last = now;

        int w = 0, h = 0, x = 0, y = 0;
        SDL_GetWindowSize(window, &w, &h);      // logical points
        SDL_GetWindowPosition(window, &x, &y);  // content area, desktop points, top-left origin
        const float scale = SDL_GetWindowPixelDensity(window);
        // Docking works in DESKTOP physical pixels, and follows a drag outside every window.
        float mx = 0, my = 0;
        const SDL_MouseButtonFlags held = SDL_GetGlobalMouseState(&mx, &my);
        const cad::shell_libgui::Buttons buttons{(held & SDL_BUTTON_LMASK) != 0, (held & SDL_BUTTON_RMASK) != 0,
                                                 (held & SDL_BUTTON_MMASK) != 0};
        shell->tick(float(w), float(h), scale, dt, float(x) * scale, float(y) * scale, buttons, mx * scale,
                    my * scale);

        if (maxFrames > 0 && ++frames >= maxFrames) {
            std::printf("frames %ld  %dx%d @%.1fx\n", frames, w, h, static_cast<double>(scale));
            std::fflush(stdout);
            running = false;
        }
    }

    void handle(const SDL_Event& e) {
        gui::Surface& s = shell->surface();
        switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                // Through the shell, which asks about unsaved changes and may be told Cancel.
                shell->requestClose([this] { running = false; });
                break;
            case SDL_EVENT_MOUSE_MOTION: s.pointerMoved(e.motion.x, e.motion.y); break;
            case SDL_EVENT_WINDOW_MOUSE_LEAVE: s.pointerLeft(); break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                s.pointerMoved(e.button.x, e.button.y);
                s.modifiers(modifiersOf(SDL_GetModState()));
                const gui::Button b = e.button.button == SDL_BUTTON_RIGHT    ? gui::Button::Secondary
                                      : e.button.button == SDL_BUTTON_MIDDLE ? gui::Button::Middle
                                                                             : gui::Button::Primary;
                s.button(b, e.button.down);
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL: {
                // SDL reports wheel "ticks", fractional on a trackpad. A whole number is a notched
                // wheel; anything else is a continuous device. SDL does not say which, so this is
                // the one heuristic in the host -- and the one to revisit if scrolling feels wrong.
                float dx = e.wheel.x, dy = e.wheel.y;
                if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) dx = -dx, dy = -dy;
                const bool precise = dx != std::round(dx) || dy != std::round(dy);
                s.wheel(dx, dy, precise);
                break;
            }
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                s.modifiers(modifiersOf(e.key.mod));
                char32_t ch = 0;
                const gui::Key k = keyFor(e.key.key, ch);
                s.key(k, ch, e.key.down, e.key.repeat);
                break;
            }
            case SDL_EVENT_TEXT_INPUT: s.text(e.text.text); break;
            case SDL_EVENT_TEXT_EDITING:  // IME composition, before it commits
                s.preedit(e.edit.text ? e.edit.text : "", std::uint64_t(std::max(0, e.edit.start)));
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST: s.focusLost(); break;
            default: break;
        }
    }
};

// macOS stops delivering events to SDL_PollEvent while a window edge is being dragged, so a loop
// that draws only after polling freezes during a live resize. SDL still calls event watches from
// inside that modal loop; drawing from the resize there keeps the window live.
bool SDLCALL onEvent(void* user, SDL_Event* e) {
    auto* host = static_cast<Host*>(user);
    if (e->type == SDL_EVENT_WINDOW_EXPOSED && host->shell) host->tick();
    return true;
}

}  // namespace

namespace {

/// What one SDL file dialog needs to outlive the call that opened it.
struct DialogRequest {
    Host* host = nullptr;
    std::function<void(const std::string&)> done;
    std::vector<std::string> names, patterns;  // storage for the SDL_DialogFileFilter pointers
    std::vector<SDL_DialogFileFilter> filters;
};

void SDLCALL dialogDone(void* user, const char* const* files, int) {
    std::unique_ptr<DialogRequest> request(static_cast<DialogRequest*>(user));
    // Copied now: SDL owns `files` only for the duration of this call. NULL is an error and an
    // empty list a cancel; the shell treats both as "nothing chosen".
    std::string path = (files != nullptr && files[0] != nullptr) ? files[0] : "";
    if (files == nullptr) std::fprintf(stderr, "file dialog: %s\n", SDL_GetError());
    Host* host = request->host;
    host->post([done = std::move(request->done), path] { done(path); });
}

}  // namespace

cad::shell_libgui::HostServices Host::services() {
    cad::shell_libgui::HostServices s;
    s.chooseFile = [this](bool save, const std::string& title, const std::vector<cad::shell_libgui::FileFilter>& filters,
                          const std::string& suggested, std::function<void(const std::string&)> done) {
        auto request = std::make_unique<DialogRequest>();
        request->host = this;
        request->done = std::move(done);
        request->names.reserve(filters.size());
        request->patterns.reserve(filters.size());
        for (const auto& f : filters) {
            request->names.push_back(f.name);
            request->patterns.push_back(f.extensions);
        }
        for (std::size_t i = 0; i < filters.size(); ++i)
            request->filters.push_back(SDL_DialogFileFilter{request->names[i].c_str(), request->patterns[i].c_str()});
        // The OS's own dialog: NSOpenPanel/NSSavePanel on macOS, the portal or GTK on Linux, the
        // common item dialog on Windows. Title through properties, which the simple calls lack.
        const SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, request->filters.data());
        SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, Sint64(request->filters.size()));
        SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window);
        SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title.c_str());
        if (!suggested.empty()) SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, suggested.c_str());
        SDL_ShowFileDialogWithProperties(save ? SDL_FILEDIALOG_SAVEFILE : SDL_FILEDIALOG_OPENFILE, dialogDone,
                                         request.release(), props);
        SDL_DestroyProperties(props);
    };
    s.ask = [this](const std::string& title, const std::string& message, const std::vector<std::string>& buttons) {
        std::vector<SDL_MessageBoxButtonData> data;
        for (std::size_t i = 0; i < buttons.size(); ++i) {
            SDL_MessageBoxButtonFlags flags = 0;
            if (i == 0) flags |= SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
            if (i + 1 == buttons.size() && buttons.size() > 1) flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
            data.push_back(SDL_MessageBoxButtonData{flags, int(i), buttons[i].c_str()});
        }
        const SDL_MessageBoxData box{SDL_MESSAGEBOX_WARNING, window, title.c_str(), message.c_str(), int(data.size()),
                                     data.data(), nullptr};
        int pressed = -1;
        if (!SDL_ShowMessageBox(&box, &pressed)) return -1;
        return pressed;
    };
    s.setTitle = [this](const std::string& title) { SDL_SetWindowTitle(window, title.c_str()); };
    return s;
}

int main(int argc, char** argv) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    // Shaders live beside the executable: vCAD ships as a bare executable plus that folder.
    // SDL_GetBasePath is the executable's real directory, whatever argv[0] was.
    if (const char* base = SDL_GetBasePath()) cad::render::setShaderDirectory(std::string(base) + "shaders");

    Host host;
    host.window = SDL_CreateWindow("vCAD (libgui prototype)", 1280, 800,
                                   SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_METAL);
    if (host.window == nullptr) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    // For scripted screenshots: `screencapture -l <number>` captures this window even when covered.
    if (std::getenv("VCAD_PRINT_WINDOW") != nullptr) {
        long number = 0;
#if defined(__APPLE__)
        // [NSWindow windowNumber], through the runtime so this file stays plain C++.
        if (void* ns = SDL_GetPointerProperty(SDL_GetWindowProperties(host.window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,
                                              nullptr)) {
            using Msg = long (*)(void*, SEL);
            number = reinterpret_cast<Msg>(objc_msgSend)(ns, sel_registerName("windowNumber"));
        }
#endif
        std::printf("window %ld\n", number);
        std::fflush(stdout);
    }
    const char* frames = std::getenv("VCAD_FRAMES");
    host.maxFrames = frames ? std::atol(frames) : 0;

    // A Metal-backed view for bgfx. The Controller puts vCAD's own CAMetalLayer on it
    // (render/src/MetalSurface.mm), the same as for the Qt shell's native view.
    host.metalView = SDL_Metal_CreateView(host.window);
    if (host.metalView == nullptr) {
        std::fprintf(stderr, "SDL_Metal_CreateView: %s\n", SDL_GetError());
        return 1;
    }

    host.shell = std::make_unique<Shell>(std::span<const std::uint8_t>(cad_libgui_font, cad_libgui_font_size));
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(host.window, &pw, &ph);
    if (auto r = host.shell->attach(host.metalView, std::uint32_t(pw), std::uint32_t(ph),
                                    static_cast<double>(SDL_GetWindowPixelDensity(host.window)));
        !r) {
        const std::string detail = r.error().message + "\n\n" + r.error().detail;
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "vCAD", detail.c_str(), host.window);
        return 1;
    }
    host.shell->setHost(host.services());
    // `vcad_libgui part.vcad` opens it, as the Qt shell does.
    if (argc > 1) {
        if (auto r = host.shell->open(argv[1]); !r)
            std::fprintf(stderr, "could not open %s: %s\n", argv[1], r.error().message.c_str());
    }
    if (const char* cmds = std::getenv("VCAD_COMMANDS")) {
        std::string list = cmds;
        for (std::size_t at = 0; at <= list.size();) {
            const std::size_t end = std::min(list.find(',', at), list.size());
            if (end > at) host.shell->runCommand(list.substr(at, end - at));
            at = end + 1;
        }
        host.shell->fitView();
    }
    // Opens one command's Properties panel at start, for scripted checks of the panel.
    if (const char* panel = std::getenv("VCAD_PANEL")) host.shell->openPanel(panel);
    if (const char* action = std::getenv("VCAD_FILE")) host.shell->fileAction(action);
    if (const char* plane = std::getenv("VCAD_SKETCH")) host.shell->sketchOn(plane);
    SDL_StartTextInput(host.window);
    SDL_AddEventWatch(onEvent, &host);

    while (host.running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) host.handle(e);
        host.runPosted();
        if (host.running) host.tick();  // paced by the display: the Metal layer syncs to it
    }

    SDL_RemoveEventWatch(onEvent, &host);
    host.shell.reset();  // the renderer before the view it draws into
    SDL_Metal_DestroyView(host.metalView);
    SDL_DestroyWindow(host.window);
    SDL_Quit();
    return 0;
}
