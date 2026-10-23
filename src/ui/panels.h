#pragma once

#include "ui/app_state.h"

#include <imgui.h>

struct GLFWwindow;

namespace rd::ui {

// --- the frame -------------------------------------------------------------
// The window is one fixed layout: a bar along the top, the viewport, the
// inspector on the right, a status bar along the bottom and a log that slides
// up over the viewport. Each surface is handed its rectangle by the main loop.
void draw_topbar(AppState& app, const ImVec2& pos, const ImVec2& size, bool& want_quit,
                 bool& want_screenshot);
void draw_viewport(AppState& app, const ImVec2& pos, const ImVec2& size);
void draw_inspector(AppState& app, const ImVec2& pos, const ImVec2& size);
void draw_statusbar(AppState& app, const ImVec2& pos, const ImVec2& size);
void draw_log_drawer(AppState& app, const ImVec2& pos, const ImVec2& size);

// Overlays: the settings sheet, the render lightbox, the about and keyboard
// sheets, the toast.
void draw_options(AppState& app);
void draw_lightbox(AppState& app);
void draw_about_modal(AppState& app);
void draw_shortcuts_modal(AppState& app);
void draw_toast(AppState& app);

// The inspector's tabs.
void tab_tune(AppState& app);
void tab_report(AppState& app);
void tab_director(AppState& app);
void tab_renders(AppState& app);

// The settings sheet's pages.
void draw_profile_settings(AppState& app);
void draw_director_settings(AppState& app);
void draw_segmenter_settings(AppState& app);
void draw_run_settings(AppState& app);

// The director arguing with the brief. Shown on the target page and on the
// Director tab; returns true when a proposal was applied.
bool   draw_profile_advice(AppState& app, TargetProfile& profile);
ImVec4 feasibility_color(Feasibility f);

// A fixed, undecorated window at `pos` x `size`. Always pair with end_surface.
bool begin_surface(const char* name, const ImVec2& pos, const ImVec2& size, const ImVec4& bg,
                   const ImVec2& padding, ImGuiWindowFlags extra = 0);
void end_surface();

// How far along the whole run is, 0..1, mapping the current stage and iteration
// onto one number. The status bar and the run card both show it.
float overall_progress(const AppState& app);

// Shared actions, used by the top bar, the shortcuts and more than one tab.
void action_open_mesh(AppState& app);
void action_run(AppState& app);
void action_rebuild(AppState& app);
void action_apply_panel(AppState& app);
void action_revert_panel(AppState& app);
bool can_rebuild(const AppState& app);

// "ps2_character" as a person would write it: "PS2 character".
std::string profile_display_name(const std::string& name);

// The faceted mark as the window and taskbar icon.
void set_window_icon(GLFWwindow* window);

} // namespace rd::ui
