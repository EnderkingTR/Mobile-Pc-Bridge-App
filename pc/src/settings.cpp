#include "settings.h"

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

static std::wstring ini_path() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    fs::path dir = fs::path(buf) / L"Kopru";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return (dir / L"kopru.ini").wstring();
}

static int get(const std::wstring& path, const wchar_t* key, int def) {
    return int(GetPrivateProfileIntW(L"kopru", key, def, path.c_str()));
}

static void put(const std::wstring& path, const wchar_t* key, int value) {
    WritePrivateProfileStringW(L"kopru", key, std::to_wstring(value).c_str(), path.c_str());
}

void Settings::load() {
    std::wstring p = ini_path();
    if (p.empty()) return;
    profile = get(p, L"profile", profile);
    if (profile < 0 || profile >= kProfileCount) profile = 1;
    topmost = get(p, L"topmost", 0) != 0;
    video_w = get(p, L"video_w", video_w);
    video_h = get(p, L"video_h", video_h);
    if (video_w <= 0 || video_h <= 0) { video_w = 1080; video_h = 1920; }
    window = {get(p, L"x", 0), get(p, L"y", 0), get(p, L"x", 0) + get(p, L"w", 0), get(p, L"y", 0) + get(p, L"h", 0)};
    has_window = window.right - window.left > 100 && window.bottom - window.top > 100;
}

void Settings::save() const {
    std::wstring p = ini_path();
    if (p.empty()) return;
    put(p, L"profile", profile);
    put(p, L"topmost", topmost ? 1 : 0);
    put(p, L"video_w", video_w);
    put(p, L"video_h", video_h);
    if (has_window) {
        put(p, L"x", window.left);
        put(p, L"y", window.top);
        put(p, L"w", window.right - window.left);
        put(p, L"h", window.bottom - window.top);
    }
}
