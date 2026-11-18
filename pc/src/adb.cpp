#include "adb.h"

#include <filesystem>
#include <sstream>

#include "log.h"
#include "util.h"

namespace fs = std::filesystem;

namespace adb {

static HANDLE g_job = nullptr;

void init_job() {
    g_job = CreateJobObjectW(nullptr, nullptr);
    if (!g_job) return;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &info, sizeof info);
}

std::wstring exe_dir() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    buf.resize(n);
    return fs::path(buf).parent_path().wstring();
}

static std::wstring env(const wchar_t* name) {
    wchar_t buf[2048];
    DWORD n = GetEnvironmentVariableW(name, buf, 2048);
    return (n > 0 && n < 2048) ? std::wstring(buf, n) : std::wstring();
}

std::wstring executable() {
    static std::wstring cached;
    if (!cached.empty()) return cached;

    if (auto e = env(L"KOPRU_ADB"); !e.empty()) return cached = e;
    for (const wchar_t* var : {L"ANDROID_HOME", L"ANDROID_SDK_ROOT"}) {
        if (auto e = env(var); !e.empty()) {
            fs::path p = fs::path(e) / L"platform-tools" / L"adb.exe";
            if (fs::exists(p)) return cached = p.wstring();
        }
    }
    if (auto e = env(L"LOCALAPPDATA"); !e.empty()) {
        fs::path p = fs::path(e) / L"Android" / L"Sdk" / L"platform-tools" / L"adb.exe";
        if (fs::exists(p)) return cached = p.wstring();
    }
    return cached = L"adb.exe";  // PATH'ten bulunsun
}

// Windows komut satırı kurallarına göre tırnaklama
static void append_quoted(std::wstring& cmd, const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    for (size_t i = 0;; ++i) {
        size_t bs = 0;
        while (i < arg.size() && arg[i] == L'\\') { ++bs; ++i; }
        if (i == arg.size()) { cmd.append(bs * 2, L'\\'); break; }
        if (arg[i] == L'"') { cmd.append(bs * 2 + 1, L'\\'); cmd += L'"'; }
        else { cmd.append(bs, L'\\'); cmd += arg[i]; }
    }
    cmd += L'"';
}

static std::wstring build_cmd(const std::vector<std::wstring>& args) {
    std::wstring cmd;
    append_quoted(cmd, executable());
    for (const auto& a : args) { cmd += L' '; append_quoted(cmd, a); }
    return cmd;
}

int run(const std::vector<std::wstring>& args, std::string* output) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = build_cmd(args);
    std::wstring dir = exe_dir();
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, dir.c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        LOGE("adb çalıştırılamadı (%s). Android SDK platform-tools kurulu mu?",
             to_utf8(executable()).c_str());
        return -1;
    }
    if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);

    std::string data;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) data.append(buf, n);
    CloseHandle(rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (output) *output = std::move(data);
    return int(code);
}

HANDLE spawn(const std::vector<std::wstring>& args) {
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = build_cmd(args);
    std::wstring dir = exe_dir();
    // Konsol uygulaması olduğumuz için alt süreç aynı konsolu kullanır.
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(),
                        &si, &pi)) {
        LOGE("adb başlatılamadı");
        return nullptr;
    }
    if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

std::vector<Device> list_devices() {
    std::vector<Device> devices;
    std::string out;
    if (run({L"devices"}, &out) != 0) return devices;
    std::istringstream in(out);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        devices.push_back({to_wide(line.substr(0, tab)), line.substr(tab + 1)});
    }
    return devices;
}

bool has_usb_device() {
    for (const auto& d : list_devices())
        if (!d.network() && d.state == "device") return true;
    return false;
}

std::wstring first_device(std::string* error) {
    auto devices = list_devices();
    bool unauthorized = false;
    for (bool want_network : {false, true}) {
        for (const auto& d : devices) {
            if (d.network() != want_network) continue;
            if (d.state == "device") return d.serial;
            if (d.state == "unauthorized") unauthorized = true;
        }
    }
    if (error) {
        *error = unauthorized
                     ? "Telefon yetkisiz: telefonda 'USB hata ayıklamaya izin ver' sorusunu onayla."
                     : "Bağlı telefon bulunamadı. USB kablosunu ve USB hata ayıklamayı kontrol et.";
    }
    return {};
}

}  // namespace adb
