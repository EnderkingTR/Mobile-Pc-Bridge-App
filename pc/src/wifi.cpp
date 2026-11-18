#include <winsock2.h>
#include <ws2tcpip.h>

#include "wifi.h"

#include <sstream>

#include "adb.h"
#include "log.h"
#include "util.h"

namespace wifi {

static std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

std::wstring device_ip(const std::wstring& serial) {
    std::string out;
    if (adb::run({L"-s", serial, L"shell", L"ip -f inet addr show wlan0"}, &out) != 0) return {};
    // "    inet 192.168.1.23/24 brd 192.168.1.255 scope global wlan0"
    size_t p = out.find("inet ");
    if (p == std::string::npos) return {};
    p += 5;
    size_t end = out.find_first_of("/ \r\n", p);
    std::string ip = out.substr(p, end == std::string::npos ? std::string::npos : end - p);
    if (ip.empty() || ip.find('.') == std::string::npos) return {};
    return to_wide(ip);
}

static std::wstring endpoint(const std::wstring& ip) { return ip + L":" + std::to_wstring(kPort); }

// adbd'nin portu 500 ms içinde cevap veriyor mu? (adb connect ulaşılamayan IP'de uzun süre bekler)
static bool probe(const std::wstring& ip) {
    std::string ip8 = to_utf8(ip);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    if (inet_pton(AF_INET, ip8.c_str(), &addr.sin_addr) != 1) return false;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
    connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    fd_set wr, ex;
    FD_ZERO(&wr);
    FD_ZERO(&ex);
    FD_SET(s, &wr);
    FD_SET(s, &ex);
    timeval tv{0, 500'000};
    bool ok = select(0, nullptr, &wr, &ex, &tv) > 0 && FD_ISSET(s, &wr);
    closesocket(s);
    return ok;
}

bool connect(const std::wstring& ip) {
    if (ip.empty() || !probe(ip)) return false;
    std::string out;
    adb::run({L"connect", endpoint(ip)}, &out);
    // "connected to 192.168.1.23:5555" ya da "already connected to ..."
    bool ok = out.find("connected to") != std::string::npos && out.find("cannot") == std::string::npos;
    if (!ok) LOGW("adb connect başarısız: %s", trim(out).c_str());
    return ok;
}

void disconnect(const std::wstring& ip) {
    if (!ip.empty()) adb::run({L"disconnect", endpoint(ip)});
}

std::wstring prepare(const std::wstring& serial, const std::atomic<bool>& abort, std::string* error) {
    std::wstring ip = device_ip(serial);
    if (ip.empty()) {
        if (error) *error = "Telefon Wi-Fi'ye bağlı değil";
        return {};
    }

    std::string port;
    adb::run({L"-s", serial, L"shell", L"getprop service.adb.tcp.port"}, &port);
    if (trim(port) != std::to_string(kPort)) {
        LOGI("Telefon kablosuz moda alınıyor (adb tcpip %d)...", kPort);
        std::string out;
        if (adb::run({L"-s", serial, L"tcpip", std::to_wstring(kPort)}, &out) != 0) {
            if (error) *error = "adb tcpip başarısız: " + trim(out);
            return {};
        }
        // adbd yeniden başlıyor: USB cihazı kısa süre kaybolur, geri gelmesini bekle
        Sleep(1000);
        bool back = false;
        for (int i = 0; i < 40 && !abort; ++i) {
            std::string st;
            adb::run({L"-s", serial, L"get-state"}, &st);
            if (trim(st) == "device") { back = true; break; }
            Sleep(250);
        }
        if (!back) {
            if (error) *error = "Telefon tcpip sonrası geri gelmedi";
            return {};
        }
    }

    // adbd TCP portunu açması bir an sürebilir
    for (int i = 0; i < 8 && !abort; ++i) {
        if (connect(ip)) {
            LOGI("Kablosuz bağlantı hazır: %s:%d (kablo çıkarılınca Wi-Fi'den devam edilir)", to_utf8(ip).c_str(), kPort);
            return ip;
        }
        Sleep(500);
    }
    if (error) *error = "Telefona Wi-Fi üzerinden ulaşılamadı (aynı ağda mısınız?)";
    return {};
}

}  // namespace wifi
