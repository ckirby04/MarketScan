#include "marketscan/util/version.hpp"

// The generated header is header-only; this translation unit exists so that
// marketscan_core is a real compiled target from the first milestone and the
// generated version header is proven to compile on every build.
namespace marketscan::version {

static_assert(kMajor >= 0, "version header failed to configure");

}  // namespace marketscan::version
