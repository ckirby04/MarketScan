#include "marketscan/live/recorded_broker.hpp"

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <utility>

#include "marketscan/live/detail.hpp"
#include "marketscan/live/trade_updates.hpp"
#include "marketscan/util/logging.hpp"

namespace marketscan::live {
namespace {

[[nodiscard]] const char* side_name(Side side) noexcept {
    return side == MS_SIDE_SELL ? "sell" : "buy";
}

[[nodiscard]] const char* type_name(MS_OrderType type) noexcept {
    return type == MS_ORD_LIMIT ? "limit" : "market";
}

using detail::tif_name;

[[nodiscard]] std::string describe(std::string_view symbol,
                                   Side side,
                                   Quantity quantity,
                                   MS_OrderType type,
                                   MS_TimeInForce tif,
                                   Price limit) {
    char buffer[192];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%s %s %.4f %s tif=%s limit=%.4f",
                  side_name(side),
                  std::string(symbol).c_str(),
                  quantity,
                  type_name(type),
                  tif_name(tif),
                  limit);
    return buffer;
}

/// Prices and quantities are compared exactly.
///
/// Both sides computed them from the same doubles in the same order, so a
/// tolerance here would hide a real difference rather than absorb a rounding
/// one -- the same rule the decision comparison follows. If this ever needs a
/// tolerance, that is the finding.
[[nodiscard]] bool same(double a, double b) noexcept {
    return a == b;
}

}  // namespace

std::string CorrespondenceFailure::describe() const {
    if (!happened) {
        return "no correspondence failure";
    }
    return "submission " + std::to_string(index) + " does not match the recording (" + field +
           ")\n  recorded " + expected + "\n  replayed " + actual;
}

RecordedBroker::RecordedBroker(std::vector<RecordedSubmission> submissions,
                               std::vector<RecordedResponse> responses,
                               const SymbolTable* symbols,
                               Money starting_cash,
                               OrderId order_id_base,
                               std::string run_token)
    : submissions_(std::move(submissions)),
      responses_(std::move(responses)),
      symbols_(symbols),
      ledger_(starting_cash),
      tracker_(&ledger_, std::move(run_token)),
      order_id_base_(order_id_base) {
    if (symbols_ == nullptr) {
        throw std::invalid_argument("RecordedBroker needs a symbol table to name instruments");
    }
}

void RecordedBroker::reset() {
    throw std::logic_error(
        "RecordedBroker::reset(): a recording is a fixed sequence of answers. Replaying it again "
        "means a new broker and a new ledger, not a rewind.");
}

bool RecordedBroker::corresponds(const RecordedSubmission& expected,
                                 const MS_OrderRequest& actual) {
    const char* ticker = symbols_->c_name(actual.symbol);
    const std::string actual_symbol = ticker != nullptr ? ticker : "";

    const char* field = nullptr;
    if (actual_symbol != expected.symbol) {
        field = "symbol";
    } else if (actual.side != expected.side) {
        field = "side";
    } else if (actual.type != expected.type) {
        field = "type";
    } else if (actual.tif != expected.tif) {
        field = "time in force";
    } else if (!same(actual.quantity, expected.quantity)) {
        field = "quantity";
    } else if (actual.type == MS_ORD_LIMIT && !same(actual.limit_price, expected.limit_price)) {
        field = "limit price";
    }

    if (field == nullptr) {
        return true;
    }

    failure_.happened = true;
    failure_.index = cursor_;
    failure_.field = field;
    failure_.expected = describe(expected.symbol,
                                 expected.side,
                                 expected.quantity,
                                 expected.type,
                                 expected.tif,
                                 expected.limit_price);
    failure_.actual = describe(
        actual_symbol, actual.side, actual.quantity, actual.type, actual.tif, actual.limit_price);

    log::write(MS_LOG_ERROR, "replay", failure_.describe());
    return false;
}

OrderId
RecordedBroker::submit(const MS_OrderRequest& request, Timestamp now, Timestamp decision_ts) {
    if (cursor_ >= submissions_.size()) {
        // The strategy asked for more orders than the live run did. That is a
        // divergence and it is reported here rather than absorbed, because the
        // alternative is inventing an answer.
        if (!failure_.happened) {
            failure_.happened = true;
            failure_.index = cursor_;
            failure_.field = "count";
            failure_.expected = "(the recording has no further submissions)";
            const char* ticker = symbols_->c_name(request.symbol);
            failure_.actual = describe(ticker != nullptr ? ticker : "",
                                       request.side,
                                       request.quantity,
                                       request.type,
                                       request.tif,
                                       request.limit_price);
            log::write(MS_LOG_ERROR, "replay", failure_.describe());
        }
        ++cursor_;
        return kInvalidOrderId;
    }

    const RecordedSubmission& expected = submissions_[cursor_];
    const bool matched = corresponds(expected, request);
    ++cursor_;

    if (!matched) {
        // Still register it, so the decision log the equivalence test reads is
        // complete and the comparison can name the same order this did.
        (void)tracker_.register_refusal(request,
                                        now,
                                        decision_ts,
                                        order_id_base_,
                                        RejectReason::NotMarketable,
                                        "replay diverged from the recording here");
        return kInvalidOrderId;
    }

    if (!expected.accepted) {
        // The live run's submit returned nothing for this order, so this one
        // must too -- and it must look the same to a strategy that reacts to a
        // refusal, which is the whole point of this class.
        const OrderId refused = tracker_.register_refusal(
            request,
            now,
            decision_ts,
            order_id_base_,
            RejectReason::NotMarketable,
            expected.reason.empty() ? "refused by the venue (recorded)" : expected.reason.c_str());
        (void)refused;
        return kInvalidOrderId;
    }

    // Registered under *the recording's own* engine id, not one derived from a
    // base. Every client order id in the recording is `ms-<run>-<engine id>`, so
    // an id that differs derives a different client id and routes none of the
    // recorded responses.
    //
    // This was a base before, and the live run's base and the replay's differed:
    // all 391 responses of one session were dequeued and matched nothing, while
    // the decisions still agreed and the run reported EQUIVALENT. Taking the id
    // from the recording removes the opportunity rather than checking for it.
    return tracker_.register_order_with_id(request, now, decision_ts, expected.engine_id);
}

bool RecordedBroker::cancel(OrderId id, Timestamp now) {
    (void)now;
    const Order* order = tracker_.find_order(id);
    if (order == nullptr || order->is_terminal()) {
        // Exactly what the live broker answers for an order already finished,
        // and counted the same way: a cancel that lost its race.
        tracker_.note_lost_cancel_race();
        return false;
    }
    // The cancel was sent. What it achieved arrives as a recorded response.
    return true;
}

void RecordedBroker::on_market_event(SymbolId symbol, Timestamp now) {
    (void)symbol;
    const std::uint64_t upto =
        draining_ ? UINT64_MAX : (position_ != nullptr ? position_->sequence : 0);
    while (delivered_ < responses_.size() && responses_[delivered_].sequence <= upto) {
        const RecordedResponse& response = responses_[delivered_];
        ++delivered_;

        const TradeUpdateParse parsed = parse_trade_update(response.raw);
        if (!parsed.error.empty()) {
            log::write(MS_LOG_WARN, "replay", parsed.error);
            continue;
        }
        for (const TradeUpdate& update : parsed.updates) {
            tracker_.apply_update(update, now);
        }
    }
}

void RecordedBroker::drain_remaining(Timestamp now) {
    draining_ = true;
    on_market_event(kInvalidSymbol, now);
}

}  // namespace marketscan::live
