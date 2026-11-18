#include "control_msg.h"

#include <algorithm>

namespace ctrl {

namespace {

struct Writer {
    std::vector<uint8_t> b;
    explicit Writer(uint8_t type) { b.push_back(type); }
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
    void u32(uint32_t v) { u16(uint16_t(v >> 16)); u16(uint16_t(v)); }
    void u64(uint64_t v) { u32(uint32_t(v >> 32)); u32(uint32_t(v)); }
    void pos(Position p) { u32(uint32_t(p.x)); u32(uint32_t(p.y)); u16(p.screen_w); u16(p.screen_h); }
    void str(const std::string& s, size_t max_len) {
        size_t len = std::min(s.size(), max_len);
        // UTF-8 karakterini ortadan bölme
        while (len < s.size() && len > 0 && (uint8_t(s[len]) & 0xC0) == 0x80) --len;
        u32(uint32_t(len));
        b.insert(b.end(), s.begin(), s.begin() + ptrdiff_t(len));
    }
};

uint16_t float_to_u16fp(float f) {
    f = std::clamp(f, 0.0f, 1.0f);
    uint32_t u = uint32_t(f * 65536.0f);
    return uint16_t(std::min<uint32_t>(u, 0xffff));
}

int16_t float_to_i16fp(float f) {
    f = std::clamp(f, -1.0f, 1.0f);
    int32_t i = int32_t(f * 32768.0f);
    return int16_t(std::clamp<int32_t>(i, -0x8000, 0x7fff));
}

constexpr size_t kInjectTextMax = 300;
constexpr size_t kClipboardMax = (1 << 18) - 14;

}  // namespace

std::vector<uint8_t> inject_keycode(uint8_t action, uint32_t keycode, uint32_t repeat, uint32_t metastate) {
    Writer w(INJECT_KEYCODE);
    w.u8(action);
    w.u32(keycode);
    w.u32(repeat);
    w.u32(metastate);
    return std::move(w.b);
}

std::vector<uint8_t> inject_text(const std::string& utf8) {
    Writer w(INJECT_TEXT);
    w.str(utf8, kInjectTextMax);
    return std::move(w.b);
}

std::vector<uint8_t> inject_touch(uint8_t action, uint64_t pointer_id, Position pos, float pressure) {
    Writer w(INJECT_TOUCH_EVENT);
    w.u8(action);
    w.u64(pointer_id);
    w.pos(pos);
    w.u16(float_to_u16fp(pressure));
    w.u32(0);  // action button
    w.u32(0);  // buttons
    return std::move(w.b);
}

std::vector<uint8_t> inject_scroll(Position pos, float hscroll, float vscroll) {
    Writer w(INJECT_SCROLL_EVENT);
    w.pos(pos);
    // Protokol [-16, 16] aralığını [-1, 1]'e normalize eder
    w.u16(uint16_t(float_to_i16fp(hscroll / 16)));
    w.u16(uint16_t(float_to_i16fp(vscroll / 16)));
    w.u32(0);  // buttons
    return std::move(w.b);
}

std::vector<uint8_t> back_or_screen_on(uint8_t action) {
    Writer w(BACK_OR_SCREEN_ON);
    w.u8(action);
    return std::move(w.b);
}

std::vector<uint8_t> simple(Type type) {
    return {uint8_t(type)};
}

std::vector<uint8_t> get_clipboard(uint8_t copy_key) {
    Writer w(GET_CLIPBOARD);
    w.u8(copy_key);
    return std::move(w.b);
}

std::vector<uint8_t> set_clipboard(uint64_t sequence, const std::string& utf8, bool paste) {
    Writer w(SET_CLIPBOARD);
    w.u64(sequence);
    w.u8(paste ? 1 : 0);
    w.str(utf8, kClipboardMax);
    return std::move(w.b);
}

std::vector<uint8_t> set_display_power(bool on) {
    Writer w(SET_DISPLAY_POWER);
    w.u8(on ? 1 : 0);
    return std::move(w.b);
}

std::vector<uint8_t> scan_file(const std::string& device_path) {
    Writer w(SCAN_FILE);
    w.str(device_path, 256);
    return std::move(w.b);
}

}  // namespace ctrl
