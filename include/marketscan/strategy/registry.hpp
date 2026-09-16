#ifndef MARKETSCAN_STRATEGY_REGISTRY_HPP
#define MARKETSCAN_STRATEGY_REGISTRY_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "marketscan/strategy/istrategy.hpp"
#include "marketscan/strategy/plugin_loader.hpp"

namespace marketscan {

/// A strategy compiled into the host instead of loaded from a DLL.
struct BuiltinStrategy {
    std::string id;
    std::string display_name;
    std::string author;
    std::string description;

    /// Points into the strategy's own static storage, which outlives the
    /// process. Unlike a plugin's descriptor there is no boundary to copy it
    /// across, so it is pointed at rather than duplicated.
    const MS_ParamDesc* params = nullptr;
    std::uint32_t param_count = 0;

    std::vector<std::string> required_symbols;

    MS_PluginCreateFn create = nullptr;
};

/// Strategies linked directly into the host.
///
/// The theories/ folder is the primary way a strategy arrives, and nothing
/// here is needed to use it. This exists for the cases where a DLL is the
/// wrong shape: a strategy exercised by the test suite, one driven from the
/// Python bindings, or one deliberately built into a deployed binary so that
/// it cannot be swapped out.
///
/// Whichever way a strategy arrives, it ends up as the same PluginInstance
/// handed to the same StrategyInstance and driven by the same engine. There is
/// no second code path, which is the same reason there is no second code path
/// for backtest and live.
class StrategyRegistry {
public:
    [[nodiscard]] static StrategyRegistry& instance();

    /// Returns false if the id is already taken. Called during static
    /// initialisation by MARKETSCAN_REGISTER_STRATEGY.
    bool add(BuiltinStrategy entry);

    /// Sorted by id.
    ///
    /// Sorted rather than in registration order on purpose: registration
    /// happens during static initialisation, whose order across translation
    /// units the standard does not define. Leaving it alone would make the set
    /// of strategies -- and the order they receive events in -- depend on link
    /// order, which is exactly the kind of thing that makes a run irreproducible
    /// after an unrelated refactor.
    [[nodiscard]] const std::vector<BuiltinStrategy>& entries() const noexcept { return entries_; }

    [[nodiscard]] const BuiltinStrategy* find(std::string_view id) const noexcept;

    /// Instantiates a registered strategy. Invalid if the id is unknown or the
    /// constructor refused.
    [[nodiscard]] PluginInstance create(std::string_view id) const;

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    StrategyRegistry() = default;

    std::vector<BuiltinStrategy> entries_;
};

namespace detail {

template<typename T>
[[nodiscard]] BuiltinStrategy
builtin_entry(const char* id, const char* display_name, const char* author) {
    const MS_StrategyDesc desc = make_descriptor<T>(id, display_name, author);

    BuiltinStrategy entry;
    entry.id = desc.id != nullptr ? desc.id : "";
    entry.display_name = desc.display_name != nullptr ? desc.display_name : entry.id;
    entry.author = desc.author != nullptr ? desc.author : "";
    entry.description = desc.description != nullptr ? desc.description : "";
    entry.params = desc.params;
    entry.param_count = desc.param_count;
    for (std::uint32_t i = 0; i < desc.required_symbol_count; ++i) {
        if (desc.required_symbols != nullptr && desc.required_symbols[i] != nullptr) {
            entry.required_symbols.emplace_back(desc.required_symbols[i]);
        }
    }
    entry.create = &create_instance<T>;
    return entry;
}

}  // namespace detail

/// Registers a strategy that is compiled into the host.
///
/// Use MARKETSCAN_EXPORT_STRATEGY instead to build a strategy as a plugin --
/// that is the normal case. The two are not alternatives to each other so much
/// as answers to different questions: one is "how does this get into a DLL",
/// the other is "how does this get into this binary".
#define MARKETSCAN_REGISTER_STRATEGY(ClassName, IdLiteral, DisplayNameLiteral, AuthorLiteral)      \
    namespace {                                                                                    \
    const bool ClassName##_marketscan_registered = ::marketscan::StrategyRegistry::instance().add( \
        ::marketscan::detail::builtin_entry<ClassName>(                                            \
            IdLiteral, DisplayNameLiteral, AuthorLiteral));                                        \
    }

}  // namespace marketscan

#endif  // MARKETSCAN_STRATEGY_REGISTRY_HPP
