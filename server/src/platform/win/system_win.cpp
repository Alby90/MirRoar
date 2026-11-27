// MirRoar server (Windows) - system services: clock, init, network interfaces, processes, autostart.
#include "winutil.h"

#include <iphlpapi.h>
#include <mfapi.h>
#include <shlobj.h>
#include <timeapi.h>

int64_t nowUs() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 1000000.0 / (double)freq.QuadPart);
}

void platformInit() {
    SetConsoleOutputCP(CP_UTF8);
    // Physical pixel coordinates everywhere (capture + SendInput).
    using SetCtxFn = BOOL(WINAPI*)(HANDLE);
    if (auto fn = (SetCtxFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))
        fn((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */);
    else
        SetProcessDPIAware();
    timeBeginPeriod(1);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
}

void platformShutdown() {
    MFShutdown();
    WSACleanup();
    timeEndPeriod(1);
}

void platformThreadInit() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
void platformThreadExit() { CoUninitialize(); }

std::string hostName() {
    wchar_t host[256];
    DWORD n = 256;
    if (!GetComputerNameW(host, &n)) return "PC";
    return wideToUtf8(host);
}

std::string exePath() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return wideToUtf8(path);
}

std::string configDir() {
    wchar_t* appdata = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
        dir = std::wstring(appdata) + L"\\MirRoar";
        CoTaskMemFree(appdata);
    } else {
        dir = L".";
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return wideToUtf8(dir);
}

std::vector<NetIface> listIPv4Interfaces() {
    std::vector<NetIface> res;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    auto* addrs = (IP_ADAPTER_ADDRESSES*)buf.data();
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG r = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        addrs = (IP_ADAPTER_ADDRESSES*)buf.data();
        r = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
    }
    if (r != NO_ERROR) return res;
    for (auto* a = addrs; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            NetIface ni;
            ni.name = wideToUtf8(a->FriendlyName);
            ni.addr = ((sockaddr_in*)u->Address.lpSockaddr)->sin_addr.s_addr;
            ULONG mask = 0;
            if (u->OnLinkPrefixLength > 0 && u->OnLinkPrefixLength <= 32) ConvertLengthToIpv4Mask(u->OnLinkPrefixLength, &mask);
            ni.mask = mask;
            res.push_back(ni);
        }
    }
    return res;
}

std::string runProcess(const std::string& cmdline) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return {};
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring cl = utf8ToWide(cmdline);
    std::string out;
    if (CreateProcessW(nullptr, &cl[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(wr);
        wr = nullptr;
        char tmp[4096];
        DWORD n;
        while (ReadFile(rd, tmp, sizeof(tmp), &n, nullptr) && n > 0) out.append(tmp, n);
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (wr) CloseHandle(wr);
    CloseHandle(rd);
    return out;
}

std::string findAdb(const std::string& configured) {
    if (!configured.empty() && GetFileAttributesW(utf8ToWide(configured).c_str()) != INVALID_FILE_ATTRIBUTES)
        return configured;
    wchar_t path[MAX_PATH];
    if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, path, nullptr)) return wideToUtf8(path);
    std::wstring exe = utf8ToWide(exePath());
    std::wstring dir = exe.substr(0, exe.find_last_of(L"\\/") + 1);
    std::vector<std::wstring> candidates = {dir + L"adb.exe", dir + L"platform-tools\\adb.exe"};
    wchar_t env[MAX_PATH];
    if (GetEnvironmentVariableW(L"ANDROID_HOME", env, MAX_PATH))
        candidates.push_back(std::wstring(env) + L"\\platform-tools\\adb.exe");
    if (GetEnvironmentVariableW(L"ANDROID_SDK_ROOT", env, MAX_PATH))
        candidates.push_back(std::wstring(env) + L"\\platform-tools\\adb.exe");
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", env, MAX_PATH))
        candidates.push_back(std::wstring(env) + L"\\Android\\Sdk\\platform-tools\\adb.exe");
    for (auto& c : candidates)
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) return wideToUtf8(c);
    return {};
}

bool acquireSingleInstance() {
    HANDLE m = CreateMutexW(nullptr, TRUE, L"Local\\MirRoarServer");
    return m && GetLastError() != ERROR_ALREADY_EXISTS;
}

void showMessage(const std::string& title, const std::string& text) {
    MessageBoxW(nullptr, utf8ToWide(text).c_str(), utf8ToWide(title).c_str(), MB_OK | MB_ICONINFORMATION);
}

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool autostartEnabled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    bool found = RegQueryValueExW(k, L"MirRoar", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return found;
}

void setAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring cmd = L"\"" + utf8ToWide(exePath()) + L"\" --minimized";
        RegSetValueExW(k, L"MirRoar", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"MirRoar");
    }
    RegCloseKey(k);
}
