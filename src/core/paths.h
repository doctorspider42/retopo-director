#pragma once

// Where things live on disk. Everything the pipeline writes goes under the
// project directory so a run can be zipped up and shipped to someone else.

#include <filesystem>
#include <string>
#include <vector>

namespace rd::paths {

namespace fs = std::filesystem;

void init(const char* argv0);

// Directory containing the executable.
const fs::path& exe_dir();

// Bundled read only data (target profiles shipped with the build).
fs::path profiles_dir();

// Per user configuration, e.g. the selected LLM backend and API endpoints.
fs::path config_dir();
fs::path config_file();

// Active project. Defaults to <exe_dir>/work but can be pointed anywhere.
const fs::path& project_dir();
void            set_project_dir(const fs::path& p);

// Sub directories of the project, created on demand.
fs::path renders_dir();               // reference renders of the high poly
fs::path iteration_dir(int iteration); // per iteration renders / meshes / knobs
fs::path bake_dir();
fs::path export_dir();
fs::path reports_dir();

// mkdir -p, returns false and logs on failure.
bool ensure_dir(const fs::path& p);

// Convenience: read/write whole files, returning success.
bool read_file(const fs::path& p, std::string& out);
bool read_file(const fs::path& p, std::vector<uint8_t>& out);
bool write_file(const fs::path& p, const std::string& data);
bool write_file(const fs::path& p, const void* data, size_t size);

// Lowercase extension without the dot.
std::string extension_of(const fs::path& p);

// Human readable byte count, e.g. "3.4 MB".
std::string format_bytes(uint64_t bytes);

} // namespace rd::paths
