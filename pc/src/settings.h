#pragma once
#include <windows.h>

#include <cstdint>
#include <iterator>

// Kalite profilleri: çalışırken değiştirilebilir (sunucu yeni ayarla yeniden başlar)
struct Profile {
    const wchar_t* name;
    uint32_t max_size;  // 0 = tam çözünürlük
    uint32_t bit_rate;
};
inline constexpr Profile kProfiles[] = {
    {L"Hız", 1280, 4'000'000},       // daha az piksel: telefonun kodlayıcısı daha hızlı, gecikme düşük
    {L"Dengeli", 0, 8'000'000},
    {L"Kalite", 0, 16'000'000},
};
inline constexpr int kProfileCount = int(std::size(kProfiles));

// %APPDATA%\Kopru\kopru.ini içinde saklanan kullanıcı tercihleri
struct Settings {
    int profile = 1;
    bool topmost = false;
    RECT window{};          // son pencere konumu/boyutu
    bool has_window = false;
    int video_w = 1080, video_h = 1920;  // son görülen video boyutu (telefon bağlanmadan pencere boyu için)

    void load();
    void save() const;
};
