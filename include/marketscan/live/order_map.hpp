#ifndef MARKETSCAN_LIVE_ORDER_MAP_HPP
#define MARKETSCAN_LIVE_ORDER_MAP_HPP

/*
 * The link between an engine order and a venue order.
 *
 * THE PROBLEM THIS SOLVES
 *
 * The venue assigns an order id, and it assigns it in the response to the
 * submit. If the only link were that id, then a connection that dropped
 * between the submit going out and the response coming back would leave an
 * order alive at the venue that this process could no longer name -- and the
 * ledger would be wrong from that moment on, quietly.
 *
 * So the link is a *client* order id: a string this process derives from its
 * own engine order id before it sends anything. It is on the order at the
 * venue whatever happens to the connection, it comes back on every trade
 * update, and it can be recomputed after a restart without having stored
 * anything. A reconnect then means: list the venue's orders, read their client
 * ids, and know exactly which engine order each one is.
 *
 * THE FORMAT
 *
 *   ms-<run>-<order id>
 *
 * `run` is a per-process token, so two runs against the same paper account
 * cannot collide -- which matters because the venue rejects a duplicate client
 * order id, and a run that silently adopted another run's orders would be
 * worse than one that was refused.
 *
 * Fixed and parseable rather than random, because the reverse direction is the
 * whole point: given a client id off the wire, this has to say which engine
 * order it is without consulting anything that a crash could have lost.
 */

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "marketscan/exec/order.hpp"

namespace marketscan::live {

/// What a lookup found, and how.
struct OrderLink {
    OrderId engine_id = kInvalidOrderId;
    std::string client_order_id;
    std::string broker_order_id;

    /// True once the venue has confirmed the order exists. False for one this
    /// process believes it submitted but has had no answer about.
    bool acknowledged = false;
};

/// Client order ids for one run, and the venue ids they turned out to mean.
class OrderMap {
public:
    /// `run_token` distinguishes this run from another against the same
    /// account. Must be non-empty and must not contain a '-', so the format
    /// stays parseable.
    ///
    /// Throws std::invalid_argument otherwise, rather than producing ids that
    /// cannot be read back -- an unparseable client id is an order this
    /// process cannot reclaim after a reconnect, which is the exact failure
    /// this class exists to prevent.
    explicit OrderMap(std::string run_token);

    /// The client order id for an engine order. Deterministic: the same engine
    /// id always produces the same string, before and after a reconnect.
    [[nodiscard]] std::string client_id_for(OrderId engine_id) const;

    /// The engine order id inside a client order id, or nullopt when the
    /// string is not one of this run's.
    ///
    /// A client id from a *different* run reads as nullopt, not as an error
    /// and not as a match. The venue's order list contains other runs' orders
    /// and adopting one would book somebody else's fills into this ledger.
    [[nodiscard]] std::optional<OrderId> engine_id_for(std::string_view client_id) const;

    /// Records that an order was sent, before any answer has arrived.
    void record_submitted(OrderId engine_id);

    /// Records the venue's own id once it is known, from the submit response
    /// or from a trade update.
    void record_acknowledged(OrderId engine_id, std::string broker_order_id);

    /// Forgets an order that has reached a terminal state.
    void retire(OrderId engine_id);

    [[nodiscard]] const OrderLink* find(OrderId engine_id) const noexcept;

    /// Orders this process sent and has had no acknowledgement for. After a
    /// reconnect these are the ones to go looking for, because they are the
    /// ones that may exist at the venue without this process knowing.
    [[nodiscard]] std::vector<OrderId> unacknowledged() const;

    /// Every live order, in engine id order. Deterministic iteration: nothing
    /// here iterates a hash container, for the same reason nothing else in the
    /// engine does.
    [[nodiscard]] std::vector<OrderLink> live() const;

    [[nodiscard]] const std::string& run_token() const noexcept { return run_token_; }

    [[nodiscard]] std::size_t size() const noexcept { return links_.size(); }

    void clear() { links_.clear(); }

private:
    std::string run_token_;
    std::map<OrderId, OrderLink> links_;
};

/// A run token from the clock and the process id.
///
/// Not random: a token a person can read off a log line and match against an
/// order at the venue is worth more than one with an extra few bits of
/// collision resistance nobody needed.
[[nodiscard]] std::string make_run_token();

}  // namespace marketscan::live

#endif  // MARKETSCAN_LIVE_ORDER_MAP_HPP
