// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>
#include "frontend/ini_rewrite.h"

namespace {
struct Line {
    std::string_view body;   // without the line ending
    std::string_view ending; // "\n", "\r\n" or "" on a last line without one
};

std::vector<Line> SplitLines(std::string_view text) {
    std::vector<Line> lines;
    while (!text.empty()) {
        const size_t newline = text.find('\n');
        if (newline == std::string_view::npos) {
            lines.push_back({text, {}});
            break;
        }
        const bool crlf = newline > 0 && text[newline - 1] == '\r';
        lines.push_back({text.substr(0, crlf ? newline - 1 : newline),
                         text.substr(crlf ? newline - 1 : newline, crlf ? 2 : 1)});
        text.remove_prefix(newline + 1);
    }
    return lines;
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

bool IsComment(std::string_view body) {
    const auto t = Trim(body);
    return t.starts_with(';') || t.starts_with('#');
}

/// "[name]" -> name; empty optional otherwise.
std::optional<std::string_view> SectionName(std::string_view body) {
    const auto t = Trim(body);
    if (t.size() < 2 || t.front() != '[' || t.back() != ']') return std::nullopt;
    return Trim(t.substr(1, t.size() - 2));
}

/// For "key = value": the key, and where the value starts and ends in `body`.
struct KeyLine {
    std::string_view key;
    size_t value_begin;
    size_t value_end;
};
std::optional<KeyLine> ParseKeyLine(std::string_view body) {
    if (IsComment(body)) return std::nullopt;
    const size_t equals = body.find('=');
    if (equals == std::string_view::npos) return std::nullopt;
    const auto key = Trim(body.substr(0, equals));
    if (key.empty()) return std::nullopt;
    size_t begin = equals + 1;
    while (begin < body.size() && (body[begin] == ' ' || body[begin] == '\t')) ++begin;
    size_t end = body.size();
    while (end > begin && (body[end - 1] == ' ' || body[end - 1] == '\t')) --end;
    return KeyLine{key, begin, end};
}

std::string_view DominantEnding(const std::vector<Line>& lines) {
    for (const auto& line : lines) {
        if (!line.ending.empty()) return line.ending;
    }
    return "\n";
}
} // namespace

std::string RewriteIni(std::string_view text, std::span<const IniChange> changes) {
    const std::vector<Line> lines = SplitLines(text);
    const std::string_view eol = DominantEnding(lines);
    std::vector<std::string> out_bodies;
    std::vector<std::string> out_endings;
    std::vector<bool> applied(changes.size(), false);

    // Pass 1: replace values in place; remember the last content line of each section.
    std::string current;
    std::vector<std::pair<std::string, size_t>> last_content; // section -> index in out
    for (const auto& line : lines) {
        std::string body(line.body);
        if (const auto name = SectionName(line.body)) {
            current = std::string(*name);
            last_content.emplace_back(current, out_bodies.size());
        } else if (const auto kv = ParseKeyLine(line.body)) {
            for (size_t i = 0; i < changes.size(); ++i) {
                if (changes[i].section == current && changes[i].key == kv->key) {
                    body = std::string(line.body.substr(0, kv->value_begin)) + changes[i].value +
                           std::string(line.body.substr(kv->value_end));
                    applied[i] = true;
                }
            }
        }
        if (!Trim(line.body).empty() && !last_content.empty() && last_content.back().first == current &&
            !SectionName(line.body)) {
            last_content.back().second = out_bodies.size();
        }
        out_bodies.push_back(std::move(body));
        out_endings.emplace_back(line.ending);
    }

    // Pass 2: insert missing keys after their section's last content line,
    // from the bottom up so earlier indices stay valid.
    std::vector<std::string> new_sections;
    std::vector<std::vector<size_t>> pending(last_content.size());
    for (size_t i = 0; i < changes.size(); ++i) {
        if (applied[i]) continue;
        size_t found = last_content.size();
        for (size_t s = 0; s < last_content.size(); ++s) {
            if (last_content[s].first == changes[i].section) found = s; // last occurrence wins
        }
        if (found < last_content.size()) {
            pending[found].push_back(i);
        } else {
            new_sections.push_back(changes[i].section);
        }
    }
    for (size_t s = last_content.size(); s-- > 0;) {
        if (pending[s].empty()) continue;
        size_t at = last_content[s].second;
        if (out_endings[at].empty()) out_endings[at] = std::string(eol);
        for (const size_t i : pending[s]) {
            ++at;
            out_bodies.insert(out_bodies.begin() + at, changes[i].key + " = " + changes[i].value);
            out_endings.insert(out_endings.begin() + at, std::string(eol));
        }
    }

    std::string result;
    for (size_t i = 0; i < out_bodies.size(); ++i) {
        result += out_bodies[i];
        result += out_endings[i];
    }
    // Pass 3: append wholly missing sections.
    std::vector<std::string> done;
    for (const auto& section : new_sections) {
        if (std::find(done.begin(), done.end(), section) != done.end()) continue;
        done.push_back(section);
        if (!result.empty() && !result.ends_with('\n')) result += eol;
        if (!result.empty()) result += eol;
        result += "[" + section + "]" + std::string(eol);
        for (size_t i = 0; i < changes.size(); ++i) {
            if (!applied[i] && changes[i].section == section) {
                result += changes[i].key + " = " + changes[i].value + std::string(eol);
            }
        }
    }
    return result;
}

bool SaveIniChanges(const std::filesystem::path& file, std::span<const IniChange> changes,
                    std::string_view template_text, std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string text(template_text);
    if (fs::exists(file, ec)) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            error = "cannot read " + file.string();
            return false;
        }
        std::stringstream buffer;
        buffer << in.rdbuf();
        text = buffer.str();
    }
    fs::create_directories(file.parent_path(), ec);
    const fs::path temp = file.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write " + temp.string();
            return false;
        }
        out << RewriteIni(text, changes);
        out.flush();
        if (!out) {
            out.close();
            fs::remove(temp, ec);
            error = "cannot write " + temp.string();
            return false;
        }
    }
    fs::rename(temp, file, ec);
    if (ec) {
        fs::remove(temp, ec);
        error = "cannot replace " + file.string() + ": " + ec.message();
        return false;
    }
    return true;
}

std::filesystem::path AcreIniSavePath(const std::optional<std::filesystem::path>& found) {
    if (found) return *found;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "astralchain" / "astralchain.ini";
    }
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : ".") / ".config" / "astralchain" / "astralchain.ini";
}
