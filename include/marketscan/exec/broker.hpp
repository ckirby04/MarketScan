#ifndef MARKETSCAN_EXEC_BROKER_HPP
#define MARKETSCAN_EXEC_BROKER_HPP

#include <cstdint>
#include <vector>

#include "marketscan/core/random.hpp"
#include "marketscan/core/session_calendar.hpp"
#include "marketscan/exec/fill_model.hpp"
#include "marketscan/exec/ledger.hpp"

namespace marketscan {

/// Told about every fill on one account, as it happens.
///
/// An interface rather than a std::function because this is called from the
/// per-event path, and a std::function is an indirect call through a type
/// erased object that may have had to allocate to exist.
class IFillListener {
public:
    IFillListener() = default;
    IFillListener(const IFillListener&) = delete;
    IFillListener& operator=(const IFillListener&) = delete;
    IFillListener(IFillListener&&) = delete;
    IFillListener& operator=(IFillListener&&) = delete;
    virtual ~IFillListener() = default;

    virtual void on_fill(const MS_Fill& fill) = 0;
};

/// Order lifecycle and execution for one account.
///
/// Sits between a strategy and its Ledger. The strategy submits; the broker
/// prices the order through the shared fill model, refuses it if the cash
/// account cannot pay for it, books the result and reports the fill back.
///
/// Every order ever submitted is kept, in submission order, whatever became of
/// it. That log is what the blotter shows, what the determinism digest hashes,
/// and the reason a rejected order is visible rather than simply absent.
///
/// **Order behaviour in this build:**
///
/// - A *market* order fills at its first eligible event or not at all; it
///   never rests waiting for a price. With the default impact model it always
///   fills in full, because there is always a price at which size clears.
/// - A *limit* order fills whatever quantity stays within its limit price. An
///   IOC cancels the remainder; a DAY or GTC order rests and is retried on
///   every subsequent event for its symbol.
/// - A *DAY* order expires when the exchange date changes. Not the UTC date:
///   see core/session_calendar.hpp.
/// - An order that cannot be paid for is refused **whole**, never part filled.
///   This is a cash account.
///
/// **Latency.** When the fill model configures a submit-to-fill delay, an
/// accepted order is *in flight* until it elapses: alive, cancellable, and not
/// yet priced. Market orders rest for that window like anything else. The
/// price an order gets is the price when it arrives, never the price when it
/// was sent -- which is the whole reason the delay exists, and the reason a
/// strategy written against a zero-latency simulator has never once
/// experienced a pending order.
class Broker {
public:
    /// `fill_model` and `market` are borrowed and must outlive the broker. The
    /// fill model is shared across accounts and stateless, so two strategies
    /// submitting the same order against the same book are priced identically.
    Broker(Money starting_cash,
           const IFillModel* fill_model,
           const MarketView* market,
           OrderId order_id_base = 0);

    void set_fill_listener(IFillListener* listener) noexcept { listener_ = listener; }

    /// Where latency jitter is drawn from.
    ///
    /// Borrowed, and shared with whatever else draws from it -- which is why
    /// it is the strategy's own stream and not a private one: a broker with a
    /// generator of its own would be a second source of randomness nobody
    /// seeded on purpose. Null means no jitter, whatever the configuration
    /// asks for.
    void set_jitter_source(DeterministicRng* rng) noexcept { jitter_ = rng; }

    /// Which wall clock DAY orders expire against.
    void set_exchange_zone(ExchangeZone zone) noexcept { zone_ = zone; }

    /// Orders accepted but not yet eligible to fill, as of `now`.
    [[nodiscard]] std::size_t in_flight_count(Timestamp now) const noexcept;

    /// Cancels that arrived after the order had already finished.
    ///
    /// Counted rather than ignored: live, this is a race that happened, and a
    /// strategy whose cancels are routinely too late is one whose logic
    /// assumes an execution speed it does not have.
    [[nodiscard]] std::uint64_t late_cancel_count() const noexcept { return late_cancels_; }

    /// Returns the engine order id, or kInvalidOrderId if the order was
    /// refused outright. A refused order still appears in orders() with its
    /// reason, so nothing disappears silently.
    ///
    /// `decision_ts` is the information horizon of the decision that produced
    /// this order -- see `Order::decision_ts`. It is recorded on every fill and
    /// never acted on: the broker does not refuse a lookahead, it makes one
    /// visible, and a fill model that refused one would hide the defect instead
    /// of reporting it. kNever means unrecorded.
    OrderId submit(const MS_OrderRequest& request, Timestamp now, Timestamp decision_ts = kNever);

    /// Cancels a working order. Returns false if the id is unknown or the
    /// order already reached a terminal state.
    bool cancel(OrderId id, Timestamp now);

    /// Retries resting orders for `symbol` and expires stale DAY orders.
    /// Called by the engine after each event, once the book has been updated.
    void on_market_event(SymbolId symbol, Timestamp now);

    [[nodiscard]] Ledger& ledger() noexcept { return ledger_; }

    [[nodiscard]] const Ledger& ledger() const noexcept { return ledger_; }

    /// Every order ever submitted, in submission order.
    [[nodiscard]] const std::vector<Order>& orders() const noexcept { return orders_; }

    [[nodiscard]] const Order* find_order(OrderId id) const noexcept;

    [[nodiscard]] std::vector<const Order*> working_orders() const;

    [[nodiscard]] const IFillModel& fill_model() const noexcept { return *fill_model_; }

    [[nodiscard]] std::uint64_t rejected_count() const noexcept { return rejected_count_; }

    void reset();

private:
    /// Prices `order` against the current book and books whatever fills.
    /// Returns the model's reason when nothing filled, so that an order with
    /// no second chance can be rejected with the real cause.
    RejectReason try_fill(Order& order, Timestamp now);

    void finalise_immediate(Order& order, Timestamp now, RejectReason model_reason);

    /// Whether this order gets exactly one chance at the book rather than
    /// resting for another.
    [[nodiscard]] static bool resolves_on_arrival(const Order& order) noexcept {
        return order.request.type == MS_ORD_MARKET || order.request.tif == MS_TIF_IOC;
    }

    /// The simulated flight time for an order submitted now.
    [[nodiscard]] Duration flight_time();

    void expire_day_orders(Timestamp now);

    void drop_from_working(OrderId id);

    Ledger ledger_;
    const IFillModel* fill_model_;
    const MarketView* market_;
    IFillListener* listener_ = nullptr;

    std::vector<Order> orders_;

    /// Indices into orders_, not pointers: orders_ reallocates.
    std::vector<std::size_t> working_;

    /// Scratch for on_market_event. A member rather than a local so that the
    /// per-event path does not allocate once it has warmed up; clear() keeps
    /// the capacity.
    std::vector<std::size_t> candidates_;

    DeterministicRng* jitter_ = nullptr;
    ExchangeZone zone_ = ExchangeZone::UsEastern;
    std::uint64_t late_cancels_ = 0;

    OrderId order_id_base_;
    OrderId next_sequence_ = 1;
    std::uint64_t rejected_count_ = 0;
    std::int64_t last_day_ = 0;
    bool seen_any_event_ = false;
};

}  // namespace marketscan

#endif  // MARKETSCAN_EXEC_BROKER_HPP
