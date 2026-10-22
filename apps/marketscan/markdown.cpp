#include "markdown.hpp"

#include <cctype>

namespace marketscan::app {

namespace {

[[nodiscard]] std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) {
        s.remove_suffix(1);
    }
    return s;
}

[[nodiscard]] std::string_view skip_spaces(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    return s;
}

/// "- x", "* x": the item's text after the marker, or false.
[[nodiscard]] bool bullet_item(std::string_view line, std::string_view& item) {
    const std::string_view s = skip_spaces(line);
    if (s.size() >= 2 && (s[0] == '-' || s[0] == '*') && s[1] == ' ') {
        item = s.substr(2);
        return true;
    }
    return false;
}

/// "1. x", "12) x".
[[nodiscard]] bool number_item(std::string_view line, std::string_view& item) {
    const std::string_view s = skip_spaces(line);
    std::size_t digits = 0;
    while (digits < s.size() && std::isdigit(static_cast<unsigned char>(s[digits])) != 0) {
        ++digits;
    }
    if (digits > 0 && digits + 1 < s.size() && (s[digits] == '.' || s[digits] == ')') &&
        s[digits + 1] == ' ') {
        item = s.substr(digits + 2);
        return true;
    }
    return false;
}

[[nodiscard]] bool table_line(std::string_view line) {
    const std::string_view s = trim(line);
    return !s.empty() && s.front() == '|';
}

/// "|---|:--:|": every character a pipe, dash, colon or space.
[[nodiscard]] bool separator_row(std::string_view line) {
    const std::string_view s = trim(line);
    if (s.empty()) {
        return false;
    }
    bool dash = false;
    for (const char c : s) {
        if (c == '-') {
            dash = true;
        } else if (c != '|' && c != ':' && c != ' ' && c != '\t') {
            return false;
        }
    }
    return dash;
}

[[nodiscard]] std::vector<MdLine> cells(std::string_view row) {
    std::string_view s = trim(row);
    if (!s.empty() && s.front() == '|') {
        s.remove_prefix(1);
    }
    if (!s.empty() && s.back() == '|') {
        s.remove_suffix(1);
    }
    std::vector<MdLine> out;
    for (;;) {
        const auto bar = s.find('|');
        out.push_back(parse_inline(trim(s.substr(0, bar))));
        if (bar == std::string_view::npos) {
            break;
        }
        s.remove_prefix(bar + 1);
    }
    return out;
}

[[nodiscard]] std::string_view drop_heading(std::string_view line) {
    std::size_t hashes = 0;
    while (hashes < line.size() && line[hashes] == '#') {
        ++hashes;
    }
    return hashes == 0 ? line : skip_spaces(line.substr(hashes));
}

}  // namespace

MdLine parse_inline(std::string_view text) {
    MdLine out;
    std::string plain;
    const auto flush = [&] {
        if (!plain.empty()) {
            out.push_back(MdSpan{plain, false, false});
            plain.clear();
        }
    };
    std::size_t i = 0;
    while (i < text.size()) {
        // **bold**: non-empty, with no * inside, as the web's pattern.
        if (text.compare(i, 2, "**") == 0) {
            const auto close = text.find("**", i + 2);
            if (close != std::string_view::npos && close > i + 2) {
                const std::string_view inner = text.substr(i + 2, close - i - 2);
                if (inner.find('*') == std::string_view::npos) {
                    flush();
                    out.push_back(MdSpan{std::string(inner), true, false});
                    i = close + 2;
                    continue;
                }
            }
        }
        if (text[i] == '`') {
            const auto close = text.find('`', i + 1);
            if (close != std::string_view::npos && close > i + 1) {
                flush();
                out.push_back(MdSpan{std::string(text.substr(i + 1, close - i - 1)), false, true});
                i = close + 1;
                continue;
            }
        }
        plain += text[i];
        ++i;
    }
    flush();
    return out;
}

std::vector<MdBlock> parse_markdown(std::string_view text) {
    std::vector<std::string_view> lines;
    for (;;) {
        const auto newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        lines.push_back(line);
        if (newline == std::string_view::npos) {
            break;
        }
        text.remove_prefix(newline + 1);
    }

    std::vector<MdBlock> blocks;
    std::size_t i = 0;
    std::string_view item;
    while (i < lines.size()) {
        if (trim(lines[i]).empty()) {
            ++i;
            continue;
        }
        if (bullet_item(lines[i], item) || number_item(lines[i], item)) {
            MdBlock block;
            block.kind =
                number_item(lines[i], item) ? MdBlock::Kind::Numbers : MdBlock::Kind::Bullets;
            while (i < lines.size() &&
                   (bullet_item(lines[i], item) || number_item(lines[i], item))) {
                block.lines.push_back(parse_inline(item));
                ++i;
            }
            blocks.push_back(std::move(block));
            continue;
        }
        if (table_line(lines[i])) {
            MdBlock block;
            block.kind = MdBlock::Kind::Table;
            while (i < lines.size() && table_line(lines[i])) {
                if (!separator_row(lines[i])) {
                    block.rows.push_back(cells(lines[i]));
                }
                ++i;
            }
            if (!block.rows.empty()) {
                blocks.push_back(std::move(block));
            }
            continue;
        }
        std::string paragraph;
        while (i < lines.size() && !trim(lines[i]).empty() && !bullet_item(lines[i], item) &&
               !number_item(lines[i], item) && !table_line(lines[i])) {
            if (!paragraph.empty()) {
                paragraph += ' ';
            }
            paragraph += drop_heading(lines[i]);
            ++i;
        }
        MdBlock block;
        block.lines.push_back(parse_inline(paragraph));
        blocks.push_back(std::move(block));
    }
    return blocks;
}

}  // namespace marketscan::app
