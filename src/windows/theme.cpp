// Theme palettes: DreamTree (navy and gold) and the Windows themes (light or dark, the user's accent color,
// contrast themes), with the Windows settings they are built from.

#include "theme.h"
#include <roapi.h>
#include <windows.ui.viewmanagement.h>
#include <wrl/wrappers/corewrappers.h>

namespace th {

Color TitleBg, ToolbarBg, PanelBg, GraphBg, HeaderBg, StatusBg, SunkenBg;
Color Card, CardStroke, Divider, DividerSoft, ControlStroke, InputLine;
Color Hover, Faint, Subtle, SubtleHover, RowHover, RowSelected, LineSelected;
Color InputBg, InputSolid, Solid, NodeBg;
Color PopupBg, ToastBg, DialogBg, PopupStroke, Shadow, Scrim, ScrollbarThumb, ScrollbarTrack;
Color Text, TextSec, TextDim, TextSelected, ToolOn, ToolOff, Caret;
Color Accent, AccentHi, AccentDim, AccentText, TextOnAccent;
Color Modified, Added, Deleted, Renamed, Conflict, Danger, DangerHi, ErrorBg, ErrorText;
Color DiffAddBg, DiffAddHl, DiffAddMark, DiffDelBg, DiffDelHl, DiffDelMark, DiffHunkBg, DiffHunkText, DiffGutter, DiffFiller, CodeText;
Color Lanes[LaneCount];

namespace {

bool g_dark = true, g_contrast = false;
constexpr Color White = C(0xFFFFFF), Black = C(0x000000);

// The accent color and the lighter and darker shades Windows derives from it.
struct AccentShades {
    Color light3, light2, light1, base, dark1, dark2, dark3;
};

Color FromBytes(uint8_t r, uint8_t g, uint8_t b) { return C((uint32_t)r << 16 | (uint32_t)g << 8 | b); }

// Windows.UI.ViewManagement.UISettings: the shades exactly as the Windows apps use them.
bool AccentFromUISettings(AccentShades& s) {
    using namespace ABI::Windows::UI::ViewManagement;
    ComPtr<IInspectable> insp;
    if (FAILED(RoActivateInstance(Microsoft::WRL::Wrappers::HStringReference(RuntimeClass_Windows_UI_ViewManagement_UISettings).Get(),
                                  &insp)))
        return false;
    ComPtr<IUISettings3> ui;
    if (FAILED(insp.As(&ui))) return false;
    auto get = [&](UIColorType type, Color& out) {
        ABI::Windows::UI::Color c{};
        if (FAILED(ui->GetColorValue(type, &c))) return false;
        out = FromBytes(c.R, c.G, c.B);
        return true;
    };
    return get(UIColorType_AccentLight3, s.light3) && get(UIColorType_AccentLight2, s.light2) && get(UIColorType_AccentLight1, s.light1) &&
           get(UIColorType_Accent, s.base) && get(UIColorType_AccentDark1, s.dark1) && get(UIColorType_AccentDark2, s.dark2) &&
           get(UIColorType_AccentDark3, s.dark3);
}

// The same shades as Explorer stores them (light 3 .. dark 3, then an unused one; RGBA bytes).
bool AccentFromRegistry(AccentShades& s) {
    uint8_t p[32] = {};
    DWORD n = sizeof p;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent", L"AccentPalette",
                     RRF_RT_REG_BINARY, nullptr, p, &n) != ERROR_SUCCESS ||
        n < 28)
        return false;
    Color* out[] = { &s.light3, &s.light2, &s.light1, &s.base, &s.dark1, &s.dark2, &s.dark3 };
    for (int i = 0; i < 7; ++i) *out[i] = FromBytes(p[i * 4], p[i * 4 + 1], p[i * 4 + 2]);
    return true;
}

// Shades made from one color, when Windows can't be asked (and for DREAMTREE_ACCENT, used by UI tests).
AccentShades ShadesOf(Color base) {
    return { Mix(base, White, 0.62f), Mix(base, White, 0.42f), Mix(base, White, 0.22f), base,
             Mix(base, Black, 0.24f), Mix(base, Black, 0.44f), Mix(base, Black, 0.64f) };
}

AccentShades ReadAccent() {
    wchar_t env[16];
    if (GetEnvironmentVariableW(L"DREAMTREE_ACCENT", env, 16) == 6) return ShadesOf(C(wcstoul(env, nullptr, 16)));
    AccentShades s;
    if (AccentFromUISettings(s) || AccentFromRegistry(s)) return s;
    return { C(0xA6D8FF), C(0x76B9ED), C(0x429CE3), C(0x0078D4), C(0x005A9E), C(0x004275), C(0x002642) };  // Windows' default blue
}

// "Choose your mode" > app mode in the Windows settings.
bool SystemAppsDark() {
    DWORD light = 1, n = sizeof light;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD, nullptr, &light, &n) != ERROR_SUCCESS)
        return false;
    return light == 0;
}

bool SystemHighContrast() {
    if (GetEnvironmentVariableW(L"DREAMTREE_CONTRAST", nullptr, 0) > 0) return true;  // UI tests
    HIGHCONTRASTW hc{ sizeof hc };
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

float Luminance(const Color& c) {
    auto lin = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    return 0.2126f * lin(c.r) + 0.7152f * lin(c.g) + 0.0722f * lin(c.b);
}

// Status colors tuned for dark or for light surfaces: the Windows themes use the WinUI caution, success,
// critical and attention colors, the DreamTree theme keeps its own.
void StatusColors(bool dark, bool fluent) {
    if (dark && !fluent) {
        Modified = C(0xE8A33D);
        Added = C(0x5CC46A);
        Deleted = C(0xE5534B);
        Renamed = C(0x4C86F6);
        Conflict = C(0xF0558C);
        Danger = Deleted;
        DangerHi = C(0xF0655D);
    } else if (dark) {
        Modified = C(0xF2C14E);
        Added = C(0x6CCB5F);
        Deleted = C(0xFF99A4);
        Renamed = C(0x60CDFF);
        Conflict = C(0xF0558C);
        Danger = C(0xC42B1C);
        DangerHi = C(0xD13438);
    } else {
        Modified = C(0xA8650A);
        Added = C(0x0F7B0F);
        Deleted = C(0xC42B1C);
        Renamed = C(0x005FB8);
        Conflict = C(0xB4009E);
        Danger = C(0xC42B1C);
        DangerHi = C(0xA82416);
    }
}

void LaneColors(bool dark, const Color& first) {
    static constexpr uint32_t kDark[LaneCount] = { 0xE3B341, 0x1FB0D6, 0x4F7DFF, 0x9D5CFF, 0xD14FC4,
                                                   0xF0558C, 0xF47E3E, 0xA6D84B, 0x3FCF8E, 0x2BC4C4 };
    static constexpr uint32_t kLight[LaneCount] = { 0xB3870F, 0x0E86A8, 0x3557D8, 0x7C3AED, 0xB42EA8,
                                                    0xD12A69, 0xC9561A, 0x5C8F1A, 0x16935E, 0x138F8F };
    for (int i = 0; i < LaneCount; ++i) Lanes[i] = C((dark ? kDark : kLight)[i]);
    Lanes[0] = first;
}

// The original look: deep navy surfaces over Mica, gold accent.
void LoadDreamTree() {
    g_dark = true;
    TitleBg = C(0x0C1834, 0.74f);
    ToolbarBg = C(0x0F1A33, 0.94f);
    PanelBg = C(0x0B1630, 0.86f);
    GraphBg = C(0x08101F, 0.96f);
    HeaderBg = C(0x0C152A, 0.97f);
    StatusBg = C(0x070E1E, 0.78f);
    SunkenBg = C(0x0A1430, 0.55f);
    Card = C(0x13214A, 0.75f);
    CardStroke = C(0xFFFFFF, 0.05f);
    Divider = C(0x1B2948);
    DividerSoft = C(0x182543, 0.7f);
    ControlStroke = C(0xFFFFFF, 0.08f);
    InputLine = C(0xFFFFFF, 0.30f);
    Hover = C(0xFFFFFF, 0.045f);
    Faint = C(0xFFFFFF, 0.03f);
    Subtle = C(0xFFFFFF, 0.06f);
    SubtleHover = C(0xFFFFFF, 0.10f);
    RowHover = C(0x15254A, 0.75f);
    RowSelected = C(0x1D3366, 0.95f);
    LineSelected = C(0xE3B341, 0.17f);
    InputBg = C(0x060C1A, 0.70f);
    InputSolid = C(0x050A16, 0.98f);
    Solid = C(0x0B1428);
    NodeBg = C(0x0A1328);
    PopupBg = C(0x1A2A55, 0.98f);
    ToastBg = C(0x14224A, 0.97f);
    DialogBg = C(0x13204A, 0.99f);
    PopupStroke = C(0xFFFFFF, 0.10f);
    Shadow = C(0x000000, 0.35f);
    Scrim = C(0x02050C, 0.55f);
    ScrollbarThumb = C(0xFFFFFF, 0.28f);
    ScrollbarTrack = C(0xFFFFFF, 0.035f);
    Text = C(0xE6EBF5);
    TextSec = C(0x93A0BD);
    TextDim = C(0x5E6A87);
    TextSelected = C(0xFFFFFF);
    ToolOn = C(0xC9D2E3);
    ToolOff = C(0x56627D);
    Accent = C(0xE3B341);
    AccentHi = C(0xF2C95C);
    AccentDim = C(0xE3B341, 0.16f);
    AccentText = Accent;
    TextOnAccent = C(0x1A1306);
    Caret = Accent;
    StatusColors(true, false);
    ErrorBg = C(0x3A1418, 0.98f);
    ErrorText = C(0xFFD9D6);
    DiffAddBg = C(0x2EA043, 0.15f);
    DiffAddHl = C(0x3FB950, 0.40f);
    DiffAddMark = C(0x56D364);
    DiffDelBg = C(0xDA3633, 0.16f);
    DiffDelHl = C(0xF85149, 0.42f);
    DiffDelMark = C(0xFF7B72);
    DiffHunkBg = C(0x172A55, 0.92f);
    DiffHunkText = C(0x9FB3D9);
    DiffGutter = C(0x040A16, 0.55f);
    DiffFiller = C(0x000000, 0.16f);
    CodeText = C(0xD6DEEB);
    LaneColors(true, C(0xE3B341));
}

// Windows 11 app colors (the WinUI "dark" and "light" resources) with the user's accent color.
void LoadWindows(bool dark, const AccentShades& s) {
    g_dark = dark;
    if (dark) {
        TitleBg = C(0x000000, 0.f);  // Mica Alt shows as is behind the tabs and the navigation pane
        ToolbarBg = C(0x2B2B2B, 0.84f);
        PanelBg = C(0x000000, 0.f);
        GraphBg = C(0x272727, 0.92f);  // the content card
        HeaderBg = C(0x202020, 0.96f);
        StatusBg = C(0x1C1C1C, 0.82f);
        SunkenBg = C(0x000000, 0.14f);
        Card = C(0xFFFFFF, 0.05f);
        CardStroke = C(0xFFFFFF, 0.06f);
        Divider = C(0xFFFFFF, 0.085f);
        DividerSoft = C(0xFFFFFF, 0.06f);
        ControlStroke = C(0xFFFFFF, 0.07f);
        InputLine = C(0xFFFFFF, 0.45f);
        Hover = C(0xFFFFFF, 0.06f);
        Faint = C(0xFFFFFF, 0.03f);
        Subtle = C(0xFFFFFF, 0.06f);
        SubtleHover = C(0xFFFFFF, 0.10f);
        RowHover = C(0xFFFFFF, 0.04f);
        RowSelected = C(0xFFFFFF, 0.07f);  // with an accent pill, like WinUI lists
        LineSelected = A(s.light2, 0.20f);
        InputBg = C(0xFFFFFF, 0.06f);
        InputSolid = C(0x1F1F1F, 0.98f);  // a focused text box
        Solid = C(0x2B2B2B);
        NodeBg = C(0x262626);  // the card, opaque: rings around the graph nodes
        PopupBg = C(0x2C2C2C, 0.98f);
        ToastBg = C(0x2B2B2B, 0.98f);
        DialogBg = C(0x2B2B2B, 0.99f);
        PopupStroke = C(0xFFFFFF, 0.09f);
        Shadow = C(0x000000, 0.40f);
        Scrim = C(0x000000, 0.40f);
        ScrollbarThumb = C(0xFFFFFF, 0.36f);
        ScrollbarTrack = C(0xFFFFFF, 0.04f);
        Text = C(0xFFFFFF);
        TextSec = C(0xC8C8C8);
        TextDim = C(0x8B8B8B);
        TextSelected = C(0xFFFFFF);
        ToolOn = C(0xDADADA);
        ToolOff = C(0x707070);
        Accent = s.light2;
        AccentHi = s.light3;
        AccentDim = A(s.light2, 0.18f);
        AccentText = s.light3;
        ErrorBg = C(0x442726, 0.98f);
        ErrorText = C(0xFFDAD6);
        DiffAddBg = C(0x2EA043, 0.15f);
        DiffAddHl = C(0x3FB950, 0.40f);
        DiffAddMark = C(0x56D364);
        DiffDelBg = C(0xDA3633, 0.16f);
        DiffDelHl = C(0xF85149, 0.42f);
        DiffDelMark = C(0xFF7B72);
        DiffHunkBg = A(s.light2, 0.13f);
        DiffHunkText = C(0xB4B4B4);
        DiffGutter = C(0x000000, 0.20f);
        DiffFiller = C(0x000000, 0.18f);
        CodeText = C(0xD4D4D4);
    } else {
        TitleBg = C(0xFFFFFF, 0.f);
        ToolbarBg = C(0xFAFAFA, 0.80f);
        PanelBg = C(0xFFFFFF, 0.f);
        GraphBg = C(0xFDFDFD, 0.92f);
        HeaderBg = C(0xF7F7F7, 0.97f);
        StatusBg = C(0xF2F2F2, 0.86f);
        SunkenBg = C(0x000000, 0.03f);
        Card = C(0xFFFFFF, 0.80f);
        CardStroke = C(0x000000, 0.07f);
        Divider = C(0x000000, 0.10f);
        DividerSoft = C(0x000000, 0.07f);
        ControlStroke = C(0x000000, 0.08f);
        InputLine = C(0x000000, 0.45f);
        Hover = C(0x000000, 0.045f);
        Faint = C(0x000000, 0.025f);
        Subtle = C(0x000000, 0.04f);
        SubtleHover = C(0x000000, 0.075f);
        RowHover = C(0x000000, 0.03f);
        RowSelected = C(0x000000, 0.055f);
        LineSelected = A(s.dark1, 0.16f);
        InputBg = C(0xFFFFFF, 0.75f);
        InputSolid = C(0xFFFFFF, 0.98f);
        Solid = C(0xFFFFFF);
        NodeBg = C(0xFCFCFC);
        PopupBg = C(0xF9F9F9, 0.98f);
        ToastBg = C(0xFBFBFB, 0.98f);
        DialogBg = C(0xFFFFFF, 0.99f);
        PopupStroke = C(0x000000, 0.10f);
        Shadow = C(0x000000, 0.14f);
        Scrim = C(0x000000, 0.25f);
        ScrollbarThumb = C(0x000000, 0.36f);
        ScrollbarTrack = C(0x000000, 0.04f);
        Text = C(0x1B1B1B);
        TextSec = C(0x5D5D5D);
        TextDim = C(0x8E8E8E);
        TextSelected = C(0x1B1B1B);
        ToolOn = C(0x3B3B3B);
        ToolOff = C(0xA6A6A6);
        Accent = s.dark1;
        AccentHi = Mix(s.dark1, White, 0.12f);
        AccentDim = A(s.dark1, 0.12f);
        AccentText = s.dark2;
        ErrorBg = C(0xFDE7E9, 0.98f);
        ErrorText = C(0x9C1C10);
        DiffAddBg = C(0x2DA44E, 0.14f);
        DiffAddHl = C(0x2DA44E, 0.33f);
        DiffAddMark = C(0x1A7F37);
        DiffDelBg = C(0xCF222E, 0.11f);
        DiffDelHl = C(0xCF222E, 0.27f);
        DiffDelMark = C(0xCF222E);
        DiffHunkBg = A(s.dark1, 0.09f);
        DiffHunkText = C(0x5D5D5D);
        DiffGutter = C(0x000000, 0.035f);
        DiffFiller = C(0x000000, 0.045f);
        CodeText = C(0x1F1F1F);
    }
    TextOnAccent = OnColor(Accent);
    Caret = Text;
    StatusColors(dark, true);
    LaneColors(dark, Accent);
}

// A Windows contrast theme: the system colors, opaque everywhere (no Mica, no translucency).
void LoadContrast() {
    auto sys = [](int i) { const COLORREF c = GetSysColor(i); return FromBytes(GetRValue(c), GetGValue(c), GetBValue(c)); };
    const Color window = sys(COLOR_WINDOW), text = sys(COLOR_WINDOWTEXT), high = sys(COLOR_HIGHLIGHT), highText = sys(COLOR_HIGHLIGHTTEXT);
    const Color gray = sys(COLOR_GRAYTEXT), link = sys(COLOR_HOTLIGHT), face = sys(COLOR_BTNFACE);
    g_dark = Luminance(window) < 0.4f;
    TitleBg = ToolbarBg = PanelBg = GraphBg = HeaderBg = StatusBg = window;
    SunkenBg = face;
    Card = window;
    CardStroke = Divider = DividerSoft = ControlStroke = InputLine = text;
    Hover = Faint = Subtle = RowHover = Mix(window, high, 0.25f);
    SubtleHover = Mix(window, high, 0.45f);
    RowSelected = LineSelected = Mix(window, high, 0.55f);
    InputBg = InputSolid = Solid = NodeBg = window;
    PopupBg = ToastBg = DialogBg = window;
    PopupStroke = text;
    Shadow = C(0x000000, 0.f);
    Scrim = C(0x000000, 0.5f);
    ScrollbarThumb = text;
    ScrollbarTrack = face;
    Text = TextSec = ToolOn = text;
    TextDim = ToolOff = gray;
    TextSelected = text;
    Accent = AccentHi = high;
    AccentDim = Mix(window, high, 0.35f);
    AccentText = link;
    TextOnAccent = highText;
    Caret = text;
    StatusColors(g_dark, true);
    Danger = Deleted;
    ErrorBg = window;
    ErrorText = text;
    DiffAddBg = Mix(window, Added, 0.22f);
    DiffAddHl = Mix(window, Added, 0.45f);
    DiffAddMark = Added;
    DiffDelBg = Mix(window, Deleted, 0.22f);
    DiffDelHl = Mix(window, Deleted, 0.45f);
    DiffDelMark = Deleted;
    DiffHunkBg = face;
    DiffHunkText = text;
    DiffGutter = face;
    DiffFiller = face;
    CodeText = text;
    LaneColors(g_dark, high);
}

}  // namespace

void Apply(Theme t) {
    g_contrast = SystemHighContrast();
    if (g_contrast) LoadContrast();
    else if (t == Theme::DreamTree) LoadDreamTree();
    else LoadWindows(t == Theme::System ? SystemAppsDark() : t == Theme::Dark, ReadAccent());
}

bool IsDark() { return g_dark; }
bool IsHighContrast() { return g_contrast; }

Color OnColor(const Color& fill) {
    // The higher contrast ratio of the two (WCAG relative luminance).
    const float l = Luminance(fill);
    return (l + 0.05f) / 0.05f > 1.05f / (l + 0.05f) ? C(0x111111) : White;
}

Color TintText(const Color& c) {
    if (g_contrast) return Text;
    return g_dark ? Mix(c, White, 0.3f) : Mix(c, Black, 0.3f);
}

}  // namespace th
