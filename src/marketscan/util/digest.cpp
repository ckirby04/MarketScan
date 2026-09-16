#include "marketscan/util/digest.hpp"

#include <cstdio>

namespace marketscan {

std::string Digest::to_hex() const {
    char buf[17];
    const int n = std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(hash_));
    return std::string(buf, static_cast<std::size_t>(n < 0 ? 0 : n));
}

}  // namespace marketscan
