#include "ui_state.hpp"

#include <algorithm>

#include "session.hpp"

namespace marketscan::app {

bool UiState::is_hidden(const std::string& id) const {
    return std::find(hidden_series.begin(), hidden_series.end(), id) != hidden_series.end();
}

void UiState::toggle_series(const std::string& id) {
    const auto it = std::find(hidden_series.begin(), hidden_series.end(), id);
    if (it == hidden_series.end()) {
        hidden_series.push_back(id);
    } else {
        hidden_series.erase(it);
    }
}

std::size_t UiState::selected_index(const SessionSnapshot& snapshot) const {
    if (snapshot.strategies.empty()) {
        return 0;
    }
    for (std::size_t i = 0; i < snapshot.strategies.size(); ++i) {
        if (snapshot.strategies[i].id == selected_theory) {
            return i;
        }
    }
    // The selection names a theory that has since been unloaded. Fall back to
    // the first rather than showing an empty panel with no explanation.
    return 0;
}

}  // namespace marketscan::app
