// src/tui/ChatView.cpp
// =============================================================================
// batbox::tui::ChatView — implementation
//
// See include/batbox/tui/ChatView.hpp for design notes and API contract.
//
// Blueprint contract: batbox::tui::ChatView (blueprints table, CPP 1.7)
// =============================================================================

#include <batbox/tui/ChatView.hpp>
#include <batbox/perf/PerfSnapshot.hpp>
#include <batbox/tui/Events.hpp>
#include <batbox/tui/InputBar.hpp>
#include <batbox/tui/ThemeApply.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <ctime>
#include <thread>

using namespace ftxui;

#include "splash_taglines.hpp"

namespace batbox::tui {

// =============================================================================
// FTXUI keyboard input constants
// =============================================================================

namespace {

constexpr std::string_view kInputArrowUp   = "\x1b[A";
constexpr std::string_view kInputArrowDown = "\x1b[B";
constexpr std::string_view kInputPageUp    = "\x1b[5~";
constexpr std::string_view kInputPageDown  = "\x1b[6~";
constexpr std::string_view kInputEnd       = "\x1b[F";
constexpr std::string_view kInputEndAlt    = "\x1b[4~";  // some terminal variants

// How many lines a PgUp/PgDn moves (fraction of visible height).
// We use half the visible height for a comfortable overlap.
constexpr int kPageFraction = 2;

/// Format a role into the label prefix shown beside each message.
std::string role_label(batbox::conversation::Role role) {
    switch (role) {
        case batbox::conversation::Role::User:      return "You";
        case batbox::conversation::Role::Assistant: return "Batbox";
        case batbox::conversation::Role::Tool:      return "Tool";
        case batbox::conversation::Role::System:    return "System";
    }
    return "Unknown";
}

/// True when the given role should have its body rendered via MarkdownRenderer.
bool should_render_markdown(batbox::conversation::Role role) {
    return role == batbox::conversation::Role::User ||
           role == batbox::conversation::Role::Assistant;
}

} // namespace

// =============================================================================
// Construction
// =============================================================================

ChatView::ChatView(const batbox::theme::Theme& theme)
    : theme_(theme)
{}

// =============================================================================
// make_label — build the display label string for an entry
// =============================================================================

/*static*/
std::string ChatView::make_label(batbox::conversation::Role role,
                                 const batbox::conversation::Message& msg)
{
    std::string label = role_label(role);
    // For tool results, append the tool name if present.
    if (role == batbox::conversation::Role::Tool && msg.tool_name.has_value()) {
        label += "[" + *msg.tool_name + "]";
    }
    label += ": ";
    return label;
}

// =============================================================================
// append_message
// =============================================================================

void ChatView::append_message(const batbox::conversation::Message& msg) {
    MessageEntry entry;
    entry.role        = msg.role;
    entry.label       = make_label(msg.role, msg);
    entry.raw_content = msg.content;
    entry.is_tool_call = msg.is_tool_call();

    // Pre-render the body via MarkdownRenderer for markdown-bearing roles.
    if (should_render_markdown(msg.role) && !msg.content.empty()) {
        MarkdownRenderer md(theme_);
        md.append(msg.content);
        entry.rendered = md.render();
    } else if (!msg.content.empty()) {
        // Tool results / system messages: plain monospace, code_bg background.
        entry.rendered = paragraph(msg.content) | color(color_for(theme_, ThemeRole::Fg));
    } else {
        entry.rendered = emptyElement();
    }

    // Tool calls start collapsed; other roles start expanded.
    entry.collapsed = entry.is_tool_call;

    entries_.push_back(std::move(entry));

    // If pinned to bottom, stay pinned (no offset change needed; render will
    // naturally include the new last entry).
    // If the user has scrolled up, do NOT forcibly scroll down.
}

// =============================================================================
// set_streaming_text / clear_streaming
// =============================================================================

void ChatView::set_streaming_text(std::string_view text) {
    streaming_text_.assign(text.data(), text.size());
}

void ChatView::clear_streaming() {
    streaming_text_.clear();
}

// =============================================================================
// message_count
// =============================================================================

std::size_t ChatView::message_count() const noexcept {
    return entries_.size();
}

// =============================================================================
// set_screen_post_fn — TUI-FLOW-T1
// =============================================================================

void ChatView::set_screen_post_fn(std::function<void(ftxui::Event)> fn) {
    screen_post_fn_ = std::move(fn);
}

// =============================================================================
// TUI-FLOW-T6 — footer hint chips wiring
// =============================================================================

void ChatView::set_input_bar(InputBar* bar) {
    input_bar_ = bar;
}

// =============================================================================
// set_question_card — TUI-ASKQ-T4
// =============================================================================

void ChatView::set_question_card(std::shared_ptr<QuestionCard> qc) {
    question_card_ = std::move(qc);
}

// =============================================================================
// start_spinner_timer — TUI-FLOW-T1
// =============================================================================

void ChatView::start_spinner_timer() {
    if (spinner_active_) return;  // already running
    spinner_stop_flag_.store(false, std::memory_order_relaxed);
    spinner_thread_ = std::thread([this]() {
        while (!spinner_stop_flag_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!spinner_stop_flag_.load(std::memory_order_relaxed)) {
                if (screen_post_fn_) {
                    screen_post_fn_(batbox::tui::make_spinner_tick_event());
                }
            }
        }
    });
}

// =============================================================================
// stop_spinner_timer — TUI-FLOW-T1
// =============================================================================

void ChatView::stop_spinner_timer() {
    spinner_stop_flag_.store(true, std::memory_order_relaxed);
    if (spinner_thread_.joinable()) {
        spinner_thread_.join();
    }
}

// =============================================================================
// render_spinner_row — TUI-FLOW-T1
// =============================================================================

ftxui::Element ChatView::render_spinner_row() const {
    auto muted  = color_for(theme_, ThemeRole::Muted);
    auto accent = color_for(theme_, ThemeRole::AccentCyan);

    if (spinner_active_) {
        // Live spinner row: bullet + tagline + (Ns · arrow N tokens)
        const char* bullet = spinner_tokens_started_ ? "* " : "+ ";
        const char* arrow  = spinner_tool_in_flight_  ? "\u2191" : "\u2193"; // ↑ or ↓
        std::string elapsed_str = std::to_string(spinner_elapsed_s_) + "s";
        std::string tok_str     = std::to_string(spinner_token_count_) + " tokens";
        std::string suffix = "(" + elapsed_str + " \u00b7 " + arrow + " " + tok_str + ")";
        // · is U+00B7
        return hbox({
            text(bullet)                       | ftxui::color(accent),
            text(spinner_tagline_)             | ftxui::color(muted),
            text("  ")                         | ftxui::color(muted),
            text(suffix)                       | ftxui::color(muted),
        });
    }

    if (spinner_show_summary_) {
        // Frozen summary row after stream completes.
        std::string summary = "  (" + std::to_string(spinner_frozen_elapsed_s_) +
                              "s \u00b7 " + std::to_string(spinner_frozen_token_count_) +
                              " tokens)";
        return text(summary) | ftxui::color(muted) | dim;
    }

    return emptyElement();
}

// =============================================================================
// render_label — styled role prefix element
// =============================================================================

ftxui::Element ChatView::render_label(const MessageEntry& entry) const {
    ThemeRole label_role;
    switch (entry.role) {
        case batbox::conversation::Role::User:
            label_role = ThemeRole::PromptPrefix;
            break;
        case batbox::conversation::Role::Assistant:
            label_role = ThemeRole::AccentCyan;
            break;
        case batbox::conversation::Role::Tool:
            label_role = ThemeRole::Muted;
            break;
        case batbox::conversation::Role::System:
            label_role = ThemeRole::Muted;
            break;
        default:
            label_role = ThemeRole::Fg;
            break;
    }
    return text(entry.label) | bold | color(color_for(theme_, label_role));
}

// =============================================================================
// render_body — render the message body with appropriate styling
// =============================================================================

ftxui::Element ChatView::render_body(const MessageEntry& entry) const {
    if (entry.is_tool_call && entry.collapsed) {
        // Collapsed tool call: show a one-line summary.
        std::string summary = "<tool call";
        if (!entry.raw_content.empty()) {
            // Show just the first 60 chars of content as a collapsed preview.
            constexpr std::size_t kMaxPreview = 60;
            std::string preview = entry.raw_content.substr(
                0, std::min(entry.raw_content.size(), kMaxPreview));
            if (entry.raw_content.size() > kMaxPreview) preview += "…";
            summary += ": " + preview;
        }
        summary += " [collapsed]>";
        return text(summary) | color(color_for(theme_, ThemeRole::Muted)) | dim;
    }

    if (entry.role == batbox::conversation::Role::Tool) {
        // Tool results: monospace, slightly muted background hint.
        std::string body = entry.raw_content;
        if (body.empty()) body = "(no output)";
        return paragraph(body)
             | color(color_for(theme_, ThemeRole::Fg))
             | bgcolor(color_for(theme_, ThemeRole::CodeBg));
    }

    // Default: use pre-rendered markdown element.
    if (entry.rendered) {
        return entry.rendered;
    }
    return emptyElement();
}

// =============================================================================
// render_entry — compose label + body for one message
// =============================================================================

ftxui::Element ChatView::render_entry(const MessageEntry& entry) const {
    // TUI-FLOW-T7: user prompts render as a single-line grey bar prefixed with "> ".
    // The body text is shown in Muted colour; if the raw content exceeds
    // kUserPromptMaxChars characters (or contains a newline) it is truncated
    // with a trailing "â¦" so the bar stays on one line.
    //
    // TUI-FLOW-T8: when entry.expanded is true (user toggled via ctrl+o),
    // render the full raw content instead of the truncated form.  Multi-line
    // content is shown using paragraph() so it wraps at the terminal width.
    if (entry.role == batbox::conversation::Role::User) {
        constexpr std::size_t kUserPromptMaxChars = 120;

        auto prefix_color = color_for(theme_, ThemeRole::PromptPrefix);
        auto muted_color  = color_for(theme_, ThemeRole::Muted);

        // Determine whether this user prompt is collapsible (longer than the
        // display limit or contains a newline).  Only collapsible prompts
        // participate in ctrl+o toggling via TUI-FLOW-T8.
        bool has_newline    = (entry.raw_content.find('\n') != std::string::npos);
        bool over_limit     = (entry.raw_content.size() > kUserPromptMaxChars);
        bool is_collapsible = has_newline || over_limit;

        if (is_collapsible && entry.expanded) {
            // Expanded view: full content rendered verbatim.  Multi-line
            // input is displayed with paragraph() so it wraps at terminal width.
            // A dim "(ctrl+o to collapse)" hint follows the > prefix so the
            // user knows how to return to the collapsed view.
            return vbox({
                hbox({
                    text("> ")                   | ftxui::color(prefix_color),
                    text("(ctrl+o to collapse)") | ftxui::color(muted_color) | dim,
                }),
                paragraph(entry.raw_content) | ftxui::color(muted_color),
                separatorEmpty(),
            });
        }

        // Collapsed / non-collapsible view: single-line truncated form.
        // Flatten to first line only (collapse multi-line input).
        std::string body = entry.raw_content;
        auto newline_pos = body.find('\n');
        bool had_newline = (newline_pos != std::string::npos);
        if (had_newline) {
            body = body.substr(0, newline_pos);
        }

        // Truncate with ellipsis if still too long.
        bool truncated = had_newline || (body.size() > kUserPromptMaxChars);
        if (body.size() > kUserPromptMaxChars) {
            body = body.substr(0, kUserPromptMaxChars - 1);
        }
        if (truncated) {
            body += "\xe2\x80\xa6";  // UTF-8 for U+2026 HORIZONTAL ELLIPSIS "â¦"
        }

        return vbox({
            hbox({
                text("> ")   | ftxui::color(prefix_color),
                text(body)   | ftxui::color(muted_color),
            }),
            separatorEmpty(),
        });
    }

    auto label_el = render_label(entry);
    auto body_el  = render_body(entry);

    // Compose: label on its own line, then indented body (2-space indent via hbox).
    return vbox({
        label_el,
        hbox({text("  "), body_el}),
        separatorEmpty(),
    });
}

// =============================================================================
// render_streaming — the in-progress assistant turn at the bottom
// =============================================================================

ftxui::Element ChatView::render_streaming() const {
    if (streaming_text_.empty()) return emptyElement();

    // Label for streaming assistant turn.
    Element label_el = text("Batbox: ")
                     | bold
                     | color(color_for(theme_, ThemeRole::AccentCyan));

    // Incrementally rendered markdown body.
    MarkdownRenderer md(theme_);
    md.append(streaming_text_);
    Element body_el = hbox({text("  "), md.render()});

    return vbox({
        label_el,
        body_el,
        separatorEmpty(),
    });
}

// =============================================================================
// clamp_scroll — keep scroll_offset_ within [0, total_content_lines - visible]
// =============================================================================

void ChatView::clamp_scroll(int visible_height) {
    if (scroll_offset_ < 0) {
        scroll_offset_ = 0;
    }
    // We don't know exact line counts per entry without rendering; use the
    // simpler heuristic: cap at (entries count * 4) to prevent runaway scrolling
    // while still allowing the user to scroll up through many messages.
    // The actual ftxui yframe() handles clamping at the Element level.
    int max_scroll = static_cast<int>(entries_.size()) * 4 + (streaming_text_.empty() ? 0 : 4);
    if (max_scroll < 0) max_scroll = 0;
    if (scroll_offset_ > max_scroll) {
        scroll_offset_ = max_scroll;
    }
    (void)visible_height;  // reserved for future precise line counting
}


// =============================================================================
// TUI-FLOW-T2 — verb mapper
// =============================================================================

namespace {

/// Canonical verb lookup table for standard tool names.
/// Maps tool_name (case-insensitive via normalisation) to (gerund, past) pair.
struct VerbEntry {
    const char* key;       // lower-case normalised key
    const char* gerund;    // "Reading"
    const char* past;      // "Read"
};

static constexpr VerbEntry kVerbTable[] = {
    { "read",        "Reading",    "Read"      },
    { "read_file",   "Reading",    "Read"      },
    { "write",       "Writing",    "Write"     },
    { "write_file",  "Writing",    "Wrote"     },
    { "edit",        "Editing",    "Edited"    },
    { "edit_file",   "Editing",    "Edited"    },
    { "bash",        "Running",    "Ran"       },
    { "run",         "Running",    "Ran"       },
    { "glob",        "Searching",  "Searched"  },
    { "grep",        "Searching",  "Searched"  },
    { "web_fetch",   "Fetching",   "Fetched"   },
    { "webfetch",    "Fetching",   "Fetched"   },
    { "web_search",  "Searching",  "Searched"  },
    { "websearch",   "Searching",  "Searched"  },
    { "task",        "Spawning",   "Spawned"   },
    { "ls",          "Listing",    "Listed"    },
    { "list",        "Listing",    "Listed"    },
    { "find",        "Searching",  "Searched"  },
};

/// Normalise a tool name to lower-case for table lookup.
std::string normalise_tool_name(const std::string& name) {
    std::string n;
    n.reserve(name.size());
    for (char c : name) {
        n += (char)std::tolower((unsigned char)c);
    }
    return n;
}

/// Look up a verb entry by normalised key.  Returns nullptr if not found.
const VerbEntry* lookup_verb(const std::string& normalised) {
    for (const auto& e : kVerbTable) {
        if (normalised == e.key) return &e;
    }
    return nullptr;
}

} // anonymous namespace

/*static*/
std::string ChatView::verb_past(const std::string& tool_name) {
    const std::string norm = normalise_tool_name(tool_name);
    const VerbEntry* e = lookup_verb(norm);
    if (e) return e->past;
    // Unknown: capitalise first letter and use as-is
    if (tool_name.empty()) return "Ran";
    std::string r = tool_name;
    r[0] = (char)std::toupper((unsigned char)r[0]);
    return r;
}

/*static*/
std::string ChatView::verb_gerund(const std::string& tool_name) {
    const std::string norm = normalise_tool_name(tool_name);
    const VerbEntry* e = lookup_verb(norm);
    if (e) return e->gerund;
    // Unknown: "Running <tool_name>"
    if (tool_name.empty()) return "Running";
    return "Running";
}

// =============================================================================
// TUI-FLOW-T2 — tool_card_summary
// =============================================================================

std::string ChatView::tool_card_summary(const ToolCardEntry& card,
                                         int width) const {
    // Build the human-readable summary of the tool batch.
    // For a single tool: "Reading 1 file" / "Reading manifest.json"
    // For multiple:      "Reading 1 file, running 1 command"
    // When in-flight append "…"

    std::string summary;
    if (!card.preview_lines.empty()) {
        // Build compound summary from all preview lines
        std::string first_verb = card.in_flight
            ? verb_gerund(card.tool_name)
            : verb_past(card.tool_name);
        summary = first_verb;
        if (!card.preview_lines.empty() && !card.preview_lines[0].empty()) {
            summary += " " + card.preview_lines[0];
        } else {
            // Use a count-based fallback
            summary += " " + std::to_string(card.tool_count) + " "
                     + (card.tool_count == 1 ? "operation" : "operations");
        }
        // Additional tools
        for (std::size_t i = 1; i < card.preview_lines.size(); ++i) {
            // Build a gerund/past for subsequent tool names if we stored them
            summary += ", running additional tool";
        }
    } else {
        // Simple path: use tool_name + count
        std::string verb = card.in_flight
            ? verb_gerund(card.tool_name)
            : verb_past(card.tool_name);
        if (!card.args_summary.empty()) {
            summary = verb + " " + card.args_summary;
        } else {
            int cnt = card.tool_count;
            summary = verb + " " + std::to_string(cnt) + " "
                    + (cnt == 1 ? "operation" : "operations");
        }
    }
    if (card.in_flight) summary += "\xe2\x80\xa6"; // U+2026 ELLIPSIS "…"

    return summary;
}

// =============================================================================
// TUI-FLOW-T2 — render_tool_card
// =============================================================================

ftxui::Element ChatView::render_tool_card(const ToolCardEntry& card,
                                           int term_width) const {
    auto muted  = color_for(theme_, ThemeRole::Muted);
    auto accent = color_for(theme_, ThemeRole::AccentCyan);

    // --- Summary line ---
    // "* Reading manifest.json…  (ctrl+o to expand)"   [in flight, bullet]
    // "  Read manifest.json  (ctrl+o to expand)"        [complete, dim]

    // Affordance text at end of line
    std::string affordance = card.expanded
        ? "  (ctrl+o to collapse)"
        : "  (ctrl+o to expand)";

    std::string summary = tool_card_summary(card, term_width);

    // Truncate summary so total line fits within term_width - 6
    int avail = term_width - 6 - (int)affordance.size();
    if (avail > 0 && (int)summary.size() > avail) {
        // trim summary to avail bytes
        summary = summary.substr(0, (std::size_t)avail);
    }

    Elements summary_line;
    if (card.in_flight) {
        summary_line.push_back(text("* ") | ftxui::color(accent));
    } else {
        summary_line.push_back(text("  "));
    }
    summary_line.push_back(text(summary) | (card.in_flight
        ? ftxui::color(muted)
        : (ftxui::color(muted) | dim)));
    summary_line.push_back(text(affordance) | ftxui::color(muted) | dim);

    Elements rows;
    rows.push_back(hbox(std::move(summary_line)));

    // --- Preview line ---
    // Always show when in flight; show expanded lines when expanded
    if (card.in_flight || card.expanded) {
        // Primary preview line (the first arg)
        if (!card.args_summary.empty()) {
            std::string preview = card.args_summary;
            int max_preview = term_width - 6;
            if (max_preview > 0 && (int)preview.size() > max_preview) {
                preview = preview.substr(0, (std::size_t)max_preview);
            }
            rows.push_back(hbox({
                text("  \xe2\x94\x94 ") | ftxui::color(muted) | dim, // "  └─ "
                text(preview) | ftxui::color(muted) | dim,
            }));
        }
        // Additional preview lines when expanded
        if (card.expanded) {
            for (std::size_t i = 1; i < card.preview_lines.size(); ++i) {
                std::string pl = card.preview_lines[i];
                int mp = term_width - 6;
                if (mp > 0 && (int)pl.size() > mp) pl = pl.substr(0, (std::size_t)mp);
                rows.push_back(hbox({
                    text("  \xe2\x94\x94 ") | ftxui::color(muted) | dim,
                    text(pl) | ftxui::color(muted) | dim,
                }));
            }
        }
    }

    return vbox(std::move(rows));
}

// =============================================================================
// TUI-FLOW-T2 — render_tool_cards
// =============================================================================

ftxui::Element ChatView::render_tool_cards(int term_width) const {
    if (tool_cards_.empty()) return emptyElement();

    Elements cards;
    for (const auto& card : tool_cards_) {
        cards.push_back(render_tool_card(card, term_width));
    }
    return vbox(std::move(cards));
}

// =============================================================================
// OnRender
// =============================================================================

ftxui::Element ChatView::OnRender() {
    // TUI-FLOW-T3: time the entire OnRender() body (per-frame render duration).
    const auto render_start = std::chrono::steady_clock::now();

    // TUI-FLOW-T3: if a token was posted since the last OnRender, compute the
    // stream-to-paint latency now (token-event-post-time → render start).
    if (pending_token_post_time_.time_since_epoch().count() != 0) {
        int paint_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                render_start - pending_token_post_time_).count());
        batbox::perf::g_perf.set_stream_to_paint_ms(paint_ms);
        // Reset so we only record once per token batch.
        pending_token_post_time_ = std::chrono::steady_clock::time_point{};
    }

    // -------------------------------------------------------------------------
    // 1. Build all message rows.
    // -------------------------------------------------------------------------
    Elements rows;
    rows.reserve(entries_.size() + 2);

    // Top padding.
    rows.push_back(separatorEmpty());

    for (const auto& entry : entries_) {
        rows.push_back(render_entry(entry));
    }

    // TUI-FLOW-T1: spinner row (live counter or frozen summary).
    rows.push_back(render_spinner_row());

    // TUI-FLOW-T2: tool-call cards (live and completed) rendered between
    // the user prompt and the eventual assistant text reply.
    rows.push_back(render_tool_cards(term_width_));

    // Streaming tail (empty element when nothing is streaming).
    rows.push_back(render_streaming());

    // -------------------------------------------------------------------------
    // 2. Compose into a scrollable vbox.
    //
    // FTXUI does not have a built-in "scroll from bottom" primitive.  We use
    // yframe() with an inversion trick: we request a focus element at the
    // very bottom, then let yframe clip from there upwards by scroll_offset_.
    //
    // Strategy:
    //   - Wrap all rows in a vbox.
    //   - Place a zero-height focus sentinel at the bottom.
    //   - Use yframe() so FTXUI scrolls to keep the sentinel in view when
    //     scroll_offset_ == 0.
    //   - When scroll_offset_ > 0 the sentinel is pushed below the viewport by
    //     the scroll amount, achieved by appending blank padding rows.
    // -------------------------------------------------------------------------

    // Append blank rows equal to scroll_offset_ to push content upward
    // (simulating a scroll-up from the bottom).
    for (int i = 0; i < scroll_offset_; ++i) {
        rows.push_back(text(""));
    }

    // The focus sentinel: a dselect-highlighted empty element that FTXUI
    // uses to anchor the yframe at the bottom.
    auto sentinel = text("") | focus;

    rows.push_back(sentinel);

    auto content = vbox(std::move(rows));

    // Capture visible height for PgUp/PgDn (approximate from last Terminal size).
    // FTXUI does not expose the box size inside OnRender directly; we use the
    // Screen dimension query via the Element sizing decorator as a proxy.
    // The size() decorator captures the rendered box dimensions into our field.
    last_visible_height_ = std::max(last_visible_height_, 8);

    // TUI-FLOW-T3: record per-frame render duration.
    {
        auto render_end = std::chrono::steady_clock::now();
        int frame_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                render_end - render_start).count());
        batbox::perf::g_perf.set_frame_ms(frame_ms);
    }

    return content
         | vscroll_indicator
         | yframe
         | flex;
}

// =============================================================================
// OnEvent — token/user-message/stream-done events + keyboard scroll handling
// =============================================================================

bool ChatView::OnEvent(ftxui::Event event) {
    // -------------------------------------------------------------------------
    // Token event: append chunk to streaming buffer and update the display tail.
    // Posted by screen_mgr.post_token() from the inference worker thread.
    // -------------------------------------------------------------------------
    if (auto tok = extract_token(event); tok.has_value()) {
        streaming_buffer_ += tok->text;
        set_streaming_text(streaming_buffer_);
        // TUI-FLOW-T1: count content tokens for the spinner row.
        if (!tok->text.empty()) {
            ++spinner_token_count_;
            spinner_tokens_started_ = true;
            // TUI-FLOW-T3: stamp the time this token event arrived so OnRender()
            // can compute stream-to-paint latency on the next frame.
            pending_token_post_time_ = std::chrono::steady_clock::now();
        }
        return true;
    }

    // -------------------------------------------------------------------------
    // UserMessage event: add the user's submitted text to the history.
    // Posted by the on_submit lambda (UI thread) before run_turn starts.
    // -------------------------------------------------------------------------
    if (auto um = extract_user_message(event); um.has_value()) {
        batbox::conversation::Message msg;
        msg.role    = batbox::conversation::Role::User;
        msg.content = um->text;
        append_message(msg);

        // TUI-FLOW-T1: start the live spinner for this turn.
        spinner_active_           = true;
        spinner_tokens_started_   = false;
        spinner_tool_in_flight_   = false;
        spinner_elapsed_s_        = 0;
        spinner_token_count_      = 0;
        spinner_show_summary_     = false;
        // TUI-FLOW-T2: reset tool cards for this new turn.
        tool_cards_.clear();
        // Pick a tagline from kTaglines deterministically by elapsed seconds seed.
        {
            std::size_t idx = static_cast<std::size_t>(
                std::time(nullptr)) % kTaglines.size();
            spinner_tagline_ = std::string(kTaglines[idx]) + "…";
        }
        start_spinner_timer();
        // TUI-FLOW-T6: signal InputBar that a stream is now in flight.
        if (input_bar_) input_bar_->set_stream_active(true);
        return true;
    }

    // -------------------------------------------------------------------------
    // StreamDone event: commit the streaming buffer as a completed assistant
    // message, then clear the streaming state.
    // Posted by the worker thread after run_turn() returns.
    // -------------------------------------------------------------------------
    if (auto sd = extract_stream_done(event); sd.has_value()) {
        // TUI-FLOW-T1: stop the spinner and freeze summary data before
        // committing the buffer so the frozen summary row can persist.
        if (spinner_active_) {
            stop_spinner_timer();
            spinner_frozen_elapsed_s_    = spinner_elapsed_s_;
            spinner_frozen_token_count_  = spinner_token_count_;
            spinner_active_              = false;
            spinner_show_summary_        = true;
        }
        // TUI-FLOW-T6: signal InputBar that the stream is complete.
        if (input_bar_) input_bar_->set_stream_active(false);
        if (!streaming_buffer_.empty()) {
            batbox::conversation::Message msg;
            msg.role    = batbox::conversation::Role::Assistant;
            msg.content = streaming_buffer_;
            append_message(msg);
        }
        streaming_buffer_.clear();
        clear_streaming();
        return true;
    }

    // -------------------------------------------------------------------------
    // MessageAppended event: a tool-call or tool-result message was appended
    // to the conversation by the worker thread.  Render it immediately.
    // Posted by the on_message_appended callback (worker thread) after each
    // tool-call assistant message and tool-result message is persisted.
    // -------------------------------------------------------------------------
    if (auto ma = extract_message_appended(event); ma.has_value()) {
        batbox::conversation::Message msg;
        if (ma->role == "tool") {
            msg.role      = batbox::conversation::Role::Tool;
            msg.tool_name = ma->tool_name.empty()
                                ? std::optional<std::string>(std::nullopt)
                                : std::make_optional(ma->tool_name);
            msg.is_error  = ma->is_error
                                ? std::make_optional(true)
                                : std::optional<bool>(std::nullopt);
            // Truncate tool result content to 200 chars for display.
            constexpr std::size_t kMaxResultDisplay = 200;
            if (ma->content.size() > kMaxResultDisplay) {
                msg.content = ma->content.substr(0, kMaxResultDisplay) + "…";
            } else {
                msg.content = ma->content;
            }
        } else {
            // "assistant" role — this is a tool-call message.
            // TUI-FLOW-T2: The [tool: ...] inline marker is superseded by the
            // live tool-call card rendered via the ToolRunning/ToolDone event
            // path.  We still append the message to maintain conversation
            // history but skip adding it to the visible entries_ list — the
            // card is the canonical UI representation.
            // NOTE: We intentionally do NOT call append_message() here so
            // that old "[tool: ...]" collapsed entries are not shown.
            // Streaming buffer becomes redundant once the canonical MessageAppended arrives.
            streaming_buffer_.clear();
            clear_streaming();
            return true;
        }
        append_message(msg);
        return true;
    }

    // -------------------------------------------------------------------------
    // TUI-FLOW-T1 + TUI-FLOW-T2: ToolRunning
    //   T1: flip spinner arrow to ↑ while a tool is dispatching.
    //   T2: append or update the current tool-call card.
    // -------------------------------------------------------------------------
    if (auto tr = extract_tool_running(event); tr.has_value()) {
        spinner_tool_in_flight_ = true;

        // TUI-FLOW-T2: manage the tool-card state vector.
        // If there is no current in-flight card, push a new one.
        // If the latest card is still in-flight (same dispatch batch arriving
        // in multiple ToolRunning events), update its count.
        bool appended_to_existing = false;
        if (!tool_cards_.empty() && tool_cards_.back().in_flight) {
            // Same batch: increment tool count and add preview line
            auto& back = tool_cards_.back();
            back.tool_count = std::max(back.tool_count, tr->tool_count);
            if (!tr->args_summary.empty() &&
                back.preview_lines.size() < 8) {
                back.preview_lines.push_back(tr->args_summary);
            }
            appended_to_existing = true;
        }
        if (!appended_to_existing) {
            ToolCardEntry card;
            card.tool_name   = tr->tool_name;
            card.args_summary = tr->args_summary;
            card.tool_count  = tr->tool_count;
            card.in_flight   = true;
            card.expanded    = false;
            if (!tr->args_summary.empty()) {
                card.preview_lines.push_back(tr->args_summary);
            }
            tool_cards_.push_back(std::move(card));
        }

        return false;  // Let InputBar also handle this for its status row.
    }

    // -------------------------------------------------------------------------
    // TUI-FLOW-T1 + TUI-FLOW-T2: ToolDone
    //   T1: flip spinner arrow back to â when tool dispatch finishes.
    //   T2: mark the current in-flight card as complete.
    // -------------------------------------------------------------------------
    if (auto td = extract_tool_done(event); td.has_value()) {
        spinner_tool_in_flight_ = false;

        // TUI-FLOW-T2: mark the most recent in-flight card as complete.
        for (auto it = tool_cards_.rbegin(); it != tool_cards_.rend(); ++it) {
            if (it->in_flight) {
                it->in_flight = false;
                break;
            }
        }

        return false;  // Let InputBar also handle this for its status row.
    }

    // -------------------------------------------------------------------------
    // TUI-FLOW-T8: ctrl+o — toggle expand/collapse on the most-recent
    // collapsible entry.  Collapsible entries are:
    //   (a) ToolCardEntry (completed, not in-flight) — expanded flag.
    //   (b) MessageEntry with User role and raw_content > 120 chars or
    //       containing a newline — expanded flag.
    //
    // Algorithm: walk both tool_cards_ and entries_ backwards, find the
    // most-recently-appended collapsible item, and toggle it.  If an
    // item is already expanded, collapse it first (second ctrl+o
    // re-collapses the entry that was just expanded).  If nothing is
    // collapsible, return false so the event falls through to InputBar.
    //
    // Modal gate: when show_question_card_ is set, the WireTui CatchEvent
    // layer routes the event to QuestionCard before ChatView sees it, so
    // this block is generally unreachable during a modal.  The explicit
    // guard below is a belt-and-suspenders safety for future rewiring.
    // -------------------------------------------------------------------------
    if (!event.is_mouse() && event.input() == "\x0f") {
        // Modal gate: do not consume ctrl+o while question modal is visible.
        if (show_question_card_.load(std::memory_order_acquire)) {
            return false;
        }

        // Signed indices so we can use -1 as "exhausted".
        int tc_idx = static_cast<int>(tool_cards_.size()) - 1;
        int me_idx = static_cast<int>(entries_.size())    - 1;

        // Priority 1: collapse any currently-expanded item (second ctrl+o
        // behaviour — re-collapses what was just expanded).

        for (int i = tc_idx; i >= 0; --i) {
            if (!tool_cards_[i].in_flight && tool_cards_[i].expanded) {
                tool_cards_[i].expanded = false;
                return true;
            }
        }
        for (int i = me_idx; i >= 0; --i) {
            if (entries_[i].role == batbox::conversation::Role::User &&
                entries_[i].expanded) {
                entries_[i].expanded = false;
                return true;
            }
        }

        // Priority 2: no item is currently expanded — expand the most-recent
        // collapsible one.  Tool cards checked first (appear later in layout).

        for (int i = tc_idx; i >= 0; --i) {
            if (!tool_cards_[i].in_flight) {
                tool_cards_[i].expanded = true;
                return true;
            }
        }

        constexpr std::size_t kUserPromptMaxChars = 120;
        for (int i = me_idx; i >= 0; --i) {
            if (entries_[i].role == batbox::conversation::Role::User) {
                const auto& raw = entries_[i].raw_content;
                bool has_nl   = (raw.find('\n') != std::string::npos);
                bool too_long = (raw.size() > kUserPromptMaxChars);
                if (has_nl || too_long) {
                    entries_[i].expanded = true;
                    return true;
                }
            }
        }

        // Nothing collapsible on screen — fall through.
        return false;
    }

    // -------------------------------------------------------------------------
    // TUI-FLOW-T1: SpinnerTick — advance the elapsed-seconds counter.
    // Posted at 1Hz by the spinner timer thread.
    // -------------------------------------------------------------------------
    if (auto st = extract_spinner_tick(event); st.has_value()) {
        ++spinner_elapsed_s_;
        return true;
    }

    // -------------------------------------------------------------------------
    // TUI-ASKQ-T4: QuestionShow — load spec into QuestionCard, show overlay.
    //
    // The worker thread (AskUserQuestion::run()) posts this event after calling
    // question_card_->set_spec() and before blocking on await_user_answer().
    // ChatView sets show_question_card_ = true to make the overlay visible on
    // the next render frame.
    //
    // Freeform text capture (design note):
    //   When the user selects the "Type something…" row in QuestionCard, the
    //   card resolves with chosen_labels empty and freeform_text = "".  The
    //   model receives an empty freeform_text and may re-ask.  Full InputBar
    //   "modal capture" mode for freeform text is deferred to a follow-up task;
    //   the empty-string result is a documented, intentional behaviour for T4.
    // -------------------------------------------------------------------------
    if (auto qs = extract_question_show(event); qs.has_value()) {
        if (question_card_) {
            question_card_->set_spec(*qs);
            show_question_card_.store(true, std::memory_order_release);
        }
        return true;
    }

    // -------------------------------------------------------------------------
    // TUI-ASKQ-T4: QuestionResolved — hide the QuestionCard overlay.
    //
    // Posted by the QuestionCard itself (via resolve() → hide()) after the user
    // presses Enter or Esc.  ChatView clears show_question_card_ so the overlay
    // disappears on the next render frame and keyboard focus returns to InputBar.
    // The resolved payload (chosen_labels, freeform_text, cancelled, etc.) is
    // passed to the callback stored in QuestionShowPayload::callback by the
    // worker thread's await_user_answer() unblocking path — ChatView does not
    // need to inspect the payload.
    // -------------------------------------------------------------------------
    if (auto qr = extract_question_resolved(event); qr.has_value()) {
        show_question_card_.store(false, std::memory_order_release);
        return true;
    }

    // -------------------------------------------------------------------------
    // Mouse events — not handled by ChatView.
    // -------------------------------------------------------------------------
    if (event.is_mouse()) return false;

    const auto& input = event.input();

    // ArrowUp or 'k' — scroll up 1 line
    if (input == kInputArrowUp || input == "k") {
        scroll_offset_ += 1;
        clamp_scroll(last_visible_height_);
        return true;
    }

    // ArrowDown or 'j' — scroll down 1 line (toward bottom)
    if (input == kInputArrowDown || input == "j") {
        scroll_offset_ = std::max(0, scroll_offset_ - 1);
        return true;
    }

    // PageUp — scroll up half the visible height
    if (input == kInputPageUp) {
        int step = std::max(1, last_visible_height_ / kPageFraction);
        scroll_offset_ += step;
        clamp_scroll(last_visible_height_);
        return true;
    }

    // PageDown — scroll down half the visible height (toward bottom)
    if (input == kInputPageDown) {
        int step = std::max(1, last_visible_height_ / kPageFraction);
        scroll_offset_ = std::max(0, scroll_offset_ - step);
        return true;
    }

    // End / 'G' — snap back to bottom, re-enable auto-scroll
    if (input == kInputEnd || input == kInputEndAlt || input == "G") {
        scroll_offset_ = 0;
        return true;
    }

    return false;
}

} // namespace batbox::tui
