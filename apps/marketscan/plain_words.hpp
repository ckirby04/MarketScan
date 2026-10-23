#ifndef MARKETSCAN_APP_PLAIN_WORDS_HPP
#define MARKETSCAN_APP_PLAIN_WORDS_HPP

/*
 * Plain words for the Results' identifiers -- gate ids, warning codes, swept
 * parameter paths, objective metrics, scorecard sub-grades -- so the Research
 * view never shows a schema name.
 *
 * The words are contracts/research/plain_words.json, which the web app reads
 * too (web/src/lib/plain.ts): one file, so the two apps say the same thing.
 * This only looks them up, with the same fallbacks as the web: an identifier
 * the file does not know is shown with its underscores as spaces, and a
 * warning the file does not know is shown in the engine's own words.
 *
 * Words only. Formatting a parameter's value is the one thing here that
 * touches a number, and it only writes it out.
 */

#include <filesystem>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

namespace marketscan::app {

class PlainWords {
public:
    /// Empty: every lookup falls back. What the view uses when the file is missing.
    PlainWords() = default;

    /// Throws nlohmann::json::exception on a file not shaped like plain_words.json.
    explicit PlainWords(const nlohmann::json& words);

    [[nodiscard]] static PlainWords load(const std::filesystem::path& file);

    [[nodiscard]] std::string gate_name(const std::string& id) const;
    /// Empty when the file has none.
    [[nodiscard]] std::string gate_asks(const std::string& id) const;
    [[nodiscard]] std::string warning(const std::string& code, const std::string& message) const;
    [[nodiscard]] std::string param_label(const std::string& path) const;
    /// 20 -> "20%", 252 -> "252 days", 2.5 -> "2.5%".
    [[nodiscard]] std::string param_value(const std::string& path, double value) const;
    [[nodiscard]] std::string objective(const std::string& metric) const;
    [[nodiscard]] std::string subgrade_name(const std::string& id) const;
    [[nodiscard]] std::string subgrade_reads(const std::string& id) const;

private:
    struct Param {
        std::string label;
        std::string unit;
    };

    std::map<std::string, std::pair<std::string, std::string>> gates_;  // name, asks
    std::map<std::string, std::string> warnings_;
    std::map<std::string, Param> params_;
    std::map<std::string, std::string> objectives_;
    std::map<std::string, std::pair<std::string, std::string>> subgrades_;  // name, reads
};

/// A number without trailing zeros, to four places: 20 -> "20", 2.5 -> "2.5".
[[nodiscard]] std::string plain_number(double value);

}  // namespace marketscan::app

#endif  // MARKETSCAN_APP_PLAIN_WORDS_HPP
