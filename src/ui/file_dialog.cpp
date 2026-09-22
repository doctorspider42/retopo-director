#include "ui/file_dialog.h"

#include "core/log.h"
#include "core/util.h"

#include <algorithm>

#if defined(RD_PLATFORM_WINDOWS)
#  include <windows.h>
#  include <commdlg.h>
#  include <shellapi.h>
#  include <shlobj.h>
#endif

namespace rd::ui {
namespace fs = std::filesystem;

#if defined(RD_PLATFORM_WINDOWS)
namespace {

std::wstring widen(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

// COMMDLG wants a double-null terminated list of "label\0pattern\0" pairs.
std::wstring build_filter(const std::vector<FileFilter>& filters)
{
    std::wstring out;
    for (const FileFilter& f : filters) {
        std::string patterns;
        for (const std::string& ext : split(f.extensions, ';')) {
            if (!patterns.empty()) patterns += ";";
            patterns += "*." + ext;
        }
        out += widen(f.label + " (" + patterns + ")");
        out.push_back(L'\0');
        out += widen(patterns);
        out.push_back(L'\0');
    }
    out += widen("All files (*.*)");
    out.push_back(L'\0');
    out += widen("*.*");
    out.push_back(L'\0');
    out.push_back(L'\0');
    return out;
}

} // namespace

bool native_dialogs_available() { return true; }

fs::path open_file_dialog(const std::string& title, const std::vector<FileFilter>& filters,
                          const fs::path& initial)
{
    wchar_t buffer[MAX_PATH * 4] = {0};

    const std::wstring filter  = build_filter(filters);
    const std::wstring caption = widen(title);
    const std::wstring dir     = initial.empty() ? std::wstring() : initial.wstring();

    OPENFILENAMEW ofn{};
    ofn.lStructSize     = sizeof(ofn);
    ofn.lpstrFilter     = filter.c_str();
    ofn.lpstrFile       = buffer;
    ofn.nMaxFile        = DWORD(std::size(buffer));
    ofn.lpstrTitle      = caption.c_str();
    ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn)) return {};
    return fs::path(buffer);
}

fs::path save_file_dialog(const std::string& title, const std::vector<FileFilter>& filters,
                          const std::string& default_name, const fs::path& initial)
{
    wchar_t buffer[MAX_PATH * 4] = {0};
    const std::wstring initial_name = widen(default_name);
    std::copy(initial_name.begin(),
              initial_name.begin() + long(std::min<size_t>(initial_name.size(), MAX_PATH)),
              buffer);

    const std::wstring filter  = build_filter(filters);
    const std::wstring caption = widen(title);
    const std::wstring dir     = initial.empty() ? std::wstring() : initial.wstring();

    OPENFILENAMEW ofn{};
    ofn.lStructSize     = sizeof(ofn);
    ofn.lpstrFilter     = filter.c_str();
    ofn.lpstrFile       = buffer;
    ofn.nMaxFile        = DWORD(std::size(buffer));
    ofn.lpstrTitle      = caption.c_str();
    ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetSaveFileNameW(&ofn)) return {};
    return fs::path(buffer);
}

fs::path pick_folder_dialog(const std::string& title, const fs::path& initial)
{
    wchar_t        buffer[MAX_PATH] = {0};
    const std::wstring caption = widen(title);

    BROWSEINFOW bi{};
    bi.lpszTitle      = caption.c_str();
    bi.ulFlags        = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.pszDisplayName = buffer;

    LPITEMIDLIST id = SHBrowseForFolderW(&bi);
    if (!id) return {};

    wchar_t path[MAX_PATH] = {0};
    const BOOL ok = SHGetPathFromIDListW(id, path);
    CoTaskMemFree(id);
    if (!ok) return {};
    (void)initial;
    return fs::path(path);
}

void reveal_in_file_manager(const fs::path& path)
{
    std::error_code ec;
    const fs::path target = fs::is_directory(path, ec) ? path : path.parent_path();
    ShellExecuteW(nullptr, L"open", target.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

#else   // no native dialogs

bool native_dialogs_available() { return false; }

fs::path open_file_dialog(const std::string&, const std::vector<FileFilter>&, const fs::path&)
{
    return {};
}

fs::path save_file_dialog(const std::string&, const std::vector<FileFilter>&,
                          const std::string&, const fs::path&)
{
    return {};
}

fs::path pick_folder_dialog(const std::string&, const fs::path&) { return {}; }

void reveal_in_file_manager(const fs::path& path)
{
    std::string cmd;
#if defined(RD_PLATFORM_MACOS)
    cmd = "open \"" + path.string() + "\"";
#else
    cmd = "xdg-open \"" + path.string() + "\"";
#endif
    if (std::system(cmd.c_str()) != 0) RD_WARN("cannot open %s", path.string().c_str());
}

#endif

} // namespace rd::ui
