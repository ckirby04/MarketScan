// A theory whose only purpose is to draw random numbers.
//
// Neither reference theory calls random_uniform(), so through Spec 1 the
// per-strategy random stream was plumbed, documented and never once
// exercised end to end. This plugin exercises it.
//
// It does two things a test can check from the outside:
//
//   - publishes a running digest of every draw, so two runs can be compared
//     without the test needing to know the algorithm; and
//   - publishes the first few draws individually, so a failure says which
//     draw went wrong rather than only that something did.
//
// It deliberately does not trade. An order would bring the fill model and the
// ledger into a test about randomness, and if this ever fails the question
// should be about the generator and nothing else.

#include "marketscan/strategy/istrategy.hpp"

namespace {

class DiceRoller final : public marketscan::IStrategy {
public:
    static constexpr const char* kDescription =
        "Draws from the host's random stream and publishes what it got. A test "
        "instrument, not a strategy: it never trades.";

    static constexpr MS_ParamDesc kParams[] = {
        marketscan::param_int(
            "draws_per_bar", "How many numbers to draw on each bar.", 1.0, 16.0, 1.0),
    };
    static constexpr uint32_t kParamCount = 1;

    DiceRoller() { bind_param("draws_per_bar", &draws_per_bar_); }

    void on_bar(const MS_Bar& bar) override {
        (void)bar;
        const int draws = draws_per_bar_ < 1 ? 1 : draws_per_bar_;
        for (int i = 0; i < draws; ++i) {
            const double x = random_uniform();

            // Every draw has to reach the digest, and it has to reach it in a
            // way that depends on its position as well as its value -- a plain
            // sum would hash the same for two streams that drew the same
            // numbers in a different order, which is exactly the failure a
            // shared generator would produce.
            ++count_;
            digest_ = digest_ * 6'364'136'223'846'793'005ULL + 1'442'695'040'888'963'407ULL;
            digest_ ^= static_cast<unsigned long long>(x * 9'007'199'254'740'992.0);  // 2^53

            if (count_ <= kReportedDraws) {
                report(count_, x);
            }
        }

        // Doubles carry the digest exactly up to 2^53; the publishing channel
        // is a double, so the top bits are dropped rather than rounded.
        publish_metric("draw_digest", static_cast<double>(digest_ & 0x1F'FFFF'FFFF'FFFFULL));
        publish_metric("draw_count", static_cast<double>(count_));
    }

private:
    static constexpr unsigned long long kReportedDraws = 8;

    /// Publishes draw n under its own name, so a mismatch is locatable.
    void report(unsigned long long n, double value) const noexcept {
        static constexpr const char* kNames[] = {
            "draw_1",
            "draw_2",
            "draw_3",
            "draw_4",
            "draw_5",
            "draw_6",
            "draw_7",
            "draw_8",
        };
        publish_metric(kNames[n - 1], value);
    }

    int draws_per_bar_ = 1;
    unsigned long long digest_ = 0xCBF2'9CE4'8422'2325ULL;
    unsigned long long count_ = 0;
};

}  // namespace

MARKETSCAN_EXPORT_STRATEGY(DiceRoller, "dice_roller", "Dice Roller", "MarketScan tests")
