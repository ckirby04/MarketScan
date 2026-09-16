// MarketScan's Python bindings.
//
// The point of these is research: load a dataset, run every theory against it,
// and get the numbers out as plain Python so they can be plotted, sliced or
// fed to something else. They call exactly the same run_backtest the command
// line does, through the same engine, ledger and fill models, so a notebook
// and the desktop application cannot disagree about what happened.
//
// Two deliberate choices about what is *not* exposed:
//
//   - Writing a strategy in Python. The engine's contract is that a strategy
//     is a pure function of the events it was shown, driven by a clock the
//     host owns. Hanging the GIL off the per-event path would make replay
//     speed depend on the interpreter, and a Python strategy holding a
//     reference to anything outside the engine would make a run
//     irreproducible. Strategies are DLLs; see theories/README.md.
//
//   - Money as a float. Cash is exact integer micro-dollars, and a binding
//     that quietly turned it into a double would reintroduce the drift the
//     type exists to prevent. Money is bound as its own class with an
//     explicit .dollars for when a float is genuinely wanted.

#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

#include "marketscan/core/random.hpp"
#include "marketscan/data/csv_feed.hpp"
#include "marketscan/data/synthetic_feed.hpp"
#include "marketscan/metrics/correlation.hpp"
#include "marketscan/metrics/performance.hpp"
#include "marketscan/metrics/rolling.hpp"
#include "marketscan/run/backtest.hpp"
#include "marketscan/util/logging.hpp"
#include "marketscan/util/version.hpp"

namespace py = pybind11;
using namespace marketscan;

namespace {

/// Wraps a metric that takes a span so Python can hand it any sequence.
template<typename Fn>
auto over_span(Fn fn) {
    return [fn](const std::vector<double>& values) { return fn(std::span<const double>(values)); };
}

void bind_money(py::module_& m) {
    py::class_<Money>(m, "Money", R"doc(
Exact cash, in micro-dollars.

Binary floating point cannot represent 0.01, so a double ledger drifts:
accumulate a cent a million times and the answer is wrong in a way that looks
like P&L. Every cash quantity in MarketScan is an integer count of
micro-dollars, and this class is that integer.

Use ``.dollars`` when a float is genuinely what you want -- to plot, or to
compute a ratio. Never to accumulate.
)doc")
        .def(py::init<>())
        .def_static("from_dollars",
                    &Money::from_dollars,
                    py::arg("dollars"),
                    "Exact: whole dollars carry no rounding.")
        .def_static(
            "from_double", &Money::from_double, py::arg("dollars"), "Rounds half away from zero.")
        .def_static("from_micros", &Money::from_micros, py::arg("micros"))
        .def_property_readonly(
            "dollars", &Money::to_double, "Lossy by construction. For display and ratios.")
        .def_property_readonly("micros", &Money::micros, "The exact underlying integer.")
        .def(py::self + py::self)
        .def(py::self - py::self)
        .def(-py::self)
        .def(py::self == py::self)
        .def(py::self < py::self)
        .def(py::self > py::self)
        .def("__float__", &Money::to_double)
        .def("__repr__", [](const Money& self) { return "Money(" + self.to_string() + ")"; })
        .def("__str__", &Money::to_string);
}

void bind_enums(py::module_& m) {
    py::enum_<MS_Side>(m, "Side").value("BUY", MS_SIDE_BUY).value("SELL", MS_SIDE_SELL);

    py::enum_<MS_OrderType>(m, "OrderType")
        .value("MARKET", MS_ORD_MARKET)
        .value("LIMIT", MS_ORD_LIMIT);

    py::enum_<MS_TimeInForce>(m, "TimeInForce")
        .value("DAY", MS_TIF_DAY)
        .value("IOC", MS_TIF_IOC)
        .value("GTC", MS_TIF_GTC);

    py::enum_<MS_LogLevel>(m, "LogLevel")
        .value("DEBUG", MS_LOG_DEBUG)
        .value("INFO", MS_LOG_INFO)
        .value("WARN", MS_LOG_WARN)
        .value("ERROR", MS_LOG_ERROR);

    py::enum_<PluginState>(m, "PluginState")
        .value("DISCOVERED", PluginState::Discovered)
        .value("LOADED", PluginState::Loaded)
        .value("FAILED", PluginState::Failed);
}

void bind_time(py::module_& m) {
    py::module_ t = m.def_submodule("time", "Timestamps. Nanoseconds since the Unix epoch, UTC.");
    t.attr("SECOND") = time::kSecond;
    t.attr("MINUTE") = time::kMinute;
    t.attr("HOUR") = time::kHour;
    t.attr("DAY") = time::kDay;
    t.def("to_iso8601", &time::to_iso8601, py::arg("ts"));
    t.def(
        "from_iso8601",
        [](const std::string& text) -> py::object {
            Timestamp ts = 0;
            if (!time::from_iso8601(text, ts)) {
                // None rather than an exception: parsing an unknown string is
                // a question, and "no" is an answer.
                return py::none();
            }
            return py::cast(ts);
        },
        py::arg("text"),
        "Parses an ISO-8601 UTC timestamp, or returns None. Numeric offsets "
        "are refused: honouring one silently would make the same file mean "
        "different things depending on where it was written.");
}

void bind_symbols_and_feeds(py::module_& m) {
    py::class_<SymbolTable>(m, "SymbolTable", "Tickers to dense ids, assigned in first-seen order.")
        .def(py::init<>())
        .def("intern", &SymbolTable::intern, py::arg("ticker"))
        .def("find", &SymbolTable::find, py::arg("ticker"), "0 if the ticker is unknown.")
        .def(
            "name",
            [](const SymbolTable& self, SymbolId id) { return std::string(self.name(id)); },
            py::arg("symbol"))
        .def("__len__", &SymbolTable::count)
        .def("ids", &SymbolTable::ids);

    py::class_<FeedSpan>(m, "FeedSpan")
        .def_readonly("first", &FeedSpan::first)
        .def_readonly("last", &FeedSpan::last)
        .def_readonly("event_count", &FeedSpan::event_count)
        .def_readonly("known", &FeedSpan::known);

    py::class_<SyntheticInstrument>(m, "SyntheticInstrument")
        .def(py::init<>())
        .def_readwrite("ticker", &SyntheticInstrument::ticker)
        .def_readwrite("initial_price", &SyntheticInstrument::initial_price)
        .def_readwrite("annual_drift", &SyntheticInstrument::annual_drift)
        .def_readwrite("annual_volatility", &SyntheticInstrument::annual_volatility)
        .def_readwrite("mean_volume", &SyntheticInstrument::mean_volume);

    py::class_<SyntheticFeedConfig>(m, "SyntheticFeedConfig", R"doc(
Geometric Brownian motion, seeded and reproducible.

Not a market model. It exists so the engine can be exercised without a data
file and so tests have an input that is the same on every machine. Nothing
about a strategy's performance against it means anything.
)doc")
        .def(py::init<>())
        .def_readwrite("instruments", &SyntheticFeedConfig::instruments)
        .def_readwrite("start", &SyntheticFeedConfig::start)
        .def_readwrite("bar_interval", &SyntheticFeedConfig::bar_interval)
        .def_readwrite("bars_per_session", &SyntheticFeedConfig::bars_per_session)
        .def_readwrite("sessions", &SyntheticFeedConfig::sessions)
        .def_readwrite("seed", &SyntheticFeedConfig::seed)
        .def_readwrite("emit_quotes", &SyntheticFeedConfig::emit_quotes)
        .def_readwrite("spread_bps", &SyntheticFeedConfig::spread_bps);

    py::class_<CsvFeedOptions>(m, "CsvFeedOptions")
        .def(py::init<>())
        .def_readwrite("default_symbol", &CsvFeedOptions::default_symbol)
        .def_readwrite("synthesize_quotes", &CsvFeedOptions::synthesize_quotes)
        .def_readwrite("spread_bps", &CsvFeedOptions::spread_bps)
        .def_readwrite("quote_size", &CsvFeedOptions::quote_size);

    // The feeds themselves, for reading a dataset without running anything.
    m.def(
        "read_csv",
        [](const std::filesystem::path& path, CsvFeedOptions options) {
            SymbolTable symbols;
            CsvFeed feed(path, symbols, std::move(options));

            py::list bars;
            MarketEvent event;
            while (feed.next(event)) {
                if (event.type != EventType::Bar) {
                    continue;
                }
                py::dict row;
                row["ts"] = event.bar.ts;
                row["symbol"] = std::string(symbols.name(event.bar.symbol));
                row["open"] = event.bar.open;
                row["high"] = event.bar.high;
                row["low"] = event.bar.low;
                row["close"] = event.bar.close;
                row["volume"] = event.bar.volume;
                bars.append(std::move(row));
            }
            return bars;
        },
        py::arg("path"),
        py::arg("options") = CsvFeedOptions{},
        R"doc(
Reads a CSV of bars into a list of dicts.

A malformed row raises with the file and line, never a silently skipped row:
quietly dropping input is how a backtest ends up describing a dataset that
does not exist.
)doc");
}

void bind_ledger(py::module_& m) {
    py::class_<Position>(m, "Position")
        .def_readonly("symbol", &Position::symbol)
        .def_readonly("quantity", &Position::quantity, "Signed. Negative is short.")
        .def_readonly("cost_basis", &Position::cost_basis)
        .def_readonly("mark", &Position::mark)
        .def_readonly("realized_pnl", &Position::realized_pnl)
        .def_property_readonly("average_cost", &Position::average_cost)
        .def_property_readonly("market_value", &Position::market_value)
        .def_property_readonly("unrealized_pnl", &Position::unrealized_pnl)
        .def("__repr__", [](const Position& self) {
            return "Position(symbol=" + std::to_string(self.symbol) +
                   ", quantity=" + std::to_string(self.quantity) + ")";
        });

    py::class_<TradeRecord>(m, "Trade", "One row of the blotter.")
        .def_readonly("ts", &TradeRecord::ts)
        .def_readonly("order_id", &TradeRecord::order_id)
        .def_readonly("client_order_id", &TradeRecord::client_order_id)
        .def_readonly("symbol", &TradeRecord::symbol)
        .def_readonly("side", &TradeRecord::side)
        .def_readonly("quantity", &TradeRecord::quantity)
        .def_readonly("reference_price",
                      &TradeRecord::reference_price,
                      "The mid the strategy could see when it decided.")
        .def_readonly("fill_price", &TradeRecord::fill_price, "What was actually paid.")
        .def_readonly("slippage",
                      &TradeRecord::slippage,
                      "Positive means the fill was worse than the reference price.")
        .def_readonly("commission", &TradeRecord::commission)
        .def_readonly("cash_delta", &TradeRecord::cash_delta)
        .def_readonly("realized_pnl", &TradeRecord::realized_pnl);

    py::class_<EquityPoint>(m, "EquityPoint")
        .def_readonly("ts", &EquityPoint::ts)
        .def_readonly("equity", &EquityPoint::equity)
        .def_readonly("cash", &EquityPoint::cash);

    py::class_<Ledger>(m, "Ledger", R"doc(
One strategy's virtual account.

Semantics fixed for this build, and deliberately not invented silently:
shorting is allowed and the proceeds are credited immediately, with borrow
cost NOT modelled; this is a cash account, so an order that would overdraw is
refused whole rather than filled part way; and there are no corporate actions.
)doc")
        .def(py::init<Money>(), py::arg("starting_cash"))
        .def_property_readonly("starting_cash", &Ledger::starting_cash)
        .def_property_readonly("cash", &Ledger::cash)
        .def_property_readonly("equity", &Ledger::equity)
        .def_property_readonly("market_value", &Ledger::market_value)
        .def_property_readonly("realized_pnl", &Ledger::realized_pnl)
        .def_property_readonly("unrealized_pnl", &Ledger::unrealized_pnl)
        .def_property_readonly("commission_paid", &Ledger::commission_paid)
        .def_property_readonly("total_pnl", &Ledger::total_pnl)
        .def("position", &Ledger::position, py::arg("symbol"))
        .def("mark", &Ledger::mark, py::arg("symbol"), py::arg("price"))
        .def_property_readonly("blotter", &Ledger::blotter)
        .def_property_readonly("equity_curve", &Ledger::equity_curve)
        .def("check_invariant",
             &Ledger::check_invariant,
             "equity == starting_cash + realized - commission + unrealized.\n"
             "Holds exactly, not to within rounding. False means value was "
             "created or destroyed, which is a bug.");
}

void bind_metrics(py::module_& m) {
    py::module_ mt = m.def_submodule("metrics", R"doc(
Performance statistics.

Undefined results are NaN, never zero. A Sharpe ratio of zero means "no
risk-adjusted edge", which is a specific claim; a series too short to have one
is not making that claim. Check ``math.isfinite`` before using a result.
)doc");

    py::class_<metrics::Annualisation>(mt, "Annualisation")
        .def(py::init<>())
        .def_readwrite("periods_per_year", &metrics::Annualisation::periods_per_year)
        .def_readwrite("risk_free_rate", &metrics::Annualisation::risk_free_rate);

    py::class_<metrics::DrawdownResult>(mt, "Drawdown")
        .def_readonly("max_drawdown", &metrics::DrawdownResult::max_drawdown)
        .def_readonly("peak_index", &metrics::DrawdownResult::peak_index)
        .def_readonly("trough_index", &metrics::DrawdownResult::trough_index)
        .def_readonly("recovery_index", &metrics::DrawdownResult::recovery_index)
        .def_readonly("underwater_periods", &metrics::DrawdownResult::underwater_periods)
        .def_property_readonly("recovered", &metrics::DrawdownResult::recovered);

    py::class_<metrics::Regression>(mt, "Regression")
        .def_readonly("beta", &metrics::Regression::beta)
        .def_readonly("alpha", &metrics::Regression::alpha)
        .def_readonly("r_squared", &metrics::Regression::r_squared);

    py::class_<metrics::PerformanceSummary>(mt, "Summary")
        .def_readonly("samples", &metrics::PerformanceSummary::samples)
        .def_readonly("years", &metrics::PerformanceSummary::years)
        .def_readonly("starting_equity", &metrics::PerformanceSummary::starting_equity)
        .def_readonly("final_equity", &metrics::PerformanceSummary::final_equity)
        .def_readonly("total_return", &metrics::PerformanceSummary::total_return)
        .def_readonly("cagr", &metrics::PerformanceSummary::cagr)
        .def_readonly("volatility", &metrics::PerformanceSummary::volatility)
        .def_readonly("sharpe", &metrics::PerformanceSummary::sharpe)
        .def_readonly("sortino", &metrics::PerformanceSummary::sortino)
        .def_readonly("max_drawdown", &metrics::PerformanceSummary::max_drawdown)
        .def_readonly("calmar", &metrics::PerformanceSummary::calmar)
        .def_readonly("beta", &metrics::PerformanceSummary::beta)
        .def_readonly("alpha", &metrics::PerformanceSummary::alpha)
        .def_readonly("r_squared", &metrics::PerformanceSummary::r_squared)
        .def_readonly("information_ratio", &metrics::PerformanceSummary::information_ratio)
        .def_readonly("tracking_error", &metrics::PerformanceSummary::tracking_error)
        .def_readonly("correlation", &metrics::PerformanceSummary::correlation);

    mt.def("is_defined", &metrics::is_defined, py::arg("value"));
    mt.def("mean", over_span(&metrics::mean), py::arg("values"));
    mt.def("variance",
           over_span(&metrics::variance),
           py::arg("values"),
           "Sample variance, dividing by n-1. Two-pass.");
    mt.def("stddev", over_span(&metrics::stddev), py::arg("values"));
    mt.def(
        "simple_returns",
        [](const std::vector<double>& levels) {
            return metrics::simple_returns(std::span<const double>(levels));
        },
        py::arg("levels"));
    mt.def(
        "log_returns",
        [](const std::vector<double>& levels) {
            return metrics::log_returns(std::span<const double>(levels));
        },
        py::arg("levels"));
    mt.def("total_return", over_span(&metrics::total_return), py::arg("levels"));
    mt.def("cagr",
           py::overload_cast<double, double, double>(&metrics::cagr),
           py::arg("first_level"),
           py::arg("last_level"),
           py::arg("years"));

    mt.def(
        "sharpe",
        [](const std::vector<double>& returns, double periods_per_year, double risk_free) {
            metrics::Annualisation a;
            a.periods_per_year = periods_per_year;
            a.risk_free_rate = risk_free;
            return metrics::sharpe(std::span<const double>(returns), a);
        },
        py::arg("returns"),
        py::arg("periods_per_year") = 252.0,
        py::arg("risk_free") = 0.0,
        "NaN when volatility is zero: a series that never moved has no "
        "risk-adjusted return, and infinity would top the leaderboard.");

    mt.def(
        "sortino",
        [](const std::vector<double>& returns, double periods_per_year, double risk_free) {
            metrics::Annualisation a;
            a.periods_per_year = periods_per_year;
            a.risk_free_rate = risk_free;
            return metrics::sortino(std::span<const double>(returns), a);
        },
        py::arg("returns"),
        py::arg("periods_per_year") = 252.0,
        py::arg("risk_free") = 0.0);

    mt.def(
        "volatility",
        [](const std::vector<double>& returns, double periods_per_year) {
            metrics::Annualisation a;
            a.periods_per_year = periods_per_year;
            return metrics::annualised_volatility(std::span<const double>(returns), a);
        },
        py::arg("returns"),
        py::arg("periods_per_year") = 252.0);

    mt.def("max_drawdown", over_span(&metrics::max_drawdown), py::arg("levels"));
    mt.def(
        "drawdown_series",
        [](const std::vector<double>& levels) {
            return metrics::drawdown_series(std::span<const double>(levels));
        },
        py::arg("levels"));
    mt.def("calmar", &metrics::calmar, py::arg("annual_return"), py::arg("max_drawdown"));

    mt.def(
        "regress",
        [](const std::vector<double>& returns,
           const std::vector<double>& benchmark,
           double periods_per_year) {
            metrics::Annualisation a;
            a.periods_per_year = periods_per_year;
            return metrics::regress(
                std::span<const double>(returns), std::span<const double>(benchmark), a);
        },
        py::arg("returns"),
        py::arg("benchmark_returns"),
        py::arg("periods_per_year") = 252.0);

    mt.def(
        "information_ratio",
        [](const std::vector<double>& returns,
           const std::vector<double>& benchmark,
           double periods_per_year) {
            metrics::Annualisation a;
            a.periods_per_year = periods_per_year;
            return metrics::information_ratio(
                std::span<const double>(returns), std::span<const double>(benchmark), a);
        },
        py::arg("returns"),
        py::arg("benchmark_returns"),
        py::arg("periods_per_year") = 252.0);

    mt.def(
        "correlation",
        [](const std::vector<double>& a, const std::vector<double>& b) {
            return metrics::correlation(std::span<const double>(a), std::span<const double>(b));
        },
        py::arg("a"),
        py::arg("b"),
        "NaN when either series is constant: a flat line has no correlation "
        "with anything, which is not the same as zero correlation.");

    mt.def(
        "covariance",
        [](const std::vector<double>& a, const std::vector<double>& b) {
            return metrics::covariance(std::span<const double>(a), std::span<const double>(b));
        },
        py::arg("a"),
        py::arg("b"));

    mt.def("infer_periods_per_year",
           &metrics::infer_periods_per_year,
           py::arg("sample_interval"),
           py::arg("trading_days_per_year") = 252.0,
           py::arg("hours_per_session") = 6.5,
           "Respects the session: annualising one-minute returns as though "
           "markets trade overnight inflates volatility by about 2.3x.");

    // --- Rolling ---
    mt.def(
        "rolling_mean",
        [](const std::vector<double>& values, std::size_t window) {
            return metrics::rolling_mean(std::span<const double>(values), window);
        },
        py::arg("values"),
        py::arg("window"));
    mt.def(
        "rolling_stddev",
        [](const std::vector<double>& values, std::size_t window) {
            return metrics::rolling_stddev(std::span<const double>(values), window);
        },
        py::arg("values"),
        py::arg("window"));
    mt.def(
        "rolling_sharpe",
        [](const std::vector<double>& returns, std::size_t window, double periods_per_year) {
            metrics::Annualisation a;
            a.periods_per_year = periods_per_year;
            return metrics::rolling_sharpe(std::span<const double>(returns), window, a);
        },
        py::arg("returns"),
        py::arg("window"),
        py::arg("periods_per_year") = 252.0);
    mt.def(
        "rolling_correlation",
        [](const std::vector<double>& a, const std::vector<double>& b, std::size_t window) {
            return metrics::rolling_correlation(
                std::span<const double>(a), std::span<const double>(b), window);
        },
        py::arg("a"),
        py::arg("b"),
        py::arg("window"));
    mt.def(
        "relative_strength",
        [](const std::vector<double>& levels, const std::vector<double>& benchmark) {
            return metrics::relative_strength(std::span<const double>(levels),
                                              std::span<const double>(benchmark));
        },
        py::arg("levels"),
        py::arg("benchmark_levels"),
        "Both rebased to 1, so a flat stretch means they moved together.");
}

void bind_backtest(py::module_& m) {
    py::class_<ParamInfo>(m, "ParamInfo")
        .def_readonly("name", &ParamInfo::name)
        .def_readonly("description", &ParamInfo::description)
        .def_readonly("min_value", &ParamInfo::min_value)
        .def_readonly("max_value", &ParamInfo::max_value)
        .def_readonly("default_value", &ParamInfo::default_value);

    py::class_<PluginInfo>(m, "PluginInfo")
        .def_readonly("path", &PluginInfo::path)
        .def_readonly("id", &PluginInfo::id)
        .def_readonly("display_name", &PluginInfo::display_name)
        .def_readonly("author", &PluginInfo::author)
        .def_readonly("description", &PluginInfo::description)
        .def_readonly("abi_version", &PluginInfo::abi_version)
        .def_readonly("params", &PluginInfo::params)
        .def_readonly("required_symbols", &PluginInfo::required_symbols)
        .def_readonly("state", &PluginInfo::state)
        .def_readonly("error", &PluginInfo::error, "Verbatim, when the plugin was refused.")
        .def("__repr__", [](const PluginInfo& self) {
            return "PluginInfo(id='" + self.id + "', state=" + std::string(to_string(self.state)) +
                   ")";
        });

    py::class_<LatencyHistogram>(m, "Latency")
        .def_property_readonly("count", &LatencyHistogram::count)
        .def_property_readonly("mean", &LatencyHistogram::mean)
        .def_property_readonly("min", &LatencyHistogram::min)
        .def_property_readonly("max", &LatencyHistogram::max)
        .def_property_readonly("p50", &LatencyHistogram::p50)
        .def_property_readonly("p90", &LatencyHistogram::p90)
        .def_property_readonly("p99", &LatencyHistogram::p99)
        .def_property_readonly("p999", &LatencyHistogram::p999);

    py::class_<run::BacktestConfig>(m, "BacktestConfig")
        .def(py::init<>())
        .def_readwrite("data_file",
                       &run::BacktestConfig::data_file,
                       "A CSV to replay. Empty generates synthetic data instead.")
        .def_readwrite("symbol", &run::BacktestConfig::symbol)
        .def_readwrite("bars_per_session", &run::BacktestConfig::bars_per_session)
        .def_readwrite("sessions", &run::BacktestConfig::sessions)
        .def_readwrite("seed", &run::BacktestConfig::seed)
        .def_readwrite("starting_cash", &run::BacktestConfig::starting_cash)
        .def_readwrite("equity_sample_interval", &run::BacktestConfig::equity_sample_interval)
        .def_readwrite("theory_roots", &run::BacktestConfig::theory_roots)
        .def_readwrite("benchmark_id", &run::BacktestConfig::benchmark_id)
        .def_readwrite("optimistic_fills",
                       &run::BacktestConfig::optimistic_fills,
                       "Fills everything at the last trade price. The results "
                       "are not a result; it exists so the two can be "
                       "subtracted.")
        .def_readwrite("spread_bps", &run::BacktestConfig::spread_bps);

    py::class_<run::TheoryResult>(m, "TheoryResult")
        .def_readonly("id", &run::TheoryResult::id)
        .def_readonly("display_name", &run::TheoryResult::display_name)
        .def_readonly("author", &run::TheoryResult::author)
        .def_readonly("is_benchmark", &run::TheoryResult::is_benchmark)
        .def_readonly("starting_cash", &run::TheoryResult::starting_cash)
        .def_readonly("final_equity", &run::TheoryResult::final_equity)
        .def_readonly("cash", &run::TheoryResult::cash)
        .def_readonly("realized_pnl", &run::TheoryResult::realized_pnl)
        .def_readonly("unrealized_pnl", &run::TheoryResult::unrealized_pnl)
        .def_readonly("commission", &run::TheoryResult::commission)
        .def_readonly("slippage", &run::TheoryResult::slippage)
        .def_readonly("trades", &run::TheoryResult::trades)
        .def_readonly("rejected", &run::TheoryResult::rejected)
        .def_readonly("summary", &run::TheoryResult::summary)
        .def_readonly(
            "times", &run::TheoryResult::times, "Seconds since the Unix epoch, for plotting.")
        .def_readonly("equity_curve", &run::TheoryResult::equity_curve)
        .def_readonly("returns", &run::TheoryResult::returns)
        .def_readonly("drawdown", &run::TheoryResult::drawdown)
        .def_readonly("blotter", &run::TheoryResult::blotter)
        .def_readonly("positions", &run::TheoryResult::positions)
        .def_readonly("latency", &run::TheoryResult::latency)
        .def_readonly("invariant_held",
                      &run::TheoryResult::invariant_held,
                      "False means the ledger created or destroyed value.")
        .def_property_readonly(
            "published",
            [](const run::TheoryResult& self) {
                py::dict out;
                for (const auto& [name, value] : self.published) {
                    out[py::str(name)] = value;
                }
                return out;
            },
            "Whatever the theory passed to publish_metric.")
        .def("__repr__", [](const run::TheoryResult& self) {
            return "TheoryResult('" + self.id + "', equity=" + self.final_equity.to_string() +
                   ", trades=" + std::to_string(self.trades) + ")";
        });

    py::class_<metrics::CorrelationMatrix>(m, "CorrelationMatrix")
        .def("__len__", &metrics::CorrelationMatrix::size)
        .def_property_readonly("labels", &metrics::CorrelationMatrix::labels)
        .def("at", &metrics::CorrelationMatrix::at, py::arg("row"), py::arg("column"))
        .def("rows", [](const metrics::CorrelationMatrix& self) {
            py::list out;
            for (std::size_t i = 0; i < self.size(); ++i) {
                py::list row;
                for (std::size_t j = 0; j < self.size(); ++j) {
                    row.append(self.at(i, j));
                }
                out.append(std::move(row));
            }
            return out;
        });

    py::class_<run::BacktestResult>(m, "BacktestResult")
        .def_readonly("theories", &run::BacktestResult::theories)
        .def_readonly("plugins",
                      &run::BacktestResult::plugins,
                      "Every DLL found, loaded or not, with its error.")
        .def_readonly("symbols", &run::BacktestResult::symbols)
        .def_readonly("events", &run::BacktestResult::events)
        .def_readonly("first_ts", &run::BacktestResult::first_ts)
        .def_readonly("last_ts", &run::BacktestResult::last_ts)
        .def_readonly("fill_model", &run::BacktestResult::fill_model)
        .def_readonly("fill_model_realistic", &run::BacktestResult::fill_model_realistic)
        .def_readonly("correlation", &run::BacktestResult::correlation)
        .def_readonly("error", &run::BacktestResult::error)
        .def_property_readonly("ok", &run::BacktestResult::ok)
        .def("benchmark",
             &run::BacktestResult::benchmark,
             py::return_value_policy::reference_internal)
        .def("find",
             &run::BacktestResult::find,
             py::arg("id"),
             py::return_value_policy::reference_internal)
        .def(
            "__getitem__",
            [](const run::BacktestResult& self, const std::string& id) {
                const run::TheoryResult* found = self.find(id);
                if (found == nullptr) {
                    throw py::key_error("no theory with id '" + id + "'");
                }
                return found;
            },
            py::return_value_policy::reference_internal)
        .def("__len__", [](const run::BacktestResult& self) { return self.theories.size(); })
        .def("__repr__", [](const run::BacktestResult& self) {
            return "BacktestResult(" + std::to_string(self.theories.size()) + " theories, " +
                   std::to_string(self.events) + " events)";
        });

    m.def("run_backtest",
          &run::run_backtest,
          py::arg("config"),
          py::call_guard<py::gil_scoped_release>(),
          R"doc(
Loads every theory it can find, replays the data through all of them at once,
and returns what happened.

Every theory sees the same events in the same order and owns its own account.
None of them can see any other.

The GIL is released for the duration: this can run for a while, and holding it
would block every other Python thread for no reason.
)doc");

    // The convenience form, which is what a notebook actually wants.
    m.def(
        "run",
        [](const std::vector<std::filesystem::path>& theories,
           const std::filesystem::path& data,
           double cash,
           const std::string& symbol,
           std::uint32_t bars,
           std::uint32_t sessions,
           std::uint64_t seed,
           bool optimistic_fills) {
            run::BacktestConfig config;
            config.theory_roots = theories;
            config.data_file = data;
            config.starting_cash = Money::from_double(cash);
            config.symbol = symbol;
            config.bars_per_session = bars;
            config.sessions = sessions;
            config.seed = seed;
            config.optimistic_fills = optimistic_fills;
            py::gil_scoped_release release;
            return run::run_backtest(config);
        },
        py::arg("theories"),
        py::arg("data") = std::filesystem::path{},
        py::arg("cash") = 100'000.0,
        py::arg("symbol") = "SPY",
        py::arg("bars") = 390,
        py::arg("sessions") = 2,
        py::arg("seed") = 0xC0FF'EE00'1234'5678ULL,
        py::arg("optimistic_fills") = false,
        "Runs a backtest with keyword arguments instead of a config object.");
}

}  // namespace

PYBIND11_MODULE(marketscan, m) {
    m.doc() = R"doc(
MarketScan -- a quantitative trading research platform.

One engine for backtest, paper and live: a strategy cannot tell which it is
running under, because only the clock and the transport differ.

Strategies are compiled plugins, not Python. See theories/README.md for how to
write one; this module is for running them and getting the numbers out.

    import marketscan as ms

    result = ms.run(theories=["path/to/theories"],
                    data="data/samples/synthetic_spy_1min.csv")

    for theory in result.theories:
        print(theory.display_name, theory.final_equity, theory.summary.sharpe)
)doc";

    m.attr("__version__") = version::kString;
    m.attr("ABI_VERSION") = MS_ABI_VERSION;
    m.attr("BUILD_TYPE") = version::kBuildType;
    m.attr("COMPILER") = version::kCompiler;

    bind_money(m);
    bind_enums(m);
    bind_time(m);
    bind_symbols_and_feeds(m);
    bind_ledger(m);
    bind_metrics(m);
    bind_backtest(m);

    m.def("set_log_level", &log::set_level, py::arg("level"));
    m.def("init_logging", &log::init, py::arg("minimum") = MS_LOG_INFO);
}
