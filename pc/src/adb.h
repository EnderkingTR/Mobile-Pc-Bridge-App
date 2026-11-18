#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace adb {

// Bizim açtığımız tüm adb süreçleri bir Job nesnesine bağlanır:
// program çökse bile telefon tarafındaki sunucu kapanır.
void init_job();

std::wstring executable();
std::wstring exe_dir();

// adb'yi çalıştırır, bitmesini bekler. stdout+stderr çıktısını `output`a yazar.
int run(const std::vector<std::wstring>& args, std::string* output = nullptr);

// adb'yi arka planda başlatır (çıktısı konsolumuza akar). Süreç handle'ını döndürür.
HANDLE spawn(const std::vector<std::wstring>& args);

struct Device {
    std::wstring serial;
    std::string state;  // "device", "unauthorized", "offline"...
    bool network() const { return serial.find(L':') != std::wstring::npos; }  // "192.168.1.5:5555"
};
std::vector<Device> list_devices();

// Bağlı ve yetkili cihaz: USB her zaman Wi-Fi'den önce gelir. Yoksa boş döner, sebebi `error`da.
std::wstring first_device(std::string* error);
bool has_usb_device();

}  // namespace adb
