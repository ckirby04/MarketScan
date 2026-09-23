#include "marketscan/live/live_broker.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <thread>

#include <nlohmann/json.hpp>

#include "marketscan/core/clock.hpp"
#include "marketscan/live/detail.hpp"
#include "marketscan/live/endpoints.hpp"
#include "marketscan/live/websocket.hpp"
#include "marketscan/util/logging.hpp"

namespace marketscan::live {
namespace {

/// This socket's receive timeout, deliberately not shared with live_feed.cpp's
/// identical value: see the note in detail.hpp.
constexpr int kReceiveTimeoutMs = 250;

using detail::interruptible_sleep;
using detail::local_now;
using detail::number;
using detail::tif_name;

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

}  // namespace

LiveBroker::LiveBroker(LiveBrokerConfig config, Credentials credentials, const SymbolTable* symbols)
    : config_(std::move(config)),
      credentials_(std::move(credentials)),
      symbols_(symbols),
      ledger_(config_.starting_cash),
      tracker_(&ledger_, config_.run_token.empty() ? make_run_token() : config_.run_token) {
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
               "paper account, run token " + tracker_.order_map().run_token() +
                   "; orders capped at " + number(config_.max_order_quantity, 0) + " shares, $" +
                   number(static_cast<double>(config_.max_order_notional.micros()) / 1e6, 0) +
                   " notional, $" +
                   number(static_cast<double>(config_.max_gross_exposure.micros()) / 1e6, 0) +
                   " gross exposure and " + std::to_string(config_.max_orders) + " orders");
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
    return tracker_.find_order(id);
}

std::vector<const Order*> LiveBroker::working_orders() const {
    return tracker_.working_orders();
}

OrderId LiveBroker::register_local_order_for_test(const MS_OrderRequest& request, Timestamp now) {
    return tracker_.register_order(request, now, kNever, config_.order_id_base);
}

void LiveBroker::apply_update(const TradeUpdate& update, Timestamp now) {
    tracker_.apply_update(update, now);
}

LiveBrokerStats LiveBroker::stats() const {
    LiveBrokerStats out;
    static_cast<OrderTrackerStats&>(out) = tracker_.stats();
    out.reconnects = reconnects_.load(std::memory_order_relaxed);
    out.reclaimed_after_reconnect = reclaimed_.load(std::memory_order_relaxed);
    return out;
}

void LiveBroker::record_submission(const MS_OrderRequest& request,
                                   OrderId engine_id,
                                   bool returned_id_valid,
                                   std::string_view reason) {
    if (!config_.recorder) {
        return;
    }
    const char* ticker = symbols_->c_name(request.symbol);
    config_.recorder->record_submission(tracker_.order_map().client_id_for(engine_id),
                                        engine_id,
                                        ticker != nullptr ? ticker : "",
                                        request.side,
                                        request.type,
                                        request.tif,
                                        request.quantity,
                                        request.limit_price,
                                        request.client_order_id,
                                        returned_id_valid,
                                        reason,
                                        local_now());
}

RejectReason LiveBroker::check_rails(const MS_OrderRequest& request) const {
    if (!(request.quantity > 0.0) || !std::isfinite(request.quantity)) {
        return RejectReason::InvalidQuantity;
    }
    if (request.type == MS_ORD_LIMIT &&
        (!(request.limit_price > 0.0) || !std::isfinite(request.limit_price))) {
        return RejectReason::InvalidPrice;
    }
    if (request.quantity > config_.max_order_quantity) {
        return RejectReason::InvalidQuantity;
    }
    if (config_.max_order_notional.micros() > 0 && request.type == MS_ORD_LIMIT) {
        // From the limit price, which is the only price this class has. A market
        // order carries none and passes on the share cap alone -- stated here
        // and asserted in tests/test_live_broker.cpp, rather than left to be
        // discovered at the moment it matters.
        if (Money::notional(request.limit_price, request.quantity) > config_.max_order_notional) {
            return RejectReason::InvalidQuantity;
        }
    }

    if (config_.max_gross_exposure.micros() > 0 && request.type == MS_ORD_LIMIT) {
        // Only orders that *increase* exposure are capped. Refusing a closing
        // order would trap a run in the position the cap exists to prevent.
        const Quantity held = ledger_.position(request.symbol);
        const Quantity delta = request.side == MS_SIDE_SELL ? -request.quantity : request.quantity;
        if (std::abs(held + delta) > std::abs(held)) {
            Money gross;
            for (const Position* position : ledger_.open_positions()) {
                gross += Money::from_micros(std::abs(position->cost_basis.micros()));
            }
            gross += Money::notional(request.limit_price, request.quantity);
            if (gross > config_.max_gross_exposure) {
                return RejectReason::InsufficientCash;
            }
        }
    }

    return RejectReason::None;
}

OrderId LiveBroker::submit(const MS_OrderRequest& request, Timestamp now, Timestamp decision_ts) {
    const auto refuse = [&](RejectReason reason, const char* message) -> OrderId {
        log::write(MS_LOG_WARN, "broker", std::string("order refused here: ") + message);
        tracker_.note_rail_refusal();
        const OrderId refused_id = tracker_.register_refusal(
            request, now, decision_ts, config_.order_id_base, reason, message);
        record_submission(request, refused_id, false, message);
        return kInvalidOrderId;
    };

    // The rails, checked before anything leaves this process. An order that
    // breaks one never reaches the venue at all.
    switch (check_rails(request)) {
        case RejectReason::InvalidQuantity:
            return refuse(RejectReason::InvalidQuantity,
                          "order refused by this run's quantity rails: not a positive finite "
                          "number, over the share cap, or over the notional cap. The share cap "
                          "alone is not a safety limit when instruments differ fourfold in price");
        case RejectReason::InvalidPrice:
            return refuse(RejectReason::InvalidPrice,
                          "limit price is not a positive finite number");
        case RejectReason::InsufficientCash:
            return refuse(RejectReason::InsufficientCash,
                          "order would take this run past its gross exposure cap. Refused, not "
                          "resized: a cap that quietly shrank an order would absorb a sizing bug "
                          "instead of reporting one");
        default:
            break;
    }

    bool budget_spent = false;
    if (config_.max_orders != 0) {
        budget_spent = tracker_.stats().submitted >= config_.max_orders;
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

    const OrderId engine_id =
        tracker_.register_order(request, now, decision_ts, config_.order_id_base);

    nlohmann::json body;
    body["symbol"] = ticker;
    body["qty"] = number(request.quantity, 0);
    body["side"] = request.side == MS_SIDE_SELL ? "sell" : "buy";
    body["type"] = request.type == MS_ORD_LIMIT ? "limit" : "market";
    body["time_in_force"] = tif_name(request.tif);
    body["client_order_id"] = tracker_.order_map().client_id_for(engine_id);
    if (request.type == MS_ORD_LIMIT) {
        body["limit_price"] = number(request.limit_price, 2);
        if (config_.allow_extended_hours && request.tif == MS_TIF_DAY) {
            // The venue accepts this only for a limit order with a DAY time in
            // force, so it is asked for only then rather than sent always and
            // hoped about.
            body["extended_hours"] = true;
        }
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
        record_submission(request, engine_id, true, "rate limited; state unknown");
        return engine_id;
    }

    if (!response.ok()) {
        // Ends the order and counts it, in one call. This used to be four lines
        // of state change here plus a counter call, and a refactor kept the state
        // change and lost the counter -- see note_venue_refusal's comment.
        tracker_.note_venue_refusal(engine_id, now);
        Order* stored = tracker_.mutable_order(engine_id);
        const std::string detail =
            response.error.empty()
                ? "status " + std::to_string(response.status) + ": " + response.body
                : response.error;
        log::write(MS_LOG_WARN, "broker", "venue refused the order: " + detail);
        tracker_.order_map().retire(engine_id);
        tracker_.emit_status(*stored, MS_ORDER_REJECTED, true, "refused by the venue");
        record_submission(request, engine_id, false, detail);
        return kInvalidOrderId;
    }

    const nlohmann::json accepted = nlohmann::json::parse(response.body, nullptr, false);
    if (!accepted.is_discarded() && accepted.is_object()) {
        const auto id = accepted.find("id");
        if (id != accepted.end() && id->is_string()) {
            tracker_.order_map().record_acknowledged(engine_id, id->get<std::string>());
        }
    }

    Order* stored = tracker_.mutable_order(engine_id);
    tracker_.emit_status(*stored, MS_ORDER_ACCEPTED, false, "accepted by the venue");
    record_submission(request, engine_id, true, {});
    return engine_id;
}

bool LiveBroker::cancel(OrderId id, Timestamp now) {
    Order* order = tracker_.mutable_order(id);
    if (order == nullptr || order->is_terminal()) {
        tracker_.note_lost_cancel_race();
        return false;
    }

    const OrderLink* link = tracker_.order_map().find(id);
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
        tracker_.note_lost_cancel_race();
        return false;
    }

    return response.ok() || response.status == 204;
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
            tracker_.order_map().engine_id_for(client->get<std::string_view>());
        if (!engine_id.has_value() || tracker_.mutable_order(*engine_id) == nullptr) {
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

    reclaimed_.fetch_add(reclaimed, std::memory_order_relaxed);
    log::write(MS_LOG_INFO,
               "broker",
               "reconnected; reclaimed " + std::to_string(reclaimed) +
                   " of this run's orders from the venue by client order id");
}

void LiveBroker::cancel_all_open(Timestamp now) {
    for (const OrderLink& link : tracker_.order_map().live()) {
        Order* order = tracker_.mutable_order(link.engine_id);
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

                if (config_.recorder) {
                    // Raw, like market data, and for the same reason: replay
                    // re-parses with this same function rather than a second
                    // one that might disagree.
                    config_.recorder->record_response(message.text, local_now());
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

        reconnects_.fetch_add(1, std::memory_order_relaxed);
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
