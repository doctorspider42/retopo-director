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

// Files dropped on the window. GLFW calls this from inside glfwPollEvents, on the
// main thread, so the pointer needs no lock.
ui::AppState* g_drop_target = nullptr;

void drop_callback(GLFWwindow*, int count, const char** paths)
{
    if (!g_drop_target) return;
    for (int i = 0; i < count; ++i) g_drop_target->dropped.emplace_back(paths[i]);
}

void handle_shortcuts(ui::AppState& app, bool& want_quit, bool& want_screenshot)
{
    const ImGuiIO& io = ImGui::GetIO();
    // F12 works even while typing: it is the one key that cannot damage anything.
    if (ImGui::IsKeyPressed(ImGuiKey_F12, false)) want_screenshot = true;
    if (io.WantTextInput) return;

    const bool ctrl = io.KeyCtrl;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Q, false)) want_quit = true;
    // A sheet over the window owns the keyboard; it closes itself on Escape.
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        return;

    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) ui::action_open_mesh(app);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Comma, false)) app.show_options = true;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_L, false)) app.show_log = !app.show_log;
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false))        ui::action_run(app);
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false))        ui::action_rebuild(app);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) ui::action_apply_panel(app);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && app.pipeline.running())
        app.pipeline.cancel();
    if (ImGui::IsKeyPressed(ImGuiKey_Slash, false) && io.KeyShift) app.show_shortcuts = true;
    if (ctrl) return;

    const RenderMode before = app.mode;
    if (ImGui::IsKeyPressed(ImGuiKey_1, false)) app.mode   = RenderMode::Shaded;
    if (ImGui::IsKeyPressed(ImGuiKey_2, false)) app.mode   = RenderMode::Regions;
    if (ImGui::IsKeyPressed(ImGuiKey_3, false)) app.mode   = RenderMode::Plain;
    if (ImGui::IsKeyPressed(ImGuiKey_4, false)) app.mode   = RenderMode::Checker;
    if (app.mode != before) app.gpu_version = 0;   // region colours come and go with the mode
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) app.wireframe = !app.wireframe;
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        app.use_profile_camera = false;
        app.camera.frame(app.scene_bounds);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
        app.source           = ui::ViewSource((int(app.source) + 1) % 3);
        app.auto_view_source = false;
    }
}

// Notices a run starting and ending, so the clock and the inspector follow it:
// a finished run lands on its report, which is the first thing anybody asks.
void follow_run(ui::AppState& app)
{
    const Stage stage   = app.pipeline.stage();
    const bool  running = app.pipeline.running();
    const bool  was_terminal = app.last_stage == Stage::Idle || stage_is_terminal(app.last_stage);
    if (running && was_terminal && stage != app.last_stage) app.run_started = ImGui::GetTime();
    if (stage == Stage::Done && app.last_stage != Stage::Done && app.jump_to_report) {
        app.tab            = ui::AppState::Tab::Report;
        app.jump_to_report = false;
    }
    app.last_stage = stage;
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
    if (!opts.llm_model.empty()) app->settings.llm.claude_model = opts.llm_model;
    if (!opts.llm_path.empty())  app->settings.llm.claude_path  = opts.llm_path;
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

    GLFWwindow* window = glfwCreateWindow(1680, 980, "PS2 Retopo Director", nullptr, nullptr);
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
    // The layout is fixed and computed every frame, so there is nothing for an
    // ini file to remember.
    io.IniFilename = nullptr;

    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    const float dpi = std::clamp(std::max(xscale, yscale), 1.0f, 3.0f);

    ui::load_fonts(dpi);
    ui::apply_theme(dpi);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    g_drop_target = app.get();
    glfwSetDropCallback(window, drop_callback);
    ui::set_window_icon(window);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    std::string render_error;
    if (!app->renderer.init(&render_error))
        RD_ERROR("renderer init failed: %s", render_error.c_str());
    app->renderer_alt.init(nullptr);
    app->pipeline.set_renderer(&app->renderer);

    glfwShowWindow(window);

    bool want_quit         = false;
    bool autorun_pending   = opts.autorun && !app->mesh_path.empty();

    // A mesh named on the command line goes on screen before anything is
    // pressed, exactly like one picked from the dialog. Not when the run starts
    // by itself: that would load the same file twice.
    if (!app->mesh_path.empty() && !autorun_pending) app->open_mesh(app->mesh_path);

    // Asked for on the command line: pinned for the whole session, so a run
    // ending does not move the inspector off the tab a screenshot wants.
    bool pin_tab = false;
    {
        static const char* const kTabs[] = {"tune", "report", "director", "renders"};
        for (int i = 0; i < 4; ++i)
            if (iequals(opts.ui_tab, kTabs[i])) {
                app->tab = ui::AppState::Tab(i);
                pin_tab  = true;
            }
        static const char* const kViews[] = {"low", "source", "compare"};
        for (int i = 0; i < 3; ++i)
            if (iequals(opts.ui_view, kViews[i])) {
                app->source           = ui::ViewSource(i);
                app->auto_view_source = false;
            }
        // The profile's cameras exist only once a run has framed them, so the
        // name waits until they do.
        app->pending_camera = opts.ui_camera;
        if (iequals(opts.ui_show, "settings"))  app->show_options   = true;
        if (iequals(opts.ui_show, "regions"))   app->open_regions_picker = true;
        if (iequals(opts.ui_show, "log"))       app->show_log       = true;
        if (iequals(opts.ui_show, "shortcuts")) app->show_shortcuts = true;
        if (iequals(opts.ui_show, "about"))     app->show_about     = true;
    }
    const ui::AppState::Tab pinned_tab = app->tab;

    // Headless runs never get here, so the benchmark and the tests stay off
    // the network; --no-settings keeps a scripted window off it too.
    if (ui::Updater::enabled() && app->check_updates && !opts.no_settings)
        app->updater.check(true);

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

        app->sync_from_pipeline();
        app->upload_meshes();
        app->upload_highlight();
        if (!app->pending_camera.empty()) {
            app->pipeline.with_results([&](const PipelineResults& r) {
                for (size_t i = 0; i < r.cameras.size(); ++i)
                    if (iequals(r.cameras[i].name, app->pending_camera)) {
                        app->use_profile_camera   = true;
                        app->profile_camera_index = int(i);
                        app->pending_camera.clear();
                        break;
                    }
            });
        }

        if (autorun_pending && ImGui::GetFrameCount() > 3) {
            autorun_pending = false;
            ui::action_run(*app);
        }

        // A file dropped on the window opens like one picked from the dialog.
        if (!app->dropped.empty()) {
            const fs::path dropped = app->dropped.back();
            app->dropped.clear();
            if (app->pipeline.running())
                app->notify("Busy; drop it again when the run has finished", 4.0);
            else {
                app->open_mesh(dropped);
                app->notify("Loaded " + dropped.filename().string());
            }
        }

        follow_run(*app);

        // A check somebody asked for gets an answer either way; the quiet one
        // at start only speaks up when there is something new.
        if (ui::UpdateState news; app->updater.take_news(&news)) {
            const bool asked = !app->updater.quiet();
            if (news == ui::UpdateState::Available)
                app->notify(format("Build %d is out - see the top bar",
                                   app->updater.latest_build()), 6.0);
            else if (news == ui::UpdateState::UpToDate && asked)
                app->notify("This is the latest build");
            else if (news == ui::UpdateState::Failed && asked)
                app->notify("Update check failed: " + app->updater.error(), 6.0);
            else if (news == ui::UpdateState::Ready)
                app->notify("Update downloaded - restart from the top bar", 6.0);
        }
        if (pin_tab) app->tab = pinned_tab;

        bool manual_screenshot = false;
        handle_shortcuts(*app, want_quit, manual_screenshot);

        // --- the frame --------------------------------------------------------
        {
            const ui::Metrics& m   = ui::metrics();
            const float        d   = ui::dpi();
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            const ImVec2 o  = vp->WorkPos;
            const ImVec2 ws = vp->WorkSize;
            const float top    = m.topbar_height * d;
            const float bottom = m.statusbar_height * d;
            // The inspector only appears once there is something to inspect.
            const bool  inspect = !app->mesh_path.empty();
            const float side    = inspect ? std::min(app->inspector_width * d, ws.x * 0.5f) : 0.0f;
            const float logh    = app->show_log ? std::min(m.log_height * d, ws.y * 0.4f) : 0.0f;
            const float mid_h   = ws.y - top - bottom;

            ui::draw_topbar(*app, o, {ws.x, top}, want_quit, manual_screenshot);
            ui::draw_viewport(*app, {o.x, o.y + top}, {ws.x - side, mid_h - logh});
            if (logh > 0.0f)
                ui::draw_log_drawer(*app, {o.x, o.y + top + mid_h - logh}, {ws.x - side, logh});
            if (inspect)
                ui::draw_inspector(*app, {o.x + ws.x - side, o.y + top}, {side, mid_h});
            ui::draw_statusbar(*app, {o.x, o.y + ws.y - bottom}, {ws.x, bottom});
        }

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

        ui::draw_options(*app);
        ui::draw_lightbox(*app);
        ui::draw_about_modal(*app);
        ui::draw_shortcuts_modal(*app);
        ui::draw_toast(*app);

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
    app->updater.join();

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
