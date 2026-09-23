#include "ui/app.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/thread_pool.h"
#include "core/util.h"
#include "llm/http.h"
#include "render/gl.h"
#include "segment/sam.h"
#include "ui/app_state.h"
#include "ui/file_dialog.h"
#include "ui/panels.h"
#include "ui/theme.h"
#include "ui/widgets.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <imgui.h>
// The DockBuilder functions that lay out the default workspace are internal.
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <thread>

namespace rd {

namespace fs = std::filesystem;

namespace {

void glfw_error_callback(int code, const char* description)
{
    RD_ERROR("glfw error %d: %s", code, description);
}

// ---------------------------------------------------------------------------
// Docking layout, built once and then left to the user's imgui.ini.
// ---------------------------------------------------------------------------
void build_default_layout(ImGuiID dockspace)
{
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);

    ImGuiID centre = dockspace;
    const ImGuiID left   = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.24f, nullptr, &centre);
    const ImGuiID right  = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.30f, nullptr, &centre);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.26f, nullptr, &centre);

    ImGui::DockBuilderDockWindow("Pipeline", left);
    ImGui::DockBuilderDockWindow("Viewport", centre);
    ImGui::DockBuilderDockWindow("Renders", centre);
    ImGui::DockBuilderDockWindow("Knob panel", right);
    ImGui::DockBuilderDockWindow("Statistics", right);
    ImGui::DockBuilderDockWindow("Validation", bottom);
    ImGui::DockBuilderDockWindow("Director", bottom);
    ImGui::DockBuilderDockWindow("Log", bottom);

    ImGui::DockBuilderFinish(dockspace);
}

// ---------------------------------------------------------------------------
void draw_menu_bar(ui::AppState& app, bool& want_quit, bool& want_reset_layout)
{
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open mesh...", "Ctrl+O")) ui::action_open_mesh(app);

        if (ImGui::BeginMenu("Open recent", !app.recent_meshes.empty())) {
            // PushID per row because two files can share a name, and the label
            // is what ImGui hashes into the item id otherwise. Acting on the
            // choice waits until the loop is over: open_mesh rewrites the very
            // vector being walked.
            fs::path chosen;
            for (size_t i = 0; i < app.recent_meshes.size(); ++i) {
                ImGui::PushID(int(i));
                if (ImGui::MenuItem(app.recent_label(i).c_str()))
                    chosen = app.recent_meshes[i];
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", app.recent_meshes[i].string().c_str());
                ImGui::PopID();
            }
            ImGui::EndMenu();
            if (!chosen.empty()) app.open_mesh(chosen);
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Open project folder"))
            ui::reveal_in_file_manager(paths::project_dir());
        if (ImGui::MenuItem("Options...", "Ctrl+,")) app.show_options = true;

        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Alt+F4")) want_quit = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Run")) {
        const bool running = app.pipeline.running();
        if (ImGui::MenuItem(app.settings.use_llm ? "Run with the director" : "Run",
                            "F5", false, !running && !app.mesh_path.empty()))
            ui::action_run(app);
        if (ImGui::MenuItem("Rebuild the low poly", "F6", false, !running))
            ui::action_rebuild(app);
        if (ImGui::MenuItem("Stop", "Esc", false, running)) app.pipeline.cancel();
        ImGui::Separator();
        ImGui::MenuItem("Let the director decide", nullptr, &app.settings.use_llm);
        ImGui::Separator();
        if (ImGui::MenuItem("Apply knob panel", "Ctrl+Return", false, app.panel_dirty))
            ui::action_apply_panel(app);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Viewport", nullptr, &app.show_viewport);
        ImGui::MenuItem("Pipeline", nullptr, &app.show_pipeline);
        ImGui::MenuItem("Knob panel", nullptr, &app.show_regions);
        ImGui::MenuItem("Director transcript", nullptr, &app.show_director);
        ImGui::MenuItem("Validation", nullptr, &app.show_validation);
        ImGui::MenuItem("Renders", nullptr, &app.show_gallery);
        ImGui::MenuItem("Statistics", nullptr, &app.show_stats);
        ImGui::MenuItem("Log", nullptr, &app.show_log);
        ImGui::Separator();
        ImGui::MenuItem("Options window", "Ctrl+,", &app.show_options);
        if (ImGui::MenuItem("Reset layout")) want_reset_layout = true;
        ImGui::MenuItem("ImGui demo", nullptr, &app.show_demo);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About")) app.show_about = true;
        ImGui::EndMenu();
    }

    // Right aligned: the file, since the stage and the progress now have a bar
    // of their own along the bottom.
    {
        const ui::Palette& p = ui::palette();
        const Stage stage = app.pipeline.stage();
        const std::string text = app.mesh_path.empty()
                                     ? std::string("no mesh loaded")
                                     : app.mesh_path.filename().string();

        const float width = ImGui::CalcTextSize(text.c_str()).x + 24.0f;
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - width);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              stage == Stage::Failed ? p.danger : p.text_faint);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopStyleColor();
        if (!app.mesh_path.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", app.mesh_path.string().c_str());
    }

    ImGui::EndMainMenuBar();
}

// ---------------------------------------------------------------------------
// One bar along the bottom for the whole run.
//
// A per stage bar tucked inside a panel answers "what is it doing"; nobody was
// asking that. The question during the two minutes this takes is how much is
// left, so the answer sits across the bottom of the window where it cannot be
// docked away, scrolled past or covered up.
// ---------------------------------------------------------------------------
void draw_status_bar(ui::AppState& app)
{
    const ui::Palette& p = ui::palette();
    const float height = ImGui::GetFrameHeight() + 14.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 7));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, p.surface);
    // Like Begin, this has to be closed whether or not it returned true.
    const bool open = ImGui::BeginViewportSideBar(
        "##status_bar", ImGui::GetMainViewport(), ImGuiDir_Down, height,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar);

    if (open) {
        const Stage stage   = app.pipeline.stage();
        const bool  running = app.pipeline.running();
        const float overall = ui::overall_progress(app);

        ImVec4 tint = p.text_faint;
        if (stage == Stage::Failed)         tint = p.danger;
        else if (stage == Stage::Cancelled) tint = p.warning;
        else if (stage == Stage::Done)      tint = p.success;
        else if (running)                   tint = p.accent;

        ImGui::AlignTextToFramePadding();
        ui::dot(tint, 5.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, tint);
        ImGui::TextUnformatted(running || stage != Stage::Idle ? stage_name(stage)
                               : (app.mesh_path.empty() ? "Waiting for a mesh" : "Ready"));
        ImGui::PopStyleColor();

        const int iteration = app.pipeline.iteration();
        if (running && iteration > 0) {
            ImGui::SameLine(0.0f, 10.0f);
            ui::badge(format("iteration %d", iteration).c_str(), p.accent);
        }

        // Reserve the right hand side before measuring what is left, so the
        // readout and the stop button never get pushed off the edge.
        const float tail = running ? 148.0f : 64.0f;
        ImGui::SameLine(0.0f, 16.0f);
        const float bar = std::max(80.0f, ImGui::GetContentRegionAvail().x - tail);

        // The last stage message is only worth repeating while something is
        // happening; at rest it is the name of a file already on screen twice.
        const std::string msg = running ? app.pipeline.message() : std::string{};
        const float y = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(y + 5.0f);
        ui::progress_bar(overall, msg.empty() ? nullptr : msg.c_str(), 16.0f, bar);
        ImGui::SetCursorPosY(y);

        ImGui::SameLine(0.0f, 12.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, p.text_dim);
        ImGui::TextUnformatted(format("%3.0f%%", overall * 100.0f).c_str());
        ImGui::PopStyleColor();

        if (running) {
            ImGui::SameLine(0.0f, 10.0f);
            if (ui::danger_button("Stop", ImVec2(70, 0))) app.pipeline.cancel();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void draw_toast(ui::AppState& app)
{
    if (app.toast.empty() || ImGui::GetTime() > app.toast_until) return;

    const ui::Palette& p = ui::palette();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 size = ImGui::CalcTextSize(app.toast.c_str());

    ImGui::SetNextWindowPos({vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                             vp->WorkPos.y + vp->WorkSize.y - 48.0f},
                            ImGuiCond_Always, {0.5f, 1.0f});
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 12));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, p.elevated);

    if (ImGui::Begin("##toast", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav)) {
        ImGui::Dummy(ImVec2(std::max(size.x, 180.0f), 0.0f));
        ImGui::TextUnformatted(app.toast.c_str());
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void handle_shortcuts(ui::AppState& app, bool& want_quit, bool& want_screenshot)
{
    const ImGuiIO& io = ImGui::GetIO();
    // F12 works even while typing: it is the one key that cannot damage anything.
    if (ImGui::IsKeyPressed(ImGuiKey_F12, false)) want_screenshot = true;
    if (io.WantTextInput) return;

    const bool ctrl = io.KeyCtrl;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) ui::action_open_mesh(app);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Comma, false)) app.show_options = !app.show_options;
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false))        ui::action_run(app);
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false))        ui::action_rebuild(app);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) ui::action_apply_panel(app);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && app.pipeline.running())
        app.pipeline.cancel();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Q, false)) want_quit = true;

    // Viewport shortcuts, only when the pointer is not over a text field.
    if (ImGui::IsKeyPressed(ImGuiKey_1, false)) app.mode   = RenderMode::Shaded;
    if (ImGui::IsKeyPressed(ImGuiKey_2, false)) app.mode   = RenderMode::Regions;
    if (ImGui::IsKeyPressed(ImGuiKey_3, false)) app.mode   = RenderMode::Plain;
    if (ImGui::IsKeyPressed(ImGuiKey_4, false)) app.mode   = RenderMode::Checker;
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) app.wireframe = !app.wireframe;
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false) && !io.KeyCtrl)
        app.source = ui::ViewSource((int(app.source) + 1) % 3);
}

// ---------------------------------------------------------------------------
// Creates a window that is never shown, purely to get an OpenGL context. This
// is what makes headless mode a complete run rather than a degraded one: the
// director still sees renders and the silhouette metric still means something.
// Returns nullptr when there is no usable driver, which is a warning, not an
// error - the deterministic half does not need a GPU.
GLFWwindow* create_hidden_context()
{
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        RD_WARN("glfwInit failed; running without renders");
        return nullptr;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow* window = glfwCreateWindow(64, 64, "retopo-director (offscreen)",
                                          nullptr, nullptr);
    if (!window) {
        RD_WARN("no OpenGL 3.3 context available; running without renders");
        glfwTerminate();
        return nullptr;
    }

    glfwMakeContextCurrent(window);
    if (!gl::load()) {
        RD_WARN("OpenGL loader failed (%s); running without renders",
                gl::last_load_error());
        glfwDestroyWindow(window);
        glfwTerminate();
        return nullptr;
    }
    return window;
}

// Fetch a checkpoint and stop. Deliberately its own path: it needs no mesh, no
// profile, no GL context and no pipeline, and a machine that is only being
// provisioned should not have to start any of them.
int run_download(const std::string& model_type)
{
    log::set_echo_stderr(true);

    const SamCheckpoint* entry = nullptr;
    for (const SamCheckpoint& c : sam_checkpoints())
        if (model_type == c.model_type || iequals(model_type, c.label)) entry = &c;

    if (!entry) {
        std::fprintf(stderr, "error: unknown checkpoint '%s'; known:", model_type.c_str());
        for (const SamCheckpoint& c : sam_checkpoints())
            std::fprintf(stderr, " %s", c.model_type);
        std::fprintf(stderr, "\n");
        return 2;
    }

    std::string reason;
    if (!http_available(&reason)) {
        std::fprintf(stderr, "error: %s\n", reason.c_str());
        return 1;
    }

    const fs::path destination = sam_checkpoint_path(*entry);
    std::error_code ec;
    if (fs::exists(destination, ec)) {
        std::printf("%s is already there\n", destination.string().c_str());
        return 0;
    }

    std::printf("%s (%s), Apache-2.0, from %s\n", entry->label,
                paths::format_bytes(entry->bytes).c_str(), entry->url);

    HttpDownloadRequest req;
    req.url         = entry->url;
    req.destination = destination.string();

    // One line, rewritten in place, so a log file does not fill with progress.
    int last_percent = -1;
    req.progress = [&](uint64_t received, uint64_t total) {
        if (!total) return;
        const int percent = int(100.0 * double(received) / double(total));
        if (percent == last_percent) return;
        last_percent = percent;
        std::printf("\r  %3d%%  %s", percent, paths::format_bytes(received).c_str());
        std::fflush(stdout);
    };

    const HttpDownloadResult res = http_download(req);
    std::printf("\n");

    if (!res.ok) {
        std::fprintf(stderr, "error: %s\n", res.error.c_str());
        return 1;
    }
    std::printf("wrote %s in %s\n", destination.string().c_str(),
                format_duration(res.seconds).c_str());
    return 0;
}

int run_headless(const AppOptions& opts, ui::AppState& app)
{
    // No console panel here, so the ring buffer is mirrored to stderr.
    log::set_echo_stderr(true);
    RD_INFO("headless run");
    if (app.mesh_path.empty()) {
        std::fprintf(stderr, "error: --headless needs --mesh\n");
        return 2;
    }

    // Everything the pipeline renders happens on this thread, through the same
    // dispatcher the windowed build uses. The only difference is that nobody
    // ever sees the window.
    GLFWwindow* window = opts.no_gpu ? nullptr : create_hidden_context();
    if (window) {
        std::string error;
        if (app.renderer.init(&error)) {
            app.pipeline.set_renderer(&app.renderer);
            RD_INFO("offscreen renderer ready");
        } else {
            RD_WARN("renderer init failed (%s); running without renders", error.c_str());
        }
    }

    int rc = 1;
    if (app.pipeline.start(app.mesh_path, app.settings)) {
        Stage last = Stage::Idle;
        while (app.pipeline.running()) {
            // Servicing the queue is what lets the worker render at all.
            app.dispatcher.drain();
            if (window) glfwPollEvents();

            const Stage stage = app.pipeline.stage();
            if (stage != last) {
                last = stage;
                std::printf("[%s] %s\n", stage_name(stage), app.pipeline.message().c_str());
                std::fflush(stdout);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        app.dispatcher.drain();
        app.pipeline.join();

        app.pipeline.with_results([&](const PipelineResults& r) {
            std::printf("\n%s\n", r.validation.full_text().c_str());
            std::printf("low poly: %zu triangles, %zu vertices\n",
                        r.lowpoly.triangle_count(), r.lowpoly.vertex_count());
            if (!r.candidate_views.entries.empty())
                std::printf("silhouette error: %.4f mean, %.4f worst%s%s\n",
                            r.silhouette.mean, r.silhouette.worst,
                            r.silhouette.worst_view.empty() ? "" : " on ",
                            r.silhouette.worst_view.c_str());
            for (const auto& f : r.exported.files)
                std::printf("  wrote %s\n", f.string().c_str());
            rc = r.validation.passed ? 0 : 3;
        });
    }

    app.dispatcher.shutdown();
    if (window) {
        app.renderer.shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
    }
    return rc;
}

} // namespace

// ---------------------------------------------------------------------------
int run_application(const AppOptions& opts)
{
    if (!opts.sam_download.empty()) {
        paths::ensure_dir(paths::config_dir());
        return run_download(opts.sam_download);
    }

    auto app = std::make_unique<ui::AppState>();

    // Before anything that logs: a --verbose run wants the startup diagnostics
    // too, and in headless mode stderr is the only place they can be read.
    if (opts.verbose) log::set_echo_stderr(true);

    app->settings.profile = TargetProfile::ps2_character_default();
    if (opts.no_settings) RD_INFO("saved settings ignored (--no-settings)");
    else                  app->load_settings();

    // After load_settings, not before: the stored project directory is the one
    // the window was last pointed at, and it would otherwise quietly win over
    // the one asked for on the command line.
    if (!opts.project_dir.empty()) paths::set_project_dir(opts.project_dir);
    app->refresh_profiles();

    if (!opts.startup_profile.empty()) app->load_profile(opts.startup_profile);
    else if (!app->profile_files.empty() && app->settings.profile.name.empty())
        app->load_profile(app->profile_files.front());

    if (!opts.startup_mesh.empty()) {
        app->mesh_path = opts.startup_mesh;
        app->push_recent(app->mesh_path);
    }

    if (!opts.backend.empty()) {
        app->settings.backend       = backend_from_name(opts.backend);
        app->settings.force_backend = true;
        RD_INFO("backend forced to %s", backend_name(app->settings.backend));
    }
    if (!opts.segmenter.empty()) {
        if (parse_segmenter_kind(opts.segmenter, app->settings.segmenter.kind))
            RD_INFO("segmenter forced to %s",
                    segmenter_kind_name(app->settings.segmenter.kind));
        else
            RD_WARN("unknown segmenter '%s', keeping %s", opts.segmenter.c_str(),
                    segmenter_kind_name(app->settings.segmenter.kind));
    }
    if (!opts.sam_checkpoint.empty()) app->settings.segmenter.sam.checkpoint = opts.sam_checkpoint;
    if (!opts.sam_device.empty())     app->settings.segmenter.sam.device     = opts.sam_device;

    if (opts.keep_hidden) app->settings.retopo.hard_rules.drop_hidden_shells = false;
    if (!opts.replay.empty()) {
        app->settings.llm.kind       = LlmBackendKind::Replay;
        app->settings.llm.replay_dir = opts.replay;
        app->settings.use_llm        = true;
    }

    // --no-llm wins over --llm: the one that takes something away is the safe
    // reading of a contradictory command line.
    if (opts.force_llm) app->settings.use_llm = true;
    if (opts.no_llm)    app->settings.use_llm = false;

    // Otherwise a run with the director switched off in the saved settings looks
    // exactly like a run where the director had nothing to say, which cost an
    // afternoon once already.
    if (!app->settings.use_llm)
        RD_INFO("the director is off (%s); running the deterministic half only",
                opts.no_llm ? "--no-llm" : "saved settings, override with --llm");

    app->pipeline.set_dispatcher(&app->dispatcher);

    RD_INFO("worker threads: %u", ThreadPool::shared().lane_count());

    if (opts.headless) {
        return run_headless(opts, *app);
    }

    // --- window ---------------------------------------------------------------
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        RD_ERROR("glfwInit failed");
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_SAMPLES, 0);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow* window = glfwCreateWindow(1680, 980, "Retopo Director", nullptr, nullptr);
    if (!window) {
        RD_ERROR("cannot create a window; is there an OpenGL 3.3 driver?");
        glfwTerminate();
        return 1;
    }
    app->window = window;

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    if (!gl::load()) {
        RD_ERROR("OpenGL 3.3 is not available: %s", gl::last_load_error());
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // --- imgui -----------------------------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    static std::string ini_path = (paths::config_dir() / "layout.ini").string();
    paths::ensure_dir(paths::config_dir());
    io.IniFilename = ini_path.c_str();

    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    const float dpi = std::clamp(std::max(xscale, yscale), 1.0f, 3.0f);

    ui::load_fonts(dpi);
    ui::apply_theme(dpi);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    std::string render_error;
    if (!app->renderer.init(&render_error))
        RD_ERROR("renderer init failed: %s", render_error.c_str());
    app->renderer_alt.init(nullptr);
    app->pipeline.set_renderer(&app->renderer);

    glfwShowWindow(window);

    bool want_quit         = false;
    bool want_reset_layout = false;
    bool layout_built      = false;
    bool autorun_pending   = opts.autorun && !app->mesh_path.empty();

    // A mesh named on the command line goes on screen before anything is
    // pressed, exactly like one picked from the dialog. Not when the run starts
    // by itself: that would load the same file twice.
    if (!app->mesh_path.empty() && !autorun_pending) app->open_mesh(app->mesh_path);

    // Screenshots are taken from the back buffer, so they need neither focus
    // nor an unobstructed window. F12 takes one at any time; the command line
    // options exist so a script can drive the interface unattended.
    bool     screenshot_pending = false;
    bool     screenshot_done    = false;
    fs::path screenshot_path    = opts.screenshot.empty()
                                      ? fs::path{}
                                      : fs::absolute(fs::path(opts.screenshot));
    const double start_time = glfwGetTime();

    // --- main loop -------------------------------------------------------------
    while (!glfwWindowShouldClose(window) && !want_quit) {
        glfwPollEvents();
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        // Anything the pipeline needs the GPU for runs right here.
        app->dispatcher.drain();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const ImGuiID dockspace = ImGui::DockSpaceOverViewport(
            0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

        if (!layout_built || want_reset_layout) {
            if (want_reset_layout || !ImGui::DockBuilderGetNode(dockspace) ||
                ImGui::DockBuilderGetNode(dockspace)->IsEmpty())
                build_default_layout(dockspace);
            layout_built      = true;
            want_reset_layout = false;
        }

        app->sync_from_pipeline();
        app->upload_meshes();

        if (autorun_pending && ImGui::GetFrameCount() > 3) {
            autorun_pending = false;
            ui::action_run(*app);
        }

        draw_menu_bar(*app, want_quit, want_reset_layout);
        bool manual_screenshot = false;
        handle_shortcuts(*app, want_quit, manual_screenshot);
        if (manual_screenshot) {
            if (screenshot_path.empty() || screenshot_done) {
                const std::time_t now = std::time(nullptr);
                std::tm tm{};
#if defined(RD_PLATFORM_WINDOWS)
                localtime_s(&tm, &now);
#else
                localtime_r(&now, &tm);
#endif
                char name[64];
                std::strftime(name, sizeof(name), "shot_%Y%m%d_%H%M%S.png", &tm);
                screenshot_path = paths::project_dir() / "screenshots" / name;
            }
            paths::ensure_dir(screenshot_path.parent_path());
            screenshot_pending = true;
        }

        ui::panel_viewport(*app);
        ui::panel_pipeline(*app);
        ui::panel_regions(*app);
        ui::panel_director(*app);
        ui::panel_validation(*app);
        ui::panel_gallery(*app);
        ui::panel_stats(*app);
        ui::panel_log(*app);
        ui::panel_options(*app);
        ui::draw_about_modal(*app);
        draw_status_bar(*app);
        draw_toast(*app);

        if (app->show_demo) ImGui::ShowDemoWindow(&app->show_demo);

        if (app->auto_rotate && !app->use_profile_camera)
            app->camera.yaw += float(io.DeltaTime) * 18.0f;

        ImGui::Render();

        int width = 0, height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);
        const ui::Palette& p = ui::palette();
        glClearColor(p.canvas.x, p.canvas.y, p.canvas.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Read the back buffer before the swap. That is what makes this work
        // with the window unfocused, partly covered or on another desktop:
        // nothing is being read off the screen.
        if (screenshot_pending && width > 0 && height > 0) {
            screenshot_pending = false;
            Texture shot;
            if (capture_framebuffer(width, height, shot) && shot.save_png(screenshot_path)) {
                RD_INFO("screenshot: %s (%dx%d)", screenshot_path.string().c_str(),
                        width, height);
                app->notify("Screenshot written to " + screenshot_path.filename().string());
            } else {
                RD_ERROR("could not write %s", screenshot_path.string().c_str());
            }
            screenshot_done = true;
        }

        glfwSwapBuffers(window);

        // --- automation -------------------------------------------------------
        const double uptime = glfwGetTime() - start_time;
        if (!opts.screenshot.empty() && !screenshot_done && !screenshot_pending) {
            const bool by_clock = opts.screenshot_delay > 0.0 &&
                                  uptime >= opts.screenshot_delay;
            // With no delay given, wait for the run to settle rather than
            // catching a half drawn first frame.
            const bool by_run = opts.screenshot_delay <= 0.0 && !autorun_pending &&
                                !app->pipeline.running() &&
                                stage_is_terminal(app->pipeline.stage()) && uptime > 1.0;
            if (by_clock || by_run) {
                paths::ensure_dir(screenshot_path.parent_path());
                screenshot_pending = true;
            }
        }
        if (opts.exit_after > 0.0 && uptime >= opts.exit_after &&
            (opts.screenshot.empty() || screenshot_done))
            want_quit = true;
    }

    // --- shutdown ---------------------------------------------------------------
    // A download in flight is abandoned rather than waited out: it writes to a
    // .part file that is removed on the way out, so nothing half written is
    // left looking like a checkpoint.
    app->checkpoint_download.join();

    app->pipeline.cancel();
    // Keep servicing render requests so a worker blocked on one can finish.
    while (app->pipeline.running()) {
        app->dispatcher.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    app->dispatcher.shutdown();
    app->pipeline.join();

    app->save_settings();

    app->images.clear();
    app->gpu_high.release();
    app->gpu_low.release();
    app->renderer.shutdown();
    app->renderer_alt.shutdown();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

} // namespace rd
