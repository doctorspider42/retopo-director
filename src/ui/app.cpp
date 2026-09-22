#include "ui/app.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/thread_pool.h"
#include "core/util.h"
#include "render/gl.h"
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
    ImGui::DockBuilderDockWindow("Target profile", left);
    ImGui::DockBuilderDockWindow("Viewport", centre);
    ImGui::DockBuilderDockWindow("Renders", centre);
    ImGui::DockBuilderDockWindow("Knob panel", right);
    ImGui::DockBuilderDockWindow("Director", right);
    ImGui::DockBuilderDockWindow("Statistics", right);
    ImGui::DockBuilderDockWindow("Log", bottom);
    ImGui::DockBuilderDockWindow("Validation", bottom);

    ImGui::DockBuilderFinish(dockspace);
}

// ---------------------------------------------------------------------------
void draw_menu_bar(ui::AppState& app, bool& want_quit, bool& want_reset_layout)
{
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open mesh...", "Ctrl+O")) ui::action_open_mesh(app);

        if (ImGui::BeginMenu("Open recent", !app.recent_meshes.empty())) {
            for (const auto& path : app.recent_meshes)
                if (ImGui::MenuItem(path.filename().string().c_str())) {
                    app.mesh_path = path;
                    app.push_recent(path);
                }
            ImGui::EndMenu();
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Set project folder...")) {
            const auto chosen = ui::pick_folder_dialog("Where should this run write?",
                                                       paths::project_dir());
            if (!chosen.empty()) {
                paths::set_project_dir(chosen);
                app.notify("Project folder: " + chosen.string());
            }
        }
        if (ImGui::MenuItem("Open project folder"))
            ui::reveal_in_file_manager(paths::project_dir());

        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Alt+F4")) want_quit = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Run")) {
        const bool running = app.pipeline.running();
        if (ImGui::MenuItem("Run the director", "F5", false, !running && !app.mesh_path.empty()))
            ui::action_run(app);
        if (ImGui::MenuItem("Rebuild geometry", "F6", false, !running))
            ui::action_rebuild(app);
        if (ImGui::MenuItem("Cancel", "Esc", false, running)) app.pipeline.cancel();
        ImGui::Separator();
        if (ImGui::MenuItem("Apply knob panel", "Ctrl+Return", false, app.panel_dirty))
            ui::action_apply_panel(app);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Viewport", nullptr, &app.show_viewport);
        ImGui::MenuItem("Pipeline", nullptr, &app.show_pipeline);
        ImGui::MenuItem("Knob panel", nullptr, &app.show_regions);
        ImGui::MenuItem("Target profile", nullptr, &app.show_profile);
        ImGui::MenuItem("Director", nullptr, &app.show_director);
        ImGui::MenuItem("Validation", nullptr, &app.show_validation);
        ImGui::MenuItem("Renders", nullptr, &app.show_gallery);
        ImGui::MenuItem("Statistics", nullptr, &app.show_stats);
        ImGui::MenuItem("Log", nullptr, &app.show_log);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset layout")) want_reset_layout = true;
        ImGui::MenuItem("ImGui demo", nullptr, &app.show_demo);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About")) app.show_about = true;
        ImGui::EndMenu();
    }

    // Right aligned status: what the pipeline is doing right now.
    {
        const ui::Palette& p = ui::palette();
        const Stage stage = app.pipeline.stage();
        const std::string text =
            app.pipeline.running()
                ? format("%s  %.0f%%", stage_name(stage), app.pipeline.progress() * 100.0f)
                : std::string(stage_name(stage));

        const float width = ImGui::CalcTextSize(text.c_str()).x + 24.0f;
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - width);
        ImVec4 tint = p.text_faint;
        if (stage == Stage::Failed)      tint = p.danger;
        else if (stage == Stage::Done)   tint = p.success;
        else if (app.pipeline.running()) tint = p.accent;
        ImGui::PushStyleColor(ImGuiCol_Text, tint);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::EndMainMenuBar();
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
    auto app = std::make_unique<ui::AppState>();

    if (!opts.project_dir.empty()) paths::set_project_dir(opts.project_dir);

    app->settings.profile = TargetProfile::ps2_character_default();
    app->load_settings();
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
    if (opts.no_llm)  app->settings.use_llm = false;
    if (opts.verbose) log::set_echo_stderr(true);

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
        ui::panel_profile(*app);
        ui::panel_director(*app);
        ui::panel_validation(*app);
        ui::panel_gallery(*app);
        ui::panel_stats(*app);
        ui::panel_log(*app);
        ui::draw_about_modal(*app);
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
