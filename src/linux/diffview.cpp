// The diff of one file, shown in place of the history. Rows have a fixed height and only the visible ones are
// drawn; the text scrolls sideways under line numbers that stay in place. Changes of the working tree or of the
// index can be staged, unstaged or discarded by hunk or by selected lines (click, Shift+click or drag,
// Ctrl+click).

#include "LinuxUI.h"

namespace {

int Digits(int n) {
    int d = 1;
    while (n >= 10) {
        n /= 10;
        ++d;
    }
    return d;
}

bool IsBanner(const DiffLine& L) { return L.kind == DiffLine::Hunk || L.kind == DiffLine::Info; }

// Where things are, for the current width and layout.
struct Geo {
    bool split = false;
    float width = 0, numW = 0, markW = 16, half = 0, textW = 0;
    float textX[2] = { 0, 0 };  // unified: [0]
    int64_t rows = 0;
};

graphene_rect_t Box(float x, float y, float w, float h) {
    graphene_rect_t r;
    graphene_rect_init(&r, x, y, w, h);
    return r;
}

}  // namespace

class DiffCanvas;

class DiffPane {
public:
    explicit DiffPane(RepoTab* o) : owner(o) {}
    ~DiffPane() {
        if (owner && owner->diffPane == this) owner->diffPane = nullptr;
    }
    RepoTab* owner;
    GtkWidget* root = nullptr;
    GtkWidget* letter = nullptr;
    GtkWidget* where = nullptr;
    GtkWidget* path = nullptr;
    GtkWidget* plus = nullptr;
    GtkWidget* minus = nullptr;
    GtkWidget* fileButton = nullptr;
    GtkWidget* prevHunk = nullptr;
    GtkWidget* nextHunk = nullptr;
    GtkWidget* unified = nullptr;
    GtkWidget* split = nullptr;
    GtkWidget* body = nullptr;  // a stack: the lines, or a message
    GtkWidget* message = nullptr;
    GtkWidget* bar = nullptr;   // the selected lines
    GtkWidget* barLabel = nullptr;
    GtkWidget* barMain = nullptr;
    GtkWidget* barDiscard = nullptr;
    DiffCanvas* canvas = nullptr;
    std::string shownKey;  // the file on screen: another one starts from the top
    bool hasStats = false;
    bool syncing = false;
    uint64_t waitSeq = 0;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    RepoSession* Session() const { return owner ? owner->session() : nullptr; }
    void Build();
    void Reload();
    void UpdateBar();
    void FitHeader();
    void HunkAction(int hunk, bool discard) {
        if (RepoSession* s = Session()) owner->ApplyPatch(s->HunkJob(hunk, discard));
    }
};

class DiffCanvas : public Canvas {
public:
    explicit DiffCanvas(DiffPane* p) : pane(p) {}
    ~DiffCanvas() override {
        if (autoscroll) g_source_remove(autoscroll);
        if (pane && pane->canvas == this) pane->canvas = nullptr;
    }
    DiffPane* pane;
    float rowH = 20, charW = 8, numCharW = 7, lineH = 16;
    bool metricsReady = false;
    bool splitShown = false;
    bool dragging = false;
    double dragX = 0, dragY = 0;  // the pointer while dragging, in the view
    guint autoscroll = 0;
    bool focused = false;

    std::shared_ptr<FileDiff> Diff() const {
        RepoSession* s = pane ? pane->Session() : nullptr;
        return s && s->diff.open && s->diff.diff && s->diff.diff->ok ? s->diff.diff : nullptr;
    }
    void Metrics() {
        if (metricsReady || !widget) return;
        charW = draw::TextWidth(widget, "0000000000", Font().mono) / 10;
        numCharW = draw::TextWidth(widget, "0000000000", Font().monoSmall) / 10;
        lineH = draw::LineHeight(widget, Font().mono);
        rowH = std::max(18.0f, roundf(lineH + 3));
        metricsReady = true;
    }
    Geo GeoOf(const FileDiff& d) {
        Metrics();
        Geo g;
        g.width = Width();
        g.split = ShowSplit(AppSettings().diffSplit, d);
        g.numW = roundf((float)Digits(std::max(999, d.maxLineNo)) * numCharW + 14);
        if (!g.split) {
            g.textX[0] = 2 * g.numW + g.markW;
            g.textW = g.width - g.textX[0] - 8;
            g.rows = (int64_t)d.lines.size();
        } else {
            g.half = floorf(g.width / 2);
            g.textX[0] = g.numW + g.markW;
            g.textX[1] = g.half + 1 + g.numW + g.markW;
            g.textW = g.half - g.numW - g.markW - 6;
            g.rows = (int64_t)d.split.size();
        }
        return g;
    }
    double ContentHeight() override {
        const std::shared_ptr<FileDiff> d = Diff();
        return d ? (double)(GeoOf(*d).rows + 2) * rowH : 0;
    }
    double ContentWidth() override {  // how far the text scrolls sideways
        const std::shared_ptr<FileDiff> d = Diff();
        if (!d) return 0;
        const Geo g = GeoOf(*d);
        return Width() + std::max(0.0f, (float)d->maxLen * charW + 60 - g.textW);
    }
    void Resized() override {
        metricsReady = false;
        // a narrow view keeps room for the path (later: not while the layout runs)
        Later([p = pane, a = Alive()] {
            if (a.lock() && p) p->FitHeader();
        });
    }
    void Draw(GtkSnapshot* s, float w, float h) override;
    void DrawBanner(GtkSnapshot* s, const FileDiff& d, int32_t idx, float y, const DiffState& m, bool canApply);
    void DrawText(GtkSnapshot* s, const FileDiff& d, int32_t idx, float x, float y, float w);
    void DrawNumber(GtkSnapshot* s, int n, float x, float y, float w);
    void DrawLineCell(GtkSnapshot* s, const FileDiff& d, int32_t idx, float cx, float y, float cw, const DiffState& m, bool canApply, const Geo& g, int side);
    void HunkButtons(const DiffState& m, float y, float w, graphene_rect_t* main, graphene_rect_t* discard);
    int32_t LineAt(double x, double y, const FileDiff& d, const Geo& g) const;
    int32_t BannerAt(int64_t row, const FileDiff& d, const Geo& g) const;
    void DiffChanged(bool anotherFile);
    void Jump(int dir);
    void DragTo(double x, double y);
};

// ---------------------------------------------------------------- the lines

// The diff, its layout or the width changed. Another file starts from the top; a new layout keeps the same lines
// at the top.
void DiffCanvas::DiffChanged(bool anotherFile) {
    const std::shared_ptr<FileDiff> d = Diff();
    if (!d) return;
    const Geo g = GeoOf(*d);
    int64_t top = (int64_t)floor(ScrollY() / rowH);
    const bool relayout = !anotherFile && g.split != splitShown;
    if (relayout && !d->lines.empty() && !d->split.empty()) {
        if (g.split) {
            top = SplitRowOfLine(*d, (int)std::min<int64_t>(top, (int64_t)d->lines.size() - 1));
        } else {
            const SplitRow& sr = d->split[(size_t)std::min<int64_t>(top, (int64_t)d->split.size() - 1)];
            top = std::max(0, sr.left >= 0 ? sr.left : sr.right);
        }
    }
    splitShown = g.split;
    Refresh();
    if (anotherFile) {
        SetScrollX(0);
        SetScrollY(0);
    } else if (relayout) {
        SetScrollY((double)top * rowH);
    }
}

// Next (1) or previous (-1) hunk, one row below the top.
void DiffCanvas::Jump(int dir) {
    const std::shared_ptr<FileDiff> d = Diff();
    if (!d || d->hunks.empty()) return;
    const Geo g = GeoOf(*d);
    const double top = ScrollY() / rowH + 1.0;
    int target = -1;
    for (const DiffHunk& h : d->hunks) {
        const int row = g.split ? h.splitRow : h.line;
        if (row < 0) continue;
        if (dir > 0 && row > top + 0.5) {
            target = row;
            break;
        }
        if (dir < 0 && row < top - 0.5) target = row;
    }
    if (target >= 0) SetScrollY((double)std::max(0, target - 1) * rowH);
}

// The hunk buttons of a hunk header row: Stage / Unstage, and Discard for unstaged changes.
void DiffCanvas::HunkButtons(const DiffState& m, float y, float w, graphene_rect_t* main, graphene_rect_t* discard) {
    const char* label = m.req.section == 1 ? "Stage Hunk" : "Unstage Hunk";
    const float bw = draw::TextWidth(widget, label, Font().small) + 18;
    *main = Box(w - 12 - bw, y + 2, bw, rowH - 4);
    *discard = m.req.section == 1 ? Box(main->origin.x - 6 - 24, y + 1, 24, rowH - 2) : Box(0, 0, 0, 0);
}

void DiffCanvas::DrawBanner(GtkSnapshot* s, const FileDiff& d, int32_t idx, float y, const DiffState& m, bool canApply) {
    const DiffLine& L = d.lines[(size_t)idx];
    const float W = Width();
    const bool hunk = L.kind == DiffLine::Hunk;
    draw::Rect(s, 0, y, W, rowH, hunk ? WithAlpha(Pal().accent, 0.1f) : Pal().Fg(0.05f));
    float right = W - 12;
    if (hunk && canApply && L.hunk >= 0) {
        graphene_rect_t main, discard;
        HunkButtons(m, y, W, &main, &discard);
        const bool busy = m.HunkBusy();
        const GdkRGBA c = m.req.section == 1 ? AddedColor() : DeletedColor();
        draw::Round(s, main.origin.x, main.origin.y, main.size.width, main.size.height, 4, WithAlpha(c, busy ? 0.05f : 0.15f));
        draw::RoundBorder(s, main.origin.x, main.origin.y, main.size.width, main.size.height, 4, 1, WithAlpha(c, busy ? 0.25f : 0.7f));
        draw::Text(s, widget, m.req.section == 1 ? "Stage Hunk" : "Unstage Hunk", Font().small, Pal().Fg(busy ? 0.35f : 0.9f), main.origin.x + 9,
                   main.origin.y, main.size.width - 16, main.size.height);
        right = main.origin.x - 10;
        if (discard.size.width > 0) {
            draw::Icon(s, widget, "user-trash-symbolic", discard.origin.x + 4, y + (rowH - 16) / 2, 16, Pal().Fg(busy ? 0.3f : 0.6f));
            right = discard.origin.x - 8;
        }
    }
    const std::string text = U8(std::wstring_view(d.text).substr(L.off, L.len));
    draw::Text(s, widget, text, Font().monoSmall, hunk ? Pal().accentText : Pal().Fg(0.55f), 12, y, std::max(0.0f, right - 12), rowH);
}

// The text of a line from x, scrolled, inside [x, x + w].
void DiffCanvas::DrawText(GtkSnapshot* s, const FileDiff& d, int32_t idx, float x, float y, float w) {
    const DiffLine& L = d.lines[(size_t)idx];
    const std::wstring_view text = std::wstring_view(d.text).substr(L.off, L.len);
    const float hscroll = (float)ScrollX();
    // tabs become spaces (stops every 4 columns); the changed words move with them
    std::wstring line;
    line.reserve(text.size());
    size_t hs = std::wstring::npos, he = std::wstring::npos;
    for (size_t i = 0; i < text.size(); ++i) {
        if (L.hlLen && i == L.hlStart) hs = line.size();
        if (L.hlLen && i == (size_t)L.hlStart + L.hlLen) he = line.size();
        if (text[i] == L'\t') line.append(4 - line.size() % 4, L' ');
        else if (text[i] != L'\r') line.push_back(text[i]);
    }
    if (hs != std::wstring::npos && he == std::wstring::npos) he = line.size();
    // very long lines: only the part on screen
    size_t from = 0, count = line.size();
    if (line.size() > 600) {
        from = (size_t)std::max(0.0f, floorf(hscroll / charW) - 8);
        from = std::min(from, line.size());
        count = std::min(line.size() - from, (size_t)(w / charW) + 24);
    }
    const float x0 = x - hscroll + (float)from * charW;
    const std::wstring_view part = std::wstring_view(line).substr(from, count);
    // the UTF-8 text, and where the changed words start and end in it
    std::string u8;
    u8.reserve(part.size());
    int hb = -1, heb = -1;
    for (size_t i = 0; i <= part.size(); ++i) {
        if (hs != std::wstring::npos && from + i == std::max(hs, from)) hb = (int)u8.size();
        if (he != std::wstring::npos && from + i == std::min(he, from + count)) heb = (int)u8.size();
        if (i < part.size()) {
            char buf[8];
            const int n = g_unichar_to_utf8((gunichar)part[i], buf);
            u8.append(buf, (size_t)n);
        }
    }
    const graphene_rect_t clip = Box(x, y, w, rowH);
    gtk_snapshot_push_clip(s, &clip);
    PangoLayout* layout = gtk_widget_create_pango_layout(widget, nullptr);
    pango_layout_set_font_description(layout, Font().mono);
    pango_layout_set_single_paragraph_mode(layout, TRUE);
    pango_layout_set_text(layout, u8.data(), (int)u8.size());
    const float ty = roundf(y + (rowH - lineH) / 2);
    if (hb >= 0 && heb > hb && IsChange(L)) {
        PangoRectangle a, b;
        pango_layout_index_to_pos(layout, hb, &a);
        pango_layout_index_to_pos(layout, heb, &b);
        const float hx = x0 + (float)a.x / PANGO_SCALE, hw = std::max(3.0f, (float)(b.x - a.x) / PANGO_SCALE);
        draw::Round(s, hx, y + 2, hw, rowH - 4, 2, WithAlpha(L.kind == DiffLine::Added ? AddedColor() : DeletedColor(), Pal().dark ? 0.3f : 0.25f));
    }
    const graphene_point_t p = { roundf(x0), ty };
    gtk_snapshot_save(s);
    gtk_snapshot_translate(s, &p);
    gtk_snapshot_append_layout(s, layout, &Pal().fg);
    gtk_snapshot_restore(s);
    int lw = 0, lh = 0;
    pango_layout_get_pixel_size(layout, &lw, &lh);
    g_object_unref(layout);
    if (L.noEol) {  // what git prints as "\ No newline at end of file"
        const char* note = "no newline at end of file";
        const float end = x0 + (from == 0 ? (float)lw : (float)count * charW);
        const float nw = draw::TextWidth(widget, note, Font().small) + 12;
        draw::Round(s, end + 10, y + 2, nw, rowH - 4, 3, Pal().Fg(0.07f));
        draw::Text(s, widget, note, Font().small, Pal().Fg(0.55f), end + 16, y + 2, nw - 10, rowH - 4);
    }
    gtk_snapshot_pop(s);
}

void DiffCanvas::DrawNumber(GtkSnapshot* s, int n, float x, float y, float w) {
    if (n < 0) return;
    const std::string t = std::to_string(n);
    draw::Text(s, widget, t, Font().monoSmall, Pal().Fg(0.4f), x, y, w - 8, rowH, PANGO_ELLIPSIZE_NONE, true);
}

// Background, selection, line number(s) and marker of one line in a cell of a row.
void DiffCanvas::DrawLineCell(GtkSnapshot* s, const FileDiff& d, int32_t idx, float cx, float y, float cw, const DiffState& m, bool canApply,
                              const Geo& g, int side) {
    const DiffLine& L = d.lines[(size_t)idx];
    const float bgAlpha = Pal().dark ? 0.16f : 0.13f;
    if (L.kind == DiffLine::Added) draw::Rect(s, cx, y, cw, rowH, WithAlpha(AddedColor(), bgAlpha));
    else if (L.kind == DiffLine::Deleted) draw::Rect(s, cx, y, cw, rowH, WithAlpha(DeletedColor(), bgAlpha));
    const float markX = g.split ? cx + g.numW : cx + 2 * g.numW;
    if (canApply && IsChange(L) && m.Selected(idx)) {
        draw::Rect(s, cx, y, cw, rowH, WithAlpha(Pal().accent, 0.26f));
        draw::Rect(s, markX, y, 3, rowH, Pal().accent);
    }
    if (g.split) {
        DrawNumber(s, side == 0 ? L.oldNo : L.newNo, cx, y, g.numW);
    } else {
        DrawNumber(s, L.oldNo, cx, y, g.numW);
        DrawNumber(s, L.newNo, cx + g.numW, y, g.numW);
    }
    if (L.kind == DiffLine::Added) draw::Text(s, widget, "+", Font().mono, AddedColor(), markX + 4, y, g.markW - 4, rowH, PANGO_ELLIPSIZE_NONE);
    else if (L.kind == DiffLine::Deleted) draw::Text(s, widget, "\u2212", Font().mono, DeletedColor(), markX + 4, y, g.markW - 4, rowH, PANGO_ELLIPSIZE_NONE);
}

void DiffCanvas::Draw(GtkSnapshot* s, float W, float H) {
    RepoSession* ss = pane ? pane->Session() : nullptr;
    const std::shared_ptr<FileDiff> dp = Diff();
    draw::Rect(s, 0, 0, W, H, Pal().viewBg);
    if (!ss || !dp || dp->lines.empty()) return;
    const FileDiff& d = *dp;
    const DiffState& m = ss->diff;
    const Geo g = GeoOf(d);
    const bool canApply = m.req.section > 0 && CanApplyBack(d);
    const double top = ScrollY();
    const int64_t first = std::max<int64_t>(0, (int64_t)floor(top / rowH));
    const int64_t last = std::min<int64_t>(g.rows, (int64_t)ceil((top + H) / rowH) + 1);
    // gutters with the line numbers
    draw::Rect(s, 0, 0, g.split ? g.numW : 2 * g.numW, H, Pal().Fg(0.035f));
    if (g.split) draw::Rect(s, g.half + 1, 0, g.numW, H, Pal().Fg(0.035f));
    for (int64_t r = first; r < last; ++r) {
        const float y = (float)((double)r * rowH - top);
        if (!g.split) {
            const int32_t idx = (int32_t)r;
            if (IsBanner(d.lines[(size_t)idx])) {
                DrawBanner(s, d, idx, y, m, canApply);
                continue;
            }
            DrawLineCell(s, d, idx, 0, y, g.width, m, canApply, g, 0);
            DrawText(s, d, idx, g.textX[0], y, g.textW);
            continue;
        }
        const SplitRow& sr = d.split[(size_t)r];
        if (sr.left >= 0 && sr.left == sr.right && IsBanner(d.lines[(size_t)sr.left])) {
            DrawBanner(s, d, sr.left, y, m, canApply);
            continue;
        }
        for (int side = 0; side < 2; ++side) {
            const int32_t idx = side == 0 ? sr.left : sr.right;
            const float cx = side == 0 ? 0 : g.half + 1, cw = g.half - (side == 1 ? 1 : 0);
            if (idx < 0) {
                draw::Rect(s, cx, y, cw, rowH, Pal().Fg(0.05f));
                continue;
            }
            DrawLineCell(s, d, idx, cx, y, cw, m, canApply, g, side);
            DrawText(s, d, idx, g.textX[side], y, g.textW);
        }
    }
    if (g.split) draw::Rect(s, g.half, 0, 1, H, Pal().Fg(0.12f));
    if (d.truncated && last >= g.rows)
        draw::Text(s, widget, "The diff is too large and was cut here.", Font().small, Pal().warning, 16, (float)((double)g.rows * rowH - top) + 4,
                   g.width - 32, rowH);
}

// ---------------------------------------------------------------- mouse and keyboard

// The changed line at a point of the view (the side under it, in the split layout), or -1.
int32_t DiffCanvas::LineAt(double x, double y, const FileDiff& d, const Geo& g) const {
    const int64_t row = (int64_t)floor((y + ScrollY()) / rowH);
    if (row < 0 || row >= g.rows) return -1;
    int32_t idx = (int32_t)row;
    if (g.split) {
        const SplitRow& sr = d.split[(size_t)row];
        idx = x < g.half ? sr.left : sr.right;
    }
    return idx >= 0 && IsChange(d.lines[(size_t)idx]) ? idx : -1;
}

// The hunk header line of a row, or -1.
int32_t DiffCanvas::BannerAt(int64_t row, const FileDiff& d, const Geo& g) const {
    if (row < 0 || row >= g.rows) return -1;
    if (!g.split) return d.lines[(size_t)row].kind == DiffLine::Hunk ? (int32_t)row : -1;
    const SplitRow& sr = d.split[(size_t)row];
    return sr.left >= 0 && sr.left == sr.right && d.lines[(size_t)sr.left].kind == DiffLine::Hunk ? sr.left : -1;
}

void DiffCanvas::DragTo(double x, double y) {
    RepoSession* s = pane ? pane->Session() : nullptr;
    const std::shared_ptr<FileDiff> d = Diff();
    if (!s || !d) return;
    const Geo g = GeoOf(*d);
    const int32_t idx = LineAt(x, std::clamp(y, 1.0, (double)Height() - 1), *d, g);
    if (idx >= 0) s->SelectDiffLine(idx, true, false);
}

static void OnPressed(GtkGestureClick* gesture, int, double x, double y, gpointer data) {
    DiffCanvas* c = static_cast<DiffCanvas*>(data);
    gtk_widget_grab_focus(c->widget);
    RepoSession* s = c->pane ? c->pane->Session() : nullptr;
    const std::shared_ptr<FileDiff> d = c->Diff();
    if (!s || !d) return;
    if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) != GDK_BUTTON_PRIMARY) return;
    const Geo g = c->GeoOf(*d);
    const bool canApply = s->diff.req.section > 0 && CanApplyBack(*d);
    const int64_t row = (int64_t)floor((y + c->ScrollY()) / c->rowH);
    const int32_t banner = c->BannerAt(row, *d, g);
    if (banner >= 0) {
        if (!canApply || s->diff.HunkBusy()) return;
        graphene_rect_t main, discard;
        c->HunkButtons(s->diff, (float)((double)row * c->rowH - c->ScrollY()), g.width, &main, &discard);
        const int hunk = d->lines[(size_t)banner].hunk;
        if (Inside(main, x, y)) c->pane->HunkAction(hunk, false);
        else if (discard.size.width > 0 && Inside(discard, x, y)) c->pane->HunkAction(hunk, true);
        return;
    }
    const int32_t idx = c->LineAt(x, y, *d, g);
    if (!canApply || idx < 0) return;
    const GdkModifierType mods = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
    const bool shift = mods & GDK_SHIFT_MASK, toggle = (mods & GDK_CONTROL_MASK) && !shift;
    s->SelectDiffLine(idx, shift, toggle);
    c->dragging = !toggle;
    c->dragX = x;
    c->dragY = y;
}

// Past the top or the bottom the view scrolls; the line there joins the selection.
static gboolean OnAutoscroll(gpointer data) {
    DiffCanvas* c = static_cast<DiffCanvas*>(data);
    if (!c->dragging) {
        c->autoscroll = 0;
        return G_SOURCE_REMOVE;
    }
    const double h = c->Height();
    double by = 0;
    if (c->dragY < 0) by = c->dragY;
    else if (c->dragY > h) by = c->dragY - h;
    if (by == 0) {
        c->autoscroll = 0;
        return G_SOURCE_REMOVE;
    }
    c->SetScrollY(c->ScrollY() + std::clamp(by, -60.0, 60.0) / 2);
    c->DragTo(c->dragX, c->dragY);
    return G_SOURCE_CONTINUE;
}

static void OnDragUpdate(GtkGestureDrag* g, double dx, double dy, gpointer data) {
    DiffCanvas* c = static_cast<DiffCanvas*>(data);
    if (!c->dragging) return;
    double sx = 0, sy = 0;
    gtk_gesture_drag_get_start_point(g, &sx, &sy);
    c->dragX = sx + dx;
    c->dragY = sy + dy;
    c->DragTo(c->dragX, c->dragY);
    if ((c->dragY < 0 || c->dragY > c->Height()) && !c->autoscroll) c->autoscroll = g_timeout_add(30, OnAutoscroll, c);
}

static void OnDragEnd(GtkGestureDrag*, double, double, gpointer data) {
    DiffCanvas* c = static_cast<DiffCanvas*>(data);
    c->dragging = false;
    if (c->autoscroll) {
        g_source_remove(c->autoscroll);
        c->autoscroll = 0;
    }
}

static gboolean OnKey(GtkEventControllerKey*, guint key, guint, GdkModifierType, gpointer data) {
    DiffCanvas* c = static_cast<DiffCanvas*>(data);
    const double page = c->Height() * 0.9;
    switch (key) {
    case GDK_KEY_Escape: c->pane->owner->Escape(); return TRUE;
    case GDK_KEY_Up: case GDK_KEY_KP_Up: c->SetScrollY(c->ScrollY() - 2 * c->rowH); return TRUE;
    case GDK_KEY_Down: case GDK_KEY_KP_Down: c->SetScrollY(c->ScrollY() + 2 * c->rowH); return TRUE;
    case GDK_KEY_Left: case GDK_KEY_KP_Left: c->SetScrollX(c->ScrollX() - c->charW * 8); return TRUE;
    case GDK_KEY_Right: case GDK_KEY_KP_Right: c->SetScrollX(c->ScrollX() + c->charW * 8); return TRUE;
    case GDK_KEY_Page_Up: case GDK_KEY_KP_Page_Up: c->SetScrollY(c->ScrollY() - page); return TRUE;
    case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down: case GDK_KEY_space: c->SetScrollY(c->ScrollY() + page); return TRUE;
    case GDK_KEY_Home: case GDK_KEY_KP_Home: c->SetScrollY(0); return TRUE;
    case GDK_KEY_End: case GDK_KEY_KP_End: c->SetScrollY(c->ContentHeight()); return TRUE;
    default: return FALSE;
    }
}

// ---------------------------------------------------------------- the pane

static GtkWidget* IconButton(const char* icon, const char* tip) {
    GtkWidget* b = gtk_button_new_from_icon_name(icon);
    AddClass(b, "flat");
    gtk_widget_set_tooltip_text(b, tip);
    return b;
}

void DiffPane::Build() {
    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    AddClass(root, "view mg-diff");

    // header: the file, then its actions
    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    AddClass(header, "mg-diff-header");
    letter = gtk_label_new("");
    AddClass(letter, "monospace mg-letter");
    where = MakeLabel("", "monospace caption dim-label");
    path = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(path), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_xalign(GTK_LABEL(path), 0);
    gtk_widget_set_hexpand(path, TRUE);
    plus = MakeLabel("", "monospace caption success");
    minus = MakeLabel("", "monospace caption error");
    fileButton = gtk_button_new_with_label("Stage File");
    prevHunk = IconButton("go-up-symbolic", "Previous change");
    nextHunk = IconButton("go-down-symbolic", "Next change");
    GtkWidget* layout = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    AddClass(layout, "linked");
    unified = gtk_toggle_button_new_with_label("Unified");
    split = gtk_toggle_button_new_with_label("Split");
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(split), GTK_TOGGLE_BUTTON(unified));
    gtk_box_append(GTK_BOX(layout), unified);
    gtk_box_append(GTK_BOX(layout), split);
    GtkWidget* close = IconButton("window-close-symbolic", "Close (Esc)");
    AddClass(close, "circular");
    for (GtkWidget* w : { letter, where, path, plus, minus, fileButton, prevHunk, nextHunk, layout, close }) gtk_box_append(GTK_BOX(header), w);
    gtk_widget_set_margin_start(prevHunk, 6);
    gtk_widget_set_margin_start(layout, 6);
    gtk_box_append(GTK_BOX(root), header);
    gtk_box_append(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    g_signal_connect(fileButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        DiffPane* p = static_cast<DiffPane*>(data);
        RepoSession* s = p->Session();
        if (!s || s->diff.req.section == 0) return;
        std::vector<std::wstring> paths{ s->diff.req.path };
        if (!s->diff.req.oldPath.empty()) paths.push_back(s->diff.req.oldPath);
        p->owner->StagePaths(paths, s->diff.req.section == 1);
    }), this);
    g_signal_connect(prevHunk, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        if (DiffCanvas* c = static_cast<DiffPane*>(data)->canvas) c->Jump(-1);
    }), this);
    g_signal_connect(nextHunk, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        if (DiffCanvas* c = static_cast<DiffPane*>(data)->canvas) c->Jump(1);
    }), this);
    g_signal_connect(split, "toggled", G_CALLBACK(+[](GtkToggleButton* b, gpointer data) {
        DiffPane* p = static_cast<DiffPane*>(data);
        if (p->syncing) return;
        AppSettings().diffSplit = gtk_toggle_button_get_active(b);
        AppSettings().Save();
        BroadcastSettingsChanged();
    }), this);
    g_signal_connect(close, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        if (RepoSession* s = static_cast<DiffPane*>(data)->Session()) s->CloseDiff();
    }), this);

    // body
    canvas = new DiffCanvas(this);
    GtkWidget* sw = MakeCanvas(canvas, Scroll::Both);
    gtk_widget_set_focusable(canvas->widget, TRUE);
    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(OnPressed), canvas);
    gtk_widget_add_controller(canvas->widget, GTK_EVENT_CONTROLLER(click));
    GtkGesture* drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-update", G_CALLBACK(OnDragUpdate), canvas);
    g_signal_connect(drag, "drag-end", G_CALLBACK(OnDragEnd), canvas);
    gtk_widget_add_controller(canvas->widget, GTK_EVENT_CONTROLLER(drag));
    GtkEventController* key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(OnKey), canvas);
    gtk_widget_add_controller(canvas->widget, key);

    message = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(message), TRUE);
    gtk_label_set_justify(GTK_LABEL(message), GTK_JUSTIFY_CENTER);
    gtk_label_set_max_width_chars(GTK_LABEL(message), 60);
    AddClass(message, "dim-label");
    body = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(body), sw, "lines");
    gtk_stack_add_named(GTK_STACK(body), message, "message");

    // the bar for the selected lines
    bar = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(bar), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_widget_set_halign(bar, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(bar, GTK_ALIGN_END);
    GtkWidget* barBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    AddClass(barBox, "mg-floating");
    barLabel = gtk_label_new("");
    barMain = gtk_button_new_with_label("Stage Lines");
    AddClass(barMain, "suggested-action");
    barDiscard = gtk_button_new_with_label("Discard\u2026");
    AddClass(barDiscard, "destructive-action");
    GtkWidget* clear = IconButton("window-close-symbolic", "Clear the selection (Esc)");
    AddClass(clear, "circular");
    for (GtkWidget* w : { barLabel, barMain, barDiscard, clear }) gtk_box_append(GTK_BOX(barBox), w);
    gtk_revealer_set_child(GTK_REVEALER(bar), barBox);
    for (GtkWidget* b : { barMain, barDiscard })
        g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer data) {
            DiffPane* p = static_cast<DiffPane*>(data);
            if (RepoSession* s = p->Session()) p->owner->ApplyPatch(s->LinesJob(GTK_WIDGET(b) == p->barDiscard));
        }), this);
    g_signal_connect(clear, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        if (RepoSession* s = static_cast<DiffPane*>(data)->Session()) s->ClearDiffSelection();
    }), this);

    GtkWidget* overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), body);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), bar);
    gtk_widget_set_vexpand(overlay, TRUE);
    gtk_box_append(GTK_BOX(root), overlay);
}

// In a narrow view the file's path keeps its room: "unstaged" and the line counts go first.
void DiffPane::FitHeader() {
    const int w = gtk_widget_get_width(root);
    if (w <= 0) return;
    gtk_widget_set_visible(where, w >= 640);
    gtk_widget_set_visible(plus, hasStats && w >= 540);
    gtk_widget_set_visible(minus, hasStats && w >= 540);
}

void DiffPane::Reload() {
    RepoSession* s = Session();
    if (!s || !s->diff.open) return;
    const DiffState& m = s->diff;
    const std::shared_ptr<FileDiff> d = m.diff;

    // header
    gtk_label_set_text(GTK_LABEL(letter), U8(std::wstring(1, StatusLetter(m.req.status))).c_str());
    for (const char* c : { "mg-st-m", "mg-st-a", "mg-st-d", "mg-st-r", "mg-st-u" }) gtk_widget_remove_css_class(letter, c);
    const wchar_t st = m.req.status;
    gtk_widget_add_css_class(letter, st == L'A' || st == L'?' ? "mg-st-a" : st == L'D' ? "mg-st-d" : st == L'R' || st == L'C' ? "mg-st-r" : st == L'U' ? "mg-st-u" : "mg-st-m");
    gtk_label_set_text(GTK_LABEL(where), U8(DiffWhere(m)).c_str());
    const std::wstring& p = m.req.path;
    const size_t slash = p.find_last_of(L'/');
    std::wstring dir = slash == std::wstring::npos ? L"" : p.substr(0, slash + 1);
    std::wstring old = m.req.oldPath;
    if (old.empty() && d) old = d->oldPath;
    if (!old.empty() && old != p) dir = old + L"  \u2192  " + dir;
    const std::string markup = "<span alpha=\"60%\">" + Esc(U8(dir)) + "</span><b>" + Esc(U8(slash == std::wstring::npos ? p : p.substr(slash + 1))) + "</b>";
    gtk_label_set_markup(GTK_LABEL(path), markup.c_str());
    gtk_widget_set_tooltip_text(path, U8(p).c_str());
    hasStats = d && d->ok && !d->binary && !d->lines.empty();
    if (hasStats) {
        gtk_label_set_text(GTK_LABEL(plus), ("+" + std::to_string(d->added)).c_str());
        gtk_label_set_text(GTK_LABEL(minus), ("\u2212" + std::to_string(d->deleted)).c_str());
    }
    FitHeader();
    gtk_widget_set_visible(fileButton, m.req.section != 0);
    gtk_button_set_label(GTK_BUTTON(fileButton), m.req.section == 1 ? "Stage File" : "Unstage File");
    gtk_widget_set_sensitive(fileButton, !s->opBusy);
    const bool hunks = d && d->hunks.size() > 1;
    gtk_widget_set_sensitive(prevHunk, hunks);
    gtk_widget_set_sensitive(nextHunk, hunks);
    syncing = true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(AppSettings().diffSplit ? split : unified), TRUE);
    syncing = false;
    const bool twoSides = !d || ShowSplit(true, *d);  // a file only added or only deleted has one side
    gtk_widget_set_sensitive(unified, twoSides);
    gtk_widget_set_sensitive(split, twoSides);

    // body
    const std::wstring notice = DiffNotice(m);
    if (notice.empty()) {
        gtk_stack_set_visible_child_name(GTK_STACK(body), "lines");
        const std::string key = U8(m.req.path) + "|" + m.req.commit.HexA() + "|" + std::to_string(m.req.section);
        const bool another = key != shownKey;
        shownKey = key;
        if (canvas) canvas->DiffChanged(another);
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(body), "message");
        gtk_label_set_text(GTK_LABEL(message), U8(notice).c_str());
        SetClass(message, "error", d && !d->ok);
        SetClass(message, "dim-label", !(d && !d->ok));
        gtk_widget_set_opacity(message, d ? 1 : 0);  // most diffs arrive before "Loading" is worth showing
        if (!d) {
            const uint64_t seq = waitSeq = m.seq;
            After(150, [this, seq, a = std::weak_ptr<bool>(alive)] {
                if (!a.lock()) return;
                RepoSession* rs = Session();
                if (rs && rs->diff.open && !rs->diff.diff && rs->diff.seq == seq && waitSeq == seq) gtk_widget_set_opacity(message, 1);
            });
        }
        shownKey.clear();
    }
    UpdateBar();
}

void DiffPane::UpdateBar() {
    RepoSession* s = Session();
    const DiffState& m = s->diff;
    const bool canApply = m.diff && m.diff->ok && m.req.section > 0 && CanApplyBack(*m.diff);
    const bool show = canApply && m.selCount > 0;
    gtk_revealer_set_reveal_child(GTK_REVEALER(bar), show);
    if (!show) return;
    const bool stage = m.req.section == 1, one = m.selCount == 1;
    gtk_label_set_text(GTK_LABEL(barLabel), (std::to_string(m.selCount) + (one ? " line selected" : " lines selected")).c_str());
    gtk_button_set_label(GTK_BUTTON(barMain), stage ? (one ? "Stage Line" : "Stage Lines") : (one ? "Unstage Line" : "Unstage Lines"));
    gtk_widget_set_visible(barDiscard, stage);
    gtk_widget_set_sensitive(barMain, !m.HunkBusy());
    gtk_widget_set_sensitive(barDiscard, !m.HunkBusy());
}

GtkWidget* MakeDiff(RepoTab* owner, DiffPane** out) {
    DiffPane* p = new DiffPane(owner);
    *out = p;
    p->Build();
    g_object_set_data_full(G_OBJECT(p->root), "mg-diff", p, [](gpointer d) { delete static_cast<DiffPane*>(d); });
    return p->root;
}

namespace diff {

void Reload(DiffPane* d) {
    if (d) d->Reload();
}

void Focus(DiffPane* d) {
    if (d && d->canvas && d->canvas->widget) gtk_widget_grab_focus(d->canvas->widget);
}

}  // namespace diff
