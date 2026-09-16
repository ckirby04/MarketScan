#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "marketscan/core/random.hpp"
#include "marketscan/exec/fill_model.hpp"

using namespace marketscan;
using Catch::Matchers::WithinRel;

namespace {

constexpr SymbolId kSymbol = 1;
constexpr Timestamp kNow = 1'700'000'000'000'000'000LL;

OrderBook two_sided(Price bid, Quantity bid_size, Price ask, Quantity ask_size) {
    OrderBook book;
    MS_Quote quote{};
    quote.ts = kNow;
    quote.symbol = kSymbol;
    quote.bid_px = bid;
    quote.bid_sz = bid_size;
    quote.ask_px = ask;
    quote.ask_sz = ask_size;
    book.apply(quote);
    return book;
}

Order market_order(Side side, Quantity quantity) {
    Order order;
    order.id = 1;
    order.request.symbol = kSymbol;
    order.request.side = side;
    order.request.type = MS_ORD_MARKET;
    order.request.tif = MS_TIF_DAY;
    order.request.quantity = quantity;
    order.submitted_ts = kNow;
    return order;
}

Order limit_order(Side side, Quantity quantity, Price limit) {
    Order order = market_order(side, quantity);
    order.request.type = MS_ORD_LIMIT;
    order.request.limit_price = limit;
    return order;
}

FillResult price_it(const IFillModel& model, const Order& order, const OrderBook& book) {
    FillRequest request;
    request.order = &order;
    request.book = &book;
    request.ts = kNow;
    return model.evaluate(request);
}

/// No commission, so that a test about price is only about price.
FillModelConfig bare() {
    FillModelConfig config;
    config.commission = CommissionSchedule{};
    config.commission.per_share = Money{};
    return config;
}

}  // namespace

// --------------------------------------------------------------------------
// The property the whole build rests on
// --------------------------------------------------------------------------

// A market order crosses the spread. Always. This is the single assumption
// that separates an honest backtest from one that quietly pays itself the
// spread on every trade.
TEST_CASE("a market order never fills better than the touch", "[fills]") {
    const SpreadCrossingFillModel model(bare());
    const OrderBook book = two_sided(99.95, 500, 100.05, 500);

    SECTION("buys lift the ask") {
        const Order order = market_order(MS_SIDE_BUY, 100);
        const FillResult result = price_it(model, order, book);

        REQUIRE(result.filled());
        REQUIRE(result.quantity == 100);
        REQUIRE_THAT(result.price, WithinRel(100.05, 1e-12));
        REQUIRE(result.price >= book.best_ask());
        REQUIRE(result.price > book.mid());
    }

    SECTION("sells hit the bid") {
        const Order order = market_order(MS_SIDE_SELL, 100);
        const FillResult result = price_it(model, order, book);

        REQUIRE(result.filled());
        REQUIRE_THAT(result.price, WithinRel(99.95, 1e-12));
        REQUIRE(result.price <= book.best_bid());
        REQUIRE(result.price < book.mid());
    }
}

// Swept over a wide range of sizes, including sizes far past the displayed
// touch, because the impact term is where a sign error would hide.
TEST_CASE("no size of market order ever beats the touch", "[fills]") {
    const SpreadCrossingFillModel model(bare());
    const OrderBook book = two_sided(99.95, 500, 100.05, 500);

    DeterministicRng rng(4242);
    Quantity worst_buy_size = 0.0;
    Quantity worst_sell_size = 0.0;
    bool buys_always_pay_up = true;
    bool sells_always_pay_down = true;

    for (int i = 0; i < 2'000; ++i) {
        const Quantity size = rng.uniform_range(1.0, 50'000.0);

        const FillResult buy = price_it(model, market_order(MS_SIDE_BUY, size), book);
        if (!buy.filled() || buy.price < book.best_ask()) {
            buys_always_pay_up = false;
            worst_buy_size = size;
        }
        const FillResult sell = price_it(model, market_order(MS_SIDE_SELL, size), book);
        if (!sell.filled() || sell.price > book.best_bid()) {
            sells_always_pay_down = false;
            worst_sell_size = size;
        }
    }
    INFO("first offending buy size: " << worst_buy_size);
    INFO("first offending sell size: " << worst_sell_size);
    REQUIRE(buys_always_pay_up);
    REQUIRE(sells_always_pay_down);
}

TEST_CASE("size beyond the touch costs more, monotonically", "[fills]") {
    const SpreadCrossingFillModel model(bare());
    const OrderBook book = two_sided(99.95, 500, 100.05, 500);

    const FillResult small = price_it(model, market_order(MS_SIDE_BUY, 100), book);
    const FillResult at_touch = price_it(model, market_order(MS_SIDE_BUY, 500), book);
    const FillResult over = price_it(model, market_order(MS_SIDE_BUY, 1'000), book);
    const FillResult huge = price_it(model, market_order(MS_SIDE_BUY, 10'000), book);

    // Everything within the displayed size pays exactly the touch.
    REQUIRE_THAT(small.price, WithinRel(100.05, 1e-12));
    REQUIRE_THAT(at_touch.price, WithinRel(100.05, 1e-12));

    REQUIRE(over.price > at_touch.price);
    REQUIRE(huge.price > over.price);

    // Twice the displayed size pays the full impact coefficient, 10bp, on the
    // half that is not displayed: (500*100.05 + 500*100.05*1.001) / 1000.
    const double expected = (500 * 100.05 + 500 * 100.05 * 1.001) / 1000.0;
    REQUIRE_THAT(over.price, WithinRel(expected, 1e-12));

    // Everything fills. That is the chosen semantics: size is expensive, not
    // impossible.
    REQUIRE(huge.quantity == 10'000);
}

TEST_CASE("an order walks real depth level by level", "[fills][book]") {
    const SpreadCrossingFillModel model(bare());

    OrderBook book;
    book.set_levels({BookLevel{99.90, 100}, BookLevel{99.80, 200}},
                    {BookLevel{100.00, 100}, BookLevel{100.10, 200}, BookLevel{100.25, 300}},
                    kNow);

    SECTION("stopping part way through the second level") {
        const FillResult result = price_it(model, market_order(MS_SIDE_BUY, 250), book);
        REQUIRE(result.quantity == 250);
        // 100 @ 100.00 then 150 @ 100.10
        REQUIRE_THAT(result.price, WithinRel((100 * 100.00 + 150 * 100.10) / 250.0, 1e-12));
    }

    SECTION("consuming the whole book") {
        const FillResult result = price_it(model, market_order(MS_SIDE_BUY, 600), book);
        REQUIRE(result.quantity == 600);
        REQUIRE_THAT(result.price,
                     WithinRel((100 * 100.00 + 200 * 100.10 + 300 * 100.25) / 600.0, 1e-12));
        REQUIRE(result.price > book.best_ask());
    }

    SECTION("a sell walks down the bids") {
        const FillResult result = price_it(model, market_order(MS_SIDE_SELL, 300), book);
        REQUIRE(result.quantity == 300);
        REQUIRE_THAT(result.price, WithinRel((100 * 99.90 + 200 * 99.80) / 300.0, 1e-12));
        REQUIRE(result.price <= book.best_bid());
    }
}

// --------------------------------------------------------------------------
// Limit orders
// --------------------------------------------------------------------------

TEST_CASE("a limit order never trades through its own price", "[fills][limit]") {
    const SpreadCrossingFillModel model(bare());
    const OrderBook book = two_sided(99.95, 500, 100.05, 500);

    SECTION("below the market it does not fill at all") {
        const FillResult result = price_it(model, limit_order(MS_SIDE_BUY, 100, 99.00), book);
        REQUIRE_FALSE(result.filled());
    }

    SECTION("at the touch it fills the displayed size and no more") {
        const FillResult result = price_it(model, limit_order(MS_SIDE_BUY, 2'000, 100.05), book);
        REQUIRE(result.quantity == 500);
        REQUIRE_THAT(result.price, WithinRel(100.05, 1e-12));
    }

    SECTION("above the touch it reaches exactly as far as the limit allows") {
        // 10bp above the touch buys exactly one more displayed size worth,
        // since impact_bps = 10 * sqrt(excess / displayed).
        const Price limit = 100.05 * 1.001;
        const FillResult result = price_it(model, limit_order(MS_SIDE_BUY, 5'000, limit), book);
        REQUIRE_THAT(result.quantity, WithinRel(1'000.0, 1e-9));
        REQUIRE(result.price <= limit);
    }

    SECTION("a sell limit above the bid does not fill") {
        const FillResult result = price_it(model, limit_order(MS_SIDE_SELL, 100, 101.00), book);
        REQUIRE_FALSE(result.filled());
    }

    SECTION("a marketable sell limit fills at the bid, not at its limit") {
        const FillResult result = price_it(model, limit_order(MS_SIDE_SELL, 100, 99.00), book);
        REQUIRE(result.quantity == 100);
        REQUIRE_THAT(result.price, WithinRel(99.95, 1e-12));
    }
}

TEST_CASE("a limit order sweeping depth stops at its price", "[fills][limit]") {
    const SpreadCrossingFillModel model(bare());
    OrderBook book;
    book.set_levels(
        {}, {BookLevel{100.00, 100}, BookLevel{100.10, 200}, BookLevel{100.25, 300}}, kNow);

    const FillResult result = price_it(model, limit_order(MS_SIDE_BUY, 600, 100.10), book);
    REQUIRE(result.quantity == 300);  // the 100.25 level is beyond the limit
    REQUIRE_THAT(result.price, WithinRel((100 * 100.00 + 200 * 100.10) / 300.0, 1e-12));
}

// --------------------------------------------------------------------------
// Configuration
// --------------------------------------------------------------------------

TEST_CASE("extra slippage widens the fill further", "[fills]") {
    FillModelConfig config = bare();
    config.slippage_bps = 5.0;
    const SpreadCrossingFillModel model(config);
    const OrderBook book = two_sided(99.95, 500, 100.05, 500);

    const FillResult buy = price_it(model, market_order(MS_SIDE_BUY, 100), book);
    const FillResult sell = price_it(model, market_order(MS_SIDE_SELL, 100), book);

    REQUIRE_THAT(buy.price, WithinRel(100.05 * 1.0005, 1e-12));
    REQUIRE_THAT(sell.price, WithinRel(99.95 * 0.9995, 1e-12));
    REQUIRE(buy.price > book.best_ask());
    REQUIRE(sell.price < book.best_bid());
}

TEST_CASE("commission is charged per unit with a floor", "[fills][commission]") {
    CommissionSchedule schedule;
    schedule.per_share = Money::from_double(0.005);
    schedule.minimum = Money::from_double(1.00);

    REQUIRE(schedule.charge(1'000) == Money::from_double(5.00));
    REQUIRE(schedule.charge(100) == Money::from_double(1.00));  // floored
    REQUIRE(schedule.charge(0) == Money::from_double(1.00));

    schedule.minimum = Money{};
    schedule.per_order = Money::from_double(0.35);
    REQUIRE(schedule.charge(100) == Money::from_double(0.85));
}

TEST_CASE("orders with nothing to trade against are refused", "[fills]") {
    const SpreadCrossingFillModel model(bare());

    SECTION("an empty book") {
        const OrderBook book;
        const FillResult result = price_it(model, market_order(MS_SIDE_BUY, 100), book);
        REQUIRE_FALSE(result.filled());
        REQUIRE(result.reason == RejectReason::NoMarketData);
    }

    SECTION("a one-sided book has nothing for the aggressor") {
        OrderBook book;
        book.set_levels({BookLevel{99.95, 500}}, {}, kNow);
        const FillResult result = price_it(model, market_order(MS_SIDE_BUY, 100), book);
        REQUIRE_FALSE(result.filled());
        REQUIRE(result.reason == RejectReason::NoMarketData);
    }

    SECTION("a nonsense quantity") {
        const OrderBook book = two_sided(99.95, 500, 100.05, 500);
        const FillResult result = price_it(model, market_order(MS_SIDE_BUY, 0), book);
        REQUIRE(result.reason == RejectReason::InvalidQuantity);
    }
}

// --------------------------------------------------------------------------
// The optimistic model, and what it is for
// --------------------------------------------------------------------------

TEST_CASE("the optimistic model declares itself unrealistic", "[fills][optimistic]") {
    const OptimisticFillModel optimistic;
    const SpreadCrossingFillModel realistic;

    REQUIRE_FALSE(optimistic.is_realistic());
    REQUIRE(realistic.is_realistic());
    REQUIRE(optimistic.name().find("unrealistic") != std::string_view::npos);
}

TEST_CASE("the optimistic model fills everything at the last print", "[fills][optimistic]") {
    const OptimisticFillModel model(bare());

    OrderBook book = two_sided(99.95, 500, 100.05, 500);
    MS_Trade trade{};
    trade.ts = kNow;
    trade.symbol = kSymbol;
    trade.price = 100.00;
    book.apply(trade);

    const FillResult buy = price_it(model, market_order(MS_SIDE_BUY, 1'000'000), book);
    REQUIRE(buy.quantity == 1'000'000);  // in full, at any size
    REQUIRE_THAT(buy.price, WithinRel(100.00, 1e-12));

    // Better than the ask it should have paid. That is the whole point.
    REQUIRE(buy.price < book.best_ask());
}

// The number this comparison exists to show: what a naive fill assumption is
// quietly awarding a strategy on every single trade.
TEST_CASE("the gap between the two models is the cost of the assumption", "[fills][optimistic]") {
    const SpreadCrossingFillModel realistic(bare());
    const OptimisticFillModel optimistic(bare());

    OrderBook book = two_sided(99.95, 500, 100.05, 500);
    MS_Trade trade{};
    trade.ts = kNow;
    trade.symbol = kSymbol;
    trade.price = 100.00;
    book.apply(trade);

    const Order buy = market_order(MS_SIDE_BUY, 500);
    const double honest = price_it(realistic, buy, book).price;
    const double wishful = price_it(optimistic, buy, book).price;

    REQUIRE(honest > wishful);
    // Half the spread, per side, per trade.
    REQUIRE_THAT(honest - wishful, WithinRel(0.05, 1e-9));
}

TEST_CASE("even the optimistic model honours a limit price", "[fills][optimistic]") {
    const OptimisticFillModel model(bare());
    OrderBook book = two_sided(99.95, 500, 100.05, 500);
    MS_Trade trade{};
    trade.ts = kNow;
    trade.symbol = kSymbol;
    trade.price = 100.00;
    book.apply(trade);

    REQUIRE_FALSE(price_it(model, limit_order(MS_SIDE_BUY, 100, 99.00), book).filled());
    REQUIRE(price_it(model, limit_order(MS_SIDE_BUY, 100, 101.00), book).filled());
}
