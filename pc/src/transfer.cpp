#include "transfer.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <thread>

#include "adb.h"
#include "log.h"
#include "util.h"

namespace fs = std::filesystem;

namespace transfer {

static std::atomic<int> g_active{0};

bool busy() { return g_active > 0; }

static bool is_apk(const fs::path& p) {
    std::wstring ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    return ext == L".apk";
}

// adb çıktısından okunur bir hata satırı çıkar (ör. "Failure [INSTALL_FAILED_...]")
static std::wstring short_error(const std::string& out) {
    std::string best;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t end = out.find('\n', pos);
        std::string line = out.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find("Failure") != std::string::npos || line.find("error") != std::string::npos ||
            line.find("failed") != std::string::npos)
            best = line;
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    if (best.empty()) best = out.substr(0, 120);
    // "adb.exe: failed to install C:\uzun\yol\x.apk: Failure [INSTALL_...]" → asıl sebep sonda
    if (size_t f = best.find("Failure ["); f != std::string::npos) {
        best = best.substr(f + 9);
        if (size_t colon = best.find_first_of(":]"); colon != std::string::npos) best.resize(colon);
    }
    if (best.size() > 120) best.resize(120);
    return to_wide(best);
}

void start(std::wstring serial, std::vector<std::wstring> files,
           std::function<void(const std::wstring&, bool)> status,
           std::function<void(const std::string&)> on_pushed) {
    ++g_active;
    std::thread([serial = std::move(serial), files = std::move(files), status = std::move(status),
                 on_pushed = std::move(on_pushed)] {
        int ok = 0, failed = 0;
        size_t total = files.size();
        for (size_t i = 0; i < total; ++i) {
            fs::path p(files[i]);
            std::wstring name = p.filename().wstring();
            std::wstring counter = total > 1 ? L" (" + std::to_wstring(i + 1) + L"/" + std::to_wstring(total) + L")" : L"";
            std::string out;

            if (is_apk(p)) {
                status(L"Kuruluyor: " + name + counter, false);
                int rc = adb::run({L"-s", serial, L"install", L"-r", p.wstring()}, &out);
                if (rc == 0 && out.find("Success") != std::string::npos) {
                    ++ok;
                    LOGI("Kuruldu: %s", to_utf8(name).c_str());
                } else {
                    ++failed;
                    LOGE("Kurulamadı: %s\n%s", to_utf8(name).c_str(), out.c_str());
                    status(L"Kurulamadı: " + name + L" — " + short_error(out), false);
                    Sleep(2500);
                }
            } else {
                status(L"Gönderiliyor: " + name + counter, false);
                std::wstring dest = std::wstring(kDeviceDir) + name;
                int rc = adb::run({L"-s", serial, L"push", p.wstring(), dest}, &out);
                if (rc == 0) {
                    ++ok;
                    LOGI("Gönderildi: %s -> %s", to_utf8(name).c_str(), to_utf8(dest).c_str());
                    std::error_code ec;
                    if (!fs::is_directory(p, ec) && on_pushed) on_pushed(to_utf8(dest));
                } else {
                    ++failed;
                    LOGE("Gönderilemedi: %s\n%s", to_utf8(name).c_str(), out.c_str());
                    status(L"Gönderilemedi: " + name + L" — " + short_error(out), false);
                    Sleep(2500);
                }
            }
        }
        std::wstring summary;
        if (failed == 0) {
            summary = total == 1 ? L"Tamamlandı: " + fs::path(files[0]).filename().wstring()
                                 : L"Tamamlandı: " + std::to_wstring(ok) + L" dosya";
        } else {
            summary = std::to_wstring(ok) + L" başarılı, " + std::to_wstring(failed) + L" başarısız (ayrıntı konsolda)";
        }
        --g_active;
        status(summary, true);
    }).detach();
}

}  // namespace transfer
