// Köprü — Android ekranını PC'de düşük gecikmeyle gösterir ve kontrol eder.
//
// Thread yapısı:
//   ana thread      : Win32 pencere, fare/klavye -> kontrol mesajları
//   bağlantı thread : telefonu bekler, bağlanır, video paketlerini okuyup D3D11VA ile çözer,
//                     bağlantı koparsa baştan dener (otomatik yeniden bağlanma)
//   render thread   : kareyi çizer (eski kareler beklemeden atılır = düşük gecikme)
//   mesaj thread    : telefondan gelen pano (clipboard) mesajları (her oturumda yeniden)
//   aktarım thread'i: sürükle-bırak ile dosya gönderme / APK kurma
#include "session.h"

#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cwchar>
#include <thread>
#include <utility>

#include "adb.h"
#include "control_msg.h"
#include "decoder.h"
#include "log.h"
#include "renderer.h"
#include "settings.h"
#include "transfer.h"
#include "util.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

using namespace std::chrono_literals;

namespace {

constexpr UINT WM_APP_VIDEO_SIZE = WM_APP + 1;
constexpr UINT WM_APP_FPS = WM_APP + 2;
constexpr UINT WM_APP_DISCONNECTED = WM_APP + 3;
constexpr UINT WM_APP_CLIPBOARD = WM_APP + 4;
constexpr UINT WM_APP_CONNECTED = WM_APP + 5;
constexpr UINT WM_APP_STATUS = WM_APP + 6;   // lParam: new std::wstring, wParam: 1 = geçici
constexpr UINT WM_APP_WAITING = WM_APP + 7;  // lParam: new std::wstring (bekleme sebebi)
constexpr UINT_PTR kTimeLimitTimer = 1;
constexpr UINT_PTR kStatusTimer = 2;

// Başlık çubuğu menüsü (WM_SYSCOMMAND: alt 4 bit sisteme ayrılmış, kimlikler 16'nın katı)
constexpr UINT IDM_PROFILE_BASE = 0x0100;  // + i * 0x10
constexpr UINT IDM_TOPMOST = 0x0200;
constexpr UINT IDM_SCREEN = 0x0210;
constexpr UINT IDM_SEND_FILE = 0x0220;
constexpr UINT IDM_RECONNECT = 0x0230;

struct Options {
    SessionOptions session;
    bool custom_bit_rate = false;  // komut satırı profili ezer
    bool custom_max_size = false;
    int time_limit = 0;  // saniye, test için
};

HWND g_hwnd = nullptr;   // ana pencere (alttaki gezinme çubuğunu çizer, klavyeyi alır)
HWND g_video = nullptr;  // video alt penceresi (swap chain burada, dokunma burada)
Renderer g_renderer;
Session g_session;
Decoder g_decoder;
std::atomic<int> g_vw{0}, g_vh{0};  // telefonun güncel video boyutu
std::atomic<bool> g_quit{false};

struct RenderQueue {
    std::mutex m;
    std::condition_variable cv;
    AVFrame* frame = nullptr;  // sadece en yeni kare tutulur
    bool redraw = false, resize = false, quit = false;
    UINT w = 0, h = 0;
} g_rq;

struct Stats {
    std::atomic<uint64_t> packets{0}, bytes{0}, decoded{0}, dropped{0}, presented{0};
} g_stats;

// Giriş durumu (sadece ana thread)
bool g_touch_down = false;
int g_last_x = 0, g_last_y = 0;
bool g_topmost = false;
bool g_phone_screen_on = true;
wchar_t g_high_surrogate = 0;
int g_fps = 0;
int g_known_vw = 0, g_known_vh = 0;

// Gezinme çubuğu (fiziksel tuşlu telefonlar için): Samsung sırası
enum NavButton { NAV_RECENTS = 0, NAV_HOME = 1, NAV_BACK = 2, NAV_COUNT = 3 };
int g_bar_h = 0;          // piksel, DPI'a göre
int g_nav_hover = -1;
int g_nav_pressed = -1;

std::mutex g_clip_mutex;
std::string g_last_clip_sent;  // telefonun bize geri yankıladığı panoyu yok saymak için

Settings g_settings;
Options g_opt;

// Bağlantı yönetimi
std::mutex g_conn_mutex;
std::condition_variable g_conn_cv;
std::atomic<bool> g_reconnect{false};   // profil değişti: oturumu kapatıp hemen yeniden bağlan
std::atomic<bool> g_connected{false};

// Ana thread'in gösterdiği durum
std::wstring g_device_name;  // son bağlanılan cihaz
std::wstring g_serial;
std::wstring g_wait_reason;  // bağlı değilken başlıkta gösterilir
std::wstring g_status;       // geçici durum mesajı (dosya aktarımı vb.)

void post_text(UINT msg, std::wstring text, WPARAM wp = 0) {
    auto* s = new std::wstring(std::move(text));
    if (!PostMessageW(g_hwnd, msg, wp, reinterpret_cast<LPARAM>(s))) delete s;
}

// ---------------------------------------------------------------- render

void push_frame(AVFrame* f) {
    {
        std::lock_guard lk(g_rq.m);
        if (g_rq.frame) {
            av_frame_free(&g_rq.frame);
            ++g_stats.dropped;
        }
        g_rq.frame = f;
    }
    ++g_stats.decoded;
    g_rq.cv.notify_one();
}

void request_redraw() {
    { std::lock_guard lk(g_rq.m); g_rq.redraw = true; }
    g_rq.cv.notify_one();
}

void request_resize(UINT w, UINT h) {
    { std::lock_guard lk(g_rq.m); g_rq.resize = true; g_rq.w = w; g_rq.h = h; }
    g_rq.cv.notify_one();
}

void render_thread_fn() {
    auto last = std::chrono::steady_clock::now();
    uint64_t last_count = 0;
    for (;;) {
        AVFrame* f = nullptr;
        bool resize = false, redraw = false;
        UINT w = 0, h = 0;
        {
            std::unique_lock lk(g_rq.m);
            g_rq.cv.wait_for(lk, 500ms, [] { return g_rq.quit || g_rq.frame || g_rq.redraw || g_rq.resize; });
            if (g_rq.quit) break;
            f = std::exchange(g_rq.frame, nullptr);
            resize = std::exchange(g_rq.resize, false);
            redraw = std::exchange(g_rq.redraw, false);
            w = g_rq.w;
            h = g_rq.h;
        }
        if (resize) g_renderer.resize(w, h);
        bool had_frame = f != nullptr;
        if (f) {
            g_renderer.upload(f);
            av_frame_free(&f);
            ++g_stats.presented;
        }
        if (had_frame || resize || redraw) g_renderer.draw();

        auto now = std::chrono::steady_clock::now();
        if (now - last >= 1s) {
            uint64_t c = g_stats.presented;
            PostMessageW(g_hwnd, WM_APP_FPS, WPARAM(c - last_count), 0);
            last_count = c;
            last = now;
        }
    }
}

// ---------------------------------------------------------------- video

AVCodecID codec_from_id(uint32_t id) {
    switch (id) {
        case 0x68323634: return AV_CODEC_ID_H264;
        case 0x68323635: return AV_CODEC_ID_HEVC;
        case 0x00617631: return AV_CODEC_ID_AV1;
        default: return AV_CODEC_ID_NONE;
    }
}

// Bir oturum boyunca video paketlerini okur ve çözer; bağlantı kopunca döner.
void video_loop() {
    constexpr uint64_t kFlagConfig = uint64_t(1) << 62;
    constexpr uint64_t kFlagKey = uint64_t(1) << 61;
    std::vector<uint8_t> config;
    uint8_t hdr[12];

    while (!g_quit && !g_reconnect) {
        if (!g_session.recv_video(hdr, sizeof hdr)) break;

        if (hdr[0] & 0x80) {
            // Oturum paketi: yeni video boyutu (ör. telefon döndürüldü)
            int w = int(read32be(hdr + 4)), h = int(read32be(hdr + 8));
            g_vw = w;
            g_vh = h;
            PostMessageW(g_hwnd, WM_APP_VIDEO_SIZE, WPARAM(w), LPARAM(h));
            continue;
        }

        uint64_t pts_flags = read64be(hdr);
        uint32_t len = read32be(hdr + 8);
        if (len == 0 || len > (64u << 20)) {
            LOGE("Geçersiz paket boyutu: %u", len);
            break;
        }
        ++g_stats.packets;
        g_stats.bytes += len;

        if (pts_flags & kFlagConfig) {
            // SPS/PPS: bir sonraki kareyle birleştirip çözücüye ver
            config.resize(len);
            if (!g_session.recv_video(config.data(), len)) break;
            continue;
        }

        AVPacket* pkt = av_packet_alloc();
        if (av_new_packet(pkt, int(config.size() + len)) < 0) {
            av_packet_free(&pkt);
            break;
        }
        if (!config.empty()) std::memcpy(pkt->data, config.data(), config.size());
        bool ok = g_session.recv_video(pkt->data + config.size(), len);
        config.clear();
        if (!ok) {
            av_packet_free(&pkt);
            break;
        }
        if (pts_flags & kFlagKey) pkt->flags |= AV_PKT_FLAG_KEY;
        pkt->pts = int64_t(pts_flags & (kFlagKey - 1));

        g_decoder.decode(pkt, push_frame);
        av_packet_free(&pkt);
    }
}

// ---------------------------------------------------------------- pano

void set_pc_clipboard(const std::string& utf8) {
    std::wstring w = to_wide(utf8);
    for (int i = 0; i < 5; ++i) {
        if (OpenClipboard(g_hwnd)) {
            EmptyClipboard();
            HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
            if (mem) {
                std::memcpy(GlobalLock(mem), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
                GlobalUnlock(mem);
                if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
            }
            CloseClipboard();
            return;
        }
        Sleep(10);
    }
}

std::string get_pc_clipboard() {
    std::string out;
    if (!OpenClipboard(g_hwnd)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            out = to_utf8(p);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

void send_phone_clipboard(const std::string& text, bool paste) {
    { std::lock_guard lk(g_clip_mutex); g_last_clip_sent = text; }
    g_session.send_control(ctrl::set_clipboard(0, text, paste));
}

void device_msg_thread_fn() {
    for (;;) {
        uint8_t type;
        if (!g_session.recv_control(&type, 1)) break;
        if (type == 0) {  // pano içeriği
            uint8_t lb[4];
            if (!g_session.recv_control(lb, 4)) break;
            uint32_t len = read32be(lb);
            if (len > (1u << 20)) break;
            std::string text(len, '\0');
            if (len && !g_session.recv_control(text.data(), len)) break;
            {
                std::lock_guard lk(g_clip_mutex);
                if (text == g_last_clip_sent) continue;
            }
            auto* s = new std::string(std::move(text));
            if (!PostMessageW(g_hwnd, WM_APP_CLIPBOARD, 0, reinterpret_cast<LPARAM>(s))) delete s;
        } else if (type == 1) {  // pano onayı
            uint8_t b[8];
            if (!g_session.recv_control(b, 8)) break;
        } else if (type == 2) {  // UHID çıktısı (kullanmıyoruz)
            uint8_t b[4];
            if (!g_session.recv_control(b, 4)) break;
            std::vector<uint8_t> d(read16be(b + 2));
            if (!d.empty() && !g_session.recv_control(d.data(), d.size())) break;
        } else {
            LOGW("Bilinmeyen cihaz mesajı: %u", type);
            break;
        }
    }
}

// ---------------------------------------------------------------- bağlantı yönetimi

SessionOptions current_session_options() {
    std::lock_guard lk(g_conn_mutex);
    SessionOptions o = g_opt.session;
    const Profile& p = kProfiles[g_settings.profile];
    if (!g_opt.custom_bit_rate) o.bit_rate = p.bit_rate;
    if (!g_opt.custom_max_size) o.max_size = p.max_size;
    return o;
}

// `ms` kadar bekler; kapanış ya da yeniden bağlanma isteği gelirse hemen döner
void wait_or_wake(std::chrono::milliseconds ms) {
    std::unique_lock lk(g_conn_mutex);
    g_conn_cv.wait_for(lk, ms, [] { return g_quit.load() || g_reconnect.load(); });
}

// Telefonu bekler, bağlanır, oturum bitince tekrar dener. Pencere hiç kapanmaz.
void connection_thread_fn() {
    std::string last_error;
    while (!g_quit) {
        g_reconnect = false;
        std::string err;
        Session::Result r = g_session.start(current_session_options(), g_quit, &err);
        if (g_quit) { g_session.stop(); break; }

        if (r != Session::Result::Ok) {
            if (err != last_error) {
                LOGW("%s", err.c_str());
                last_error = err;
                std::wstring reason = r == Session::Result::NoDevice
                                          ? L"Telefon bekleniyor (USB kablosu ve USB hata ayıklama)"
                                          : L"Bağlanamadı, tekrar deneniyor";
                if (err.find("yetkisiz") != std::string::npos) reason = L"Telefonda USB hata ayıklama iznini onayla";
                post_text(WM_APP_WAITING, reason);
            }
            wait_or_wake(1500ms);
            continue;
        }
        last_error.clear();

        // Akışın başı: codec id (4 bayt) + ilk oturum paketi (12 bayt)
        uint8_t buf[12];
        AVCodecID codec = AV_CODEC_ID_NONE;
        bool header_ok = g_session.recv_video(buf, 4);
        if (header_ok) {
            uint32_t id = read32be(buf);
            codec = codec_from_id(id);
            if (codec == AV_CODEC_ID_NONE) LOGE("Telefon video akışını başlatamadı (kod 0x%08x)", id);
        }
        header_ok = header_ok && codec != AV_CODEC_ID_NONE && g_session.recv_video(buf, 12) && (buf[0] & 0x80);
        if (!header_ok || !g_decoder.open(codec, g_renderer.device(), &g_renderer.context_lock())) {
            g_session.stop();
            g_decoder.close();
            post_text(WM_APP_WAITING, L"Video başlatılamadı, tekrar deneniyor");
            wait_or_wake(3000ms);
            continue;
        }
        int w = int(read32be(buf + 4)), h = int(read32be(buf + 8));
        g_vw = w;
        g_vh = h;
        g_connected = true;
        g_renderer.set_dimmed(false);
        auto* info = new std::wstring(to_wide(g_session.device_name()) + L"\n" + g_session.serial());
        if (!PostMessageW(g_hwnd, WM_APP_CONNECTED, MAKEWPARAM(w, h), reinterpret_cast<LPARAM>(info))) delete info;

        std::thread msg_thread(device_msg_thread_fn);
        video_loop();

        g_connected = false;
        g_session.interrupt();  // mesaj thread'ini uyandır
        msg_thread.join();
        g_session.stop();
        g_decoder.close();
        g_renderer.set_dimmed(true);
        request_redraw();
        if (g_quit) break;

        bool requested = g_reconnect.load();
        LOGW("%s", requested ? "Yeni ayarlarla yeniden bağlanılıyor" : "Telefonla bağlantı koptu, yeniden bağlanılacak");
        post_text(WM_APP_WAITING, requested ? L"Yeniden bağlanıyor…" : L"Bağlantı koptu, yeniden bağlanıyor…");
        if (!requested) wait_or_wake(1000ms);
    }
}

void request_reconnect() {
    {
        std::lock_guard lk(g_conn_mutex);
        g_reconnect = true;
    }
    g_conn_cv.notify_all();
    g_session.interrupt();
}

// ---------------------------------------------------------------- giriş

bool map_point(int x, int y, bool clamp, ctrl::Position& out) {
    int vw = g_vw, vh = g_vh;
    if (!vw || !vh) return false;
    RECT rc;
    GetClientRect(g_video, &rc);
    IRect r = letterbox(rc.right, rc.bottom, vw, vh);
    if (r.w <= 0 || r.h <= 0) return false;
    double fx = double(x - r.x) * vw / r.w;
    double fy = double(y - r.y) * vh / r.h;
    if (!clamp && (fx < 0 || fy < 0 || fx >= vw || fy >= vh)) return false;
    out.x = std::clamp(int(fx), 0, vw - 1);
    out.y = std::clamp(int(fy), 0, vh - 1);
    out.screen_w = uint16_t(vw);
    out.screen_h = uint16_t(vh);
    return true;
}

bool send_touch(uint8_t action, int x, int y) {
    ctrl::Position p;
    if (!map_point(x, y, action != ctrl::ACTION_DOWN, p)) return false;
    float pressure = action == ctrl::ACTION_UP ? 0.0f : 1.0f;
    g_session.send_control(ctrl::inject_touch(action, ctrl::POINTER_ID_GENERIC_FINGER, p, pressure));
    g_last_x = x;
    g_last_y = y;
    return true;
}

void send_key_press(uint32_t keycode, uint32_t meta = 0) {
    g_session.send_control(ctrl::inject_keycode(ctrl::ACTION_DOWN, keycode, 0, meta));
    g_session.send_control(ctrl::inject_keycode(ctrl::ACTION_UP, keycode, 0, meta));
}

uint32_t map_special_key(WPARAM vk) {
    using namespace ctrl::key;
    switch (vk) {
        case VK_RETURN: return ENTER;
        case VK_BACK: return DEL;
        case VK_DELETE: return FORWARD_DEL;
        case VK_TAB: return TAB;
        case VK_ESCAPE: return ESCAPE;
        case VK_UP: return DPAD_UP;
        case VK_DOWN: return DPAD_DOWN;
        case VK_LEFT: return DPAD_LEFT;
        case VK_RIGHT: return DPAD_RIGHT;
        case VK_HOME: return MOVE_HOME;
        case VK_END: return MOVE_END;
        case VK_PRIOR: return PAGE_UP;
        case VK_NEXT: return PAGE_DOWN;
        default: return 0;
    }
}

bool ctrl_only_down() {
    // AltGr = Ctrl+Alt; AltGr ile yazılan karakterler (@, € ...) kısayol sayılmasın
    return GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0;
}

bool on_key(WPARAM vk, LPARAM lp, bool down) {
    bool ctrl = ctrl_only_down();
    bool shift = GetKeyState(VK_SHIFT) < 0;
    if (ctrl && (vk == 'V' || vk == 'C' || vk == 'X')) {
        if (down && !(lp & (1 << 30))) {
            if (vk == 'V') send_phone_clipboard(get_pc_clipboard(), true);
            else g_session.send_control(ctrl::get_clipboard(vk == 'C' ? ctrl::COPY_KEY_COPY : ctrl::COPY_KEY_CUT));
        }
        return true;
    }
    uint32_t kc = map_special_key(vk);
    if (!kc && ctrl && vk >= 'A' && vk <= 'Z') kc = ctrl::key::A + uint32_t(vk - 'A');
    if (!kc) return false;
    uint32_t meta = (shift ? ctrl::META_SHIFT : 0) | (ctrl ? ctrl::META_CTRL : 0);
    uint32_t repeat = (down && (lp & (1 << 30))) ? 1 : 0;
    g_session.send_control(ctrl::inject_keycode(down ? ctrl::ACTION_DOWN : ctrl::ACTION_UP, kc, repeat, meta));
    return true;
}

void on_char(wchar_t ch) {
    std::wstring w;
    if (ch >= 0xD800 && ch <= 0xDBFF) { g_high_surrogate = ch; return; }
    if (ch >= 0xDC00 && ch <= 0xDFFF) {
        if (!g_high_surrogate) return;
        w = {g_high_surrogate, ch};
        g_high_surrogate = 0;
    } else {
        if (ch < 0x20 || ch == 0x7f) return;  // Enter/Tab/Backspace tuş olarak gidiyor
        w = {ch};
    }
    std::string u = to_utf8(w);
    bool ascii = std::all_of(u.begin(), u.end(), [](char c) { return uint8_t(c) < 0x80; });
    if (ascii) {
        g_session.send_control(ctrl::inject_text(u));
    } else {
        // Android'in sanal klavye haritası ç, ğ, ı, ö, ş, ü gibi karakterleri enjekte
        // edemiyor; bunları telefonun panosu üzerinden yapıştırıyoruz.
        send_phone_clipboard(u, true);
    }
}

void update_title() {
    std::wstring t = L"Köprü";
    if (g_connected) {
        t += L" — " + g_device_name;
        if (g_known_vw) t += L"  |  " + std::to_wstring(g_known_vw) + L"×" + std::to_wstring(g_known_vh);
        t += L"  |  " + std::to_wstring(g_fps) + L" FPS";
        t += L"  |  ";
        t += (g_opt.custom_bit_rate || g_opt.custom_max_size) ? L"Özel" : kProfiles[g_settings.profile].name;
        if (!g_phone_screen_on) t += L"  |  Telefon ekranı kapalı";
    } else {
        t += L" — " + (g_wait_reason.empty() ? std::wstring(L"Bağlanıyor…") : g_wait_reason);
    }
    if (g_topmost) t += L"  |  Üstte";
    if (!g_status.empty()) t += L"  |  " + g_status;
    SetWindowTextW(g_hwnd, t.c_str());
}

void show_status(const std::wstring& text, bool temporary) {
    g_status = text;
    update_title();
    if (temporary) SetTimer(g_hwnd, kStatusTimer, 4000, nullptr);
    else KillTimer(g_hwnd, kStatusTimer);
}

void update_menu() {
    HMENU m = GetSystemMenu(g_hwnd, FALSE);
    CheckMenuRadioItem(m, IDM_PROFILE_BASE, IDM_PROFILE_BASE + (kProfileCount - 1) * 0x10,
                       IDM_PROFILE_BASE + UINT(g_settings.profile) * 0x10, MF_BYCOMMAND);
    CheckMenuItem(m, IDM_TOPMOST, MF_BYCOMMAND | (g_topmost ? MF_CHECKED : MF_UNCHECKED));
}

void build_menu() {
    HMENU m = GetSystemMenu(g_hwnd, FALSE);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (int i = 0; i < kProfileCount; ++i) {
        std::wstring label = L"Profil: " + std::wstring(kProfiles[i].name) + L"\tAlt+" + std::to_wstring(i + 1);
        AppendMenuW(m, MF_STRING, IDM_PROFILE_BASE + UINT(i) * 0x10, label.c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_TOPMOST, L"Her zaman üstte\tAlt+T");
    AppendMenuW(m, MF_STRING, IDM_SCREEN, L"Telefon ekranını kapat / aç\tAlt+O");
    AppendMenuW(m, MF_STRING, IDM_SEND_FILE, L"Dosya gönder / APK kur…\tAlt+F");
    AppendMenuW(m, MF_STRING, IDM_RECONNECT, L"Yeniden bağlan");
    update_menu();
}

void set_profile(int i) {
    if (i < 0 || i >= kProfileCount) return;
    bool custom = g_opt.custom_bit_rate || g_opt.custom_max_size;
    if (i == g_settings.profile && !custom) return;
    {
        std::lock_guard lk(g_conn_mutex);
        g_settings.profile = i;
        g_opt.custom_bit_rate = g_opt.custom_max_size = false;  // menüden seçim komut satırını ezer
    }
    g_settings.save();
    update_menu();
    show_status(L"Profil: " + std::wstring(kProfiles[i].name), true);
    if (g_connected) request_reconnect();
}

void set_topmost(bool on) {
    g_topmost = on;
    g_settings.topmost = on;
    g_settings.save();
    SetWindowPos(g_hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    update_menu();
    update_title();
}

void toggle_phone_screen() {
    if (!g_connected) return;
    g_phone_screen_on = !g_phone_screen_on;
    g_session.send_control(ctrl::set_display_power(g_phone_screen_on));
    update_title();
}

// ---------------------------------------------------------------- dosya aktarımı

void send_files(std::vector<std::wstring> files) {
    if (files.empty()) return;
    if (!g_connected || g_serial.empty()) {
        show_status(L"Dosya göndermek için telefon bağlı olmalı", true);
        return;
    }
    transfer::start(
        g_serial, std::move(files),
        [](const std::wstring& text, bool done) { post_text(WM_APP_STATUS, text, done ? 1 : 0); },
        [](const std::string& path) { g_session.send_control(ctrl::scan_file(path)); });
}

void open_file_dialog() {
    std::wstring buf(32768, L'\0');
    OPENFILENAMEW ofn{sizeof ofn};
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"Tüm dosyalar\0*.*\0Android uygulaması (*.apk)\0*.apk\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = DWORD(buf.size());
    ofn.lpstrTitle = L"Telefona gönderilecek dosyaları seç (APK'lar kurulur)";
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;

    // Çoklu seçimde: klasör\0dosya1\0dosya2\0\0 ; tekli seçimde: tam yol\0\0
    std::vector<std::wstring> parts;
    for (const wchar_t* p = buf.c_str(); *p; p += wcslen(p) + 1) parts.emplace_back(p);
    std::vector<std::wstring> files;
    if (parts.size() == 1) {
        files = parts;
    } else {
        for (size_t i = 1; i < parts.size(); ++i) files.push_back(parts[0] + L"\\" + parts[i]);
    }
    send_files(std::move(files));
}

void on_drop(HDROP drop) {
    std::vector<std::wstring> files;
    UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; ++i) {
        UINT len = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring f(len + 1, L'\0');
        DragQueryFileW(drop, i, f.data(), len + 1);
        f.resize(len);
        files.push_back(std::move(f));
    }
    DragFinish(drop);
    send_files(std::move(files));
}

// Pencereyi video oranında, ekranın %90'ına sığacak şekilde boyutlandır
void fit_window(int vw, int vh) {
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wa = mi.rcWork;
    int ww = wa.right - wa.left, wh = wa.bottom - wa.top;
    double s = std::min({ww * 0.9 / vw, (wh * 0.9 - g_bar_h) / vh, 1.0});
    RECT r{0, 0, int(std::lround(vw * s)), int(std::lround(vh * s)) + g_bar_h};
    DWORD style = DWORD(GetWindowLongW(g_hwnd, GWL_STYLE));
    DWORD ex = DWORD(GetWindowLongW(g_hwnd, GWL_EXSTYLE));
    AdjustWindowRectExForDpi(&r, style, FALSE, ex, GetDpiForWindow(g_hwnd));
    int w = r.right - r.left, h = r.bottom - r.top;

    int cx = wa.left + ww / 2, cy = wa.top + wh / 2;
    if (IsWindowVisible(g_hwnd)) {
        RECT cur;
        GetWindowRect(g_hwnd, &cur);
        cx = (cur.left + cur.right) / 2;
        cy = (cur.top + cur.bottom) / 2;
    }
    int x = std::max<int>(wa.left, std::min<int>(cx - w / 2, wa.right - w));
    int y = std::max<int>(wa.top, std::min<int>(cy - h / 2, wa.bottom - h));
    SetWindowPos(g_hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// Kenardan sürüklerken en-boy oranını koru
void keep_aspect(WPARAM edge, RECT* r) {
    int vw = g_known_vw, vh = g_known_vh;
    if (!vw || !vh) return;
    RECT f{0, 0, 0, 0};
    AdjustWindowRectExForDpi(&f, DWORD(GetWindowLongW(g_hwnd, GWL_STYLE)), FALSE,
                             DWORD(GetWindowLongW(g_hwnd, GWL_EXSTYLE)), GetDpiForWindow(g_hwnd));
    int bw = f.right - f.left, bh = f.bottom - f.top + g_bar_h;  // çubuk da sabit yükseklik
    int cw = r->right - r->left - bw, ch = r->bottom - r->top - bh;
    double ar = double(vw) / vh;
    switch (edge) {
        case WMSZ_TOP:
        case WMSZ_BOTTOM:
            r->right = r->left + int(std::lround(ch * ar)) + bw;
            break;
        case WMSZ_TOPLEFT:
        case WMSZ_TOPRIGHT:
            r->top = r->bottom - int(std::lround(cw / ar)) - bh;
            break;
        default:  // sol, sağ, alt köşeler
            r->bottom = r->top + int(std::lround(cw / ar)) + bh;
            break;
    }
}

bool on_alt_shortcut(WPARAM vk, LPARAM lp) {
    using namespace ctrl::key;
    bool repeat = lp & (1 << 30);
    bool shift = GetKeyState(VK_SHIFT) < 0;
    switch (vk) {
        case 'H': if (!repeat) send_key_press(HOME); return true;
        case 'B':
            if (!repeat) {
                g_session.send_control(ctrl::back_or_screen_on(ctrl::ACTION_DOWN));
                g_session.send_control(ctrl::back_or_screen_on(ctrl::ACTION_UP));
            }
            return true;
        case 'S': if (!repeat) send_key_press(APP_SWITCH); return true;
        case 'P': if (!repeat) send_key_press(POWER); return true;
        case VK_UP: send_key_press(VOLUME_UP); return true;
        case VK_DOWN: send_key_press(VOLUME_DOWN); return true;
        case 'N':
            if (!repeat) g_session.send_control(ctrl::simple(shift ? ctrl::COLLAPSE_PANELS : ctrl::EXPAND_NOTIFICATION_PANEL));
            return true;
        case 'R': if (!repeat) g_session.send_control(ctrl::simple(ctrl::ROTATE_DEVICE)); return true;
        case 'O': if (!repeat) toggle_phone_screen(); return true;
        case 'T': if (!repeat) set_topmost(!g_topmost); return true;
        case 'F': if (!repeat) open_file_dialog(); return true;
        case '1': case '2': case '3':
            if (!repeat) set_profile(int(vk - '1'));
            return true;
        default: return false;
    }
}

// ---------------------------------------------------------------- gezinme çubuğu

void update_bar_height() {
    g_bar_h = MulDiv(44, int(GetDpiForWindow(g_hwnd)), 96);
}

RECT nav_bar_rect() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    rc.top = std::max<LONG>(0, rc.bottom - g_bar_h);
    return rc;
}

int nav_hit(int x, int y) {
    RECT bar = nav_bar_rect();
    if (y < bar.top || y >= bar.bottom || x < 0 || x >= bar.right || bar.right <= 0) return -1;
    return std::clamp(int(x * NAV_COUNT / bar.right), 0, NAV_COUNT - 1);
}

void nav_send(int button, uint8_t action) {
    if (button == NAV_BACK) {
        g_session.send_control(ctrl::back_or_screen_on(action));
    } else {
        uint32_t kc = button == NAV_HOME ? ctrl::key::HOME : ctrl::key::APP_SWITCH;
        g_session.send_control(ctrl::inject_keycode(action, kc, 0, 0));
    }
}

void invalidate_nav_bar() {
    RECT bar = nav_bar_rect();
    InvalidateRect(g_hwnd, &bar, FALSE);
}

void paint_nav_bar(HDC hdc) {
    RECT bar = nav_bar_rect();
    int w = bar.right - bar.left, h = bar.bottom - bar.top;
    if (w <= 0 || h <= 0) return;

    // Titremesin diye önce bellekte çiz
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ old_bmp = SelectObject(mem, bmp);

    HBRUSH bg = CreateSolidBrush(RGB(20, 20, 22));
    RECT all{0, 0, w, h};
    FillRect(mem, &all, bg);
    DeleteObject(bg);

    HPEN pen = CreatePen(PS_SOLID, std::max(1, h / 22), RGB(225, 225, 228));
    HGDIOBJ old_pen = SelectObject(mem, pen);
    HGDIOBJ old_brush = SelectObject(mem, GetStockObject(NULL_BRUSH));

    int r = std::max(4, h * 18 / 100);  // simge yarıçapı
    for (int i = 0; i < NAV_COUNT; ++i) {
        RECT b{i * w / NAV_COUNT, 0, (i + 1) * w / NAV_COUNT, h};
        if (i == g_nav_pressed || i == g_nav_hover) {
            HBRUSH hb = CreateSolidBrush(i == g_nav_pressed ? RGB(70, 70, 76) : RGB(42, 42, 46));
            FillRect(mem, &b, hb);
            DeleteObject(hb);
        }
        int cx = (b.left + b.right) / 2, cy = h / 2;
        switch (i) {
            case NAV_RECENTS:
                RoundRect(mem, cx - r, cy - r, cx + r, cy + r, r / 2, r / 2);
                break;
            case NAV_HOME:
                Ellipse(mem, cx - r, cy - r, cx + r, cy + r);
                break;
            case NAV_BACK: {
                POINT tri[3] = {{cx - r, cy}, {cx + r * 4 / 5, cy - r}, {cx + r * 4 / 5, cy + r}};
                Polygon(mem, tri, 3);
                break;
            }
        }
    }

    SelectObject(mem, old_brush);
    SelectObject(mem, old_pen);
    DeleteObject(pen);
    BitBlt(hdc, bar.left, bar.top, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ---------------------------------------------------------------- pencereler

// Video alt penceresi: dokunma ve fare tuşları
LRESULT CALLBACK video_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            request_redraw();
            return 0;
        case WM_ERASEBKGND:
            return 1;

        case WM_LBUTTONDOWN:
            if (send_touch(ctrl::ACTION_DOWN, GET_X_LPARAM(lp), GET_Y_LPARAM(lp))) {
                g_touch_down = true;
                SetCapture(hwnd);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (g_touch_down) send_touch(ctrl::ACTION_MOVE, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_LBUTTONUP:
            if (g_touch_down) {
                g_touch_down = false;
                send_touch(ctrl::ACTION_UP, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                ReleaseCapture();
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (g_touch_down) {
                g_touch_down = false;
                send_touch(ctrl::ACTION_UP, g_last_x, g_last_y);
            }
            return 0;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            g_session.send_control(ctrl::back_or_screen_on(msg == WM_RBUTTONDOWN ? ctrl::ACTION_DOWN : ctrl::ACTION_UP));
            return 0;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            g_session.send_control(ctrl::inject_keycode(msg == WM_MBUTTONDOWN ? ctrl::ACTION_DOWN : ctrl::ACTION_UP,
                                                        ctrl::key::HOME, 0, 0));
            return 0;
        case WM_DROPFILES:
            return SendMessageW(g_hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Ana pencere: boyut, klavye, gezinme çubuğu, uygulama mesajları
LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED && g_video) {
                int w = LOWORD(lp), h = HIWORD(lp);
                int vh = std::max(1, h - g_bar_h);
                MoveWindow(g_video, 0, 0, w, vh, FALSE);
                request_resize(UINT(w), UINT(vh));
                invalidate_nav_bar();
            }
            return 0;
        case WM_SIZING:
            keep_aspect(wp, reinterpret_cast<RECT*>(lp));
            return TRUE;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {160, 160};
            return 0;
        case WM_DPICHANGED: {
            update_bar_height();
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            paint_nav_bar(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;

        // Gezinme çubuğu: basılı tutma telefona da basılı tutma olarak gider
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            int b = nav_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (b >= 0) {
                g_nav_pressed = b;
                SetCapture(hwnd);
                nav_send(b, ctrl::ACTION_DOWN);
                invalidate_nav_bar();
            }
            return 0;
        }
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (g_nav_pressed >= 0) {
                nav_send(g_nav_pressed, ctrl::ACTION_UP);
                g_nav_pressed = -1;
                if (msg == WM_LBUTTONUP) ReleaseCapture();
                invalidate_nav_bar();
            }
            return 0;
        case WM_MOUSEMOVE: {
            int b = nav_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (b != g_nav_hover) {
                g_nav_hover = b;
                invalidate_nav_bar();
                TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (g_nav_hover >= 0) {
                g_nav_hover = -1;
                invalidate_nav_bar();
            }
            return 0;

        // Tekerlek mesajları odaktaki pencereye (ana pencere) gelir; konumu video penceresine çevir
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(g_video, &pt);
            ctrl::Position p;
            if (map_point(pt.x, pt.y, false, p)) {
                float d = float(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
                g_session.send_control(msg == WM_MOUSEWHEEL ? ctrl::inject_scroll(p, 0, d) : ctrl::inject_scroll(p, d, 0));
            }
            return 0;
        }

        case WM_KEYDOWN:
        case WM_KEYUP:
            if (on_key(wp, lp, msg == WM_KEYDOWN)) return 0;
            break;
        case WM_CHAR:
            on_char(wchar_t(wp));
            return 0;
        case WM_SYSKEYDOWN:
            if ((lp & (1 << 29)) && on_alt_shortcut(wp, lp)) return 0;
            break;
        case WM_SYSCHAR:
            if (wp != L' ') return 0;  // Alt+harf bip sesini engelle; Alt+Boşluk sistem menüsü kalsın
            break;

        case WM_APP_VIDEO_SIZE: {
            int w = int(wp), h = int(lp);
            bool rotated = g_known_vw && ((w > h) != (g_known_vw > g_known_vh));
            g_known_vw = w;
            g_known_vh = h;
            if (rotated) fit_window(w, h);
            update_title();
            return 0;
        }
        case WM_APP_CONNECTED: {
            auto* info = reinterpret_cast<std::wstring*>(lp);
            size_t nl = info->find(L'\n');
            g_device_name = info->substr(0, nl);
            g_serial = nl == std::wstring::npos ? L"" : info->substr(nl + 1);
            delete info;
            int w = LOWORD(wp), h = HIWORD(wp);
            // Pencerenin oranı videoya uymuyorsa (ilk bağlantı, farklı profil/telefon) yeniden boyutla
            RECT rc;
            GetClientRect(g_video, &rc);
            double win_ar = rc.bottom > 0 ? double(rc.right) / rc.bottom : 0;
            double vid_ar = double(w) / h;
            g_known_vw = w;
            g_known_vh = h;
            if (std::abs(win_ar - vid_ar) > 0.02) fit_window(w, h);
            g_settings.video_w = w;
            g_settings.video_h = h;
            g_phone_screen_on = true;
            g_wait_reason.clear();
            update_title();
            return 0;
        }
        case WM_APP_WAITING: {
            auto* s = reinterpret_cast<std::wstring*>(lp);
            g_wait_reason = *s;
            delete s;
            g_fps = 0;
            update_title();
            return 0;
        }
        case WM_APP_STATUS: {
            auto* s = reinterpret_cast<std::wstring*>(lp);
            show_status(*s, wp == 1);
            delete s;
            return 0;
        }
        case WM_SYSCOMMAND: {
            UINT id = UINT(wp & 0xFFF0);
            if (id >= IDM_PROFILE_BASE && id < IDM_PROFILE_BASE + UINT(kProfileCount) * 0x10) {
                set_profile(int((id - IDM_PROFILE_BASE) / 0x10));
                return 0;
            }
            switch (id) {
                case IDM_TOPMOST: set_topmost(!g_topmost); return 0;
                case IDM_SCREEN: toggle_phone_screen(); return 0;
                case IDM_SEND_FILE: open_file_dialog(); return 0;
                case IDM_RECONNECT: request_reconnect(); return 0;
            }
            break;
        }
        case WM_DROPFILES:
            on_drop(reinterpret_cast<HDROP>(wp));
            return 0;
        case WM_APP_FPS:
            g_fps = int(wp);
            update_title();
            return 0;
        case WM_APP_CLIPBOARD: {
            auto* s = reinterpret_cast<std::string*>(lp);
            set_pc_clipboard(*s);
            delete s;
            return 0;
        }
        case WM_TIMER:
            if (wp == kTimeLimitTimer) DestroyWindow(hwnd);
            if (wp == kStatusTimer) {
                KillTimer(hwnd, kStatusTimer);
                g_status.clear();
                update_title();
            }
            return 0;
        case WM_DESTROY: {
            WINDOWPLACEMENT wpl{sizeof wpl};
            if (GetWindowPlacement(hwnd, &wpl)) {
                g_settings.window = wpl.rcNormalPosition;
                g_settings.has_window = true;
            }
            g_settings.save();
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void print_help() {
    std::fputs(
        "Kullanim: kopru [secenekler]\n"
        "  --serial SERI      Birden fazla cihaz varsa hangisi\n"
        "  --bit-rate MBPS    Video bit hizi (profili ezer)\n"
        "  --max-fps N        En fazla FPS (varsayilan 60)\n"
        "  --max-size N       En uzun kenari N pikselle sinirla (profili ezer)\n"
        "  --stay-awake       USB takiliyken telefon ekrani kapanmasin\n"
        "  --time-limit SN    N saniye sonra kapan (test icin)\n"
        "\nProfiller: Alt+1 Hiz (1280p, 4 Mbps), Alt+2 Dengeli (8 Mbps), Alt+3 Kalite (16 Mbps)\n"
        "Kisayollar: sag tik=Geri, orta tik=Ana ekran, Alt+H ana ekran, Alt+B geri,\n"
        "  Alt+S son uygulamalar, Alt+P guc, Alt+Yukari/Asagi ses, Alt+N bildirimler,\n"
        "  Alt+R dondur, Alt+O telefon ekranini kapat/ac, Alt+T her zaman ustte,\n"
        "  Alt+F dosya gonder / APK kur (ya da pencereye surukle-birak),\n"
        "  Ctrl+V PC panosunu telefona yapistir, Ctrl+C/X telefondan PC'ye kopyala\n",
        stderr);
}

bool parse_args(int argc, wchar_t** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() -> const wchar_t* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == L"--help" || a == L"-h") { print_help(); return false; }
        else if (a == L"--stay-awake") o.session.stay_awake = true;
        else if (a == L"--serial") { if (auto v = next()) o.session.serial = v; }
        else if (a == L"--bit-rate") {
            if (auto v = next()) { o.session.bit_rate = uint32_t(_wtof(v) * 1'000'000); o.custom_bit_rate = true; }
        }
        else if (a == L"--max-fps") { if (auto v = next()) o.session.max_fps = uint32_t(_wtoi(v)); }
        else if (a == L"--max-size") {
            if (auto v = next()) { o.session.max_size = uint32_t(_wtoi(v)); o.custom_max_size = true; }
        }
        else if (a == L"--time-limit") { if (auto v = next()) o.time_limit = _wtoi(v); }
        else { LOGE("Bilinmeyen seçenek: %s", to_utf8(a).c_str()); print_help(); return false; }
    }
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (!parse_args(argc, argv, g_opt)) return 1;
    g_settings.load();

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    adb::init_job();

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"KopruWindow";
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Köprü", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 400, 700, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;
    update_bar_height();

    WNDCLASSEXW vc{sizeof vc};
    vc.lpfnWndProc = video_proc;
    vc.hInstance = inst;
    vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    vc.lpszClassName = L"KopruVideo";
    RegisterClassExW(&vc);
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_video = CreateWindowExW(0, vc.lpszClassName, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, rc.right,
                              std::max<LONG>(1, rc.bottom - g_bar_h), g_hwnd, nullptr, inst, nullptr);
    if (!g_video || !g_renderer.init(g_video)) return 1;
    DragAcceptFiles(g_hwnd, TRUE);
    DragAcceptFiles(g_video, TRUE);
    build_menu();

    // Pencere telefon bağlanmadan açılır: son konum ya da son video boyutuna göre
    auto t0 = std::chrono::steady_clock::now();
    g_known_vw = g_settings.video_w;
    g_known_vh = g_settings.video_h;
    if (g_settings.has_window && MonitorFromRect(&g_settings.window, MONITOR_DEFAULTTONULL)) {
        const RECT& r = g_settings.window;
        SetWindowPos(g_hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        fit_window(g_known_vw, g_known_vh);
    }
    update_title();
    ShowWindow(g_hwnd, SW_SHOW);
    if (g_settings.topmost) set_topmost(true);

    std::thread render_thread(render_thread_fn);
    std::atomic<bool> conn_done{false};
    std::thread conn_thread([&] { connection_thread_fn(); conn_done = true; });
    if (g_opt.time_limit > 0) SetTimer(g_hwnd, kTimeLimitTimer, UINT(g_opt.time_limit * 1000), nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    // Kapanış: bağlantı thread'ini uyandır, soketleri kapat
    {
        std::lock_guard lk(g_conn_mutex);
        g_quit = true;
    }
    g_conn_cv.notify_all();
    g_session.interrupt();
    // adb komutu ortasındaysa en fazla 5 sn bekle; sonra süreci zaten kapatıyoruz
    for (int i = 0; i < 50 && !conn_done; ++i) Sleep(100);
    bool clean = conn_done.load();
    if (clean) conn_thread.join(); else conn_thread.detach();
    { std::lock_guard lk(g_rq.m); g_rq.quit = true; }
    g_rq.cv.notify_one();
    render_thread.join();
    if (g_rq.frame) av_frame_free(&g_rq.frame);
    if (!clean) {
        LOGW("Bağlantı thread'i zamanında kapanmadı, çıkılıyor");
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 0);
    }

    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    LOGI("Özet: %.1f sn, %llu paket, %.1f MB (ort. %.1f Mbps), çözülen %llu, gösterilen %llu, atlanan %llu",
         secs, (unsigned long long)g_stats.packets, g_stats.bytes / 1e6, g_stats.bytes * 8 / 1e6 / secs,
         (unsigned long long)g_stats.decoded, (unsigned long long)g_stats.presented,
         (unsigned long long)g_stats.dropped);
    WSACleanup();
    return 0;
}
