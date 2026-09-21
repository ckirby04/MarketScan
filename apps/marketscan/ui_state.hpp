#ifndef MARKETSCAN_APP_UI_STATE_HPP
#define MARKETSCAN_APP_UI_STATE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "marketscan/core/types.hpp"

namespace marketscan::app {

class Session;
struct SessionSnapshot;

/// Everything the interface remembers that is not the engine's business.
///
/// Selection, filters and toggles live here rather than in the Session,
/// because none of them change what the run does. Which theory is highlighted
/// is a fact about the person looking, not about the market.
struct UiState {
    /// Which theory every panel focuses on. Empty means the first one.
    std::string selected_theory;

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
    bool equity_normalised = true;
    bool equity_show_benchmark = true;

    /// Keep the chart fitted to the data. On by default: a normalised curve
    /// sits close to 1.0, and an axis starting at zero flattens every theory
    /// into one line.
    bool equity_autofit = true;

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

    [[nodiscard]] bool is_hidden(const std::string& id) const;
    void toggle_series(const std::string& id);

    /// Index of the selected theory in the snapshot, or 0 when the selection
    /// is empty or names a theory that is no longer loaded.
    [[nodiscard]] std::size_t selected_index(const SessionSnapshot& snapshot) const;
};

}  // namespace marketscan::app

#endif  // MARKETSCAN_APP_UI_STATE_HPP
