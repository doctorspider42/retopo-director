#pragma once

#include "ui/app_state.h"

namespace rd::ui {

void panel_viewport(AppState& app);
void panel_pipeline(AppState& app);
void panel_regions(AppState& app);
void panel_director(AppState& app);
// The second window: the target profile, the director backend and the region
// split. Floating, opened from the menu or from the Pipeline panel.
void panel_options(AppState& app);
void panel_validation(AppState& app);
void panel_gallery(AppState& app);
void panel_log(AppState& app);
void panel_stats(AppState& app);
void draw_about_modal(AppState& app);

// How far along the whole run is, 0..1, mapping the current stage and iteration
// onto one number. The status bar and the Pipeline panel both show it.
float overall_progress(const AppState& app);

// Shared actions, used by the menu bar and by more than one panel.
void action_open_mesh(AppState& app);
void action_run(AppState& app);
void action_rebuild(AppState& app);
void action_apply_panel(AppState& app);

} // namespace rd::ui
