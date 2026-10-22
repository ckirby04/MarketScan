#ifndef MARKETSCAN_APP_UI_STATE_HPP
#define MARKETSCAN_APP_UI_STATE_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "marketscan/core/types.hpp"

namespace marketscan::app {

class Session;
struct SessionSnapshot;

/// Everything the interface remembers that is not the engine's business.
///
/// Selection, filters and toggles live here rather than in the Session,
/// because none of them change what the run does. Which theory is highlighted
/// is a fact about the person looking, not about the market.
/// Which of the two presentations is showing.
///
/// Two presentations over one running engine, not a full version and a lite one.
/// Overview asks *is this working, and is it beating the market?*; Operator is
/// for watching and debugging a run. Both are live, both read the same published
/// snapshot, and switching between them never touches the engine.
enum class ViewMode : std::uint8_t {
    Overview = 0,
    Operator = 1,
    /// A question about a trading rule, answered by a graded job rather than a
    /// replay. SPEC_RESEARCH_VIEW.md. Reached with F3; F2 still toggles the
    /// other two.
    Research = 2,
};

[[nodiscard]] const char* to_string(ViewMode mode) noexcept;

struct UiState {
    /// Launches in Overview and remembers the last used mode thereafter.
    ///
    /// Overview first because the first thing anybody should see is the answer,
    /// not the instruments.
    ViewMode view_mode = ViewMode::Overview;

    /// Which theory every panel focuses on. Empty means the first one.
    std::string selected_theory;

    // --- Research ---
    /// The form being edited, and the session's form_version it was taken
    /// from. When the session's moves on -- a run was opened, the starter form
    /// was loaded -- the working copy is replaced.
    nlohmann::json research_form;
    std::uint64_t research_form_version = static_cast<std::uint64_t>(-1);

    /// Panel visibility, driven by the View menu.
    bool show_leaderboard = true;
    bool show_equity = true;
    bool show_drawdown = true;
    bool show_relative = true;
    bool show_correlation = true;
    bool show_blotter = true;
    bool show_positions = true;
    bool show_detail = true;
    bool show_universe = true;
    bool show_plugins = true;
    bool show_transport = true;
    bool show_latency = true;
    bool show_log = true;

    // --- Equity curves ---
    bool equity_log_scale = false;
    /// The Research view's equity chart. On by default: a backtest spans decades, and a
    /// crash early on flattens everything after it on a linear axis.
    bool research_log_scale = true;
    bool equity_normalised = true;
    bool equity_show_benchmark = true;

    /// Series the legend has switched off, by theory id.
    std::vector<std::string> hidden_series;

    // --- Log console ---
    LogLevel log_level = MS_LOG_INFO;
    bool log_autoscroll = true;
    /// Empty means every source.
    std::string log_source_filter;
    char log_search[128] = {};

    // --- Blotter ---
    char blotter_search[128] = {};
    bool blotter_selected_only = true;

    // --- Transport ---
    /// Index into the speed presets offered by the transport panel.
    int speed_index = 3;
    /// Where the user has dragged the scrub bar, 0..1. Applied on release, not
    /// while dragging: seeking rebuilds and replays the run, and doing that on
    /// every mouse-move would peg a core and make the bar feel glued.
    float scrub_target = 0.0F;
    bool scrubbing = false;

    /// Raised when the loader notices a theory changed on disk.
    bool reload_available = false;
    double last_disk_check = 0.0;

    /// Set by the menu, consumed by the app shell on the next frame.
    bool request_reset_layout = false;
    bool request_quit = false;

    /// Reads the remembered mode from `path`. A missing or unreadable file
    /// leaves the default, which is how a first run lands in Overview.
    void load(const std::filesystem::path& path);

    /// Writes it back. Beside the layout file, so the two travel together.
    void save(const std::filesystem::path& path) const;

    [[nodiscard]] bool is_hidden(const std::string& id) const;
    void toggle_series(const std::string& id);

    /// Index of the selected theory in the snapshot, or 0 when the selection
    /// is empty or names a theory that is no longer loaded.
    [[nodiscard]] std::size_t selected_index(const SessionSnapshot& snapshot) const;
};

}  // namespace marketscan::app

#endif  // MARKETSCAN_APP_UI_STATE_HPP
