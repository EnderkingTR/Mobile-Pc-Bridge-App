#pragma once
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace transfer {

constexpr wchar_t kDeviceDir[] = L"/sdcard/Download/";

// Dosyaları arka planda telefona gönderir: .apk → kurulur, diğerleri → İndirilenler klasörüne.
// İlerleme metinleri `status` ile bildirilir (arka plan thread'inden çağrılır).
// Gönderilen her dosyanın telefondaki yolu `on_pushed` ile bildirilir (galeri taraması için).
void start(std::wstring serial, std::vector<std::wstring> files,
           std::function<void(const std::wstring& text, bool done)> status,
           std::function<void(const std::string& device_path)> on_pushed);

bool busy();

}  // namespace transfer
