#include "ui_state.hpp"

#include <algorithm>
#include <fstream>
#include <string>

#include "session.hpp"

namespace marketscan::app {

const char* to_string(ViewMode mode) noexcept {
    switch (mode) {
        case ViewMode::Overview:
            return "overview";
        case ViewMode::Operator:
            return "operator";
        case ViewMode::Research:
            return "research";
    }
    return "overview";
}

void UiState::load(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        return;  // First run. The default is Overview, deliberately.
    }
    std::string line;
    while (std::getline(file, line)) {
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        if (key == "view_mode") {
            view_mode = value == "operator"   ? ViewMode::Operator
                        : value == "research" ? ViewMode::Research
                                              : ViewMode::Overview;
        }
    }
}

void UiState::save(const std::filesystem::path& path) const {
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        return;  // Not being able to remember a preference is not worth failing over.
    }
    file << "# MarketScan interface state. Written on exit; edit freely.\n";
    file << "view_mode=" << to_string(view_mode) << "\n";
}

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
