# Köprü

Android telefon ekranını Windows'ta düşük gecikmeyle gösterip kontrol etmek için bir uygulama.
Android 5+ ile çalışır (Android 9 dahil). Root gerekmez.

Telefon tarafında [scrcpy](https://github.com/Genymobile/scrcpy) sunucusu kullanılır (Apache 2.0,
`third_party/scrcpy-server`). PC istemcisi bu projeye özeldir: FFmpeg D3D11VA ile donanımsal çözme +
Direct3D 11 ile çizim.

## Derleme

Gerekenler:
- Visual Studio 2022, **"C++ ile masaüstü geliştirme"** iş yükü (MSVC + Windows SDK)
- CMake 3.20+
- FFmpeg shared geliştirici paketi → `third_party/ffmpeg` klasörüne
  (ör. BtbN `ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip`; içinde `include/`, `lib/`, `bin/` olmalı)

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Çıktı: `build/Release/kopru.exe` (FFmpeg DLL'leri ve `scrcpy-server` yanına kopyalanır).

## Kullanım

1. Telefonda **Geliştirici seçenekleri → USB hata ayıklama**'yı aç, USB ile bağla.
2. `kopru.exe`'yi çalıştır.

adb şu sırayla aranır: `KOPRU_ADB` ortam değişkeni, `ANDROID_HOME`/`ANDROID_SDK_ROOT`,
`%LOCALAPPDATA%\Android\Sdk\platform-tools`, sonra `PATH`.

Seçenekler: `kopru --help`

Program telefon takılı olmadan da açılır ve bekler; kablo çıkarılır ya da bağlantı koparsa
telefon geri geldiğinde kendiliğinden yeniden bağlanır.

**Profiller** (Alt+1/2/3 ya da başlık çubuğu menüsü, çalışırken değişir):

| Profil | Çözünürlük | Bit hızı |
|---|---|---|
| Hız | en uzun kenar 1280 | 4 Mbps |
| Dengeli (varsayılan) | tam | 8 Mbps |
| Kalite | tam | 16 Mbps |

**Dosya gönderme:** Dosyaları pencereye sürükle-bırak ya da Alt+F. `.apk` dosyaları telefona kurulur,
diğerleri telefonun `Download` klasörüne gider ve galeride/dosyalarda görünür.

Profil, "her zaman üstte" ve pencere konumu `%APPDATA%\Kopru\kopru.ini` dosyasında saklanır.
Pencere simgesine tıklayınca (ya da Alt+Boşluk) açılan menüde tüm seçenekler var.

Pencerenin altında Android gezinme çubuğu var (□ son uygulamalar, ○ ana ekran, ◁ geri). Fiziksel
tuşlu telefonlarda bu tuşlar ekranda görünmediği için eklendi. Basılı tutma da telefona iletilir.

| Kısayol | İşlev |
|---|---|
| Sol tık / sürükle | Dokunma |
| Sağ tık | Geri (ekran kapalıysa açar) |
| Orta tık | Ana ekran |
| Tekerlek | Kaydırma |
| Alt+H / Alt+B / Alt+S | Ana ekran / Geri / Son uygulamalar |
| Alt+P | Güç tuşu |
| Alt+↑ / Alt+↓ | Ses |
| Alt+N / Alt+Shift+N | Bildirim panelini aç / kapat |
| Alt+R | Ekranı döndür |
| Alt+O | Telefon ekranını kapat / aç (yansıtma devam eder) |
| Alt+T | Pencere her zaman üstte |
| Alt+1 / Alt+2 / Alt+3 | Profil: Hız / Dengeli / Kalite |
| Alt+F | Dosya gönder / APK kur |
| Ctrl+V | PC panosunu telefona yapıştır |
| Ctrl+C / Ctrl+X | Telefonda kopyala/kes, PC panosuna al |

Türkçe karakterler (ç, ğ, ı, ö, ş, ü) Android'in tuş enjeksiyonu desteklemediği için telefonun panosu
üzerinden yapıştırılır.

## Lisans

Bu proje MIT lisanslıdır (`LICENSE`). `third_party/scrcpy-server` Genymobile'ın scrcpy projesinden
alınmıştır ve Apache 2.0 lisansı altındadır (`third_party/scrcpy-server/LICENSE`).

## Bilinen sınırlar

- Android 10 altında ses aktarımı yok (Android'in API sınırı).
