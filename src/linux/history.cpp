// The history: the branch and tag chips, the graph, two-line commit rows and the lines each commit changed.
// Drawn from the session's data, only the rows on screen; nothing is copied. Above it, the scope (all branches or
// the current one), the search results and the column titles.

#include "LinuxUI.h"

namespace {

constexpr float kLaneW = 14;   // distance between graph lanes
constexpr float kLaneX0 = 13;  // the first lane, from the column's left edge
constexpr float kNodeR = 4.5f;
constexpr float kChangesW = 92, kCommitMinW = 110, kRefsMinW = 40, kRefsMaxW = 480;

struct Columns {
    float refsX, refsW, graphX, graphW, commitX, commitW, changesX, changesW;
};

void Stroke(GtkSnapshot* s, GskPathBuilder* b, float width, const GdkRGBA& c, bool dashed) {
    GskPath* p = gsk_path_builder_free_to_path(b);
    GskStroke* st = gsk_stroke_new(width);
    if (dashed) {
        const float d[2] = { 3, 3 };
        gsk_stroke_set_dash(st, d, 2);
    }
    gtk_snapshot_append_stroke(s, p, st, &c);
    gsk_stroke_free(st);
    gsk_path_unref(p);
}

}  // namespace

class HistoryView : public Canvas {
public:
    explicit HistoryView(RepoTab* o) : owner(o) { refsW = std::clamp(AppSettings().GetFloat(L"colRef", 150), kRefsMinW, kRefsMaxW); }
    ~HistoryView() override;
    RepoTab* owner;
    Canvas* header = nullptr;
    GtkWidget* all = nullptr;
    GtkWidget* current = nullptr;
    GtkWidget* matches = nullptr;
    GtkWidget* prev = nullptr;
    GtkWidget* next = nullptr;
    float refsW = 150, graphW = 80;
    int hoverRow = -1;
    bool focused = false;
    bool syncing = false;
    std::vector<Oid> wantStats;
    bool statsQueued = false;

    float RowH() const {
        GtkWidget* w = widget;
        return std::max(46.0f, roundf(draw::LineHeight(w, Font().ui) + draw::LineHeight(w, Font().small) + 16));
    }
    Columns Cols(float width) const {
        Columns c;
        c.changesW = kChangesW;
        c.refsX = 0;
        c.refsW = refsW;
        c.graphX = refsW;
        c.graphW = graphW;
        c.commitX = c.graphX + c.graphW;
        c.commitW = std::max(kCommitMinW, width - c.commitX - c.changesW);
        c.changesX = c.commitX + c.commitW;
        return c;
    }
    RepoSession* Session() const {
        RepoSession* s = owner ? owner->session() : nullptr;
        return s && s->graph && s->data ? s : nullptr;
    }
    int64_t Rows() const {
        RepoSession* s = Session();
        return s ? (int64_t)s->graph->rows.size() : 0;
    }
    int RowAt(double y) const {
        const int64_t r = (int64_t)floor((y + ScrollY()) / RowH());
        return r >= 0 && r < Rows() ? (int)r : -1;
    }
    double ContentHeight() override { return (double)Rows() * RowH(); }
    void Draw(GtkSnapshot* s, float w, float h) override;
    void Scrolled() override { hoverRow = -1; }

    void DrawGraph(GtkSnapshot* s, const Graph& g, const RepoData& d, const GraphRow& row, float x0, float y0, float h);
    void DrawChip(GtkSnapshot* s, const RepoData& d, const GraphRow& row, float x, float y, float w, float h);
    void DrawCommit(GtkSnapshot* s, RepoSession* ss, const GraphRow& gr, float x, float y, float w, float h);
    void DrawChanges(GtkSnapshot* s, RepoSession* ss, const GraphRow& gr, float x, float y, float w, float h);
    void WantStats(const Oid& id);
    void RequestStats();
    void Reveal(bool center);
    void UpdateMatches();
    std::string RefsTip(int row) const;
};

class HistoryHeader : public Canvas {
public:
    HistoryView* view = nullptr;
    bool resizing = false;
    float startW = 0;
    ~HistoryHeader() override {
        if (view) view->header = nullptr;
    }
    float Boundary() const { return view ? view->refsW : 0; }
    void Draw(GtkSnapshot* s, float w, float h) override {
        if (!view) return;
        const Columns c = view->Cols(w);
        const GdkRGBA dim = Pal().Fg(0.55f);
        draw::Text(s, widget, "Branch / Tag", Font().small, dim, c.refsX + 8, 0, c.refsW - 16, h, PANGO_ELLIPSIZE_END, true);
        draw::Text(s, widget, "Commit", Font().small, dim, c.commitX + 6, 0, c.commitW - 12, h);
        draw::Text(s, widget, "Changes", Font().small, dim, c.changesX, 0, c.changesW - 12, h, PANGO_ELLIPSIZE_END, true);
        draw::Rect(s, c.refsX + c.refsW - 1, 7, 1, h - 14, Pal().Fg(0.15f));  // drag it to resize the column
        draw::Rect(s, 0, h - 1, w, 1, Pal().Fg(0.08f));
    }
};

HistoryView::~HistoryView() {
    if (header) static_cast<HistoryHeader*>(header)->view = nullptr;
    if (owner && owner->history == this) owner->history = nullptr;
}

// ---------------------------------------------------------------- drawing

void HistoryView::DrawGraph(GtkSnapshot* s, const Graph& g, const RepoData& d, const GraphRow& row, float x0, float y0, float h) {
    auto X = [&](int lane) { return roundf(x0 + kLaneX0 + (float)lane * kLaneW); };
    const float y1 = y0 + h, cy = roundf(y0 + h / 2);
    const float nx = X(row.lane);
    const float gap = kNodeR + 1;  // lines stop at the node, so hollow nodes stay hollow
    for (uint32_t k = 0; k < row.segCount; ++k) {
        const Seg& sg = g.segs[row.segOff + k];
        const float lx = X(sg.lane);
        GskPathBuilder* b = gsk_path_builder_new();
        if (sg.type == SEG_PASS) {
            gsk_path_builder_move_to(b, lx, y0);
            gsk_path_builder_line_to(b, lx, y1);
        } else if (sg.lane == row.lane) {
            if (sg.type == SEG_IN) {
                gsk_path_builder_move_to(b, lx, y0);
                gsk_path_builder_line_to(b, lx, cy - gap);
            } else {
                gsk_path_builder_move_to(b, nx, cy + gap);
                gsk_path_builder_line_to(b, nx, y1);
            }
        } else if (sg.type == SEG_IN) {  // from a lane above into the node
            const float hh = cy - y0;
            gsk_path_builder_move_to(b, lx, y0);
            gsk_path_builder_cubic_to(b, lx, y0 + hh * 0.57f, nx, cy - hh * 0.57f, nx, cy - gap);
        } else {  // from the node down into another lane
            const float hh = y1 - cy;
            gsk_path_builder_move_to(b, nx, cy + gap);
            gsk_path_builder_cubic_to(b, nx, cy + hh * 0.57f, lx, y1 - hh * 0.57f, lx, y1);
        }
        Stroke(s, b, 1.6f, sg.dashed ? Pal().Fg(0.3f) : LaneColor(sg.lane), sg.dashed);
    }
    // nodes: dots for commits, rings for merges, a dashed ring for the uncommitted changes, a square for stashes
    if (row.commit < 0) {
        draw::Ring(s, nx, cy, 5, 1.5f, Pal().Fg(0.55f), 2.4f);
        return;
    }
    const Commit& cm = d.commits[(size_t)row.commit];
    const GdkRGBA lc = LaneColor(row.lane);
    if (cm.flags & CF_STASH) {
        draw::RoundBorder(s, nx - 4.5f, cy - 4.5f, 9, 9, 2, 1.5f, Pal().Fg(0.55f));
    } else if (cm.flags & CF_MERGE) {
        draw::Ring(s, nx, cy, 4, 1.8f, lc);
    } else {
        draw::Circle(s, nx, cy, kNodeR, lc);
    }
}

// The first branch or tag of a commit as a chip, with local / remote / tag icons and "+N" for the others.
void HistoryView::DrawChip(GtkSnapshot* s, const RepoData& d, const GraphRow& row, float x, float y, float w, float h) {
    if (row.commit < 0) return;
    auto it = d.labels.find(row.commit);
    if (it == d.labels.end() || it->second.empty()) return;
    const Label& L = it->second[0];
    const int more = (int)it->second.size() - 1;
    const std::string text = U8(LabelText(d, L));
    const std::string moreText = more > 0 ? "+" + std::to_string(more) : std::string();
    const int icons = (L.local ? 1 : 0) + (L.remote ? 1 : 0) + (L.tag ? 1 : 0);
    const float padX = 8, gap = 4, iconW = 12, chipH = 22;
    const float textW = draw::TextWidth(widget, text, Font().small);
    const float moreW = more > 0 ? draw::TextWidth(widget, moreText, Font().smallBold) : 0;
    const float cw = std::min(padX + (float)icons * (iconW + gap) + textW + (more > 0 ? gap + moreW : 0) + padX, w - 16);
    if (cw < 34) return;
    const float cx = roundf(x + w - 8 - cw), cy = roundf(y + h / 2 - chipH / 2);
    const GdkRGBA lc = L.tag ? Pal().Fg(0.55f) : LaneColor(row.lane);
    if (L.head) draw::Round(s, cx, cy, cw, chipH, chipH / 2, WithAlpha(lc, 0.18f));  // checked out
    draw::RoundBorder(s, cx, cy, cw, chipH, chipH / 2, 1, WithAlpha(lc, L.tag ? 0.45f : 0.7f));
    float ix = cx + padX;
    auto icon = [&](const char* name) {
        draw::Icon(s, widget, name, ix, cy + (chipH - iconW) / 2, iconW, lc);
        ix += iconW + gap;
    };
    if (L.local) icon("mg-local-symbolic");
    if (L.remote) icon("mg-cloud-symbolic");
    if (L.tag) icon("mg-tag-symbolic");
    const float textR = cx + cw - padX - (more > 0 ? moreW + gap : 0);
    draw::Text(s, widget, text, Font().small, Pal().Fg(0.9f), ix, cy, std::max(0.0f, textR - ix) + 1, chipH);
    if (more > 0) draw::Text(s, widget, moreText, Font().smallBold, Pal().Fg(0.55f), textR + gap, cy, moreW + 2, chipH);
}

void HistoryView::DrawCommit(GtkSnapshot* s, RepoSession* ss, const GraphRow& gr, float x0, float y, float w0, float h) {
    const RepoData& d = *ss->data;
    const float x = x0 + 6, w = w0 - 12;
    const float l1 = draw::LineHeight(widget, Font().ui), l2 = draw::LineHeight(widget, Font().small);
    const float top = y + floorf((h - l1 - l2 - 2) / 2);
    const GdkRGBA dim = Pal().Fg(0.55f);
    if (gr.commit < 0) {
        draw::Text(s, widget, "Uncommitted Changes", Font().medium, Pal().fg, x, top, w, l1);
        draw::Text(s, widget, U8(WipSummary(d, ss->status.get())), Font().small, dim, x, top + l1 + 2, w, l2);
        return;
    }
    const Commit& cm = d.commits[(size_t)gr.commit];
    draw::Text(s, widget, U8(CommitSubject(d, cm)), Font().ui, Pal().fg, x, top, w, l1);
    const std::string meta = U8(CommitMeta(d, cm));  // "Ada \u00B7 5 minutes ago \u00B7 ", then the short id when it fits
    const float mw = draw::TextWidth(widget, meta, Font().small);
    if (mw + 30 < w) {
        draw::Text(s, widget, meta, Font().small, dim, x, top + l1 + 2, w, l2);
        draw::Text(s, widget, U8(cm.id.Hex(7)), Font().monoSmall, dim, x + mw, top + l1 + 2, w - mw, l2);
    } else {
        std::string trimmed = meta;  // without the " \u00B7 " that leads to the id
        while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
        if (trimmed.size() >= 2 && trimmed.compare(trimmed.size() - 2, 2, "\xC2\xB7") == 0) trimmed.resize(trimmed.size() - 2);
        while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
        draw::Text(s, widget, trimmed, Font().small, dim, x, top + l1 + 2, w, l2);
    }
}

// Lines added and removed; loaded in the background for the rows on screen (merges and stashes have none).
void HistoryView::DrawChanges(GtkSnapshot* s, RepoSession* ss, const GraphRow& gr, float x, float y, float w, float h) {
    if (gr.commit < 0) return;
    const Commit& cm = ss->data->commits[(size_t)gr.commit];
    if (cm.flags & (CF_MERGE | CF_STASH)) return;
    auto st = ss->lineStats.find(cm.id);
    if (st == ss->lineStats.end()) {
        WantStats(cm.id);
        return;
    }
    if (st->second.added < 0) return;  // on its way
    const std::string plus = "+" + std::to_string(st->second.added);
    const std::string minus = "\u2212" + std::to_string(st->second.deleted);
    const float pw = draw::TextWidth(widget, plus, Font().monoSmall), mw = draw::TextWidth(widget, minus, Font().monoSmall);
    const float right = x + w - 12;
    draw::Text(s, widget, minus, Font().monoSmall, DeletedColor(), right - mw, y, mw + 2, h);
    draw::Text(s, widget, plus, Font().monoSmall, AddedColor(), right - mw - 7 - pw, y, pw + 2, h);
}

void HistoryView::Draw(GtkSnapshot* s, float w, float h) {
    RepoSession* ss = Session();
    if (!ss) return;
    const Graph& g = *ss->graph;
    const RepoData& d = *ss->data;
    const float rh = RowH();
    const double top = ScrollY();
    const int64_t rows = (int64_t)g.rows.size();
    const int64_t first = std::max<int64_t>(0, (int64_t)floor(top / rh));
    const int64_t last = std::min<int64_t>(rows, (int64_t)ceil((top + h) / rh));
    const int sel = ss->SelectedRow();
    const Columns c = Cols(w);
    for (int64_t r = first; r < last; ++r) {
        const float y = (float)((double)r * rh - top);
        const GraphRow& gr = g.rows[(size_t)r];
        if (r == sel) draw::Round(s, 6, y + 1, w - 12, rh - 2, 8, WithAlpha(Pal().accent, focused ? 0.26f : 0.16f));
        else if (r == hoverRow) draw::Round(s, 6, y + 1, w - 12, rh - 2, 8, Pal().Fg(0.045f));
        // search: the commits that don't match fade
        const bool dim = ss->Searching() && (gr.commit < 0 || !ss->searchHits[(size_t)gr.commit]);
        if (dim) gtk_snapshot_push_opacity(s, 0.35);
        DrawChip(s, d, gr, c.refsX, y, c.refsW, rh);
        graphene_rect_t clip;
        graphene_rect_init(&clip, c.graphX, y, c.graphW, rh);
        gtk_snapshot_push_clip(s, &clip);
        DrawGraph(s, g, d, gr, c.graphX, y, rh);
        gtk_snapshot_pop(s);
        DrawCommit(s, ss, gr, c.commitX, y, c.commitW, rh);
        DrawChanges(s, ss, gr, c.changesX, y, c.changesW, rh);
        if (dim) gtk_snapshot_pop(s);
    }
}

void HistoryView::WantStats(const Oid& id) {
    if (wantStats.size() >= 64 || std::find(wantStats.begin(), wantStats.end(), id) != wantStats.end()) return;
    wantStats.push_back(id);
    if (statsQueued) return;
    statsQueued = true;
    Later([this, a = Alive()] {
        if (a.lock()) RequestStats();
    });
}

// One request for the rows drawn since the last one, plus the rows just below, so they are there when scrolling.
void HistoryView::RequestStats() {
    statsQueued = false;
    RepoSession* s = Session();
    if (!s) {
        wantStats.clear();
        return;
    }
    const float rh = RowH();
    const int64_t rows = Rows();
    const int64_t below = std::min<int64_t>(rows, (int64_t)ceil((ScrollY() + Height()) / rh));
    for (int64_t r = below; r < below + 24 && r < rows && wantStats.size() < 64; ++r) {
        const int32_t c = s->graph->rows[(size_t)r].commit;
        if (c < 0) continue;
        const Commit& cm = s->data->commits[(size_t)c];
        if (!(cm.flags & (CF_MERGE | CF_STASH)) && !s->lineStats.count(cm.id) &&
            std::find(wantStats.begin(), wantStats.end(), cm.id) == wantStats.end())
            wantStats.push_back(cm.id);
    }
    if (!wantStats.empty() && !s->statsInflight) s->RequestLineStats(std::move(wantStats));
    wantStats.clear();  // asked again when the rows are drawn after the answer
}

// ---------------------------------------------------------------- updates

void HistoryView::Reveal(bool center) {
    RepoSession* s = Session();
    const int row = s ? s->SelectedRow() : -1;
    if (row < 0 || !widget) return;
    const double rh = RowH(), y = row * rh, top = ScrollY(), h = Height();
    if (h <= 0) return;
    if (y >= top && y + rh <= top + h) return;
    const bool near = y + rh >= top - rh && y <= top + h + rh;
    if (center || !near) SetScrollY(y - (h - rh) / 2);  // a jump: the row in the middle of the view
    else if (y < top) SetScrollY(y);
    else SetScrollY(y + rh - h);
}

void HistoryView::UpdateMatches() {
    RepoSession* s = owner ? owner->session() : nullptr;
    if (!matches) return;
    gtk_label_set_text(GTK_LABEL(matches), s ? U8(SearchStatus(*s)).c_str() : "");
    SetClass(matches, "error", s && s->Searching() && s->searchCount == 0);
    const bool any = s && s->Searching() && s->searchCount > 0;
    gtk_widget_set_visible(prev, any);
    gtk_widget_set_visible(next, any);
}

// Every branch and tag of the commit, and how each relates to its remote.
std::string HistoryView::RefsTip(int row) const {
    RepoSession* s = Session();
    if (!s || row < 0 || row >= (int)s->graph->rows.size()) return {};
    const RepoData& d = *s->data;
    auto it = d.labels.find(s->graph->rows[(size_t)row].commit);
    if (it == d.labels.end() || it->second.empty()) return {};
    std::string tip;
    for (const Label& L : it->second) {
        if (!tip.empty()) tip += "\n";
        tip += U8(L.text) + "  \u2014  " + U8(RefNote(d, L));
    }
    return tip;
}

namespace history {

void Reload(HistoryView* v) {
    if (!v) return;
    RepoSession* s = v->owner->session();
    if (s && s->graph && !s->lastLoadRefresh)  // a new repository or scope: a graph column as wide as its lanes
        v->graphW = std::clamp(kLaneX0 * 2 + (float)(s->graph->maxLanes - 1) * kLaneW, 40.0f, 260.0f);
    v->hoverRow = -1;
    v->Refresh();
    if (v->header) v->header->Redraw();
    v->syncing = true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s && s->headOnly ? v->current : v->all), TRUE);
    v->syncing = false;
    SyncSelection(v);
}

void SyncSelection(HistoryView* v) {
    if (!v) return;
    v->Redraw();
    v->UpdateMatches();
}

void RevealSelection(HistoryView* v) {
    if (v) v->Reveal(true);
}

void Redraw(HistoryView* v) {
    if (!v) return;
    v->Redraw();
    v->UpdateMatches();
}

void StatusChanged(HistoryView* v, int delta) {
    if (!v) return;
    const double top = v->ScrollY();
    v->Refresh();
    // The "uncommitted changes" row came or went: the rows below keep their place on screen.
    if (delta != 0 && top > 0) v->SetScrollY(std::max(0.0, top + delta * v->RowH()));
    v->UpdateMatches();
}

void Focus(HistoryView* v) {
    if (v && v->widget) gtk_widget_grab_focus(v->widget);
}

}  // namespace history

// ---------------------------------------------------------------- interaction

static void OnPressed(GtkGestureClick* g, int n, double x, double y, gpointer data) {
    HistoryView* v = static_cast<HistoryView*>(data);
    RepoSession* s = v->Session();
    gtk_widget_grab_focus(v->widget);
    const int row = v->RowAt(y);
    if (!s || row < 0) return;
    const guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    RepoTab* owner = v->owner;
    if (button == GDK_BUTTON_SECONDARY) {
        owner->SelectRow(row);
        Menu m;
        owner->FillCommitMenu(m, s->graph->rows[(size_t)row].commit);
        m.PopupAt(v->widget, x, y);
        return;
    }
    if (button != GDK_BUTTON_PRIMARY) return;
    owner->SelectRow(row);
    if (n != 2) return;
    // Double-click a chip: check its branch out. Double-click the uncommitted changes: review them.
    const int32_t c = s->graph->rows[(size_t)row].commit;
    if (c < 0) {
        owner->ReviewChanges();
        return;
    }
    if (x > v->refsW) return;
    auto it = s->data->labels.find(c);
    if (it != s->data->labels.end() && !it->second.empty() && !it->second[0].tag) owner->CheckoutRef(it->second[0].ref);
}

static gboolean OnKey(GtkEventControllerKey*, guint key, guint, GdkModifierType state, gpointer data) {
    HistoryView* v = static_cast<HistoryView*>(data);
    RepoSession* s = v->Session();
    if (!s || s->graph->rows.empty()) return FALSE;
    const int rows = (int)s->graph->rows.size();
    const int cur = s->SelectedRow();
    const int page = std::max(1, (int)(v->Height() / v->RowH()) - 1);
    const bool ctrl = state & GDK_CONTROL_MASK;
    int to;
    switch (key) {
    case GDK_KEY_Up: case GDK_KEY_KP_Up: to = cur < 0 ? 0 : cur - 1; break;
    case GDK_KEY_Down: case GDK_KEY_KP_Down: to = cur + 1; break;
    case GDK_KEY_Page_Up: case GDK_KEY_KP_Page_Up: to = cur - page; break;
    case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down: to = cur + page; break;
    case GDK_KEY_Home: case GDK_KEY_KP_Home: to = 0; break;
    case GDK_KEY_End: case GDK_KEY_KP_End: to = rows - 1; break;
    case GDK_KEY_Return: case GDK_KEY_KP_Enter:
        if (!s->selWip) return FALSE;
        v->owner->ReviewChanges();
        return TRUE;
    case GDK_KEY_c: case GDK_KEY_C:
        if (!ctrl || s->selWip || s->selCommit < 0) return FALSE;
        v->owner->Copy(U8(s->data->commits[(size_t)s->selCommit].id.Hex()));
        return TRUE;
    case GDK_KEY_Menu: case GDK_KEY_F10: {
        if (key == GDK_KEY_F10 && !(state & GDK_SHIFT_MASK)) return FALSE;
        if (cur < 0) return TRUE;
        Menu m;
        v->owner->FillCommitMenu(m, s->graph->rows[(size_t)cur].commit);
        m.PopupAt(v->widget, v->Cols(v->Width()).commitX + 20, (float)(cur * v->RowH() - v->ScrollY()) + v->RowH() - 6);
        return TRUE;
    }
    default: return FALSE;
    }
    v->owner->SelectRow(std::clamp(to, 0, rows - 1));
    v->Reveal(false);
    return TRUE;
}

static gboolean OnTooltip(GtkWidget*, int x, int y, gboolean keyboard, GtkTooltip* tip, gpointer data) {
    HistoryView* v = static_cast<HistoryView*>(data);
    if (keyboard || x > v->refsW) return FALSE;
    const int row = v->RowAt(y);
    const std::string text = v->RefsTip(row);
    if (text.empty()) return FALSE;
    gtk_tooltip_set_text(tip, text.c_str());
    const GdkRectangle area = { 0, (int)(row * v->RowH() - v->ScrollY()), (int)v->refsW, (int)v->RowH() };
    gtk_tooltip_set_tip_area(tip, &area);
    return TRUE;
}

// ---- the column titles: dragging the line after "Branch / Tag" resizes that column

static void OnHeaderDragBegin(GtkGestureDrag*, double x, double, gpointer data) {
    HistoryHeader* h = static_cast<HistoryHeader*>(data);
    h->resizing = h->view && fabs(x - h->Boundary()) <= 6;
    if (h->resizing) h->startW = h->view->refsW;
}

static void OnHeaderDragUpdate(GtkGestureDrag*, double dx, double, gpointer data) {
    HistoryHeader* h = static_cast<HistoryHeader*>(data);
    if (!h->resizing || !h->view) return;
    const float room = h->Width() - h->view->graphW - kChangesW - kCommitMinW;
    h->view->refsW = std::clamp(h->startW + (float)dx, kRefsMinW, std::max(kRefsMinW, std::min(kRefsMaxW, room)));
    h->Redraw();
    h->view->Redraw();
}

static void OnHeaderDragEnd(GtkGestureDrag*, double, double, gpointer data) {
    HistoryHeader* h = static_cast<HistoryHeader*>(data);
    if (h->resizing && h->view) AppSettings().SetFloat(L"colRef", h->view->refsW, 0);
    h->resizing = false;
}

static void OnHeaderMotion(GtkEventControllerMotion*, double x, double, gpointer data) {
    HistoryHeader* h = static_cast<HistoryHeader*>(data);
    gtk_widget_set_cursor_from_name(h->widget, h->resizing || fabs(x - h->Boundary()) <= 6 ? "col-resize" : nullptr);
}

GtkWidget* MakeHistory(RepoTab* owner, HistoryView** out) {
    HistoryView* v = new HistoryView(owner);
    *out = v;
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    AddClass(box, "view mg-history");

    // the scope and the search results
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    AddClass(bar, "mg-scope-bar");
    GtkWidget* scope = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    AddClass(scope, "linked");
    v->all = gtk_toggle_button_new_with_label("All Branches");
    v->current = gtk_toggle_button_new_with_label("Current Branch");
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(v->current), GTK_TOGGLE_BUTTON(v->all));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->all), TRUE);
    for (GtkWidget* b : { v->all, v->current }) {
        AddClass(b, "mg-small");
        gtk_box_append(GTK_BOX(scope), b);
    }
    g_signal_connect(v->current, "toggled", G_CALLBACK(+[](GtkToggleButton* b, gpointer data) {
        HistoryView* hv = static_cast<HistoryView*>(data);
        if (!hv->syncing) hv->owner->SetHeadOnly(gtk_toggle_button_get_active(b));
    }), v);
    gtk_box_append(GTK_BOX(bar), scope);
    GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(bar), spacer);
    v->matches = MakeLabel("", "caption dim-label");
    gtk_box_append(GTK_BOX(bar), v->matches);
    v->prev = gtk_button_new_from_icon_name("go-up-symbolic");
    v->next = gtk_button_new_from_icon_name("go-down-symbolic");
    gtk_widget_set_tooltip_text(v->prev, "Previous match (Shift+Ctrl+G)");
    gtk_widget_set_tooltip_text(v->next, "Next match (Ctrl+G)");
    for (GtkWidget* b : { v->prev, v->next }) {
        AddClass(b, "flat mg-small");
        gtk_widget_set_visible(b, FALSE);
        gtk_box_append(GTK_BOX(bar), b);
    }
    g_signal_connect(v->prev, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<HistoryView*>(data)->owner->SearchStep(-1); }), v);
    g_signal_connect(v->next, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<HistoryView*>(data)->owner->SearchStep(1); }), v);
    gtk_box_append(GTK_BOX(box), bar);

    // the column titles
    HistoryHeader* header = new HistoryHeader();
    header->view = v;
    v->header = header;
    GtkWidget* hw = MakeCanvas(header, Scroll::None);
    gtk_widget_set_size_request(hw, -1, 26);
    GtkGesture* drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(OnHeaderDragBegin), header);
    g_signal_connect(drag, "drag-update", G_CALLBACK(OnHeaderDragUpdate), header);
    g_signal_connect(drag, "drag-end", G_CALLBACK(OnHeaderDragEnd), header);
    gtk_widget_add_controller(hw, GTK_EVENT_CONTROLLER(drag));
    GtkEventController* hmotion = gtk_event_controller_motion_new();
    g_signal_connect(hmotion, "motion", G_CALLBACK(OnHeaderMotion), header);
    gtk_widget_add_controller(hw, hmotion);
    gtk_box_append(GTK_BOX(box), hw);

    // the rows
    GtkWidget* sw = MakeCanvas(v, Scroll::Vertical);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_widget_set_focusable(v->widget, TRUE);
    gtk_widget_set_has_tooltip(v->widget, TRUE);
    g_signal_connect(v->widget, "query-tooltip", G_CALLBACK(OnTooltip), v);
    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(OnPressed), v);
    gtk_widget_add_controller(v->widget, GTK_EVENT_CONTROLLER(click));
    GtkEventController* key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(OnKey), v);
    gtk_widget_add_controller(v->widget, key);
    GtkEventController* motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(+[](GtkEventControllerMotion*, double, double y, gpointer data) {
        HistoryView* hv = static_cast<HistoryView*>(data);
        const int row = hv->RowAt(y);
        if (row != hv->hoverRow) {
            hv->hoverRow = row;
            hv->Redraw();
        }
    }), v);
    g_signal_connect(motion, "leave", G_CALLBACK(+[](GtkEventControllerMotion*, gpointer data) {
        HistoryView* hv = static_cast<HistoryView*>(data);
        hv->hoverRow = -1;
        hv->Redraw();
    }), v);
    gtk_widget_add_controller(v->widget, motion);
    GtkEventController* focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "enter", G_CALLBACK(+[](GtkEventControllerFocus*, gpointer data) {
        HistoryView* hv = static_cast<HistoryView*>(data);
        hv->focused = true;
        hv->Redraw();
    }), v);
    g_signal_connect(focus, "leave", G_CALLBACK(+[](GtkEventControllerFocus*, gpointer data) {
        HistoryView* hv = static_cast<HistoryView*>(data);
        hv->focused = false;
        hv->Redraw();
    }), v);
    gtk_widget_add_controller(v->widget, focus);
    gtk_box_append(GTK_BOX(box), sw);
    return box;
}
