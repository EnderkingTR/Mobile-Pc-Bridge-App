# Faz 0 — Ölçümler (2026-11-18)

## Cihazlar
- **Telefon:** Samsung SM-G611F (Galaxy J7 Prime 2), Android 9 (API 28), Exynos 7870, 2.8 GB RAM
- **Ekran:** 1080x1920, 420 dpi, 60 Hz
- **PC:** Windows 11, RTX 4060 Laptop GPU

## Telefon kodek yetenekleri (media_codecs.xml)
| Kodek | Yön | Maks. boyut | blocks/s | Yorum |
|---|---|---|---|---|
| OMX.Exynos.AVC.Encoder | encode | 1920x1088 | 486000 | 1080p ≈ 60 FPS → **yansıtma için ana kodek** |
| OMX.Exynos.HEVC.Encoder | encode | 1920x1088 | 30375 | 1080p'de ~4 FPS → **kullanılamaz** |
| OMX.Exynos.avc.dec | decode | 1920x1440 | 489600 | 1080p60 → ikinci ekran için uygun |
| OMX.Exynos.hevc.dec | decode | 1920x1088 | 489600 | 1080p60 → ikinci ekranda bant genişliği tasarrufu için aday |

## scrcpy taban testi (USB, H.264, 1080x1920, 8 Mbps)
- PC tarafında D3D11VA ile donanımsal çözme çalıştı.
- Hareket varken **53–57 FPS** (ekran durağansa FPS düşer, bu normal: kare sadece değişince gelir).
- Ses: Android 9'da desteklenmiyor (beklendiği gibi).
- `setLatency` parametresi kodlayıcı tarafından desteklenmiyor → düşük gecikmeyi tampon/bitrate ayarlarıyla sağlamamız gerekecek.

## Sorunlar
- İlk testte ADB USB bağlantısı koptu (`transport endpoint shutdown`). Aynı anda paralel `adb shell` komutları gidiyordu. İkinci testte kopma olmadı.
- Şüpheliler: kablo/port, PC'de kurulu spacedesk Android USB sürücüsü. Uygulamamızda otomatik yeniden bağlanma şart.

## Sonuç
- Native 1080p @ ~60 FPS H.264 yansıtma bu cihazda mümkün.
- Telefon tarafında kodlama için H.264 kullanacağız. İkinci ekranda PC NVENC ile kodlayacak, telefon H.264 ya da HEVC çözecek.
