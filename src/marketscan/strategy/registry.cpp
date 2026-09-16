#include "marketscan/strategy/registry.hpp"

#include <algorithm>
#include <utility>

namespace marketscan {

StrategyRegistry& StrategyRegistry::instance() {
    // Function-local, so it exists before the first MARKETSCAN_REGISTER_STRATEGY
    // runs regardless of static initialisation order across translation units.
    static StrategyRegistry registry;
    return registry;
}

bool StrategyRegistry::add(BuiltinStrategy entry) {
    if (entry.id.empty() || entry.create == nullptr) {
        return false;
    }
    if (find(entry.id) != nullptr) {
        return false;
    }

    const auto position =
        std::lower_bound(entries_.begin(),
                         entries_.end(),
                         entry.id,
                         [](const BuiltinStrategy& a, const std::string& id) { return a.id < id; });
    entries_.insert(position, std::move(entry));
    return true;
}

const BuiltinStrategy* StrategyRegistry::find(std::string_view id) const noexcept {
    for (const BuiltinStrategy& entry : entries_) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

PluginInstance StrategyRegistry::create(std::string_view id) const {
    const BuiltinStrategy* entry = find(id);
    if (entry == nullptr || entry->create == nullptr) {
        return {};
    }

    MS_StrategyVTable vtable{};
    void* self = entry->create(&vtable);
    if (self == nullptr || vtable.destroy == nullptr) {
        // The constructor refused, or the vtable has no way to free the object.
        // Either way there is nothing safe to hand back.
        return {};
    }
    return PluginInstance::adopt(self, vtable, entry->id);
}

}  // namespace marketscan
