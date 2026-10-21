// The Settings window: Mangrove's preferences by category, laid out like the Windows 11 Settings app (a
// navigation pane, pages of cards). General, Appearance and Git, and Integrations: the accounts on GitHub,
// GitLab and Gitea / Forgejo that fetch, pull and push use over HTTPS. An account connects by signing in with
// the browser (github.com: a code to enter on GitHub) or with a token pasted in a box that shows dots and never
// copies it; either way the token is checked with its server, then kept in Windows Credential Manager
// (src\core\accounts.cpp).

#include "app.h"
#include "theme.h"
#include <shellapi.h>

using namespace th;

namespace {

constexpr float kTitleH = 48.f;   // title bar (DIP)
constexpr float kNavW = 280.f;    // navigation pane
constexpr float kHeadH = 72.f;    // the page title, above the cards that scroll
constexpr float kMaxW = 1000.f;   // cards are at most this wide
constexpr float kCardMinH = 68.f;
constexpr UINT_PTR kCaretTimer = 3;
constexpr UINT_PTR kSpinTimer = 5;  // spinners turn on a timer, not frame after frame: an authorization can take minutes

enum Page : int { kGeneral, kAppearance, kGit, kIntegrations, kPages };
const wchar_t* const kPageTitles[kPages] = { L"General", L"Appearance", L"Git", L"Integrations" };
const wchar_t kPageGlyphs[kPages] = { 0xE713, 0xE790, 0, 0xE77B };  // Git has the branch icon

enum class SHit : uint8_t {
    None, Block, Nav, Toggle, Choice, Zoom, TrustRemove, AccountRemove, Service, Field, CreateToken, Connect, Scroll, DialogButton,
    SignIn, OpenCode, CancelSignIn,
};

struct SZone {
    RectF r;
    SHit id = SHit::None;
    int a = 0;
};

enum : int { kReopen, kDetails, kNavIcons };       // toggles
enum : int { kPull, kDiffs, kTheme };              // choices (drop-down lists)
enum : int { kNoField, kServer, kToken };          // text boxes
enum : int { kInfo, kError, kSuccess, kWarning };  // how the token check went
enum : int { kSignInIdle, kSignInStarting, kSignInWaiting };  // signing in with the browser: asking for a code, then for the token

const th::Theme kThemes[] = { th::Theme::System, th::Theme::Light, th::Theme::Dark, th::Theme::Mangrove };
const std::vector<std::wstring> kThemeLabels = { L"Windows (system setting)", L"Windows light", L"Windows dark",
                                                 L"Mangrove (navy and gold)" };
const std::vector<std::wstring> kDiffLabels = { L"Unified", L"Side by side" };
constexpr wchar_t kServerHelp[] = L"Change it for GitHub Enterprise, a self-managed GitLab or another Gitea / Forgejo server.";
constexpr wchar_t kBrowserHelp[] =
    L"Mangrove shows a code to enter on GitHub: once you authorize Mangrove there, the account is connected, with no token to copy.";
constexpr wchar_t kCodeHelp[] = L"Enter this code on GitHub, then authorize Mangrove:";

RectF Intersect(const RectF& a, const RectF& b) {
    const float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    const float x1 = std::min(a.r(), b.r()), y1 = std::min(a.b(), b.b());
    return { x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0) };
}

// Calls into the main window at its own scale: while this window handles a message, g_S is this window's.
template <class Fn>
void OnMain(Fn&& fn) {
    const float s = g_S;
    if (g_app) g_S = g_app->MainScale();
    fn();
    g_S = s;
}

std::wstring AccountTitle(const Account& a) {
    return a.name.empty() || a.name == a.login ? a.login : a.name + L" (" + a.login + L")";
}

bool Blank(const std::wstring& s) { return s.find_first_not_of(L" \t\r\n") == std::wstring::npos; }

// A key combination that a line never breaks inside ("Ctrl+Plus"): word joiners around each "+".
std::wstring Keys(std::wstring_view keys) {
    std::wstring s;
    for (wchar_t c : keys) {
        if (c == L'+') s += L"\u2060+\u2060";
        else s += c;
    }
    return s;
}

class SettingsWindow {
public:
    HWND hwnd = nullptr;
    bool gitChecked = false;  // GitVersion() returned: an empty version means there is no git

    void Show(int p);
    void Close() { if (hwnd) DestroyWindow(hwnd); }
    void Invalidate() const { if (hwnd) InvalidateRect(hwnd, nullptr, FALSE); }
    void ThemeChanged();
    void ForSnapshot(int state);
    void TokenChecked(const TokenCheck& check, const std::string& tok);
    void CodeArrived(const std::wstring& code, const std::wstring& url);
    void SignedIn(const TokenCheck& check, const std::string& tok);
    static LRESULT CALLBACK Thunk(HWND h, UINT m, WPARAM w, LPARAM l);

private:
    struct Icon {
        wchar_t glyph = 0;
        bool branch = false;
        const Author* avatar = nullptr;
    };

    bool Create();
    LRESULT Proc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT HitTestNC(LPARAM l);
    float Scale() const { return (dpi ? dpi : 96) / 96.f; }
    void UpdateCaption();

    // painting
    void Paint();
    void PaintNav();
    void PaintPage(const RectF& view);
    void PaintGeneral(float& y);
    void PaintAppearance(float& y);
    void PaintGit(float& y);
    void PaintIntegrations(float& y);
    void PaintAddAccount(float& y);
    RectF Card(float& y, const Icon& icon, std::wstring_view title, std::wstring_view desc, float ctrlW, float ctrlH,
               const Color* titleColor = nullptr);
    void Section(float& y, std::wstring_view title);
    void Paragraph(float& y, std::wstring_view text);
    void ToggleSwitch(const RectF& area, bool on, int id);
    void ChoiceBox(const RectF& r, std::wstring_view label, int id, bool enabled);
    float ChoiceWidth(const std::vector<std::wstring>& labels) const;
    void Radio(float x, float y, std::wstring_view label, bool on, int index, bool enabled);
    void AddZone(const RectF& r, SHit id, int a = 0);
    bool Hov(SHit id, int a = 0) const { return hover.id == id && hover.a == a; }

    // input
    const SZone* ZoneAt(float x, float y) const;
    void UpdateHover();
    void OnMouseMove(float x, float y);
    void OnMouseDown(float x, float y, bool dbl);
    void OnMouseUp();
    bool OnKey(UINT vk);
    void OnChar(wchar_t ch);

    // what the controls do
    void SetPage(int p);
    void Toggle(int id);
    void Choose(int id, const RectF& r);
    void SetService(int i);
    void CreateToken();
    void Connect();
    void Connected(const TokenCheck& check, const std::string& tok, bool pasted);
    void StartSignIn();
    void EndSignIn();  // stops waiting for the authorization
    void OpenSignInPage();
    void WipeToken();
    void SetStatus(std::wstring text, int kind) { status = std::move(text); statusKind = kind; }
    void Ask(const Prompt& p, std::function<void(int)> onResult);
    void CloseDialog(int result);
    TextEdit* Field(int id) { return id == kServer ? &server : id == kToken ? &token : nullptr; }
    void FocusField(int id);  // from the keyboard: selects its text and scrolls it into view

    std::unique_ptr<Gfx> gfx;  // one per window: created with it, released when it closes
    UINT dpi = 96;
    RectF caption;             // DWM caption buttons, client coordinates
    float titleH = 0;
    bool active = true;
    int page = kGeneral;
    Scroller scroll;
    RectF scrollView;          // the part of the page that scrolls (set while painting)
    RectF clip;                // zones are cut to it
    RectF fieldBox[3];         // the text boxes as last painted (kServer, kToken)
    float x0 = 0, cw = 0;      // the cards: left edge, width
    std::vector<SZone> zones;
    SZone hover;
    float mx = -1, my = -1;
    bool tracking = false;
    int dragField = kNoField;  // text box being drag-selected
    bool dragScroll = false;
    float dragGrab = 0;
    bool animating = false;
    double lastFrame = 0;
    DialogState dialog;

    // Integrations
    std::vector<Account> accounts;
    int service = 0;  // index in Forges()
    TextEdit server{ L"", 0, 0, false, 300 };
    TextEdit token{ L"", 0, 0, false, 500 };
    int focus = kNoField;
    bool busy = false;  // a token is being checked
    int signIn = kSignInIdle;
    std::shared_ptr<std::atomic<bool>> signInCancel;  // set to stop the sign-in's worker thread
    std::wstring userCode, verifyUrl;                 // the code to enter, and where
    bool scrollToEnd = false;                         // at the next paint (pictures for the tests)
    std::wstring status;
    int statusKind = kInfo;
    bool gitAsked = false;
};

SettingsWindow& Win() {
    static SettingsWindow w;
    return w;
}

// ============================================================================ window

void SettingsWindow::Show(int p) {
    if (p >= 0 && p < kPages && (p != page || !hwnd)) SetPage(p);
    if (server.text.empty() && !busy) server.Set(Forges()[(size_t)service].server);
    accounts = LoadAccounts();
    if (!hwnd && !Create()) return;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    Invalidate();
}

bool SettingsWindow::Create() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static ATOM cls = 0;
    if (!cls) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = Thunk;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                       GetSystemMetrics(SM_CYSMICON), 0);
        wc.lpszClassName = L"MangroveSettings";
        cls = RegisterClassExW(&wc);
        if (!cls) return false;
    }
    // Over the main window (it owns this one: always in front of it, closed with it), as big as the screen allows.
    HWND owner = g_app ? g_app->hwnd : nullptr;
    MONITORINFO mi{ sizeof mi };
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT wa = mi.rcWork;
    const int d = (int)(owner ? GetDpiForWindow(owner) : GetDpiForSystem());
    const int ww = std::min(MulDiv(1060, d, 96), (int)((wa.right - wa.left) * 0.94));
    const int wh = std::min(MulDiv(760, d, 96), (int)((wa.bottom - wa.top) * 0.94));
    RECT orc = wa;
    if (owner && !IsIconic(owner)) GetWindowRect(owner, &orc);
    const int x = std::clamp((int)(orc.left + orc.right - ww) / 2, (int)wa.left, std::max((int)wa.left, (int)wa.right - ww));
    const int y = std::clamp((int)(orc.top + orc.bottom - wh) / 2, (int)wa.top, std::max((int)wa.top, (int)wa.bottom - wh));
    hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, L"MangroveSettings", L"Mangrove Settings", WS_OVERLAPPEDWINDOW, x, y, ww, wh,
                           owner, nullptr, inst, nullptr);
    if (!hwnd) return false;
    dpi = GetDpiForWindow(hwnd);

    // Mica like the Settings app, rounded corners, the frame of the theme; the title bar is drawn here.
    DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_MAINWINDOW;
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof backdrop);
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof corner);
    MARGINS margins{ -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    if (g_app) ThemeWindowFrame(hwnd, g_app->theme);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    g_S = Scale();
    gfx = std::make_unique<Gfx>();
    gfx->scale = g_S;
    RECT cr{};
    GetClientRect(hwnd, &cr);
    if (!gfx->Init(hwnd, (UINT)std::max<LONG>(1, cr.right), (UINT)std::max<LONG>(1, cr.bottom))) {
        DestroyWindow(hwnd);
        return false;
    }
    UpdateCaption();
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    return true;
}

void SettingsWindow::ThemeChanged() {
    if (!hwnd || !g_app) return;
    ThemeWindowFrame(hwnd, g_app->theme);
    if (gfx) gfx->ClearGradients();
    Invalidate();
}

void SettingsWindow::UpdateCaption() {
    RECT r{};
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CAPTION_BUTTON_BOUNDS, &r, sizeof r)) && r.right > r.left) {
        RECT wr{};
        GetWindowRect(hwnd, &wr);
        POINT o{ 0, 0 };
        ClientToScreen(hwnd, &o);
        const float dx = (float)(wr.left - o.x), dy = (float)(wr.top - o.y);
        caption = { r.left + dx, r.top + dy, (float)(r.right - r.left), (float)(r.bottom - r.top) };
    } else {
        caption = { (gfx ? (float)gfx->Width() : 0.f) - DR(138), 0, DR(138), DR(32) };
    }
}

LRESULT CALLBACK SettingsWindow::Thunk(HWND h, UINT m, WPARAM w, LPARAM l) {
    SettingsWindow& s = Win();
    if (!s.hwnd) s.hwnd = h;  // WM_GETMINMAXINFO comes before WM_NCCREATE
    if (h != s.hwnd) return DefWindowProcW(h, m, w, l);
    const float saved = g_S;  // this window's scale while it handles the message (see WndProcThunk)
    g_S = s.Scale();
    const LRESULT r = s.Proc(h, m, w, l);
    g_S = saved;
    return r;
}

LRESULT SettingsWindow::HitTestNC(LPARAM l) {
    const LRESULT def = DefWindowProcW(hwnd, WM_NCHITTEST, 0, l);
    if (def != HTCLIENT) return def;  // left / right / bottom resize borders
    POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
    ScreenToClient(hwnd, &p);
    if (!IsZoomed(hwnd)) {
        const UINT d = GetDpiForWindow(hwnd);
        const int frame = std::max(4, (GetSystemMetricsForDpi(SM_CYFRAME, d) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, d)) / 2);
        if (p.y < frame) {
            RECT cr{};
            GetClientRect(hwnd, &cr);
            if (p.x < frame * 3) return HTTOPLEFT;
            if (p.x >= cr.right - frame * 3) return HTTOPRIGHT;
            return HTTOP;
        }
    }
    return (float)p.y < titleH ? HTCAPTION : HTCLIENT;
}

LRESULT SettingsWindow::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    // Lets DWM handle the native caption buttons (hover, press, Snap Layouts flyout).
    LRESULT dwmResult = 0;
    if (DwmDefWindowProc(h, m, w, l, &dwmResult)) return dwmResult;

    switch (m) {
    case WM_NCCALCSIZE:
        if (w) {
            // No standard title bar; the resize borders stay on the other sides.
            auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(l);
            const LONG top = p->rgrc[0].top;
            const LRESULT r = DefWindowProcW(h, m, w, l);
            p->rgrc[0].top = top;
            if (IsZoomed(h)) {
                const UINT d = GetDpiForWindow(h);
                p->rgrc[0].top += GetSystemMetricsForDpi(SM_CYFRAME, d) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, d);
            }
            return r;
        }
        break;
    case WM_NCHITTEST:
        return HitTestNC(l);
    case WM_ACTIVATE:
        active = LOWORD(w) != WA_INACTIVE;
        if (active) accounts = LoadAccounts();
        Invalidate();
        break;
    case WM_SIZE:
        if (gfx && LOWORD(l) && HIWORD(l)) {
            gfx->Resize(LOWORD(l), HIWORD(l));
            UpdateCaption();
            Paint();
        }
        return 0;
    case WM_DPICHANGED: {
        dpi = HIWORD(w);
        g_S = Scale();
        if (gfx) gfx->SetScale(g_S);
        const RECT* r = reinterpret_cast<RECT*>(l);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        UpdateCaption();
        Invalidate();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(l);
        UINT d = GetDpiForWindow(h);
        if (!d) d = 96;
        mm->ptMinTrackSize = { MulDiv(700, (int)d, 96), MulDiv(500, (int)d, 96) };
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
        if (!dragField && !dragScroll) {
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
        OnMouseUp();
        return 0;
    case WM_CAPTURECHANGED:
        dragField = kNoField;
        dragScroll = false;
        return 0;
    case WM_MOUSEWHEEL:
        if (!dialog.active) {
            scroll.By(-(double)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * DR(100));
            Invalidate();
        }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (OnKey((UINT)w)) return 0;
        break;
    case WM_CHAR:
        OnChar((wchar_t)w);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            const bool text = dragField || (hover.id == SHit::Field && !dialog.active);
            SetCursor(LoadCursorW(nullptr, text ? IDC_IBEAM : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_TIMER:  // the caret blinks
        Invalidate();
        return 0;
    // Windows settings that the Windows themes follow (the main window gets them too; repeats change nothing).
    case WM_SETTINGCHANGE:
        if (g_app && l && CompareStringOrdinal(reinterpret_cast<LPCWSTR>(l), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL)
            g_app->ApplyTheme();
        break;
    case WM_SYSCOLORCHANGE:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        if (g_app) g_app->ApplyTheme();
        break;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        KillTimer(h, kCaretTimer);
        KillTimer(h, kSpinTimer);
        EndSignIn();  // closing the window gives up on the authorization
        WipeToken();  // a token typed and not connected doesn't stay around
        focus = kNoField;
        dialog = DialogState{};
        if (!busy) SetStatus(L"", kInfo);
        hover = {};
        zones.clear();
        gfx.reset();
        return 0;
    case WM_NCDESTROY:
        hwnd = nullptr;
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

// ============================================================================ painting

void SettingsWindow::Paint() {
    if (!gfx || !gfx->Begin()) return;
    Gfx& g = *gfx;
    const double now = NowMs();
    const float dt = lastFrame > 0 ? (float)std::min(64.0, now - lastFrame) : 16.f;
    lastFrame = now;
    zones.clear();
    animating = scroll.Step(dt);
    const float W = (float)g.Width(), H = (float)g.Height();
    const RectF full{ 0, 0, W, H };
    clip = full;
    titleH = std::max(DR(kTitleH), std::round(caption.b()));
    // One surface, title bar included: Mica in the Windows themes, navy in Mangrove's.
    g.Fill(full, PanelBg);
    DrawLogo(g, DR(24), std::round(titleH * 0.5f), DR(16));
    g.Text(L"Settings", { DR(44), 0, std::max(0.f, (caption.w > 0 ? caption.x : W) - DR(52)), titleH }, F::Small, active ? Text : TextDim);
    PaintNav();

    const float navW = DR(kNavW);
    x0 = navW + DR(20);
    cw = std::max(DR(240), std::min(DR(kMaxW), W - x0 - DR(28)));
    g.Text(kPageTitles[page], { x0, titleH + DR(10), cw, DR(46) }, F::PageTitle, Text);
    PaintPage({ navW, titleH + DR(kHeadH), W - navW, std::max(0.f, H - titleH - DR(kHeadH)) });

    clip = full;
    if (dialog.active) {
        AddZone(full, SHit::Block);
        std::vector<RectF> buttons;
        DrawDialog(g, dialog, hover.id == SHit::DialogButton ? hover.a : -1, buttons);
        for (size_t i = 0; i < buttons.size(); ++i) AddZone(buttons[i], SHit::DialogButton, (int)i);
    }
    g.End();

    if (focus && !dialog.active) SetTimer(hwnd, kCaretTimer, 530, nullptr);
    else KillTimer(hwnd, kCaretTimer);
    if (busy || signIn != kSignInIdle) SetTimer(hwnd, kSpinTimer, 33, nullptr);
    else KillTimer(hwnd, kSpinTimer);
    if (animating) Invalidate();
    else lastFrame = 0;
    UpdateHover();  // the zones moved (scrolling, another page)
}

void SettingsWindow::PaintNav() {
    Gfx& g = *gfx;
    const float navW = DR(kNavW);
    float y = titleH + DR(4);
    for (int i = 0; i < kPages; ++i) {
        const RectF r{ DR(8), y, navW - DR(16), DR(36) };
        const bool sel = i == page;
        if (sel) {
            g.FillR(r, DR(4), Subtle);
            g.FillR({ r.x, r.y + DR(10), DR(3), r.h - DR(20) }, DR(1.5f), Accent);
        } else if (Hov(SHit::Nav, i)) {
            g.FillR(r, DR(4), Hover);
        }
        const float icx = r.x + DR(20);
        if (i == kGit) DrawBranchIcon(g, icx, r.cy(), DR(16), Text);
        else g.Glyph(kPageGlyphs[i], { icx - DR(12), r.y, DR(24), r.h }, F::Icon16, Text);
        g.Text(kPageTitles[i], { r.x + DR(40), r.y, r.w - DR(48), r.h }, F::Nav, Text);
        AddZone(r, SHit::Nav, i);
        y += DR(40);
    }
}

void SettingsWindow::PaintPage(const RectF& view) {
    Gfx& g = *gfx;
    scrollView = view;
    scroll.view = view.h;
    const float top = view.y + DR(2) - (float)std::round(scroll.pos);
    float y = top;
    clip = view;
    g.PushClip(view);
    switch (page) {
    case kGeneral: PaintGeneral(y); break;
    case kAppearance: PaintAppearance(y); break;
    case kGit: PaintGit(y); break;
    default: PaintIntegrations(y); break;
    }
    g.PopClip();
    clip = { 0, 0, (float)g.Width(), (float)g.Height() };
    scroll.content = (double)(y - top) + DR(28);
    const double before = scroll.pos;
    scroll.Clamp();
    if (scroll.pos != before) animating = true;  // the page got shorter
    if (scrollToEnd) {
        scrollToEnd = false;
        scroll.To(scroll.Max(), true);
        animating = true;
    }

    // the scroll bar: thin, wider while the pointer is on it
    RectF thumb;
    if (ScrollThumb(view, scroll, thumb)) {
        const bool act = Hov(SHit::Scroll) || dragScroll;
        const RectF track{ view.r() - DR(12), view.y, DR(12), view.h };
        if (act) g.FillR(track.Inset(DR(2), DR(2)), DR(5), ScrollbarTrack);
        const float w = act ? DR(6) : DR(3);
        g.FillR({ thumb.r() - DR(3) - w, thumb.y + DR(3), w, thumb.h - DR(6) }, w * 0.5f, act ? A(TextSec, 0.9f) : ScrollbarThumb);
        AddZone(track, SHit::Scroll);
    }
}

// A settings card (WinUI SettingsCard): an icon, a title and a description on the left, a control on the
// right, or under the text when the card is too narrow for both. Returns where the control goes.
RectF SettingsWindow::Card(float& y, const Icon& icon, std::wstring_view title, std::wstring_view desc, float ctrlW, float ctrlH,
                           const Color* titleColor) {
    Gfx& g = *gfx;
    const float pad = DR(16);
    const float iconW = icon.avatar ? DR(32) : (icon.glyph || icon.branch) ? DR(20) : 0.f;
    const float textX = x0 + pad + (iconW > 0 ? iconW + DR(16) : 0.f);
    const float right = x0 + cw - pad;
    float textW = right - (ctrlW > 0 ? ctrlW + DR(16) : 0.f) - textX;
    const bool stacked = ctrlW > 0 && textW < DR(200);
    if (stacked) textW = right - textX;
    ComPtr<IDWriteTextLayout> dl;
    float dh = 0;
    if (!desc.empty() && (dl = g.Layout(desc, F::Caption, std::max(DR(40), textW)))) {
        DWRITE_TEXT_METRICS m{};
        dl->GetMetrics(&m);
        dh = std::ceil(m.height);
    }
    const float lineH = DR(20);
    const float textH = lineH + (dl ? DR(2) + dh : 0.f);
    const float h = stacked ? DR(14) + textH + DR(12) + ctrlH + DR(16) : std::max({ DR(kCardMinH), textH + DR(28), ctrlH + DR(28) });
    const RectF r{ x0, std::round(y), cw, std::round(h) };
    y = r.b() + DR(4);
    g.FillR(r, DR(4), th::Card);
    g.StrokeR(r, DR(4), CardStroke);
    const float ty = stacked ? r.y + DR(14) : std::round(r.cy() - textH * 0.5f);
    const float icx = x0 + pad + iconW * 0.5f, icy = stacked ? ty + lineH * 0.5f : r.cy();
    if (icon.avatar) DrawAvatar(g, icx, icy, DR(16), *icon.avatar);
    else if (icon.branch) DrawBranchIcon(g, icx, icy, DR(18), Text);
    else if (icon.glyph) g.Glyph(icon.glyph, { icx - DR(14), icy - DR(14), DR(28), DR(28) }, F::Icon20, Text);
    g.Text(title, { textX, ty, textW, lineH }, F::Nav, titleColor ? *titleColor : Text);
    if (dl) g.dc()->DrawTextLayout({ textX, ty + lineH + DR(2) }, dl.Get(), g.B(TextSec), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    if (ctrlW <= 0) return {};
    if (stacked) return { textX, r.b() - DR(16) - ctrlH, std::min(ctrlW, textW), ctrlH };
    return { right - ctrlW, std::round(r.cy() - ctrlH * 0.5f), ctrlW, ctrlH };
}

void SettingsWindow::Section(float& y, std::wstring_view title) {
    y += DR(18);
    gfx->Text(title, { x0 + DR(2), std::round(y), cw, DR(22) }, F::RowTitle, Text);
    y += DR(22) + DR(8);
}

void SettingsWindow::Paragraph(float& y, std::wstring_view text) {
    auto l = gfx->Layout(text, F::Body, cw - DR(4));
    if (!l) return;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    gfx->dc()->DrawTextLayout({ x0 + DR(2), std::round(y) }, l.Get(), gfx->B(TextSec), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    y += std::ceil(m.height) + DR(10);
}

// A toggle switch (WinUI) at the right of its area, with "On" or "Off" before it.
void SettingsWindow::ToggleSwitch(const RectF& area, bool on, int id) {
    Gfx& g = *gfx;
    const bool hov = Hov(SHit::Toggle, id);
    const RectF sw{ area.r() - DR(40), std::round(area.cy() - DR(10)), DR(40), DR(20) };
    if (on) {
        g.FillR(sw, DR(10), hov ? AccentHi : Accent);
    } else {
        g.FillR(sw, DR(10), hov ? SubtleHover : Subtle);
        g.StrokeR(sw, DR(10), TextSec, std::max(1.f, D(1.f)));
    }
    g.Circle(on ? sw.r() - DR(10) : sw.x + DR(10), sw.cy(), hov ? D(7.f) : D(6.f), on ? TextOnAccent : TextSec);
    const wchar_t* label = on ? L"On" : L"Off";
    const float lw = g.Measure(label, F::UI);
    const RectF lr{ sw.x - DR(12) - lw, area.y, lw + 2, area.h };
    g.Text(label, lr, F::UI, Text);
    AddZone({ lr.x - DR(6), sw.y - DR(6), sw.r() - lr.x + DR(12), sw.h + DR(12) }, SHit::Toggle, id);
}

// A drop-down list (WinUI ComboBox); the choices open as a menu.
void SettingsWindow::ChoiceBox(const RectF& r, std::wstring_view label, int id, bool enabled) {
    Gfx& g = *gfx;
    const bool hov = enabled && Hov(SHit::Choice, id);
    g.FillR(r, DR(4), hov ? SubtleHover : Subtle);
    g.StrokeR(r, DR(4), ControlStroke);
    g.Text(label, { r.x + DR(12), r.y, r.w - DR(44), r.h }, F::UI, enabled ? Text : TextDim);
    g.Glyph(0xE70D, { r.r() - DR(32), r.y, DR(20), r.h }, F::Icon10, enabled ? TextSec : TextDim);
    if (enabled) AddZone(r, SHit::Choice, id);
}

float SettingsWindow::ChoiceWidth(const std::vector<std::wstring>& labels) const {
    float w = 0;
    for (const std::wstring& s : labels) w = std::max(w, gfx->Measure(s, F::UI));
    return std::max(DR(160), std::ceil(w) + DR(56));
}

// A radio button (WinUI) for a service, its label after it; 32 DIP high.
void SettingsWindow::Radio(float x, float y, std::wstring_view label, bool on, int index, bool enabled) {
    Gfx& g = *gfx;
    const bool hov = enabled && Hov(SHit::Service, index);
    const float cx = x + DR(10), cy = y + DR(16);
    if (on) {
        g.Circle(cx, cy, DR(10), enabled ? (hov ? AccentHi : Accent) : TextDim);
        g.Circle(cx, cy, hov ? D(5.f) : D(4.f), TextOnAccent);
    } else {
        g.Circle(cx, cy, DR(10), hov ? SubtleHover : Subtle);
        g.CircleS(cx, cy, DR(10) - D(0.5f), enabled ? TextSec : TextDim, std::max(1.f, D(1.f)));
    }
    const float lw = g.Measure(label, F::UI);
    g.Text(label, { x + DR(28), y, lw + 2, DR(32) }, F::UI, enabled ? Text : TextDim);
    if (enabled) AddZone({ x - DR(4), y, DR(28) + lw + DR(8), DR(32) }, SHit::Service, index);
}

void SettingsWindow::AddZone(const RectF& r, SHit id, int a) {
    const RectF v = Intersect(r, clip);  // what scrolled out of the page doesn't answer clicks
    if (!v.Empty()) zones.push_back({ v, id, a });
}

// ---------------------------------------------------------------------------- the pages

void SettingsWindow::PaintGeneral(float& y) {
    const Settings& s = AppSettings();
    RectF c = Card(y, { 0xE838 }, L"Reopen repositories at launch", L"The tabs that were open when Mangrove closed open again.",
                   DR(100), DR(32));
    ToggleSwitch(c, s.restoreTabs, kReopen);
    const std::vector<std::wstring>& pull = PullModeLabels();
    c = Card(y, { 0xE896 }, L"Pull", PullModeHelp(), ChoiceWidth(pull), DR(32));
    ChoiceBox(c, pull[(size_t)std::clamp(s.pullMode, 0, (int)pull.size() - 1)], kPull, true);
    c = Card(y, { 0xE8A5 }, L"Diffs", L"How the changes of a file are shown: one column, or the old and new text side by side.",
             ChoiceWidth(kDiffLabels), DR(32));
    ChoiceBox(c, kDiffLabels[g_app && g_app->diffSplit ? 1 : 0], kDiffs, true);
}

void SettingsWindow::PaintAppearance(float& y) {
    Gfx& g = *gfx;
    const bool contrast = IsHighContrast();
    int theme = 0;
    for (int i = 0; i < 4; ++i)
        if (g_app && kThemes[i] == g_app->theme) theme = i;
    RectF c = Card(y, { 0xE790 }, L"Theme",
                   contrast ? L"A contrast theme is on in Windows: Mangrove uses its colors."
                            : L"The Windows themes follow your accent color; Mangrove's own theme is navy and gold.",
                   ChoiceWidth(kThemeLabels), DR(32));
    ChoiceBox(c, kThemeLabels[(size_t)theme], kTheme, !contrast);

    // Zoom: [-] 100% [+]   Reset
    const float zoom = g_app ? g_app->zoom : 1.f;
    c = Card(y, { 0xE71E }, L"Zoom",
             L"Size of the text and controls in the main window: " + Keys(L"Ctrl+Plus") + L", " + Keys(L"Ctrl+Minus") + L", " + Keys(L"Ctrl+0") + L".",
             DR(224), DR(32));
    const RectF minus{ c.x, c.y, DR(36), DR(32) }, value{ minus.r(), c.y, DR(64), DR(32) }, plus{ value.r(), c.y, DR(36), DR(32) };
    const RectF reset{ c.r() - DR(72), c.y, DR(72), DR(32) };
    const bool canOut = zoom > 0.75f, canIn = zoom < 1.95f, canReset = std::fabs(zoom - 1.f) > 0.01f;
    DrawButton(g, minus, L"", 0, Hov(SHit::Zoom, 0), canOut);
    g.Glyph(0xE738, minus, F::Icon12, canOut ? Text : TextDim);
    g.Text(std::to_wstring((int)std::lround(zoom * 100)) + L"%", value, F::UI, Text, DWRITE_TEXT_ALIGNMENT_CENTER);
    DrawButton(g, plus, L"", 0, Hov(SHit::Zoom, 1), canIn);
    g.Glyph(0xE710, plus, F::Icon12, canIn ? Text : TextDim);
    DrawButton(g, reset, L"Reset", 0, Hov(SHit::Zoom, 2), canReset);
    if (canOut) AddZone(minus, SHit::Zoom, 0);
    if (canIn) AddZone(plus, SHit::Zoom, 1);
    if (canReset) AddZone(reset, SHit::Zoom, 2);

    c = Card(y, { 0xE8A0 }, L"Commit details", L"The details of the selected commit, beside the history (" + Keys(L"Ctrl+J") + L").",
             DR(100), DR(32));
    ToggleSwitch(c, g_app && g_app->detailVisible, kDetails);
    c = Card(y, { 0xE700 }, L"Navigation icons only",
             L"A narrow navigation pane that leaves more room to the history (" + Keys(L"Ctrl+K") + L").", DR(100), DR(32));
    ToggleSwitch(c, g_app && !g_app->sideVisible, kNavIcons);
}

void SettingsWindow::PaintGit(float& y) {
    Gfx& g = *gfx;
    const std::wstring version = GitVersionIfReady();
    if (version.empty() && !gitAsked) {  // finds git on a worker thread, once
        gitAsked = true;
        std::thread([] {
            GitVersion();
            RunOnMain([] {
                Win().gitChecked = true;
                Win().Invalidate();
            });
        }).detach();
    }
    if (!version.empty()) Card(y, { 0, true }, L"Git " + version, GitPath(false), 0, 0);
    else if (gitChecked) Card(y, { 0xE7BA }, L"Git wasn't found", L"Install Git for Windows (git-scm.com), then open Mangrove again.", 0, 0);
    else Card(y, { 0, true }, L"Git", L"Looking for Git…", 0, 0);

    Section(y, L"Trusted folders");
    Paragraph(y, TrustedHelp());
    const std::vector<std::wstring>& trusted = AppSettings().trusted;
    if (trusted.empty()) Card(y, { 0xE8B7 }, L"No trusted folders", L"", 0, 0, &TextSec);
    for (size_t i = 0; i < trusted.size(); ++i) {
        const RectF c = Card(y, { 0xE8B7 }, trusted[i], L"", DR(96), DR(32));
        DrawButton(g, c, L"Remove", 0, Hov(SHit::TrustRemove, (int)i), true);
        AddZone(c, SHit::TrustRemove, (int)i);
    }
}

void SettingsWindow::PaintIntegrations(float& y) {
    Gfx& g = *gfx;
    Paragraph(y, AccountsIntro());
    Section(y, L"Accounts");
    if (accounts.empty()) Card(y, {}, L"No accounts yet", L"Add one below: Mangrove checks the token with the server first.", 0, 0, &TextSec);
    for (size_t i = 0; i < accounts.size(); ++i) {
        const Account& a = accounts[i];
        Author av;  // the same initials and color as in the macOS app
        av.initials = Initials(a.name.empty() ? a.login : a.name, L"");
        av.color = AvatarColor(a.Key());
        const RectF c = Card(y, { 0, false, &av }, AccountTitle(a), std::wstring(InfoOf(a.forge).name) + L" · " + a.Host(),
                             DR(96), DR(32));
        const bool can = !busy && signIn == kSignInIdle;
        DrawButton(g, c, L"Remove", 0, Hov(SHit::AccountRemove, (int)i), can);
        if (can) AddZone(c, SHit::AccountRemove, (int)i);
    }
    Section(y, L"Add an account");
    PaintAddAccount(y);
}

// The form that connects an account: the service and its server, then signing in with the browser (where
// Mangrove has an OAuth app) or a token created on the server's site and pasted here.
void SettingsWindow::PaintAddAccount(float& y) {
    Gfx& g = *gfx;
    const float pad = DR(16), labelW = DR(80);
    const bool narrow = cw - 2 * pad < DR(440);  // the labels go above their controls
    const float fx = narrow ? x0 + pad : x0 + pad + labelW + DR(12);
    const float fw = std::max(DR(120), std::min(DR(520), x0 + cw - pad - fx));
    const float labelH = narrow ? DR(24) : 0.f;
    const ForgeInfo& forge = Forges()[(size_t)service];
    const bool locked = busy || signIn != kSignInIdle;  // a token check or a sign-in is running
    const bool browser = signIn == kSignInIdle && BrowserSignInAvailable(forge.forge, server.text);

    // Everything that wraps is measured first: the card is drawn before what it holds.
    std::vector<std::pair<float, float>> radios;  // offsets in the service row
    float rx = 0, ry = 0;
    for (const ForgeInfo& f : Forges()) {
        const float w = DR(28) + g.Measure(f.name, F::UI) + DR(24);
        if (rx > 0 && rx + w > fw) {
            rx = 0;
            ry += DR(32);
        }
        radios.push_back({ rx, ry });
        rx += w;
    }
    auto wrap = [&](std::wstring_view text, float w, float& h) {
        h = 0;
        ComPtr<IDWriteTextLayout> l = text.empty() ? nullptr : g.Layout(text, F::Caption, w);
        if (l) {
            DWRITE_TEXT_METRICS m{};
            l->GetMetrics(&m);
            h = std::ceil(m.height);
        }
        return l;
    };
    // A button that opens a page in the browser: its label, then the "new window" glyph.
    auto linkWidth = [&](std::wstring_view label, int style) { return g.Measure(label, style == 1 ? F::UISemi : F::UI) + DR(52); };
    auto linkButton = [&](const RectF& r, std::wstring_view label, int style, SHit id, bool enabled) {
        const F font = style == 1 ? F::UISemi : F::UI;
        DrawButton(g, r, L"", style, Hov(id), enabled, font);
        const Color fg = !enabled ? TextDim : style == 1 ? TextOnAccent : Text;
        g.Text(label, { r.x + DR(14), r.y, r.w - DR(40), r.h }, font, fg);
        g.Glyph(0xE8A7, { r.r() - DR(30), r.y, DR(20), r.h }, F::Icon12, !enabled ? TextDim : style == 1 ? TextOnAccent : TextSec);
        if (enabled) AddZone(r, id);
    };
    const std::wstring createLabel = L"Create a token", browserLabel = L"Sign in with the browser", openLabel = L"Copy the code and open GitHub";
    float serverHelpH = 0, browserHelpH = 0, tokenHelpH = 0, statusH = 0, codeHelpH = 0;
    const ComPtr<IDWriteTextLayout> serverHelp = wrap(kServerHelp, fw, serverHelpH);
    const ComPtr<IDWriteTextLayout> browserHelp = browser ? wrap(kBrowserHelp, fw, browserHelpH) : nullptr;
    const ComPtr<IDWriteTextLayout> tokenHelp = signIn == kSignInIdle ? wrap(forge.tokenHelp, fw, tokenHelpH) : nullptr;
    const ComPtr<IDWriteTextLayout> codeHelp = signIn == kSignInWaiting ? wrap(kCodeHelp, fw, codeHelpH) : nullptr;
    const float connectW = DR(112);
    const float statusX = fx + connectW + DR(12) + (busy ? DR(26) : 0.f);
    const ComPtr<IDWriteTextLayout> statusL =
        signIn == kSignInIdle ? wrap(status, std::max(DR(100), x0 + cw - pad - statusX), statusH) : nullptr;
    const float openW = std::min(fw, linkWidth(openLabel, 1)), cancelW = DR(96);
    const bool cancelBelow = openW + DR(8) + cancelW > fw;

    const float hService = labelH + ry + DR(32);
    const float hServer = labelH + DR(32) + DR(6) + serverHelpH;
    const float hBrowser = labelH + DR(32) + DR(6) + browserHelpH;
    const float hToken = labelH + DR(32) + DR(6) + tokenHelpH + DR(10) + DR(32);
    const float hConnect = std::max(DR(32), statusH);
    const float hStarting = labelH + DR(32) + DR(12) + DR(32);
    const float hWaiting = labelH + codeHelpH + DR(10) + DR(56) + DR(14) + DR(32) + (cancelBelow ? DR(8) + DR(32) : 0.f) + DR(12) + DR(20);
    float h = pad + hService + DR(18) + hServer + pad;
    if (signIn == kSignInStarting) h += DR(18) + hStarting;
    else if (signIn == kSignInWaiting) h += DR(18) + hWaiting;
    else h += (browser ? DR(18) + hBrowser : 0.f) + DR(18) + hToken + DR(20) + hConnect;
    const RectF card{ x0, std::round(y), cw, std::round(h) };
    y = card.b() + DR(4);
    g.FillR(card, DR(4), th::Card);
    g.StrokeR(card, DR(4), CardStroke);

    auto label = [&](std::wstring_view text, float rowY) {
        if (narrow) g.Text(text, { fx, rowY, fw, DR(20) }, F::UI, Text);
        else g.Text(text, { x0 + pad, rowY, labelW, DR(32) }, F::UI, Text);
    };
    auto help = [&](const ComPtr<IDWriteTextLayout>& l, float atY, const Color& c) {
        if (l) g.dc()->DrawTextLayout({ fx, atY }, l.Get(), g.B(c), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    };
    auto cancelButton = [&](const RectF& r) {
        DrawButton(g, r, L"Cancel", 0, Hov(SHit::CancelSignIn), true);
        AddZone(r, SHit::CancelSignIn);
    };
    float cy = card.y + pad;

    label(L"Service", cy);
    for (size_t i = 0; i < radios.size(); ++i)
        Radio(fx + radios[i].first, cy + labelH + radios[i].second, Forges()[i].name, (int)i == service, (int)i, !locked);
    cy += hService + DR(18);

    label(L"Server", cy);
    {
        const RectF box{ fx, cy + labelH, fw, DR(32) };
        fieldBox[kServer] = box;
        TextBoxFrame(g, box, focus == kServer);
        const RectF inner{ box.x + DR(10), box.y, box.w - DR(20), box.h };
        if (locked) g.Text(server.text, inner, F::UI, TextDim);  // can't change while an account connects
        else DrawEdit(g, server, inner, F::UI, L"https://", focus == kServer && !dialog.active);
        if (!locked) AddZone(box, SHit::Field, kServer);
        help(serverHelp, box.b() + DR(6), TextSec);
    }
    cy += hServer;

    if (signIn != kSignInIdle) {  // signing in: the code to enter on GitHub while Mangrove waits for the authorization
        cy += DR(18);
        label(L"Sign in", cy);
        float sy = cy + labelH;
        if (signIn == kSignInStarting) {
            DrawSpinner(g, fx + DR(9), sy + DR(16), DR(7));
            g.Text(L"Asking GitHub for a code\u2026", { fx + DR(26), sy, fw - DR(26), DR(32) }, F::UI, TextSec);
            cancelButton({ fx, sy + DR(32) + DR(12), cancelW, DR(32) });
            return;
        }
        help(codeHelp, sy, Text);
        sy += codeHelpH + DR(10);
        const RectF code{ fx, sy, std::min(fw, g.Measure(userCode, F::PageTitle) + DR(56)), DR(56) };
        g.FillR(code, DR(6), InputBg);
        g.StrokeR(code, DR(6), ControlStroke);
        g.Text(userCode, code, F::PageTitle, Text, DWRITE_TEXT_ALIGNMENT_CENTER);
        sy = code.b() + DR(14);
        const RectF open{ fx, sy, openW, DR(32) };
        linkButton(open, openLabel, 1, SHit::OpenCode, true);
        const RectF cancel = cancelBelow ? RectF{ fx, open.b() + DR(8), cancelW, DR(32) } : RectF{ open.r() + DR(8), sy, cancelW, DR(32) };
        cancelButton(cancel);
        sy = cancel.b() + DR(12);
        DrawSpinner(g, fx + DR(9), sy + DR(10), DR(7));
        g.Text(L"Waiting for you to authorize Mangrove on GitHub\u2026", { fx + DR(26), sy, fw - DR(26), DR(20) }, F::UI, TextSec);
        return;
    }

    if (browser) {  // the easy way, where Mangrove has an OAuth app
        cy += DR(18);
        label(L"Sign in", cy);
        const RectF b{ fx, cy + labelH, std::min(fw, linkWidth(browserLabel, 1)), DR(32) };
        linkButton(b, browserLabel, 1, SHit::SignIn, !busy);
        help(browserHelp, b.b() + DR(6), TextSec);
        cy += hBrowser;
    }

    cy += DR(18);
    label(L"Token", cy);
    {
        // The service's page for a new token, with its name and scopes filled in: it opens in the browser.
        const RectF cb{ fx, cy + labelH, std::min(fw, linkWidth(createLabel, 0)), DR(32) };
        linkButton(cb, createLabel, 0, SHit::CreateToken, true);
        help(tokenHelp, cb.b() + DR(6), TextSec);
        const RectF box{ fx, cb.b() + DR(6) + tokenHelpH + DR(10), fw, DR(32) };
        fieldBox[kToken] = box;
        TextBoxFrame(g, box, focus == kToken);
        DrawEdit(g, token, { box.x + DR(10), box.y, box.w - DR(20), box.h }, F::UI, L"Paste the token here",
                 focus == kToken && !dialog.active, true);
        if (!busy) AddZone(box, SHit::Field, kToken);
    }
    cy += hToken + DR(20);

    {
        const RectF cb{ fx, cy, connectW, DR(32) };
        const bool can = !busy && !Blank(token.text);
        DrawButton(g, cb, L"Connect", 1, Hov(SHit::Connect), can, F::UISemi);
        if (can) AddZone(cb, SHit::Connect);
        if (busy) DrawSpinner(g, cb.r() + DR(12) + DR(9), cb.cy(), DR(7));
        if (statusL) {
            const Color c = statusKind == kError ? Deleted : statusKind == kSuccess ? Added : statusKind == kWarning ? Modified : TextSec;
            const float sy = statusH < DR(32) ? std::round(cb.cy() - statusH * 0.5f) : cb.y;
            g.dc()->DrawTextLayout({ statusX, sy }, statusL.Get(), g.B(c), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        }
    }
}

// ============================================================================ input

const SZone* SettingsWindow::ZoneAt(float x, float y) const {
    for (size_t i = zones.size(); i-- > 0;)
        if (zones[i].r.Contains(x, y)) return &zones[i];
    return nullptr;
}

void SettingsWindow::UpdateHover() {
    const SZone* z = mx >= 0 ? ZoneAt(mx, my) : nullptr;
    const SZone nz = z ? *z : SZone{};
    if (nz.id != hover.id || nz.a != hover.a) {
        hover = nz;
        Invalidate();
    }
}

void SettingsWindow::OnMouseMove(float x, float y) {
    mx = x;
    my = y;
    if (!tracking) {
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        tracking = true;
    }
    if (dragField) {
        if (TextEdit* e = Field(dragField)) e->caret = e->HitTest(x, y);
        Invalidate();
    } else if (dragScroll) {
        RectF th;
        if (ScrollThumb(scrollView, scroll, th)) {
            const float track = scrollView.h - th.h;
            scroll.To(track > 0 ? (y - dragGrab - scrollView.y) / track * scroll.Max() : 0.0, true);
        }
        Invalidate();
    } else {
        UpdateHover();
    }
}

void SettingsWindow::OnMouseDown(float x, float y, bool dbl) {
    const SZone* zp = ZoneAt(x, y);
    const SZone z = zp ? *zp : SZone{};
    if (dialog.active) {  // modal: only its buttons answer
        if (z.id == SHit::DialogButton) CloseDialog(z.a);
        return;
    }
    if (z.id != SHit::Field) focus = kNoField;
    switch (z.id) {
    case SHit::Nav: SetPage(z.a); break;
    case SHit::Toggle: Toggle(z.a); break;
    case SHit::Choice: Choose(z.a, z.r); break;
    case SHit::Zoom:
        if (g_app) {
            const float to = z.a == 0 ? g_app->zoom - 0.1f : z.a == 1 ? g_app->zoom + 0.1f : 1.f;
            OnMain([to] { g_app->SetZoom(to); });
        }
        break;
    case SHit::TrustRemove: {
        std::vector<std::wstring>& trusted = AppSettings().trusted;
        if (z.a >= 0 && z.a < (int)trusted.size()) {
            trusted.erase(trusted.begin() + z.a);
            OnMain([] { g_app->SaveSettings(); });
        }
        break;
    }
    case SHit::AccountRemove:
        if (z.a >= 0 && z.a < (int)accounts.size()) {
            const Account a = accounts[(size_t)z.a];
            Ask(RemoveAccountPrompt(a), [this, a](int b) {
                if (b != 0) return;
                RemoveAccount(a);
                accounts = LoadAccounts();
                SetStatus(L"", kInfo);
            });
        }
        break;
    case SHit::Service: SetService(z.a); break;
    case SHit::Field: {
        TextEdit* e = Field(z.a);
        if (!e) break;
        focus = z.a;
        const size_t i = e->HitTest(x, y);
        if (dbl) {
            if (z.a == kToken) e->SelectAll();  // no words to show in a token
            else e->SelectWordAt(i);
            break;
        }
        e->caret = i;
        if (GetKeyState(VK_SHIFT) >= 0) e->anchor = i;
        dragField = z.a;
        SetCapture(hwnd);
        break;
    }
    case SHit::CreateToken: CreateToken(); break;
    case SHit::SignIn: StartSignIn(); break;
    case SHit::OpenCode: OpenSignInPage(); break;
    case SHit::CancelSignIn: EndSignIn(); break;
    case SHit::Connect: Connect(); break;
    case SHit::Scroll: {
        RectF th;
        if (!ScrollThumb(scrollView, scroll, th)) break;
        if (y < th.y || y >= th.b()) {  // a page up or down
            scroll.By((y < th.y ? -1.0 : 1.0) * scroll.view * 0.9);
        } else {
            dragScroll = true;
            dragGrab = y - th.y;
            SetCapture(hwnd);
        }
        break;
    }
    default: break;
    }
    Invalidate();
}

void SettingsWindow::OnMouseUp() {
    if (dragField || dragScroll) ReleaseCapture();
    dragField = kNoField;
    dragScroll = false;
    Invalidate();
}

bool SettingsWindow::OnKey(UINT vk) {
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    if (alt && !ctrl) return false;  // Alt+F4, Alt+Space
    if (dialog.active) {
        if (vk == VK_ESCAPE) CloseDialog(-1);
        else if (vk == VK_RETURN) CloseDialog(dialog.defButton);
        return true;
    }
    if (signIn != kSignInIdle && (vk == VK_ESCAPE || vk == VK_RETURN)) {  // Esc gives up, Enter opens the page again
        if (vk == VK_ESCAPE) EndSignIn();
        else OpenSignInPage();
        Invalidate();
        return true;
    }
    if (TextEdit* e = Field(focus)) {
        const bool masked = focus == kToken;
        switch (vk) {
        case VK_ESCAPE: focus = kNoField; break;
        case VK_TAB: FocusField(focus == kServer ? kToken : kServer); break;
        case VK_RETURN: Connect(); break;
        default: {
            if (masked && ctrl && (vk == 'C' || vk == 'X')) break;  // a token isn't copied out of its box
            if (masked && ctrl && (vk == VK_BACK || vk == VK_DELETE)) {  // no words in a token: to the start or the end
                e->anchor = vk == VK_BACK ? 0 : e->text.size();
                e->Insert(L"");
            } else {
                if (masked && ctrl && (vk == VK_LEFT || vk == VK_RIGHT)) vk = vk == VK_LEFT ? VK_HOME : VK_END;
                const size_t before = e->text.size();
                e->OnKey(vk, ctrl, shift, hwnd);
                if (e->text.size() == before) break;
            }
            if (statusKind == kError) SetStatus(L"", kInfo);  // the field changed: the error is old news
            break;
        }
        }
        Invalidate();
        return true;
    }
    if (ctrl && vk == VK_TAB) {
        SetPage((page + (shift ? kPages - 1 : 1)) % kPages);
    } else if (vk == VK_TAB && page == kIntegrations && !busy && signIn == kSignInIdle) {  // into the form
        FocusField(shift ? kToken : kServer);
    } else if ((ctrl && vk == 'W') || vk == VK_ESCAPE) {
        Close();
        return true;
    } else {
        switch (vk) {
        case VK_UP: scroll.By(-DR(60)); break;
        case VK_DOWN: scroll.By(DR(60)); break;
        case VK_PRIOR: scroll.By(-scroll.view * 0.9); break;
        case VK_NEXT: scroll.By(scroll.view * 0.9); break;
        case VK_HOME: scroll.To(0, false); break;
        case VK_END: scroll.To(scroll.Max(), false); break;
        default: return false;
        }
    }
    Invalidate();
    return true;
}

void SettingsWindow::OnChar(wchar_t ch) {
    if (dialog.active || ch < 32 || ch == 127) return;
    TextEdit* e = Field(focus);
    if (!e) return;
    e->Insert(std::wstring_view(&ch, 1));
    if (statusKind == kError) SetStatus(L"", kInfo);
    Invalidate();
}

// ============================================================================ what the controls do

void SettingsWindow::FocusField(int id) {
    TextEdit* e = Field(id);
    if (!e) return;
    focus = id;
    e->SelectAll();
    const RectF& r = fieldBox[id];
    if (r.Empty()) return;
    if (r.y < scrollView.y) scroll.By(r.y - scrollView.y - DR(24));
    else if (r.b() > scrollView.b()) scroll.By(r.b() - scrollView.b() + DR(24));
}

void SettingsWindow::SetPage(int p) {
    if (p < 0 || p >= kPages) return;
    page = p;
    scroll = Scroller{};
    focus = kNoField;
    fieldBox[kServer] = fieldBox[kToken] = RectF{};
    if (p == kIntegrations) accounts = LoadAccounts();
}

void SettingsWindow::Toggle(int id) {
    if (!g_app) return;
    switch (id) {
    case kReopen: AppSettings().restoreTabs = !AppSettings().restoreTabs; break;
    case kDetails: g_app->detailVisible = !g_app->detailVisible; break;
    case kNavIcons: g_app->sideVisible = !g_app->sideVisible; break;
    default: return;
    }
    OnMain([] {
        g_app->SaveSettings();
        g_app->Invalidate();
    });
}

void SettingsWindow::Choose(int id, const RectF& r) {
    if (!g_app) return;
    const std::vector<std::wstring>& items = id == kPull ? PullModeLabels() : id == kDiffs ? kDiffLabels : kThemeLabels;
    int cur = 0;
    if (id == kPull) cur = std::clamp(AppSettings().pullMode, 0, (int)items.size() - 1);
    else if (id == kDiffs) cur = g_app->diffSplit ? 1 : 0;
    else
        for (int i = 0; i < 4; ++i)
            if (kThemes[i] == g_app->theme) cur = i;
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < items.size(); ++i) AppendMenuW(m, MF_STRING, (UINT_PTR)(i + 1), items[i].c_str());
    CheckMenuRadioItem(m, 1, (UINT)items.size(), (UINT)cur + 1, MF_BYCOMMAND);
    POINT pt{ (LONG)r.x, (LONG)(r.b() + DR(2)) };
    ClientToScreen(hwnd, &pt);
    const int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y,
                                     hwnd, nullptr);
    DestroyMenu(m);
    hover = {};
    if (cmd <= 0 || cmd - 1 == cur) return;
    const int v = cmd - 1;
    switch (id) {
    case kPull:
        AppSettings().pullMode = v;
        OnMain([] { g_app->SaveSettings(); });
        break;
    case kDiffs: OnMain([v] { g_app->SetDiffMode(v == 1); }); break;
    default:
        g_app->theme = kThemes[v];
        OnMain([] {
            g_app->ApplyTheme();
            g_app->SaveSettings();
        });
        break;
    }
}

void SettingsWindow::SetService(int i) {
    if (busy || signIn != kSignInIdle || i < 0 || i >= (int)Forges().size() || i == service) return;
    service = i;
    server.Set(Forges()[(size_t)i].server);
    SetStatus(L"", kInfo);
}

void SettingsWindow::CreateToken() {
    std::wstring why;
    const std::wstring srv = NormalizeServer(server.text, &why);
    if (srv.empty()) {
        SetStatus(why, kError);
        focus = kServer;
        return;
    }
    // Always an https:// address of the server (NormalizeServer): the browser opens it.
    const std::wstring url = TokenPageUrl(Forges()[(size_t)service].forge, srv);
    ShellExecuteW(hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    focus = kToken;  // the token is pasted here next
}

void SettingsWindow::Connect() {
    if (busy || signIn != kSignInIdle || Blank(token.text)) return;
    const Forge f = Forges()[(size_t)service].forge;
    const std::wstring srv = server.text;
    const std::string tok = Narrow(token.text);
    busy = true;
    focus = kNoField;
    SetStatus(L"Checking the token…", kInfo);
    std::thread([f, srv, tok] {
        const TokenCheck check = CheckToken(f, srv, tok);
        RunOnMain([check, tok] { Win().TokenChecked(check, tok); });
    }).detach();
    Invalidate();
}

void SettingsWindow::TokenChecked(const TokenCheck& check, const std::string& tok) {
    busy = false;
    Connected(check, tok, true);
}

// The end of both ways: the account is kept, or why not is shown.
void SettingsWindow::Connected(const TokenCheck& check, const std::string& tok, bool pasted) {
    std::wstring error = check.error;
    if (check.ok && AddAccount(check.account, tok, &error)) {
        if (pasted) WipeToken();
        accounts = LoadAccounts();
        const std::wstring done = L"Connected as " + AccountTitle(check.account) + L".";
        if (check.note.empty()) SetStatus(done, kSuccess);
        else SetStatus(done + L" " + check.note, kWarning);
    } else {
        SetStatus(error.empty() ? L"The account couldn't be connected." : error, kError);
        if (pasted && hwnd) focus = kToken;
    }
    Invalidate();
}

// Signing in with the browser: a worker thread asks GitHub for a code, then waits for the user to authorize
// Mangrove on GitHub (src\core\accounts.cpp). Cancelling, or closing the window, leaves it to stop by itself.
void SettingsWindow::StartSignIn() {
    if (busy || signIn != kSignInIdle) return;
    const Forge f = Forges()[(size_t)service].forge;
    const std::wstring srv = server.text;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    signInCancel = cancel;
    signIn = kSignInStarting;
    focus = kNoField;
    SetStatus(L"", kInfo);
    std::thread([f, srv, cancel] {
        std::string tok;
        const TokenCheck check = BrowserSignIn(
            f, srv,
            [cancel](const DeviceCode& code) {
                const std::wstring userCode = code.userCode, url = code.verifyUrl;
                RunOnMain([cancel, userCode, url] {
                    if (!*cancel) Win().CodeArrived(userCode, url);
                });
            },
            *cancel, &tok);
        RunOnMain([cancel, check, tok] {
            if (!*cancel) Win().SignedIn(check, tok);
        });
    }).detach();
    Invalidate();
}

void SettingsWindow::CodeArrived(const std::wstring& code, const std::wstring& url) {
    if (signIn == kSignInIdle) return;
    userCode = code;
    verifyUrl = url;
    signIn = kSignInWaiting;
    Invalidate();
}

void SettingsWindow::SignedIn(const TokenCheck& check, const std::string& tok) {
    if (signIn == kSignInIdle) return;
    EndSignIn();
    Connected(check, tok, false);
}

void SettingsWindow::EndSignIn() {
    if (signInCancel) *signInCancel = true;  // the worker stops at its next look, and its results are dropped
    signInCancel.reset();
    signIn = kSignInIdle;
    userCode.clear();
    verifyUrl.clear();
    Invalidate();
}

// The code goes to the clipboard, ready to paste on the page; the page is always the server's (BrowserSignIn
// checks it).
void SettingsWindow::OpenSignInPage() {
    if (signIn != kSignInWaiting || verifyUrl.empty()) return;
    CopyToClipboard(hwnd, userCode);
    ShellExecuteW(hwnd, L"open", verifyUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void SettingsWindow::WipeToken() {
    std::fill(token.text.begin(), token.text.end(), L'\0');
    token.Set(L"");
    token.layout.Reset();
}

void SettingsWindow::Ask(const Prompt& p, std::function<void(int)> onResult) {
    dialog.active = true;
    dialog.title = p.title;
    dialog.message = p.message;
    dialog.buttons = p.buttons;
    dialog.defButton = p.defaultButton;
    dialog.danger = p.danger;
    dialog.onResult = std::move(onResult);
    focus = kNoField;
    Invalidate();
}

void SettingsWindow::CloseDialog(int result) {
    auto fn = std::move(dialog.onResult);
    dialog = DialogState{};
    if (fn) fn(result);
    Invalidate();
}

// States the interface test takes pictures of: 1 a token typed in (shown as dots), the form in view; 2 signing
// in with the browser, a code shown (no request is made); 0 back to the start.
void SettingsWindow::ForSnapshot(int state) {
    WipeToken();
    EndSignIn();
    focus = kNoField;
    if (state == 1) {
        token.Set(L"ghp_" + std::wstring(36, L'x'));
        focus = kToken;
        scroll.To(1e9, true);
    } else if (state == 2) {
        signIn = kSignInWaiting;
        userCode = L"WDJB-MJHT";
        verifyUrl = L"https://github.com/login/device";
        scrollToEnd = true;
    } else {
        scroll.To(0, true);
    }
    Invalidate();
}

}  // namespace

// ============================================================================ the window's interface

void ShowSettings(int page) {
    const float s = g_S;
    Win().Show(page);
    g_S = s;
}

void CloseSettings() { Win().Close(); }

void RefreshSettings(bool theme) {
    if (theme) Win().ThemeChanged();
    else Win().Invalidate();
}

HWND SettingsWindowHandle() { return Win().hwnd; }

void SettingsForSnapshot(int state) { Win().ForSnapshot(state); }
