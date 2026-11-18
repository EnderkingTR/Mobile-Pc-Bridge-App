#pragma once
// PC -> telefon kontrol mesajları (scrcpy 5.x protokolü, app/src/control_msg.c ile birebir)
#include <cstdint>
#include <string>
#include <vector>

namespace ctrl {

enum Type : uint8_t {
    INJECT_KEYCODE = 0,
    INJECT_TEXT = 1,
    INJECT_TOUCH_EVENT = 2,
    INJECT_SCROLL_EVENT = 3,
    BACK_OR_SCREEN_ON = 4,
    EXPAND_NOTIFICATION_PANEL = 5,
    EXPAND_SETTINGS_PANEL = 6,
    COLLAPSE_PANELS = 7,
    GET_CLIPBOARD = 8,
    SET_CLIPBOARD = 9,
    SET_DISPLAY_POWER = 10,
    ROTATE_DEVICE = 11,
    SCAN_FILE = 22,  // telefona gönderilen dosyayı galeride/medyada görünür yap
};

// Android sabitleri
enum : uint8_t { ACTION_DOWN = 0, ACTION_UP = 1, ACTION_MOVE = 2 };
enum : uint8_t { COPY_KEY_NONE = 0, COPY_KEY_COPY = 1, COPY_KEY_CUT = 2 };
constexpr uint64_t POINTER_ID_GENERIC_FINGER = uint64_t(-2);
constexpr uint32_t META_SHIFT = 0x01 | 0x40;    // SHIFT_ON | SHIFT_LEFT_ON
constexpr uint32_t META_CTRL = 0x1000 | 0x2000; // CTRL_ON | CTRL_LEFT_ON

namespace key {
enum : uint32_t {
    HOME = 3, BACK = 4, DPAD_UP = 19, DPAD_DOWN = 20, DPAD_LEFT = 21, DPAD_RIGHT = 22,
    VOLUME_UP = 24, VOLUME_DOWN = 25, POWER = 26, A = 29, TAB = 61, ENTER = 66, DEL = 67,
    PAGE_UP = 92, PAGE_DOWN = 93, ESCAPE = 111, FORWARD_DEL = 112, MOVE_HOME = 122,
    MOVE_END = 123, APP_SWITCH = 187,
};
}

struct Position {
    int32_t x, y;
    uint16_t screen_w, screen_h;  // sunucu, bunun güncel video boyutuyla aynı olmasını bekler
};

std::vector<uint8_t> inject_keycode(uint8_t action, uint32_t keycode, uint32_t repeat, uint32_t metastate);
std::vector<uint8_t> inject_text(const std::string& utf8);
std::vector<uint8_t> inject_touch(uint8_t action, uint64_t pointer_id, Position pos, float pressure);
std::vector<uint8_t> inject_scroll(Position pos, float hscroll, float vscroll);
std::vector<uint8_t> back_or_screen_on(uint8_t action);
std::vector<uint8_t> simple(Type type);  // paneller, döndürme
std::vector<uint8_t> get_clipboard(uint8_t copy_key);
std::vector<uint8_t> set_clipboard(uint64_t sequence, const std::string& utf8, bool paste);
std::vector<uint8_t> set_display_power(bool on);
std::vector<uint8_t> scan_file(const std::string& device_path);

}  // namespace ctrl
