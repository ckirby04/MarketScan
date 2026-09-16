#ifndef MARKETSCAN_APP_APP_HPP
#define MARKETSCAN_APP_APP_HPP

#include <filesystem>
#include <memory>
#include <string>

#include "session.hpp"
#include "ui_state.hpp"

struct GLFWwindow;

namespace marketscan::app {

struct AppOptions {
    SessionConfig session;

    /// Directory holding fonts/. Defaults to `assets` beside the executable,
    /// falling back to the source tree so a build tree runs without an install
    /// step.
    std::filesystem::path assets_dir;

    /// Where the docking layout is remembered. Next to the executable, so two
    /// builds do not fight over one layout.
    std::filesystem::path layout_file;

    int width = 1680;
    int height = 980;

    /// Begin replaying as soon as the window is up.
    bool autoplay = false;

    /// Initial replay speed. Zero is unthrottled.
    double speed = 0.0;
};

/// The desktop application.
///
/// Owns the window, the ImGui context and the Session. Draws at the display's
/// refresh rate and never waits on the engine: everything on screen comes from
/// the most recently published snapshot, which is a complete and consistent
/// picture of one instant rather than a live view of something being mutated.
class Application {
public:
    explicit Application(AppOptions options);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&&) = delete;
    Application& operator=(Application&&) = delete;

    /// Returns a process exit code. Blocks until the window closes.
    int run();

private:
    bool initialise();
    void shutdown();

    void draw_frame();
    void draw_menu_bar();
    void draw_toolbar();
    void draw_status_bar();
    void handle_shortcuts();

    /// Arranges the panels the first time, and whenever Reset Layout is used.
    void build_default_layout(unsigned int dockspace_id);

    /// Whether the saved layout knows about every panel this build has.
    [[nodiscard]] bool layout_file_covers_all_panels() const;

    AppOptions options_;
    std::unique_ptr<Session> session_;
    UiState ui_;

    GLFWwindow* window_ = nullptr;

    /// ImGui keeps a pointer to this, so it has to outlive the context.
    std::string layout_path_;

    bool layout_built_ = false;

    /// Frames left in the sequence that brings the default tabs to the front
    /// after the layout is built. Zero means nothing is pending.
    int default_focus_stage_ = 0;
    bool running_ = true;
    std::string init_error_;
};

}  // namespace marketscan::app

#endif  // MARKETSCAN_APP_APP_HPP
