#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "marketscan/core/random.hpp"
#include "marketscan/exec/broker.hpp"

using namespace marketscan;
using Catch::Matchers::WithinRel;

namespace {

constexpr SymbolId kSymbol = 1;
constexpr SymbolId kOther = 2;
constexpr Timestamp kNow = 1'700'000'000'000'000'000LL;

Money dollars(std::int64_t amount) {
    return Money::from_dollars(amount);
}

/// Books a fill with no commission, so a test about position maths is only
/// about position maths.
void fill(Ledger& ledger,
          Side side,
          Quantity quantity,
          Price price,
          SymbolId symbol = kSymbol,
          Money commission = Money{}) {
    static OrderId next_id = 1;
    ledger.apply_fill(kNow, next_id++, 0, symbol, side, quantity, price, price, commission);
}

}  // namespace

// --------------------------------------------------------------------------
// The invariant
// --------------------------------------------------------------------------

// equity == starting_cash + realized - commission + unrealized.
//
// This holds exactly, not to within rounding, and a failure means value was
// created or destroyed somewhere in the ledger.
TEST_CASE("the equity invariant holds after every operation", "[ledger]") {
    Ledger ledger(dollars(100'000));
    REQUIRE(ledger.check_invariant());

    fill(ledger, MS_SIDE_BUY, 100, 50.25, kSymbol, Money::from_double(0.50));
    REQUIRE(ledger.check_invariant());

    ledger.mark(kSymbol, 51.00);
    REQUIRE(ledger.check_invariant());

    fill(ledger, MS_SIDE_SELL, 40, 51.10, kSymbol, Money::from_double(0.20));
    REQUIRE(ledger.check_invariant());

    fill(ledger, MS_SIDE_SELL, 200, 50.90, kSymbol, Money::from_double(1.00));  // flips short
    REQUIRE(ledger.check_invariant());

    ledger.mark(kSymbol, 49.00);
    REQUIRE(ledger.check_invariant());

    fill(ledger, MS_SIDE_BUY, 140, 48.95, kSymbol, Money::from_double(0.70));  // back to flat
    REQUIRE(ledger.check_invariant());

    REQUIRE(ledger.position(kSymbol) == 0.0);
    REQUIRE(ledger.unrealized_pnl().is_zero());
}

// Randomised, because the paths that break an accounting identity are the ones
// nobody thought to write a case for: awkward sizes, repeated flips, prices
// that do not land on a cent.
TEST_CASE("the invariant survives a long random sequence", "[ledger]") {
    Ledger ledger(dollars(10'000'000));
    DeterministicRng rng(20'260'916);

    bool held = true;
    Price price = 100.0;

    for (int i = 0; i < 20'000; ++i) {
        price *= std::exp(rng.normal() * 0.002);

        const Side side = rng.uniform() < 0.5 ? MS_SIDE_BUY : MS_SIDE_SELL;
        const Quantity quantity = rng.uniform_range(0.1, 500.0);
        const SymbolId symbol = rng.uniform() < 0.5 ? kSymbol : kOther;
        const Money commission = Money::from_double(rng.uniform_range(0.0, 2.0));

        fill(ledger, side, quantity, price, symbol, commission);
        ledger.mark(symbol, price);

        if (!ledger.check_invariant()) {
            held = false;
            break;
        }
    }

    REQUIRE(held);
    REQUIRE(ledger.blotter().size() == 20'000);
}

// --------------------------------------------------------------------------
// Position accounting
// --------------------------------------------------------------------------

TEST_CASE("a long round trip books the right P&L", "[ledger]") {
    Ledger ledger(dollars(100'000));

    fill(ledger, MS_SIDE_BUY, 100, 50.00);
    REQUIRE(ledger.position(kSymbol) == 100);
    REQUIRE(ledger.cash() == dollars(95'000));
    REQUIRE(ledger.find_position(kSymbol)->average_cost() == 50.00);
    REQUIRE(ledger.realized_pnl().is_zero());

    ledger.mark(kSymbol, 55.00);
    REQUIRE(ledger.unrealized_pnl() == dollars(500));
    REQUIRE(ledger.equity() == dollars(100'500));

    fill(ledger, MS_SIDE_SELL, 100, 55.00);
    REQUIRE(ledger.position(kSymbol) == 0);
    REQUIRE(ledger.realized_pnl() == dollars(500));
    REQUIRE(ledger.unrealized_pnl().is_zero());
    REQUIRE(ledger.cash() == dollars(100'500));
    REQUIRE(ledger.equity() == dollars(100'500));
}

// Shorting is allowed and the proceeds are credited immediately. Borrow cost
// is not modelled, which is a real simplification and is documented as one.
TEST_CASE("a short round trip credits proceeds and books P&L", "[ledger][short]") {
    Ledger ledger(dollars(100'000));

    fill(ledger, MS_SIDE_SELL, 100, 50.00);
    REQUIRE(ledger.position(kSymbol) == -100);
    REQUIRE(ledger.cash() == dollars(105'000));
    REQUIRE(ledger.find_position(kSymbol)->average_cost() == 50.00);

    // The position is worth -5000 against the +5000 of cash it produced, so
    // opening a short changes nothing about equity.
    ledger.mark(kSymbol, 50.00);
    REQUIRE(ledger.equity() == dollars(100'000));
    REQUIRE(ledger.unrealized_pnl().is_zero());

    // A short loses as the price rises.
    ledger.mark(kSymbol, 55.00);
    REQUIRE(ledger.unrealized_pnl() == dollars(-500));
    REQUIRE(ledger.equity() == dollars(99'500));

    ledger.mark(kSymbol, 45.00);
    REQUIRE(ledger.unrealized_pnl() == dollars(500));

    fill(ledger, MS_SIDE_BUY, 100, 45.00);
    REQUIRE(ledger.position(kSymbol) == 0);
    REQUIRE(ledger.realized_pnl() == dollars(500));
    REQUIRE(ledger.equity() == dollars(100'500));
    REQUIRE(ledger.check_invariant());
}

TEST_CASE("adding to a position averages the cost", "[ledger]") {
    Ledger ledger(dollars(100'000));

    fill(ledger, MS_SIDE_BUY, 100, 50.00);
    fill(ledger, MS_SIDE_BUY, 100, 60.00);

    REQUIRE(ledger.position(kSymbol) == 200);
    REQUIRE_THAT(ledger.find_position(kSymbol)->average_cost(), WithinRel(55.00, 1e-12));
    REQUIRE(ledger.realized_pnl().is_zero());
}

TEST_CASE("a partial close books only the part that closed", "[ledger]") {
    Ledger ledger(dollars(100'000));

    fill(ledger, MS_SIDE_BUY, 100, 50.00);
    fill(ledger, MS_SIDE_SELL, 40, 55.00);

    REQUIRE(ledger.position(kSymbol) == 60);
    REQUIRE(ledger.realized_pnl() == dollars(200));  // 40 * 5
    // The basis of what is left is untouched by the sale.
    REQUIRE_THAT(ledger.find_position(kSymbol)->average_cost(), WithinRel(50.00, 1e-12));

    ledger.mark(kSymbol, 55.00);
    REQUIRE(ledger.unrealized_pnl() == dollars(300));  // 60 * 5
    REQUIRE(ledger.check_invariant());
}

// Selling more than is held closes the long and opens a short in one fill.
// The rounding here is the reason the ledger derives the opening notional by
// subtraction instead of recomputing it.
TEST_CASE("a fill can flip a position through zero", "[ledger]") {
    Ledger ledger(dollars(100'000));

    fill(ledger, MS_SIDE_BUY, 100, 50.00);
    fill(ledger, MS_SIDE_SELL, 250, 60.00);

    REQUIRE(ledger.position(kSymbol) == -150);
    REQUIRE(ledger.realized_pnl() == dollars(1'000));  // 100 * 10
    // The new short's basis is the price it opened at, not a blend.
    REQUIRE_THAT(ledger.find_position(kSymbol)->average_cost(), WithinRel(60.00, 1e-12));

    ledger.mark(kSymbol, 60.00);
    REQUIRE(ledger.unrealized_pnl().is_zero());
    REQUIRE(ledger.check_invariant());
}

TEST_CASE("a flat position leaves no rounding crumb behind", "[ledger]") {
    Ledger ledger(dollars(100'000));

    // A price and size whose notional does not land on a whole micro-dollar.
    fill(ledger, MS_SIDE_BUY, 3.0, 33.3333335);
    fill(ledger, MS_SIDE_SELL, 3.0, 33.3333335);

    REQUIRE(ledger.position(kSymbol) == 0.0);
    REQUIRE(ledger.find_position(kSymbol)->cost_basis.is_zero());

    // A crumb left in the basis would show up here as unrealized P&L on a
    // position that does not exist.
    ledger.mark(kSymbol, 1'000.0);
    REQUIRE(ledger.unrealized_pnl().is_zero());
    REQUIRE(ledger.check_invariant());
}

TEST_CASE("positions are reported in ascending symbol order", "[ledger]") {
    Ledger ledger(dollars(100'000));
    fill(ledger, MS_SIDE_BUY, 10, 10.0, 3);
    fill(ledger, MS_SIDE_BUY, 10, 10.0, 1);
    fill(ledger, MS_SIDE_BUY, 10, 10.0, 2);

    const auto open = ledger.open_positions();
    REQUIRE(open.size() == 3);
    REQUIRE(open[0]->symbol == 1);
    REQUIRE(open[1]->symbol == 2);
    REQUIRE(open[2]->symbol == 3);
}

// --------------------------------------------------------------------------
// The blotter
// --------------------------------------------------------------------------

TEST_CASE("slippage is measured against what the strategy could see", "[ledger][blotter]") {
    Ledger ledger(dollars(100'000));

    // Saw a mid of 100.00, paid 100.05.
    ledger.apply_fill(
        kNow, 1, 42, kSymbol, MS_SIDE_BUY, 100, 100.05, 100.00, Money::from_double(0.50));

    const TradeRecord& record = ledger.blotter().back();
    REQUIRE(record.client_order_id == 42);
    REQUIRE(record.slippage == dollars(5));  // 100 shares * 0.05, a cost
    REQUIRE(record.commission == Money::from_double(0.50));
    REQUIRE(record.cash_delta == Money::from_double(-10'005.50));

    // Selling below the mid is a cost too, so slippage stays positive.
    ledger.apply_fill(kNow, 2, 43, kSymbol, MS_SIDE_SELL, 100, 99.95, 100.00, Money{});
    REQUIRE(ledger.blotter().back().slippage == dollars(5));
}

TEST_CASE("the blotter reconstructs cash exactly", "[ledger][blotter]") {
    Ledger ledger(dollars(100'000));
    DeterministicRng rng(7);

    for (int i = 0; i < 500; ++i) {
        fill(ledger,
             rng.uniform() < 0.5 ? MS_SIDE_BUY : MS_SIDE_SELL,
             rng.uniform_range(1.0, 100.0),
             rng.uniform_range(10.0, 200.0),
             kSymbol,
             Money::from_double(rng.uniform_range(0.0, 1.0)));
    }

    Money reconstructed = ledger.starting_cash();
    for (const TradeRecord& record : ledger.blotter()) {
        reconstructed += record.cash_delta;
    }
    REQUIRE(reconstructed == ledger.cash());
}

// --------------------------------------------------------------------------
// Equity curve
// --------------------------------------------------------------------------

TEST_CASE("the equity curve samples on an interval", "[ledger]") {
    Ledger ledger(dollars(100'000));
    ledger.set_equity_sample_interval(time::kMinute);

    ledger.maybe_sample_equity(kNow);
    ledger.maybe_sample_equity(kNow + 30 * time::kSecond);  // too soon
    ledger.maybe_sample_equity(kNow + 61 * time::kSecond);
    ledger.maybe_sample_equity(kNow + 62 * time::kSecond);  // too soon

    REQUIRE(ledger.equity_curve().size() == 2);
    REQUIRE(ledger.equity_curve().front().ts == kNow);
    REQUIRE(ledger.equity_curve().front().equity == dollars(100'000));
}

TEST_CASE("reset returns the ledger to its opening state", "[ledger]") {
    Ledger ledger(dollars(100'000));
    fill(ledger, MS_SIDE_BUY, 100, 50.0);
    ledger.sample_equity(kNow);

    ledger.reset();

    REQUIRE(ledger.cash() == dollars(100'000));
    REQUIRE(ledger.equity() == dollars(100'000));
    REQUIRE(ledger.position(kSymbol) == 0.0);
    REQUIRE(ledger.blotter().empty());
    REQUIRE(ledger.equity_curve().empty());
    REQUIRE(ledger.check_invariant());
}

// --------------------------------------------------------------------------
// The cash account, through the broker
// --------------------------------------------------------------------------

namespace {

/// A broker wired to a one-symbol market, for the order-level tests.
struct Desk {
    MarketView market;
    SpreadCrossingFillModel model;
    Broker broker;

    explicit Desk(Money starting_cash, FillModelConfig config = {})
        : model(config), broker(starting_cash, &model, &market) {}

    void quote(Price bid, Quantity bid_size, Price ask, Quantity ask_size, Timestamp ts = kNow) {
        MS_Quote q{};
        q.ts = ts;
        q.symbol = kSymbol;
        q.bid_px = bid;
        q.bid_sz = bid_size;
        q.ask_px = ask;
        q.ask_sz = ask_size;
        market.book_for_update(kSymbol).apply(q);
        broker.on_market_event(kSymbol, ts);
    }

    MS_OrderRequest order(Side side, Quantity quantity) const {
        MS_OrderRequest request{};
        request.symbol = kSymbol;
        request.side = side;
        request.type = MS_ORD_MARKET;
        request.tif = MS_TIF_DAY;
        request.quantity = quantity;
        return request;
    }
};

FillModelConfig no_commission() {
    FillModelConfig config;
    config.commission.per_share = Money{};
    return config;
}

}  // namespace

// This is a cash account. An order that would overdraw is refused whole, with
// a reason, rather than silently levering the strategy.
TEST_CASE("an order that would overdraw is refused whole", "[ledger][broker][cash]") {
    Desk desk(dollars(10'000), no_commission());
    desk.quote(99.95, 10'000, 100.05, 10'000);

    const OrderId rejected = desk.broker.submit(desk.order(MS_SIDE_BUY, 1'000), kNow);

    REQUIRE(rejected == kInvalidOrderId);
    REQUIRE(desk.broker.orders().size() == 1);
    REQUIRE(desk.broker.orders().back().status == OrderStatus::Rejected);
    REQUIRE(desk.broker.orders().back().reject_reason == RejectReason::InsufficientCash);

    // Refused whole: nothing was bought, and the cash is untouched.
    REQUIRE(desk.broker.ledger().position(kSymbol) == 0.0);
    REQUIRE(desk.broker.ledger().cash() == dollars(10'000));
    REQUIRE(desk.broker.ledger().blotter().empty());

    // An order it can afford still works.
    const OrderId accepted = desk.broker.submit(desk.order(MS_SIDE_BUY, 50), kNow);
    REQUIRE(accepted != kInvalidOrderId);
    REQUIRE(desk.broker.ledger().position(kSymbol) == 50);
    REQUIRE(desk.broker.ledger().cash() > Money{});
}

TEST_CASE("cash never goes negative across a long run", "[ledger][broker][cash]") {
    Desk desk(dollars(50'000));
    DeterministicRng rng(99);

    Price price = 100.0;
    bool stayed_solvent = true;

    for (int i = 0; i < 3'000; ++i) {
        price *= std::exp(rng.normal() * 0.003);
        desk.quote(price * 0.9995, 500, price * 1.0005, 500, kNow + i * time::kSecond);

        const Side side = rng.uniform() < 0.5 ? MS_SIDE_BUY : MS_SIDE_SELL;
        desk.broker.submit(desk.order(side, rng.uniform_range(1.0, 400.0)),
                           kNow + i * time::kSecond);

        if (desk.broker.ledger().cash() < Money{} || !desk.broker.ledger().check_invariant()) {
            stayed_solvent = false;
            break;
        }
    }

    REQUIRE(stayed_solvent);
    // The account did trade; this is not vacuously true.
    REQUIRE(desk.broker.ledger().blotter().size() > 100);
}

// A short sale credits cash, so it is never the thing that overdraws an
// account -- which is what makes "shorting allowed" and "cash account"
// compatible rather than contradictory.
TEST_CASE("shorting is always affordable", "[ledger][broker][short]") {
    Desk desk(dollars(1'000), no_commission());
    desk.quote(99.95, 100'000, 100.05, 100'000);

    const OrderId id = desk.broker.submit(desk.order(MS_SIDE_SELL, 10'000), kNow);

    REQUIRE(id != kInvalidOrderId);
    REQUIRE(desk.broker.ledger().position(kSymbol) == -10'000);
    REQUIRE(desk.broker.ledger().cash() > dollars(1'000));
    REQUIRE(desk.broker.ledger().check_invariant());
}

TEST_CASE("an order before any market data is refused", "[broker]") {
    Desk desk(dollars(100'000));
    const OrderId id = desk.broker.submit(desk.order(MS_SIDE_BUY, 10), kNow);

    REQUIRE(id == kInvalidOrderId);
    REQUIRE(desk.broker.orders().back().reject_reason == RejectReason::NoMarketData);
}

TEST_CASE("nonsense orders are rejected and recorded", "[broker]") {
    Desk desk(dollars(100'000));
    desk.quote(99.95, 500, 100.05, 500);

    MS_OrderRequest zero = desk.order(MS_SIDE_BUY, 0);
    REQUIRE(desk.broker.submit(zero, kNow) == kInvalidOrderId);

    MS_OrderRequest bad_limit = desk.order(MS_SIDE_BUY, 10);
    bad_limit.type = MS_ORD_LIMIT;
    bad_limit.limit_price = -5.0;
    REQUIRE(desk.broker.submit(bad_limit, kNow) == kInvalidOrderId);

    REQUIRE(desk.broker.orders().size() == 2);
    REQUIRE(desk.broker.orders()[0].reject_reason == RejectReason::InvalidQuantity);
    REQUIRE(desk.broker.orders()[1].reject_reason == RejectReason::InvalidPrice);
    REQUIRE(desk.broker.rejected_count() == 2);
}

// --------------------------------------------------------------------------
// Resting orders
// --------------------------------------------------------------------------

TEST_CASE("a limit order rests until its price arrives", "[broker][limit]") {
    Desk desk(dollars(100'000), no_commission());
    desk.quote(99.95, 500, 100.05, 500);

    MS_OrderRequest request = desk.order(MS_SIDE_BUY, 100);
    request.type = MS_ORD_LIMIT;
    request.limit_price = 99.00;
    request.tif = MS_TIF_GTC;

    const OrderId id = desk.broker.submit(request, kNow);
    REQUIRE(id != kInvalidOrderId);
    REQUIRE(desk.broker.working_orders().size() == 1);
    REQUIRE(desk.broker.ledger().position(kSymbol) == 0.0);

    // Still too expensive.
    desk.quote(99.40, 500, 99.50, 500, kNow + time::kSecond);
    REQUIRE(desk.broker.working_orders().size() == 1);

    // Now the ask is inside the limit.
    desk.quote(98.90, 500, 98.95, 500, kNow + 2 * time::kSecond);
    REQUIRE(desk.broker.working_orders().empty());
    REQUIRE(desk.broker.ledger().position(kSymbol) == 100);
    REQUIRE(desk.broker.find_order(id)->status == OrderStatus::Filled);
    // It filled at the market, not at its limit.
    REQUIRE_THAT(desk.broker.find_order(id)->average_fill_price, WithinRel(98.95, 1e-12));
}

TEST_CASE("a resting order can be cancelled", "[broker][limit]") {
    Desk desk(dollars(100'000), no_commission());
    desk.quote(99.95, 500, 100.05, 500);

    MS_OrderRequest request = desk.order(MS_SIDE_BUY, 100);
    request.type = MS_ORD_LIMIT;
    request.limit_price = 90.00;
    request.tif = MS_TIF_GTC;

    const OrderId id = desk.broker.submit(request, kNow);
    REQUIRE(desk.broker.cancel(id, kNow));
    REQUIRE(desk.broker.working_orders().empty());
    REQUIRE(desk.broker.find_order(id)->status == OrderStatus::Cancelled);

    // Cancelling twice, or cancelling something unknown, is simply false.
    REQUIRE_FALSE(desk.broker.cancel(id, kNow));
    REQUIRE_FALSE(desk.broker.cancel(9'999, kNow));

    // And it does not fill afterwards.
    desk.quote(80.00, 500, 80.05, 500, kNow + time::kSecond);
    REQUIRE(desk.broker.ledger().position(kSymbol) == 0.0);
}

TEST_CASE("an IOC order does not rest", "[broker][limit]") {
    Desk desk(dollars(100'000), no_commission());
    desk.quote(99.95, 500, 100.05, 500);

    MS_OrderRequest request = desk.order(MS_SIDE_BUY, 100);
    request.type = MS_ORD_LIMIT;
    request.limit_price = 90.00;
    request.tif = MS_TIF_IOC;

    REQUIRE(desk.broker.submit(request, kNow) == kInvalidOrderId);
    REQUIRE(desk.broker.working_orders().empty());
    REQUIRE(desk.broker.orders().back().status == OrderStatus::Rejected);
    REQUIRE(desk.broker.orders().back().reject_reason == RejectReason::NotMarketable);
}

TEST_CASE("a DAY order expires when the date changes", "[broker][limit]") {
    Desk desk(dollars(100'000), no_commission());
    desk.quote(99.95, 500, 100.05, 500);

    MS_OrderRequest request = desk.order(MS_SIDE_BUY, 100);
    request.type = MS_ORD_LIMIT;
    request.limit_price = 90.00;
    request.tif = MS_TIF_DAY;

    const OrderId id = desk.broker.submit(request, kNow);
    REQUIRE(desk.broker.working_orders().size() == 1);

    desk.quote(99.95, 500, 100.05, 500, kNow + time::kDay);
    REQUIRE(desk.broker.working_orders().empty());
    REQUIRE(desk.broker.find_order(id)->status == OrderStatus::Expired);
}

TEST_CASE("a GTC order outlives the session", "[broker][limit]") {
    Desk desk(dollars(100'000), no_commission());
    desk.quote(99.95, 500, 100.05, 500);

    MS_OrderRequest request = desk.order(MS_SIDE_BUY, 100);
    request.type = MS_ORD_LIMIT;
    request.limit_price = 90.00;
    request.tif = MS_TIF_GTC;

    desk.broker.submit(request, kNow);
    desk.quote(99.95, 500, 100.05, 500, kNow + 5 * time::kDay);
    REQUIRE(desk.broker.working_orders().size() == 1);
}

TEST_CASE("fills are reported to the listener with the client id", "[broker][abi]") {
    struct Recorder final : IFillListener {
        std::vector<MS_Fill> fills;

        void on_fill(const MS_Fill& fill) override { fills.push_back(fill); }
    };

    Desk desk(dollars(100'000), no_commission());
    Recorder recorder;
    desk.broker.set_fill_listener(&recorder);
    desk.quote(99.95, 500, 100.05, 500);

    MS_OrderRequest request = desk.order(MS_SIDE_BUY, 100);
    request.client_order_id = 7'777;
    const OrderId id = desk.broker.submit(request, kNow);

    REQUIRE(recorder.fills.size() == 1);
    REQUIRE(recorder.fills[0].order_id == id);
    REQUIRE(recorder.fills[0].client_order_id == 7'777);
    REQUIRE(recorder.fills[0].quantity == 100);
    REQUIRE(recorder.fills[0].side == MS_SIDE_BUY);
    REQUIRE_THAT(recorder.fills[0].price, WithinRel(100.05, 1e-12));
}
