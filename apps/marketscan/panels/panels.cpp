#include "panels.hpp"

namespace marketscan::app::panels {

void draw_all(Context& context) {
    // Order matters only for the first frame, when ImGui assigns dock slots
    // to windows it has not seen before. After that the layout is whatever the
    // user made it, remembered in imgui.ini.
    leaderboard(context);
    equity_curves(context);
    drawdown(context);
    relative_strength(context);
    correlation_matrix(context);
    theory_detail(context);
    positions(context);
    trade_blotter(context);
    universe_inspector(context);
    plugin_manager(context);
    transport(context);
    latency(context);
    log_console(context);
}

}  // namespace marketscan::app::panels
