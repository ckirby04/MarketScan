#include "marketscan/data/security_master.hpp"

#include <algorithm>
#include <stdexcept>

namespace marketscan {

std::string_view Security::ticker_at(Timestamp ts) const noexcept {
    if (tickers.empty()) {
        return {};
    }
    for (const TickerPeriod& period : tickers) {
        if (period.covers(ts)) {
            return period.ticker;
        }
    }
    // Outside every period. Clamped rather than empty: this is asked by the
    // blotter, by the log and by the GUI, and a row that displays nothing is
    // less useful than one that displays the nearest name it had.
    if (ts < tickers.front().from) {
        return tickers.front().ticker;
    }
    return tickers.back().ticker;
}

std::string SecurityMaster::intern_key(std::string_view ticker, std::size_t incarnation) {
    std::string key(ticker);
    if (incarnation > 0) {
        // The tilde is deliberate: no ticker contains one, so a key can never
        // collide with a real ticker no matter how many incarnations there are.
        key += '~';
        key += std::to_string(incarnation + 1);
    }
    return key;
}

SecurityId SecurityMaster::declare(std::string_view ticker,
                                   Timestamp from,
                                   Timestamp until,
                                   std::string reason) {
    if (ticker.empty()) {
        throw std::invalid_argument("SecurityMaster::declare: empty ticker");
    }
    if (until <= from) {
        throw std::invalid_argument("SecurityMaster::declare: " + std::string(ticker) +
                                    " has a period that ends before it begins");
    }

    const auto existing = by_ticker_.find(ticker);
    const bool first_time = existing == by_ticker_.end();

    // An empty reason means "the same company as last time under this ticker",
    // which is the ordinary case: a membership interval being extended, or a
    // second file for a symbol already known.
    if (!first_time && reason.empty()) {
        const SecurityId id = existing->second.back();
        Security& security = securities_[id - 1];

        // Extend the last period if this abuts or overlaps it, rather than
        // accumulating adjacent fragments -- a security with four hundred
        // one-day ticker periods is technically correct and useless to read.
        TickerPeriod& last = security.tickers.back();
        if (last.ticker == ticker && from <= last.until) {
            last.from = std::min(last.from, from);
            last.until = std::max(last.until, until);
            return id;
        }
        if (last.ticker == ticker) {
            // A gap the caller did not call a split. Recorded as a separate
            // period of the same security: this is a name that left the index
            // and came back, which is not the same thing as a name that was
            // reassigned.
            security.tickers.push_back(TickerPeriod{std::string(ticker), from, until});
            return id;
        }
        // A different ticker on a security we already know: a rename. The id is
        // unchanged, which is the whole point of the id.
        security.tickers.push_back(TickerPeriod{std::string(ticker), from, until});
        std::sort(security.tickers.begin(),
                  security.tickers.end(),
                  [](const TickerPeriod& a, const TickerPeriod& b) { return a.from < b.from; });
        return id;
    }

    // A new security: either the ticker has never been seen, or the caller has
    // decided this is a different company holding the same string.
    const std::size_t incarnation = first_time ? 0 : existing->second.size();
    const SecurityId id = symbols_->intern(intern_key(ticker, incarnation));

    if (id != static_cast<SecurityId>(securities_.size() + 1)) {
        // The symbol table was interned into behind this master's back, so ids
        // and slots have drifted apart. Everything downstream indexes by id, so
        // this is not a thing to recover from quietly.
        throw std::runtime_error(
            "SecurityMaster: the symbol table was interned into directly; ids and securities are "
            "out of step. Every ticker in a run with a security master must be resolved through "
            "it.");
    }

    Security security;
    security.id = id;
    security.tickers.push_back(TickerPeriod{std::string(ticker), from, until});
    security.split_reason = std::move(reason);
    securities_.push_back(std::move(security));

    by_ticker_[std::string(ticker)].push_back(id);
    return id;
}

void SecurityMaster::add_ticker(SecurityId id,
                                std::string_view ticker,
                                Timestamp from,
                                Timestamp until) {
    if (id == kInvalidSecurity || id > securities_.size()) {
        throw std::invalid_argument("SecurityMaster::add_ticker: no such security");
    }
    if (ticker.empty() || until <= from) {
        throw std::invalid_argument("SecurityMaster::add_ticker: empty ticker or inverted period");
    }

    // A ticker already held by a different security would make resolution
    // ambiguous on any date the two periods overlapped, and the resolution is
    // the whole product. Refused rather than resolved by a tie-break.
    if (const auto entry = by_ticker_.find(ticker); entry != by_ticker_.end()) {
        for (const SecurityId other : entry->second) {
            if (other == id) {
                continue;
            }
            for (const TickerPeriod& period : securities_[other - 1].tickers) {
                if (period.ticker == ticker && from < period.until && period.from < until) {
                    throw std::invalid_argument(
                        "SecurityMaster::add_ticker: '" + std::string(ticker) +
                        "' already belongs to another security over an overlapping period");
                }
            }
        }
    }

    Security& security = securities_[id - 1];
    security.tickers.push_back(TickerPeriod{std::string(ticker), from, until});
    std::sort(security.tickers.begin(),
              security.tickers.end(),
              [](const TickerPeriod& a, const TickerPeriod& b) { return a.from < b.from; });

    std::vector<SecurityId>& holders = by_ticker_[std::string(ticker)];
    if (std::find(holders.begin(), holders.end(), id) == holders.end()) {
        holders.push_back(id);
    }
}

SecurityId SecurityMaster::resolve(std::string_view ticker, Timestamp as_of) const noexcept {
    const auto entry = by_ticker_.find(ticker);
    if (entry == by_ticker_.end()) {
        return kInvalidSecurity;
    }

    // In declaration order, which is chronological. The first security whose
    // period covers the date is the answer, and there is at most one: periods
    // for a ticker never overlap, which is what the split heuristic guarantees.
    for (const SecurityId id : entry->second) {
        const Security& security = securities_[id - 1];
        for (const TickerPeriod& period : security.tickers) {
            if (period.ticker == ticker && period.covers(as_of)) {
                return id;
            }
        }
    }
    return kInvalidSecurity;
}

const Security* SecurityMaster::find(SecurityId id) const noexcept {
    if (id == kInvalidSecurity || id > securities_.size()) {
        return nullptr;
    }
    return &securities_[id - 1];
}

std::string_view SecurityMaster::ticker_at(SecurityId id, Timestamp ts) const noexcept {
    const Security* security = find(id);
    return security != nullptr ? security->ticker_at(ts) : std::string_view{};
}

std::size_t SecurityMaster::incarnations(std::string_view ticker) const noexcept {
    const auto entry = by_ticker_.find(ticker);
    return entry == by_ticker_.end() ? 0 : entry->second.size();
}

}  // namespace marketscan
