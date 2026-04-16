#include "llmlog/core/isql_splitter.hpp"

#include <cctype>
#include <string>

namespace llmlog::core {

namespace {

bool iequalsView(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

std::string_view trimView(std::string_view v) noexcept {
    const auto isSpace = [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    };
    while (!v.empty() && isSpace(v.front())) v.remove_prefix(1);
    while (!v.empty() && isSpace(v.back()))  v.remove_suffix(1);
    return v;
}

/// Detects a 'SET TERM <new> [<old>]' directive at the start of @p statement.
/// Firebird's isql writes 'SET TERM ^ ;' to switch from ';' to '^'. The
/// trailing old-terminator is usually but not always present in scripts.
///
/// Returns the new terminator on success; empty string if not a SET TERM.
std::string detectSetTerm(std::string_view statement,
                          std::string_view /*currentTerm*/) {
    auto s = trimView(statement);
    if (s.size() < 3 || !iequalsView(s.substr(0, 3), "SET")) return {};
    s.remove_prefix(3);
    s = trimView(s);
    if (s.size() < 4 || !iequalsView(s.substr(0, 4), "TERM")) return {};
    s.remove_prefix(4);
    s = trimView(s);
    if (s.empty()) return {};
    // Take everything up to first whitespace as the new terminator. Anything
    // after (the trailing old-terminator) we simply ignore.
    const auto end = s.find_first_of(" \t\r\n");
    return (end == std::string_view::npos)
        ? std::string(s)
        : std::string(s.substr(0, end));
}

} // namespace

std::vector<std::string> splitIsqlScript(std::string_view script) {
    std::vector<std::string> out;
    std::string current;
    std::string term = ";";
    std::size_t i = 0;
    const std::size_t n = script.size();

    while (i < n) {
        const char c = script[i];

        // Line comment '-- ...\n'
        if (c == '-' && i + 1 < n && script[i + 1] == '-') {
            while (i < n && script[i] != '\n') ++i;
            continue;
        }
        // Block comment '/* ... */'
        if (c == '/' && i + 1 < n && script[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(script[i] == '*' && script[i + 1] == '/')) ++i;
            if (i + 1 < n) i += 2;
            continue;
        }
        // String literal '...'
        if (c == '\'') {
            current.push_back(c);
            ++i;
            while (i < n) {
                const char q = script[i];
                current.push_back(q);
                ++i;
                if (q == '\'') {
                    if (i < n && script[i] == '\'') {
                        // Doubled quote — stays inside the literal.
                        current.push_back('\'');
                        ++i;
                        continue;
                    }
                    break;
                }
            }
            continue;
        }
        // Terminator match
        if (script.compare(i, term.size(), term) == 0) {
            i += term.size();

            // Check whether the just-completed statement is a SET TERM.
            if (auto newTerm = detectSetTerm(current, term); !newTerm.empty()) {
                term = std::move(newTerm);
                current.clear();
                continue;
            }
            auto trimmed = trimView(current);
            if (!trimmed.empty()) out.emplace_back(trimmed);
            current.clear();
            continue;
        }
        current.push_back(c);
        ++i;
    }

    // Tail: allow unterminated final statement (tolerant of missing ';').
    auto tail = trimView(current);
    if (!tail.empty()) {
        if (auto newTerm = detectSetTerm(current, term); newTerm.empty()) {
            out.emplace_back(tail);
        }
    }
    return out;
}

} // namespace llmlog::core
