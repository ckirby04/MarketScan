#include "series_colors.hpp"

#include <algorithm>

namespace marketscan::app {

SeriesColors::SeriesColors(const SessionSnapshot& snapshot)
    : SeriesColors([&snapshot] {
          std::vector<std::pair<std::string, bool>> entities;
          entities.reserve(snapshot.strategies.size());
          for (const StrategySnapshot& row : snapshot.strategies) {
              entities.emplace_back(row.id, row.is_benchmark);
          }
          return entities;
      }()) {}

SeriesColors::SeriesColors(const std::vector<std::pair<std::string, bool>>& entities) {
    std::vector<std::string> ids;
    ids.reserve(entities.size());
    for (const auto& [id, is_benchmark] : entities) {
        if (is_benchmark) {
            // Held aside, not given a slot. Drawing the benchmark in a
            // categorical hue would put it in the same visual class as the
            // theories being compared against it.
            benchmark_id_ = id;
            continue;
        }
        ids.push_back(id);
    }

    // Sorted, so the assignment depends on the *set* of loaded theories and
    // nothing else -- not the order the loader happened to find the DLLs in,
    // not the order the leaderboard is sorted by, not which rows are visible.
    std::sort(ids.begin(), ids.end());

    for (std::string& id : ids) {
        if (order_.size() < Palette::kSeriesCount) {
            order_.push_back(std::move(id));
        } else {
            folded_.push_back(std::move(id));
        }
    }
}

std::size_t SeriesColors::slot(const std::string& id) const noexcept {
    for (std::size_t i = 0; i < order_.size(); ++i) {
        if (order_[i] == id) {
            return i;
        }
    }
    return kFolded;
}

bool SeriesColors::folded(const std::string& id) const noexcept {
    return slot(id) == kFolded && id != benchmark_id_;
}

unsigned int SeriesColors::benchmark_color() noexcept {
    // Its own named token, deliberately not one of the eight and deliberately
    // not the context gray -- see Palette::benchmark for what went wrong when it
    // was borrowed from the muted ink.
    return theme::colors().benchmark;
}

unsigned int SeriesColors::color(const std::string& id) const noexcept {
    if (!benchmark_id_.empty() && id == benchmark_id_) {
        return benchmark_color();
    }
    const std::size_t index = slot(id);
    if (index == kFolded) {
        return theme::colors().emphasis_gray;
    }
    return theme::colors().series[index];
}

}  // namespace marketscan::app
