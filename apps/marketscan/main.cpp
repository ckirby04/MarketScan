#include "headless.hpp"

int main(int argc, char** argv) {
    marketscan::app::HeadlessOptions options;
    int exit_code = 0;
    if (!marketscan::app::parse_arguments(argc, argv, options, exit_code)) {
        return exit_code;
    }

#if MARKETSCAN_HAS_GUI
    // The desktop dashboard arrives at M6. Until then the same binary runs the
    // engine headlessly, which is also what a GUI-less CI build does.
    return marketscan::app::run_headless(options, marketscan::app::executable_directory());
#else
    return marketscan::app::run_headless(options, marketscan::app::executable_directory());
#endif
}
