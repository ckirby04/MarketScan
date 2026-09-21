#include "marketscan/live/live_broker.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <thread>

#include <nlohmann/json.hpp>

#include "marketscan/core/clock.hpp"
#include "marketscan/live/endpoints.hpp"
#include "marketscan/live/websocket.hpp"
#include "marketscan/util/logging.hpp"

namespace marketscan::live {
namespace {

constexpr int kReceiveTimeoutMs = 250;

[[nodiscard]] Timestamp local_now() noexcept {
    static const SystemClock clock;
    return clock.now();
}

void interruptible_sleep(int total_ms, const std::atomic<bool>& stopping) {
    constexpr int kSlice = 100;
    int slept = 0;
    while (slept < total_ms && !stopping.load(std::memory_order_relaxed)) {
        const int slice = std::min(kSlice, total_ms - slept);
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        slept += slice;
    }
}

/// The venue takes quantities and prices as strings.
[[nodiscard]] std::string number(double value, int decimals) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

/// A field the venue may send as a string, as a number, or as null.
///
/// nlohmann::json::value throws type_error when the key is present and null,
/// which is not the same as absent and is exactly what the venue sends for
/// filled_avg_price on an order that has not filled. That threw in the middle
/// of the reclaim-after-reconnect path -- the one path this was written to
/// exercise -- and took the process with it.
[[nodiscard]] std::string
text_or(const nlohmann::json& obj, const char* key, const char* fallback) {
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) {
        return fallback;
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    if (it->is_number()) {
        return std::to_string(it->get<double>());
    }
    return fallback;
}

/// The same, as a number. Zero when absent, null or unparseable -- and zero is
/// the right answer for every field this reads, because the caller checks a
/// quantity before it books anything.
[[nodiscard]] double number_or_zero(const nlohmann::json& obj, const char* key) {
    const std::string raw = text_or(obj, key, "0");
    if (raw.empty()) {
        return 0.0;
    }
    try {
        return std::stod(raw);
    } catch (const std::exception&) {
        return 0.0;
    }
}

[[nodiscard]] const char* tif_name(MS_TimeInForce tif) noexcept {
    switch (tif) {
        case MS_TIF_IOC:
            return "ioc";
        case MS_TIF_GTC:
            return "gtc";
        case MS_TIF_DAY:
        default:
            return "day";
    }
}

}  // namespace

LiveBroker::LiveBroker(LiveBrokerConfig config, Credentials credentials, const SymbolTable* symbols)
    : config_(std::move(config)),
      credentials_(std::move(credentials)),
      symbols_(symbols),
      ledger_(config_.starting_cash),
      order_map_(config_.run_token.empty() ? make_run_token() : config_.run_token) {
    if (!credentials_.ok()) {
        throw std::invalid_argument("LiveBroker has no credentials: " + credentials_.error);
    }
    if (symbols_ == nullptr) {
        throw std::invalid_argument("LiveBroker needs a symbol table to name instruments");
    }

    // Throws for a host off the paper allowlist. There is no other host it
    // could be given: kPaperTradingHost is the only trading endpoint this
    // project knows the name of.
    http_ = std::make_unique<HttpClient>(
        std::string(kPaperTradingHost), credentials_, config_.user_agent);

    log::write(MS_LOG_INFO,
               "broker",
               "paper account, run token " + order_map_.run_token() + "; orders capped at " +
                   number(config_.max_order_quantity, 0) + " shares and " +
                   std::to_string(config_.max_orders) + " orders");
}

LiveBroker::~LiveBroker() {
    stop();
}

void LiveBroker::reset() {
    // A backtest rewinds by replaying. A live account has positions in it and
    // a venue that has never heard of "start again"; quietly doing nothing
    // would let a caller believe the state had been cleared.
    throw std::logic_error(
        "LiveBroker::reset(): a live account cannot be rewound. Rewinding is a replay operation; "
        "see IBroker in exec/broker.hpp.");
}

const Order* LiveBroker::find_order(OrderId id) const noexcept {
    const auto it = index_.find(id);
    return it == index_.end() ? nullptr : &orders_[it->second];
}

Order* LiveBroker::mutable_order(OrderId id) noexcept {
    const auto it = index_.find(id);
    return it == index_.end() ? nullptr : &orders_[it->second];
}

std::vector<const Order*> LiveBroker::working_orders() const {
    std::vector<const Order*> out;
    for (const Order& order : orders_) {
        if (order.is_working()) {
            out.push_back(&order);
        }
    }
    return out;
}

LiveBrokerStats LiveBroker::stats() const {
    const std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void LiveBroker::emit_status(const Order& order,
                             MS_OrderState state,
                             bool terminal,
                             const char* reason) {
    if (listener_ == nullptr) {
        return;
    }
    MS_OrderStatus status{};
    status.ts = order.last_update_ts;
    status.order_id = order.id;
    status.client_order_id = order.request.client_order_id;
    status.state = state;
    status.terminal = terminal ? 1u : 0u;
    status.filled_quantity = order.filled_quantity;
    status.replaced_by = 0;
    status.reason = reason;
    listener_->on_order_status(status);
}

void LiveBroker::book_fill(Order& order, Quantity quantity, Price price, Timestamp ts) {
    if (!(quantity > 0.0) || !(price > 0.0)) {
        return;
    }

    const Quantity before = order.filled_quantity;
    order.filled_quantity = before + quantity;
    order.average_fill_price =
        before > 0.0
            ? (order.average_fill_price * before + price * quantity) / order.filled_quantity
            : price;
    order.last_update_ts = ts;
    order.status = order.filled_quantity + 1e-9 >= order.request.quantity
                       ? OrderStatus::Filled
                       : OrderStatus::PartiallyFilled;

    const TradeRecord& record = ledger_.apply_fill(ts,
                                                   order.id,
                                                   order.request.client_order_id,
                                                   order.request.symbol,
                                                   order.request.side,
                                                   quantity,
                                                   price,
                                                   order.reference_price,
                                                   Money{},
                                                   order.decision_ts,
                                                   ts);
    (void)record;

    if (listener_ != nullptr) {
        MS_Fill fill{};
        fill.ts = ts;
        fill.symbol = order.request.symbol;
        fill.order_id = order.id;
        fill.client_order_id = order.request.client_order_id;
        fill.side = order.request.side;
        fill.price = price;
        fill.quantity = quantity;
        listener_->on_fill(fill);
    }
}

OrderId LiveBroker::register_local_order_for_test(const MS_OrderRequest& request, Timestamp now) {
    Order order;
    order.id = config_.order_id_base + next_sequence_++;
    order.request = request;
    order.submitted_ts = now;
    order.last_update_ts = now;
    order.eligible_ts = now;
    order.status = OrderStatus::Working;
    order.decision_ts = kNever;

    index_[order.id] = orders_.size();
    orders_.push_back(order);
    order_map_.record_submitted(order.id);
    return order.id;
}

OrderId LiveBroker::submit(const MS_OrderRequest& request, Timestamp now, Timestamp decision_ts) {
    Order order;
    order.id = config_.order_id_base + next_sequence_++;
    order.request = request;
    order.submitted_ts = now;
    order.last_update_ts = now;
    order.eligible_ts = now;
    order.decision_ts = decision_ts;
    order.status = OrderStatus::Working;

    const auto refuse = [&](RejectReason reason, const char* message) -> OrderId {
        order.status = OrderStatus::Rejected;
        order.reject_reason = reason;
        index_[order.id] = orders_.size();
        orders_.push_back(order);
        {
            const std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.rejected;
            ++stats_.refused_by_rail;
        }
        log::write(MS_LOG_WARN, "broker", std::string("order refused here: ") + message);
        emit_status(orders_.back(), MS_ORDER_REJECTED, true, message);
        return kInvalidOrderId;
    };

    if (!(request.quantity > 0.0) || !std::isfinite(request.quantity)) {
        return refuse(RejectReason::InvalidQuantity, "quantity is not a positive finite number");
    }
    if (request.type == MS_ORD_LIMIT &&
        (!(request.limit_price > 0.0) || !std::isfinite(request.limit_price))) {
        return refuse(RejectReason::InvalidPrice, "limit price is not a positive finite number");
    }

    // The rails. Checked before anything leaves this process, so an order that
    // breaks one never reaches the venue at all.
    if (request.quantity > config_.max_order_quantity) {
        return refuse(RejectReason::InvalidQuantity,
                      "order exceeds this run's size cap; nothing in this milestone has any "
                      "reason to send size");
    }
    bool budget_spent = false;
    if (config_.max_orders != 0) {
        const std::lock_guard<std::mutex> lock(stats_mutex_);
        budget_spent = stats_.submitted >= config_.max_orders;
    }
    if (budget_spent) {
        return refuse(RejectReason::NotMarketable,
                      "this run has sent its whole order budget; raising it is a deliberate act, "
                      "not a retry");
    }

    const char* ticker = symbols_->c_name(request.symbol);
    if (ticker == nullptr || *ticker == '\0') {
        return refuse(RejectReason::UnknownSymbol, "symbol is not in this run's symbol table");
    }

    index_[order.id] = orders_.size();
    orders_.push_back(order);
    order_map_.record_submitted(order.id);

    nlohmann::json body;
    body["symbol"] = ticker;
    body["qty"] = number(request.quantity, 0);
    body["side"] = request.side == MS_SIDE_SELL ? "sell" : "buy";
    body["type"] = request.type == MS_ORD_LIMIT ? "limit" : "market";
    body["time_in_force"] = tif_name(request.tif);
    body["client_order_id"] = order_map_.client_id_for(order.id);
    if (request.type == MS_ORD_LIMIT) {
        body["limit_price"] = number(request.limit_price, 2);
        if (config_.allow_extended_hours && request.tif == MS_TIF_DAY) {
            // The venue accepts this only for a limit order with a DAY time in
            // force, so it is asked for only then rather than sent always and
            // hoped about.
            body["extended_hours"] = true;
        }
    }

    {
        const std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.submitted;
    }

    const HttpResponse response = http_->post("/v2/orders", body.dump());

    if (response.rate_limited()) {
        // Backed off, not retried. The order is left in Working: it may well
        // have been accepted, and marking it rejected would tell a strategy
        // something this process does not know.
        log::write(MS_LOG_ERROR,
                   "broker",
                   "order submit was rate limited; not retrying. The order's state is unknown "
                   "until the trade-updates stream says otherwise.");
        return order.id;
    }

    if (!response.ok()) {
        Order* stored = mutable_order(order.id);
        stored->status = OrderStatus::Rejected;
        stored->reject_reason = RejectReason::NotMarketable;
        stored->last_update_ts = now;
        {
            const std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.rejected;
        }
        const std::string detail =
            response.error.empty()
                ? "status " + std::to_string(response.status) + ": " + response.body
                : response.error;
        log::write(MS_LOG_WARN, "broker", "venue refused the order: " + detail);
        order_map_.retire(order.id);
        emit_status(*stored, MS_ORDER_REJECTED, true, "refused by the venue");
        return kInvalidOrderId;
    }

    const nlohmann::json accepted = nlohmann::json::parse(response.body, nullptr, false);
    if (!accepted.is_discarded() && accepted.is_object()) {
        const auto id = accepted.find("id");
        if (id != accepted.end() && id->is_string()) {
            order_map_.record_acknowledged(order.id, id->get<std::string>());
        }
    }

    {
        const std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.accepted;
    }
    Order* stored = mutable_order(order.id);
    emit_status(*stored, MS_ORDER_ACCEPTED, false, "accepted by the venue");
    return order.id;
}

bool LiveBroker::cancel(OrderId id, Timestamp now) {
    Order* order = mutable_order(id);
    if (order == nullptr || order->is_terminal()) {
        const std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.lost_cancel_races;
        return false;
    }

    const OrderLink* link = order_map_.find(id);
    if (link == nullptr || link->broker_order_id.empty()) {
        // Sent, not yet acknowledged. There is nothing to address a cancel to
        // yet; saying so is better than sending one into the void.
        log::write(MS_LOG_WARN,
                   "broker",
                   "cannot cancel order " + std::to_string(id) +
                       " yet: the venue has not acknowledged it. It will be picked up by "
                       "cancel_all_open once it has.");
        return false;
    }

    const HttpResponse response = http_->del("/v2/orders/" + link->broker_order_id);
    order->last_update_ts = now;

    if (response.rate_limited()) {
        log::write(MS_LOG_ERROR, "broker", "cancel was rate limited; not retrying.");
        return false;
    }

    // 404 and 422 both mean the order was already done. That is a race this
    // process lost, not a failure to send -- and the terminal state will
    // arrive on the socket like any other.
    if (response.status == 404 || response.status == 422) {
        const std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.lost_cancel_races;
        return false;
    }

    return response.ok() || response.status == 204;
}

void LiveBroker::apply_update(const TradeUpdate& update, Timestamp now) {
    const std::optional<OrderId> engine_id = order_map_.engine_id_for(update.client_order_id);
    if (!engine_id.has_value()) {
        // Another run's order on the same account, or a manual one. Counted so
        // that "the venue reported a fill nobody booked" is never silent.
        const std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.foreign_updates;
        return;
    }

    Order* order = mutable_order(*engine_id);
    if (order == nullptr) {
        const std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.foreign_updates;
        return;
    }

    if (order->is_terminal()) {
        // Already over here. The reclaim after a reconnect re-reads every order
        // the venue knows about, so this is the ordinary case rather than an
        // odd one -- and applying it again would emit a second terminal status
        // for one order.
        const std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.stale_updates;
        return;
    }

    if (!update.broker_order_id.empty()) {
        order_map_.record_acknowledged(order->id, update.broker_order_id);
    }

    const Timestamp ts = update.ts != 0 ? update.ts : now;
    order->last_update_ts = ts;

    switch (update.kind) {
        case UpdateKind::Accepted: {
            const std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.accepted;
            break;
        }

        case UpdateKind::PartialFill:
        case UpdateKind::Fill: {
            // The venue's cumulative number, not a running sum of our own. A
            // sum that missed one message would be quietly wrong forever;
            // reading the cumulative figure means a missed message costs one
            // blotter row's granularity and nothing else.
            const Quantity already = order->filled_quantity;
            Quantity increment = update.cumulative_filled - already;
            if (!(increment > 0.0)) {
                increment = update.fill_quantity;
            }
            const Price price =
                update.fill_price > 0.0 ? update.fill_price : update.average_fill_price;

            if (increment > 0.0 && price > 0.0) {
                book_fill(*order, increment, price, ts);
            }

            const std::lock_guard<std::mutex> lock(stats_mutex_);
            if (update.kind == UpdateKind::Fill) {
                ++stats_.filled;
            } else {
                ++stats_.partial_fills;
            }
            break;
        }

        case UpdateKind::Cancelled:
            order->status = OrderStatus::Cancelled;
            {
                const std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.cancelled;
            }
            break;

        case UpdateKind::Expired:
            order->status = OrderStatus::Expired;
            {
                const std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.expired;
            }
            break;

        case UpdateKind::Rejected:
            order->status = OrderStatus::Rejected;
            order->reject_reason = RejectReason::NotMarketable;
            {
                const std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.rejected;
            }
            break;

        case UpdateKind::Replaced:
            order->status = OrderStatus::Cancelled;
            break;

        case UpdateKind::Transient:
            // Alive, nothing to do, and deliberately not silence: the order
            // stays working and the next message decides.
            return;

        case UpdateKind::Unknown: {
            {
                const std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.unknown_updates;
            }
            log::write(MS_LOG_ERROR,
                       "broker",
                       "unknown trade update '" + update.raw_event + "' for order " +
                           std::to_string(order->id) +
                           "; the order is left working rather than assumed finished");
            return;
        }
    }

    if (update.kind == UpdateKind::Fill && order->status != OrderStatus::Filled) {
        // The venue says the order is done and our arithmetic says it is not.
        // Believe the venue about the state, say so about the difference.
        log::write(MS_LOG_ERROR,
                   "broker",
                   "order " + std::to_string(order->id) + " filled " +
                       number(order->filled_quantity, 4) + " of " +
                       number(order->request.quantity, 4) +
                       " but the venue calls it complete. Booking it complete and reporting the "
                       "difference rather than correcting it silently.");
        order->status = OrderStatus::Filled;
    }

    if (order->is_terminal()) {
        MS_OrderState state = MS_ORDER_CANCELLED;
        const char* reason = "cancelled";
        switch (order->status) {
            case OrderStatus::Filled:
                // A complete fill is reported through on_fill, and a terminal
                // status as well, so a strategy that only implements one of
                // the two callbacks still learns the order is over.
                state = MS_ORDER_ACCEPTED;
                reason = "filled";
                break;
            case OrderStatus::Expired:
                state = MS_ORDER_EXPIRED;
                reason = "expired at the venue";
                break;
            case OrderStatus::Rejected:
                state = MS_ORDER_REJECTED;
                reason = update.reason.empty() ? "rejected by the venue" : update.reason.c_str();
                break;
            default:
                break;
        }
        emit_status(*order, state, true, reason);
        order_map_.retire(order->id);
    }
}

void LiveBroker::on_market_event(SymbolId symbol, Timestamp now) {
    (void)symbol;

    std::vector<TradeUpdate> batch;
    bool reconnected = false;
    {
        const std::lock_guard<std::mutex> lock(inbox_mutex_);
        batch.swap(inbox_);
        reconnected = reconnected_;
        reconnected_ = false;
    }

    for (const TradeUpdate& update : batch) {
        apply_update(update, now);
    }

    if (reconnected) {
        reclaim_after_reconnect(now);
    }
}

void LiveBroker::reclaim_after_reconnect(Timestamp now) {
    // Every order this run still believes is live, looked up by the client id
    // it was sent with. Anything the venue no longer lists as open has reached
    // a terminal state that this process may have missed while disconnected.
    const HttpResponse response = http_->get("/v2/orders?status=all&limit=500&nested=false");
    if (!response.ok()) {
        log::write(MS_LOG_ERROR,
                   "broker",
                   "could not re-read the venue's orders after a reconnect; this run's view of "
                   "its own orders may be stale: " +
                       (response.error.empty() ? "status " + std::to_string(response.status)
                                               : response.error));
        return;
    }

    const nlohmann::json list = nlohmann::json::parse(response.body, nullptr, false);
    if (list.is_discarded() || !list.is_array()) {
        log::write(MS_LOG_ERROR, "broker", "the venue's order list was not a JSON array");
        return;
    }

    std::uint64_t reclaimed = 0;
    for (const nlohmann::json& entry : list) {
        if (!entry.is_object()) {
            continue;
        }
        const auto client = entry.find("client_order_id");
        if (client == entry.end() || !client->is_string()) {
            continue;
        }
        const std::optional<OrderId> engine_id =
            order_map_.engine_id_for(client->get<std::string_view>());
        if (!engine_id.has_value() || mutable_order(*engine_id) == nullptr) {
            continue;
        }

        // Rebuild an update from the order's own state and run it through the
        // same path a socket message takes. Not a second application of the
        // rules: the same function, given the venue's current answer.
        TradeUpdate update;
        update.raw_event = text_or(entry, "status", "");
        update.client_order_id = client->get<std::string>();
        update.broker_order_id = text_or(entry, "id", "");
        update.symbol = text_or(entry, "symbol", "");
        update.cumulative_filled = number_or_zero(entry, "filled_qty");
        update.average_fill_price = number_or_zero(entry, "filled_avg_price");
        update.order_quantity = number_or_zero(entry, "qty");
        update.side = text_or(entry, "side", "buy") == "sell" ? MS_SIDE_SELL : MS_SIDE_BUY;

        const std::string status = update.raw_event;
        if (status == "filled") {
            update.kind = UpdateKind::Fill;
        } else if (status == "partially_filled") {
            update.kind = UpdateKind::PartialFill;
        } else if (status == "canceled" || status == "cancelled") {
            update.kind = UpdateKind::Cancelled;
        } else if (status == "expired" || status == "done_for_day") {
            update.kind = UpdateKind::Expired;
        } else if (status == "rejected") {
            update.kind = UpdateKind::Rejected;
        } else if (status == "replaced") {
            update.kind = UpdateKind::Replaced;
        } else {
            update.kind = UpdateKind::Accepted;
        }
        update.fill_price = update.average_fill_price;

        apply_update(update, now);
        ++reclaimed;
    }

    {
        const std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.reclaimed_after_reconnect += reclaimed;
    }
    log::write(MS_LOG_INFO,
               "broker",
               "reconnected; reclaimed " + std::to_string(reclaimed) +
                   " of this run's orders from the venue by client order id");
}

void LiveBroker::cancel_all_open(Timestamp now) {
    for (const OrderLink& link : order_map_.live()) {
        Order* order = mutable_order(link.engine_id);
        if (order == nullptr || order->is_terminal()) {
            continue;
        }
        (void)cancel(link.engine_id, now);
    }

    // A blanket cancel as well, because an order submitted just before a
    // dropped connection may exist at the venue without a broker id here.
    const HttpResponse response = http_->del("/v2/orders");
    if (!response.ok() && response.status != 207 && response.status != 204) {
        log::write(MS_LOG_WARN,
                   "broker",
                   "blanket cancel returned status " + std::to_string(response.status));
    }
}

Reconciliation LiveBroker::reconcile() {
    Reconciliation out;

    const HttpResponse account = http_->get("/v2/account");
    if (!account.ok()) {
        out.error = account.error.empty()
                        ? "account request returned status " + std::to_string(account.status)
                        : account.error;
        return out;
    }

    const HttpResponse positions = http_->get("/v2/positions");
    if (!positions.ok()) {
        out.error = positions.error.empty()
                        ? "positions request returned status " + std::to_string(positions.status)
                        : positions.error;
        return out;
    }

    return compare_with(account.body, positions.body);
}

Reconciliation LiveBroker::compare_with(const std::string& account_body,
                                        const std::string& positions_body) const {
    Reconciliation out;

    const nlohmann::json parsed = nlohmann::json::parse(account_body, nullptr, false);
    if (parsed.is_discarded()) {
        out.error = "account response was not JSON";
        return out;
    }

    const nlohmann::json held = nlohmann::json::parse(positions_body, nullptr, false);
    if (held.is_discarded() || !held.is_array()) {
        out.error = "positions response was not a JSON array";
        return out;
    }

    out.checked = true;

    // Positions first, because a position difference is the one that changes
    // what the next order does.
    std::map<std::string, double> venue_positions;
    for (const nlohmann::json& entry : held) {
        if (!entry.is_object()) {
            continue;
        }
        const std::string symbol = entry.value("symbol", std::string{});
        const std::string qty = entry.value("qty", std::string{"0"});
        if (!symbol.empty()) {
            venue_positions[symbol] = std::stod(qty);
        }
    }

    std::map<std::string, double> our_positions;
    for (const Position* position : ledger_.open_positions()) {
        const char* name = symbols_->c_name(position->symbol);
        if (name != nullptr && *name != '\0' && std::abs(position->quantity) > 1e-9) {
            our_positions[name] = position->quantity;
        }
    }

    for (const auto& [symbol, quantity] : our_positions) {
        const auto it = venue_positions.find(symbol);
        const double theirs = it == venue_positions.end() ? 0.0 : it->second;
        if (std::abs(theirs - quantity) > 1e-6) {
            out.divergences.push_back(
                {"position in " + symbol, number(quantity, 4), number(theirs, 4)});
        }
    }
    for (const auto& [symbol, quantity] : venue_positions) {
        if (our_positions.find(symbol) == our_positions.end() && std::abs(quantity) > 1e-9) {
            // A position at the venue this run has no record of. Usually
            // another run's, or one left over from a previous session -- and
            // either way something a person has to decide about.
            out.divergences.push_back(
                {"position in " + symbol + " (unknown to this run)", "0", number(quantity, 4)});
        }
    }

    for (const Divergence& divergence : out.divergences) {
        log::write(MS_LOG_ERROR,
                   "reconcile",
                   divergence.what + ": this run says " + divergence.ours + ", the venue says " +
                       divergence.theirs +
                       ". Reported, not corrected -- a ledger that adopts the venue's answer "
                       "hides whatever made them differ.");
    }
    if (out.divergences.empty()) {
        log::write(MS_LOG_INFO, "reconcile", "this run's positions agree with the venue");
    }
    return out;
}

bool LiveBroker::wait_until_listening(int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (listening_.load(std::memory_order_acquire)) {
            return true;
        }
        if (fatal_.load(std::memory_order_acquire)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return listening_.load(std::memory_order_acquire);
}

void LiveBroker::request_drop_for_test() {
    drop_requested_.store(true, std::memory_order_relaxed);
}

bool LiveBroker::start() {
    if (thread_.joinable()) {
        return listening_.load(std::memory_order_acquire);
    }
    stopping_.store(false, std::memory_order_relaxed);
    fatal_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this] { run_socket(); });
    return wait_until_listening(20000);
}

void LiveBroker::stop() {
    stopping_.store(true, std::memory_order_relaxed);

    {
        const std::lock_guard<std::mutex> lock(socket_mutex_);
        if (active_socket_ != nullptr) {
            active_socket_->request_close();
        }
    }

    if (thread_.joinable()) {
        thread_.join();
    }
    listening_.store(false, std::memory_order_release);
}

void LiveBroker::run_socket() {
    int backoff_ms = config_.reconnect_initial_ms;
    bool first_connection = true;

    while (!stopping_.load(std::memory_order_relaxed)) {
        drop_requested_.store(false, std::memory_order_relaxed);
        WebSocket socket;
        std::string reason;
        bool authorised = false;

        {
            const std::lock_guard<std::mutex> lock(socket_mutex_);
            active_socket_ = &socket;
        }

        if (!socket.connect(std::string(kPaperTradingHost),
                            std::string(kPaperTradeStreamPath),
                            config_.user_agent)) {
            reason = socket.error();
        } else {
            // Authorize, then listen. The auth frame is the only message that
            // carries the secret and is never recorded anywhere.
            nlohmann::json auth;
            auth["action"] = "authenticate";
            auth["data"]["key_id"] = credentials_.key_id;
            auth["data"]["secret_key"] = credentials_.secret;

            if (!socket.send(auth.dump())) {
                reason = "could not send authenticate: " + socket.error();
            } else {
                const SocketMessage reply = socket.receive(10000);
                if (!reply.ok) {
                    reason = "no answer to authenticate";
                } else {
                    const TradeUpdateParse parsed = parse_trade_update(reply.text);
                    if (parsed.unauthorised) {
                        reason = "the trade-updates stream refused these credentials";
                        fatal_.store(true, std::memory_order_release);
                    } else {
                        authorised = true;
                    }
                }
            }
        }

        if (authorised) {
            nlohmann::json listen;
            listen["action"] = "listen";
            listen["data"]["streams"] = nlohmann::json::array({"trade_updates"});
            if (!socket.send(listen.dump())) {
                reason = "could not send listen: " + socket.error();
                authorised = false;
            } else {
                const SocketMessage reply = socket.receive(10000);
                if (!reply.ok) {
                    reason = "no answer to listen";
                    authorised = false;
                }
            }
        }

        if (authorised) {
            listening_.store(true, std::memory_order_release);
            backoff_ms = config_.reconnect_initial_ms;
            if (!first_connection) {
                const std::lock_guard<std::mutex> lock(inbox_mutex_);
                reconnected_ = true;
            }
            first_connection = false;
            log::write(MS_LOG_INFO, "broker", "listening for trade updates");

            while (!stopping_.load(std::memory_order_relaxed)) {
                if (drop_requested_.exchange(false, std::memory_order_relaxed)) {
                    reason = "drop requested by the caller (reconnect exercise)";
                    break;
                }
                const SocketMessage message = socket.receive(kReceiveTimeoutMs);
                if (message.closed) {
                    reason =
                        socket.error().empty() ? "connection closed by the venue" : socket.error();
                    break;
                }
                if (!message.ok) {
                    continue;
                }

                const TradeUpdateParse parsed = parse_trade_update(message.text);
                if (!parsed.error.empty()) {
                    log::write(MS_LOG_WARN, "broker", parsed.error);
                    continue;
                }
                if (!parsed.updates.empty()) {
                    const std::lock_guard<std::mutex> lock(inbox_mutex_);
                    inbox_.insert(inbox_.end(), parsed.updates.begin(), parsed.updates.end());
                }
            }
        }

        listening_.store(false, std::memory_order_release);
        {
            const std::lock_guard<std::mutex> lock(socket_mutex_);
            active_socket_ = nullptr;
        }
        socket.close();

        if (stopping_.load(std::memory_order_relaxed)) {
            break;
        }
        if (fatal_.load(std::memory_order_acquire)) {
            const std::lock_guard<std::mutex> lock(error_mutex_);
            error_ = reason;
            log::write(MS_LOG_ERROR, "broker", "not retrying: " + reason);
            return;
        }

        {
            const std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.reconnects;
        }
        log::write(MS_LOG_WARN,
                   "broker",
                   "trade-updates connection lost (" + reason + "); retrying in " +
                       std::to_string(backoff_ms) +
                       "ms. Orders live at the venue are unaffected "
                       "and will be reclaimed by client order id.");

        interruptible_sleep(backoff_ms, stopping_);
        backoff_ms = std::min(backoff_ms * 2, config_.reconnect_max_ms);
    }
}

}  // namespace marketscan::live
