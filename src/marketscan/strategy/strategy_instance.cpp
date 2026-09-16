#include "marketscan/strategy/strategy_instance.hpp"

#include <algorithm>
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
    if (started_ && !stopped_ && wants(bar.symbol)) {
        // Measured around the callback, which is where an order is submitted,
        // priced and booked -- so this is event-in to order-out, not just
        // dispatch overhead.
        const ScopedLatency timer(latency_);
        plugin_.bar(bar);
    }
}

void StrategyInstance::on_quote(const MS_Quote& quote) {
    if (started_ && !stopped_ && wants(quote.symbol)) {
        // Measured around the callback, which is where an order is submitted,
        // priced and booked -- so this is event-in to order-out, not just
        // dispatch overhead.
        const ScopedLatency timer(latency_);
        plugin_.quote(quote);
    }
}

void StrategyInstance::on_trade(const MS_Trade& trade) {
    if (started_ && !stopped_ && wants(trade.symbol)) {
        // Measured around the callback, which is where an order is submitted,
        // priced and booked -- so this is event-in to order-out, not just
        // dispatch overhead.
        const ScopedLatency timer(latency_);
        plugin_.trade(trade);
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
    broker_.ledger().maybe_sample_equity(ts);
}

void StrategyInstance::on_fill(const MS_Fill& fill) {
    if (started_ && !stopped_) {
        plugin_.fill(fill);
    }
}

// --- HostServices ----------------------------------------------------------

std::uint64_t StrategyInstance::submit_order(const MS_OrderRequest& request) {
    return broker_.submit(request, now());
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
    // Lookup, never interning. A strategy inventing symbols would grow the
    // table mid-run and change the ids a later run assigns.
    return symbols_->find(ticker);
}

const char* StrategyInstance::symbol_name(SymbolId symbol) const {
    return symbols_ == nullptr ? "" : symbols_->c_name(symbol);
}

std::uint32_t StrategyInstance::symbol_count() const {
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
