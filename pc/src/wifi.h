#pragma once
#include <atomic>
#include <string>

// Android 9'da "kablosuz hata ayıklama" yok: telefon USB ile bağlıyken adbd'yi
// `adb tcpip 5555` ile TCP moduna alıp IP üzerinden bağlanıyoruz. Bu mod telefon
// yeniden başlatılana kadar kalıcı.
namespace wifi {

constexpr int kPort = 5555;

// Telefonun wlan0 IPv4 adresi ("192.168.1.23"); Wi-Fi'ye bağlı değilse boş.
std::wstring device_ip(const std::wstring& serial);

// USB'deki telefonu Wi-Fi'ye hazırlar: gerekirse tcpip moduna alır (adbd yeniden başlar,
// USB bağlantısı birkaç saniye kopar) ve `adb connect` yapar. Başarılıysa IP'yi döndürür.
std::wstring prepare(const std::wstring& usb_serial, const std::atomic<bool>& abort, std::string* error);

// Kayıtlı IP'ye bağlanmayı dener (önce kısa bir TCP yoklaması: ulaşılamıyorsa adb'yi bekletme).
bool connect(const std::wstring& ip);

void disconnect(const std::wstring& ip);

}  // namespace wifi
