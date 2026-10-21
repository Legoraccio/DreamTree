// Pictures of the windows for the interface smoke test (tools\uitest\win-snapshots.ps1). With MANGROVE_SNAPSHOT
// set to a folder, Mangrove waits for the repository of its tab to load, then saves what the screen shows of
// the main window and of the pages of the settings window (in the Windows themes and in Mangrove's), writes
// snapshots.log there and quits. The windows stay on top meanwhile: nothing else may cover them.

#include "app.h"
#include <wincodec.h>

namespace {

constexpr UINT_PTR kTimer = 9;
std::wstring g_dir;
int g_step = 0;
double g_start = 0;

void Log(const std::wstring& line) {
    const std::string s = Narrow(line) + "\r\n";
    HANDLE f = CreateFileW((g_dir + L"\\snapshots.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    WriteFile(f, s.data(), (DWORD)s.size(), &n, nullptr);
    CloseHandle(f);
}

bool WritePng(const void* bgrx, int w, int h, const std::wstring& path) {
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    ComPtr<IWICBitmap> bmp;
    if (FAILED(f->CreateBitmapFromMemory((UINT)w, (UINT)h, GUID_WICPixelFormat32bppBGR, (UINT)w * 4, (UINT)(w * h * 4),
                                         (BYTE*)bgrx, &bmp)))
        return false;
    ComPtr<IWICStream> stream;
    if (FAILED(f->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(enc->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetSize((UINT)w, (UINT)h)) || FAILED(frame->SetPixelFormat(&format))) return false;
    if (FAILED(frame->WriteSource(bmp.Get(), nullptr))) return false;
    return SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}

// What the screen shows of a window: the backdrop, the frame and the caption buttons DWM draws included.
bool SaveWindow(HWND h, const wchar_t* name) {
    bool ok = false;
    RECT r{};
    if (h && FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) GetWindowRect(h, &r);
    const int w = r.right - r.left, ht = r.bottom - r.top;
    if (h && w > 0 && ht > 0) {
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -ht;  // top-down rows
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        if (HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0)) {
            HGDIOBJ old = SelectObject(mem, bmp);
            ok = BitBlt(mem, 0, 0, w, ht, screen, r.left, r.top, SRCCOPY | CAPTUREBLT) != FALSE;
            SelectObject(mem, old);
            GdiFlush();
            ok = ok && WritePng(bits, w, ht, g_dir + L"\\" + name);
            DeleteObject(bmp);
        }
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
    }
    Log(std::wstring(name) + (ok ? L": saved" : L": FAILED"));
    return ok;
}

void Next(int step, UINT ms) {
    g_step = step;
    SetTimer(g_app->hwnd, kTimer, ms, nullptr);
}

void SetTheme(th::Theme t) {
    g_app->theme = t;
    g_app->ApplyTheme();
}

void Step() {
    switch (g_step) {
    case 0: {  // the repository of the tab loads (30 s at most)
        RepoTab* t = g_app->Active();
        const bool ready = !t || t->IsStart() || !t->error.empty() || (t->data && t->status && !t->loading);
        if (!ready && NowMs() - g_start < 30000) return Next(0, 250);
        Log(ready ? L"loaded" : L"still loading after 30 s");
        return Next(1, 2000);
    }
    case 1:
        SaveWindow(g_app->hwnd, L"1-main.png");
        ShowSettings(0);
        return Next(2, 1200);
    case 2:
        SaveWindow(SettingsWindowHandle(), L"2-settings-general.png");
        ShowSettings(1);
        return Next(3, 900);
    case 3:
        SaveWindow(SettingsWindowHandle(), L"3-settings-appearance.png");
        ShowSettings(2);
        return Next(4, 1500);  // git --version runs first
    case 4:
        SaveWindow(SettingsWindowHandle(), L"4-settings-git.png");
        ShowSettings(3);
        return Next(5, 900);
    case 5:
        SaveWindow(SettingsWindowHandle(), L"5-settings-integrations.png");
        SettingsForSnapshot(1);
        return Next(6, 900);
    case 6:
        SaveWindow(SettingsWindowHandle(), L"6-settings-token.png");
        SettingsForSnapshot(2);  // signing in with the browser: the code
        return Next(7, 900);
    case 7:
        SaveWindow(SettingsWindowHandle(), L"7-settings-sign-in.png");
        SettingsForSnapshot(0);
        CloseSettings();
        SetTheme(th::Theme::Dark);
        return Next(8, 1200);
    case 8:
        SaveWindow(g_app->hwnd, L"8-main-dark.png");
        ShowSettings(3);
        return Next(9, 1200);
    case 9:
        SaveWindow(SettingsWindowHandle(), L"9-settings-dark.png");
        CloseSettings();
        SetTheme(th::Theme::Mangrove);
        return Next(10, 1200);
    case 10:
        SaveWindow(g_app->hwnd, L"10-main-mangrove.png");
        ShowSettings(0);
        return Next(11, 1200);
    case 11:
        SaveWindow(SettingsWindowHandle(), L"11-settings-mangrove.png");
        CloseSettings();
        SetTheme(th::Theme::System);
        Log(L"done");
        PostMessageW(g_app->hwnd, WM_CLOSE, 0, 0);
        return;
    default:
        return;
    }
}

}  // namespace

void StartSnapshots() {
    g_dir = EnvVar("MANGROVE_SNAPSHOT");
    if (g_dir.empty() || !g_app || !g_app->hwnd) return;
    CreateDirectoryW(g_dir.c_str(), nullptr);
    // On top of everything (the settings window, which it owns, too) while the screen is read.
    SetWindowPos(g_app->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    g_start = NowMs();
    Log(L"snapshots: waiting for the repository");
    Next(0, 250);
}

bool SnapshotTimer(UINT_PTR id) {
    if (id != kTimer || g_dir.empty() || !g_app) return false;
    KillTimer(g_app->hwnd, kTimer);
    Step();
    return true;
}
