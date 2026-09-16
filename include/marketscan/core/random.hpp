#ifndef MARKETSCAN_CORE_RANDOM_HPP
#define MARKETSCAN_CORE_RANDOM_HPP

#include <cmath>
#include <cstdint>
#include <numbers>
#include <string_view>

namespace marketscan {

/// The only source of randomness in MarketScan.
///
/// Everything here is specified down to the bit: xoshiro256** for the stream,
/// SplitMix64 to expand the seed, an explicit 53-bit mantissa construction for
/// uniform(), and Box-Muller for normal().
///
/// None of that uses <random>. std::mt19937_64 would be reproducible, but
/// std::uniform_real_distribution and std::normal_distribution are not: the
/// standard fixes their *distribution*, not their algorithm, so the same seed
/// legitimately yields different numbers on a different standard library or
/// after a toolchain upgrade. A backtest whose results move when the compiler
/// is updated is not reproducible in any useful sense.
///
/// Small enough to live inside a strategy instance, so each one can own its
/// own stream and draw independently of what else happens to be loaded.
class DeterministicRng {
public:
    explicit constexpr DeterministicRng(std::uint64_t seed) noexcept { reseed(seed); }

    constexpr void reseed(std::uint64_t seed) noexcept {
        // SplitMix64 spreads a single seed -- including 0, which xoshiro
        // cannot be started from directly -- across the whole state.
        std::uint64_t x = seed;
        for (std::uint64_t& word : state_) {
            word = splitmix64(x);
        }
        has_spare_ = false;
        spare_ = 0.0;
    }

    /// Raw 64 bits. xoshiro256**.
    constexpr std::uint64_t next_u64() noexcept {
        const std::uint64_t result = rotl(state_[1] * 5, 7) * 9;
        const std::uint64_t t = state_[1] << 17;

        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];
        state_[2] ^= t;
        state_[3] = rotl(state_[3], 45);

        return result;
    }

    /// Uniform on [0, 1). Takes the top 53 bits, which is exactly the number a
    /// double can hold without rounding, so every result is representable and
    /// the mapping is spelled out rather than left to the library.
    constexpr double uniform() noexcept {
        return static_cast<double>(next_u64() >> 11) * 0x1.0p-53;
    }

    /// Uniform on (0, 1]. For logarithms, which must not be handed a zero.
    constexpr double uniform_positive() noexcept {
        return (static_cast<double>(next_u64() >> 11) + 1.0) * 0x1.0p-53;
    }

    /// Uniform on [lo, hi).
    constexpr double uniform_range(double lo, double hi) noexcept {
        return lo + (hi - lo) * uniform();
    }

    /// Standard normal, by Box-Muller.
    ///
    /// The transform produces two independent normals per pair of uniforms;
    /// the second is cached. That makes a draw's value depend on how many
    /// draws preceded it, which is fine -- it is still a pure function of the
    /// seed and the call sequence.
    double normal() noexcept {
        if (has_spare_) {
            has_spare_ = false;
            return spare_;
        }
        const double u1 = uniform_positive();
        const double u2 = uniform();
        const double magnitude = std::sqrt(-2.0 * std::log(u1));
        const double angle = 2.0 * std::numbers::pi * u2;

        spare_ = magnitude * std::sin(angle);
        has_spare_ = true;
        return magnitude * std::cos(angle);
    }

    double normal(double mean, double stddev) noexcept { return mean + stddev * normal(); }

    /// Uniform integer on [0, bound). Rejection sampled, so the result is not
    /// skewed by a modulo that does not divide 2^64 evenly.
    constexpr std::uint64_t below(std::uint64_t bound) noexcept {
        if (bound == 0) {
            return 0;
        }
        const std::uint64_t limit = UINT64_MAX - (UINT64_MAX % bound) - 1;
        std::uint64_t value = next_u64();
        while (value > limit) {
            value = next_u64();
        }
        return value % bound;
    }

private:
    static constexpr std::uint64_t rotl(std::uint64_t x, int k) noexcept {
        return (x << k) | (x >> (64 - k));
    }

    static constexpr std::uint64_t splitmix64(std::uint64_t& x) noexcept {
        std::uint64_t z = (x += 0x9E37'79B9'7F4A'7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBULL;
        return z ^ (z >> 31);
    }

    std::uint64_t state_[4]{};
    double spare_ = 0.0;
    bool has_spare_ = false;
};

/// Derives an independent stream from a run seed and a name.
///
/// Every strategy gets its own RNG seeded through this, so that the numbers a
/// strategy draws do not depend on which *other* strategies happen to be
/// loaded. Sharing one generator across instances would make a run's results
/// change when an unrelated theory is enabled, which would be indefensible.
[[nodiscard]] constexpr std::uint64_t derive_seed(std::uint64_t run_seed,
                                                  std::string_view name) noexcept {
    // FNV-1a over the name, mixed into the run seed.
    std::uint64_t hash = 0xCBF2'9CE4'8422'2325ULL;
    for (const char c : name) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        hash *= 0x0000'0100'0000'01B3ULL;
    }
    std::uint64_t mixed = run_seed ^ hash;
    mixed ^= mixed >> 33;
    mixed *= 0xFF51'AFD7'ED55'8CCDULL;
    mixed ^= mixed >> 33;
    mixed *= 0xC4CE'B9FE'1A85'EC53ULL;
    mixed ^= mixed >> 33;
    return mixed;
}

}  // namespace marketscan

#endif  // MARKETSCAN_CORE_RANDOM_HPP
