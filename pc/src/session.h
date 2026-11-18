#pragma once
#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct SessionOptions {
    std::wstring serial;          // boşsa ilk cihaz
    uint32_t bit_rate = 8'000'000;
    uint32_t max_fps = 60;
    uint32_t max_size = 0;        // 0 = telefonun kendi çözünürlüğü
    bool stay_awake = false;      // USB'deyken ekranı açık tut
    bool wifi_enabled = false;    // USB'deyken Wi-Fi'yi hazırla, USB yoksa Wi-Fi'den bağlan
    std::wstring wifi_ip;         // son bilinen telefon IP'si
};

// Cihaz seçildikten sonra, bağlantı türüne göre (USB/Wi-Fi) seçenekleri son haline getirir
using OptionsAdjuster = std::function<void(SessionOptions&, bool over_wifi)>;

// Telefondaki scrcpy sunucusuyla bir bağlantı oturumu:
// sunucuyu yükler, adb tüneli kurar, video + kontrol soketlerini açar.
// start()/stop() bağlantı thread'inden çağrılır; send_control() ve interrupt()
// her thread'den güvenle çağrılabilir. Aynı nesne tekrar tekrar başlatılabilir.
class Session {
public:
    enum class Result { Ok, NoDevice, Failed };

    // `abort` true olursa bekleme döngülerinden erken çıkar. Hata metni `error`a yazılır.
    Result start(SessionOptions opts, const OptionsAdjuster& adjust, const std::atomic<bool>& abort,
                 std::string* error);
    void stop();
    // Açık soketleri kapatır: okuma yapan thread'ler hemen uyanır (yeniden bağlanma/kapanış için)
    void interrupt();

    bool recv_video(void* buf, size_t len) { return recv_all(video_, buf, len); }
    bool recv_control(void* buf, size_t len) { return recv_all(control_, buf, len); }
    bool send_control(const std::vector<uint8_t>& msg);

    const std::string& device_name() const { return device_name_; }
    const std::wstring& serial() const { return serial_; }
    bool over_wifi() const { return serial_.find(L':') != std::wstring::npos; }
    // USB oturumunda Wi-Fi hazırlandıysa telefonun IP'si (kaydedilmek üzere)
    const std::wstring& prepared_wifi_ip() const { return prepared_ip_; }

private:
    static bool recv_all(SOCKET s, void* buf, size_t len);

    std::wstring serial_;
    std::string device_name_;
    std::wstring prepared_ip_;
    std::atomic<SOCKET> video_{INVALID_SOCKET};
    std::atomic<SOCKET> control_{INVALID_SOCKET};
    HANDLE server_proc_ = nullptr;
    std::mutex sock_mutex_;  // soket açma/kapama ve gönderme
};
