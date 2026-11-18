#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

inline uint32_t read32be(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline uint64_t read64be(const uint8_t* p) {
    return (uint64_t(read32be(p)) << 32) | read32be(p + 4);
}
inline uint16_t read16be(const uint8_t* p) {
    return uint16_t((p[0] << 8) | p[1]);
}

inline std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
inline std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

struct IRect { int x, y, w, h; };

// Videoyu en-boy oranını koruyarak pencereye ortalar (siyah kenarlar).
inline IRect letterbox(int cw, int ch, int vw, int vh) {
    if (cw <= 0 || ch <= 0 || vw <= 0 || vh <= 0) return {0, 0, cw, ch};
    double s = std::min(double(cw) / vw, double(ch) / vh);
    int w = int(std::lround(vw * s));
    int h = int(std::lround(vh * s));
    return {(cw - w) / 2, (ch - h) / 2, w, h};
}
