#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "headless.hpp"
#include "marketscan/strategy/plugin_loader.hpp"
#include "marketscan/util/logging.hpp"

#if MARKETSCAN_HAS_GUI
#    include "app.hpp"
#endif

namespace {

/// The GUI is the point of this application, so it is the default. --headless
/// reaches the same engine without a window, which is what CI and a batch run
/// want -- and, importantly, it is the *same* engine rather than a second
/// implementation of one.
[[nodiscard]] bool wants_headless(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") == 0) {
            return true;
        }
    }
    return false;
}

/// Strips --headless before the headless parser sees it.
[[nodiscard]] std::vector<char*> without_headless(int argc, char** argv) {
    std::vector<char*> filtered;
    filtered.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") != 0) {
            filtered.push_back(argv[i]);
        }
    }
    return filtered;
}

}  // namespace

int main(int argc, char** argv) {
    const bool headless = wants_headless(argc, argv);
    std::vector<char*> args = without_headless(argc, argv);

    marketscan::app::HeadlessOptions options;
    int exit_code = 0;
    if (!marketscan::app::parse_arguments(
            static_cast<int>(args.size()), args.data(), options, exit_code)) {
        return exit_code;
    }

    const std::filesystem::path executable_dir = marketscan::app::executable_directory();

#if MARKETSCAN_HAS_GUI
    if (!headless) {
        marketscan::app::AppOptions app_options;
        app_options.session.data_file = options.data_file;
        app_options.session.default_symbol = options.default_symbol;
        // --no-universe wins, the same way it does headless: a viewer who asked
        // for an unfiltered run gets one, and the Universe Inspector says so in
        // as many words rather than going blank.
        if (!options.no_universe) {
            app_options.session.universe_file = options.universe_file;
            if (!options.universe_file.empty()) {
                app_options.session.security_overrides = "data/security_overrides.csv";
            }
        }
        app_options.session.bars_per_session = options.bars;
        app_options.session.sessions = options.sessions;
        app_options.session.seed = options.seed;
        app_options.session.starting_cash = marketscan::Money::from_double(options.starting_cash);
        app_options.session.theory_roots = marketscan::PluginLoader::default_roots(executable_dir);
        app_options.autoplay = options.autoplay;
        app_options.focus_panel = options.focus_panel;
        if (options.view_mode == "overview") {
            app_options.view_mode = marketscan::app::ViewMode::Overview;
        } else if (options.view_mode == "operator") {
            app_options.view_mode = marketscan::app::ViewMode::Operator;
        } else if (options.view_mode == "research") {
            app_options.view_mode = marketscan::app::ViewMode::Research;
        }
        app_options.research.daily_dir = options.research_daily;
        for (const std::string& spec : options.research_csv) {
            const auto equals = spec.find('=');
            if (equals != std::string::npos && equals > 0) {
                app_options.research.csv_sources.emplace_back(spec.substr(0, equals),
                                                              spec.substr(equals + 1));
            }
        }
        app_options.research_form = options.research_form;
        app_options.research_conversation = options.research_conversation;
        app_options.research_ask = options.research_ask;
        app_options.screenshot = options.screenshot;
        app_options.screenshot_after = options.screenshot_after;
        app_options.speed = options.speed;
        app_options.assets_dir = executable_dir / "assets";
        app_options.layout_file = executable_dir / "imgui.ini";

        marketscan::log::init(options.verbose ? MS_LOG_DEBUG : MS_LOG_INFO);
        marketscan::app::Application application(std::move(app_options));
        return application.run();
    }
#else
    (void)headless;
#endif

    return marketscan::app::run_headless(options, executable_dir);
}
