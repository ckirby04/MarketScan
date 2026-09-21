#ifndef MARKETSCAN_LIVE_TRADE_UPDATES_HPP
#define MARKETSCAN_LIVE_TRADE_UPDATES_HPP

/*
 * What the venue says happened to an order.
 *
 * Parsed apart from the broker that acts on it, for the same reason the market
 * data parser is apart from the feed: this is the part with all the cases, and
 * the cases are testable without a socket. Every message shape in
 * tests/test_trade_updates.cpp is written by hand to the vendor's documented
 * form.
 *
 * WHY THE TERMINAL STATES ARE ENUMERATED RATHER THAN INFERRED
 *
 * A broker that treated "anything I do not recognise" as "still working" would
 * leave an order alive forever after a message it had not been taught about --
 * and the strategy would wait for a fill that already happened, or a cancel
 * that already succeeded. So an unrecognised event is its own outcome,
 * counted and logged, and never quietly folded into a state that looks fine.
 */

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "marketscan/core/types.hpp"
#include "marketscan/exec/order.hpp"

namespace marketscan::live {

/// The lifecycle events this build understands.
enum class UpdateKind : std::uint8_t {
    /// Routed and live at the venue. Not terminal.
    Accepted = 0,
    PartialFill = 1,
    Fill = 2,
    Cancelled = 3,
    Expired = 4,
    Rejected = 5,
    Replaced = 6,
    /// A venue-side state that is not terminal and that this build does not
    /// act on: pending_new, pending_cancel, calculated, stopped, suspended.
    /// Recognised so it is not mistaken for something unknown.
    Transient = 7,
    /// A message kind this build has never seen. Never treated as harmless.
    Unknown = 8,
};

[[nodiscard]] std::string_view to_string(UpdateKind kind) noexcept;

/// Whether this kind ends an order's life.
[[nodiscard]] constexpr bool is_terminal(UpdateKind kind) noexcept {
    return kind == UpdateKind::Fill || kind == UpdateKind::Cancelled ||
           kind == UpdateKind::Expired || kind == UpdateKind::Rejected ||
           kind == UpdateKind::Replaced;
}

/// One trade update, as this build needs it.
struct TradeUpdate {
    UpdateKind kind = UpdateKind::Unknown;

    /// The venue's own event string, kept verbatim. A log line that says
    /// "unknown event 'order_replace_rejected'" is worth more than one that
    /// says "unknown event".
    std::string raw_event;

    /// The id this process assigned when it submitted. The only durable link
    /// between an engine order and a venue order -- see OrderMap.
    std::string client_order_id;

    /// The venue's id for the order. Useful for a cancel and for a log line;
    /// never used to find an order, because it is not known until the submit
    /// response arrives and a reconnect may see a fill before that.
    std::string broker_order_id;

    std::string symbol;

    /// When the venue says it happened.
    Timestamp ts = 0;

    /// This event's own fill, for a fill or partial_fill. Zero otherwise.
    Quantity fill_quantity = 0.0;
    Price fill_price = 0.0;

    /// The order's cumulative filled quantity as the venue sees it, which is
    /// what reconciliation compares against rather than a running sum of our
    /// own -- a sum that drops one message is a sum that is quietly wrong.
    Quantity cumulative_filled = 0.0;
    Price average_fill_price = 0.0;

    /// What the venue said the order was for, so a fill can be booked even if
    /// the submit response never arrived.
    Quantity order_quantity = 0.0;
    Side side = MS_SIDE_BUY;

    /// Present on a rejection. Empty otherwise.
    std::string reason;

    [[nodiscard]] bool terminal() const noexcept { return is_terminal(kind); }
};

struct TradeUpdateParse {
    std::vector<TradeUpdate> updates;

    /// Non-market messages: the authorization reply, the listen reply.
    std::vector<std::string> controls;

    /// The stream said authorization failed. Fatal -- never retried.
    bool unauthorised = false;

    /// Set when the text was not JSON or not a shape this understands at all.
    std::string error;
};

/// Parses one trade-updates socket message.
[[nodiscard]] TradeUpdateParse parse_trade_update(std::string_view text);

}  // namespace marketscan::live

#endif  // MARKETSCAN_LIVE_TRADE_UPDATES_HPP
