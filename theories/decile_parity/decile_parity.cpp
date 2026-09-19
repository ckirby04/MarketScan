// The plugin boundary for decile_parity. The strategy itself is in the header,
// so the parity test can compile it into a test binary without going through a
// DLL -- and so that what the test exercises and what the dashboard loads are
// the same code rather than two copies of it.

#include "decile_parity.hpp"

MARKETSCAN_EXPORT_STRATEGY(decile_parity::DecileParity,
                           "decile_parity",
                           "Decile Parity (fixture)",
                           "Clark Kirby")
