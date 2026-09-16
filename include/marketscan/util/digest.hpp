#ifndef MARKETSCAN_UTIL_DIGEST_HPP
#define MARKETSCAN_UTIL_DIGEST_HPP

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "marketscan/core/event.hpp"
#include "marketscan/core/money.hpp"

namespace marketscan {

/// A running FNV-1a hash over a sequence of values.
///
/// This is how the determinism contract is checked: two runs of the same input
/// must produce the same digest over their event stream and their order log.
///
/// Values are absorbed field by field, never by hashing a struct's raw bytes.
/// A struct carries alignment padding and, in the case of MarketEvent, union
/// storage past the end of the active member; those bytes belong to no field,
/// and folding them in would make the digest depend on whatever happened to be
/// in that memory. A digest that changes for reasons unrelated to the data is
/// worse than no digest, because it gets quarantined.
///
/// Not cryptographic. It exists to catch accidental divergence, not tampering.
class Digest {
public:
    constexpr Digest() noexcept = default;

    constexpr void absorb_u64(std::uint64_t value) noexcept {
        for (int byte = 0; byte < 8; ++byte) {
            hash_ ^= static_cast<std::uint64_t>((value >> (byte * 8)) & 0xFFU);
            hash_ *= kPrime;
        }
    }

    constexpr void absorb_i64(std::int64_t value) noexcept {
        absorb_u64(static_cast<std::uint64_t>(value));
    }

    constexpr void absorb_u32(std::uint32_t value) noexcept { absorb_u64(value); }

    /// Absorbs a double by its bit pattern, so the digest distinguishes values
    /// that compare equal but are not identical, and does not depend on how
    /// the platform prints a float.
    void absorb_double(double value) noexcept {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        // Normalise the two zeros, which compare equal and should hash equal.
        if (bits == 0x8000'0000'0000'0000ULL) {
            bits = 0;
        }
        absorb_u64(bits);
    }

    constexpr void absorb_money(Money value) noexcept { absorb_i64(value.micros()); }

    constexpr void absorb_bytes(std::string_view text) noexcept {
        for (const char c : text) {
            hash_ ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
            hash_ *= kPrime;
        }
        // Length, so that absorbing "ab" then "c" differs from "a" then "bc".
        absorb_u64(text.size());
    }

    void absorb(const MS_Bar& bar) noexcept {
        absorb_i64(bar.ts);
        absorb_u32(bar.symbol);
        absorb_double(bar.open);
        absorb_double(bar.high);
        absorb_double(bar.low);
        absorb_double(bar.close);
        absorb_double(bar.volume);
    }

    void absorb(const MS_Quote& quote) noexcept {
        absorb_i64(quote.ts);
        absorb_u32(quote.symbol);
        absorb_double(quote.bid_px);
        absorb_double(quote.ask_px);
        absorb_double(quote.bid_sz);
        absorb_double(quote.ask_sz);
    }

    void absorb(const MS_Trade& trade) noexcept {
        absorb_i64(trade.ts);
        absorb_u32(trade.symbol);
        absorb_double(trade.price);
        absorb_double(trade.size);
        absorb_u32(static_cast<std::uint32_t>(trade.aggressor));
    }

    void absorb(const MarketEvent& event) noexcept {
        absorb_u32(static_cast<std::uint32_t>(event.type));
        switch (event.type) {
            case EventType::Bar:
                absorb(event.bar);
                break;
            case EventType::Quote:
                absorb(event.quote);
                break;
            case EventType::Trade:
                absorb(event.trade);
                break;
        }
    }

    void absorb(const MS_Fill& fill) noexcept {
        absorb_i64(fill.ts);
        absorb_u32(fill.symbol);
        absorb_u64(fill.order_id);
        absorb_u64(fill.client_order_id);
        absorb_u32(static_cast<std::uint32_t>(fill.side));
        absorb_double(fill.price);
        absorb_double(fill.quantity);
        absorb_i64(fill.commission);
    }

    void absorb(const MS_OrderRequest& order) noexcept {
        absorb_u32(order.symbol);
        absorb_u32(static_cast<std::uint32_t>(order.side));
        absorb_u32(static_cast<std::uint32_t>(order.type));
        absorb_u32(static_cast<std::uint32_t>(order.tif));
        absorb_double(order.quantity);
        absorb_double(order.limit_price);
        absorb_u64(order.client_order_id);
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return hash_; }

    [[nodiscard]] std::string to_hex() const;

private:
    static constexpr std::uint64_t kOffsetBasis = 0xCBF2'9CE4'8422'2325ULL;
    static constexpr std::uint64_t kPrime = 0x0000'0100'0000'01B3ULL;

    std::uint64_t hash_ = kOffsetBasis;
};

}  // namespace marketscan

#endif  // MARKETSCAN_UTIL_DIGEST_HPP
