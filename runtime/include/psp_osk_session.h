#pragma once
// On-screen keyboard hand-off (pure, thread-safe). The sceUtilityOsk HLE
// (a guest thread) opens a request and polls for the result; the main thread
// feeds keyboard input and draws view(). Confirm returns the typed text;
// cancel returns the initial text, like PPSSPP's input box, and is reported
// as a cancel so the HLE can answer the game with CANCEL.
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
    // True once, when the user finished; *cancelled (optional) tells Esc / B on an
    // empty text from an accepted text.
    bool poll(std::string* result, bool* cancelled = nullptr);

    // Main-thread side (no-ops while idle).
    void type(const std::string& utf8);
    void backspace();
    void back();  // pad B: delete one character, or cancel when the text is empty
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
    bool cancelled_ = false;
    TextEdit edit_;
};

// The process-wide session used by the HLE and the event loop.
OskSession& osk_session();
