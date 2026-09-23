#ifndef MARKETSCAN_LIVE_LIVE_BROKER_HPP
#define MARKETSCAN_LIVE_LIVE_BROKER_HPP

/*
 * An IBroker that sends orders to the paper venue.
 *
 * The same interface the fill simulator implements. A strategy submits and
 * cancels; whether that reaches a fill model or a venue was decided once, by
 * whoever built the run, and nothing below that point can tell or ask.
 *
 * WHAT IS DIFFERENT FROM THE SIMULATOR, AND WHY IT IS NOT A MODE
 *
 * The simulator answers inside submit(). This does not: it sends the order and
 * returns, and the answer arrives later on a socket. That is not a second code
 * path, it is the same code path with real latency in it -- and it is exactly
 * why the simulator grew a submit-to-fill delay and an on_order_status
 * callback before there was anything live to need them. A strategy that copes
 * with a pending order in replay copes with one here.
 *
 * ORDER IDENTITY SURVIVES THE CONNECTION
 *
 * Every order carries a client order id this process derived from its own
 * engine order id before sending anything -- see OrderMap. An order is found
 * by that string and never by the venue's id, because the venue's id is not
 * known until the submit response arrives and a dropped connection can deliver
 * a fill before it.
 *
 * RECONCILIATION IS LOUD
 *
 * This keeps a Ledger, and the venue keeps the real one. They are compared,
 * and a difference is reported as an error with both numbers. It is never
 * corrected silently: a ledger that quietly adopts the venue's position hides
 * the bug that made them differ, and the next difference is the one nobody
 * catches.
 */

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "marketscan/core/symbol_table.hpp"
#include "marketscan/exec/broker.hpp"
#include "marketscan/live/credentials.hpp"
#include "marketscan/live/http.hpp"
#include "marketscan/live/order_map.hpp"
#include "marketscan/live/order_tracker.hpp"
#include "marketscan/live/recorder.hpp"
#include "marketscan/live/trade_updates.hpp"

namespace marketscan::live {

struct LiveBrokerConfig {
    /// Distinguishes this run's orders from another run's against the same
    /// account. Generated when empty.
    std::string run_token;

    /// Starting cash for the local ledger. Should match the paper account, and
    /// reconcile() says so loudly when it does not.
    Money starting_cash = Money::from_dollars(100'000);

    /// Order ids start here, exactly as the simulator's do.
    OrderId order_id_base = 0;

    /// Reconnect backoff for the trade-updates socket.
    int reconnect_initial_ms = 1000;
    int reconnect_max_ms = 60000;

    /// Every order is capped at this many shares, whatever a strategy asks.
    ///
    /// A safety rail, not a sizing rule. An order above the cap is rejected
    /// locally and never reaches the venue.
    ///
    /// This was 10, on the stated grounds that "nothing in this milestone has
    /// any reason to send size". That premise turned out to be false: one share
    /// can never partially fill, so a ten-share cap made the partial-fill path
    /// unreachable by construction, and a partial was the one order state the
    /// live sessions never produced.
    ///
    /// Raised deliberately, and paired with `max_order_notional` below --
    /// because a share cap alone is not a safety limit when the instruments
    /// span a fourfold price range. 300 shares is $54k of a $180 stock and
    /// $232k of an $800 one, and only one of those is a sane order on a $100k
    /// account.
    Quantity max_order_quantity = 300.0;

    /// Every order is also capped at this many dollars, when a price is known.
    ///
    /// This is the rail that actually protects the account, and the share cap
    /// is the one that protects against a fat finger in a cheap instrument.
    /// Checked from the limit price, which is the only price this class has:
    /// there is no MarketView here, and a market order therefore passes this
    /// check on the share cap alone. The lifecycle exercise sends nothing but
    /// limits, for that reason among others.
    ///
    /// Zero disables it.
    Money max_order_notional = Money::from_dollars(60'000);

    /// The most gross exposure this run may hold, in dollars.
    ///
    /// The per-order caps bound one order; this bounds the position they add up
    /// to. Needed because they do add up in ways the per-order caps cannot see:
    /// a run whose orders were each $15k finished holding 599 shares of one name
    /// worth $201,317 against $99,897 of equity -- 2.0x levered, buying power
    /// zero -- because fifty *partial* fills accumulated on a leg that only ever
    /// bought.
    ///
    /// REFUSES, NEVER CLAMPS
    ///
    /// An order that would breach this is rejected and reported, exactly like
    /// the notional cap. A cap that silently resized an order down to fit would
    /// stop being able to tell anyone a sizing bug exists -- it would absorb
    /// one instead, which is the failure mode this whole project keeps finding.
    ///
    /// An order that *reduces* exposure is always allowed, whatever the cap
    /// says: refusing to close a position is how a rail turns into the thing it
    /// was protecting against.
    ///
    /// Measured from cost basis rather than from marks, because this class has
    /// no MarketView and a stale mark is worse than a known-conservative one.
    /// Zero disables it.
    Money max_gross_exposure = Money::from_dollars(150'000);

    /// Orders this run may send in total. Zero means no limit.
    ///
    /// The other rail. A loop that submits on every quote would send thousands
    /// of orders a minute at a venue that did not ask for them; this stops that
    /// at a number rather than at somebody noticing.
    std::uint64_t max_orders = 200;

    /// Marks limit orders as eligible to trade outside the regular session.
    ///
    /// Off by default, because a strategy that has not been told it is trading
    /// in a thin book should not be. It exists because the order lifecycle
    /// cannot be exercised at all outside 09:30-16:00 without it: a DAY order
    /// sent at 16:30 is accepted and queued for the next open, so a session
    /// that means to provoke a fill, a partial and a cancel race provokes
    /// none of them.
    ///
    /// The venue only honours it for limit orders with a DAY time in force,
    /// which is what the lifecycle exercise sends.
    ///
    /// This is a venue detail, not a mode. Nothing above IBroker knows the
    /// field exists, exactly as nothing above it knows how a time in force is
    /// spelled on the wire.
    bool allow_extended_hours = false;

    /// Where to record submissions and venue responses. Shared with the market
    /// data feed so that the three streams -- market data, orders out, answers
    /// back -- land in one file in one arrival order.
    ///
    /// Null means do not record, which is what every test and every synthetic
    /// run passes.
    std::shared_ptr<StreamRecorder> recorder;

    std::string user_agent = "MarketScan/0.1 (research tool)";
};

/// A difference between what this process thinks and what the venue says.
struct Divergence {
    std::string what;
    std::string ours;
    std::string theirs;
};

/// The result of comparing the local ledger against the venue.
struct Reconciliation {
    bool checked = false;

    /// Empty when the two agree. Never acted on automatically.
    std::vector<Divergence> divergences;

    /// Set when the comparison could not be made at all -- a failed request, a
    /// rate limit. Distinct from agreeing, and must not be read as agreement.
    std::string error;

    [[nodiscard]] bool agreed() const noexcept {
        return checked && error.empty() && divergences.empty();
    }
};

/// The broker's own numbers: the venue's answers, plus what this transport did
/// to stay connected.
///
/// Inherits the venue-side tallies rather than restating them, so there is one
/// definition of "how many partials were there" and a caller cannot read a
/// stale copy of it.
struct LiveBrokerStats : OrderTrackerStats {
    std::uint32_t reconnects = 0;

    /// Orders that were live when the socket dropped and were found again
    /// afterwards by their client order id.
    std::uint64_t reclaimed_after_reconnect = 0;
};

/// The live broker.
class LiveBroker final : public IBroker {
public:
    /// Throws std::invalid_argument on missing credentials, and whatever
    /// HttpClient throws for a host off the paper allowlist.
    LiveBroker(LiveBrokerConfig config, Credentials credentials, const SymbolTable* symbols);
    ~LiveBroker() override;

    /// Opens the trade-updates socket and starts the pump. Returns false if
    /// the stream refuses the credentials.
    [[nodiscard]] bool start();

    void stop();

    /// Blocks until the trade-updates stream is listening, or the timeout.
    [[nodiscard]] bool wait_until_listening(int timeout_ms);

    /// Asks the pump to drop its connection, to exercise the reclaim path.
    void request_drop_for_test();

    // --- IBroker ----------------------------------------------------------

    void set_fill_listener(IFillListener* listener) noexcept override {
        tracker_.set_fill_listener(listener);
    }

    OrderId
    submit(const MS_OrderRequest& request, Timestamp now, Timestamp decision_ts = kNever) override;

    /// Sends a cancel. Returns false when the order is already terminal here.
    ///
    /// True does not mean the order was cancelled -- only that a cancel was
    /// sent. The venue may fill it first, and that race is reported through
    /// on_order_status like any other outcome, not swallowed here.
    bool cancel(OrderId id, Timestamp now) override;

    /// Drains whatever the venue has said since the last event and applies it.
    void on_market_event(SymbolId symbol, Timestamp now) override;

    [[nodiscard]] Ledger& ledger() noexcept override { return ledger_; }

    [[nodiscard]] const Ledger& ledger() const noexcept override { return ledger_; }

    [[nodiscard]] const std::vector<Order>& orders() const noexcept override {
        return tracker_.orders();
    }

    [[nodiscard]] const Order* find_order(OrderId id) const noexcept override;
    [[nodiscard]] std::vector<const Order*> working_orders() const override;

    [[nodiscard]] std::uint64_t rejected_count() const noexcept override {
        return tracker_.stats().rejected;
    }

    [[nodiscard]] std::uint64_t late_cancel_count() const noexcept override {
        return tracker_.stats().lost_cancel_races;
    }

    /// Refuses. A live account cannot be rewound, and quietly doing nothing
    /// would let a caller believe it had been.
    void reset() override;

    [[nodiscard]] std::string_view venue() const noexcept override { return "alpaca-paper"; }

    // --- Live only --------------------------------------------------------

    /// Fetches the venue's account and positions and compares them with the
    /// local ledger. Never corrects anything.
    [[nodiscard]] Reconciliation reconcile();

    /// The comparison alone, given the two response bodies.
    ///
    /// Split out from the fetch so it can be tested without a network, the same
    /// way MarketCalendar::parse is. That matters here more than most: the live
    /// check can only be seen agreeing when the account happens to be flat, and
    /// an assertion that only ever runs in one direction is not an assertion.
    [[nodiscard]] Reconciliation compare_with(const std::string& account_body,
                                              const std::string& positions_body) const;

    /// Cancels every order this run still has working, and waits briefly for
    /// the venue to confirm. For the end of a session.
    void cancel_all_open(Timestamp now);

    [[nodiscard]] LiveBrokerStats stats() const;

    [[nodiscard]] const OrderMap& order_map() const noexcept { return tracker_.order_map(); }

    /// Whether the local rails would refuse this order, and why.
    ///
    /// `RejectReason::None` means the rails allow it -- not that the venue
    /// will. Split out from submit() so the rails can be asserted without
    /// sending anything: a test that had to call submit() to find out would
    /// have to reach the network for every case the rails *allow*, and "no test
    /// touches the network" is a property this project keeps.
    ///
    /// Not noexcept: reading the ledger's open positions allocates.
    [[nodiscard]] RejectReason check_rails(const MS_OrderRequest& request) const;

    /// Applies one trade update as if it had arrived on the socket. The pump
    /// calls this; a test calls it to drive a lifecycle without a network.
    void apply_update(const TradeUpdate& update, Timestamp now);

    /// Registers an order without sending it, so a test can drive the update
    /// path against a known order. Returns the engine id.
    OrderId register_local_order_for_test(const MS_OrderRequest& request, Timestamp now);

private:
    void run_socket();

    /// Writes one submission to the recording, when there is one.
    ///
    /// `returned_id_valid` is what submit() is about to return, not whether the
    /// venue was pleased. Replay has to answer the strategy exactly as live did,
    /// so what gets recorded is the answer the strategy got -- a rate-limited
    /// order, for instance, is left live and reported as valid even though the
    /// venue never confirmed it.
    void record_submission(const MS_OrderRequest& request,
                           OrderId engine_id,
                           bool returned_id_valid,
                           std::string_view reason);

    /// Re-reads the venue's open orders and matches them to this run's by
    /// client order id. Called after every reconnect.
    void reclaim_after_reconnect(Timestamp now);

    LiveBrokerConfig config_;
    Credentials credentials_;
    const SymbolTable* symbols_;

    /// Declared before the tracker, which borrows it.
    Ledger ledger_;

    /// The orders, and the one definition of what a venue answer means. Shared
    /// with RecordedBroker so that a replay applies responses identically --
    /// see order_tracker.hpp.
    OrderTracker tracker_;

    /// The REST client. Used from the caller's thread only.
    std::unique_ptr<HttpClient> http_;

    /// Updates the socket thread has received and the engine thread has not
    /// yet applied. A mutex rather than the SPSC ring: these are strings and
    /// vectors, the rate is a handful a second rather than thousands, and a
    /// lock held for a push is not on any path that matters.
    mutable std::mutex inbox_mutex_;
    std::vector<TradeUpdate> inbox_;
    bool reconnected_ = false;

    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> drop_requested_{false};
    std::atomic<bool> listening_{false};
    std::atomic<bool> fatal_{false};

    /// Transport-side only. The venue-side tallies live in the tracker.
    std::atomic<std::uint32_t> reconnects_{0};
    std::atomic<std::uint64_t> reclaimed_{0};

    mutable std::mutex error_mutex_;
    std::string error_;

    /// The socket the pump is using, so stop() can end a receive that would
    /// otherwise never return. The trade-updates stream is silent whenever no
    /// order is working, which is most of the time, so this is not an edge
    /// case here -- it is the common one. See WebSocket::request_close().
    mutable std::mutex socket_mutex_;
    class WebSocket* active_socket_ = nullptr;
};

}  // namespace marketscan::live

#endif  // MARKETSCAN_LIVE_LIVE_BROKER_HPP
