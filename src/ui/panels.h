#pragma once

#include "ui/app_state.h"

namespace rd::ui {

void panel_viewport(AppState& app);
void panel_pipeline(AppState& app);
void panel_regions(AppState& app);
void panel_profile(AppState& app);
void panel_director(AppState& app);
void panel_validation(AppState& app);
void panel_gallery(AppState& app);
void panel_log(AppState& app);
void panel_stats(AppState& app);
void draw_about_modal(AppState& app);

// Shared actions, used by the menu bar and by more than one panel.
void action_open_mesh(AppState& app);
void action_run(AppState& app);
void action_rebuild(AppState& app);
void action_apply_panel(AppState& app);

} // namespace rd::ui
