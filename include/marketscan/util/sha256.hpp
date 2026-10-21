#ifndef MARKETSCAN_UTIL_SHA256_HPP
#define MARKETSCAN_UTIL_SHA256_HPP

/*
 * SHA-256 (FIPS 180-4), for identifying a research form.
 *
 * The Results document names the form it graded by this hash, and the job
 * seeds its random draws from it, so the research layer has to be able to
 * compute the same thing with hashlib. FNV-1a (util/digest.hpp) would do for
 * the seed and nothing else: a hash that names a document should be one a
 * reader can recompute with a standard tool.
 *
 * Small, unoptimised and checked against the NIST example vectors.
 */

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace marketscan {

[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::string_view data);

/// Lower-case hex, 64 characters.
[[nodiscard]] std::string sha256_hex(std::string_view data);

}  // namespace marketscan

#endif  // MARKETSCAN_UTIL_SHA256_HPP
