#include "marketscan/core/symbol_table.hpp"

namespace marketscan {
namespace {

constexpr const char* kEmpty = "";

}  // namespace

SymbolId SymbolTable::intern(std::string_view ticker) {
    if (ticker.empty()) {
        return kInvalidSymbol;
    }
    if (const auto it = index_.find(ticker); it != index_.end()) {
        return it->second;
    }

    names_.emplace_back(ticker);
    const auto id = static_cast<SymbolId>(names_.size());
    index_.emplace(names_.back(), id);
    return id;
}

SymbolId SymbolTable::find(std::string_view ticker) const noexcept {
    const auto it = index_.find(ticker);
    return it == index_.end() ? kInvalidSymbol : it->second;
}

std::string_view SymbolTable::name(SymbolId id) const noexcept {
    if (!contains(id)) {
        return {};
    }
    return names_[id - 1];
}

const char* SymbolTable::c_name(SymbolId id) const noexcept {
    if (!contains(id)) {
        return kEmpty;
    }
    return names_[id - 1].c_str();
}

std::vector<SymbolId> SymbolTable::ids() const {
    std::vector<SymbolId> result;
    result.reserve(names_.size());
    for (SymbolId id = 1; id <= count(); ++id) {
        result.push_back(id);
    }
    return result;
}

void SymbolTable::clear() noexcept {
    names_.clear();
    index_.clear();
}

}  // namespace marketscan
