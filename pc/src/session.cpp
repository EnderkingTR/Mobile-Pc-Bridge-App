#include "session.h"

#include <ws2tcpip.h>

#include <cstdio>
#include <random>

#include "adb.h"
#include "log.h"
#include "util.h"

static constexpr wchar_t kDeviceServerPath[] = L"/data/local/tmp/scrcpy-server.jar";
static constexpr uint16_t kFirstPort = 27183;
static constexpr uint16_t kLastPort = 27199;

bool Session::recv_all(SOCKET s, void* buf, size_t len) {
    auto* p = static_cast<char*>(buf);
    while (len) {
        int n = recv(s, p, int(std::min<size_t>(len, 1 << 20)), 0);
        if (n <= 0) return false;
        p += n;
        len -= size_t(n);
    }
    return true;
}

bool Session::send_control(const std::vector<uint8_t>& msg) {
    std::lock_guard lk(send_mutex_);
    if (control_ == INVALID_SOCKET) return false;
    const char* p = reinterpret_cast<const char*>(msg.data());
    size_t left = msg.size();
    while (left) {
        int n = send(control_, p, int(left), 0);
        if (n <= 0) return false;
        p += n;
        left -= size_t(n);
    }
    return true;
}

static SOCKET connect_local(uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

bool Session::start(const SessionOptions& o) {
    adb::run({L"start-server"});

    serial_ = o.serial;
    if (serial_.empty()) {
        std::string err;
        serial_ = adb::first_device(&err);
        if (serial_.empty()) {
            LOGE("%s", err.c_str());
            return false;
        }
    }
    LOGI("Cihaz: %s", to_utf8(serial_).c_str());

    // 1) Sunucuyu telefona yükle (exe'nin yanındaki dosya, göreli yol: Türkçe karakterli
    //    klasör adları adb'ye hiç gitmesin)
    std::string out;
    if (adb::run({L"-s", serial_, L"push", L"scrcpy-server", kDeviceServerPath}, &out) != 0) {
        LOGE("Sunucu telefona yüklenemedi:\n%s", out.c_str());
        return false;
    }

    // 2) adb forward tüneli: PC'deki TCP portu -> telefondaki yerel soket
    std::random_device rd;
    uint32_t scid = rd() & 0x7fffffff;
    wchar_t socket_name[32];
    swprintf(socket_name, 32, L"localabstract:scrcpy_%08x", scid);

    uint16_t port = 0;
    for (uint16_t p = kFirstPort; p <= kLastPort; ++p) {
        if (adb::run({L"-s", serial_, L"forward", L"tcp:" + std::to_wstring(p), socket_name}, &out) == 0) {
            port = p;
            break;
        }
    }
    if (!port) {
        LOGE("adb forward kurulamadı:\n%s", out.c_str());
        return false;
    }

    // 3) Sunucuyu başlat
    wchar_t scid_arg[32];
    swprintf(scid_arg, 32, L"scid=%08x", scid);
    std::vector<std::wstring> args = {
        L"-s", serial_, L"shell",
        std::wstring(L"CLASSPATH=") + kDeviceServerPath,
        L"app_process", L"/", L"com.genymobile.scrcpy.Server",
        to_wide(SCRCPY_SERVER_VERSION),
        scid_arg,
        L"log_level=info",
        L"audio=false",           // Android < 11'de zaten desteklenmiyor
        L"tunnel_forward=true",
        L"video_codec=h264",      // Exynos 7870'te HEVC kodlayıcı çok yavaş (Faz 0)
        L"video_bit_rate=" + std::to_wstring(o.bit_rate),
        L"max_fps=" + std::to_wstring(o.max_fps),
    };
    if (o.max_size) args.push_back(L"max_size=" + std::to_wstring(o.max_size));
    if (o.stay_awake) args.push_back(L"stay_awake=true");

    server_proc_ = adb::spawn(args);
    if (!server_proc_) return false;

    // 4) Video soketine bağlan. Forward modunda adb, telefonda dinleyen olmasa bile
    //    bağlantıyı kabul eder; sunucu hazır olunca ilk sokete 1 "dummy" bayt gönderir.
    for (int attempt = 0; attempt < 100 && video_ == INVALID_SOCKET; ++attempt) {
        if (WaitForSingleObject(server_proc_, 0) == WAIT_OBJECT_0) {
            LOGE("Telefondaki sunucu beklenmedik şekilde kapandı");
            break;
        }
        SOCKET s = connect_local(port);
        uint8_t dummy;
        if (s != INVALID_SOCKET && recv_all(s, &dummy, 1)) {
            video_ = s;
            break;
        }
        if (s != INVALID_SOCKET) closesocket(s);
        Sleep(100);
    }
    if (video_ == INVALID_SOCKET) {
        LOGE("Video soketine bağlanılamadı");
        adb::run({L"-s", serial_, L"forward", L"--remove", L"tcp:" + std::to_wstring(port)});
        stop();
        return false;
    }

    // Sunucu, cihaz bilgisini ancak tüm soketler kabul edildikten sonra gönderir
    control_ = connect_local(port);
    if (control_ == INVALID_SOCKET) {
        LOGE("Kontrol soketine bağlanılamadı");
        stop();
        return false;
    }

    // Cihaz bilgisi: 64 baytlık cihaz adı
    char name[64];
    if (!recv_all(video_, name, sizeof name)) {
        LOGE("Cihaz bilgisi okunamadı");
        stop();
        return false;
    }
    name[63] = '\0';
    device_name_ = name;

    // Soketler açıldıktan sonra tünele gerek yok
    adb::run({L"-s", serial_, L"forward", L"--remove", L"tcp:" + std::to_wstring(port)});

    BOOL nodelay = TRUE;
    setsockopt(control_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof nodelay);
    setsockopt(video_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof nodelay);
    int rcvbuf = 4 << 20;
    setsockopt(video_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof rcvbuf);

    LOGI("Bağlandı: %s (port %u)", device_name_.c_str(), port);
    return true;
}

void Session::stop() {
    {
        std::lock_guard lk(send_mutex_);
        if (control_ != INVALID_SOCKET) {
            shutdown(control_, SD_BOTH);
            closesocket(control_);
            control_ = INVALID_SOCKET;
        }
    }
    if (video_ != INVALID_SOCKET) {
        shutdown(video_, SD_BOTH);
        closesocket(video_);
        video_ = INVALID_SOCKET;
    }
    if (server_proc_) {
        // Soketler kapanınca sunucu kendiliğinden çıkar; kısa süre bekle, sonra zorla kapat
        if (WaitForSingleObject(server_proc_, 1500) == WAIT_TIMEOUT) TerminateProcess(server_proc_, 0);
        CloseHandle(server_proc_);
        server_proc_ = nullptr;
    }
}
