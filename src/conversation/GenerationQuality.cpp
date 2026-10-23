#include <batbox/conversation/GenerationQuality.hpp>

#include <algorithm>
#include <cctype>

namespace batbox::conversation {
namespace {

struct Token {
    std::string_view text;
    bool eligible{false};
    bool word{false};
};

constexpr std::string_view receipt_key = "batbox_generation_quality";

bool accepted_full_content(const batbox::Json& observation) {
    if (!observation.is_object() || !observation.contains("kind") ||
        observation.at("kind") != "generated_observation") return false;
    const batbox::Json* receipt = nullptr;
    if (const auto found = observation.find(receipt_key); found != observation.end()) receipt = &*found;
    else if (const auto metadata = observation.find("metadata");
             metadata != observation.end() && metadata->is_object()) {
        if (const auto found = metadata->find(receipt_key); found != metadata->end()) receipt = &*found;
    }
    return receipt && receipt->is_object() && receipt->contains("version") &&
        receipt->at("version").is_number_integer() && receipt->at("version") == 1 &&
        receipt->contains("full_content_accepted") && receipt->at("full_content_accepted").is_boolean() &&
        receipt->at("full_content_accepted") == true;
}

bool space(char c) { return std::isspace(static_cast<unsigned char>(c)); }
bool letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

Token prose_token(std::string_view text, bool excluded) {
    if (excluded) return {text};
    // Markdown emphasis can surround a looping word. Keep separators in the
    // periodic sequence, but a separator-only run can never qualify.
    if (text == "---" || text == "***" || text == "___") return {text, true, false};
    for (const std::string_view marker : {"**", "__", "*", "_"}) {
        if (text.size() > marker.size() * 2 && text.starts_with(marker) && text.ends_with(marker)) {
            text.remove_prefix(marker.size());
            text.remove_suffix(marker.size());
            break;
        }
    }
    while (!text.empty() && (text.back() == '.' || text.back() == ',' ||
                            text.back() == '!' || text.back() == '?'))
        text.remove_suffix(1);
    if (text.empty() || text.size() > 64 || !letter(text.front()) || !letter(text.back())) return {text};
    if (!std::all_of(text.begin(), text.end(), [](char c) { return letter(c) || c == '\'' || c == '-'; }))
        return {text};
    // Common serialized values are data even without explicit code fencing.
    if (text == "true" || text == "false" || text == "null" ||
        text == "True" || text == "False" || text == "None") return {text};
    return {text, true, true};
}

std::vector<Token> tokenize(std::string_view content) {
    std::vector<Token> tokens;
    char fence = '\0';
    std::size_t fence_length = 0;
    std::size_t inline_ticks = 0;
    while (!content.empty()) {
        const auto newline = content.find('\n');
        const auto line = content.substr(0, newline);
        auto trimmed = line;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
            trimmed.remove_prefix(1);
        std::size_t marker_length = 0;
        const char marker = trimmed.empty() ? '\0' : trimmed.front();
        if (marker == '`' || marker == '~')
            while (marker_length < trimmed.size() && trimmed[marker_length] == marker) ++marker_length;
        const bool fence_line = marker_length >= 3 && (!fence || marker == fence);
        bool excluded_line = fence || fence_line || line.starts_with("    ") || line.starts_with('\t') ||
            trimmed.starts_with('>') || trimmed.starts_with('|');
        if (fence_line) {
            if (!fence) { fence = marker; fence_length = marker_length; }
            else if (marker_length >= fence_length) { fence = '\0'; fence_length = 0; }
            inline_ticks = 0;
        }
        std::size_t start = 0;
        while (start < line.size()) {
            while (start < line.size() && space(line[start])) ++start;
            if (start == line.size()) break;
            auto end = start;
            while (end < line.size() && !space(line[end])) ++end;
            const auto text = line.substr(start, end - start);
            bool excluded_token = excluded_line || inline_ticks != 0;
            if (!excluded_line) {
                for (std::size_t i = 0; i < text.size();) {
                    if (text[i] != '`') { ++i; continue; }
                    const auto tick_start = i;
                    while (i < text.size() && text[i] == '`') ++i;
                    const auto count = i - tick_start;
                    excluded_token = true;
                    if (!inline_ticks) inline_ticks = count;
                    else if (inline_ticks == count) inline_ticks = 0;
                }
            }
            tokens.push_back(prose_token(text, excluded_token));
            start = end;
        }
        if (newline == std::string_view::npos) break;
        content.remove_prefix(newline + 1);
    }
    return tokens;
}

} // namespace

std::optional<GenerationDegeneration> detect_degenerate_prose(std::string_view content) {
    const auto tokens = tokenize(content);
    if (tokens.size() < 128) return std::nullopt;
    std::vector<std::size_t> words(tokens.size() + 1);
    for (std::size_t i = 0; i < tokens.size(); ++i) words[i + 1] = words[i] + tokens[i].word;
    std::optional<GenerationDegeneration> strongest;
    for (std::size_t period = 1; period <= 4; ++period) {
        std::size_t matches = 0;
        for (std::size_t i = period; i < tokens.size(); ++i) {
            if (tokens[i].eligible && tokens[i - period].eligible && tokens[i].text == tokens[i - period].text)
                ++matches;
            else matches = 0;
            const auto repeats = (matches + period) / period;
            const auto repeated = repeats * period;
            if (repeats < 64 || repeated < 128 || repeated < (tokens.size() + 4) / 5) continue;
            const auto begin = i + 1 - repeated;
            if (words[i + 1] == words[begin]) continue;
            if (!strongest || repeated > strongest->repeated_tokens)
                strongest = GenerationDegeneration{tokens.size(), repeated, period, repeats};
        }
    }
    return strongest;
}

void replace_with_generation_recovery(Message& message) {
    message.role = Role::User;
    message.content = "[Generation recovery: the preceding assistant output was rejected for extreme repetition. "
        "Its original bytes remain in the session transcript and are not evidence of completed work. "
        "Continue the active task using verified tool results; check durable state before repeating any action.]";
    message.usage.reset();
}

std::size_t sanitize_degenerate_assistant_context(std::vector<Message>& messages) {
    std::size_t rejected = 0;
    for (auto& message : messages) {
        if (message.role == Role::Assistant && !message.tool_calls.has_value() &&
            detect_degenerate_prose(message.content)) {
            replace_with_generation_recovery(message);
            ++rejected;
        }
    }
    return rejected;
}

bool reject_generated_observation(const batbox::Json& observation) {
    if (!observation.is_object()) return false;
    const auto kind = observation.find("kind");
    const auto content = observation.find("content");
    return kind != observation.end() && kind->is_string() && *kind == "generated_observation" &&
           content != observation.end() && content->is_string() &&
           detect_degenerate_prose(content->get_ref<const std::string&>()).has_value();
}

bool stamp_generation_quality_receipt(batbox::Json& observation) {
    // A caller-provided flag can never waive insertion/outbox validation.
    if (reject_generated_observation(observation)) return false;
    if (observation.is_object() && observation.contains("kind") &&
        observation.at("kind") == "generated_observation" &&
        observation.contains("content") && observation.at("content").is_string()) {
        if (!observation.contains("metadata") || observation["metadata"].is_null())
            observation["metadata"] = batbox::Json::object();
        // Preserve malformed metadata for the service's normal validation;
        // do not grant a receipt to an invalid record.
        if (observation["metadata"].is_object()) {
            observation["metadata"][receipt_key] = {{"version", 1}, {"full_content_accepted", true}};
            // Native metadata is canonical, including when an imported queue
            // record happened to carry a caller-controlled top-level flag.
            observation.erase(std::string(receipt_key));
        }
    }
    return true;
}

std::size_t filter_degenerate_generated_memories(batbox::Json& recall) {
    if (accepted_full_content(recall)) return 0;
    if (reject_generated_observation(recall)) {
        // A top-level record cannot be erased from its parent here. Retain its
        // identity as a receipt, without sending its generated text to a model.
        recall["content"] = "[Generated observation withheld: extreme repetition; original memory retained.]";
        recall["generation_rejected"] = "degenerate_repetition";
        return 1;
    }
    std::size_t rejected = 0;
    if (recall.is_array()) {
        for (auto it = recall.begin(); it != recall.end();) {
            if (!accepted_full_content(*it) && reject_generated_observation(*it)) { it = recall.erase(it); ++rejected; }
            else { rejected += filter_degenerate_generated_memories(*it); ++it; }
        }
    } else if (recall.is_object()) {
        for (auto& value : recall) rejected += filter_degenerate_generated_memories(value);
    }
    return rejected;
}

} // namespace batbox::conversation
