#ifndef MARKETSCAN_APP_MARKDOWN_HPP
#define MARKETSCAN_APP_MARKDOWN_HPP

/*
 * The little Markdown the assistant is allowed (contracts/research/assistant/
 * system_prompt.md, "Style"): paragraphs, **bold**, `code`, bullet and
 * numbered lists, and simple tables.
 *
 * The same rules as the web app's renderer (web/src/components/Markdown.tsx),
 * so a reply reads the same in both: a heading's #s are dropped and it reads as
 * a paragraph, a table's separator row is skipped, unmatched ** is left as
 * text. Parsed into blocks here, with no ImGui, so a test can reach it; the
 * panel draws the blocks.
 */

#include <string>
#include <string_view>
#include <vector>

namespace marketscan::app {

struct MdSpan {
    std::string text;
    bool bold = false;
    bool code = false;
};

using MdLine = std::vector<MdSpan>;

struct MdBlock {
    enum class Kind { Paragraph, Bullets, Numbers, Table };
    Kind kind = Kind::Paragraph;
    /// Paragraph: one entry. Lists: one per item. Table: unused.
    std::vector<MdLine> lines;
    /// Table: the first row is the header.
    std::vector<std::vector<MdLine>> rows;
};

[[nodiscard]] MdLine parse_inline(std::string_view text);
[[nodiscard]] std::vector<MdBlock> parse_markdown(std::string_view text);

}  // namespace marketscan::app

#endif  // MARKETSCAN_APP_MARKDOWN_HPP
