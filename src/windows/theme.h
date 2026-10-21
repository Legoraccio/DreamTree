#pragma once
#include "common.h"

// Interface colors. The palette in use is chosen at run time: the Windows themes follow the system like the
// Windows 11 apps do (light or dark mode, accent color, contrast themes); the Mangrove theme is the original
// deep navy with a gold accent. Painters read these globals every frame, so switching only needs a repaint.
namespace th {

constexpr Color C(uint32_t rgb, float a = 1.f) {
    return { ((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a };
}
inline Color A(Color c, float a) { c.a = a; return c; }
inline Color Mix(Color a, Color b, float t) {
    return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
}

enum class Theme : uint8_t { System, Light, Dark, Mangrove };  // the user's choice

// Loads the palette of a theme, reading the Windows settings it depends on. A Windows contrast theme
// takes precedence over any choice.
void Apply(Theme t);
bool IsDark();          // the palette in use has dark surfaces
bool IsHighContrast();  // a Windows contrast theme is on: opaque system colors

Color OnColor(const Color& fill);  // text on a colored fill: near black or white, whichever reads better
Color TintText(const Color& c);    // label of a button tinted with color c

// Surfaces (alpha < 1 lets the Mica backdrop show through)
extern Color TitleBg, ToolbarBg, PanelBg, GraphBg, HeaderBg, StatusBg, SunkenBg;
extern Color Card, CardStroke, Divider, DividerSoft, ControlStroke, InputLine;  // control border, text box underline
extern Color Hover, Faint, Subtle, SubtleHover, RowHover, RowSelected, LineSelected;
extern Color InputBg, InputSolid, Solid, NodeBg;
extern Color PopupBg, ToastBg, DialogBg, PopupStroke, Shadow, Scrim, ScrollbarThumb, ScrollbarTrack;

// Text
extern Color Text, TextSec, TextDim, TextSelected, ToolOn, ToolOff, Caret;

// Accent: gold in the Mangrove theme, the Windows accent color otherwise
extern Color Accent, AccentHi, AccentDim, AccentText, TextOnAccent;

// File changes and messages
extern Color Modified, Added, Deleted, Renamed, Conflict, Danger, DangerHi, ErrorBg, ErrorText;

// Diff view
extern Color DiffAddBg, DiffAddHl, DiffAddMark, DiffDelBg, DiffDelHl, DiffDelMark, DiffHunkBg, DiffHunkText, DiffGutter, DiffFiller, CodeText;

// Graph lanes (lane 0 is the accent)
constexpr int LaneCount = 10;
extern Color Lanes[LaneCount];
inline Color Lane(int i) { return Lanes[((i % LaneCount) + LaneCount) % LaneCount]; }

// The Mangrove logo keeps its gold in every theme.
inline constexpr Color LogoGold = C(0xE3B341), LogoGoldHi = C(0xF2C95C);

}  // namespace th
