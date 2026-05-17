// src/tui/Events.cpp
//
// BatBox custom ftxui::Event subtypes — implementation.
//
// See include/batbox/tui/Events.hpp for design notes and usage examples.

#include "batbox/tui/Events.hpp"

#include <any>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

namespace batbox::tui {

// =============================================================================
// Event name constants
//
// Each constant is an ftxui::Event::Special with the canonical name string.
// Components test identity with operator==, which compares input_ strings.
// We embed a unique prefix ("batbox.") to avoid collision with FTXUI internals.
// =============================================================================

// Internal helpers — canonical special-string names
static constexpr const char* kNameToken        = "batbox.token";
static constexpr const char* kNameAgentsDirty  = "batbox.agents-dirty";
static constexpr const char* kNameDemonDirty   = "batbox.demon-dirty";
static constexpr const char* kNameStatusUpdate = "batbox.status-update";
static constexpr const char* kNameModalShow    = "batbox.modal-show";
static constexpr const char* kNameModalHide    = "batbox.modal-hide";
static constexpr const char* kNameUserMessage  = "batbox.user-message";
static constexpr const char* kNameStreamDone   = "batbox.stream-done";
static constexpr const char* kNameMessageAppended = "batbox.message-appended";
static constexpr const char* kNameToolRunning        = "batbox.tool-running";
static constexpr const char* kNameToolDone           = "batbox.tool-done";
static constexpr const char* kNameThinkingStarted    = "batbox.thinking-started";
static constexpr const char* kNameThinkingStopped    = "batbox.thinking-stopped";
static constexpr const char* kNameSpinnerTick        = "batbox.spinner-tick";
static constexpr const char* kNamePlanApprovalShow    = "batbox.plan-approval-show";
static constexpr const char* kNameQuestionShow       = "batbox.question-show";
static constexpr const char* kNameQuestionResolved   = "batbox.question-resolved";

const ftxui::Event Events::Token        = ftxui::Event::Special(kNameToken);
const ftxui::Event Events::AgentsDirty  = ftxui::Event::Special(kNameAgentsDirty);
const ftxui::Event Events::DemonDirty   = ftxui::Event::Special(kNameDemonDirty);
const ftxui::Event Events::StatusUpdate = ftxui::Event::Special(kNameStatusUpdate);
const ftxui::Event Events::ModalShow    = ftxui::Event::Special(kNameModalShow);
const ftxui::Event Events::ModalHide    = ftxui::Event::Special(kNameModalHide);
const ftxui::Event Events::UserMessage  = ftxui::Event::Special(kNameUserMessage);
const ftxui::Event Events::StreamDone        = ftxui::Event::Special(kNameStreamDone);
const ftxui::Event Events::MessageAppended  = ftxui::Event::Special(kNameMessageAppended);
const ftxui::Event Events::ToolRunning      = ftxui::Event::Special(kNameToolRunning);
const ftxui::Event Events::ToolDone         = ftxui::Event::Special(kNameToolDone);
const ftxui::Event Events::ThinkingStarted  = ftxui::Event::Special(kNameThinkingStarted);
const ftxui::Event Events::ThinkingStopped  = ftxui::Event::Special(kNameThinkingStopped);
const ftxui::Event Events::SpinnerTick      = ftxui::Event::Special(kNameSpinnerTick);
const ftxui::Event Events::PlanApprovalShow = ftxui::Event::Special(kNamePlanApprovalShow);
const ftxui::Event Events::QuestionShow    = ftxui::Event::Special(kNameQuestionShow);
const ftxui::Event Events::QuestionResolved = ftxui::Event::Special(kNameQuestionResolved);

// =============================================================================
// Payload registry
//
// When a factory function creates an event it must attach a payload that will
// survive until the main-loop thread's OnEvent handler retrieves it.
// ftxui::Event has no generic payload slot so we maintain a static registry
// keyed by a per-event unique ID embedded in the event's special string.
//
// Key format:  "<canonical-name>:<seq>"
//   e.g.  "batbox.token:0000000042"
//
// The seq counter is a monotonically increasing atomic so keys are unique
// even when many background threads fire events simultaneously.
// =============================================================================

namespace {

using AnyPayload = std::any;

struct Registry {
    std::mutex                              mtx;
    std::unordered_map<std::string, AnyPayload> store;
};

Registry& registry() {
    static Registry s_registry;
    return s_registry;
}

/// Global sequence number — wraps at 2^64 (practically never).
std::atomic<uint64_t> g_seq{0};

/// Build a unique event key: "<base_name>:<zero-padded-seq>"
std::string make_key(const char* base_name) {
    uint64_t seq = g_seq.fetch_add(1, std::memory_order_relaxed);
    // Zero-pad to 20 digits so lexicographic sort == numeric sort (not strictly
    // required here but nice for debugging).
    char buf[32];
    // Using snprintf for portability (no <format> on C++20 on all platforms).
    std::snprintf(buf, sizeof(buf), "%020llu",
                  static_cast<unsigned long long>(seq));
    return std::string(base_name) + ':' + buf;
}

/// Store a payload and return the key embedded in the event's special string.
template<typename T>
std::string store_payload(const char* base_name, T&& payload) {
    std::string key = make_key(base_name);
    Registry& reg   = registry();
    std::lock_guard<std::mutex> lk(reg.mtx);
    reg.store.emplace(key, std::forward<T>(payload));
    return key;
}

/// Extract and erase a payload of type T.  Returns nullopt if not found or
/// wrong type.
template<typename T>
std::optional<T> extract_payload(const std::string& key) {
    Registry& reg = registry();
    std::lock_guard<std::mutex> lk(reg.mtx);
    auto it = reg.store.find(key);
    if (it == reg.store.end()) {
        return std::nullopt;
    }
    T* ptr = std::any_cast<T>(&it->second);
    if (!ptr) {
        return std::nullopt;
    }
    T result = std::move(*ptr);
    reg.store.erase(it);
    return result;
}

} // anonymous namespace

// =============================================================================
// Factory functions
// =============================================================================

ftxui::Event make_token_event(std::string text, std::string agent_id) {
    TokenPayload p{std::move(text), std::move(agent_id)};
    std::string key = store_payload(kNameToken, std::move(p));
    return ftxui::Event::Special(key);
}

ftxui::Event make_agents_dirty_event(std::string agent_id,
                                      uint32_t    step,
                                      uint32_t    tokens,
                                      std::string status) {
    AgentsDirtyPayload p{std::move(agent_id), step, tokens, std::move(status)};
    std::string key = store_payload(kNameAgentsDirty, std::move(p));
    return ftxui::Event::Special(key);
}

ftxui::Event make_demon_dirty_event(std::string demon_id) {
    DemonDirtyPayload p{std::move(demon_id)};
    std::string key = store_payload(kNameDemonDirty, std::move(p));
    return ftxui::Event::Special(key);
}

ftxui::Event make_status_update_event(SidecarState state, std::string detail) {
    StatusUpdatePayload p{state, std::move(detail)};
    std::string key = store_payload(kNameStatusUpdate, std::move(p));
    return ftxui::Event::Special(key);
}

ftxui::Event make_modal_show_event(std::string title,
                                    std::string body,
                                    std::string tool_name,
                                    std::function<void(ModalResult)> callback) {
    ModalShowPayload p{std::move(title), std::move(body),
                       std::move(tool_name), std::move(callback)};
    std::string key = store_payload(kNameModalShow, std::move(p));
    return ftxui::Event::Special(key);
}

ftxui::Event make_modal_hide_event(ModalResult result) {
    ModalHidePayload p{result};
    std::string key = store_payload(kNameModalHide, std::move(p));
    return ftxui::Event::Special(key);
}

ftxui::Event make_user_message_event(std::string text) {
    UserMessagePayload p{std::move(text)};
    std::string key = store_payload(kNameUserMessage, std::move(p));
    return ftxui::Event::Special(key);
}

ftxui::Event make_stream_done_event(bool had_error) {
    StreamDonePayload p{"assistant", had_error};
    std::string key = store_payload(kNameStreamDone, std::move(p));
    return ftxui::Event::Special(key);
}

ftxui::Event make_tool_running_event(std::string tool_name,
                                       std::string args_summary,
                                       int         tool_count) {
    ToolRunningPayload p{std::move(tool_name), std::move(args_summary), tool_count};
    std::string key = store_payload(kNameToolRunning, std::move(p));
    return ftxui::Event::Special(key);
}

ftxui::Event make_tool_done_event() {
    ToolDonePayload p{};
    std::string key = store_payload(kNameToolDone, std::move(p));
    return ftxui::Event::Special(key);
}

ftxui::Event make_thinking_started_event() {
    ThinkingStartedPayload p{};
    std::string key = store_payload(kNameThinkingStarted, std::move(p));
    return ftxui::Event::Special(key);
}

ftxui::Event make_thinking_stopped_event() {
    ThinkingStoppedPayload p{};
    std::string key = store_payload(kNameThinkingStopped, std::move(p));
    return ftxui::Event::Special(key);
}

ftxui::Event make_spinner_tick_event() {
    SpinnerTickPayload p{};
    std::string key = store_payload(kNameSpinnerTick, std::move(p));
    return ftxui::Event::Special(key);
}

ftxui::Event make_message_appended_event(std::string role,
                                          std::string tool_name,
                                          std::string content,
                                          bool        is_error) {
    MessageAppendedPayload p{std::move(role), std::move(tool_name),
                              std::move(content), is_error};
    std::string key = store_payload(kNameMessageAppended, std::move(p));
    return ftxui::Event::Special(key);
}

ftxui::Event make_question_show_event(QuestionShowPayload payload) {
    std::string key = store_payload(kNameQuestionShow, std::move(payload));
    return ftxui::Event::Special(key);
}

ftxui::Event make_question_resolved_event(QuestionResolvedPayload payload) {
    std::string key = store_payload(kNameQuestionResolved, std::move(payload));
    return ftxui::Event::Special(key);
}

// =============================================================================
// Extractor functions
//
// Each extractor checks that the event's input string starts with the expected
// prefix, then looks up the payload in the registry.
// =============================================================================

namespace {

/// Returns true when the event's special string begins with the given prefix
/// followed by a colon (the separator we inserted in make_key).
bool has_prefix(const ftxui::Event& ev, const char* prefix) {
    const std::string& s = ev.input();
    std::size_t plen = std::strlen(prefix);
    return s.size() > plen && s.compare(0, plen, prefix) == 0 && s[plen] == ':';
}

} // anonymous namespace

std::optional<TokenPayload> extract_token(const ftxui::Event& ev) {
    if (!has_prefix(ev, kNameToken)) return std::nullopt;
    return extract_payload<TokenPayload>(ev.input());
}

std::optional<AgentsDirtyPayload> extract_agents_dirty(const ftxui::Event& ev) {
    if (!has_prefix(ev, kNameAgentsDirty)) return std::nullopt;
    return extract_payload<AgentsDirtyPayload>(ev.input());
}

std::optional<DemonDirtyPayload> extract_demon_dirty(const ftxui::Event& ev) {
    if (!has_prefix(ev, kNameDemonDirty)) return std::nullopt;
    return extract_payload<DemonDirtyPayload>(ev.input());
}

std::optional<StatusUpdatePayload> extract_status_update(const ftxui::Event& ev) {
    if (!has_prefix(ev, kNameStatusUpdate)) return std::nullopt;
    return extract_payload<StatusUpdatePayload>(ev.input());
}

std::optional<ModalShowPayload> extract_modal_show(const ftxui::Event& ev) {
    if (!has_prefix(ev, kNameModalShow)) return std::nullopt;
    return extract_payload<ModalShowPayload>(ev.input());
}

std::optional<ModalHidePayload> extract_modal_hide(const ftxui::Event& ev) {
    if (!has_prefix(ev, kNameModalHide)) return std::nullopt;
    return extract_payload<ModalHidePayload>(ev.input());
}

std::optional<UserMessagePayload> extract_user_message(const ftxui::Event& ev) {
    if (!has_prefix(ev, kNameUserMessage)) return std::nullopt;
    return extract_payload<UserMessagePayload>(ev.input());
}

std::optional<StreamDonePayload> extract_stream_done(const ftxui::Event& ev) {
    if (!has_prefix(ev, kNameStreamDone)) return std::nullopt;
    return extract_payload<StreamDonePayload>(ev.input());
}

std::optional<MessageAppendedPayload> extract_message_appended(const ftxui::Event& ev) {
    if (!has_prefix(ev, kNameMessageAppended)) return std::nullopt;
    return extract_payload<MessageAppendedPayload>(ev.input());
}

std::optional<ToolRunningPayload> extract_tool_running(const ftxui::Event& ev) {
    if (!has_prefix(ev, kNameToolRunning)) return std::nullopt;
    return extract_payload<ToolRunningPayload>(ev.input());
}

std::optional<ToolDonePayload> extract_tool_done(const ftxui::Event& ev) {
    if (!has_prefix(ev, kNameToolDone)) return std::nullopt;
    return extract_payload<ToolDonePayload>(ev.input());
}

std::optional<ThinkingStartedPayload> extract_thinking_started(const ftxui::Event& ev) {
    if (!has_prefix(ev, kNameThinkingStarted)) return std::nullopt;
    return extract_payload<ThinkingStartedPayload>(ev.input());
}

std::optional<ThinkingStoppedPayload> extract_thinking_stopped(const ftxui::Event& ev) {
    if (!has_prefix(ev, kNameThinkingStopped)) return std::nullopt;
    return extract_payload<ThinkingStoppedPayload>(ev.input());
}

std::optional<SpinnerTickPayload> extract_spinner_tick(const ftxui::Event& ev) {
    if (!has_prefix(ev, kNameSpinnerTick)) return std::nullopt;
    return extract_payload<SpinnerTickPayload>(ev.input());
}

std::optional<QuestionShowPayload> extract_question_show(const ftxui::Event& ev) {
    if (!has_prefix(ev, kNameQuestionShow)) return std::nullopt;
    return extract_payload<QuestionShowPayload>(ev.input());
}

std::optional<QuestionResolvedPayload> extract_question_resolved(const ftxui::Event& ev) {
    if (!has_prefix(ev, kNameQuestionResolved)) return std::nullopt;
    return extract_payload<QuestionResolvedPayload>(ev.input());
}

} // namespace batbox::tui
