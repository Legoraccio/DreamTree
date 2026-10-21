// Main window, tabs, the navigation pane model, input handling, menus and persistence. Each tab is a
// RepoSession of the shared core (loading, refresh, git operations); this file connects it to the window.

#include "app.h"
#include "theme.h"
#include <shobjidl.h>
#include <shellapi.h>
#include <shlwapi.h>

float g_S = 1.f;
App* g_app = nullptr;

// Results of background work reach the UI thread as posted messages (see WM_APP_RUN).
static void PostToWindow(std::function<void()> fn) {
    HWND h = g_app ? g_app->hwnd : nullptr;
    if (!h) return;
    auto* p = new std::function<void()>(std::move(fn));
    if (!PostMessageW(h, WM_APP_RUN, 0, (LPARAM)p)) delete p;
}

bool Scroller::Step(float dt) {
    const double d = target - pos;
    if (std::fabs(d) < 0.5) { pos = target; return false; }
    pos += d * (1.0 - std::exp(-dt / 55.0));
    return true;
}

bool ScrollThumb(const RectF& area, const Scroller& s, RectF& thumb) {
    if (s.content <= s.view + 0.5 || area.h <= 0) return false;
    const float th = std::max(DR(32), (float)(area.h * s.view / s.content));
    const float ty = area.y + (area.h - th) * (s.Max() > 0 ? (float)(s.pos / s.Max()) : 0.f);
    thumb = { area.r() - DR(12), ty, DR(12), th };
    return true;
}

static LRESULT CALLBACK WndProcThunk(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE && g_app) g_app->hwnd = h;
    if (!g_app) return DefWindowProcW(h, m, w, l);
    // g_S is the scale of the window handling the message: the settings window has its own, and each window
    // may handle messages of the other while it waits (menus, dialogs).
    const float saved = g_S;
    g_S = g_app->MainScale();
    const LRESULT r = g_app->Proc(h, m, w, l);
    g_S = saved;
    return r;
}

// Native popup menus (rounded Windows 11 menus) follow the theme through uxtheme's app mode: forced dark
// or light, or following Windows.
static void SetMenuMode(HWND h, bool dark, bool followSystem) {
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using AllowDarkModeForWindowFn = bool(WINAPI*)(HWND, bool);
    using FlushMenuThemesFn = void(WINAPI*)();
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    auto setMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
    auto allow = reinterpret_cast<AllowDarkModeForWindowFn>(GetProcAddress(ux, MAKEINTRESOURCEA(133)));
    auto flush = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
    if (setMode) setMode(followSystem ? 1 /* AllowDark */ : dark ? 2 /* ForceDark */ : 3 /* ForceLight */);
    if (allow) allow(h, dark);
    if (flush) flush();
}

void ThemeWindowFrame(HWND h, th::Theme theme) {
    const bool dark = th::IsDark(), contrast = th::IsHighContrast();
    const BOOL darkFrame = dark;
    DwmSetWindowAttribute(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkFrame, sizeof darkFrame);
    // The Mangrove theme has its own navy border; the Windows themes leave it to Windows (accent color
    // when "Show accent color on title bars and window borders" is on).
    const COLORREF border = theme == th::Theme::Mangrove && !contrast ? RGB(0x26, 0x36, 0x5E) : DWMWA_COLOR_DEFAULT;
    DwmSetWindowAttribute(h, DWMWA_BORDER_COLOR, &border, sizeof border);
    SetMenuMode(h, dark, theme == th::Theme::System || contrast);
}

void App::ApplyTheme(bool force) {
    th::Apply(theme);
    const Color& a = th::Accent;
    wchar_t sig[96];
    swprintf_s(sig, L"%d %d %d %.3f %.3f %.3f", (int)theme, th::IsDark(), th::IsHighContrast(), a.r, a.g, a.b);
    if (!force && themeApplied == sig) return;  // a repeated notification
    themeApplied = sig;
    if (!hwnd) return;
    ThemeWindowFrame(hwnd, theme);
    gfx.ClearGradients();
    Invalidate();
    RefreshSettings(true);
}

static std::wstring PickFolder(HWND owner) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return L"";
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Open a Git repository");
    dlg->SetOkButtonLabel(L"Open repository");
    if (FAILED(dlg->Show(owner))) return L"";
    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return L"";
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = p; CoTaskMemFree(p); }
    return out;
}

// ============================================================================ window

bool App::Create(HINSTANCE inst, int show) {
    SetMainDispatcher(PostToWindow);
    LoadSettings();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"MangroveWindow";
    RegisterClassExW(&wc);

    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const UINT sysDpi = GetDpiForSystem();
    int ww = std::min((int)((wa.right - wa.left) * 0.84), MulDiv(2200, (int)sysDpi, 96));
    int wh = std::min((int)((wa.bottom - wa.top) * 0.88), MulDiv(1300, (int)sysDpi, 96));
    int x = wa.left + ((wa.right - wa.left) - ww) / 2, y = wa.top + ((wa.bottom - wa.top) - wh) / 2;

    hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_ACCEPTFILES, wc.lpszClassName, L"Mangrove", WS_OVERLAPPEDWINDOW,
                           x, y, ww, wh, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return false;
    MarkInstanceWindow(hwnd);
    dpi = GetDpiForWindow(hwnd);

    // Windows 11 look: Mica Alt backdrop (tabbed title bar), rounded corners; light or dark frame by theme.
    DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_TABBEDWINDOW;
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof backdrop);
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof corner);
    MARGINS margins{ -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    ApplyTheme(true);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    g_S = dpi / 96.f * zoom;
    gfx.scale = g_S;
    RECT cr{};
    GetClientRect(hwnd, &cr);
    if (!gfx.Init(hwnd, (UINT)std::max<LONG>(1, cr.right), (UINT)std::max<LONG>(1, cr.bottom))) {
        MessageBoxW(hwnd, L"Direct2D / DirectComposition initialization failed.", L"Mangrove", MB_ICONERROR);
        return false;
    }

    // Tabs: restore the previous session, then command-line folders.
    if (AppSettings().restoreTabs)
        for (auto& p : AppSettings().tabs) tabs.push_back(MakeTab(p));
    active = std::clamp(AppSettings().activeTab, 0, std::max(0, (int)tabs.size() - 1));
    for (const std::wstring& p : CommandLineFolders()) {
        bool found = false;
        for (size_t k = 0; k < tabs.size(); ++k)
            if (ToLower(tabs[k]->path) == ToLower(p)) { active = (int)k; found = true; }
        if (!found) {
            tabs.push_back(MakeTab(p));
            active = (int)tabs.size() - 1;
        }
    }
    if (tabs.empty()) {
        tabs.push_back(MakeTab(L""));
        active = 0;
    }
    if (auto t = ActivePtr(); t && !t->IsStart()) t->StartLoad(false);
    std::thread([] { GitVersion(); RunOnMain([] { g_app->Invalidate(); }); }).detach();

    bool maximized = false;
    if (hasPlacement) {
        WINDOWPLACEMENT wp = savedPlacement;
        wp.length = sizeof(wp);
        RECT r = wp.rcNormalPosition;
        if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL) && r.right - r.left > 300 && r.bottom - r.top > 200) {
            maximized = wp.showCmd == SW_SHOWMAXIMIZED;
            wp.showCmd = SW_HIDE;
            SetWindowPlacement(hwnd, &wp);
        }
    }
    ShowWindow(hwnd, maximized ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(hwnd);
    SetTimer(hwnd, 4, 60000, nullptr);  // relative times ("fetched 3 minutes ago") move on
    StartSnapshots();                   // MANGROVE_SNAPSHOT: pictures for the interface test
    return true;
}

void App::UpdateScale() {
    g_S = (dpi ? dpi : 96) / 96.f * zoom;
    gfx.SetScale(g_S);
    for (auto& t : tabs) t->sideDirty = true;
}

void App::SetZoom(float z) {
    zoom = std::clamp(std::round(z * 10.f) / 10.f, 0.7f, 2.0f);
    UpdateScale();
    Invalidate();
    SaveSettings();
}

void App::UpdateCaption() {
    RECT r{};
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CAPTION_BUTTON_BOUNDS, &r, sizeof r)) && r.right > r.left) {
        RECT wr{};
        GetWindowRect(hwnd, &wr);
        POINT o{ 0, 0 };
        ClientToScreen(hwnd, &o);
        float dx = (float)(wr.left - o.x), dy = (float)(wr.top - o.y);
        caption = { r.left + dx, r.top + dy, (float)(r.right - r.left), (float)(r.bottom - r.top) };
    } else {
        caption = { (float)gfx.Width() - DR(138), 0, DR(138), DR(32) };
    }
}

LRESULT App::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    // Lets DWM handle the native caption buttons (hover, press, Snap Layouts flyout).
    LRESULT dwmResult = 0;
    if (DwmDefWindowProc(h, m, w, l, &dwmResult)) return dwmResult;

    switch (m) {
    case WM_NCCALCSIZE:
        if (w) {
            // Remove the standard title bar but keep the resize borders on the other sides.
            auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(l);
            const LONG top = p->rgrc[0].top;
            LRESULT r = DefWindowProcW(h, m, w, l);
            p->rgrc[0].top = top;
            if (IsZoomed(h)) {
                UINT d = GetDpiForWindow(h);
                p->rgrc[0].top += GetSystemMetricsForDpi(SM_CYFRAME, d) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, d);
            }
            return r;
        }
        break;
    case WM_NCHITTEST:
        return HitTestNC(l);
    case WM_ACTIVATE:
        windowActive = LOWORD(w) != WA_INACTIVE;
        if (windowActive)
            if (auto t = ActivePtr()) t->CheckForChanges();
        Invalidate();
        break;
    case WM_SIZE: {
        UINT cw = LOWORD(l), ch = HIWORD(l);
        if (cw && ch) {
            gfx.Resize(cw, ch);
            UpdateCaption();
            Paint();
        }
        return 0;
    }
    case WM_DPICHANGED: {
        dpi = HIWORD(w);
        UpdateScale();
        auto* r = reinterpret_cast<RECT*>(l);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        UpdateCaption();
        Invalidate();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(l);
        UINT d = GetDpiForWindow(h);
        if (!d) d = 96;
        mm->ptMinTrackSize = { MulDiv(900, (int)d, 96), MulDiv(540, (int)d, 96) };
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        Paint();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
        OnMouseMove((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l));
        return 0;
    case WM_MOUSELEAVE:
        tracking = false;
        // UI tests move a pointer of their own with posted messages while the real one is elsewhere.
        if (drag == Drag::None && !TestMode()) {
            mx = my = -1;
            UpdateHover();
        }
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(h);
        OnMouseDown((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l), false);
        return 0;
    case WM_LBUTTONDBLCLK:
        OnMouseDown((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l), true);
        return 0;
    case WM_LBUTTONUP:
        OnMouseUp((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l));
        return 0;
    case WM_RBUTTONUP:
        OnRightClick((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l));
        return 0;
    case WM_MBUTTONUP:
        OnMiddleClick((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l));
        return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        ScreenToClient(h, &pt);
        OnWheel((float)GET_WHEEL_DELTA_WPARAM(w), (float)pt.x, (float)pt.y, m == WM_MOUSEHWHEEL);
        return 0;
    }
    case WM_KEYDOWN:
        if (OnKey((UINT)w)) return 0;
        break;
    case WM_SYSKEYDOWN:
        if (OnKey((UINT)w)) return 0;
        break;
    case WM_CHAR:
        OnChar((wchar_t)w);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, CursorForHover()));
            return TRUE;
        }
        break;
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(w);
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t p[MAX_PATH * 4];
            if (!DragQueryFileW(drop, i, p, (UINT)std::size(p))) continue;
            std::wstring s = p;
            if (!DirExists(s)) {
                size_t k = s.find_last_of(L"\\/");
                if (k == std::wstring::npos) continue;
                s.resize(k);
            }
            OpenPath(s);
        }
        DragFinish(drop);
        return 0;
    }
    case WM_COPYDATA: {  // Mangrove started again: its folders open here, and this window comes to the front
        std::vector<std::wstring> folders;
        if (!FoldersFromAnotherInstance(l, folders)) break;
        for (const std::wstring& f : folders) OpenPath(f);
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        SetForegroundWindow(h);
        return TRUE;
    }
    case WM_APP_RUN: {
        auto* fn = reinterpret_cast<std::function<void()>*>(l);
        (*fn)();
        delete fn;
        return 0;
    }
    case WM_TIMER:
        if (SnapshotTimer(w)) return 0;
        if (w == 1) {  // notifications: drop the expired ones
            const double now = NowMs();
            const size_t before = toasts.size();
            toasts.erase(std::remove_if(toasts.begin(), toasts.end(), [&](const Toast& t) { return t.ttl > 0 && now > t.born + t.ttl; }),
                         toasts.end());
            bool timed = false;
            for (const Toast& t : toasts) timed |= t.ttl > 0;
            if (!timed) KillTimer(h, 1);
            if (toasts.size() != before || timed) Invalidate();
        } else {
            if (w == 2) KillTimer(h, 2);  // tooltip or branch list delay elapsed
            Invalidate();                 // 3: caret blink, 4: relative times
        }
        return 0;
    // Windows settings that the Windows themes follow: app mode and accent color, contrast themes.
    case WM_SETTINGCHANGE:
        if (l && CompareStringOrdinal(reinterpret_cast<LPCWSTR>(l), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL) ApplyTheme();
        break;
    case WM_SYSCOLORCHANGE:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        ApplyTheme();
        break;
    case WM_CLOSE:
        SaveSettings();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        EndInstance(h);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT App::HitTestNC(LPARAM l) {
    LRESULT def = DefWindowProcW(hwnd, WM_NCHITTEST, 0, l);
    if (def != HTCLIENT) return def;  // left / right / bottom resize borders
    POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
    ScreenToClient(hwnd, &p);
    if (!IsZoomed(hwnd)) {
        UINT d = GetDpiForWindow(hwnd);
        int frame = std::max(4, (GetSystemMetricsForDpi(SM_CYFRAME, d) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, d)) / 2);
        if (p.y < frame) {
            RECT cr{};
            GetClientRect(hwnd, &cr);
            if (p.x < frame * 3) return HTTOPLEFT;
            if (p.x >= cr.right - frame * 3) return HTTOPRIGHT;
            return HTTOP;
        }
    }
    if ((float)p.y < rcTitle.b()) {
        const Zone* z = ZoneAt((float)p.x, (float)p.y);
        if (z && (z->id == Hit::Tab || z->id == Hit::TabClose || z->id == Hit::TabNew || z->id == Hit::DialogButton)) return HTCLIENT;
        return HTCAPTION;
    }
    return HTCLIENT;
}

const Zone* App::ZoneAt(float x, float y) const {
    for (size_t i = zones.size(); i-- > 0;)
        if (zones[i].r.Contains(x, y)) return &zones[i];
    return nullptr;
}

LPCWSTR App::CursorForHover() const {
    switch (drag) {
    case Drag::Detail: case Drag::ColRef: case Drag::ColGraph:
        return IDC_SIZEWE;
    default:
        break;
    }
    if (drag == Drag::TextSelect) return IDC_IBEAM;
    switch (hover.id) {
    case Hit::SplitDetail: case Hit::ColSep: return IDC_SIZEWE;
    case Hit::HistorySearch: case Hit::CommitField: case Hit::InlineEdit: return IDC_IBEAM;
    default: return IDC_ARROW;
    }
}

bool App::IsTipZone(Hit id) const {
    switch (id) {
    case Hit::ToolButton: case Hit::MoreMenu: case Hit::DiffNav: case Hit::DiffClose: case Hit::DiffHunk: case Hit::DiffLineAction:
    case Hit::DetailSha: case Hit::DetailMore: case Hit::DetailMode: case Hit::DetailAction: case Hit::SearchClear:
        return true;
    case Hit::NavPage: case Hit::SideHeader: case Hit::SettingsButton:
        return navCompact;  // icons only: the tooltip names them
    default:
        return false;
    }
}

std::shared_ptr<RepoTab> App::PtrOf(const RepoTab* t) {
    for (auto& p : tabs) if (p.get() == t) return p;
    return nullptr;
}

// ============================================================================ tabs

std::shared_ptr<RepoTab> App::MakeTab(const std::wstring& path) {
    auto t = std::make_shared<RepoTab>();
    if (!path.empty()) {
        t->path = NormalizePath(path);
        t->title = BaseName(t->path);
    }
    RepoTab* raw = t.get();  // the session calls back only while it is alive
    t->onEvent = [this, raw](SessionEvent e) { OnSessionEvent(*raw, e); };
    return t;
}

void App::NewTab(const std::wstring& path, bool activate) {
    auto t = MakeTab(path);
    tabs.push_back(t);
    if (activate) {
        active = (int)tabs.size() - 1;
        searchFocus = false;
        if (!t->IsStart()) t->StartLoad(false);
    }
    hover = {};
    Invalidate();
    SaveSettings();
}

void App::CloseTab(int i) {
    if (i < 0 || i >= (int)tabs.size()) return;
    tabs.erase(tabs.begin() + i);
    if (tabs.empty()) tabs.push_back(MakeTab(L""));
    if (active > i || active >= (int)tabs.size()) active = std::max(0, active - 1);
    ActivateTab(active);
    SaveSettings();
}

void App::ActivateTab(int i) {
    if (i < 0 || i >= (int)tabs.size()) return;
    active = i;
    searchFocus = false;
    editFocus = 0;
    refPopup = {};
    hover = {};
    auto t = ActivePtr();
    if (t && !t->IsStart()) {
        if (!t->data && !t->loading && t->error.empty()) t->StartLoad(false);
        else if (t->data) t->CheckForChanges();
    }
    Invalidate();
}

void App::OpenPath(const std::wstring& path) {
    std::wstring p = NormalizePath(path);
    for (size_t k = 0; k < tabs.size(); ++k) {
        const auto& t = tabs[k];
        if (ToLower(t->path) == ToLower(p) || (t->data && ToLower(t->data->root) == ToLower(p))) {
            ActivateTab((int)k);
            return;
        }
    }
    auto t = ActivePtr();
    if (t && t->IsStart()) {
        t->path = p;
        t->title = BaseName(p);
        t->StartLoad(false);
        SaveSettings();
        Invalidate();
    } else {
        NewTab(p);
    }
}

void App::OpenDialog() {
    std::wstring p = PickFolder(hwnd);
    if (!p.empty()) OpenPath(p);
}

// ============================================================================ loading

void App::TrustActive() {
    if (auto t = ActivePtr()) t->Trust();
}

// ============================================================================ selection

int32_t App::RowAt(const RepoTab& t, float y) const {
    if (!t.graph || !rcGraphBody.Contains(rcGraphBody.x + 1, y)) return -1;
    int32_t r = (int32_t)std::floor((y - rcGraphBody.y + t.gs.pos) / DR(kGraphRowH));
    return (r >= 0 && r < (int32_t)t.graph->rows.size()) ? r : -1;
}

void App::EnsureRowVisible(RepoTab& t, int32_t row) {
    const float rowH = DR(kGraphRowH);
    const float top = row * rowH, bottom = top + rowH;
    if (top < t.gs.target) t.gs.To(top - (row > 0 ? rowH * 0.5f : 0.f), false);
    else if (bottom > t.gs.target + t.gs.view) t.gs.To(bottom - t.gs.view + rowH * 0.5f, false);
}

void App::SelectRow(RepoTab& t, int32_t row, bool ensureVisible) {
    if (!t.graph || row < 0 || row >= (int32_t)t.graph->rows.size()) return;
    t.SelectRow(row);  // also closes the diff: picking a commit brings the history back
    t.ds.To(0, true);
    if (ensureVisible) EnsureRowVisible(t, row);
    Invalidate();
}

void App::SelectCommit(RepoTab& t, int32_t commit) {
    if (!t.graph || commit < 0 || commit >= (int32_t)t.graph->rowOfCommit.size()) return;
    const int32_t row = t.graph->rowOfCommit[(size_t)commit];
    SelectRow(t, row, false);
    const float rowH = DR(kGraphRowH);
    const float top = row * rowH;
    if (top < t.gs.target || top + rowH > t.gs.target + t.gs.view) t.gs.To(top - t.gs.view * 0.4f, false);
}

// ============================================================================ navigation pane model

void App::BuildSide(RepoTab& t) {
    t.side.clear();
    t.sideDirty = false;
    t.sideCompact = navCompact;
    if (!t.data) return;
    const RepoData& d = *t.data;
    const bool compact = navCompact;  // icons only: pages and sections, no lists
    float top = 0;
    // h: height of the item, pitch: distance to the next one (DIP)
    auto push = [&](SideItem it, float h, float pitch) {
        it.top = top;
        it.h = h;
        top += pitch;
        t.side.push_back(std::move(it));
    };
    auto expanded = [&](const std::wstring& key) { return !compact && !t.collapsed.count(key); };

    for (int page = 0; page < 2; ++page) {
        SideItem it;
        it.kind = SideKind::Page;
        it.section = (uint8_t)page;
        it.text = page == 0 ? L"Changes" : L"History";
        push(std::move(it), 36, 38);
    }

    // The branches, tags or remote branches below a node of the shared tree (folders by '/').
    std::function<void(const TreeNode&, int, const std::wstring&, SideKind, int)> emit =
        [&](const TreeNode& node, int depth, const std::wstring& keyBase, SideKind leaf, int section) {
            for (const TreeNode& c : node.children) {
                SideItem it;
                it.depth = (uint8_t)std::min(depth, 20);
                it.section = (uint8_t)section;
                it.text = c.name;
                if (c.IsFolder()) {
                    it.kind = SideKind::Folder;
                    it.key = keyBase + L"/" + c.name;
                    it.count = c.count;
                    it.expanded = expanded(it.key);
                    const bool open = it.expanded;
                    const std::wstring key = it.key;
                    push(std::move(it), 32, 32);
                    if (open) emit(c, depth + 1, key, leaf, section);
                } else {
                    it.kind = leaf;
                    it.index = c.index;
                    if (leaf == SideKind::Branch) it.head = d.refs[(size_t)c.index].head;
                    push(std::move(it), 32, 32);
                }
            }
        };
    auto header = [&](int section, const wchar_t* name, int count, const wchar_t* key) {
        SideItem it;
        it.kind = SideKind::Header;
        it.section = (uint8_t)section;
        it.text = name;
        it.count = count;
        it.key = key;
        it.expanded = expanded(key);
        const bool open = it.expanded;
        push(std::move(it), 36, 38);
        return open;
    };

    const TreeNode local = LocalBranchTree(d);
    if (header(0, L"Branches", local.count, L"L")) emit(local, 0, L"L", SideKind::Branch, 0);

    const TreeNode tags = TagTree(d);
    if (header(3, L"Tags", tags.count, L"T")) emit(tags, 0, L"T", SideKind::Tag, 3);

    if (header(2, L"Stashes", (int)d.stashes.size(), L"S")) {
        for (size_t i = 0; i < d.stashes.size(); ++i) {
            SideItem it;
            it.kind = SideKind::Stash;
            it.section = 2;
            it.index = (int32_t)i;
            it.text = d.stashes[i].message;
            it.sub = d.stashes[i].name;
            push(std::move(it), 32, 32);
        }
    }

    const TreeNode remotes = RemoteTree(d);
    if (header(1, L"Remotes", (int)remotes.children.size(), L"R")) {
        for (const TreeNode& r : remotes.children) {
            SideItem it;
            it.kind = SideKind::Remote;
            it.section = 1;
            it.text = r.name;
            it.key = L"R/" + r.name;
            it.count = r.count;
            it.expanded = expanded(it.key);
            const bool open = it.expanded;
            const std::wstring key = it.key;
            push(std::move(it), 32, 32);
            if (open) emit(r, 1, key, SideKind::Branch, 1);
        }
    }

    if (!d.submodules.empty() && header(4, L"Submodules", (int)d.submodules.size(), L"M")) {
        for (size_t i = 0; i < d.submodules.size(); ++i) {
            SideItem it;
            it.kind = SideKind::Submodule;
            it.section = 4;
            it.index = (int32_t)i;
            it.text = d.submodules[i];
            push(std::move(it), 32, 32);
        }
    }
    if (compact) return;

    const std::vector<int> recentRefs = RecentBranches(d, 4);
    if (recentRefs.empty()) return;
    {
        SideItem it;
        it.kind = SideKind::Divider;
        push(std::move(it), 21, 21);
        SideItem c;
        c.kind = SideKind::Caption;
        c.text = L"Recent branches";
        push(std::move(c), 26, 26);
    }
    for (int ri : recentRefs) {
        SideItem it;
        it.kind = SideKind::Recent;
        it.index = ri;
        it.head = d.refs[(size_t)ri].head;
        it.text = d.refs[(size_t)ri].name;
        push(std::move(it), 32, 34);
    }
}

void App::SideClick(RepoTab& t, int index) {
    if (index < 0 || index >= (int)t.side.size() || !t.data) return;
    const SideItem& it = t.side[(size_t)index];
    switch (it.kind) {
    case SideKind::Header:
    case SideKind::Folder:
    case SideKind::Remote:
        if (t.collapsed.count(it.key)) t.collapsed.erase(it.key);
        else t.collapsed.insert(it.key);
        t.sideDirty = true;
        break;
    case SideKind::Branch:
    case SideKind::Tag:
    case SideKind::Recent:
        if (it.index >= 0) SelectCommit(t, t.data->refs[(size_t)it.index].commit);
        break;
    case SideKind::Stash:
        if (it.index >= 0) SelectCommit(t, t.data->stashes[(size_t)it.index].commit);
        break;
    default:
        break;
    }
    Invalidate();
}

// ============================================================================ file rows (details panel)

void App::BuildFileRows(RepoTab& t, int section, const std::vector<FileChange>& files, std::vector<FileRow>& out) {
    out.clear();
    if (!t.treeMode) {
        out.reserve(files.size());
        for (size_t i = 0; i < files.size(); ++i) {
            const std::wstring& p = files[i].path;
            const size_t s = p.find_last_of(L'/');
            FileRow r;
            r.section = (uint8_t)section;
            r.file = (int32_t)i;
            r.name = s == std::wstring::npos ? p : p.substr(s + 1);
            r.dir = s == std::wstring::npos ? L"" : p.substr(0, s + 1);
            r.full = p;
            out.push_back(std::move(r));
        }
        return;
    }
    // The shared tree of folders (single-child chains joined, "src/app/ui"), flattened by what is expanded.
    const TreeNode root = FileTree(files);
    const std::wstring prefix = std::to_wstring(section) + L":";
    std::function<void(const TreeNode&, int, const std::wstring&)> emit = [&](const TreeNode& node, int depth, const std::wstring& base) {
        for (const TreeNode& c : node.children) {
            FileRow r;
            r.section = (uint8_t)section;
            r.depth = (uint8_t)std::min(depth, 40);
            r.name = c.name;
            if (c.IsFolder()) {
                r.folder = true;
                r.key = prefix + base + c.name;
                r.full = base + c.name;
                r.expanded = !t.collapsedDirs.count(r.key);
                const bool open = r.expanded;
                out.push_back(r);
                if (open) emit(c, depth + 1, base + c.name + L"/");
            } else {
                r.file = c.index;
                r.full = files[(size_t)c.index].path;
                out.push_back(std::move(r));
            }
        }
    };
    emit(root, 0, L"");
}

// ============================================================================ input

void App::OnMouseMove(float x, float y) {
    mx = x;
    my = y;
    if (!tracking) {
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        tracking = true;
    }
    if (drag == Drag::None) {
        UpdateHover();
        return;
    }
    RepoTab* t = Active();
    const float dx = (x - dragStart) / g_S;
    switch (drag) {
    case Drag::Detail: detailW = std::clamp(dragOrig - dx, 280.f, 760.f); break;
    case Drag::ColRef: colRef = std::clamp(dragOrig + dx, 60.f, 520.f); break;
    case Drag::ColGraph: if (t) t->graphW = std::clamp(dragOrig + dx, 28.f, 1600.f); break;
    case Drag::TextSelect:
        if (TextEdit* e = EditById(dragEdit)) e->caret = e->HitTest(x, y);
        break;
    case Drag::DiffSelect:
        if (t) DragDiffSelection(*t, x, y);
        break;
    case Drag::ScrollGraph:
    case Drag::ScrollSide:
    case Drag::ScrollDetail:
    case Drag::ScrollDiff:
        if (t) {
            Scroller& s = drag == Drag::ScrollGraph ? t->gs
                          : drag == Drag::ScrollSide ? t->ss
                          : drag == Drag::ScrollDiff ? t->diffUi.vs
                                                     : t->ds;
            RectF th;
            if (ScrollThumb(dragArea, s, th)) {
                float track = dragArea.h - th.h;
                double v = track > 0 ? (y - dragGrab - dragArea.y) / track * s.Max() : 0.0;
                s.To(v, true);
            }
        }
        break;
    default:
        break;
    }
    Invalidate();
}

void App::UpdateHover() {
    const Zone* z = (mx >= 0) ? ZoneAt(mx, my) : nullptr;
    Zone nz = z ? *z : Zone{};
    bool changed = nz.id != hover.id || nz.a != hover.a || nz.b != hover.b;
    if (RepoTab* t = Active()) {
        const int32_t row = (z && z->id == Hit::GraphBody) ? RowAt(*t, my)
                            : (z && (z->id == Hit::GraphLabel || z->id == Hit::ReviewCommit)) ? (z->id == Hit::GraphLabel ? z->a : 0)
                                                                                                : -1;
        if (row != t->hoverRow) { t->hoverRow = row; changed = true; }
    }
    // The list of a chip's branches and tags opens after a short hover and stays while the pointer is on it.
    if (z && z->id == Hit::GraphLabel) {
        if (refPopup.row != z->a || refPopup.commit != z->b) {
            refPopup.row = z->a;
            refPopup.commit = z->b;
            refPopup.since = NowMs();
            SetTimer(hwnd, 2, 330, nullptr);
            changed = true;
        }
    } else if (!(z && (z->id == Hit::RefPopup || z->id == Hit::RefPopupRow)) && refPopup.row >= 0) {
        refPopup.row = -1;
        changed = true;
    }
    if (changed) {
        const bool sameZone = nz.id == hover.id && nz.a == hover.a;
        hover = nz;
        if (!sameZone) {
            hoverSince = NowMs();
            if (IsTipZone(nz.id)) SetTimer(hwnd, 2, 520, nullptr);
        }
        Invalidate();
    }
}

void App::OnMouseDown(float x, float y, bool dbl) {
    const Zone* zp = ZoneAt(x, y);
    const Zone z = zp ? *zp : Zone{};
    auto tp = ActivePtr();
    RepoTab* t = tp.get();
    if (dialog.active) {  // modal: only the dialog buttons respond
        if (z.id == Hit::DialogButton) CloseDialog(z.a);
        return;
    }
    if (inlineInput.active && z.id != Hit::InlineEdit) CancelBranchInput();
    if (z.id != Hit::HistorySearch && z.id != Hit::SearchClear) searchFocus = false;
    if (z.id != Hit::CommitField && z.id != Hit::CommitButton && z.id != Hit::CommitAmend) editFocus = 0;
    auto beginDrag = [&](Drag d, float orig) {
        drag = d;
        dragStart = x;
        dragOrig = orig;
        SetCapture(hwnd);
    };
    // Click in a text field: place the caret (Shift extends the selection, double-click selects a word).
    auto beginTextSelect = [&](int id) {
        TextEdit* e = EditById(id);
        if (!e) return;
        const size_t i = e->HitTest(x, y);
        if (dbl) {
            e->SelectWordAt(i);
            return;
        }
        e->caret = i;
        if (GetKeyState(VK_SHIFT) >= 0) e->anchor = i;
        drag = Drag::TextSelect;
        dragEdit = id;
        SetCapture(hwnd);
    };
    switch (z.id) {
    case Hit::Tab: ActivateTab(z.a); break;
    case Hit::TabClose: CloseTab(z.a); break;
    case Hit::TabNew: NewTab(L""); break;
    case Hit::RepoMenu: ShowRepoMenu(z.r); break;
    case Hit::BranchMenu: ShowBranchMenu(z.r); break;
    case Hit::ToolButton: ToolAction(z.a); break;
    case Hit::MoreMenu: ShowMoreMenu(z.r); break;
    case Hit::SettingsButton: ShowSettings(); break;
    case Hit::ToastClose:
        if (z.a >= 0 && z.a < (int)toasts.size()) toasts.erase(toasts.begin() + z.a);
        break;
    case Hit::InlineEdit: beginTextSelect(3); break;
    case Hit::CommitField:
        FocusEdit(z.a);
        beginTextSelect(z.a);
        break;
    case Hit::CommitAmend: if (t) ToggleAmend(*t); break;
    case Hit::CommitButton: DoCommit(); break;
    case Hit::WipAction:
        if (t && t->status) {
            if (z.a == kStageAll || z.a == kUnstageAll) {
                t->Stage({}, z.a == kStageAll);
                break;
            }
            const int section = z.a == kUnstageRow ? 2 : 1;
            if (z.b < 0 || z.b >= (int)t->fileRows[section].size()) break;
            const FileRow fr = t->fileRows[section][(size_t)z.b];
            // Look the file up by path: the lists may have been refreshed since the rows were built.
            FileChange change;
            bool found = false;
            for (const FileChange& f : section == 1 ? t->status->unstaged : t->status->staged)
                if (f.path == fr.full) { change = f; found = true; break; }
            if (z.a == kDiscardRow) {
                if (found && !fr.folder) DoDiscard(*t, change);
            } else {
                std::vector<std::wstring> paths{ fr.full };
                if (found && !change.oldPath.empty()) paths.push_back(change.oldPath);  // a rename stages/unstages both paths
                if (fr.folder || found) t->Stage(paths, z.a == kStageRow);
            }
        }
        break;
    case Hit::NavPage:
        if (t && t->graph) {
            if (z.a == 0) {  // Changes
                if (t->graph->hasWip) { SelectRow(*t, 0); t->gs.To(0, false); }
            } else {         // History
                CloseDiff(*t);
                if (t->selWip && t->data && t->data->headCommit >= 0) SelectCommit(*t, t->data->headCommit);
            }
        }
        break;
    case Hit::SideHeader:
        if (t && navCompact && z.a >= 0 && z.a < (int)t->side.size()) ShowSectionMenu(*t, t->side[(size_t)z.a].section, z.r);
        else if (t) SideClick(*t, z.a);
        break;
    case Hit::SideItem:
        if (t) {
            const SideItem* it = z.a >= 0 && z.a < (int)t->side.size() ? &t->side[(size_t)z.a] : nullptr;
            const bool ref = it && (it->kind == SideKind::Branch || it->kind == SideKind::Tag || it->kind == SideKind::Recent);
            if (dbl && ref && it->index >= 0) t->CheckoutRef(it->index);
            else SideClick(*t, z.a);
        }
        break;
    case Hit::SplitDetail: beginDrag(Drag::Detail, detailW); break;
    case Hit::ColSep:
        if (z.a == 0) beginDrag(Drag::ColRef, colRef);
        else if (z.a == 1 && t) beginDrag(Drag::ColGraph, (float)z.b / g_S);
        break;
    case Hit::HistorySearch:
        searchFocus = true;
        editFocus = 0;
        beginTextSelect(4);
        break;
    case Hit::SearchClear:
        if (t) {
            t->search.Set(L"");
            t->SetSearch(L"");
        }
        searchFocus = true;
        break;
    case Hit::ScopeMenu: ShowScopeMenu(z.r); break;
    case Hit::ReviewCommit: if (t) ReviewChanges(*t); break;
    case Hit::GraphBody:
        if (t) {
            int32_t row = RowAt(*t, y);
            if (row >= 0) SelectRow(*t, row, false);
        }
        break;
    case Hit::GraphLabel:
        if (t) {
            SelectRow(*t, z.a, false);
            if (dbl && t->data) {  // double-click a branch chip: check the branch out
                auto it = t->data->labels.find(z.b);
                if (it != t->data->labels.end() && !it->second.empty() && !it->second[0].tag) t->CheckoutRef(it->second[0].ref);
            }
        }
        break;
    case Hit::RefPopupRow:  // double-click a branch or tag of the list: check it out
        if (t && t->data && dbl) {
            auto it = t->data->labels.find(z.b);
            if (it != t->data->labels.end() && z.a >= 0 && z.a < (int)it->second.size()) {
                const int ref = it->second[(size_t)z.a].ref;
                refPopup = {};
                t->CheckoutRef(ref);
            }
        }
        break;
    case Hit::Scroll:
        if (t) {
            Scroller& s = z.a == 0 ? t->gs : z.a == 1 ? t->ss : z.a == 3 ? t->diffUi.vs : t->ds;
            dragArea = scrollArea[z.a];
            RectF th;
            if (ScrollThumb(dragArea, s, th)) {
                if (y < th.y || y > th.b()) {  // jump so the thumb centers on the click
                    float track = dragArea.h - th.h;
                    s.To(track > 0 ? (y - th.h * 0.5f - dragArea.y) / track * s.Max() : 0.f, true);
                    ScrollThumb(dragArea, s, th);
                }
                dragGrab = y - th.y;
                drag = z.a == 0 ? Drag::ScrollGraph : z.a == 1 ? Drag::ScrollSide : z.a == 3 ? Drag::ScrollDiff : Drag::ScrollDetail;
                SetCapture(hwnd);
            }
        }
        break;
    case Hit::DetailMode:
        if (t) { t->treeMode = z.a == 1; t->ds.To(0, true); }
        break;
    case Hit::DetailSection:
        if (t) {
            if (z.a == 1) t->unstagedCollapsed = !t->unstagedCollapsed;
            else t->stagedCollapsed = !t->stagedCollapsed;
        }
        break;
    case Hit::DetailFolder:
        if (t && z.a >= 0 && z.b >= 0 && z.b < (int)t->fileRows[z.a].size()) {
            const std::wstring& key = t->fileRows[z.a][(size_t)z.b].key;
            if (t->collapsedDirs.count(key)) t->collapsedDirs.erase(key);
            else t->collapsedDirs.insert(key);
            t->collapseVer++;
        }
        break;
    case Hit::DetailFile:  // (the first click of a double-click already opened it)
        if (t && !dbl && z.a >= 0 && z.a < 3 && z.b >= 0 && z.b < (int)t->fileRows[z.a].size())
            OpenDiff(*t, z.a, t->fileRows[z.a][(size_t)z.b].full);
        break;
    case Hit::DetailMore:
        if (t && t->SelectedRow() >= 0) ShowCommitMenu(*t, t->SelectedRow(), z.r.r(), z.r.b());
        break;
    case Hit::DetailAction:
        if (t && t->data && !t->selWip && t->selCommit >= 0 && t->selCommit < (int32_t)t->data->commits.size()) {
            const Commit& cm = t->data->commits[(size_t)t->selCommit];
            if (cm.flags & CF_STASH) {
                if (z.a == 0) t->Pop(cm.stash, false);
                else if (z.a == 1) t->Pop(cm.stash, true);
                else DoDropStash(cm.stash);
            } else if (z.a == 0) {
                CheckoutFromDetails(*t);
            } else if (z.a == 1) {
                if (ToolEnabled(t, 4, nullptr)) BeginBranchInput(*t, t->selCommit);
            } else {
                DoRevert(*t, t->selCommit);
            }
        }
        break;
    case Hit::DiffClose: if (t) CloseDiff(*t); break;
    case Hit::DiffMode: SetDiffMode(z.a == 1); break;
    case Hit::DiffNav: if (t) DiffJump(*t, z.a); break;
    case Hit::DiffFileStage:
        if (t && t->diff.open && t->diff.req.section > 0) {
            const DiffRequest r = t->diff.req;
            std::vector<std::wstring> paths{ r.path };
            if (!r.oldPath.empty() && r.oldPath != r.path) paths.push_back(r.oldPath);  // a rename stages both paths
            t->Stage(paths, r.section == 1);
        }
        break;
    case Hit::DiffHunk: if (t) DoHunkAction(*t, z.a, z.b == 1); break;
    case Hit::DiffLine:  // select changed lines: click, Shift+click for a range, Ctrl+click to add or remove, or drag
        if (t) {
            const bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
            SelectDiffLine(*t, z.a, shift, ctrl && !shift);
            if (!shift && !ctrl) {
                drag = Drag::DiffSelect;
                SetCapture(hwnd);
            }
        }
        break;
    case Hit::DiffLineAction:
        if (t && z.a == 0) DoLinesAction(*t, false);
        else if (t && z.a == 1) DoLinesAction(*t, true);
        else if (t && z.a == 2) ClearDiffSelection(*t);
        break;
    case Hit::DiffBody: if (t) ClearDiffSelection(*t); break;
    case Hit::DetailSha:
        if (t && t->data && t->selCommit >= 0) CopyToClipboard(hwnd, t->data->commits[(size_t)t->selCommit].id.Hex());
        break;
    case Hit::DetailParent:
        if (t) SelectCommit(*t, z.b);
        break;
    case Hit::StartOpen: OpenDialog(); break;
    case Hit::StartRecent:
        if (z.a >= 0 && z.a < (int)AppSettings().recent.size()) { std::wstring p = AppSettings().recent[(size_t)z.a]; OpenPath(p); }
        break;
    case Hit::StartRemove:
        if (std::vector<std::wstring>& recent = AppSettings().recent; z.a >= 0 && z.a < (int)recent.size()) {
            recent.erase(recent.begin() + z.a);
            SaveSettings();
        }
        break;
    case Hit::Trust: TrustActive(); break;
    case Hit::Retry: if (tp) tp->StartLoad(false); break;
    case Hit::CloseTab: CloseTab(active); break;
    default: break;
    }
    UpdateHover();
    Invalidate();
}

void App::OnMouseUp(float x, float y) {
    if (drag != Drag::None) {
        bool layout = drag == Drag::Detail || drag == Drag::ColRef;
        drag = Drag::None;
        ReleaseCapture();
        if (layout) SaveSettings();
    }
    OnMouseMove(x, y);
}

void App::OnRightClick(float x, float y) {
    RepoTab* t = Active();
    const Zone* z = ZoneAt(x, y);
    if (!t || !z || dialog.active) return;
    CancelBranchInput();
    if (z->id == Hit::GraphBody || z->id == Hit::GraphLabel || z->id == Hit::ReviewCommit) {
        int32_t row = z->id == Hit::GraphLabel ? z->a : z->id == Hit::ReviewCommit ? 0 : RowAt(*t, y);
        if (row >= 0) {
            SelectRow(*t, row, false);
            refPopup = {};
            Paint();
            ShowCommitMenu(*t, row, x, y);
        }
    } else if (z->id == Hit::RefPopupRow && t->data) {
        auto it = t->data->labels.find(z->b);
        if (it != t->data->labels.end() && z->a >= 0 && z->a < (int)it->second.size()) ShowRefMenu(*t, it->second[(size_t)z->a].ref, x, y);
    } else if (z->id == Hit::SideItem) {
        ShowSideMenu(*t, z->a, x, y);
    } else if (z->id == Hit::DetailFile || z->id == Hit::DetailFolder) {
        ShowFileMenu(*t, z->a, z->b, x, y);
    } else if (z->id == Hit::WipAction && (z->a == kStageRow || z->a == kUnstageRow || z->a == kDiscardRow)) {
        ShowFileMenu(*t, z->a == kUnstageRow ? 2 : 1, z->b, x, y);
    }
}

void App::OnMiddleClick(float x, float y) {
    const Zone* z = ZoneAt(x, y);
    if (z && (z->id == Hit::Tab || z->id == Hit::TabClose)) CloseTab(z->a);
}

void App::OnWheel(float delta, float x, float y, bool horizontal) {
    if (GetKeyState(VK_CONTROL) < 0) {
        if (!horizontal) SetZoom(zoom + (delta > 0 ? 0.1f : -0.1f));
        return;
    }
    RepoTab* t = Active();
    if (!t) return;
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    if (lines == WHEEL_PAGESCROLL) lines = 20;
    const float steps = -delta / 120.f * (float)std::max(1u, lines);
    if (t->diff.open && rcGraph.Contains(x, y) && t->data && t->error.empty()) {
        DiffViewUi& v = t->diffUi;
        // Tilt wheel or Shift+wheel scroll sideways (a tilt to the right has a positive delta).
        if (horizontal) v.hscroll = std::max(0.f, v.hscroll + delta / 120.f * gfx.codeCharW * 8);
        else if (GetKeyState(VK_SHIFT) < 0) v.hscroll = std::max(0.f, v.hscroll + steps * gfx.codeCharW * 3);
        else v.vs.By(steps * DR(20));
        Invalidate();
        return;
    }
    if (horizontal) return;
    if (rcNav.Contains(x, y)) t->ss.By(steps * DR(32));
    else if (rcDetail.Contains(x, y) && detailVisible) t->ds.By(steps * DR(32));
    else if (rcGraph.Contains(x, y)) t->gs.By(steps * DR(40));
    Invalidate();
}

bool App::OnKey(UINT vk) {
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    auto tp = ActivePtr();
    RepoTab* t = tp.get();
    if (dialog.active) {
        if (alt && !ctrl) return false;
        if (vk == VK_ESCAPE) CloseDialog(-1);
        else if (vk == VK_RETURN) CloseDialog(dialog.defButton);
        return true;
    }
    if (ctrl && !alt && vk == VK_OEM_COMMA) {  // Ctrl+, opens the settings, as in most Windows apps
        ShowSettings();
        return true;
    }
    if (inlineInput.active) {
        if (alt && !ctrl) return false;
        if (vk == VK_ESCAPE) CancelBranchInput();
        else if (vk == VK_RETURN) CommitBranchInput();
        else if (inlineInput.edit.OnKey(vk, ctrl, shift, hwnd)) inlineInput.error.clear();
        Invalidate();
        return true;  // typing must not move the graph selection
    }
    if (editFocus && t) {  // commit summary / description
        if (alt && !ctrl) return false;
        TextEdit& e = editFocus == 1 ? t->commitSummary : t->commitBody;
        switch (vk) {
        case VK_ESCAPE: FocusEdit(0); return true;
        case VK_TAB: FocusEdit(editFocus == 1 ? 2 : 1); return true;
        case VK_RETURN:
            if (ctrl) DoCommit();
            else if (editFocus == 1) FocusEdit(2);  // Enter in the summary moves to the description
            else e.Insert(L"\n");
            Invalidate();
            return true;
        default:
            e.OnKey(vk, ctrl, shift, hwnd);
            Invalidate();
            return vk != VK_F5;  // everything else stays in the field (no git undo on Ctrl+Z here)
        }
    }
    if (alt && !ctrl) return false;  // keep Alt+F4, Alt+Space, ...
    if (searchFocus && t) {  // history search: arrows and paging still move through the history
        switch (vk) {
        case VK_ESCAPE:
            if (!t->search.text.empty()) {
                t->search.Set(L"");
                t->SetSearch(L"");
            } else {
                searchFocus = false;
            }
            Invalidate();
            return true;
        case VK_RETURN: SearchStep(*t, shift ? -1 : 1); return true;
        case VK_TAB: searchFocus = false; Invalidate(); return true;
        case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT: case VK_F3: case VK_F5: break;
        default: {
            const std::wstring before = t->search.text;
            if (t->search.OnKey(vk, ctrl, shift, hwnd)) {
                if (t->search.text != before) t->SetSearch(t->search.text);
                Invalidate();
                return true;
            }
            if (!ctrl) return true;  // characters arrive as WM_CHAR
        }
        }
    }
    if (!ctrl && t && t->diff.open && t->data && t->error.empty() && DiffKey(*t, vk)) return true;
    if (ctrl) {
        switch (vk) {
        case 'O': OpenDialog(); return true;
        case 'T': NewTab(L""); return true;
        case 'W': CloseTab(active); return true;
        case VK_TAB:
            if (tabs.size() > 1) ActivateTab((active + (shift ? -1 : 1) + (int)tabs.size()) % (int)tabs.size());
            return true;
        case 'K': sideVisible = !sideVisible; SaveSettings(); Invalidate(); return true;
        case 'J': detailVisible = !detailVisible; SaveSettings(); Invalidate(); return true;
        case VK_OEM_PLUS: case VK_ADD: SetZoom(zoom + 0.1f); return true;
        case VK_OEM_MINUS: case VK_SUBTRACT: SetZoom(zoom - 0.1f); return true;
        case '0': case VK_NUMPAD0: SetZoom(1.f); return true;
        case 'F':
            if (t && t->data && t->error.empty()) {
                CloseDiff(*t);
                editFocus = 0;
                searchFocus = true;
                t->search.SelectAll();
                Invalidate();
            }
            return true;
        case 'R': if (tp) tp->StartLoad(tp->data != nullptr); return true;
        case 'C':
            if (t && t->data && !t->selWip && t->selCommit >= 0)
                CopyToClipboard(hwnd, t->data->commits[(size_t)t->selCommit].id.Hex());
            return true;
        case 'Z':
            if (t && shift) t->Redo();
            else if (t) t->Undo();
            return true;
        case 'Y': if (t) t->Redo(); return true;
        case 'B': if (t && ToolEnabled(t, 4, nullptr)) BeginBranchInput(*t, t->selWip ? -1 : t->selCommit); return true;
        default: break;
        }
    }
    if (vk == VK_F5) {
        if (tp && !tp->IsStart()) tp->StartLoad(tp->data != nullptr);
        return true;
    }
    if (vk == VK_F3 && t) {
        SearchStep(*t, shift ? -1 : 1);
        return true;
    }
    if (!t || !t->graph || t->graph->rows.empty() || t->diff.open) return false;  // the history is hidden behind a diff
    const int32_t n = (int32_t)t->graph->rows.size();
    int32_t row = t->SelectedRow();
    const int32_t page = std::max(1, (int32_t)(t->gs.view / DR(kGraphRowH)) - 1);
    switch (vk) {
    case VK_UP: SelectRow(*t, std::max(0, row - 1)); return true;
    case VK_DOWN: SelectRow(*t, std::min(n - 1, row + 1)); return true;
    case VK_PRIOR: SelectRow(*t, std::max(0, row - page)); return true;
    case VK_NEXT: SelectRow(*t, std::min(n - 1, std::max(0, row) + page)); return true;
    case VK_HOME: SelectRow(*t, 0); return true;
    case VK_END: SelectRow(*t, n - 1); return true;
    default: return false;
    }
}

void App::OnChar(wchar_t ch) {
    if (dialog.active || ch < 32 || ch == 127) return;
    if (inlineInput.active) {
        if (inlineInput.edit.text.size() < 200) {
            const wchar_t c = ch == L' ' ? L'-' : ch;  // spaces become dashes in branch names
            inlineInput.edit.Insert(std::wstring_view(&c, 1));
            inlineInput.error.clear();
            Invalidate();
        }
        return;
    }
    if (editFocus) {
        if (TextEdit* e = EditById(editFocus)) {
            e->Insert(std::wstring_view(&ch, 1));
            Invalidate();
        }
        return;
    }
    RepoTab* t = Active();
    if (!searchFocus || !t) return;
    t->search.Insert(std::wstring_view(&ch, 1));
    t->SetSearch(t->search.text);
    Invalidate();
}

// ============================================================================ history search

void App::SearchStep(RepoTab& t, int dir) {
    const int32_t r = t.FindMatch(dir);
    if (r < 0) return;
    SelectRow(t, r, false);
    const float rowH = DR(kGraphRowH), top = (float)r * rowH;
    if (top < t.gs.target || top + rowH > t.gs.target + t.gs.view) t.gs.To(top - t.gs.view * 0.4f, false);
}

void App::ReviewChanges(RepoTab& t) {
    if (!t.graph || !t.graph->hasWip) return;
    SelectRow(t, 0, false);
    detailVisible = true;
    FocusEdit(1);
    Invalidate();
}

void App::CheckoutFromDetails(RepoTab& t) {
    if (!t.data || t.selWip || t.selCommit < 0) return;
    const int target = t.CheckoutTarget(t.selCommit);  // a branch at the commit, else the commit itself
    if (target >= 0) t.CheckoutRef(target);
    else if (t.selCommit != t.data->headCommit) t.CheckoutCommit(t.selCommit);
}

// ============================================================================ menus & commands

static int Popup(HWND h, HMENU m, float x, float y, UINT align = TPM_LEFTALIGN | TPM_TOPALIGN) {
    POINT pt{ (LONG)x, (LONG)y };
    ClientToScreen(h, &pt);
    return TrackPopupMenuEx(m, TPM_RETURNCMD | align | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, h, nullptr);
}

void App::ShowMoreMenu(const RectF& r) {
    RepoTab* t = Active();
    if (!t || !t->data) return;
    HMENU m = CreatePopupMenu();
    auto item = [&](UINT id, int tool, const std::wstring& label) {
        AppendMenuW(m, MF_STRING | (ToolEnabled(t, tool, nullptr) ? 0 : MF_GRAYED), id, label.c_str());
    };
    item(1, 0, t->undo.empty() ? L"Undo\tCtrl+Z" : L"Undo " + t->undo.back().label + L"\tCtrl+Z");
    item(2, 1, t->redo.empty() ? L"Redo\tCtrl+Y" : L"Redo " + t->redo.back().label + L"\tCtrl+Y");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    item(3, 6, t->data->stashes.empty() ? std::wstring(L"Pop stash") : L"Pop " + t->data->stashes[0].name);
    item(4, 7, L"Open a terminal here");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    const UINT pullOff = ToolEnabled(t, 2, nullptr) ? 0 : MF_GRAYED;
    AppendMenuW(m, MF_STRING | pullOff, 11, L"Pull, fast-forward if possible");
    AppendMenuW(m, MF_STRING | pullOff, 12, L"Pull, fast-forward only");
    AppendMenuW(m, MF_STRING | pullOff, 13, L"Pull with rebase");
    CheckMenuRadioItem(m, 11, 13, 11 + (UINT)AppSettings().pullMode, MF_BYCOMMAND);  // the Pull button's choice
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 20, L"Refresh\tF5");
    AppendMenuW(m, MF_STRING, 21, L"Show in File Explorer");
    AppendMenuW(m, MF_STRING, 22, L"Copy repository path");
    const int cmd = Popup(hwnd, m, r.r(), r.b(), TPM_RIGHTALIGN | TPM_TOPALIGN);
    DestroyMenu(m);
    auto tp = ActivePtr();
    switch (cmd) {
    case 1: if (tp) tp->Undo(); break;
    case 2: if (tp) tp->Redo(); break;
    case 3: ToolAction(6); break;
    case 4: ToolAction(7); break;
    case 11: case 12: case 13:
        AppSettings().pullMode = cmd - 11;  // the last choice becomes the Pull button default
        SaveSettings();
        if (tp) tp->Pull();
        break;
    case 20: if (tp) tp->StartLoad(true); break;
    case 21: if (tp && tp->data) ShellExecuteW(hwnd, L"open", tp->data->root.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case 22: if (tp && tp->data) CopyToClipboard(hwnd, tp->data->root); break;
    default: break;
    }
    Invalidate();
}

void App::ShowScopeMenu(const RectF& r) {
    auto tp = ActivePtr();
    if (!tp || !tp->data) return;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"All branches");
    AppendMenuW(m, MF_STRING, 2, L"Current branch only");
    CheckMenuRadioItem(m, 1, 2, tp->headOnly ? 2 : 1, MF_BYCOMMAND);
    const int cmd = Popup(hwnd, m, r.r(), r.b() + DR(4), TPM_RIGHTALIGN | TPM_TOPALIGN);
    DestroyMenu(m);
    if ((cmd == 1 || cmd == 2) && tp->headOnly != (cmd == 2)) {
        tp->headOnly = cmd == 2;
        tp->StartLoad(true);
    }
    Invalidate();
}

// A section of the navigation pane as a menu, when the pane shows icons only.
void App::ShowSectionMenu(RepoTab& t, int section, const RectF& r) {
    if (!t.data) return;
    const RepoData& d = *t.data;
    HMENU m = CreatePopupMenu();
    std::vector<int32_t> commits;  // command id - 1000 -> commit to select
    auto add = [&](const std::wstring& text, int32_t commit, bool checked) {
        if (commits.size() >= 80) return;
        AppendMenuW(m, MF_STRING | (commit < 0 ? MF_GRAYED : 0) | (checked ? MF_CHECKED : 0), 1000 + commits.size(), text.c_str());
        commits.push_back(commit);
    };
    if (section == 2) {
        for (const StashEntry& st : d.stashes) add(st.name + L"  " + st.message, st.commit, false);
    } else if (section == 4) {
        for (const std::wstring& sm : d.submodules) add(sm, -1, false);
    } else {
        const RefKind kind = section == 0 ? RefKind::Local : section == 1 ? RefKind::Remote : RefKind::Tag;
        std::vector<int> refs;
        for (size_t i = 0; i < d.refs.size(); ++i)
            if (d.refs[i].kind == kind) refs.push_back((int)i);
        std::sort(refs.begin(), refs.end(),
                  [&](int a, int b) { return StrCmpLogicalW(d.refs[(size_t)a].name.c_str(), d.refs[(size_t)b].name.c_str()) < 0; });
        for (int i : refs) add(d.refs[(size_t)i].name, d.refs[(size_t)i].commit, d.refs[(size_t)i].head);
    }
    if (commits.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"None");
    const int cmd = Popup(hwnd, m, r.r(), r.y);
    DestroyMenu(m);
    if (cmd >= 1000 && cmd < 1000 + (int)commits.size()) SelectCommit(t, commits[(size_t)cmd - 1000]);
    Invalidate();
}

void App::ShowRepoMenu(const RectF& r) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Open repository…\tCtrl+O");
    AppendMenuW(m, MF_STRING, 2, L"New tab\tCtrl+T");
    RepoTab* t = Active();
    if (t && t->data) {
        AppendMenuW(m, MF_STRING, 3, L"Refresh\tF5");
        AppendMenuW(m, MF_STRING, 4, L"Show in File Explorer");
        AppendMenuW(m, MF_STRING, 5, L"Copy repository path");
    }
    const std::vector<std::wstring>& recent = AppSettings().recent;
    if (!recent.empty()) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        for (size_t i = 0; i < recent.size() && i < 15; ++i) {
            std::wstring s = BaseName(recent[i]) + L"\t" + recent[i];
            AppendMenuW(m, MF_STRING, 100 + i, s.c_str());
        }
    }
    int cmd = Popup(hwnd, m, r.x, r.b());
    DestroyMenu(m);
    auto tp = ActivePtr();
    switch (cmd) {
    case 1: OpenDialog(); break;
    case 2: NewTab(L""); break;
    case 3: if (tp) tp->StartLoad(true); break;
    case 4: if (t && t->data) ShellExecuteW(hwnd, L"open", t->data->root.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case 5: if (t && t->data) CopyToClipboard(hwnd, t->data->root); break;
    default:
        if (cmd >= 100 && cmd < 100 + (int)recent.size()) { std::wstring p = recent[(size_t)cmd - 100]; OpenPath(p); }
        break;
    }
    Invalidate();
}

void App::ShowBranchMenu(const RectF& r) {
    RepoTab* t = Active();
    if (!t || !t->data) return;
    const RepoData& d = *t->data;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Check out a branch");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    const UINT busy = t->opBusy ? MF_GRAYED : 0;
    int count = 0;
    for (size_t i = 0; i < d.refs.size() && count < 60; ++i) {
        if (d.refs[i].kind != RefKind::Local) continue;
        AppendMenuW(m, MF_STRING | busy | (d.refs[i].head ? MF_CHECKED : 0), 1000 + i, d.refs[i].name.c_str());
        ++count;
    }
    if (!count) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"No local branches");
    int cmd = Popup(hwnd, m, r.x, r.b());
    DestroyMenu(m);
    if (cmd >= 1000 && cmd < 1000 + (int)d.refs.size()) {
        const int ri = cmd - 1000;
        SelectCommit(*t, d.refs[(size_t)ri].commit);
        t->CheckoutRef(ri);
    }
    Invalidate();
}

void App::ShowFileMenu(RepoTab& t, int section, int row, float x, float y) {
    if (!t.data || section < 0 || section > 2 || row < 0 || row >= (int)t.fileRows[section].size()) return;
    const FileRow fr = t.fileRows[section][(size_t)row];
    FileChange change;  // copied: the lists can be refreshed while the menu is open
    bool found = false;
    if (t.status && section > 0)
        for (const FileChange& f : section == 1 ? t.status->unstaged : t.status->staged)
            if (f.path == fr.full) { change = f; found = true; break; }
    HMENU m = CreatePopupMenu();
    if (section == 1) {
        AppendMenuW(m, MF_STRING, 1, fr.folder ? L"Stage folder" : L"Stage file");
        if (!fr.folder && found && change.status != L'U') AppendMenuW(m, MF_STRING, 2, L"Discard changes…");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    } else if (section == 2) {
        AppendMenuW(m, MF_STRING, 3, fr.folder ? L"Unstage folder" : L"Unstage file");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(m, MF_STRING, 4, L"Copy path");
    AppendMenuW(m, MF_STRING, 5, L"Show in File Explorer");
    const int cmd = Popup(hwnd, m, x, y);
    DestroyMenu(m);
    switch (cmd) {
    case 1:
    case 3: {
        std::vector<std::wstring> paths{ fr.full };
        if (found && !change.oldPath.empty()) paths.push_back(change.oldPath);
        t.Stage(paths, cmd == 1);
        break;
    }
    case 2: if (found) DoDiscard(t, change); break;
    case 4: CopyToClipboard(hwnd, fr.full); break;
    case 5: {
        std::wstring native = t.data->root + L"\\" + fr.full;
        for (auto& c : native) if (c == L'/') c = L'\\';
        if (GetFileAttributesW(native.c_str()) != INVALID_FILE_ATTRIBUTES) {
            const std::wstring args = L"/select,\"" + native + L"\"";
            ShellExecuteW(hwnd, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        } else {  // deleted file: open the folder that contained it
            const size_t s = native.find_last_of(L'\\');
            ShellExecuteW(hwnd, L"open", native.substr(0, s).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        break;
    }
    default: break;
    }
    Invalidate();
}

void App::ShowRefMenu(RepoTab& t, int ri, float x, float y) {
    if (!t.data || ri < 0 || ri >= (int)t.data->refs.size()) return;
    const RepoData& d = *t.data;
    const Ref r = d.refs[(size_t)ri];  // copied: the data can be reloaded while the menu is open
    const UINT busy = t.opBusy ? MF_GRAYED : 0;
    HMENU m = CreatePopupMenu();
    if (r.kind == RefKind::Remote) {
        bool hasLocal = false;
        for (const Ref& x2 : d.refs) hasLocal |= x2.kind == RefKind::Local && x2.name == r.branch;
        const std::wstring label = hasLocal ? L"Check out " + r.branch : L"Check out as new local branch '" + r.branch + L"'";
        AppendMenuW(m, MF_STRING | busy, 1, label.c_str());
    } else if (r.kind == RefKind::Tag) {
        AppendMenuW(m, MF_STRING | busy, 1, L"Check out (detached HEAD)");
    } else {
        AppendMenuW(m, MF_STRING | busy | (r.head ? MF_GRAYED : 0), 1, L"Check out");
    }
    AppendMenuW(m, MF_STRING | busy | (r.commit < 0 ? MF_GRAYED : 0), 2, L"Create branch here\u2026");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 3, r.kind == RefKind::Tag ? L"Copy tag name" : L"Copy branch name");
    const int cmd = Popup(hwnd, m, x, y);
    DestroyMenu(m);
    switch (cmd) {
    case 1: t.CheckoutRef(ri); break;
    case 2:
        SelectCommit(t, r.commit);
        BeginBranchInput(t, r.commit);
        break;
    case 3: CopyToClipboard(hwnd, r.name); break;
    default: break;
    }
    Invalidate();
}

void App::ShowSideMenu(RepoTab& t, int index, float x, float y) {
    if (!t.data || index < 0 || index >= (int)t.side.size()) return;
    const SideItem it = t.side[(size_t)index];
    if ((it.kind == SideKind::Branch || it.kind == SideKind::Tag || it.kind == SideKind::Recent) && it.index >= 0) {
        ShowRefMenu(t, it.index, x, y);
        return;
    }
    if (it.kind != SideKind::Stash || it.index < 0) return;
    const UINT busy = t.opBusy ? MF_GRAYED : 0;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | busy, 4, L"Apply stash");
    AppendMenuW(m, MF_STRING | busy, 5, L"Pop stash");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | busy, 6, L"Delete stash\u2026");
    const int cmd = Popup(hwnd, m, x, y);
    DestroyMenu(m);
    switch (cmd) {
    case 4: t.Pop(it.index, true); break;
    case 5: t.Pop(it.index, false); break;
    case 6: DoDropStash(it.index); break;
    default: break;
    }
    Invalidate();
}

void App::ShowCommitMenu(RepoTab& t, int32_t row, float x, float y) {
    if (!t.data || !t.graph) return;
    const int32_t c = t.graph->rows[(size_t)row].commit;
    const UINT busy = t.opBusy ? MF_GRAYED : 0;
    if (c < 0) {  // WIP row
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING | (t.graph && t.graph->hasWip ? 0 : MF_GRAYED), 3, L"Review and commit");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (ToolEnabled(&t, 5, nullptr) ? 0 : MF_GRAYED), 1, L"Stash all changes");
        AppendMenuW(m, MF_STRING | (ToolEnabled(&t, 4, nullptr) ? 0 : MF_GRAYED), 2, L"Create branch here…\tCtrl+B");
        const int cmd = Popup(hwnd, m, x, y);
        DestroyMenu(m);
        if (cmd == 1) t.Stash();
        else if (cmd == 2) BeginBranchInput(t, -1);
        else if (cmd == 3) ReviewChanges(t);
        Invalidate();
        return;
    }
    const RepoData& d = *t.data;
    const Commit& cm = d.commits[(size_t)c];
    HMENU m = CreatePopupMenu();
    std::vector<int> checkoutRefs;
    if (auto it = d.labels.find(c); it != d.labels.end()) {
        for (const Label& L : it->second) {
            if (L.tag || checkoutRefs.size() >= 6) continue;
            const Ref& r = d.refs[(size_t)L.ref];
            const std::wstring text = L"Check out " + (r.kind == RefKind::Remote && !L.local ? r.name : r.branch);
            AppendMenuW(m, MF_STRING | busy | (r.head ? MF_GRAYED : 0), 20 + checkoutRefs.size(), text.c_str());
            checkoutRefs.push_back(L.ref);
        }
    }
    AppendMenuW(m, MF_STRING | busy, 10, L"Create branch here…\tCtrl+B");
    AppendMenuW(m, MF_STRING | busy | ((cm.flags & CF_STASH) ? MF_GRAYED : 0), 11, L"Check out this commit (detached HEAD)");
    AppendMenuW(m, MF_STRING | busy | ((cm.flags & CF_STASH) || d.unborn ? MF_GRAYED : 0), 12, L"Revert this commit…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 1, L"Copy commit SHA\tCtrl+C");
    AppendMenuW(m, MF_STRING, 2, L"Copy short SHA");
    AppendMenuW(m, MF_STRING, 3, L"Copy commit message");
    if (cm.parentCount) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, 4, L"Go to parent commit");
    }
    int cmd = Popup(hwnd, m, x, y);
    DestroyMenu(m);
    if (cmd >= 20 && cmd < 20 + (int)checkoutRefs.size()) {
        t.CheckoutRef(checkoutRefs[(size_t)cmd - 20]);
        Invalidate();
        return;
    }
    switch (cmd) {
    case 10: BeginBranchInput(t, c); break;
    case 11: t.CheckoutCommit(c); break;
    case 12: DoRevert(t, c); break;
    case 1: CopyToClipboard(hwnd, cm.id.Hex()); break;
    case 2: CopyToClipboard(hwnd, cm.id.Hex(7)); break;
    case 3: CopyToClipboard(hwnd, CommitMessage(t, c)); break;
    case 4: if (d.Parents(cm)[0] >= 0) SelectCommit(t, d.Parents(cm)[0]); break;
    default: break;
    }
    Invalidate();
}

void App::ToolAction(int id) {
    RepoTab* t = Active();
    if (!ToolEnabled(t, id, nullptr)) return;
    switch (id) {
    case 0: t->Undo(); break;
    case 1: t->Redo(); break;
    case 2: t->Pull(); break;
    case 3: t->Push(false); break;
    case 4: BeginBranchInput(*t, t->selWip ? -1 : t->selCommit); break;
    case 5: t->Stash(); break;
    case 6: t->Pop(0, false); break;
    case 8: t->Fetch(); break;
    case 7: {  // Terminal
        const std::wstring args = L"-d \"" + t->data->root + L"\"";
        if ((INT_PTR)ShellExecuteW(hwnd, L"open", L"wt.exe", args.c_str(), nullptr, SW_SHOWNORMAL) <= 32)
            ShellExecuteW(hwnd, L"open", L"cmd.exe", nullptr, t->data->root.c_str(), SW_SHOWNORMAL);
        break;
    }
    default: break;
    }
}

// ============================================================================ settings

// The shared settings file (recent repositories, trusted folders, pull mode, ...) plus this window's keys.
void App::LoadSettings() {
    Settings& s = AppSettings();
    s.Load();
    zoom = std::clamp(s.GetFloat(L"zoom", 1.f), 0.7f, 2.f);
    detailW = std::clamp(s.GetFloat(L"detail", 360.f), 280.f, 760.f);
    colRef = std::clamp(s.GetFloat(L"colRef", 188.f), 60.f, 520.f);
    diffSplit = s.diffSplit;
    const std::wstring themeName = s.Get(L"theme");
    theme = themeName == L"light"       ? th::Theme::Light
            : themeName == L"dark"      ? th::Theme::Dark
            : themeName == L"mangrove" || themeName == L"dreamtree" ? th::Theme::Mangrove  // its name before Mangrove
                                        : th::Theme::System;
    sideVisible = s.GetBool(L"sideVisible", true);
    detailVisible = s.GetBool(L"detailVisible", true);
    int a[5] = {};
    if (swscanf_s(s.Get(L"placement").c_str(), L"%d,%d,%d,%d,%d", &a[0], &a[1], &a[2], &a[3], &a[4]) == 5) {
        savedPlacement = { sizeof(WINDOWPLACEMENT) };
        savedPlacement.rcNormalPosition = { a[0], a[1], a[2], a[3] };
        savedPlacement.showCmd = a[4] ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        hasPlacement = true;
    }
}

void App::SaveSettings() {
    if (!hwnd) return;
    Settings& s = AppSettings();
    s.SetFloat(L"zoom", zoom);
    s.SetFloat(L"detail", detailW, 0);
    s.SetFloat(L"colRef", colRef, 0);
    s.diffSplit = diffSplit;
    s.Set(L"theme", theme == th::Theme::Light ? L"light" : theme == th::Theme::Dark ? L"dark" : theme == th::Theme::Mangrove ? L"mangrove" : L"system");
    s.SetBool(L"sideVisible", sideVisible);
    s.SetBool(L"detailVisible", detailVisible);
    WINDOWPLACEMENT wp{ sizeof(wp) };
    if (GetWindowPlacement(hwnd, &wp)) {
        const RECT& r = wp.rcNormalPosition;
        wchar_t num[96];
        swprintf_s(num, L"%d,%d,%d,%d,%d", r.left, r.top, r.right, r.bottom, IsZoomed(hwnd) ? 1 : 0);
        s.Set(L"placement", num);
    }
    s.tabs.clear();
    int act = 0;
    for (size_t i = 0; i < tabs.size(); ++i) {
        if (tabs[i]->IsStart()) continue;
        if ((int)i == active) act = (int)s.tabs.size();
        s.tabs.push_back(tabs[i]->path);
    }
    s.activeTab = act;
    s.Save();
    RefreshSettings(false);
}
