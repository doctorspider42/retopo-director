#pragma once

// Native file pickers where the platform has one, a plain path prompt where it
// does not. Kept behind a tiny interface so no panel has to care.

#include <filesystem>
#include <string>
#include <vector>

namespace rd::ui {

struct FileFilter {
    std::string label;        // "Meshes"
    std::string extensions;   // "obj;gltf;glb"
};

// Returns an empty path when the user cancels.
std::filesystem::path open_file_dialog(const std::string& title,
                                       const std::vector<FileFilter>& filters,
                                       const std::filesystem::path& initial = {});

std::filesystem::path save_file_dialog(const std::string& title,
                                       const std::vector<FileFilter>& filters,
                                       const std::string& default_name,
                                       const std::filesystem::path& initial = {});

std::filesystem::path pick_folder_dialog(const std::string& title,
                                         const std::filesystem::path& initial = {});

// True when the platform provides real dialogs; panels fall back to a text
// field when it does not.
bool native_dialogs_available();

// Opens a path in the system file browser. Best effort.
void reveal_in_file_manager(const std::filesystem::path& path);

} // namespace rd::ui
