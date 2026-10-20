// Painting. Every frame is drawn immediately from the current state; interactive regions are
// registered as zones while painting, so hit testing always matches what is on screen.
//
// The layout follows the Windows 11 apps: tabs in the title bar, a command bar, a navigation pane on the
// Mica backdrop, and a content card holding the history and the details of the selection.

#include "app.h"
#include "theme.h"

using namespace th;

namespace {
constexpr float kTitleH = 46.f;       // title bar with the tabs (DIP)
constexpr float kBarH = 48.f;         // command bar
constexpr float kNavW = 232.f;        // navigation pane
constexpr float kNavCompactW = 56.f;  // navigation pane showing icons only
constexpr float kLaneW = 16.f;        // distance between graph lanes
constexpr float kPageHeadH = 91.f;    // history page header: title, search, scope
constexpr float kListHeadH = 24.f;    // column titles
constexpr float kFileRowH = 34.f;     // changed files
constexpr float kFilePitch = 36.f;

Color StatusColor(wchar_t st) {
    switch (st) {
    case L'A': case L'?': return Added;
    case L'D': return Deleted;
    case L'R': case L'C': return Renamed;
    case L'U': return Conflict;
    default: return Modified;
    }
}

RectF Intersect(const RectF& a, const RectF& b) {
    const float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    const float x1 = std::min(a.r(), b.r()), y1 = std::min(a.b(), b.b());
    return { x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0) };
}

// Advance of one F::Mono character: the line counts are laid out without measuring each of them.
float MonoAdvance(Gfx& gfx) {
    static float w = 0, scale = 0;
    if (scale != gfx.scale) {
        w = gfx.Measure(L"0000000000", F::Mono) / 10.f;
        scale = gfx.scale;
    }
    return w;
}

// Files with changes, staged or not, each counted once (cached per status).
int ChangedFiles(RepoTab& t) {
    if (!t.status) return 0;
    if (t.countedStatus.lock() != t.status) {
        t.changedFiles = t.status->ChangedFiles();
        t.countedStatus = t.status;
    }
    return t.changedFiles;
}

}  // namespace

// ============================================================================ frame

void App::Layout() {
    const float W = (float)gfx.Width(), H = (float)gfx.Height();
    const float titleH = std::max(DR(kTitleH), std::round(caption.b()));
    rcTitle = { 0, 0, W, titleH };
    rcToolbar = { 0, titleH, W, DR(kBarH) };
    const float top = rcToolbar.b();
    RepoTab* t = Active();
    const bool repo = t && !t->IsStart() && t->error.empty() && t->data;
    navCompact = !sideVisible || W < DR(1000);
    const float navW = repo ? DR(navCompact ? kNavCompactW : kNavW) : 0.f;
    rcNav = { 0, top, navW, H - top };
    rcCard = { navW, top, W - navW, H - top };
    const float dw = repo && detailVisible ? std::min(DR(detailW), std::round(rcCard.w * 0.45f)) : 0.f;
    rcDetail = { W - dw, top, dw, H - top };
    rcGraph = { rcCard.x, top, rcCard.w - dw, H - top };
}

void App::Paint() {
    if (!gfx.Begin()) return;
    const double now = NowMs();
    const float dt = lastFrame > 0 ? (float)std::min(64.0, now - lastFrame) : 16.f;
    lastFrame = now;
    zones.clear();
    tipText.clear();
    animating = false;  // painters set it when they need another frame
    Layout();

    bool anim = false;
    RepoTab* t = Active();
    if (t) {
        anim |= t->gs.Step(dt);
        anim |= t->ss.Step(dt);
        anim |= t->ds.Step(dt);
        if (t->diff.open) anim |= t->diffUi.vs.Step(dt);
    }
    PaintTitle();
    PaintCommandBar();
    if (t) {
        const RectF below{ 0, rcToolbar.b(), (float)gfx.Width(), (float)gfx.Height() - rcToolbar.b() };
        if (t->IsStart()) PaintStart(*t, below);
        else if (!t->error.empty()) PaintError(*t, below);
        else if (!t->data) { PaintLoading(*t, below); anim = true; }
        else {
            gfx.Fill(below, PanelBg);
            PaintNav(*t);
            // The content card: rounded top-left corner, hairline on its top and left edges (the other
            // corners lie outside the window).
            const RectF card{ rcCard.x, rcCard.y, rcCard.w + DR(16), rcCard.h + DR(16) };
            gfx.FillR(card, DR(8), GraphBg);
            gfx.StrokeR(card, DR(8), CardStroke);
            if (t->diff.open) PaintDiff(*t);
            else PaintHistory(*t);
            if (rcDetail.w > 0) PaintDetails(*t);
            if (!t->diff.open) PaintRefPopup(*t);
        }
        if (t->loading || t->opBusy || (t->inflight && !t->details)) anim = true;
    }
    PaintToasts();
    PaintTooltip();
    PaintDialog();
    PaintDebug();
    frameMs = NowMs() - now;
    gfx.End();

    // Text fields blink their caret on a timer instead of rendering continuously.
    if (searchFocus || inlineInput.active || editFocus) SetTimer(hwnd, 3, 530, nullptr);
    else KillTimer(hwnd, 3);

    anim |= animating;
    animating = anim;
    if (anim) Invalidate();
    else lastFrame = 0;
}

// ============================================================================ icons and controls
// Free functions drawing with the Gfx of a window: the main window and the settings window share them.

void DrawLogo(Gfx& gfx, float cx, float cy, float s) {
    // Gold commit-graph "tree": a trunk with two branches and three commits.
    const float sw = std::max(1.5f, s * 0.11f);
    const float r = s * 0.13f;
    const float xt = cx - s * 0.16f, yb = cy + s * 0.36f, yt = cy - s * 0.36f;
    gfx.Line(xt, yb, xt, yt + r, LogoGold, sw, gfx.RoundCap());
    auto path = gfx.Path();
    ComPtr<ID2D1GeometrySink> sk;
    path->Open(&sk);
    sk->BeginFigure({ xt, cy + s * 0.18f }, D2D1_FIGURE_BEGIN_HOLLOW);
    sk->AddBezier(D2D1::BezierSegment({ xt, cy - s * 0.02f }, { cx + s * 0.28f, cy + s * 0.08f }, { cx + s * 0.28f, cy - s * 0.12f }));
    sk->EndFigure(D2D1_FIGURE_END_OPEN);
    sk->Close();
    gfx.dc()->DrawGeometry(path.Get(), gfx.B(LogoGold), sw, gfx.RoundCap());
    gfx.Circle(xt, yt + r * 0.6f, r * 1.25f, LogoGold);
    gfx.Circle(cx + s * 0.28f, cy - s * 0.18f, r * 1.15f, LogoGoldHi);
    gfx.Circle(xt, yb - r * 0.4f, r * 1.0f, LogoGold);
}

void DrawBranchIcon(Gfx& gfx, float cx, float cy, float s, const Color& c) {
    const float sw = std::max(1.f, std::round(s * 0.1f * 2.f) / 2.f);
    const float r = s * 0.13f;
    const float xl = cx - s * 0.2f, xr = cx + s * 0.24f;
    const float yt = cy - s * 0.33f, yb = cy + s * 0.33f, ym = cy - s * 0.16f;
    gfx.CircleS(xl, yt, r, c, sw);
    gfx.CircleS(xl, yb, r, c, sw);
    gfx.CircleS(xr, ym, r, c, sw);
    gfx.Line(xl, yt + r, xl, yb - r, c, sw);
    auto path = gfx.Path();
    ComPtr<ID2D1GeometrySink> sk;
    path->Open(&sk);
    sk->BeginFigure({ xr, ym + r }, D2D1_FIGURE_BEGIN_HOLLOW);
    sk->AddBezier(D2D1::BezierSegment({ xr, cy + s * 0.1f }, { xl, cy + s * 0.02f }, { xl, cy + s * 0.2f }));
    sk->EndFigure(D2D1_FIGURE_END_OPEN);
    sk->Close();
    gfx.dc()->DrawGeometry(path.Get(), gfx.B(c), sw);
}

void App::FileIcon(wchar_t st, float cx, float cy) {
    const wchar_t letter = StatusLetter(st);
    gfx.Text(std::wstring_view(&letter, 1), { cx - DR(9), cy - DR(10), DR(18), DR(20) }, F::MonoSemi, StatusColor(st),
             DWRITE_TEXT_ALIGNMENT_CENTER);
}

void DrawAvatar(Gfx& gfx, float cx, float cy, float r, const Author& a) {
    gfx.Circle(cx, cy, r, C(a.color));
    gfx.Text(a.initials, { cx - r - 4, cy - r, 2 * r + 8, 2 * r }, F::Avatar, C(0xFFFFFF, 0.95f), DWRITE_TEXT_ALIGNMENT_CENTER);
}

void DrawSpinner(Gfx& gfx, float cx, float cy, float r, const Color* c) {
    const float ang = (float)std::fmod(NowMs() * 0.0065, 6.283185307);
    auto p = gfx.Path();
    ComPtr<ID2D1GeometrySink> sk;
    p->Open(&sk);
    sk->BeginFigure({ cx + r * std::cos(ang), cy + r * std::sin(ang) }, D2D1_FIGURE_BEGIN_HOLLOW);
    sk->AddArc(D2D1::ArcSegment({ cx + r * std::cos(ang + 4.4f), cy + r * std::sin(ang + 4.4f) }, { r, r }, 0.f,
                                D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
    sk->EndFigure(D2D1_FIGURE_END_OPEN);
    sk->Close();
    gfx.dc()->DrawGeometry(p.Get(), gfx.B(c ? *c : Accent), std::max(1.5f, D(2.f)), gfx.RoundCap());
}

void DrawButton(Gfx& gfx, const RectF& r, std::wstring_view label, int style, bool hot, bool enabled, F font) {
    const float rad = DR(4);
    if (style == 1) {  // accent
        gfx.FillR(r, rad, enabled ? (hot ? AccentHi : Accent) : Subtle);
        if (!enabled) gfx.StrokeR(r, rad, ControlStroke);
        gfx.Text(label, r, font, enabled ? TextOnAccent : TextDim, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    if (style == 2) {  // subtle: a fill only while hovered
        if (hot && enabled) gfx.FillR(r, rad, Hover);
    } else {
        gfx.FillR(r, rad, hot && enabled ? SubtleHover : Subtle);
        gfx.StrokeR(r, rad, ControlStroke);
    }
    gfx.Text(label, r, font, enabled ? Text : TextDim, DWRITE_TEXT_ALIGNMENT_CENTER);
}

// A text box frame (WinUI): subtle fill and border, a thin line at the bottom that turns into an accent bar
// while the box has the focus.
void TextBoxFrame(Gfx& gfx, const RectF& r, bool focused, bool error) {
    gfx.FillR(r, DR(4), focused ? InputSolid : InputBg);
    gfx.StrokeR(r, DR(4), error ? Deleted : ControlStroke);
    if (focused || error) gfx.FillR({ r.x + DR(2), r.b() - DR(2), r.w - DR(4), DR(2) }, DR(1), error ? Deleted : Accent);
    else gfx.Fill({ r.x + DR(3), r.b() - 1, r.w - DR(6), 1 }, InputLine);
}

void App::PaintScrollbar(const RectF& area, Scroller& s, int id) {
    scrollArea[id] = area;
    RectF thumb;
    if (!ScrollThumb(area, s, thumb)) return;
    const bool active = Hov(Hit::Scroll, id) || (id == 0 && drag == Drag::ScrollGraph) || (id == 1 && drag == Drag::ScrollSide) ||
                        (id == 2 && drag == Drag::ScrollDetail) || (id == 3 && drag == Drag::ScrollDiff);
    const RectF track{ area.r() - DR(12), area.y, DR(12), area.h };
    if (active) gfx.FillR(track.Inset(DR(2), DR(2)), DR(5), ScrollbarTrack);
    const float w = active ? DR(6) : DR(3);
    const RectF vis{ thumb.r() - DR(3) - w, thumb.y + DR(3), w, thumb.h - DR(6) };
    gfx.FillR(vis, w * 0.5f, active ? A(TextSec, 0.9f) : ScrollbarThumb);
    AddZone(track, Hit::Scroll, id);
}

// ============================================================================ title bar (tabs)

void App::PaintTitle() {
    gfx.Fill(rcTitle, TitleBg);
    const float tabH = DR(38), top = rcTitle.b() - tabH;
    Logo(DR(23), top + tabH * 0.5f, DR(18));
    float x = DR(44);
    const float right = (caption.w > 0 ? caption.x : rcTitle.r()) - DR(48) - DR(40);  // a drag area stays free
    const int n = (int)tabs.size();
    float tw = DR(200);
    if (n > 0 && n * (tw + DR(2)) > right - x) tw = std::max(DR(56), std::floor((right - x) / n) - DR(2));
    gfx.PushClip(rcTitle);
    for (int i = 0; i < n; ++i) {
        RepoTab& tb = *tabs[(size_t)i];
        const RectF r{ x, top, tw, tabH };
        const bool act = i == active;
        const bool hov = Hov(Hit::Tab, i) || Hov(Hit::TabClose, i);
        if (act) gfx.FillR({ r.x, r.y, r.w, r.h + DR(12) }, DR(8), ToolbarBg);  // joins the command bar below
        else if (hov) gfx.FillR({ r.x + DR(2), r.y + DR(3), r.w - DR(4), r.h - DR(6) }, DR(6), Hover);
        const bool nextHot = i + 1 < n && (i + 1 == active || Hov(Hit::Tab, i + 1) || Hov(Hit::TabClose, i + 1));
        if (!act && !hov && !nextHot && i + 1 < n) gfx.VLine(r.r() + DR(1), r.y + DR(11), r.b() - DR(9), DividerSoft);

        const float icx = r.x + DR(21);
        if (tb.loading) Spinner(icx, r.cy(), DR(6));
        else if (tb.IsStart()) gfx.Glyph(0xE8B7, { icx - DR(10), r.y, DR(20), r.h }, F::Icon12, TextSec);
        else if (!tb.error.empty()) gfx.Glyph(0xE7BA, { icx - DR(10), r.y, DR(20), r.h }, F::Icon12, Modified);
        else BranchIcon(icx, r.cy(), DR(14), act ? AccentText : TextDim);

        const std::wstring title = tb.IsStart() ? L"New tab" : tb.title;
        const RectF cr{ r.r() - DR(6) - DR(28), r.cy() - DR(14), DR(28), DR(28) };
        gfx.Text(title, { r.x + DR(38), r.y, cr.x - DR(4) - (r.x + DR(38)), r.h }, F::UI, act ? Text : TextSec);
        if (act || hov) {
            if (Hov(Hit::TabClose, i)) gfx.FillR(cr, DR(4), SubtleHover);
            gfx.Glyph(0xE711, cr, F::Icon10, Hov(Hit::TabClose, i) ? Text : TextSec);
        }
        AddZone(r, Hit::Tab, i);
        AddZone(cr, Hit::TabClose, i);
        x += tw + DR(2);
    }
    const RectF pr{ x + DR(2), rcTitle.b() - DR(36), DR(34), DR(34) };
    if (Hov(Hit::TabNew)) gfx.FillR(pr, DR(6), Hover);
    gfx.Glyph(0xE710, pr, F::Icon12, Hov(Hit::TabNew) ? Text : TextSec);
    AddZone(pr, Hit::TabNew);
    gfx.PopClip();
}

// ============================================================================ command bar

void App::PaintCommandBar() {
    const RectF r = rcToolbar;
    gfx.Fill(r, ToolbarBg);
    RepoTab* t = Active();
    const bool repo = t && t->data && t->error.empty() && !t->IsStart();
    // Over the content card the card's own outline takes over from this edge.
    gfx.HLine(r.x, repo ? rcCard.x + DR(8) : r.r(), r.b() - 1, Divider);
    const float bh = DR(36), by = r.y + DR(6);
    float x = r.x + DR(12);

    // Breadcrumb: repository > branch, each a menu.
    auto crumb = [&](const std::wstring& text, const Color& tc, Hit id, bool branch) {
        const float tw = std::min(gfx.Measure(text, F::Repo), DR(260));
        const RectF b{ x, by, DR(34) + tw + DR(4) + DR(16) + DR(8), bh };
        if (Hov(id)) gfx.FillR(b, DR(4), Hover);
        const float icx = b.x + DR(18);
        if (branch) BranchIcon(icx, b.cy(), DR(16), tc);
        else gfx.Glyph(0xE8B7, { icx - DR(12), b.y, DR(24), b.h }, F::Icon16, TextSec);
        gfx.Text(text, { b.x + DR(34), b.y, tw + 2, b.h }, F::Repo, tc);
        gfx.Glyph(0xE70D, { b.x + DR(34) + tw + DR(4), b.y, DR(16), b.h }, F::Icon10, TextSec);
        AddZone(b, id);
        x = b.r();
    };
    const std::wstring repoName = t && !t->IsStart() ? (t->data ? t->data->name : t->title) : L"Open a repository";
    crumb(repoName, Text, Hit::RepoMenu, false);
    if (!repo) return;
    const RepoData& d = *t->data;
    gfx.Text(L"›", { x, r.y, DR(18), r.h - DR(2) }, F::Title, TextDim, DWRITE_TEXT_ALIGNMENT_CENTER);
    x += DR(18);
    const std::wstring br = !d.headBranch.empty() ? d.headBranch : d.detached ? L"HEAD at " + d.headOid.Hex(7) : L"No commits yet";
    crumb(br, AccentText, Hit::BranchMenu, true);
    const float crumbEnd = x;

    // Commands, right-aligned; their labels go first when the bar gets narrow. Pull and Push show how
    // many commits they would bring in or send.
    const Ref* head = HeadRef(d);
    struct Cmd {
        int id;
        const wchar_t* label;
        wchar_t glyph;  // 0: the branch icon
        int badge;
    };
    const Cmd cmds[] = {
        { 8, L"Fetch", 0xE72C, 0 },
        { 2, L"Pull", 0xE896, head ? head->behind : 0 },
        { 3, L"Push", 0xE898, head ? head->ahead : 0 },
        { -1, nullptr, 0, 0 },
        { 4, L"New branch", 0, 0 },
        { 5, L"Stash", 0xE7B8, 0 },
        { -1, nullptr, 0, 0 },
    };
    auto badgeW = [&](int n) { return std::max(DR(18), gfx.Measure(std::to_wstring(n), F::TinySemi) + DR(10)); };
    auto fullW = [&](const Cmd& c) {
        float w = DR(12) + DR(16) + DR(8) + gfx.Measure(c.label, F::UI) + DR(12);
        if (c.badge > 0) w += DR(8) + badgeW(c.badge);
        return w;
    };
    const float sepW = DR(13), moreW = DR(36);
    float total = moreW;
    for (const Cmd& c : cmds) total += c.id < 0 ? sepW : fullW(c);
    const bool labels = total <= r.r() - DR(12) - crumbEnd - DR(24);
    float rx = r.r() - DR(12);
    {
        const RectF b{ rx - moreW, by, moreW, bh };
        const bool hov = Hov(Hit::MoreMenu);
        if (hov) {
            gfx.FillR(b, DR(4), Hover);
            tipText = L"More actions";
            tipAnchor = b;
        }
        gfx.Glyph(0xE712, b, F::Icon16, Text);
        AddZone(b, Hit::MoreMenu);
        rx = b.x;
    }
    for (int i = (int)std::size(cmds) - 1; i >= 0; --i) {
        const Cmd& c = cmds[i];
        if (c.id < 0) {
            gfx.VLine(rx - std::round(sepW * 0.5f), r.cy() - DR(12), r.cy() + DR(12), Divider);
            rx -= sepW;
            continue;
        }
        std::wstring tip;
        const bool enabled = ToolEnabled(t, c.id, &tip);
        const bool running = t->opBusy && t->opButton == c.id;
        const float w = labels ? fullW(c) : c.badge > 0 ? DR(10) + DR(16) + DR(6) + badgeW(c.badge) + DR(8) : DR(36);
        const RectF b{ rx - w, by, w, bh };
        const bool hov = Hov(Hit::ToolButton, c.id);
        if (hov && enabled) gfx.FillR(b, DR(4), Hover);
        const Color fg = running ? AccentText : enabled ? Text : TextDim;
        const float icx = b.x + (labels ? DR(12) : DR(10)) + DR(8);
        if (running) Spinner(icx, b.cy(), DR(7));
        else if (c.glyph) gfx.Glyph(c.glyph, { icx - DR(12), b.y, DR(24), b.h }, F::Icon16, fg);
        else BranchIcon(icx, b.cy(), DR(16), fg);
        float tx = icx + DR(8) + (labels ? DR(8) : DR(6));
        if (labels) {
            const float lw = gfx.Measure(c.label, F::UI);
            gfx.Text(c.label, { tx, b.y, lw + 2, b.h }, F::UI, fg);
            tx += lw + DR(8);
        }
        if (c.badge > 0) {
            const RectF pill{ tx, b.cy() - DR(9), badgeW(c.badge), DR(18) };
            gfx.FillR(pill, DR(9), enabled ? Accent : Subtle);
            gfx.Text(std::to_wstring(c.badge), pill, F::TinySemi, enabled ? TextOnAccent : TextDim, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        AddZone(b, Hit::ToolButton, c.id);  // also when disabled: the tooltip explains why
        if (hov) {
            tipText = tip;
            tipAnchor = b;
        }
        rx = b.x;
    }

    // An operation in progress: git's latest progress line between the breadcrumb and the commands, and an
    // indeterminate progress bar along the bottom edge.
    if (t->opBusy) {
        std::wstring s = t->opLabel + L"…";
        if (!t->opProgress.empty()) s += L"   " + t->opProgress;
        wchar_t secs[32];
        swprintf_s(secs, L"   %.1f s", (NowMs() - t->opStart) / 1000.0);
        s += secs;
        const RectF pr{ crumbEnd + DR(16), r.y, rx - DR(16) - (crumbEnd + DR(16)), r.h };
        if (pr.w > DR(80)) gfx.Text(s, pr, F::Small, AccentText);
        const float seg = std::round(r.w * 0.22f);
        const double ph = std::fmod(NowMs() / 1600.0, 1.0);
        gfx.PushClip(r);
        gfx.FillR({ r.x - seg + (float)ph * (r.w + seg), r.b() - DR(3), seg, DR(3) }, DR(1.5f), Accent);
        gfx.PopClip();
        animating = true;
    }
}

// ============================================================================ start page / loading / error

void App::PaintStart(RepoTab&, const RectF& r) {
    const std::vector<std::wstring>& recent = AppSettings().recent;
    gfx.Fill(r, GraphBg);
    const float cw = std::min(DR(640), r.w - DR(48));
    const float x = std::round(r.cx() - cw * 0.5f);
    float y = r.y + std::max(DR(40), std::round(r.h * 0.12f));

    Logo(x + DR(28), y + DR(28), DR(56));
    gfx.Text(L"DreamTree", { x + DR(72), y - DR(4), cw, DR(46) }, F::Huge, Text);
    gfx.Text(L"A fast, native Git client for Windows", { x + DR(74), y + DR(40), cw, DR(22) }, F::UI, TextSec);
    y += DR(104);

    const RectF ob{ x, y, DR(192), DR(36) };
    gfx.FillR(ob, DR(4), Hov(Hit::StartOpen) ? AccentHi : Accent);
    gfx.Glyph(0xE838, { ob.x + DR(8), ob.y, DR(26), ob.h }, F::Icon16, TextOnAccent);
    gfx.Text(L"Open repository", { ob.x + DR(40), ob.y, ob.w - DR(46), ob.h }, F::UISemi, TextOnAccent);
    AddZone(ob, Hit::StartOpen);
    gfx.Text(L"Ctrl+O   ·   or drop a folder anywhere on this window", { ob.r() + DR(18), y, cw - ob.w - DR(18), ob.h }, F::Small,
             TextDim);
    y += DR(72);

    gfx.Text(L"Recent repositories", { x, y, cw, DR(24) }, F::RowTitle, Text);
    y += DR(34);
    if (recent.empty()) {
        gfx.Text(L"Repositories you open will appear here.", { x, y, cw, DR(24) }, F::UI, TextDim);
        return;
    }
    const float maxY = r.b() - DR(20);
    for (size_t i = 0; i < recent.size() && i < 12; ++i) {
        const RectF rr{ x - DR(10), y, cw + DR(20), DR(48) };
        if (rr.b() > maxY) break;
        const bool hov = Hov(Hit::StartRecent, (int)i) || Hov(Hit::StartRemove, (int)i);
        if (hov) gfx.FillR(rr, DR(6), Hover);
        gfx.Glyph(0xE8B7, { x, y, DR(28), rr.h }, F::Icon16, AccentText);
        gfx.Text(BaseName(recent[i]), { x + DR(38), y + DR(5), cw - DR(80), DR(20) }, F::UISemi, Text);
        gfx.Text(recent[i], { x + DR(38), y + DR(25), cw - DR(80), DR(18) }, F::Small, TextDim);
        AddZone(rr, Hit::StartRecent, (int)i);
        if (hov) {
            const RectF xr{ rr.r() - DR(38), rr.y + DR(10), DR(28), DR(28) };
            if (Hov(Hit::StartRemove, (int)i)) gfx.FillR(xr, DR(4), SubtleHover);
            gfx.Glyph(0xE711, xr, F::Icon10, TextSec);
            AddZone(xr, Hit::StartRemove, (int)i);
        }
        y += DR(50);
    }
}

void App::PaintLoading(RepoTab& t, const RectF& r) {
    gfx.Fill(r, GraphBg);
    const float cy = r.y + r.h * 0.42f;
    Spinner(r.cx(), cy, DR(16));
    gfx.Text(L"Loading " + t.title + L"…", { r.x, cy + DR(26), r.w, DR(24) }, F::UISemi, Text, DWRITE_TEXT_ALIGNMENT_CENTER);
    wchar_t buf[64];
    swprintf_s(buf, L"%.0f ms", NowMs() - t.loadStart);
    gfx.Text(buf, { r.x, cy + DR(50), r.w, DR(20) }, F::Small, TextDim, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void App::PaintError(RepoTab& t, const RectF& r) {
    gfx.Fill(r, GraphBg);
    const float cw = std::min(DR(560), r.w - DR(40));
    const float x = std::round(r.cx() - cw * 0.5f);
    float y = r.y + r.h * 0.22f;
    const bool untrusted = !t.untrusted.empty();
    auto msg = gfx.Layout(t.error, F::Body, cw - DR(48));
    DWRITE_TEXT_METRICS mm{};
    msg->GetMetrics(&mm);
    const float h = DR(72) + mm.height + (untrusted ? DR(40) : 0.f) + DR(64);
    gfx.FillR({ x, y, cw, h }, DR(8), Card);
    gfx.StrokeR({ x, y, cw, h }, DR(8), CardStroke);
    gfx.Glyph(0xE7BA, { x + DR(20), y + DR(18), DR(28), DR(28) }, F::Icon20, Modified);
    gfx.Text(untrusted ? L"Trust this repository?" : L"Can't open this repository", { x + DR(56), y + DR(18), cw - DR(76), DR(28) },
             F::Title, Text);
    gfx.dc()->DrawTextLayout({ x + DR(24), y + DR(60) }, msg.Get(), gfx.B(TextSec));
    float by = y + DR(60) + mm.height + DR(14);
    if (untrusted) {
        gfx.Text(t.untrusted, { x + DR(24), by, cw - DR(48), DR(20) }, F::Mono, AccentText);
        by += DR(32);
    }
    float bx = x + DR(24);
    auto button = [&](const wchar_t* label, Hit id, bool primary) {
        const float w = gfx.Measure(label, F::UISemi) + DR(32);
        const RectF b{ bx, by, w, DR(32) };
        Button(b, label, primary ? 1 : 0, Hov(id), true, primary ? F::UISemi : F::UI);
        AddZone(b, id);
        bx += w + DR(8);
    };
    if (untrusted) button(L"Trust and open", Hit::Trust, true);
    button(L"Retry", Hit::Retry, !untrusted);
    button(L"Close tab", Hit::CloseTab, false);
    if (untrusted)
        gfx.Text(L"Only DreamTree will trust it: your global Git configuration is not modified.",
                 { x + DR(24), y + h + DR(12), cw, DR(18) }, F::Small, TextDim);
}

// ============================================================================ navigation pane

void App::PaintNav(RepoTab& t) {
    const RectF r = rcNav;
    if (t.sideDirty || t.sideCompact != navCompact) BuildSide(t);
    const bool compact = navCompact;
    const RepoData& d = *t.data;
    const float itemX = r.x + DR(8), itemW = r.w - DR(16);
    const RectF settings{ itemX, r.b() - DR(8) - DR(36), itemW, DR(36) };
    const RectF list{ r.x, r.y + DR(10), r.w, settings.y - DR(10) - (r.y + DR(10)) };
    t.ss.view = list.h;
    t.ss.content = t.side.empty() ? 0.0 : (double)DR(t.side.back().top + t.side.back().h) + DR(8);
    t.ss.Clamp();
    const float pos = (float)t.ss.pos;
    gfx.PushClip(list);
    auto first = std::upper_bound(t.side.begin(), t.side.end(), pos / g_S, [](float v, const SideItem& s) { return v < s.top + s.h; });
    for (auto it = first; it != t.side.end(); ++it) {
        const int i = (int)(it - t.side.begin());
        const float y = std::round(list.y + it->top * g_S - pos);
        if (y >= list.b()) break;
        const RectF rr{ itemX, y, itemW, DR(it->h) };
        const RectF zr = Intersect(rr, list);
        const float iconX = compact ? std::round(rr.cx()) : rr.x + DR(20);  // pages and sections
        const float textX = rr.x + DR(40);
        const float base = rr.x + DR(32) + it->depth * DR(14);              // items of a section
        switch (it->kind) {
        case SideKind::Page: {
            const bool hov = Hov(Hit::NavPage, it->section);
            const bool sel = (it->section == 0) == t.selWip;  // Changes while they are selected, else History
            if (sel) {
                gfx.FillR(rr, DR(4), Subtle);
                gfx.FillR({ rr.x, rr.y + DR(10), DR(3), rr.h - DR(20) }, DR(1.5f), Accent);
            } else if (hov) {
                gfx.FillR(rr, DR(4), Hover);
            }
            gfx.Glyph(it->section == 0 ? 0xE8A5 : 0xE81C, { iconX - DR(12), rr.y, DR(24), rr.h }, F::Icon16, Text);
            const int n = it->section == 0 ? ChangedFiles(t) : 0;
            if (!compact) gfx.Text(it->text, { textX, rr.y, rr.r() - DR(44) - textX, rr.h }, F::Nav, Text);
            if (n > 0) {
                const std::wstring s = std::to_wstring(n);
                if (compact) {
                    const float bw = std::max(DR(16), gfx.Measure(s, F::TinySemi) + DR(8));
                    const RectF pill{ iconX + DR(3), rr.y + DR(2), bw, DR(16) };
                    gfx.FillR(pill, DR(8), Accent);
                    gfx.Text(s, pill, F::TinySemi, TextOnAccent, DWRITE_TEXT_ALIGNMENT_CENTER);
                } else {
                    const float bw = std::max(DR(20), gfx.Measure(s, F::Tiny) + DR(12));
                    const RectF pill{ rr.r() - DR(12) - bw, rr.cy() - DR(9), bw, DR(18) };
                    gfx.FillR(pill, DR(9), SubtleHover);
                    gfx.Text(s, pill, F::Tiny, Text, DWRITE_TEXT_ALIGNMENT_CENTER);
                }
            }
            if (hov && compact) {
                tipText = it->text;
                tipAnchor = rr;
            }
            AddZone(zr, Hit::NavPage, it->section);
            continue;
        }
        case SideKind::Header: {
            const bool hov = Hov(Hit::SideHeader, i);
            if (hov) gfx.FillR(rr, DR(4), Hover);
            const RectF ib{ iconX - DR(12), rr.y, DR(24), rr.h };
            switch (it->section) {
            case 0: BranchIcon(iconX, rr.cy(), DR(16), Text); break;
            case 1: gfx.Glyph(0xE753, ib, F::Icon16, Text); break;
            case 2: gfx.Glyph(0xE81E, ib, F::Icon16, Text); break;
            case 3: gfx.Glyph(0xE8EC, ib, F::Icon16, Text); break;
            default: gfx.Glyph(0xE8F1, ib, F::Icon16, Text); break;
            }
            if (!compact) {
                gfx.Glyph(it->expanded ? 0xE70D : 0xE76C, { rr.r() - DR(30), rr.y, DR(20), rr.h }, F::Icon10, TextDim);
                const std::wstring cnt = std::to_wstring(it->count);
                const float cw = gfx.Measure(cnt, F::Small);
                gfx.Text(cnt, { rr.r() - DR(34) - cw, rr.y, cw + 2, rr.h }, F::Small, TextDim);
                gfx.Text(it->text, { textX, rr.y, rr.r() - DR(42) - cw - textX, rr.h }, F::Nav, Text);
            } else if (hov) {
                tipText = it->text;
                tipAnchor = rr;
            }
            AddZone(zr, Hit::SideHeader, i);
            continue;
        }
        case SideKind::Divider:
            gfx.HLine(r.x + DR(16), r.r() - DR(16), y + DR(10), DividerSoft);
            continue;
        case SideKind::Caption:
            gfx.Text(it->text, { rr.x + DR(12), y + DR(4), rr.w - DR(24), DR(17) }, F::SmallSemi, TextDim);
            continue;
        default:
            break;
        }
        // The items of a section, and the recent branches.
        const bool hov = Hov(Hit::SideItem, i);
        bool sel = false;
        if ((it->kind == SideKind::Branch || it->kind == SideKind::Tag || it->kind == SideKind::Recent) && it->index >= 0) {
            const Ref& ref = d.refs[(size_t)it->index];
            sel = ref.commit >= 0 && ref.commit == t.selCommit && !t.selWip;
        } else if (it->kind == SideKind::Stash && it->index >= 0) {
            const StashEntry& s = d.stashes[(size_t)it->index];
            sel = s.commit >= 0 && s.commit == t.selCommit && !t.selWip;
        }
        if (sel) gfx.FillR(rr, DR(4), Subtle);
        else if (hov) gfx.FillR(rr, DR(4), Hover);
        float right = rr.r() - DR(12);
        switch (it->kind) {
        case SideKind::Folder:
        case SideKind::Remote: {
            gfx.Glyph(it->expanded ? 0xE70D : 0xE76C, { base - DR(20), rr.y, DR(16), rr.h }, F::Icon10, TextDim);
            const wchar_t g = it->kind == SideKind::Remote ? 0xE753 : (it->expanded ? 0xE838 : 0xE8B7);
            gfx.Glyph(g, { base - DR(4), rr.y, DR(20), rr.h }, F::Icon12, TextSec);
            const std::wstring cnt = std::to_wstring(it->count);
            const float cw = gfx.Measure(cnt, F::Small);
            gfx.Text(cnt, { right - cw, rr.y, cw + 2, rr.h }, F::Small, TextDim);
            gfx.Text(it->text, { base + DR(20), rr.y, right - cw - DR(8) - (base + DR(20)), rr.h }, F::UI, TextSec);
            break;
        }
        case SideKind::Branch:
        case SideKind::Tag:
        case SideKind::Recent: {
            const Ref& ref = d.refs[(size_t)it->index];
            const bool recentItem = it->kind == SideKind::Recent;
            const float ix = recentItem ? rr.x + DR(19) : base + DR(6);
            const float tx = recentItem ? rr.x + DR(38) : base + DR(20);
            if (it->kind == SideKind::Tag) gfx.Glyph(0xE8EC, { ix - DR(10), rr.y, DR(20), rr.h }, F::Icon12, TextSec);
            else if (ref.head) gfx.Glyph(0xE73E, { ix - DR(10), rr.y, DR(20), rr.h }, F::Icon12, Accent);
            else if (!recentItem) BranchIcon(ix, rr.cy(), DR(13), TextSec);
            // commits to push and to pull, as arrows and counts; "gone" when the upstream was deleted
            auto count = [&](wchar_t arrow, int n) {
                const std::wstring s = std::to_wstring(n);
                const float nw = gfx.Measure(s, F::Small);
                gfx.Text(s, { right - nw, rr.y, nw + 2, rr.h }, F::Small, TextDim);
                gfx.Glyph(arrow, { right - nw - DR(13), rr.y, DR(12), rr.h }, F::Icon10, TextDim);
                right -= nw + DR(13) + DR(6);
            };
            if (ref.behind) count(0xE74B, ref.behind);
            if (ref.ahead) count(0xE74A, ref.ahead);
            if (ref.gone) {
                const float gw = gfx.Measure(L"gone", F::Small);
                gfx.Text(L"gone", { right - gw, rr.y, gw + 2, rr.h }, F::Small, Deleted);
                right -= gw + DR(6);
            }
            if (right < rr.r() - DR(12)) right -= DR(2);
            const Color tc = ref.commit < 0 ? TextDim : (recentItem && !ref.head) ? TextSec : Text;
            gfx.Text(it->text, { tx, rr.y, right - tx, rr.h }, ref.head ? F::UISemi : F::UI, tc);
            break;
        }
        case SideKind::Stash: {
            gfx.Glyph(0xE81E, { base - DR(4), rr.y, DR(20), rr.h }, F::Icon12, TextSec);
            const float sw = gfx.Measure(it->sub, F::MonoSmall);
            gfx.Text(it->sub, { right - sw, rr.y, sw + 2, rr.h }, F::MonoSmall, TextDim);
            gfx.Text(it->text, { base + DR(20), rr.y, right - sw - DR(8) - (base + DR(20)), rr.h }, F::UI, Text);
            break;
        }
        case SideKind::Submodule:
            gfx.Glyph(0xE8F1, { base - DR(4), rr.y, DR(20), rr.h }, F::Icon12, TextSec);
            gfx.Text(it->text, { base + DR(20), rr.y, right - (base + DR(20)), rr.h }, F::UI, Text);
            break;
        default:
            break;
        }
        AddZone(zr, Hit::SideItem, i);
    }
    gfx.PopClip();
    PaintScrollbar(list, t.ss, 1);

    // Settings, pinned to the bottom of the pane.
    const bool hov = Hov(Hit::SettingsButton);
    if (hov) gfx.FillR(settings, DR(4), Hover);
    const float icx = compact ? std::round(settings.cx()) : settings.x + DR(20);
    gfx.Glyph(0xE713, { icx - DR(12), settings.y, DR(24), settings.h }, F::Icon16, Text);
    if (!compact) gfx.Text(L"Settings", { settings.x + DR(40), settings.y, settings.w - DR(48), settings.h }, F::Nav, Text);
    else if (hov) {
        tipText = L"Settings";
        tipAnchor = settings;
    }
    AddZone(settings, Hit::SettingsButton);
}

// ============================================================================ history

App::Cols App::ComputeCols(RepoTab& t, const RectF& a) {
    Cols c{};
    c.rowX = a.x + DR(8);
    c.rowR = a.r() - DR(8);
    c.statR = c.rowR - DR(12);
    const float inner = c.statR - c.rowX;
    c.refX = c.rowX;
    c.refW = DR(colRef);
    const int lanes = t.graph ? t.graph->maxLanes : 1;
    const float autoW = DR(28) + (float)(lanes - 1) * DR(kLaneW);
    // Wide graphs (many lanes) are cut at the right; dragging the column border shows more.
    c.graphW = t.graphW > 0 ? DR(t.graphW) : std::clamp(autoW, DR(44), std::max(DR(44), std::min(std::round(inner * 0.35f), DR(260))));
    // Keep room for the commit text: the branch column gives way first, then the graph.
    float msg = inner - c.refW - c.graphW;
    const float minMsg = DR(240);
    if (msg < minMsg) {
        const float cut = std::min(minMsg - msg, c.refW - DR(72));
        if (cut > 0) { c.refW -= cut; msg += cut; }
    }
    if (msg < minMsg) {
        const float cut = std::min(minMsg - msg, c.graphW - DR(28));
        if (cut > 0) { c.graphW -= cut; msg += cut; }
    }
    c.graphX = c.refX + c.refW;
    c.msgX = c.graphX + c.graphW;
    return c;
}

float App::LaneX(const Cols& c, int lane) const { return std::floor(c.graphX + DR(14) + lane * DR(kLaneW)); }

void App::PaintHistoryHeader(RepoTab& t, const RectF& r) {
    const RepoData& d = *t.data;
    const float pad = DR(24);
    const float titleY = r.y + DR(20);

    // scope: all branches or the current one
    const wchar_t* scope = t.headOnly ? L"Current branch" : L"All branches";
    const float scopeW = DR(12) + DR(14) + DR(8) + gfx.Measure(scope, F::UI) + DR(8) + DR(12) + DR(10);
    const RectF sb{ r.r() - pad - scopeW, titleY + DR(11), scopeW, DR(32) };
    Button(sb, L"", 0, Hov(Hit::ScopeMenu), true);
    gfx.Glyph(0xE71C, { sb.x + DR(8), sb.y, DR(22), sb.h }, F::Icon12, Text);
    gfx.Text(scope, { sb.x + DR(34), sb.y, sb.w - DR(34), sb.h }, F::UI, Text);
    gfx.Glyph(0xE70D, { sb.r() - DR(26), sb.y, DR(16), sb.h }, F::Icon10, TextSec);
    AddZone(sb, Hit::ScopeMenu);
    float left = sb.x - DR(12);

    // search box
    const float qw = std::min(DR(240), left - (r.x + pad) - DR(200));
    if (qw >= DR(150)) {
        const RectF qb{ left - qw, sb.y, qw, DR(32) };
        TextBoxFrame(gfx, qb, searchFocus);
        AddZone(qb, Hit::HistorySearch);
        float right = qb.r() - DR(6);
        if (!t.search.text.empty()) {
            const RectF xb{ right - DR(24), qb.y + DR(4), DR(24), qb.h - DR(8) };
            if (Hov(Hit::SearchClear)) gfx.FillR(xb, DR(4), SubtleHover);
            gfx.Glyph(0xE711, xb, F::Icon10, TextSec);
            AddZone(xb, Hit::SearchClear);
            if (Hov(Hit::SearchClear)) {
                tipText = L"Clear (Esc)";
                tipAnchor = xb;
            }
            right = xb.x - DR(4);
        } else {
            gfx.Glyph(0xE721, { right - DR(24), qb.y, DR(24), qb.h }, F::Icon12, TextSec);
            right -= DR(28);
        }
        // "2 of 17": the selected match among all of them, in history order
        if (t.Searching() && t.graph) {
            const std::wstring count = SearchStatus(t);
            const float cw = gfx.Measure(count, F::Small);
            if (cw < qw * 0.45f) {
                gfx.Text(count, { right - cw, qb.y, cw + 2, qb.h }, F::Small, t.searchCount ? TextDim : Deleted);
                right -= cw + DR(8);
            }
        }
        PaintEdit(t.search, { qb.x + DR(10), qb.y, right - qb.x - DR(10), qb.h }, F::UI, L"Search commits", searchFocus);
        left = qb.x - DR(12);
    }

    // title and summary
    const float tw = left - (r.x + pad);
    gfx.Text(L"History", { r.x + pad, titleY, tw, DR(36) }, F::PageTitle, Text);
    const std::wstring sub = HistorySubtitle(d);
    const float sw = std::min(gfx.Measure(sub, F::Small), tw);
    gfx.Text(sub, { r.x + pad, titleY + DR(38), tw, DR(17) }, F::Small, TextDim);
    if (t.loading) Spinner(r.x + pad + sw + DR(14), titleY + DR(38) + DR(8.5f), DR(5));
}

void App::PaintHistory(RepoTab& t) {
    const RectF a = rcGraph;
    refPopup.chip = {};
    if (!t.graph || !t.data) return;
    const RepoData& d = *t.data;
    const Graph& g = *t.graph;
    PaintHistoryHeader(t, { a.x, a.y, a.w, DR(kPageHeadH) });
    const Cols c = ComputeCols(t, a);

    // ---- column titles; the borders after the first two columns can be dragged
    const RectF hr{ a.x, a.y + DR(kPageHeadH), a.w, DR(kListHeadH) };
    gfx.Text(L"Branch / tag", { c.refX, hr.y, c.refW - DR(10), hr.h }, F::Small, TextDim, DWRITE_TEXT_ALIGNMENT_TRAILING);
    gfx.Text(L"Commit", { c.msgX, hr.y, std::max(0.f, c.statR - DR(90) - c.msgX), hr.h }, F::Small, TextDim);
    gfx.Text(L"Changes", { c.statR - DR(90), hr.y, DR(90), hr.h }, F::Small, TextDim, DWRITE_TEXT_ALIGNMENT_TRAILING);
    for (int k = 0; k < 2; ++k) {
        const float sx = k == 0 ? c.graphX : c.msgX;
        if (Hov(Hit::ColSep, k) || drag == (k == 0 ? Drag::ColRef : Drag::ColGraph)) gfx.VLine(sx, hr.y + DR(3), hr.b() - DR(3), Divider);
        AddZone({ sx - DR(4), hr.y, DR(8), hr.h }, Hit::ColSep, k, k == 1 ? (int)c.graphW : 0);
    }

    // ---- rows
    const RectF body{ a.x, hr.b(), a.w, a.b() - hr.b() };
    rcGraphBody = body;
    const float rowH = DR(kGraphRowH);
    const int32_t nRows = (int32_t)g.rows.size();
    t.gs.view = body.h;
    t.gs.content = (double)nRows * rowH + DR(8);
    t.gs.Clamp();
    AddZone({ body.x, body.y, body.w - DR(12), body.h }, Hit::GraphBody);
    if (nRows == 0) {
        gfx.Text(L"This repository has no commits yet.", body, F::UI, TextDim, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    gfx.PushClip(body);
    const int32_t first = std::max(0, (int32_t)(t.gs.pos / rowH));
    const int32_t last = std::min(nRows, (int32_t)((t.gs.pos + body.h) / rowH) + 1);
    auto rowY = [&](int32_t r) { return (float)std::round((double)body.y + (double)r * rowH - t.gs.pos); };
    const int32_t selRow = t.SelectedRow();
    const bool searching = t.Searching();
    auto dimmed = [&](int32_t commit) { return searching && (commit < 0 || !t.searchHits[(size_t)commit]); };
    // Opaque color under a row (rings around the graph nodes are painted with it).
    auto rowBg = [&](int32_t r) {
        const Color o = r == selRow ? RowSelected : r == t.hoverRow ? RowHover : C(0, 0.f);
        return Mix(NodeBg, A(o, 1.f), o.a);
    };

    // Row of the inline "new branch" name field, if it is open in this tab.
    int32_t inputRow = -1;
    if (inlineInput.active && inlineInput.tab.lock().get() == &t) {
        if (inlineInput.commit < 0 && g.hasWip) inputRow = 0;
        else {
            const int32_t ic = inlineInput.commit >= 0 ? inlineInput.commit : d.headCommit;
            if (ic >= 0 && ic < (int32_t)g.rowOfCommit.size()) inputRow = g.rowOfCommit[(size_t)ic];
        }
    }

    // pass 1: selection (with an accent pill) and hover
    for (int32_t r = first; r < last; ++r) {
        const float y = rowY(r);
        const RectF hl{ c.rowX, y + DR(1), c.rowR - c.rowX, rowH - DR(2) };
        if (r == selRow) {
            gfx.FillR(hl, DR(6), RowSelected);
            gfx.FillR({ c.rowX, y + DR(18), DR(3), rowH - DR(36) }, DR(1.5f), Accent);
        } else if (r == t.hoverRow) {
            gfx.FillR(hl, DR(6), RowHover);
        }
    }

    // pass 2: edges, thin lines with smooth curves between lanes
    const float lw = std::max(1.f, std::round(D(2.f)));
    const float snap = ((int)lw % 2) ? 0.5f : 0.f;
    auto LX = [&](int lane) { return LaneX(c, lane) + snap; };
    gfx.PushClip({ c.graphX, body.y, c.graphW, body.h });
    for (int32_t r = first; r < last; ++r) {
        const GraphRow& row = g.rows[(size_t)r];
        const float y0 = rowY(r), y1 = y0 + rowH, cy = y0 + std::round(rowH * 0.5f);
        const float nx = LX(row.lane);
        const float al = dimmed(row.commit) ? 0.3f : 1.f;
        for (uint32_t k = 0; k < row.segCount; ++k) {
            const Seg& s = g.segs[row.segOff + k];
            const Color col = A(s.dashed ? TextDim : Lane(s.lane), al);  // dashed: uncommitted changes, stashes
            ID2D1StrokeStyle* st = s.dashed ? gfx.Dash() : nullptr;
            const float lx = LX(s.lane);
            if (s.type == SEG_PASS) {
                gfx.Line(lx, y0, lx, y1, col, lw, st);
            } else if (s.lane == row.lane) {
                if (s.type == SEG_IN) gfx.Line(lx, y0, lx, cy, col, lw, st);
                else gfx.Line(nx, cy, nx, y1, col, lw, st);
            } else {
                constexpr float k57 = 0.57f;
                auto path = gfx.Path();
                ComPtr<ID2D1GeometrySink> sk;
                path->Open(&sk);
                if (s.type == SEG_IN) {  // from a lane above into the node
                    const float h = cy - y0;
                    sk->BeginFigure({ lx, y0 }, D2D1_FIGURE_BEGIN_HOLLOW);
                    sk->AddBezier(D2D1::BezierSegment({ lx, y0 + h * k57 }, { nx, cy - h * k57 }, { nx, cy }));
                } else {  // from the node down into another lane
                    const float h = y1 - cy;
                    sk->BeginFigure({ nx, cy }, D2D1_FIGURE_BEGIN_HOLLOW);
                    sk->AddBezier(D2D1::BezierSegment({ nx, cy + h * k57 }, { lx, y1 - h * k57 }, { lx, y1 }));
                }
                sk->EndFigure(D2D1_FIGURE_END_OPEN);
                sk->Close();
                gfx.dc()->DrawGeometry(path.Get(), gfx.B(col), lw, st);
            }
        }
    }

    // pass 3: nodes: solid dots, hollow merges, a dashed ring for the uncommitted changes, a square for stashes
    for (int32_t r = first; r < last; ++r) {
        const GraphRow& row = g.rows[(size_t)r];
        const float cy = rowY(r) + std::round(rowH * 0.5f);
        const float nx = LX(row.lane);
        const float al = dimmed(row.commit) ? 0.3f : 1.f;
        const Color bg = rowBg(r);
        const Color lc = A(Lane(row.lane), al);
        if (row.commit < 0) {
            gfx.Circle(nx, cy, D(7.f), bg);
            gfx.CircleS(nx, cy, D(6.f), A(TextDim, al), std::max(1.f, D(1.6f)), gfx.Dash());
            continue;
        }
        const Commit& cm = d.commits[(size_t)row.commit];
        if (cm.flags & CF_STASH) {
            const RectF sq{ nx - D(6.f), cy - D(6.f), D(12.f), D(12.f) };
            gfx.FillR(sq, D(3.f), bg);
            gfx.StrokeR(sq, D(3.f), A(TextSec, al), std::max(1.f, D(1.6f)));
        } else if (cm.flags & CF_MERGE) {
            gfx.Circle(nx, cy, D(6.f), lc);
            gfx.Circle(nx, cy, D(4.f), bg);
        } else {
            gfx.Circle(nx, cy, D(6.f), bg);
            gfx.Circle(nx, cy, D(4.f), lc);
        }
    }
    gfx.PopClip();

    // pass 4: the first branch or tag of each commit as a chip: local / remote / tag icons, "+N" for the others
    gfx.PushClip({ c.refX, body.y, c.refW, body.h });
    for (int32_t r = first; r < last; ++r) {
        const float y = rowY(r);
        if (r == inputRow) {
            PaintInlineInput(t, { c.refX + DR(4), y + std::round((rowH - DR(32)) * 0.5f), c.refW - DR(14), DR(32) });
            continue;
        }
        const GraphRow& row = g.rows[(size_t)r];
        if (row.commit < 0) continue;
        auto it = d.labels.find(row.commit);
        if (it == d.labels.end() || it->second.empty()) continue;
        const Label& L = it->second[0];
        const int more = (int)it->second.size() - 1;
        const std::wstring text = LabelText(d, L);
        if (L.widthScale != g_S) {
            L.width = gfx.Measure(text, F::Small);
            L.widthScale = g_S;
        }
        const std::wstring moreText = more > 0 ? L"+" + std::to_wstring(more) : std::wstring();
        const float moreW = more > 0 ? gfx.Measure(moreText, F::SmallSemi) : 0.f;
        const int icons = (L.local ? 1 : 0) + (L.remote ? 1 : 0) + (L.tag ? 1 : 0);
        const float padX = DR(8), gap = DR(5), iconW = DR(13);
        const float w = std::min(padX + icons * (iconW + gap) + L.width + (more > 0 ? gap + moreW : 0.f) + padX, c.refW - DR(10));
        if (w < DR(36)) continue;
        const RectF chip{ c.refX + c.refW - DR(10) - w, y + std::round((rowH - DR(22)) * 0.5f), w, DR(22) };
        const float al = dimmed(row.commit) ? 0.35f : 1.f;
        const bool hot = Hov(Hit::GraphLabel, r) || (refPopup.row == r && (hover.id == Hit::RefPopup || hover.id == Hit::RefPopupRow));
        const Color lc = L.tag ? TextSec : Lane(row.lane);
        const float fillA = L.head ? (hot ? 0.26f : 0.16f) : (hot ? 0.10f : 0.f);
        gfx.FillR(chip, chip.h * 0.5f, A(lc, fillA * al));
        gfx.StrokeR(chip, chip.h * 0.5f, A(lc, (L.tag ? 0.4f : 0.55f) * al));
        const Color tc = A(lc, al);
        float x = chip.x + padX;
        auto icon = [&](wchar_t gl) {
            gfx.Glyph(gl, { x - DR(2), chip.y, iconW + DR(4), chip.h }, F::Icon12, tc);
            x += iconW + gap;
        };
        if (L.local) icon(0xE7F8);
        if (L.remote) icon(0xE753);
        if (L.tag) icon(0xE8EC);
        const float textR = chip.r() - padX - (more > 0 ? moreW + gap : 0.f);
        const float room = std::max(0.f, textR - x);
        gfx.Text(text, { x, chip.y, L.width <= room + 0.5f ? L.width + DR(3) : room, chip.h }, F::Small, tc);
        if (more > 0) gfx.Text(moreText, { textR + gap, chip.y, moreW + 2, chip.h }, F::SmallSemi, tc);
        AddZone(Intersect(chip, body), Hit::GraphLabel, r, row.commit);
        if (refPopup.row == r && refPopup.commit == row.commit) refPopup.chip = chip;
    }
    gfx.PopClip();

    // pass 5: message, author, time and id; lines added and removed
    const float monoW = MonoAdvance(gfx);
    std::vector<Oid> wantStats;
    auto wantStat = [&](const Commit& cm) -> const LineStat* {
        if (cm.flags & (CF_MERGE | CF_STASH)) return nullptr;
        auto st = t.lineStats.find(cm.id);
        if (st != t.lineStats.end()) return &st->second;
        if (wantStats.size() < 64) wantStats.push_back(cm.id);
        return nullptr;
    };
    gfx.PushClip({ c.msgX, body.y, c.rowR - c.msgX, body.h });
    for (int32_t r = first; r < last; ++r) {
        const GraphRow& row = g.rows[(size_t)r];
        const float y = rowY(r);
        const float al = dimmed(row.commit) ? 0.35f : 1.f;
        const float titleY = y + DR(9), metaY = y + DR(31);
        if (row.commit < 0) {  // uncommitted changes
            const wchar_t* label = L"Review and commit";
            const float bw = gfx.Measure(label, F::UISemi) + DR(28);
            const RectF b{ c.statR - bw, y + std::round((rowH - DR(32)) * 0.5f), bw, DR(32) };
            Button(b, label, 1, Hov(Hit::ReviewCommit), true, F::UISemi);
            AddZone(Intersect(b, body), Hit::ReviewCommit);
            const float tw = std::max(0.f, b.x - DR(16) - c.msgX);
            gfx.Text(L"Uncommitted changes", { c.msgX, titleY, tw, DR(20) }, F::RowTitle, A(Text, al));
            const std::wstring meta = WipSummary(d, t.status.get());
            gfx.Text(meta, { c.msgX, metaY, tw, DR(17) }, F::Small, A(TextDim, al));
            continue;
        }
        const Commit& cm = d.commits[(size_t)row.commit];
        float statW = 0;
        if (const LineStat* st = wantStat(cm); st && st->added >= 0) {
            const std::wstring plus = L"+" + std::to_wstring(st->added), minus = L"−" + std::to_wstring(st->deleted);
            const float mw = (float)minus.size() * monoW;
            statW = (float)(plus.size() + 1 + minus.size()) * monoW;
            gfx.Text(minus, { c.statR - mw - DR(20), y, mw + DR(20), rowH }, F::Mono, A(Deleted, al), DWRITE_TEXT_ALIGNMENT_TRAILING);
            gfx.Text(plus, { c.statR - statW - DR(20), y, statW - mw - monoW + DR(20), rowH }, F::Mono, A(Added, al),
                     DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        const float tw = std::max(0.f, c.statR - (statW > 0 ? statW + DR(16) : 0.f) - c.msgX);
        gfx.Text(CommitSubject(d, cm), { c.msgX, titleY, tw, DR(20) }, F::RowTitle, A(Text, al));
        const std::wstring meta = CommitMeta(d, cm);
        const float mw = gfx.Measure(meta, F::Small);
        gfx.Text(meta, { c.msgX, metaY, tw, DR(17) }, F::Small, A(TextDim, al));
        if (mw + DR(24) < tw) gfx.Text(cm.id.Hex(7), { c.msgX + mw, metaY, tw - mw, DR(17) }, F::MonoSmall, A(TextDim, al));
    }
    gfx.PopClip();
    // Counts for the rows just below the view too, so they are there when scrolling.
    for (int32_t r = last; r < std::min(nRows, last + 24) && wantStats.size() < 64; ++r)
        if (g.rows[(size_t)r].commit >= 0) wantStat(d.commits[(size_t)g.rows[(size_t)r].commit]);

    // validation message under the "new branch" field
    if (inputRow >= first && inputRow < last && !inlineInput.error.empty()) {
        const RectF& box = inlineInput.rect;
        const float tw = std::min(gfx.Measure(inlineInput.error, F::Small) + DR(20), body.r() - box.x - DR(20));
        const RectF bub{ box.x, box.b() + DR(5), std::max(box.w, tw), DR(26) };
        gfx.FillR({ bub.x + DR(1), bub.y + DR(2), bub.w, bub.h }, DR(5), Shadow);
        gfx.FillR(bub, DR(5), ErrorBg);
        gfx.StrokeR(bub, DR(5), A(Deleted, 0.9f));
        gfx.Text(inlineInput.error, bub.Inset(DR(10), 0), F::Small, ErrorText);
    }
    gfx.PopClip();
    PaintScrollbar(body, t.gs, 0);
    if (!wantStats.empty()) t.RequestLineStats(std::move(wantStats));
}

// The branches and tags of a commit, listed under its chip while the chip is hovered.
void App::PaintRefPopup(RepoTab& t) {
    if (refPopup.row < 0 || refPopup.chip.Empty() || !t.data || NowMs() - refPopup.since < 300) return;
    const RepoData& d = *t.data;
    auto it = d.labels.find(refPopup.commit);
    if (it == d.labels.end() || it->second.empty() || !t.graph || refPopup.row >= (int32_t)t.graph->rows.size()) return;
    const std::vector<Label>& labels = it->second;
    const int lane = t.graph->rows[(size_t)refPopup.row].lane;
    const int shown = std::min((int)labels.size(), 12);
    const float pad = DR(6), rowH = DR(34), titleH = DR(26);
    const float W = (float)gfx.Width(), H = (float)gfx.Height();
    const float w = std::min(DR(330), W - DR(16));
    const float h = pad + titleH + shown * rowH + (shown < (int)labels.size() ? DR(26) : 0.f) + pad;
    const RectF& chip = refPopup.chip;
    RectF box{ std::min(chip.x, W - DR(8) - w), chip.b() + DR(6), w, h };
    if (box.b() > H - DR(8)) box.y = chip.y - DR(6) - h;  // no room below: above the chip
    // a soft shadow, the card, and a bridge to the chip so the pointer can move across the gap
    for (int k = 3; k >= 1; --k) {
        const float e = DR(2.f * k);
        gfx.FillR({ box.x - e, box.y - e + DR(4), box.w + 2 * e, box.h + 2 * e }, DR(8) + e, A(Shadow, Shadow.a * 0.22f));
    }
    gfx.FillR(box, DR(8), PopupBg);
    gfx.StrokeR(box, DR(8), PopupStroke);
    const RectF bridge = box.y > chip.y ? RectF{ chip.x, chip.b(), std::max(chip.w, DR(40)), box.y - chip.b() }
                                        : RectF{ chip.x, box.b(), std::max(chip.w, DR(40)), chip.y - box.b() };
    AddZone(bridge, Hit::RefPopup);
    AddZone(box, Hit::RefPopup);

    int nBranches = 0, nTags = 0;
    for (const Label& L : labels) (L.tag ? nTags : nBranches)++;
    const std::wstring sha = d.commits[(size_t)refPopup.commit].id.Hex(7);
    const std::wstring title = nTags == 0   ? Plural((size_t)nBranches, L"branch", L"branches") + L" on " + sha
                               : nBranches == 0 ? Plural((size_t)nTags, L"tag", L"tags") + L" on " + sha
                                                : std::to_wstring(labels.size()) + L" branches and tags on " + sha;
    gfx.Text(title, { box.x + pad + DR(8), box.y + pad, box.w - 2 * pad - DR(16), titleH }, F::Small, TextDim);
    float y = box.y + pad + titleH;
    for (int k = 0; k < shown; ++k) {
        const Label& L = labels[(size_t)k];
        const RectF rr{ box.x + pad, y, box.w - 2 * pad, rowH };
        if (Hov(Hit::RefPopupRow, k)) gfx.FillR(rr, DR(4), Hover);
        else if (L.head) gfx.FillR(rr, DR(4), Faint);
        const Color ic = L.tag ? TextSec : Lane(lane);
        float ix = rr.x + DR(8);
        auto icon = [&](wchar_t gl) {
            gfx.Glyph(gl, { ix - DR(2), rr.y, DR(18), rr.h }, F::Icon12, ic);
            ix += DR(18);
        };
        if (L.local) icon(0xE7F8);
        if (L.remote) icon(0xE753);
        if (L.tag) icon(0xE8EC);
        const std::wstring note = RefNote(d, L);
        const float nw = std::min(gfx.Measure(note, F::Small), rr.w * 0.62f);
        const float nx = rr.x + DR(8) + DR(36) + DR(10);
        gfx.Text(note, { rr.r() - DR(8) - nw, rr.y, nw + 2, rr.h }, F::Small, TextDim);
        gfx.Text(L.text, { nx, rr.y, rr.r() - DR(16) - nw - nx, rr.h }, L.head ? F::UISemi : F::UI, Text);
        AddZone(rr, Hit::RefPopupRow, k, refPopup.commit);
        y += rowH;
    }
    if (shown < (int)labels.size())
        gfx.Text(L"and " + std::to_wstring(labels.size() - (size_t)shown) + L" more", { box.x + pad + DR(8), y, box.w - 2 * pad, DR(24) },
                 F::Small, TextDim);
}

void App::PaintInlineInput(RepoTab&, const RectF& box) {
    inlineInput.rect = box;
    TextBoxFrame(gfx, box, true, !inlineInput.error.empty());
    PaintEdit(inlineInput.edit, box.Inset(DR(10), 0), F::UI, L"New branch name", true);
    AddZone(box, Hit::InlineEdit);
}

// Draws a text field's content: placeholder, selection, text and blinking caret. The layout and its
// origin are stored in the TextEdit for mouse hit testing; the view scrolls to keep the caret visible.
// A masked field shows a dot for each character (one per UTF-16 unit, so positions match the text).
void DrawEdit(Gfx& gfx, TextEdit& e, const RectF& area, F font, std::wstring_view placeholder, bool focused, bool masked) {
    gfx.PushClip(area);
    const bool multi = e.multiline;
    const std::wstring dots = masked ? std::wstring(e.text.size(), L'\u25CF') : std::wstring();
    const std::wstring& shown = masked ? dots : e.text;
    e.layout = multi ? gfx.Layout(shown, F::Body, area.w, 100000.f) : gfx.Layout(shown, font, 100000.f, area.h);
    if (!e.layout) { gfx.PopClip(); return; }
    FLOAT cx = 0, cy = 0;
    DWRITE_HIT_TEST_METRICS cm{};
    e.layout->HitTestTextPosition((UINT32)std::min(e.caret, e.text.size()), FALSE, &cx, &cy, &cm);
    const float lineH = cm.height > 0 ? cm.height : DR(17);
    DWRITE_TEXT_METRICS tm{};
    e.layout->GetMetrics(&tm);
    if (multi) {
        if (focused) {
            if (cy + lineH - e.scroll > area.h) e.scroll = cy + lineH - area.h;
            if (cy - e.scroll < 0) e.scroll = cy;
        }
        e.scroll = std::clamp(e.scroll, 0.f, std::max(0.f, tm.height - area.h));
        e.origin = { area.x, area.y - e.scroll };
    } else {
        if (focused) {
            if (cx - e.scroll > area.w - DR(3)) e.scroll = cx - area.w + DR(3);
            if (cx - e.scroll < 0) e.scroll = cx;
        }
        e.scroll = std::clamp(e.scroll, 0.f, std::max(0.f, tm.widthIncludingTrailingWhitespace - area.w + DR(3)));
        e.origin = { area.x - e.scroll, area.y };
    }
    if (e.text.empty() && !placeholder.empty()) {
        if (multi) {
            auto ph = gfx.Layout(placeholder, F::Body, area.w);
            if (ph) gfx.dc()->DrawTextLayout({ area.x, area.y }, ph.Get(), gfx.B(TextDim));
        } else {
            gfx.Text(placeholder, area, font, TextDim);
        }
    }
    if (e.HasSel()) {
        UINT32 count = 0;
        const UINT32 start = (UINT32)e.SelStart(), len = (UINT32)(e.SelEnd() - e.SelStart());
        e.layout->HitTestTextRange(start, len, e.origin.x, e.origin.y, nullptr, 0, &count);
        std::vector<DWRITE_HIT_TEST_METRICS> ms(count);
        if (count && SUCCEEDED(e.layout->HitTestTextRange(start, len, e.origin.x, e.origin.y, ms.data(), count, &count)))
            for (const auto& m : ms) gfx.Fill({ m.left, m.top, std::max(m.width, DR(4)), m.height }, focused ? A(Accent, 0.32f) : SubtleHover);
    }
    gfx.dc()->DrawTextLayout(e.origin, e.layout.Get(), gfx.B(Text), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    if (focused && std::fmod(NowMs(), 1060.0) < 530.0)
        gfx.Fill({ std::floor(e.origin.x + cx), e.origin.y + cy + DR(1), std::max(1.f, D(1.1f)), lineH - DR(2) }, Caret);
    gfx.PopClip();
}

// ============================================================================ commit box (uncommitted changes)

float App::CommitBoxHeight() const { return DR(252); }

void App::PaintCommitBox(RepoTab& t, const RectF& r) {
    gfx.HLine(r.x, r.r(), r.y, DividerSoft);
    const RepoData& d = *t.data;
    const float x = r.x + DR(20), w = r.w - DR(40);
    float y = r.y + DR(16);
    const size_t staged = t.status ? t.status->staged.size() : 0;
    const bool canAmend = !d.unborn;
    const bool amend = t.amend && canAmend;

    // "Amend previous commit" check box
    {
        const RectF row{ x - DR(6), y, std::min(w + DR(12), DR(240)), DR(30) };
        if (canAmend && Hov(Hit::CommitAmend)) gfx.FillR(row, DR(4), Hover);
        const RectF box{ x, y + DR(5), DR(20), DR(20) };
        if (amend) {
            gfx.FillR(box, DR(4), Accent);
            gfx.Glyph(0xE73E, box, F::Icon12, TextOnAccent);
        } else {
            gfx.FillR(box, DR(4), InputBg);
            gfx.StrokeR(box, DR(4), canAmend ? InputLine : ControlStroke);
        }
        gfx.Text(L"Amend previous commit", { x + DR(30), y, w - DR(30), DR(30) }, F::UI, canAmend ? Text : TextDim);
        if (canAmend) AddZone(row, Hit::CommitAmend);
        y += DR(30) + DR(10);
    }
    // summary
    {
        const RectF fb{ x, y, w, DR(32) };
        TextBoxFrame(gfx, fb, editFocus == 1);
        const int left = 72 - (int)t.commitSummary.text.size();  // the usual limit for a summary line
        const std::wstring ls = std::to_wstring(left);
        const float lw = gfx.Measure(ls, F::Small);
        PaintEdit(t.commitSummary, { fb.x + DR(10), fb.y, fb.w - DR(20) - lw - DR(8), fb.h }, F::UI, L"Summary (required)", editFocus == 1);
        gfx.Text(ls, { fb.r() - DR(10) - lw, fb.y, lw + 2, fb.h }, F::Small, left < 0 ? Deleted : left < 8 ? Modified : TextDim);
        AddZone(fb, Hit::CommitField, 1);
        y += DR(32) + DR(8);
    }
    // description
    {
        const RectF db{ x, y, w, DR(96) };
        TextBoxFrame(gfx, db, editFocus == 2);
        PaintEdit(t.commitBody, { db.x + DR(10), db.y + DR(7), db.w - DR(20), db.h - DR(14) }, F::Body, L"Description", editFocus == 2);
        AddZone(db, Hit::CommitField, 2);
        y += DR(96) + DR(12);
    }
    // commit button
    bool hasSummary = false;
    for (wchar_t c : t.commitSummary.text) hasSummary |= !iswspace(c);
    const bool committing = t.opBusy && t.opButton == kCommitOp;
    const bool enabled = !t.opBusy && hasSummary && (amend || staged > 0);
    const std::wstring label = CommitButtonLabel(staged, amend);
    const RectF btn{ x, y, w, DR(32) };
    Button(btn, committing ? L"" : label, 1, Hov(Hit::CommitButton), enabled || committing, F::UISemi);
    if (committing) {
        const Color fg = TextOnAccent;
        const std::wstring busy = amend ? L"Amending…" : L"Committing…";
        const float bw = gfx.Measure(busy, F::UISemi);
        const float gx = std::round(btn.cx() - (bw + DR(24)) * 0.5f);
        Spinner(gx + DR(8), btn.cy(), DR(6), &fg);
        gfx.Text(busy, { gx + DR(24), btn.y, bw + 2, btn.h }, F::UISemi, fg);
    }
    AddZone(btn, Hit::CommitButton);
}

// ============================================================================ overlays

void App::PaintToasts() {
    const double now = NowMs();
    // Drop expired notifications here: while frames keep coming, WM_TIMER (lowest priority) may not run,
    // and an invisible card would still swallow clicks.
    toasts.erase(std::remove_if(toasts.begin(), toasts.end(), [&](const Toast& tt) { return tt.ttl > 0 && now >= tt.born + tt.ttl; }),
                 toasts.end());
    if (toasts.empty()) return;
    // Bottom right of the history: never over the details pane and its commit box.
    RepoTab* t = Active();
    const bool repo = t && t->data && t->error.empty() && !t->IsStart();
    const float right = repo ? rcGraph.r() : (float)gfx.Width();
    const float w = std::min(DR(400), right - DR(40));
    const float x = right - w - DR(20);
    float y = (float)gfx.Height() - DR(16);
    for (int i = (int)toasts.size() - 1; i >= 0; --i) {  // newest at the bottom
        const Toast& tt = toasts[(size_t)i];
        float alpha = 1.f;
        const double age = now - tt.born;
        if (age < 180) { alpha = (float)(age / 180.0); animating = true; }
        if (tt.ttl > 0) {
            const double left = tt.born + tt.ttl - now;
            if (left < 350) { alpha = std::min(alpha, (float)std::max(0.0, left / 350.0)); animating = true; }
        }
        ComPtr<IDWriteTextLayout> lm;
        DWRITE_TEXT_METRICS mm{};
        if (!tt.message.empty()) {
            lm = gfx.Layout(tt.message, F::Body, w - DR(66), DR(140));
            if (lm) lm->GetMetrics(&mm);
        }
        const float mh = std::min(mm.height, DR(120));
        const float h = DR(16) + DR(20) + (lm ? DR(4) + mh : 0.f) + DR(14);
        y -= h;
        const float slide = (1.f - alpha) * DR(12);
        const RectF card{ x + slide, y, w, h };
        for (int k = 3; k >= 1; --k) {
            const float e = DR(2.f * k);
            gfx.FillR({ card.x - e, card.y - e + DR(4), card.w + 2 * e, card.h + 2 * e }, DR(8) + e, A(Shadow, Shadow.a * 0.2f * alpha));
        }
        gfx.FillR(card, DR(8), A(ToastBg, ToastBg.a * alpha));
        gfx.StrokeR(card, DR(8), A(PopupStroke, PopupStroke.a * alpha));
        const Color accent = A(tt.error ? Deleted : AccentText, alpha);
        gfx.Glyph(tt.error ? 0xE7BA : 0xE73E, { card.x + DR(14), card.y + DR(12), DR(24), DR(24) }, F::Icon14, accent);
        gfx.Text(tt.title, { card.x + DR(44), card.y + DR(14), w - DR(84), DR(20) }, F::UISemi, A(Text, alpha));
        if (lm) {
            gfx.PushClip({ card.x, card.y, card.w, card.h - DR(8) });
            gfx.dc()->DrawTextLayout({ card.x + DR(44), card.y + DR(38) }, lm.Get(), gfx.B(A(TextSec, alpha)),
                                     D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            gfx.PopClip();
        }
        const RectF xr{ card.r() - DR(34), card.y + DR(10), DR(26), DR(26) };
        if (Hov(Hit::ToastClose, i)) gfx.FillR(xr, DR(4), A(SubtleHover, SubtleHover.a * alpha));
        gfx.Glyph(0xE711, xr, F::Icon10, A(TextSec, alpha));
        AddZone(card, Hit::None);  // the card itself swallows clicks
        AddZone(xr, Hit::ToastClose, i);
        y -= DR(10);
    }
}

void App::PaintTooltip() {
    if (tipText.empty() || dialog.active || !IsTipZone(hover.id)) return;
    if (NowMs() - hoverSince < 480) return;
    const float tw = gfx.Measure(tipText, F::Small) + DR(20);
    const float W = (float)gfx.Width(), H = (float)gfx.Height();
    float x = std::round(tipAnchor.cx() - tw * 0.5f);
    x = std::clamp(x, DR(8), W - tw - DR(8));
    RectF r{ x, tipAnchor.b() + DR(6), tw, DR(28) };
    if (r.b() > H - DR(4)) r.y = tipAnchor.y - DR(6) - r.h;
    gfx.FillR({ r.x, r.y + DR(2), r.w, r.h }, DR(5), Shadow);
    gfx.FillR(r, DR(5), PopupBg);
    gfx.StrokeR(r, DR(5), PopupStroke);
    gfx.Text(tipText, r, F::Small, Text, DWRITE_TEXT_ALIGNMENT_CENTER);
}

// A modal dialog over a whole window: a scrim, then a card with the title, the message and a row of buttons
// (the default one, a destructive one). `buttons` receives where each button is.
void DrawDialog(Gfx& gfx, const DialogState& dialog, int hot, std::vector<RectF>& buttons) {
    buttons.clear();
    const float W = (float)gfx.Width(), H = (float)gfx.Height();
    gfx.Fill({ 0, 0, W, H }, Scrim);
    const float w = std::min(DR(480), W - DR(40));
    auto lm = gfx.Layout(dialog.message, F::Body, w - DR(48));
    DWRITE_TEXT_METRICS mm{};
    if (lm) lm->GetMetrics(&mm);
    auto lt = gfx.Layout(dialog.title, F::BodyTitle, w - DR(48));
    DWRITE_TEXT_METRICS mt{};
    if (lt) lt->GetMetrics(&mt);
    const float footerH = DR(80);
    const float h = DR(24) + mt.height + DR(12) + mm.height + DR(24) + footerH;
    const RectF card{ std::round((W - w) * 0.5f), std::round((H - h) * 0.45f), w, h };
    for (int k = 4; k >= 1; --k) {
        const float e = DR(3.f * k);
        gfx.FillR({ card.x - e, card.y - e + DR(8), card.w + 2 * e, card.h + 2 * e }, DR(8) + e, A(Shadow, Shadow.a * 0.25f));
    }
    gfx.FillR(card, DR(8), DialogBg);
    // the button area, a shade darker, as in the Windows dialogs
    const Color footer = IsHighContrast() ? DialogBg : A(Mix(DialogBg, C(0x000000), IsDark() ? 0.22f : 0.045f), 1.f);
    const RectF fr{ card.x, card.b() - footerH, card.w, footerH };
    gfx.FillR(fr, DR(8), footer);
    gfx.Fill({ fr.x, fr.y, fr.w, DR(8) }, footer);
    gfx.HLine(fr.x, fr.r(), fr.y, DividerSoft);
    gfx.StrokeR(card, DR(8), PopupStroke);
    if (lt) gfx.dc()->DrawTextLayout({ card.x + DR(24), card.y + DR(24) }, lt.Get(), gfx.B(Text), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    if (lm)
        gfx.dc()->DrawTextLayout({ card.x + DR(24), card.y + DR(24) + mt.height + DR(12) }, lm.Get(), gfx.B(TextSec),
                                 D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    // Buttons share the width of the footer, the first on the left.
    const int n = (int)dialog.buttons.size();
    const float gap = DR(8);
    const float bw = n ? std::floor((w - DR(48) - gap * (n - 1)) / n) : 0.f;
    // With a destructive choice the safe default (Cancel) stays neutral, marked only by a focus ring.
    const bool safeDefault = dialog.danger >= 0 && dialog.defButton != dialog.danger;
    for (int i = 0; i < n; ++i) {
        const std::wstring& label = dialog.buttons[(size_t)i];
        const RectF b{ card.x + DR(24) + i * (bw + gap), fr.y + DR(24), bw, DR(32) };
        const bool hov = i == hot;
        if (i == dialog.danger) {
            gfx.FillR(b, DR(4), hov ? DangerHi : Danger);
            gfx.Text(label, b, F::UISemi, OnColor(Danger), DWRITE_TEXT_ALIGNMENT_CENTER);
        } else if (i == dialog.defButton && !safeDefault) {
            DrawButton(gfx, b, label, 1, hov, true, F::UISemi);
        } else {
            DrawButton(gfx, b, label, 0, hov, true, F::UI);
        }
        if (i == dialog.defButton && (safeDefault || i == dialog.danger)) gfx.StrokeR(b.Inset(-DR(2), -DR(2)), DR(6), A(Text, 0.7f), D(1.5f));
        buttons.push_back(b);
    }
}

void App::PaintDialog() {
    if (!dialog.active) return;
    AddZone({ 0, 0, (float)gfx.Width(), (float)gfx.Height() }, Hit::DialogBlock);
    std::vector<RectF> buttons;
    DrawDialog(gfx, dialog, HovAny(Hit::DialogButton) ? hover.a : -1, buttons);
    for (size_t i = 0; i < buttons.size(); ++i) AddZone(buttons[i], Hit::DialogButton, (int)i);
}

// Diagnostics for automated UI tests (DREAMTREE_DEBUG).
void App::PaintDebug() {
    if (!TestMode()) return;
    RepoTab* t = Active();
    wchar_t buf[256];
    swprintf_s(buf, L"[hover %d/%d/%d  focus %d  search %d  drag %d  zones %zu  toasts %zu]", (int)hover.id, hover.a, hover.b, editFocus,
               searchFocus ? 1 : 0, (int)drag, zones.size(), toasts.size());
    std::wstring s = buf;
    if (t && t->data) {
        swprintf_s(buf, L"  [git %.0f ms  graph %.1f ms  ready %.0f ms  frame %.1f ms  stats %zu  hits %d  zoom %d%%]", t->data->msGit,
                   t->graph ? t->graph->ms : 0.0, t->loadMs, frameMs, t->lineStats.size(), t->searchCount, (int)std::lround(zoom * 100));
        s += buf;
    }
    if (t && t->diff.diff) {
        swprintf_s(buf, L"  [diff %zu lines %.0f ms  layouts %zu  sel %d]", t->diff.diff->lines.size(), t->diff.diff->ms,
                   t->diffUi.layouts.size(), t->diff.selCount);
        s += buf;
    }
    const float W = (float)gfx.Width(), H = (float)gfx.Height();
    const float w = std::min(W - DR(16), gfx.Measure(s, F::Small) + DR(16));
    const RectF r{ DR(8), H - DR(8) - DR(36) - DR(8) - DR(24), w, DR(24) };
    gfx.FillR(r, DR(4), A(PopupBg, 0.94f));
    gfx.Text(s, r.Inset(DR(8), 0), F::Small, TextSec);
}

// ============================================================================ details pane

void App::PaintDetails(RepoTab& t) {
    const RectF r = rcDetail;
    gfx.VLine(r.x, r.y, r.b(), CardStroke);
    const RectF inner{ r.x + 1, r.y, r.w - 1, r.h };
    gfx.PushClip(inner);
    const bool wip = t.selWip && t.graph && t.graph->hasWip;
    if (wip) PaintWipDetails(t, inner);  // has its own scrollbar above the commit box
    else if (t.data && t.selCommit >= 0 && t.selCommit < (int32_t)t.data->commits.size()) PaintCommitDetails(t, inner);
    else {
        t.ds.content = 0;
        gfx.Text(L"Select a commit to see its details", inner, F::UI, TextDim, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    gfx.PopClip();
    if (!wip) PaintScrollbar(inner, t.ds, 2);
    AddZone({ r.x - DR(4), r.y, DR(8), r.h }, Hit::SplitDetail);
}

float App::PaintFileRows(RepoTab& t, const RectF& clip, float x, float w, float y, int section, const std::vector<FileChange>& files) {
    const int mode = t.treeMode ? 1 : 0;
    if (t.fileRowsSrc[section] != &files || t.fileRowsMode[section] != mode || t.fileRowsVer[section] != t.collapseVer) {
        BuildFileRows(t, section, files, t.fileRows[section]);
        t.fileRowsSrc[section] = &files;
        t.fileRowsMode[section] = mode;
        t.fileRowsVer[section] = t.collapseVer;
    }
    const auto& rows = t.fileRows[section];
    const float pitch = DR(kFilePitch), rowH = DR(kFileRowH);
    const int n = (int)rows.size();
    const int first = std::max(0, (int)((clip.y - y) / pitch));
    const int last = std::min(n, (int)((clip.b() - y) / pitch) + 1);
    const bool wip = section == 1 || section == 2;
    const DiffState& dv = t.diff;
    for (int i = first; i < last; ++i) {
        const FileRow& fr = rows[(size_t)i];
        const RectF rr{ x, std::round(y + i * pitch), w, rowH };
        const Hit id = fr.folder ? Hit::DetailFolder : Hit::DetailFile;
        const bool onButton = wip && hover.id == Hit::WipAction && hover.b == i &&
                              (section == 1 ? (hover.a == kStageRow || hover.a == kDiscardRow) : hover.a == kUnstageRow);
        const bool hov = (hover.id == id && hover.a == section && hover.b == i) || onButton;
        const bool open = !fr.folder && dv.open && dv.req.section == section && fr.full == dv.req.path;  // shown in the diff view
        if (open) {
            gfx.FillR(rr, DR(4), Subtle);
            gfx.FillR({ rr.x, rr.y + DR(9), DR(3), rr.h - DR(18) }, DR(1.5f), Accent);
        } else if (hov) {
            gfx.FillR(rr, DR(4), Hover);
        }
        AddZone(Intersect(rr, clip), id, section, i);  // registered first: the row's buttons lie on top of it
        float textRight = rr.r() - DR(8);
        if (wip) {
            // Stage / Unstage (and discard for unstaged files), shown while the row is hovered.
            const int act = section == 1 ? kStageRow : kUnstageRow;
            const wchar_t* label = section == 1 ? L"Stage" : L"Unstage";
            const float bw = gfx.Measure(label, F::Small) + DR(20);
            const RectF b{ rr.r() - DR(6) - bw, rr.y + DR(5), bw, rr.h - DR(10) };
            if (hov) {
                Button(b, label, 0, hover.id == Hit::WipAction && hover.a == act && hover.b == i, true, F::Small);
                textRight = b.x - DR(6);
            }
            AddZone(Intersect(b, clip), Hit::WipAction, act, i);
            if (section == 1 && !fr.folder && files[(size_t)fr.file].status != L'U') {
                const RectF dz{ b.x - DR(30), rr.y + DR(5), DR(26), rr.h - DR(10) };
                if (hov) {
                    const bool hd = hover.id == Hit::WipAction && hover.a == kDiscardRow && hover.b == i;
                    if (hd) gfx.FillR(dz, DR(4), SubtleHover);
                    gfx.Glyph(0xE74D, dz, F::Icon12, hd ? Deleted : TextSec);
                    textRight = dz.x - DR(4);
                }
                AddZone(Intersect(dz, clip), Hit::WipAction, kDiscardRow, i);
            }
        }
        const float ix = rr.x + DR(8) + fr.depth * DR(16);
        if (fr.folder) {
            gfx.Glyph(fr.expanded ? 0xE70D : 0xE76C, { ix - DR(2), rr.y, DR(14), rr.h }, F::Icon10, TextDim);
            gfx.Glyph(fr.expanded ? 0xE838 : 0xE8B7, { ix + DR(14), rr.y, DR(20), rr.h }, F::Icon12, TextSec);
            gfx.Text(fr.name, { ix + DR(38), rr.y, textRight - ix - DR(38), rr.h }, F::UI, TextSec);
            continue;
        }
        const FileChange& f = files[(size_t)fr.file];
        FileIcon(f.status, ix + DR(8), rr.cy());
        // lines added and removed, unless the row's buttons are showing
        if (!(wip && hov) && (f.added > 0 || f.deleted > 0)) {
            const std::wstring delta = LineDelta(f.added, f.deleted);
            const float dw = gfx.Measure(delta, F::MonoSmall);
            gfx.Text(delta, { textRight - dw, rr.y, dw + 2, rr.h }, F::MonoSmall, TextDim);
            textRight -= dw + DR(10);
        }
        const float tx = ix + DR(26);
        const float avail = textRight - tx;
        const float nw = gfx.Measure(fr.name, F::UI);
        if (!fr.dir.empty() && avail > nw + DR(30)) {
            const float full = gfx.Measure(fr.dir, F::UI);
            const float dw = std::min(full, avail - nw);
            // (a box exactly as wide as the text may still get an ellipsis from rounding)
            gfx.Text(fr.dir, { tx, rr.y, dw < full ? dw : dw + DR(2), rr.h }, F::UI, TextDim);
            gfx.Text(fr.name, { tx + dw, rr.y, avail - dw, rr.h }, F::UI, Text);
        } else {
            gfx.Text(fr.name, { tx, rr.y, avail, rr.h }, F::UI, Text);
        }
    }
    return y + (float)n * pitch;
}

void App::PaintCommitDetails(RepoTab& t, const RectF& r) {
    const RepoData& d = *t.data;
    const Commit& cm = d.commits[(size_t)t.selCommit];
    const Author& au = d.authors[cm.author];
    const std::shared_ptr<CommitDetails> det = t.details;
    const bool ok = det && det->ok && det->id == cm.id;
    const bool stash = (cm.flags & CF_STASH) && cm.stash >= 0;
    const float pad = DR(20);
    const float x = r.x + pad, w = r.w - 2 * pad - DR(4);
    float y = (float)(r.y - t.ds.pos) + DR(20);

    // "Commit [a3f9c21 (copy)]" and the commit's menu
    {
        const wchar_t* label = stash ? L"Stash" : L"Commit";
        const float lw = gfx.Measure(label, F::Small);
        gfx.Text(label, { x, y, lw + 2, DR(32) }, F::Small, TextDim);
        const std::wstring id = stash ? d.stashes[(size_t)cm.stash].name : cm.id.Hex(7);
        const float sw = gfx.Measure(id, F::Mono);
        const RectF chip{ x + lw + DR(8), y + DR(3), DR(8) + sw + DR(6) + DR(14) + DR(8), DR(26) };
        const bool hov = Hov(Hit::DetailSha);
        Button(chip, L"", 0, hov, true);
        gfx.Text(id, { chip.x + DR(8), chip.y, sw + 2, chip.h }, F::Mono, Text);
        gfx.Glyph(0xE8C8, { chip.r() - DR(8) - DR(16), chip.y, DR(16), chip.h }, F::Icon12, TextSec);
        AddZone(chip, Hit::DetailSha);
        if (hov) {
            tipText = L"Copy the commit ID";
            tipAnchor = chip;
        }
        const RectF mb{ x + w - DR(32), y, DR(32), DR(32) };
        if (Hov(Hit::DetailMore)) {
            gfx.FillR(mb, DR(4), Hover);
            tipText = L"More actions";
            tipAnchor = mb;
        }
        gfx.Glyph(0xE712, mb, F::Icon16, Text);
        AddZone(mb, Hit::DetailMore);
        y += DR(32) + DR(16);
    }

    // message
    {
        const std::wstring summary = stash ? d.stashes[(size_t)cm.stash].message : ok ? det->summary : Widen(d.Subject(cm));
        const std::wstring& body = ok ? det->body : std::wstring();
        auto ls = gfx.Layout(summary, F::BodyTitle, w);
        DWRITE_TEXT_METRICS ms{};
        if (ls) {
            ls->GetMetrics(&ms);
            gfx.dc()->DrawTextLayout({ x, y }, ls.Get(), gfx.B(Text), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        }
        y += ms.height;
        if (!body.empty() && !stash) {
            auto lb = gfx.Layout(body, F::Body, w);
            if (lb) {
                const float lh = DR(19.5f);  // a roomier line height for prose
                lb->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lh, lh * 0.78f);
                DWRITE_TEXT_METRICS mb{};
                lb->GetMetrics(&mb);
                gfx.dc()->DrawTextLayout({ x, y + DR(8) }, lb.Get(), gfx.B(TextSec), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                y += DR(8) + mb.height;
            }
        }
        y += DR(16);
    }

    // author, date and parents
    {
        Avatar(x + DR(16), y + DR(16), DR(16), au);
        const float tx = x + DR(42);
        gfx.Text(au.name, { tx, y - DR(1), w - DR(42), DR(18) }, F::UISemi, Text);
        float lx = tx;
        const float ly = y + DR(17);
        auto dim = [&](const std::wstring& s) {
            const float sw = gfx.Measure(s, F::Small);
            gfx.Text(s, { lx, ly, std::max(0.f, x + w - lx), DR(17) }, F::Small, TextDim);
            lx += sw;
        };
        dim(FormatDateLong(cm.authorTime) + L" · ");
        const int32_t* ps = d.Parents(cm);
        if (cm.parentCount == 0) dim(L"root commit");
        else {
            dim(cm.parentCount > 1 ? L"parents " : L"parent ");
            for (int k = 0; k < (int)cm.parentCount; ++k) {
                const std::wstring ph = ps[k] >= 0 ? d.commits[(size_t)ps[k]].id.Hex(7)
                                        : ok && k < (int)det->parents.size() ? det->parents[(size_t)k].Hex(7)
                                                                             : std::wstring(L"???????");
                const float pw = gfx.Measure(ph, F::MonoSmall);
                const RectF pr{ lx - DR(2), ly, pw + DR(4), DR(17) };
                const bool hov = ps[k] >= 0 && Hov(Hit::DetailParent, k);
                if (hov) gfx.FillR(pr, DR(3), Hover);
                gfx.Text(ph, { lx, ly, pw + 2, DR(17) }, F::MonoSmall, ps[k] >= 0 ? (hov ? Text : TextSec) : TextDim);
                if (ps[k] >= 0) AddZone(pr, Hit::DetailParent, k, ps[k]);
                lx += pw + DR(6);
            }
        }
        y += DR(32) + DR(8);
        if (const std::wstring by = ok ? CommitterNote(d, cm, *det) : std::wstring(); !by.empty()) {
            gfx.Text(by, { x, y, w, DR(17) }, F::Small, TextDim);
            y += DR(17) + DR(8);
        }
        y += DR(8);
    }

    // actions
    {
        struct Act {
            const wchar_t* label;
            bool enabled;
            std::wstring tip;
        };
        std::vector<Act> acts;
        if (stash) {
            acts = { { L"Pop", !t.opBusy, L"Apply the stash to the working tree and delete it" },
                     { L"Apply", !t.opBusy, L"Apply the stash and keep it" },
                     { L"Delete", !t.opBusy, L"Delete the stash" } };
        } else {
            std::wstring tip;
            const bool on = t.CanCheckout(t.selCommit, &tip);  // a branch at this commit (a local one first), else the commit
            std::wstring branchTip;
            const bool branchOn = ToolEnabled(&t, 4, &branchTip);
            const bool staged = t.status && !t.status->staged.empty();  // git revert commits from the index
            acts = { { L"Check out", on, tip },
                     { L"New branch here", branchOn, L"Create a branch at this commit (Ctrl+B)" },
                     { L"Revert", !t.opBusy && !d.unborn && !staged,
                       staged ? L"Commit or unstage the staged changes first" : L"Create a commit that undoes this one" } };
        }
        float bx = x, by = y;
        for (size_t k = 0; k < acts.size(); ++k) {
            const F font = k == 0 ? F::UISemi : F::UI;
            const float bw = gfx.Measure(acts[k].label, font) + DR(28);
            if (bx > x && bx + bw > x + w) {
                bx = x;
                by += DR(40);
            }
            const RectF b{ bx, by, bw, DR(32) };
            const bool hov = Hov(Hit::DetailAction, (int)k);
            Button(b, acts[k].label, k == 0 ? 1 : 0, hov, acts[k].enabled, font);
            AddZone(b, Hit::DetailAction, (int)k);
            if (hov) {
                tipText = acts[k].tip;
                tipAnchor = b;
            }
            bx += bw + DR(8);
        }
        y = by + DR(32) + DR(20);
    }
    gfx.HLine(x, x + w, y, DividerSoft);
    y += DR(16);

    if (!ok) {
        if (det && !det->ok && det->id == cm.id) gfx.Text(det->error, { x, y, w, DR(22) }, F::Small, Deleted);
        else {
            Spinner(x + DR(8), y + DR(11), DR(6));
            gfx.Text(L"Loading changes…", { x + DR(24), y, w, DR(22) }, F::Small, TextSec);
        }
        t.ds.view = r.h;
        t.ds.content = y + t.ds.pos - r.y + DR(40);
        t.ds.Clamp();
        return;
    }

    // changed files: count, lines, list or tree
    {
        const RectF tb{ x + w - DR(28), y, DR(28), DR(26) };
        const bool hov = Hov(Hit::DetailMode, t.treeMode ? 0 : 1);
        if (hov) {
            gfx.FillR(tb, DR(4), Hover);
            tipText = t.treeMode ? L"Show as a list" : L"Show as a tree";
            tipAnchor = tb;
        }
        gfx.Glyph(t.treeMode ? 0xE8FD : 0xE8B7, tb, F::Icon12, TextSec);
        AddZone(tb, Hit::DetailMode, t.treeMode ? 0 : 1);
        const std::wstring stats = FilesSummary(*det);
        const float sw = gfx.Measure(stats, F::Small);
        gfx.Text(stats, { tb.x - DR(8) - sw, y, sw + 2, DR(26) }, F::Small, TextDim);
        gfx.Text(L"Changed files", { x, y, tb.x - DR(16) - sw - x, DR(26) }, F::RowTitle, Text);
        y += DR(26) + DR(8);
    }
    if (det->files.empty()) {
        gfx.Text(L"No file changes", { x, y, w, DR(22) }, F::UI, TextDim);
        y += DR(30);
    }
    y = PaintFileRows(t, r, x - DR(8), w + DR(16), y, 0, det->files);
    t.ds.view = r.h;
    t.ds.content = y + t.ds.pos - r.y + DR(20);
    t.ds.Clamp();
}

void App::PaintWipDetails(RepoTab& t, const RectF& panel) {
    // Scrollable file lists on top, the commit box pinned to the bottom.
    const float boxH = CommitBoxHeight();
    const RectF r{ panel.x, panel.y, panel.w, std::max(DR(80), panel.h - boxH) };
    gfx.PushClip(r);
    const float pad = DR(20);
    const float x = r.x + pad, w = r.w - 2 * pad - DR(4);
    float y = (float)(r.y - t.ds.pos) + DR(20);
    const RepoData& d = *t.data;
    const WorkStatus* st = t.status.get();

    // "Working tree [branch]"
    {
        const float lw = gfx.Measure(L"Working tree", F::Small);
        gfx.Text(L"Working tree", { x, y, lw + 2, DR(32) }, F::Small, TextDim);
        const std::wstring br = !d.headBranch.empty() ? d.headBranch : L"detached HEAD";
        const float bw = std::min(gfx.Measure(br, F::UI), w - lw - DR(60));
        const RectF chip{ x + lw + DR(8), y + DR(3), DR(30) + bw + DR(10), DR(26) };
        gfx.FillR(chip, DR(4), Subtle);
        gfx.StrokeR(chip, DR(4), ControlStroke);
        BranchIcon(chip.x + DR(15), chip.cy(), DR(13), AccentText);
        gfx.Text(br, { chip.x + DR(28), chip.y, bw + 2, chip.h }, F::UI, Text);
        y += DR(32) + DR(16);
    }
    gfx.Text(L"Uncommitted changes", { x, y, w, DR(26) }, F::BodyTitle, Text);
    y += DR(26) + DR(4);
    if (st) {
        std::wstring sum = Plural((size_t)ChangedFiles(t), L"file changed", L"files changed");
        gfx.Text(sum, { x, y, w, DR(17) }, F::Small, TextDim);
    }
    y += DR(17) + DR(16);
    if (!st) {
        Spinner(x + DR(8), y + DR(11), DR(6));
        gfx.Text(L"Reading the working tree…", { x + DR(24), y, w, DR(22) }, F::Small, TextSec);
        gfx.PopClip();
        PaintCommitBox(t, { panel.x, r.b(), panel.w, panel.b() - r.b() });
        return;
    }
    auto section = [&](int id, const wchar_t* title, const std::vector<FileChange>& files, bool collapsed) {
        const RectF hr{ x - DR(8), y, w + DR(16), DR(32) };
        if (Hov(Hit::DetailSection, id)) gfx.FillR(hr, DR(4), Hover);
        gfx.Glyph(collapsed ? 0xE76C : 0xE70D, { hr.x + DR(4), hr.y, DR(16), hr.h }, F::Icon10, TextSec);
        AddZone(hr, Hit::DetailSection, id);
        float titleR = hr.r() - DR(8);
        if (!files.empty()) {  // "Stage all" / "Unstage all"
            const bool stageAll = id == 1;
            const wchar_t* label = stageAll ? L"Stage all" : L"Unstage all";
            const int act = stageAll ? kStageAll : kUnstageAll;
            const float bw = gfx.Measure(label, F::Small) + DR(20);
            const RectF b{ hr.r() - DR(4) - bw, hr.y + DR(4), bw, hr.h - DR(8) };
            Button(b, label, 0, Hov(Hit::WipAction, act), true, F::Small);
            AddZone(b, Hit::WipAction, act);
            titleR = b.x - DR(8);
        }
        const float tw = gfx.Measure(title, F::RowTitle);
        gfx.Text(title, { hr.x + DR(26), hr.y, tw + 2, hr.h }, F::RowTitle, Text);
        const std::wstring cnt = FormatCount(files.size());
        gfx.Text(cnt, { hr.x + DR(26) + tw + DR(8), hr.y, std::max(0.f, titleR - (hr.x + DR(34) + tw)), hr.h }, F::Small, TextDim);
        y += DR(32) + DR(4);
        if (!collapsed) y = PaintFileRows(t, r, x - DR(8), w + DR(16), y, id, files) + DR(12);
        else y += DR(8);
    };
    section(1, L"Unstaged", st->unstaged, t.unstagedCollapsed);
    section(2, L"Staged", st->staged, t.stagedCollapsed);
    t.ds.view = r.h;
    t.ds.content = y + t.ds.pos - r.y + DR(16);
    t.ds.Clamp();
    gfx.PopClip();
    PaintScrollbar(r, t.ds, 2);
    PaintCommitBox(t, { panel.x, r.b(), panel.w, panel.b() - r.b() });
}
