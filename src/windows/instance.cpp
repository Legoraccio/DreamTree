// One Mangrove per settings file: starting it again (a shortcut, Explorer, a command line with folders) hands the
// folders to the window already open, which opens them in tabs and comes to the front. Another settings file
// (MANGROVE_SETTINGS: profiles, interface tests) is another instance, side by side.

#include "app.h"
#include <shellapi.h>

namespace {

constexpr ULONG_PTR kOpenFolders = 0x4D475631;  // WM_COPYDATA tag: the folders to open, each ended by a NUL
constexpr wchar_t kInstanceProp[] = L"Mangrove.Instance";
HANDLE g_mutex = nullptr;  // held while the main window lives

uint32_t InstanceId() {
    static const uint32_t id = std::max<uint32_t>(1, Fnv1aW(ToLower(SettingsFile())));
    return id;
}

// The main window of the instance with these settings, if there is one.
HWND FindInstanceWindow() {
    HWND found = nullptr;
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            wchar_t cls[32];
            if (GetClassNameW(h, cls, (int)std::size(cls)) && wcscmp(cls, L"MangroveWindow") == 0 &&
                (uintptr_t)GetPropW(h, kInstanceProp) == InstanceId()) {
                *reinterpret_cast<HWND*>(l) = h;
                return FALSE;
            }
            return TRUE;
        },
        (LPARAM)&found);
    return found;
}

}  // namespace

std::vector<std::wstring> CommandLineFolders() {
    std::vector<std::wstring> out;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            std::wstring p = argv[i];
            if (!DirExists(p)) continue;
            wchar_t full[MAX_PATH * 4];
            if (GetFullPathNameW(p.c_str(), (DWORD)std::size(full), full, nullptr)) p = full;
            out.push_back(NormalizePath(p));
        }
        LocalFree(argv);
    }
    return out;
}

bool HandOffToRunningInstance() {
    wchar_t name[48];
    swprintf_s(name, L"Local\\Mangrove.%08X", InstanceId());
    g_mutex = CreateMutexW(nullptr, FALSE, name);
    if (!g_mutex || GetLastError() != ERROR_ALREADY_EXISTS) return false;  // the first one
    HWND target = nullptr;
    for (int i = 0; i < 100 && !(target = FindInstanceWindow()); ++i) Sleep(50);  // it may still be starting
    if (!target) return false;  // no window after 5 seconds (it is closing): start anyway
    std::wstring folders;
    for (const std::wstring& f : CommandLineFolders()) {
        folders += f;
        folders.push_back(L'\0');
    }
    COPYDATASTRUCT cds{ kOpenFolders, (DWORD)(folders.size() * sizeof(wchar_t)), folders.empty() ? nullptr : folders.data() };
    // This process was started by the user, so it may hand the foreground to the window it wakes up.
    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    AllowSetForegroundWindow(pid);
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(target, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 5000, &ignored);
    return true;
}

void MarkInstanceWindow(HWND h) { SetPropW(h, kInstanceProp, (HANDLE)(uintptr_t)InstanceId()); }

void EndInstance(HWND h) {
    RemovePropW(h, kInstanceProp);
    if (g_mutex) CloseHandle(g_mutex);  // a new start while this process winds down is a new instance
    g_mutex = nullptr;
}

bool FoldersFromAnotherInstance(LPARAM copyData, std::vector<std::wstring>& folders) {
    const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(copyData);
    if (!cds || cds->dwData != kOpenFolders || cds->cbData % sizeof(wchar_t) || cds->cbData > (1u << 20)) return false;
    const std::wstring_view all(static_cast<const wchar_t*>(cds->lpData), cds->lpData ? cds->cbData / sizeof(wchar_t) : 0);
    for (size_t p = 0; p < all.size();) {
        size_t e = all.find(L'\0', p);
        if (e == std::wstring_view::npos) e = all.size();
        const std::wstring f(all.substr(p, e - p));
        if (!f.empty() && DirExists(f)) folders.push_back(f);
        p = e + 1;
    }
    return true;
}
