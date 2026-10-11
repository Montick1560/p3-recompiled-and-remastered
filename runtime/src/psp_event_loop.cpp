#include "psp_event_loop.h"
#include "psp_runtime.h"
#include "psp_render_queue.h"
#include "psp_scheduler.h"
#include "psp_ge_draw.h"
#include "psp_present.h"
#include "psp_pad.h"
#include "psp_wheel.h"
#include "psp_osk_session.h"
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
// Keyboard -> PSP button mapping
//   Arrows -> D-pad | X / Enter -> CROSS | Z -> CIRCLE | A -> SQUARE
//   S -> TRIANGLE   | Q -> LTRIGGER     | W -> RTRIGGER
//   Space -> START  | Tab -> SELECT
// ---------------------------------------------------------------------------
static uint32_t key_to_psp_button(SDL_Keycode key) {
    switch (key) {
        case SDLK_UP:     return 0x0010;  // UP
        case SDLK_RIGHT:  return 0x0020;  // RIGHT
        case SDLK_DOWN:   return 0x0040;  // DOWN
        case SDLK_LEFT:   return 0x0080;  // LEFT
        case SDLK_x:
        case SDLK_RETURN: return 0x4000;  // CROSS
        case SDLK_z:      return 0x2000;  // CIRCLE
        case SDLK_a:      return 0x8000;  // SQUARE
        case SDLK_s:      return 0x1000;  // TRIANGLE
        case SDLK_q:      return 0x0100;  // LTRIGGER
        case SDLK_w:      return 0x0200;  // RTRIGGER
        case SDLK_SPACE:  return 0x0008;  // START
        case SDLK_TAB:    return 0x0001;  // SELECT
        default:          return 0;
    }
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
    g_pad_buttons.store(g_pads.buttons(), std::memory_order_relaxed);
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
        const uint32_t mask = pad_button_to_psp(ev.cbutton.button);
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

// While the game waits on the on-screen keyboard (sceUtilityOsk), keyboard
// input edits the text instead of pressing PSP buttons: typed characters
// (SDL_TEXTINPUT, accents and IME included), Backspace, Enter = accept,
// Esc = cancel. A controller's Start accepts and B cancels. Returns true
// when the event was consumed.
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
        else if (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_KP_ENTER) osk.confirm();
        else if (ev.key.keysym.sym == SDLK_ESCAPE) osk.cancel();
        return true;
    case SDL_KEYUP:
        return true;
    case SDL_CONTROLLERBUTTONDOWN:
        if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_START) osk.confirm();
        else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_B) osk.cancel();
        return true;
    case SDL_CONTROLLERBUTTONUP:
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
        SDL_StartTextInput();
    } else {
        SDL_StopTextInput();
    }
}

// Handle one SDL event: window close + keyboard button state + controllers.
static void handle_sdl_event(const SDL_Event& ev) {
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
        uint32_t bit = key_to_psp_button(ev.key.keysym.sym);
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
    while (!g_should_exit.load()) {
        // 1. Pump SDL events (window close, keyboard input)
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            handle_sdl_event(ev);
        }
        publish_wheel();  // timed wheel presses advance even without events
        sync_osk_text_input();

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

    // Final drain to unblock any game thread blocked in render_queue_post
    render_queue_process();
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
// psp_runtime_shutdown — clean shutdown: threads, render queue, SDL/GL
// ---------------------------------------------------------------------------
SDL_Window* psp_get_sdl_window() {
    return g_window;
}

void psp_runtime_shutdown() {
    // 1. Ensure exit flag is set (in case called without signal)
    g_should_exit.store(true);

    // 2. Shutdown scheduler — wake all threads, join with timeout
    psp_scheduler_shutdown();

    // 3. Final render queue drain
    render_queue_process();

    // 3b. Close game controllers
    for (auto& [id, c] : g_controllers) SDL_GameControllerClose(c);
    g_controllers.clear();

    // 4. Destroy GL context
    if (g_gl_context) {
        SDL_GL_DeleteContext(g_gl_context);
        g_gl_context = nullptr;
    }

    // 5. Destroy SDL window
    if (g_window) {
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
    }

    // 6. Quit SDL
    SDL_Quit();

    std::fprintf(stderr, "[RT] Shutdown complete\n");
}
