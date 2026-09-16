#include <cstdio>

#include "marketscan/util/version.hpp"

int main() {
    std::printf("MarketScan %s  (%s, %s)\n",
                marketscan::version::kString,
                marketscan::version::kCompiler,
                marketscan::version::kBuildType);
#if MARKETSCAN_HAS_GUI
    std::printf("gui: enabled\n");
#else
    std::printf("gui: disabled (headless build)\n");
#endif
    return 0;
}
