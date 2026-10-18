#pragma once
// On-screen keyboard hand-off (pure, thread-safe). The sceUtilityOsk HLE
// (a guest thread) opens a request and polls for the result; the main thread
// feeds keyboard input and draws view(). Confirm returns the typed text;
// cancel returns the initial text, like PPSSPP's input box.
#include "psp_text_edit.h"

#include <cstdint>
#include <mutex>
#include <string>

class OskSession {
public:
    struct View {
        bool active = false;
        std::string title;
        std::string text;
        uint32_t length = 0;
        uint32_t cursor = 0;
        uint32_t max_len = 0;
    };

    // Guest side.
    void open(const std::string& title, const std::string& initial, uint32_t max_len);
    bool poll(std::string* result);  // true once, when the user finished

    // Main-thread side (no-ops while idle).
    void type(const std::string& utf8);
    void backspace();
    void move_cursor(int delta);
    void confirm();
    void cancel();
    bool active() const;
    View view() const;

private:
    enum class State { Idle, Editing, Done };
    mutable std::mutex m_;
    State state_ = State::Idle;
    std::string title_;
    std::string initial_;
    std::string result_;
    TextEdit edit_;
};

// The process-wide session used by the HLE and the event loop.
OskSession& osk_session();
