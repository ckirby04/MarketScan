#include "app.hpp"

#include "screenshot.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder, for the default layout

#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>
#include <implot.h>

#include "marketscan/research/service_client.hpp"
#include "marketscan/util/logging.hpp"
#include "marketscan/util/version.hpp"
#include "panels/panels.hpp"
#include "theme.hpp"

namespace marketscan::app {
namespace {

void glfw_error_callback(int code, const char* description) {
    log::error("app",
               "GLFW error " + std::to_string(code) + ": " +
                   (description != nullptr ? description : "unknown"));
}

/// Speed presets for the toolbar. Zero is "as fast as the machine allows",
/// which for a replay is the useful default -- waiting out a day of market
/// data in real time is not research.
struct SpeedPreset {
    const char* label;
    double multiplier;
};

/// Every dockable panel, by window title.
///
/// Used both to lay the default arrangement out and to notice a saved layout
/// that predates one of them. Kept next to the layout builder so the two
/// cannot drift apart.
constexpr const char* kPanelTitles[] = {
    "Leaderboard",
    "Plugin Manager",
    "Equity Curves",
    "Drawdown",
    "Relative Strength",
    "Correlation",
    "Theory Detail",
    "Universe",
    "Positions & P&L",
    "Transport",
    "Blotter",
    "Latency",
    "Log",
};

constexpr SpeedPreset kSpeeds[] = {
    {"1x", 1.0},
    {"10x", 10.0},
    {"100x", 100.0},
    {"1000x", 1'000.0},
    {"10000x", 10'000.0},
    {"max", 0.0},
};

}  // namespace

Application::Application(AppOptions options) : options_(std::move(options)) {}

Application::~Application() {
    shutdown();
}

bool Application::initialise() {
    glfwSetErrorCallback(&glfw_error_callback);
    if (glfwInit() == GLFW_FALSE) {
        init_error_ = "could not initialise GLFW";
        return false;
    }

    // OpenGL 3.2 core: the floor the ImGui OpenGL3 backend targets, and
    // available on anything with a driver written this decade.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    // Never larger than the display. A window taller than the screen puts its
    // status bar under the taskbar, where the one line saying what went wrong
    // is exactly what cannot be read.
    int width = options_.width;
    int height = options_.height;
    int place_x = -1;
    int place_y = -1;
    if (GLFWmonitor* monitor = glfwGetPrimaryMonitor(); monitor != nullptr) {
        int work_x = 0;
        int work_y = 0;
        int work_width = 0;
        int work_height = 0;
        glfwGetMonitorWorkarea(monitor, &work_x, &work_y, &work_width, &work_height);
        if (work_width > 0 && work_height > 0) {
            // The work area already excludes the taskbar. The margins are for
            // the title bar and frame, which GLFW draws outside the client
            // area sized here: without them the window is placed by the
            // window manager wherever it likes and the status bar -- the one
            // line saying what went wrong -- ends up under the taskbar.
            constexpr int kFrameWidth = 16;
            constexpr int kTitleHeight = 48;
            width = std::min(width, work_width - 2 * kFrameWidth);
            height = std::min(height, work_height - kTitleHeight - kFrameWidth);
            place_x = work_x + (work_width - width) / 2;
            place_y = work_y + kTitleHeight;
        }
    }

    const std::string title = "MarketScan " + std::string(version::kString);
    window_ = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (window_ == nullptr) {
        init_error_ = "could not create a window (no OpenGL 3.2 context available)";
        glfwTerminate();
        return false;
    }

    if (place_x >= 0 && place_y >= 0) {
        glfwSetWindowPos(window_, place_x, place_y);
    }

    glfwMakeContextCurrent(window_);
    // Vsync. The frame budget is the display's refresh interval, and spinning
    // faster than that burns a core to draw frames nobody sees.
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    // ImGui stores this pointer rather than copying the string, so it has to
    // outlive the context.
    layout_path_ = options_.layout_file.string();
    io.IniFilename = layout_path_.c_str();

    // Beside the layout, so the two travel together and a reset of one is not a
    // surprise reset of the other.
    state_path_ = options_.layout_file;
    state_path_.replace_extension(".state");
    ui_.load(state_path_);
    if (options_.view_mode.has_value()) {
        ui_.view_mode = *options_.view_mode;
    }

    theme::apply();
    theme::load_fonts(options_.assets_dir);

    if (!ImGui_ImplGlfw_InitForOpenGL(window_, true)) {
        init_error_ = "could not attach ImGui to the window";
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init("#version 150")) {
        init_error_ = "could not initialise the OpenGL renderer";
        return false;
    }

    // Built before the window is shown, so the first visible frame is already
    // laid out rather than flashing through a default arrangement.
    //
    // A saved layout is only kept if it knows about every panel. ImGui floats
    // any window it has no entry for, so a layout written by a build with
    // fewer panels leaves the new ones scattered loose over the docked ones --
    // which looks like the application is broken rather than like it gained a
    // feature. Rebuilding is the lesser loss.
    layout_built_ = layout_file_covers_all_panels();

    session_ = std::make_unique<Session>(options_.session);
    // The chat reaches the job service from the desktop app itself and nowhere
    // else: the key and the URL come from the environment
    // (docs/specs/SPEC_DESKTOP_CHAT.md section 3). Tests leave this unset.
    if (!options_.research.connect_service) {
        options_.research.connect_service = [] {
            return research::connect_from_environment(std::string("MarketScan/") +
                                                      std::string(version::kString));
        };
    }
    research_ = std::make_unique<ResearchSession>(options_.research);
    if (!options_.research_form.empty()) {
        try {
            std::ifstream in(options_.research_form, std::ios::binary);
            research_->set_form(nlohmann::json::parse(in));
            research_->run();
            ui_.research_chat = false;  // a form given on the command line shows its run
        } catch (const std::exception& error) {
            log::error("research",
                       "could not load " + options_.research_form.string() + ": " + error.what());
        }
    }
    // The chat, for reproducible screenshots of it: a conversation to open, or a
    // question to ask. Both need the chat to be connected (MS_SERVICE_KEY).
    if (!options_.research_conversation.empty()) {
        research_->chat_open(options_.research_conversation);
    }
    if (!options_.research_ask.empty()) {
        research_->chat_send(options_.research_ask);
    }
    session_->set_speed(options_.speed);
    if (options_.autoplay) {
        session_->play();
    }

    glfwShowWindow(window_);
    return true;
}

bool Application::layout_file_covers_all_panels() const {
    std::error_code ec;
    if (!std::filesystem::exists(options_.layout_file, ec)) {
        return false;
    }

    std::ifstream file(options_.layout_file);
    if (!file) {
        return false;
    }
    const std::string contents((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());

    for (const char* title : kPanelTitles) {
        if (contents.find(std::string("[Window][") + title + "]") == std::string::npos) {
            log::info("app",
                      std::string("saved layout predates the '") + title +
                          "' panel; rebuilding the default arrangement");
            return false;
        }
    }
    return true;
}

void Application::shutdown() {
    // Remember which view was showing. Best-effort: not being able to write a
    // preference is not worth failing a shutdown over.
    if (!state_path_.empty()) {
        ui_.save(state_path_);
    }

    // The session's worker thread is joined here, before the GL context goes
    // away. It does not touch the GPU, but it does hold plugin instances, and
    // tearing those down after the process has started unwinding is asking for
    // a static-destruction order problem.
    session_.reset();

    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
    }
    if (window_ != nullptr) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
        glfwTerminate();
    }
}

int Application::run() {
    if (!initialise()) {
        std::fprintf(stderr, "marketscan: %s\n", init_error_.c_str());
        shutdown();
        return 1;
    }

    log::info("app", "MarketScan " + std::string(version::kString) + " ready");

    while (running_ && glfwWindowShouldClose(window_) == GLFW_FALSE) {
        glfwPollEvents();

        // Nothing to draw into: minimised. Sleep rather than spin.
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window_, &width, &height);
        if (width == 0 || height == 0) {
            glfwWaitEventsTimeout(0.1);
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        draw_frame();

        ImGui::Render();
        glViewport(0, 0, width, height);
        const ImVec4 clear = ImGui::ColorConvertU32ToFloat4(theme::colors().background);
        glClearColor(clear.x, clear.y, clear.z, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Before the swap, not after: on a double-buffered context the frame
        // that was just drawn is in the *back* buffer, and what glReadPixels
        // returns from the front one after a swap is undefined.
        if (!options_.screenshot.empty() && glfwGetTime() >= options_.screenshot_after) {
            (void)capture_back_buffer(options_.screenshot, width, height);
            options_.screenshot.clear();
            running_ = false;
        }

        glfwSwapBuffers(window_);
    }

    shutdown();
    return 0;
}

void Application::draw_frame() {
    // One snapshot for the whole frame. Taking it once means every panel draws
    // the same instant: the leaderboard cannot disagree with the equity curve
    // about what a theory is worth.
    const std::shared_ptr<const SessionSnapshot> snapshot = session_->snapshot();

    handle_shortcuts();

    // --- Host window ----------------------------------------------------
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));

    constexpr ImGuiWindowFlags kHostFlags =
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::Begin("##host", nullptr, kHostFlags);
    ImGui::PopStyleVar(3);

    draw_menu_bar();
    draw_toolbar();

    // Leave room for the status bar at the bottom. That bar is a separator and
    // a line of text, so the reservation has to cover the spacing either side
    // of the separator as well -- reserving the text height alone clips the
    // status line in half against the bottom of the window.
    const float status_height =
        ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0F;

    panels::Context context{*session_, *snapshot, ui_, research_.get()};
    const bool overview = ui_.view_mode == ViewMode::Overview;

    if (ui_.view_mode == ViewMode::Research) {
        // Like Overview: a whole view, nothing dockable, so Operator's layout
        // is untouched while it shows. The replay keeps running underneath --
        // switching views never touches either session.
        ImGui::BeginChild("##research_body", ImVec2(0.0F, -status_height));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0F, 12.0F));
        ImGui::Indent(16.0F);
        ImGui::Dummy(ImVec2(0.0F, 6.0F));
        panels::research(context);
        ImGui::Unindent(16.0F);
        ImGui::PopStyleVar();
        ImGui::EndChild();

        draw_status_bar();
        ImGui::End();
    } else if (overview) {
        // No dockspace, and no panels submitted.
        //
        // That is how each mode keeps its own layout state without any being
        // written down: Overview has nothing dockable in it, so Operator's
        // arrangement is not touched while Overview is showing, and comes back
        // exactly as it was. ImGui keeps a window's settings whether or not it
        // was submitted this frame.
        ImGui::BeginChild("##overview_body", ImVec2(0.0F, -status_height));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0F, 20.0F));
        ImGui::Indent(28.0F);
        ImGui::Dummy(ImVec2(0.0F, 10.0F));
        panels::overview(context);
        ImGui::Unindent(28.0F);
        ImGui::PopStyleVar();
        ImGui::EndChild();

        draw_status_bar();
        ImGui::End();
    } else {
        const ImGuiID dockspace_id = ImGui::GetID("##dockspace");
        if (!layout_built_ || ui_.request_reset_layout) {
            build_default_layout(dockspace_id);
            layout_built_ = true;
            ui_.request_reset_layout = false;
        }
        ImGui::DockSpace(dockspace_id, ImVec2(0.0F, -status_height), ImGuiDockNodeFlags_None);

        draw_status_bar();
        ImGui::End();

        // --- Panels ---------------------------------------------------------
        panels::draw_all(context);
    }

    // Bring the default tabs to the front, now that the panels exist.
    //
    // ImGui hands a dock node's tab selection to whichever of that node's
    // windows holds the keyboard focus, and on the frame the panels first
    // appear that is the last one drawn -- the log. So the selection written
    // in build_default_layout survives in every node except the one the log
    // is in, and that node has to be claimed by focusing into it instead.
    //
    // Two frames apart, and in this order: focusing the leaderboard hands the
    // bottom node back, and focusing the equity chart afterwards moves the
    // focus out of that node without disturbing the selection it just made.
    if (ui_.view_mode == ViewMode::Operator && default_focus_stage_ > 0) {
        --default_focus_stage_;
        if (default_focus_stage_ == 2) {
            ImGui::SetWindowFocus("Leaderboard");
        } else if (default_focus_stage_ == 0) {
            // Last, and after the defaults, so --focus wins over them rather
            // than being overwritten by the stage that runs next.
            if (!options_.focus_panel.empty()) {
                ImGui::SetWindowFocus(options_.focus_panel.c_str());
            } else {
                ImGui::SetWindowFocus("Equity Curves");
            }
        }
    }

    if (ui_.request_quit) {
        running_ = false;
    }
}

void Application::draw_menu_bar() {
    if (!ImGui::BeginMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("Run")) {
        if (ImGui::MenuItem("Play / Pause", "Space")) {
            session_->toggle_play();
        }
        if (ImGui::MenuItem("Step one event", "Right")) {
            session_->step_event();
        }
        if (ImGui::MenuItem("Step one bar", "Shift+Right")) {
            session_->step_bar();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Restart", "Ctrl+R")) {
            session_->restart();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Alt+F4")) {
            ui_.request_quit = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Theories")) {
        if (ImGui::MenuItem("Rescan theories/", "Ctrl+Shift+R")) {
            session_->rescan_theories();
            ui_.reload_available = false;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Overview", "F2", ui_.view_mode == ViewMode::Overview)) {
            ui_.view_mode = ViewMode::Overview;
        }
        if (ImGui::MenuItem("Operator", "F2", ui_.view_mode == ViewMode::Operator)) {
            ui_.view_mode = ViewMode::Operator;
        }
        if (ImGui::MenuItem("Research", "F3", ui_.view_mode == ViewMode::Research)) {
            ui_.view_mode = ViewMode::Research;
        }
        ImGui::Separator();

        // The panel toggles below belong to Operator. Greyed rather than hidden
        // in Overview, so the menu does not appear to lose entries.
        ImGui::BeginDisabled(ui_.view_mode != ViewMode::Operator);
        ImGui::MenuItem("Leaderboard", nullptr, &ui_.show_leaderboard);
        ImGui::MenuItem("Equity Curves", nullptr, &ui_.show_equity);
        ImGui::MenuItem("Drawdown", nullptr, &ui_.show_drawdown);
        ImGui::MenuItem("Relative Strength", nullptr, &ui_.show_relative);
        ImGui::MenuItem("Correlation", nullptr, &ui_.show_correlation);
        ImGui::Separator();
        ImGui::MenuItem("Theory Detail", nullptr, &ui_.show_detail);
        ImGui::MenuItem("Universe", nullptr, &ui_.show_universe);
        ImGui::MenuItem("Positions & P&L", nullptr, &ui_.show_positions);
        ImGui::MenuItem("Blotter", nullptr, &ui_.show_blotter);
        ImGui::Separator();
        ImGui::MenuItem("Plugin Manager", nullptr, &ui_.show_plugins);
        ImGui::MenuItem("Transport", nullptr, &ui_.show_transport);
        ImGui::MenuItem("Latency", nullptr, &ui_.show_latency);
        ImGui::MenuItem("Log", nullptr, &ui_.show_log);
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout")) {
            ui_.request_reset_layout = true;
        }
        ImGui::EndMenu();
    }

    // Right-aligned build identity, so a screenshot says which build produced
    // it without anyone having to ask.
    const std::string build = std::string(version::kString) + "  " + version::kBuildType;
    const float width = ImGui::CalcTextSize(build.c_str()).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - width - 16.0F);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_faint);
    ImGui::TextUnformatted(build.c_str());
    ImGui::PopStyleColor();

    ImGui::EndMenuBar();
}

void Application::draw_toolbar() {
    const std::shared_ptr<const SessionSnapshot> snapshot = session_->snapshot();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0F, 6.0F));
    ImGui::BeginChild("##toolbar", ImVec2(0, ImGui::GetFrameHeight() + 14.0F));

    const bool running = snapshot->state == TransportState::Running;
    if (ImGui::Button(running ? "Pause" : "Play", ImVec2(78, 0))) {
        session_->toggle_play();
    }
    ImGui::SameLine();
    if (ImGui::Button("Step")) {
        session_->step_event();
    }
    ImGui::SameLine();
    if (ImGui::Button("Step bar")) {
        session_->step_bar();
    }
    ImGui::SameLine();
    if (ImGui::Button("Restart")) {
        session_->restart();
    }

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0F);
    if (ImGui::BeginCombo("##speed", kSpeeds[ui_.speed_index].label)) {
        for (int i = 0; i < IM_ARRAYSIZE(kSpeeds); ++i) {
            if (ImGui::Selectable(kSpeeds[i].label, ui_.speed_index == i)) {
                ui_.speed_index = i;
                session_->set_speed(kSpeeds[i].multiplier);
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(-260.0F);
    float progress = static_cast<float>(snapshot->progress);
    ImGui::ProgressBar(
        progress, ImVec2(-260.0F, 0.0F), panels::format_timestamp(snapshot->clock).c_str());

    ImGui::SameLine();
    if (!snapshot->fill_model_realistic) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().warning);
        ImGui::TextUnformatted("OPTIMISTIC FILLS");
        ImGui::PopStyleColor();
        panels::help_marker("Every order is filling at the last trade price with no spread and "
                            "no size penalty. Nothing on screen is a result.");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_faint);
        ImGui::Text("%s fills", snapshot->fill_model.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void Application::draw_status_bar() {
    const std::shared_ptr<const SessionSnapshot> snapshot = session_->snapshot();

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_dim);

    ImGui::Text("%s", std::string(to_string(snapshot->state)).c_str());
    ImGui::SameLine();
    ImGui::Text(" |  %s events", panels::format_count(snapshot->events).c_str());

    if (snapshot->events_per_second > 0.0) {
        ImGui::SameLine();
        ImGui::Text(" |  %.0f ev/s", snapshot->events_per_second);
    }

    ImGui::SameLine();
    ImGui::Text(" |  %zu theories", snapshot->strategies.size());

    ImGui::SameLine();
    ImGui::Text(" |  %.1f fps", static_cast<double>(ImGui::GetIO().Framerate));

    ImGui::PopStyleColor();

    if (!snapshot->error.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().danger);
        ImGui::Text(" |  %s", snapshot->error.c_str());
        ImGui::PopStyleColor();
    }
}

void Application::handle_shortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    // Never steal keys from a text field: a search box that pauses the replay
    // every time someone types a space is worse than no shortcut at all.
    if (io.WantTextInput) {
        return;
    }

    // One keystroke between the two views, and it never touches the engine: the
    // run continues at whatever speed it was going, only the renderer changes.
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
        ui_.view_mode =
            ui_.view_mode == ViewMode::Overview ? ViewMode::Operator : ViewMode::Overview;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) {
        ui_.view_mode = ViewMode::Research;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        session_->toggle_play();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
        if (io.KeyShift) {
            session_->step_bar();
        } else {
            session_->step_event();
        }
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        if (io.KeyShift) {
            session_->rescan_theories();
            ui_.reload_available = false;
        } else {
            session_->restart();
        }
    }
}

void Application::build_default_layout(unsigned int dockspace_id) {
    const auto id = static_cast<ImGuiID>(dockspace_id);

    ImGui::DockBuilderRemoveNode(id);
    ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(id, ImGui::GetMainViewport()->WorkSize);

    // The arrangement a person would build for themselves, and the one Reset
    // Layout restores:
    //
    //   left     the plugin manager -- what is loaded, and why anything is not
    //   centre   the charts, tabbed
    //   right    detail and positions for whichever theory is selected
    //   bottom   leaderboard, blotter, transport, latency and log, tabbed
    //
    // Two things this gets right that the obvious arrangement does not. The
    // charts are tabbed rather than tiled: four plots at a quarter size each
    // are four plots nobody can read. And the leaderboard runs along the
    // bottom rather than down the side, because it is a wide table -- eight
    // columns in a sidebar truncates every one of them to an ellipsis.
    ImGuiID centre = id;
    const ImGuiID left =
        ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.20F, nullptr, &centre);
    const ImGuiID right =
        ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.26F, nullptr, &centre);
    const ImGuiID bottom =
        ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.36F, nullptr, &centre);

    // The remaining half of the split has to be captured. Splitting a node
    // turns it into a parent, and DockBuilderDockWindow only accepts leaves --
    // so docking into `right` after splitting it silently does nothing, and
    // the window ends up floating over the layout instead.
    ImGuiID right_top = 0;
    const ImGuiID right_bottom =
        ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45F, nullptr, &right_top);

    ImGui::DockBuilderDockWindow("Plugin Manager", left);

    ImGui::DockBuilderDockWindow("Equity Curves", centre);
    ImGui::DockBuilderDockWindow("Drawdown", centre);
    ImGui::DockBuilderDockWindow("Relative Strength", centre);
    ImGui::DockBuilderDockWindow("Correlation", centre);

    ImGui::DockBuilderDockWindow("Theory Detail", right_top);
    ImGui::DockBuilderDockWindow("Universe", right_top);
    ImGui::DockBuilderDockWindow("Positions & P&L", right_bottom);

    ImGui::DockBuilderDockWindow("Leaderboard", bottom);
    ImGui::DockBuilderDockWindow("Blotter", bottom);
    ImGui::DockBuilderDockWindow("Transport", bottom);
    ImGui::DockBuilderDockWindow("Latency", bottom);
    ImGui::DockBuilderDockWindow("Log", bottom);

    ImGui::DockBuilderFinish(id);

    // Choose which tab each tabbed node opens on, or the application starts up
    // looking at the log instead of at the results. A docked window's tab id
    // is its window id, which is the hash of its name.
    //
    // This is not the whole story -- see draw_frame, which finishes the job
    // for whichever node holds the keyboard focus -- but it is what stops the
    // other nodes flickering through a tab nobody asked for on the way.
    const auto open_on = [](ImGuiID node_id, const char* window_name) {
        if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(node_id); node != nullptr) {
            node->SelectedTabId = ImHashStr(window_name);
        }
    };
    open_on(centre, "Equity Curves");
    open_on(bottom, "Leaderboard");

    default_focus_stage_ = 3;
}

}  // namespace marketscan::app
