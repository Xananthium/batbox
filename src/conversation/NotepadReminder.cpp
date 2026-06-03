// src/conversation/NotepadReminder.cpp
//
// Implementation of batbox::conversation::NotepadReminder (DIS-981, S6).

#include <batbox/conversation/NotepadReminder.hpp>

#include <string>

namespace batbox::conversation {

std::string compose_notepad_reminder(const std::string& pad_slice) {
    if (pad_slice.empty()) return {};

    std::string out;
    out.reserve(pad_slice.size() + 256);
    out += "<notepad>\n";
    out += "This is your working notepad for the current task — your own "
           "running plan, distilled findings, and decisions-and-why. It lives "
           "outside the conversation, so it survives compaction, and it is "
           "shown back to you each turn. Use notepad_append to add to it and "
           "notepad_read to query it. Keep it current; it is what lets raw "
           "tool output be pruned without losing what mattered.\n\n";
    out += pad_slice;
    if (out.back() != '\n') out += '\n';
    out += "</notepad>";
    return out;
}

bool apply_notepad_reminder(batbox::inference::ChatRequest& req,
                            const std::string&              pad_slice) {
    const std::string reminder = compose_notepad_reminder(pad_slice);
    if (reminder.empty()) {
        return false;  // empty pad → leave the request (and its cache) untouched
    }

    batbox::inference::WireMessage msg;
    msg.role    = "system";
    msg.content = reminder;
    req.messages.push_back(std::move(msg));  // tail-only mutation
    return true;
}

} // namespace batbox::conversation
