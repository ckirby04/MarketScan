#include "marketscan/strategy/strategy_instance.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "marketscan/util/logging.hpp"

namespace marketscan {

StrategyInstance::StrategyInstance(std::string id,
                                   std::string display_name,
                                   PluginInstance plugin,
                                   std::vector<std::string> required_symbols,
                                   StrategyRuntimeConfig config,
                                   const IFillModel* fill_model,
                                   const MarketView* market,
                                   SymbolTable* symbols,
                                   const IClock* clock)
    : id_(std::move(id)),
      display_name_(std::move(display_name)),
      plugin_(std::move(plugin)),
      required_symbols_(std::move(required_symbols)),
      config_(config),
      market_(market),
      symbols_(symbols),
      clock_(clock),
      broker_(config.starting_cash, fill_model, market, config.order_id_base),
      host_(this),
      // Derived from the run seed and this strategy's id, so that the numbers
      // one theory draws do not depend on which other theories are loaded.
      rng_(derive_seed(config.run_seed, id_)) {
    broker_.set_fill_listener(this);
    // The strategy's own stream, not one of the broker's. Latency jitter is a
    // draw like any other and belongs in the same reproducible sequence; a
    // broker with a private generator would be a second source of randomness
    // that nobody chose a seed for.
    broker_.set_jitter_source(&rng_);
    broker_.set_exchange_zone(config.exchange_zone);
    broker_.ledger().set_equity_sample_interval(config_.equity_sample_interval);
    resolve_subscriptions();
}

StrategyInstance::~StrategyInstance() {
    // Detach before the plugin can be destroyed: its on_stop may call back in,
    // and by then this object is partway through dying.
    stop();
}

void StrategyInstance::resolve_subscriptions() {
    subscribed_.clear();
    if (required_symbols_.empty() || symbols_ == nullptr) {
        return;  // empty means "every symbol"
    }

    std::vector<std::string> missing;
    for (const std::string& ticker : required_symbols_) {
        const SymbolId id = symbols_->find(ticker);
        if (id == kInvalidSymbol) {
            missing.push_back(ticker);
        } else if (std::find(subscribed_.begin(), subscribed_.end(), id) == subscribed_.end()) {
            subscribed_.push_back(id);
        }
    }
    std::sort(subscribed_.begin(), subscribed_.end());

    // Said once, not on every event: a theory asking for an instrument the
    // data does not contain will simply never trade, and that is worth being
    // told about rather than left to work out from an empty blotter.
    if (!missing.empty() && !warned_missing_symbols_) {
        warned_missing_symbols_ = true;
        std::string message = "required symbol(s) not present in this dataset:";
        for (const std::string& ticker : missing) {
            message += " " + ticker;
        }
        log::warn(id_, message);
    }
}

bool StrategyInstance::visible(SymbolId symbol, Timestamp ts) const noexcept {
    // No universe installed: every symbol in the data is in scope. That is the
    // intraday and single-instrument case, and --no-universe.
    if (universe_ == nullptr) {
        return true;
    }
    return universe_->is_member(symbol, ts);
}

bool StrategyInstance::wants(SymbolId symbol) const noexcept {
    if (subscribed_.empty()) {
        return true;
    }
    return std::find(subscribed_.begin(), subscribed_.end(), symbol) != subscribed_.end();
}

void StrategyInstance::start() {
    if (started_) {
        return;
    }
    started_ = true;
    stopped_ = false;
    plugin_.start(host_.abi());
}

void StrategyInstance::stop() {
    if (!started_ || stopped_) {
        return;
    }
    stopped_ = true;
    plugin_.stop();
}

void StrategyInstance::on_bar(const MS_Bar& bar) {
    if (started_ && !stopped_ && !visible(bar.symbol, bar.ts)) {
        ++events_withheld_;
        return;
    }
    if (started_ && !stopped_ && wants(bar.symbol)) {
        // Measured around the callback, which is where an order is submitted,
        // priced and booked -- so this is event-in to order-out, not just
        // dispatch overhead.
        const ScopedLatency timer(latency_);
        plugin_.bar(bar);
        // After the callback, and that ordering is the whole point. A
        // cross-sectional strategy is handed session N's cross-section from
        // inside the dispatch of session N+1's first bar; crediting it with N+1's
        // bar there would say it knew a price it had not been shown. See the
        // lookahead probe in strategy_instance.hpp.
        note_shown(bar.symbol, bar.ts);
    }
}

void StrategyInstance::on_quote(const MS_Quote& quote) {
    if (started_ && !stopped_ && !visible(quote.symbol, quote.ts)) {
        ++events_withheld_;
        return;
    }
    if (started_ && !stopped_ && wants(quote.symbol)) {
        // Measured around the callback, which is where an order is submitted,
        // priced and booked -- so this is event-in to order-out, not just
        // dispatch overhead.
        const ScopedLatency timer(latency_);
        plugin_.quote(quote);
        // After the callback: an order submitted inside it was decided on what
        // the strategy knew before this quote. See the lookahead probe.
        note_shown(quote.symbol, quote.ts);
    }
}

void StrategyInstance::on_trade(const MS_Trade& trade) {
    if (started_ && !stopped_ && !visible(trade.symbol, trade.ts)) {
        ++events_withheld_;
        return;
    }
    if (started_ && !stopped_ && wants(trade.symbol)) {
        // Measured around the callback, which is where an order is submitted,
        // priced and booked -- so this is event-in to order-out, not just
        // dispatch overhead.
        const ScopedLatency timer(latency_);
        plugin_.trade(trade);
        note_shown(trade.symbol, trade.ts);
    }
}

void StrategyInstance::on_event_complete(const MarketEvent& event) {
    const Timestamp ts = event.ts();

    // Resting orders are retried here rather than inside the callback above,
    // so they never fill against a book that other strategies have not yet
    // been shown.
    broker_.on_market_event(event.symbol(), ts);

    if (market_ != nullptr) {
        broker_.ledger().mark_all(*market_);
    }

    // After the marks, so a forced exit prices against the book as of this
    // event rather than the previous one.
    sweep_removals(ts);

    broker_.ledger().maybe_sample_equity(ts);
}

void StrategyInstance::sweep_removals(Timestamp ts) {
    if (universe_ == nullptr) {
        return;
    }

    // Only when the membership set actually changes. A replay asks this on
    // every event and the answer changes a handful of times a year, so the
    // comparison below is what keeps this off the hot path.
    if (last_membership_ts_ != kNever) {
        const std::vector<SymbolId>& members = universe_->members_on(ts);
        if (members == last_members_) {
            return;
        }
        last_members_ = members;
    } else {
        last_members_ = universe_->members_on(ts);
        last_membership_ts_ = ts;
        return;
    }
    last_membership_ts_ = ts;

    // Anything held that is no longer a member is sold at the last price the
    // book has for it, and then -- under `apply_true_return` -- the delisting
    // return the data supplies is booked against the position that was closed.
    //
    // Liquidating at the last price alone is an OPTIMISTIC APPROXIMATION and it
    // is the single largest thing this data layer gets wrong. A name leaves an
    // index for two very different reasons: it was replaced by a bigger one, or
    // it collapsed. The first liquidates near the last price; the second does not
    // liquidate anywhere near it, and the real delisting return is frequently
    // -30%, -60%, or everything. Modelling both as the first is precisely where
    // naive backtests hide their losses.
    //
    // `DelistingReturnModel` names that assumption instead of leaving it
    // implicit, and `last_price` remains the default so that no committed result
    // moved when the switch appeared. When CRSP delisting returns arrive, loading
    // them into a `DelistingReturns` and selecting `apply_true_return` is the
    // whole of the change.
    //
    // The position is captured BY VALUE before the exit runs. Two reasons, and
    // both have teeth: `open_positions` hands out pointers into a vector the
    // fill is about to write through, and the delisting return has to be measured
    // against the position as it stood when the security left rather than against
    // whatever survived the exit.
    std::vector<const Position*> held = broker_.ledger().open_positions();
    std::vector<Position> departing;
    for (const Position* position : held) {
        if (position == nullptr || position->quantity == 0.0) {
            continue;
        }
        if (universe_->is_member(position->symbol, ts)) {
            continue;
        }
        departing.push_back(*position);
    }

    for (const Position& position : departing) {
        MS_OrderRequest exit{};
        exit.symbol = position.symbol;
        exit.side = position.quantity > 0.0 ? MS_SIDE_SELL : MS_SIDE_BUY;
        exit.type = MS_ORD_MARKET;
        exit.tif = MS_TIF_IOC;
        exit.quantity = std::fabs(position.quantity);
        exit.client_order_id = kForcedExitClientId;

        const OrderId id = broker_.submit(exit, ts);
        ++forced_exits_;

        std::string message = "forced exit: ";
        message += symbols_ != nullptr ? symbols_->c_name(position.symbol) : "?";
        message += " left the universe";
        if (id == kInvalidOrderId) {
            message += "; the exit order was REFUSED and the position is stranded";
            log::write(MS_LOG_ERROR, id_, message);
        } else {
            log::write(MS_LOG_INFO, id_, message);
        }

        // After the exit, not before. Applying it first can take cash below zero
        // on a long about to be sold -- the sale that pays for the loss has not
        // happened yet -- and the broker would then refuse the exit and strand a
        // position over an ordering choice. Booked even when the exit was refused,
        // because a stranded position in a name that collapsed is still worth
        // less, and pretending otherwise is the flattery this switch exists to
        // measure.
        if (delisting_model_ != DelistingReturnModel::ApplyTrueReturn ||
            delisting_returns_ == nullptr || !delisting_returns_->has(position.symbol)) {
            continue;
        }

        const double applied = delisting_returns_->find(position.symbol);
        const DelistingAdjustment& adjustment = broker_.ledger().apply_delisting_return(
            ts, position.symbol, position.quantity, position.mark, applied);
        ++delistings_applied_;

        std::string note = "delisting return ";
        note += std::to_string(applied);
        note += " applied to ";
        note += symbols_ != nullptr ? symbols_->c_name(position.symbol) : "?";
        note += ": ";
        note += std::to_string(adjustment.amount.micros());
        note += " micro-dollars";
        log::write(MS_LOG_INFO, id_, note);
    }
}

void StrategyInstance::on_fill(const MS_Fill& fill) {
    if (started_ && !stopped_) {
        plugin_.fill(fill);
    }
}

void StrategyInstance::on_order_status(const MS_OrderStatus& status) {
    if (!started_ || stopped_) {
        return;
    }
    // Counted before the delivery attempt, not after: the count is of events
    // the host produced. Whether the plugin has a slot to receive them is
    // PluginInstance's business, and a v1 theory legitimately has none.
    ++order_status_events_;
    plugin_.order_status(status);
}

// --- HostServices ----------------------------------------------------------

std::uint64_t StrategyInstance::submit_order(const MS_OrderRequest& request) {
    return broker_.submit(request, now(), shown_ts(request.symbol));
}

StrategyInstance::InstantAudit StrategyInstance::audit_instants() const {
    InstantAudit out;
    for (const TradeRecord& row : broker_.ledger().blotter()) {
        if (row.client_order_id == kForcedExitClientId) {
            ++out.forced_exits;
        } else if (row.decision_ts == kNever || row.price_ts == kNever) {
            ++out.unrecorded;
        } else if (row.price_ts > row.decision_ts) {
            ++out.honest;
        } else if (row.price_ts == row.decision_ts) {
            ++out.simultaneous;
        } else {
            ++out.stale;
        }
    }
    return out;
}

Timestamp StrategyInstance::shown_ts(SymbolId symbol) const noexcept {
    const auto index = static_cast<std::size_t>(symbol);
    if (symbol == kInvalidSymbol || index > shown_.size()) {
        return kNever;
    }
    return shown_[index - 1];
}

void StrategyInstance::note_shown(SymbolId symbol, Timestamp ts) {
    if (symbol == kInvalidSymbol) {
        return;
    }
    const auto index = static_cast<std::size_t>(symbol);
    if (index > shown_.size()) {
        shown_.resize(index, kNever);
    }
    shown_[index - 1] = ts;
}

void StrategyInstance::cancel_order(std::uint64_t order_id) {
    (void)broker_.cancel(order_id, now());
}

Timestamp StrategyInstance::now() const {
    return clock_ != nullptr ? clock_->now() : 0;
}

double StrategyInstance::random_uniform() {
    return rng_.uniform();
}

double StrategyInstance::position(SymbolId symbol) const {
    // A non-member reads as flat. It always is -- anything held is liquidated
    // on the day it leaves the index -- so this closes a probe rather than
    // hiding a balance: without it, a strategy could ask about a name it was
    // never shown and learn that it exists.
    if (!visible(symbol, now())) {
        return 0.0;
    }
    return broker_.ledger().position(symbol);
}

Money StrategyInstance::cash() const {
    return broker_.ledger().cash();
}

Money StrategyInstance::equity() const {
    return broker_.ledger().equity();
}

SymbolId StrategyInstance::symbol_id(const char* ticker) const {
    if (symbols_ == nullptr || ticker == nullptr) {
        return kInvalidSymbol;
    }

    // Resolved as of the simulation clock, which is the correct semantics and
    // is available without touching the frozen ABI: symbol_id has no date
    // parameter and cannot gain one, but the host knows what time it is.
    //
    // A consequence worth knowing, and documented in theories/README.md: the
    // id returned for one ticker CAN differ across a long replay, if that
    // ticker was reassigned to a different company. Resolve once and hold the
    // id; do not re-resolve a string every bar.
    if (master_ != nullptr) {
        const SecurityId id = master_->resolve(ticker, now());
        return visible(id, now()) ? id : kInvalidSymbol;
    }

    // Lookup, never interning. A strategy inventing symbols would grow the
    // table mid-run and change the ids a later run assigns.
    const SymbolId id = symbols_->find(ticker);
    return visible(id, now()) ? id : kInvalidSymbol;
}

const char* StrategyInstance::symbol_name(SymbolId symbol) const {
    if (symbols_ == nullptr) {
        return "";
    }
    // The interned key is "XYZ~2" for a second incarnation of a reused ticker.
    // That is an internal detail and no strategy should ever see it, so the
    // display name comes from the master when there is one.
    if (master_ != nullptr) {
        if (const Security* security = master_->find(symbol); security != nullptr) {
            return security->ticker_at(now()).data();
        }
    }
    return symbols_->c_name(symbol);
}

std::uint32_t StrategyInstance::symbol_count() const {
    if (universe_ != nullptr) {
        // Members today, not everything the data file happens to contain. A
        // strategy iterating 0..symbol_count() must not be able to walk off the
        // end of the index into a company that was not in it.
        return static_cast<std::uint32_t>(universe_->members_on(now()).size());
    }
    return symbols_ == nullptr ? 0 : symbols_->count();
}

void StrategyInstance::log(LogLevel level, const char* message) {
    log::write(level, id_, message != nullptr ? message : "");
}

void StrategyInstance::publish_metric(const char* name, double value) {
    if (name == nullptr) {
        return;
    }
    // Linear scan over a vector, not a hash map: there are a handful of these
    // per strategy, and first-published order is stable across runs in a way
    // hash order is not. The GUI draws them in this order.
    for (PublishedMetric& metric : metrics_) {
        if (metric.name == name) {
            metric.value = value;
            ++metric.updates;
            return;
        }
    }
    metrics_.push_back(PublishedMetric{name, value, 1});
}

}  // namespace marketscan
