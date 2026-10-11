#include "psp_osk_session.h"

void OskSession::open(const std::string& title, const std::string& initial, uint32_t max_len) {
    std::lock_guard<std::mutex> lock(m_);
    title_ = title;
    initial_ = initial;
    result_.clear();
    edit_.reset(initial, max_len);
    state_ = State::Editing;
}

bool OskSession::poll(std::string* result) {
    std::lock_guard<std::mutex> lock(m_);
    if (state_ != State::Done) return false;
    if (result) *result = result_;
    state_ = State::Idle;
    return true;
}

void OskSession::type(const std::string& utf8) {
    std::lock_guard<std::mutex> lock(m_);
    if (state_ == State::Editing) edit_.insert(utf8);
}

void OskSession::backspace() {
    std::lock_guard<std::mutex> lock(m_);
    if (state_ == State::Editing) edit_.backspace();
}

void OskSession::confirm() {
    std::lock_guard<std::mutex> lock(m_);
    if (state_ != State::Editing) return;
    result_ = edit_.utf8();
    state_ = State::Done;
}

void OskSession::cancel() {
    std::lock_guard<std::mutex> lock(m_);
    if (state_ != State::Editing) return;
    result_ = initial_;
    state_ = State::Done;
}

bool OskSession::active() const {
    std::lock_guard<std::mutex> lock(m_);
    return state_ == State::Editing;
}

OskSession::View OskSession::view() const {
    std::lock_guard<std::mutex> lock(m_);
    View v;
    v.active = state_ == State::Editing;
    v.title = title_;
    v.text = edit_.utf8();
    v.length = edit_.length();
    v.max_len = edit_.max_len();
    return v;
}

OskSession& osk_session() {
    static OskSession s;
    return s;
}
