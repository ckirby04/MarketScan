#include "app.hpp"

#include <algorithm>
#include <cstdio>

#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder, for the default layout

#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>
#include <implot.h>

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

    const std::string title = "MarketScan " + std::string(version::kString);
    window_ = glfwCreateWindow(options_.width, options_.height, title.c_str(), nullptr, nullptr);
    if (window_ == nullptr) {
        init_error_ = "could not create a window (no OpenGL 3.2 context available)";
        glfwTerminate();
        return false;
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
    layout_built_ = std::filesystem::exists(options_.layout_file);

    session_ = std::make_unique<Session>(options_.session);

    glfwShowWindow(window_);
    return true;
}

void Application::shutdown() {
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

    const ImGuiID dockspace_id = ImGui::GetID("##dockspace");
    if (!layout_built_ || ui_.request_reset_layout) {
        build_default_layout(dockspace_id);
        layout_built_ = true;
        ui_.request_reset_layout = false;
    }
    // Leave room for the status bar at the bottom.
    const float status_height = ImGui::GetFrameHeight();
    ImGui::DockSpace(dockspace_id, ImVec2(0.0F, -status_height), ImGuiDockNodeFlags_None);

    draw_status_bar();
    ImGui::End();

    // --- Panels ---------------------------------------------------------
    panels::Context context{*session_, *snapshot, ui_};
    panels::draw_all(context);

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
        ImGui::MenuItem("Leaderboard", nullptr, &ui_.show_leaderboard);
        ImGui::MenuItem("Equity Curves", nullptr, &ui_.show_equity);
        ImGui::MenuItem("Drawdown", nullptr, &ui_.show_drawdown);
        ImGui::MenuItem("Relative Strength", nullptr, &ui_.show_relative);
        ImGui::MenuItem("Correlation", nullptr, &ui_.show_correlation);
        ImGui::Separator();
        ImGui::MenuItem("Theory Detail", nullptr, &ui_.show_detail);
        ImGui::MenuItem("Positions & P&L", nullptr, &ui_.show_positions);
        ImGui::MenuItem("Blotter", nullptr, &ui_.show_blotter);
        ImGui::Separator();
        ImGui::MenuItem("Plugin Manager", nullptr, &ui_.show_plugins);
        ImGui::MenuItem("Transport", nullptr, &ui_.show_transport);
        ImGui::MenuItem("Latency", nullptr, &ui_.show_latency);
        ImGui::MenuItem("Log", nullptr, &ui_.show_log);
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
    //   left     leaderboard over plugin manager -- what is loaded, and how
    //            each one is doing
    //   centre   the charts, tabbed
    //   right    detail and positions for whichever theory is selected
    //   bottom   transport, blotter, latency and log, tabbed
    //
    // The charts are tabbed rather than tiled on purpose: four plots at a
    // quarter size each are four plots nobody can read.
    ImGuiID centre = id;
    const ImGuiID left =
        ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.22F, nullptr, &centre);
    const ImGuiID right =
        ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.27F, nullptr, &centre);
    const ImGuiID bottom =
        ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.34F, nullptr, &centre);
    const ImGuiID left_bottom =
        ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.45F, nullptr, nullptr);
    const ImGuiID right_bottom =
        ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45F, nullptr, nullptr);

    ImGui::DockBuilderDockWindow("Leaderboard", left);
    ImGui::DockBuilderDockWindow("Plugin Manager", left_bottom);

    ImGui::DockBuilderDockWindow("Equity Curves", centre);
    ImGui::DockBuilderDockWindow("Drawdown", centre);
    ImGui::DockBuilderDockWindow("Relative Strength", centre);
    ImGui::DockBuilderDockWindow("Correlation", centre);

    ImGui::DockBuilderDockWindow("Theory Detail", right);
    ImGui::DockBuilderDockWindow("Positions & P&L", right_bottom);

    ImGui::DockBuilderDockWindow("Transport", bottom);
    ImGui::DockBuilderDockWindow("Blotter", bottom);
    ImGui::DockBuilderDockWindow("Latency", bottom);
    ImGui::DockBuilderDockWindow("Log", bottom);

    ImGui::DockBuilderFinish(id);
}

}  // namespace marketscan::app
