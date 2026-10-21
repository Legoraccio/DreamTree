#include "app.h"
#include <objbase.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;

    if (HandOffToRunningInstance()) {  // Mangrove is open already: it takes the folders and comes to the front
        CoUninitialize();
        return 0;
    }
    static App app;
    g_app = &app;
    if (!app.Create(inst, show)) return 1;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_app = nullptr;
    CoUninitialize();
    // Worker threads may still be running git; ending the process is the fastest clean shutdown.
    ExitProcess(0);
}
