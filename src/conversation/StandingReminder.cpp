// src/conversation/StandingReminder.cpp
//
// Implementation of batbox::conversation::StandingReminder (DIS-988, S2/S3 AC4).
// Mirrors NotepadReminder's cache-disciplined TAIL-append shape exactly.

#include <batbox/conversation/StandingReminder.hpp>

#include <string>

namespace batbox::conversation {

std::string compose_standing_reminder(const std::vector<StandingHandle>& handles,
                                      std::size_t                        max_handles) {
    if (handles.empty()) return {};

    const std::size_t shown =
        (handles.size() < max_handles) ? handles.size() : max_handles;

    std::string out;
    out.reserve(shown * 96 + 256);
    out += "<warm_subagents>\n";
    out += "You have warm subagent(s) standing by for follow-up. Their context "
           "is still loaded — interrogate one by its handle instead of "
           "re-spawning to ask a follow-up question.\n";
    for (std::size_t i = 0; i < shown; ++i) {
        const StandingHandle& h = handles[i];
        out += "- ";
        out += h.id;
        if (!h.name.empty()) {
            out += " (";
            out += h.name;
            out += ")";
        }
        out += ": ";
        out += h.status_line;
        out += '\n';
    }
    if (handles.size() > shown) {
        out += "- (+";
        out += std::to_string(handles.size() - shown);
        out += " more)\n";
    }
    out += "</warm_subagents>";
    return out;
}

bool apply_standing_reminder(batbox::inference::ChatRequest&    req,
                             const std::vector<StandingHandle>& handles,
                             std::size_t                        max_handles) {
    const std::string reminder = compose_standing_reminder(handles, max_handles);
    if (reminder.empty()) {
        return false;  // no warm subagents → leave the request (and cache) untouched
    }

    batbox::inference::WireMessage msg;
    msg.role    = "system";
    msg.content = reminder;
    req.messages.push_back(std::move(msg));  // tail-only mutation
    return true;
}

} // namespace batbox::conversation
