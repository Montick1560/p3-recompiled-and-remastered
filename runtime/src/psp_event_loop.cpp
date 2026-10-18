#include "psp_event_loop.h"
#include "psp_runtime.h"
#include "psp_render_queue.h"
#include "psp_scheduler.h"
#include "psp_ge_draw.h"
#include "psp_present.h"
#include "psp_pad.h"
#include "psp_controls.h"
#include "psp_wheel.h"
#include "psp_osk_session.h"
#include "psp_osk_overlay.h"
#include "psp_menu_prompt.h"
#include "psp_exit.h"
#include "psp_perf.h"

#include <SDL.h>
#include <glad/glad.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <thread>

// --- Global state definitions (declared extern in psp_runtime.h) ---
std::atomic<bool> g_should_exit{false};
std::atomic<int>  g_alive_threads{0};
std::thread::id   g_main_thread_id;
std::atomic<uint32_t> g_host_buttons{0};
std::atomic<uint32_t> g_pad_buttons{0};
std::atomic<uint16_t> g_pad_analog{0x8080};
std::atomic<uint32_t> g_wheel_buttons{0};

// ---------------------------------------------------------------------------
// Keyboard and gamepad -> PSP buttons: PSPRECOMP_CONTROLS (settings.ini [controls], psp_controls.h),
// today's mapping by default. Built once SDL is up (it resolves SDL key / button names).
// ---------------------------------------------------------------------------
static ControlMap g_controls;

static void load_controls() {
    const char* spec = std::getenv("PSPRECOMP_CONTROLS");
    std::vector<std::string> log;
    g_controls = ControlMap::build(
        spec ? spec : "", [](const std::string& n) { return static_cast<int32_t>(SDL_GetKeyFromName(n.c_str())); },
        [](const std::string& n) { return static_cast<int>(SDL_GameControllerGetButtonFromString(n.c_str())); },
        &log);
    for (const std::string& l : log) std::fprintf(stderr, "%s\n", l.c_str());
    std::fprintf(stderr, "[CONTROLS] %s%s\n", spec ? "" : "defaults: ", g_controls.describe().c_str());
}

// --- Static state (SDL window + GL context, module-private) ---
static SDL_Window*  g_window     = nullptr;
static SDL_GLContext g_gl_context = nullptr;

// F11 / Alt+Enter: toggle borderless desktop fullscreen.
static void toggle_fullscreen() {
    if (!g_window) return;
    const bool full = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    SDL_SetWindowFullscreen(g_window, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
}

// --- Game controllers (SDL GameController API) ---
// Keyed by joystick instance id (what button/axis/removed events carry).
static PadSet g_pads;
static std::map<SDL_JoystickID, SDL_GameController*> g_controllers;
static constexpr int kStickDeadzone = 8000;

static void publish_pads() {
    // While the back-to-menu prompt is open the game sees no pad buttons.
    g_pad_buttons.store(psp_menu_prompt().active() ? 0u : g_pads.buttons(),
                        std::memory_order_relaxed);
    g_pad_analog.store(static_cast<uint16_t>(g_pads.analog_x() | (g_pads.analog_y() << 8)),
                       std::memory_order_relaxed);
}

// Returns true when the event was a controller event.
static bool handle_controller_event(const SDL_Event& ev) {
    switch (ev.type) {
    case SDL_CONTROLLERDEVICEADDED: {
        SDL_GameController* c = SDL_GameControllerOpen(ev.cdevice.which);
        if (!c) {
            std::fprintf(stderr, "[RT] controller open failed: %s\n", SDL_GetError());
            return true;
        }
        const SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c));
        if (g_controllers.count(id)) {  // already open (startup + hotplug event)
            SDL_GameControllerClose(c);
            return true;
        }
        g_controllers[id] = c;
        const char* name = SDL_GameControllerName(c);
        std::fprintf(stderr, "[RT] controller connected: %s\n", name ? name : "(unnamed)");
        return true;
    }
    case SDL_CONTROLLERDEVICEREMOVED: {
        const SDL_JoystickID id = ev.cdevice.which;
        g_pads.remove(id);
        auto it = g_controllers.find(id);
        if (it != g_controllers.end()) {
            SDL_GameControllerClose(it->second);
            g_controllers.erase(it);
        }
        std::fprintf(stderr, "[RT] controller disconnected (id %d)\n", static_cast<int>(id));
        publish_pads();
        return true;
    }
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
        const uint32_t mask = g_controls.pad_mask(ev.cbutton.button);
        if (mask) {
            g_pads.set_button(ev.cbutton.which, mask, ev.type == SDL_CONTROLLERBUTTONDOWN);
            publish_pads();
        }
        return true;
    }
    case SDL_CONTROLLERAXISMOTION: {
        const SDL_JoystickID id = ev.caxis.which;
        const int v = ev.caxis.value;
        switch (ev.caxis.axis) {
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
            g_pads.set_trigger(id, ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT,
                               pad_trigger_pressed(v));
            break;
        case SDL_CONTROLLER_AXIS_LEFTX:
            g_pads.set_stick(id, pad_axis_to_psp(v, kStickDeadzone), g_pads.stick_y(id));
            break;
        case SDL_CONTROLLER_AXIS_LEFTY:
            g_pads.set_stick(id, g_pads.stick_x(id), pad_axis_to_psp(v, kStickDeadzone));
            break;
        default:
            return true;
        }
        publish_pads();
        return true;
    }
    default:
        return false;
    }
}

// --- Mouse wheel: one D-pad UP/DOWN press per notch (psp_wheel.h) ---
static WheelPulses g_wheel;

static int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void publish_wheel() {
    g_wheel_buttons.store(g_wheel.buttons(steady_ms()), std::memory_order_relaxed);
}

// The keyboard grid edits the game's text through the OSK session.
struct SessionTarget : OskTarget {
    void type(const std::string& s) override { osk_session().type(s); }
    void backspace() override { osk_session().backspace(); }
    void move_cursor(int d) override { osk_session().move_cursor(d); }
    void accept() override { osk_session().confirm(); }
    void cancel() override { osk_session().cancel(); }
};
static SessionTarget g_osk_target;
static OskStickRepeat g_osk_stick;
static float g_osk_axis_x = 0.0f, g_osk_axis_y = 0.0f;

// While the game waits on the on-screen keyboard (sceUtilityOsk), keyboard
// input edits the text instead of pressing PSP buttons: typed characters
// (SDL_TEXTINPUT, accents and IME included), Backspace, arrows move the text
// cursor, Enter = accept, Esc = cancel. Gamepad buttons follow the fixed
// keyboard layout (Global Constraints): D-pad / left stick move, A press,
// B backspace, X shift, Y space, LB/RB text cursor, back layer, Start accept.
// Returns true when the event was consumed.
static bool handle_osk_event(const SDL_Event& ev) {
    OskSession& osk = osk_session();
    if (!osk.active()) return false;
    switch (ev.type) {
    case SDL_TEXTINPUT:
        osk.type(ev.text.text);
        return true;
    case SDL_KEYDOWN:
        if (ev.key.keysym.sym == SDLK_F11 ||
            (ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT))) {
            return false;  // fullscreen toggle still works
        }
        if (ev.key.keysym.sym == SDLK_BACKSPACE) osk.backspace();
        else if (ev.key.keysym.sym == SDLK_LEFT) osk.move_cursor(-1);
        else if (ev.key.keysym.sym == SDLK_RIGHT) osk.move_cursor(1);
        else if (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_KP_ENTER) osk.confirm();
        else if (ev.key.keysym.sym == SDLK_ESCAPE) osk.cancel();
        return true;
    case SDL_KEYUP:
        return true;
    case SDL_CONTROLLERBUTTONDOWN: {
        static const std::pair<int, OskAction> kPad[] = {
            {SDL_CONTROLLER_BUTTON_DPAD_UP, OskAction::Up}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, OskAction::Down},
            {SDL_CONTROLLER_BUTTON_DPAD_LEFT, OskAction::Left}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, OskAction::Right},
            {SDL_CONTROLLER_BUTTON_A, OskAction::Press}, {SDL_CONTROLLER_BUTTON_B, OskAction::Backspace},
            {SDL_CONTROLLER_BUTTON_X, OskAction::Shift}, {SDL_CONTROLLER_BUTTON_Y, OskAction::Space},
            {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, OskAction::CursorLeft},
            {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, OskAction::CursorRight},
            {SDL_CONTROLLER_BUTTON_BACK, OskAction::Layer}, {SDL_CONTROLLER_BUTTON_START, OskAction::Accept}};
        for (const auto& [b, a] : kPad) {
            if (ev.cbutton.button == b) { psp_osk_keyboard().act(a, g_osk_target); break; }
        }
        return true;
    }
    case SDL_CONTROLLERBUTTONUP:
        return true;
    case SDL_CONTROLLERAXISMOTION:
        if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) g_osk_axis_x = ev.caxis.value / 32767.0f;
        if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) g_osk_axis_y = ev.caxis.value / 32767.0f;
        return true;
    default:
        return false;
    }
}

// Start/stop SDL text input as the keyboard box opens and closes, and drop
// held PSP buttons so nothing stays pressed underneath it.
static void sync_osk_text_input() {
    static bool was_active = false;
    const bool active = osk_session().active();
    if (active == was_active) return;
    was_active = active;
    if (active) {
        g_host_buttons.store(0, std::memory_order_relaxed);
        psp_osk_keyboard_reset();
        for (auto& [id, c] : g_controllers) g_pads.remove(id);
        publish_pads();
        g_osk_axis_x = g_osk_axis_y = 0;
        SDL_StartTextInput();
    } else {
        SDL_StopTextInput();
    }
}

// Back to the launcher menu: F1, or Select+Start held 1 s on a gamepad (the Guide button
// too, when SDL delivers it) opens a Yes/No prompt; while it is open the game gets no input.
// Yes ends the process with exit code 10 (docs/contracts/settings-v1.md).
static ComboHold g_menu_combo(1000);

static void open_menu_prompt() {
    if (osk_session().active() || psp_menu_prompt().active()) return;
    psp_menu_prompt().open();
    std::fprintf(stderr, "[RT] back-to-menu prompt open\n");
    g_host_buttons.store(0, std::memory_order_relaxed);
    publish_pads();
}

static void finish_menu_prompt(PromptResult r) {
    publish_pads();  // the prompt may have closed: the game sees the pads again
    if (r != PromptResult::Confirmed) return;
    std::fprintf(stderr, "[RT] back to the launcher menu (exit code 10)\n");
    psp_requested_exit_code().store(10);
    g_should_exit.store(true);
}

// Returns true when the event was consumed by the prompt (or opened it). Key and
// button releases are not consumed: they still reach the normal handlers, so a
// button held when the prompt opened (Select+Start) does not stay pressed after it.
static bool handle_menu_prompt_event(const SDL_Event& ev) {
    MenuPrompt& p = psp_menu_prompt();
    if (!p.active()) {
        if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.sym == SDLK_F1) {
            open_menu_prompt();
            return true;
        }
        if (ev.type == SDL_CONTROLLERBUTTONDOWN && ev.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) {
            open_menu_prompt();
            return true;
        }
        return false;
    }
    if (ev.type == SDL_KEYDOWN) {
        if (ev.key.keysym.sym == SDLK_F11 ||
            (ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT))) {
            return false;  // fullscreen toggle still works
        }
        if (ev.key.repeat) return true;
        switch (ev.key.keysym.sym) {
        case SDLK_LEFT: finish_menu_prompt(p.handle(PromptAction::Left)); break;
        case SDLK_RIGHT: finish_menu_prompt(p.handle(PromptAction::Right)); break;
        case SDLK_UP: finish_menu_prompt(p.handle(PromptAction::Up)); break;
        case SDLK_DOWN: finish_menu_prompt(p.handle(PromptAction::Down)); break;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_x:
            finish_menu_prompt(p.handle(PromptAction::Confirm)); break;
        case SDLK_ESCAPE: case SDLK_F1: case SDLK_z:
            finish_menu_prompt(p.handle(PromptAction::Cancel)); break;
        default: break;
        }
        return true;
    }
    if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
        switch (ev.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: finish_menu_prompt(p.handle(PromptAction::Left)); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: finish_menu_prompt(p.handle(PromptAction::Right)); break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: finish_menu_prompt(p.handle(PromptAction::Up)); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: finish_menu_prompt(p.handle(PromptAction::Down)); break;
        case SDL_CONTROLLER_BUTTON_A: finish_menu_prompt(p.handle(PromptAction::Confirm)); break;
        case SDL_CONTROLLER_BUTTON_B: finish_menu_prompt(p.handle(PromptAction::Cancel)); break;
        default: break;
        }
        return true;
    }
    return ev.type == SDL_TEXTINPUT || ev.type == SDL_MOUSEWHEEL;
}

// Handle one SDL event: window close + keyboard button state + controllers.
static void handle_sdl_event(const SDL_Event& ev) {
    if (handle_menu_prompt_event(ev)) return;
    if (handle_osk_event(ev)) return;
    if (handle_controller_event(ev)) return;
    if (ev.type == SDL_MOUSEWHEEL) {
        int y = ev.wheel.y;
        if (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) y = -y;
        if (y != 0) g_wheel.add(y, steady_ms());
        return;
    }
    if (ev.type == SDL_QUIT) {
        g_should_exit.store(true);
    } else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
        if (ev.key.repeat) return;  // ignore OS key repeat
        if (ev.type == SDL_KEYDOWN &&
            (ev.key.keysym.sym == SDLK_F11 ||
             (ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT)))) {
            toggle_fullscreen();  // Alt+Enter must not also press CROSS
            return;
        }
        uint32_t bit = g_controls.key_mask(static_cast<int32_t>(ev.key.keysym.sym));
        if (bit == 0) return;
        if (ev.type == SDL_KEYDOWN) {
            g_host_buttons.fetch_or(bit, std::memory_order_relaxed);
        } else {
            g_host_buttons.fetch_and(~bit, std::memory_order_relaxed);
        }
    } else if (ev.type == SDL_WINDOWEVENT &&
               ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
        // KEYUP is not delivered after focus loss; release every key so
        // none sticks. Controllers keep delivering events without focus.
        g_host_buttons.store(0, std::memory_order_relaxed);
    }
}


// ---------------------------------------------------------------------------
// psp_runtime_init_sdl — create SDL window + GL 3.3 core context + GLAD
// ---------------------------------------------------------------------------
int psp_runtime_init_sdl() {
    // Record main thread id for GL_THREAD_CHECK
    g_main_thread_id = std::this_thread::get_id();

    // 1. Initialize SDL video subsystem
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr,
            "[RT] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    // 1b. Game controllers are optional: keyboard-only when unavailable.
    //     Already-connected pads arrive as SDL_CONTROLLERDEVICEADDED events.
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "[RT] game controllers unavailable: %s\n", SDL_GetError());
    }
    load_controls();

    // 2. Set GL attributes: OpenGL 3.3 core profile, double-buffered
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_PROFILE_MASK,
        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    // 3. Create a resizable window at an integer multiple of the PSP
    //    resolution (PSPRECOMP_WINDOW_SCALE, default 2); present_blit()
    //    letterboxes the rendered image into whatever size it gets.
    const int win_scale = present_window_scale(std::getenv("PSPRECOMP_WINDOW_SCALE"));
    g_window = SDL_CreateWindow(
        "PSPrecomp Runtime",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        480 * win_scale, 272 * win_scale,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!g_window) {
        std::fprintf(stderr,
            "[RT] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // 4. Create GL context (must precede gladLoadGL — Pitfall 6)
    g_gl_context = SDL_GL_CreateContext(g_window);
    if (!g_gl_context) {
        std::fprintf(stderr,
            "[RT] SDL_GL_CreateContext failed: %s\n",
            SDL_GetError());
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return 1;
    }

    // 5. Load GL function pointers via GLAD2 (MUST be after context creation)
    //    GLAD2 requires a loader function; SDL2 provides SDL_GL_GetProcAddress.
    if (!gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress)) {
        std::fprintf(stderr, "[RT] gladLoadGL failed\n");
        SDL_GL_DeleteContext(g_gl_context);
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return 1;
    }

    SDL_SetWindowMinimumSize(g_window, 480, 272);
    SDL_StopTextInput();  // on only while the on-screen keyboard is open
    {
        const char* fs = std::getenv("PSPRECOMP_FULLSCREEN");
        if (fs && std::strcmp(fs, "1") == 0) {
            SDL_SetWindowFullscreen(g_window, SDL_WINDOW_FULLSCREEN_DESKTOP);
        }
    }

    // 6. Print GL info for diagnostics
    std::fprintf(stderr, "[RT] GL: %s\n",
        glGetString(GL_VERSION));

    // 7. Clear to black and swap — show initial frame
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(g_window);

    std::fprintf(stderr, "[RT] SDL/GL initialized (%dx%d)\n",
        480 * win_scale, 272 * win_scale);
    return 0;
}

// ---------------------------------------------------------------------------
// psp_event_loop — main-thread loop: SDL events + render queue + alive check
// ---------------------------------------------------------------------------
void psp_event_loop(uint8_t* rdram) {
    const bool osk_demo =
        std::getenv("PSPRECOMP_OSK_DEMO") && std::string(std::getenv("PSPRECOMP_OSK_DEMO")) == "1";
    const uint64_t demo_at = static_cast<uint64_t>(steady_ms()) + 2000;
    bool demo_open = false;
    while (!g_should_exit.load()) {
        // 1. Pump SDL events (window close, keyboard input)
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            handle_sdl_event(ev);
        }
        publish_wheel();  // timed wheel presses advance even without events
        // Select (0x0001) + Start (0x0008) held on any pad for 1 s.
        if (g_menu_combo.update((g_pads.buttons() & 0x0009u) == 0x0009u, steady_ms()))
            open_menu_prompt();
        sync_osk_text_input();
        if (osk_session().active()) {
            if (auto a = g_osk_stick.update(g_osk_axis_x, g_osk_axis_y, static_cast<uint64_t>(steady_ms())))
                psp_osk_keyboard().act(*a, g_osk_target);
        }

        // PSPRECOMP_OSK_DEMO=1: one keyboard request over whatever is on screen, to look at the skins.
        if (osk_demo && !demo_open && static_cast<uint64_t>(steady_ms()) >= demo_at &&
            !osk_session().active()) {
            osk_session().open("Demo", "", 16);
            demo_open = true;
            std::fprintf(stderr, "[OSK] demo keyboard open\n");
        }
        if (demo_open) {
            std::string text;
            if (osk_session().poll(&text)) std::fprintf(stderr, "[OSK] demo text: %s\n", text.c_str());
        }

        // 2. Drain render queue — execute pending GL work from game threads
        render_queue_process();

        // 2a. Liveness net (issue #59): present the front buffer if lists
        //     were processed but no real page-flip arrived recently. No-op
        //     when a flip just presented (not dirty / within budget).
        ge_draw_present_if_stale(rdram);

        // 2b. Service any pending debug-socket screenshot request here on
        //     the main (GL) thread — the socket thread never touches GL.
        ge_draw_service_screenshot_request();
        perf_tick();  // PSPRECOMP_PERF=1: [PERF] line every 2 s

        // 3. Check if any game threads are still alive
        if (g_alive_threads.load() == 0) {
            std::fprintf(stderr,
                "[RT] All game threads finished — lingering 3s to flush render\n");
            auto linger_end = std::chrono::steady_clock::now()
                              + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < linger_end
                   && !g_should_exit.load()) {
                SDL_Event ev;
                while (SDL_PollEvent(&ev)) {
                    handle_sdl_event(ev);
                }
                render_queue_process();
                ge_draw_present_if_stale(rdram);
                ge_draw_service_screenshot_request();
                SDL_Delay(16);  // ~60fps drain rate
            }
            break;
        }

        // 4. Brief sleep — 1ms balances CPU usage vs responsiveness.
        //    Skip delay when GE lists are pending to drain them faster.
        if (!render_queue_has_pending_ge()) {
            SDL_Delay(1);
        }
    }

    // Quit requested: the window goes away now; the rest of shutdown runs behind it.
    if (g_window) {
        SDL_HideWindow(g_window);
        SDL_PumpEvents();
    }
}

// ---------------------------------------------------------------------------
// Signal handling — SIGTERM / SIGINT set g_should_exit
// ---------------------------------------------------------------------------
static void signal_handler(int sig) {
    (void)sig;
    g_should_exit.store(true);
}

void psp_install_signal_handlers() {
#ifdef _WIN32
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);
#else
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
#endif
}

// ---------------------------------------------------------------------------
// psp_runtime_shutdown — host teardown: controllers, GL, window, SDL (guest threads are
// stopped by psp_cpu_close before this and never joined)
// ---------------------------------------------------------------------------
SDL_Window* psp_get_sdl_window() {
    return g_window;
}

void psp_runtime_shutdown() {
    // 1. Ensure exit flag is set (in case called without signal)
    g_should_exit.store(true);

    // 2. Close game controllers
    for (auto& [id, c] : g_controllers) SDL_GameControllerClose(c);
    g_controllers.clear();

    // 3. Destroy GL context
    if (g_gl_context) {
        SDL_GL_DeleteContext(g_gl_context);
        g_gl_context = nullptr;
    }

    // 4. Destroy SDL window
    if (g_window) {
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
    }

    // 5. Quit SDL
    SDL_Quit();

    std::fprintf(stderr, "[RT] Shutdown complete\n");
}
