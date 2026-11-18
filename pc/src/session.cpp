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
    if (s == INVALID_SOCKET) return false;
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
    std::lock_guard lk(sock_mutex_);
    SOCKET s = control_;
    if (s == INVALID_SOCKET) return false;
    const char* p = reinterpret_cast<const char*>(msg.data());
    size_t left = msg.size();
    while (left) {
        int n = send(s, p, int(left), 0);
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

Session::Result Session::start(const SessionOptions& o, const std::atomic<bool>& abort, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        stop();
        return Result::Failed;
    };

    adb::run({L"start-server"});

    serial_ = o.serial;
    if (serial_.empty()) {
        std::string err;
        serial_ = adb::first_device(&err);
        if (serial_.empty()) {
            if (error) *error = err;
            return Result::NoDevice;
        }
    }

    // 1) Sunucuyu telefona yükle (exe'nin yanındaki dosya, göreli yol: Türkçe karakterli
    //    klasör adları adb'ye hiç gitmesin)
    std::string out;
    if (adb::run({L"-s", serial_, L"push", L"scrcpy-server", kDeviceServerPath}, &out) != 0) {
        // Kablo tam o anda çıkarıldıysa bu da "cihaz yok" sayılır
        if (error) *error = "Sunucu telefona yüklenemedi: " + out;
        return Result::NoDevice;
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
    if (!port) return fail("adb forward kurulamadı: " + out);
    auto remove_forward = [&] {
        adb::run({L"-s", serial_, L"forward", L"--remove", L"tcp:" + std::to_wstring(port)});
    };

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
    if (!server_proc_) {
        remove_forward();
        return fail("Sunucu başlatılamadı");
    }

    // 4) Video soketine bağlan. Forward modunda adb, telefonda dinleyen olmasa bile
    //    bağlantıyı kabul eder; sunucu hazır olunca ilk sokete 1 "dummy" bayt gönderir.
    SOCKET video = INVALID_SOCKET;
    for (int attempt = 0; attempt < 100 && !abort; ++attempt) {
        if (WaitForSingleObject(server_proc_, 0) == WAIT_OBJECT_0) break;  // sunucu kapandı
        SOCKET s = connect_local(port);
        uint8_t dummy;
        if (s != INVALID_SOCKET && recv_all(s, &dummy, 1)) {
            video = s;
            break;
        }
        if (s != INVALID_SOCKET) closesocket(s);
        Sleep(100);
    }
    if (video == INVALID_SOCKET) {
        remove_forward();
        return fail(abort ? "İptal edildi" : "Video soketine bağlanılamadı");
    }

    // Sunucu, cihaz bilgisini ancak tüm soketler kabul edildikten sonra gönderir
    SOCKET control = connect_local(port);
    {
        std::lock_guard lk(sock_mutex_);
        video_ = video;
        control_ = control;
    }
    remove_forward();  // soketler açıldıktan sonra tünele gerek yok
    if (control == INVALID_SOCKET) return fail("Kontrol soketine bağlanılamadı");

    // Cihaz bilgisi: 64 baytlık cihaz adı
    char name[64];
    if (!recv_all(video, name, sizeof name)) return fail("Cihaz bilgisi okunamadı");
    name[63] = '\0';
    device_name_ = name;

    BOOL nodelay = TRUE;
    setsockopt(control, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof nodelay);
    setsockopt(video, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof nodelay);
    int rcvbuf = 4 << 20;
    setsockopt(video, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof rcvbuf);

    LOGI("Bağlandı: %s (%s, port %u)", device_name_.c_str(), to_utf8(serial_).c_str(), port);
    return Result::Ok;
}

void Session::interrupt() {
    std::lock_guard lk(sock_mutex_);
    if (SOCKET s = video_; s != INVALID_SOCKET) shutdown(s, SD_BOTH);
    if (SOCKET s = control_; s != INVALID_SOCKET) shutdown(s, SD_BOTH);
}

void Session::stop() {
    {
        std::lock_guard lk(sock_mutex_);
        for (auto* sock : {&video_, &control_}) {
            SOCKET s = sock->exchange(INVALID_SOCKET);
            if (s != INVALID_SOCKET) {
                shutdown(s, SD_BOTH);
                closesocket(s);
            }
        }
    }
    if (server_proc_) {
        // Soketler kapanınca sunucu kendiliğinden çıkar; kısa süre bekle, sonra zorla kapat
        if (WaitForSingleObject(server_proc_, 1500) == WAIT_TIMEOUT) TerminateProcess(server_proc_, 0);
        CloseHandle(server_proc_);
        server_proc_ = nullptr;
    }
}
