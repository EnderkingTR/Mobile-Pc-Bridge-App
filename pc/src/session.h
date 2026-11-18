#pragma once
#include <winsock2.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct SessionOptions {
    std::wstring serial;          // boşsa ilk cihaz
    uint32_t bit_rate = 8'000'000;
    uint32_t max_fps = 60;
    uint32_t max_size = 0;        // 0 = telefonun kendi çözünürlüğü
    bool stay_awake = false;      // USB'deyken ekranı açık tut
};

// Telefondaki scrcpy sunucusuyla bir bağlantı oturumu:
// sunucuyu yükler, adb tüneli kurar, video + kontrol soketlerini açar.
class Session {
public:
    bool start(const SessionOptions& opts);
    void stop();

    bool recv_video(void* buf, size_t len) { return recv_all(video_, buf, len); }
    bool recv_control(void* buf, size_t len) { return recv_all(control_, buf, len); }
    bool send_control(const std::vector<uint8_t>& msg);

    const std::string& device_name() const { return device_name_; }
    const std::wstring& serial() const { return serial_; }

private:
    static bool recv_all(SOCKET s, void* buf, size_t len);

    std::wstring serial_;
    std::string device_name_;
    SOCKET video_ = INVALID_SOCKET;
    SOCKET control_ = INVALID_SOCKET;
    HANDLE server_proc_ = nullptr;
    std::mutex send_mutex_;
};
