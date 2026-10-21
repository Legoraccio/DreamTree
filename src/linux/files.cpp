// A list of changed files: the files of the selected commit, or the unstaged and staged files under their headers,
// as a list or as a tree of folders. Each file shows its status letter, its folder and name and the lines it
// changed; uncommitted ones have a Stage or Unstage button, and a discard button under the pointer.

#include "LinuxUI.h"

#include <set>

namespace {

struct Item {
    int section = 0;  // 0 a commit's files, 1 unstaged, 2 staged
    int file = -1;    // index in the section's files; -1 for folders and headers
    bool header = false;
    int count = 0;     // headers and folders: the files below
    std::string name;  // file name, folder (tree) or section title
    std::string dir;   // list: the file's folder, shown dimmed
    std::wstring path; // in the repository; folders: their path
    std::vector<Item> children;
};

void AddNodes(std::vector<Item>& out, const TreeNode& n, int section, const std::vector<FileChange>& files, const std::wstring& prefix) {
    for (const TreeNode& c : n.children) {
        Item f;
        f.section = section;
        f.name = U8(c.name);
        if (c.IsFolder()) {
            f.count = c.count;
            f.path = prefix.empty() ? c.name : prefix + L"/" + c.name;
            AddNodes(f.children, c, section, files, f.path);
        } else {
            f.file = c.index;
            f.path = files[(size_t)c.index].path;
        }
        out.push_back(std::move(f));
    }
}

std::vector<Item> Items(const std::vector<FileChange>& files, int section, bool tree) {
    std::vector<Item> out;
    if (tree) {
        AddNodes(out, FileTree(files), section, files, L"");
        return out;
    }
    for (size_t i = 0; i < files.size(); ++i) {
        const std::wstring& p = files[i].path;
        const size_t slash = p.find_last_of(L'/');
        Item f;
        f.section = section;
        f.file = (int)i;
        f.name = U8(slash == std::wstring::npos ? p : p.substr(slash + 1));
        f.dir = slash == std::wstring::npos ? "" : U8(p.substr(0, slash + 1));
        f.path = p;
        out.push_back(std::move(f));
    }
    return out;
}

struct Row {
    const Item* item;
    int depth;
};

// Where the parts of a row are.
struct Parts {
    graphene_rect_t action{}, discard{};
    bool hasAction = false, hasDiscard = false;
    float right = 0;  // the text ends before it
};

graphene_rect_t Box(float x, float y, float w, float h) {
    graphene_rect_t r;
    graphene_rect_init(&r, x, y, w, h);
    return r;
}

}  // namespace

class FileList : public Canvas {
public:
    FileList(RepoTab* o, bool w) : owner(o), wip(w) {}
    ~FileList() override;
    RepoTab* owner;
    bool wip;
    bool tree = false;
    std::vector<Item> roots;
    std::vector<Row> rows;
    std::set<std::string> collapsed;  // headers and folders closed by the user
    std::weak_ptr<CommitDetails> shownDetails;
    std::weak_ptr<WorkStatus> shownStatus;
    bool shownTree = false;
    bool hasContent = false;
    int hoverRow = -1;
    int hoverPart = 0;  // 1 the action button, 2 the discard button
    int pressed = -1;
    bool focused = false;

    float RowH() const { return std::max(30.0f, roundf(draw::LineHeight(widget, Font().ui) + 12)); }
    double ContentHeight() override { return rows.empty() ? 0 : rows.size() * RowH() + 6; }
    void Draw(GtkSnapshot* s, float w, float h) override;
    void Scrolled() override { hoverRow = -1; }

    std::string KeyOf(const Item& it) const { return std::to_string(it.section) + (it.header ? ":#:" : "::") + U8(it.path); }
    void Flatten() {
        rows.clear();
        std::function<void(const std::vector<Item>&, int)> add = [&](const std::vector<Item>& items, int depth) {
            for (const Item& it : items) {
                rows.push_back({ &it, depth });
                if (!it.children.empty() && !collapsed.count(KeyOf(it))) add(it.children, depth + 1);
            }
        };
        add(roots, 0);
        hoverRow = -1;
    }
    int RowAt(double y) const {
        const int64_t r = (int64_t)floor((y + ScrollY() - 3) / RowH());
        return r >= 0 && r < (int64_t)rows.size() ? (int)r : -1;
    }
    const FileChange* ChangeOf(const Item& it) const {
        RepoSession* s = owner->session();
        if (!s || it.file < 0) return nullptr;
        const std::vector<FileChange>* files = nullptr;
        if (it.section == 0) files = s->details ? &s->details->files : nullptr;
        else if (s->status) files = it.section == 1 ? &s->status->unstaged : &s->status->staged;
        return files && it.file < (int)files->size() ? &(*files)[(size_t)it.file] : nullptr;
    }
    bool Shown(const Item& it) const {  // the file of the open diff
        RepoSession* s = owner->session();
        return s && it.file >= 0 && s->diff.open && s->diff.req.section == it.section && s->diff.req.path == it.path;
    }
    bool CanDiscard(const Item& it) const {
        const FileChange* f = ChangeOf(it);
        return it.section == 1 && f && f->status != L'U';
    }
    std::string ActionLabel(const Item& it) const {
        if (it.header) return it.section == 1 ? "Stage All" : "Unstage All";
        return it.section == 1 ? "Stage" : "Unstage";
    }
    Parts PartsOf(int row, float w) const;
    // The paths a row stands for: a file (and the old name of a rename), a folder, or everything (headers).
    std::vector<std::wstring> PathsOf(const Item& it) const {
        std::vector<std::wstring> paths;
        if (it.header) return paths;
        paths.push_back(it.path);
        if (const FileChange* f = ChangeOf(it); f && !f->oldPath.empty()) paths.push_back(f->oldPath);
        return paths;
    }
    void Rebuild(bool force);
    void Open(int row);
    void Step(int dir);
    void ContextMenu(int row, double x, double y);
};

FileList::~FileList() {}

Parts FileList::PartsOf(int row, float w) const {
    Parts p;
    const float rh = RowH();
    const float y = (float)(3 + row * rh - ScrollY());
    p.right = w - 8;
    const Item& it = *rows[(size_t)row].item;
    if (wip && (!it.header || it.count > 0)) {
        const float bw = draw::ButtonWidth(widget, ActionLabel(it));
        p.action = Box(p.right - bw, y + 4, bw, rh - 8);
        p.hasAction = true;
        p.right -= bw + 6;
    }
    if (row == hoverRow && CanDiscard(it)) {
        p.discard = Box(p.right - 26, y + 3, 26, rh - 6);
        p.hasDiscard = true;
        p.right -= 30;
    }
    return p;
}

// ---------------------------------------------------------------- drawing

void FileList::Draw(GtkSnapshot* s, float w, float h) {
    if (rows.empty()) return;
    const float rh = RowH();
    const double top = ScrollY() - 3;
    const int64_t first = std::max<int64_t>(0, (int64_t)floor(top / rh));
    const int64_t last = std::min<int64_t>((int64_t)rows.size(), (int64_t)ceil((top + h) / rh));
    const GdkRGBA dim = Pal().Fg(0.55f);
    for (int64_t r = first; r < last; ++r) {
        const Row& row = rows[(size_t)r];
        const Item& it = *row.item;
        const float y = (float)(r * rh - top);
        const bool hover = r == hoverRow;
        if (Shown(it)) draw::Round(s, 4, y + 1, w - 8, rh - 2, 6, WithAlpha(Pal().accent, focused ? 0.26f : 0.18f));
        else if (hover) draw::Round(s, 4, y + 1, w - 8, rh - 2, 6, Pal().Fg(0.05f));
        const Parts p = PartsOf((int)r, w);
        if (p.hasAction) draw::Button(s, widget, p.action, ActionLabel(it), hover && hoverPart == 1, !(owner->session() && owner->session()->opBusy));
        if (p.hasDiscard) {
            const graphene_rect_t& d = p.discard;
            if (hover && hoverPart == 2) draw::Round(s, d.origin.x, d.origin.y, d.size.width, d.size.height, 6, Pal().Fg(0.1f));
            draw::Icon(s, widget, "user-trash-symbolic", d.origin.x + (d.size.width - 16) / 2, d.origin.y + (d.size.height - 16) / 2, 16, dim);
        }
        float x = 10 + (float)row.depth * 16;
        if (!it.children.empty() || it.header) {
            draw::Icon(s, widget, collapsed.count(KeyOf(it)) ? "pan-end-symbolic" : "pan-down-symbolic", x, y + (rh - 16) / 2, 16, dim);
        }
        if (wip || tree) x += 20;  // room for the arrows of the headers and folders
        float right = p.right;
        if (it.header) {
            const float tw = draw::Text(s, widget, it.name, Font().medium, Pal().fg, x, y, right - x, rh);
            draw::Text(s, widget, std::to_string(it.count), Font().small, dim, x + tw + 8, y, std::max(0.0f, right - x - tw - 8), rh);
            continue;
        }
        if (it.file < 0) {  // a folder of the tree
            draw::Icon(s, widget, "folder-symbolic", x, y + (rh - 16) / 2, 16, dim);
            draw::Text(s, widget, it.name, Font().ui, Pal().Fg(0.7f), x + 24, y, right - x - 24, rh);
            continue;
        }
        const FileChange* f = ChangeOf(it);
        const wchar_t status = f ? f->status : L'M';
        const std::string letter = U8(std::wstring(1, StatusLetter(status)));
        draw::Text(s, widget, letter, Font().monoSmallBold, StatusColor(status), x + 2, y, 14, rh);
        x += 22;
        if (f) {
            const std::string delta = U8(LineDelta(f->added, f->deleted));
            if (!delta.empty()) {
                const float dw = draw::TextWidth(widget, delta, Font().monoSmall);
                if (right - x > dw + 60) {
                    draw::Text(s, widget, delta, Font().monoSmall, dim, right - dw, y, dw + 2, rh);
                    right -= dw + 10;
                }
            }
        }
        const float room = std::max(0.0f, right - x);
        const float nameW = std::min(draw::TextWidth(widget, it.name, Font().ui), room);
        const float dirW = it.dir.empty() ? 0 : std::min(draw::TextWidth(widget, it.dir, Font().ui), room - nameW);
        if (dirW > 24) {
            draw::Text(s, widget, it.dir, Font().ui, dim, x, y, dirW, rh, PANGO_ELLIPSIZE_START);
            draw::Text(s, widget, it.name, Font().ui, Pal().fg, x + dirW, y, room - dirW, rh);
        } else {
            draw::Text(s, widget, it.name, Font().ui, Pal().fg, x, y, room, rh);
        }
    }
}

// ---------------------------------------------------------------- contents

void FileList::Rebuild(bool force) {
    RepoSession* s = owner->session();
    if (!wip) {
        const bool commit = s && s->data && !s->selWip && s->selCommit >= 0 && s->selCommit < (int32_t)s->data->commits.size();
        const std::shared_ptr<CommitDetails> det = commit ? s->details : nullptr;
        const bool ok = det && det->ok && det->id == s->data->commits[(size_t)s->selCommit].id;
        if (!ok) {
            if (!roots.empty() || hasContent) {
                roots.clear();
                shownDetails.reset();
                hasContent = false;
                Flatten();
                Refresh();
            }
            return;
        }
        if (!force && shownDetails.lock() == det && shownTree == tree) return;
        const bool another = shownDetails.lock() != det;
        shownDetails = det;
        shownTree = tree;
        roots = Items(det->files, 0, tree);
        hasContent = true;
        Flatten();
        Refresh();
        if (another) SetScrollY(0);
        return;
    }
    const std::shared_ptr<WorkStatus> st = s ? s->status : nullptr;
    if (!st) {
        roots.clear();
        shownStatus.reset();
        hasContent = false;
        Flatten();
        Refresh();
        return;
    }
    if (!force && shownStatus.lock() == st && shownTree == tree && hasContent) return;
    shownStatus = st;
    shownTree = tree;
    roots.clear();
    for (int section = 1; section <= 2; ++section) {
        const std::vector<FileChange>& files = section == 1 ? st->unstaged : st->staged;
        Item h;
        h.header = true;
        h.section = section;
        h.count = (int)files.size();
        h.name = section == 1 ? "Unstaged" : "Staged";
        h.children = Items(files, section, tree);
        roots.push_back(std::move(h));
    }
    hasContent = true;
    Flatten();
    Refresh();
}

void FileList::Open(int row) {
    if (row < 0 || row >= (int)rows.size()) return;
    const Item& it = *rows[(size_t)row].item;
    if (it.file < 0) return;
    owner->OpenDiff(it.section, it.path);
    Redraw();
}

// The arrows: the file above or below the one shown.
void FileList::Step(int dir) {
    int cur = -1;
    for (size_t i = 0; i < rows.size(); ++i)
        if (Shown(*rows[i].item)) cur = (int)i;
    if (cur < 0) cur = dir > 0 ? -1 : (int)rows.size();
    for (int i = cur + dir; i >= 0 && i < (int)rows.size(); i += dir) {
        if (rows[(size_t)i].item->file < 0) continue;
        Open(i);
        const double rh = RowH(), y = 3 + i * rh;
        if (y < ScrollY()) SetScrollY(y - 3);
        else if (y + rh > ScrollY() + Height()) SetScrollY(y + rh - Height() + 3);
        return;
    }
}

void FileList::ContextMenu(int row, double x, double y) {
    if (row < 0 || row >= (int)rows.size()) return;
    const Item it = *rows[(size_t)row].item;  // a copy: the list may change before an item is picked
    Menu m;
    RepoTab* o = owner;
    if (it.section > 0) {
        const bool stage = it.section == 1;
        const std::vector<std::wstring> paths = PathsOf(it);
        m.Add(it.header ? (stage ? "Stage All" : "Unstage All") : (stage ? "Stage" : "Unstage"), [o, paths, stage] { o->StagePaths(paths, stage); });
        if (stage && CanDiscard(it)) {
            const FileChange f = *ChangeOf(it);
            m.Add("Discard Changes\u2026", [o, f] { o->DiscardFile(f); });
        }
    }
    if (!it.header) {
        m.Section();
        const std::wstring path = it.path;
        m.Add("Copy Path", [o, path] { o->Copy(U8(path)); });
        m.Add("Show in Files", [o, path] { o->RevealInFiles(path); });
    }
    m.PopupAt(widget, x, y);
}

// ---------------------------------------------------------------- interaction

static void UpdateHover(FileList* f, double x, double y) {
    const int row = f->RowAt(y);
    int part = 0;
    if (row >= 0) {
        const int before = f->hoverRow;
        f->hoverRow = row;  // the discard button shows for the row under the pointer
        const Parts p = f->PartsOf(row, f->Width());
        if (p.hasAction && Inside(p.action, x, y)) part = 1;
        else if (p.hasDiscard && Inside(p.discard, x, y)) part = 2;
        f->hoverRow = before;
    }
    if (row != f->hoverRow || part != f->hoverPart) {
        f->hoverRow = row;
        f->hoverPart = part;
        f->Redraw();
    }
}

static void OnPressed(GtkGestureClick* g, int n, double x, double y, gpointer data) {
    FileList* f = static_cast<FileList*>(data);
    UpdateHover(f, x, y);
    const int row = f->RowAt(y);
    if (row < 0) return;
    const guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    if (button == GDK_BUTTON_SECONDARY) {
        f->ContextMenu(row, x, y);
        return;
    }
    if (button != GDK_BUTTON_PRIMARY) return;
    gtk_widget_grab_focus(f->widget);
    const Item& it = *f->rows[(size_t)row].item;
    const Parts p = f->PartsOf(row, f->Width());
    RepoSession* s = f->owner->session();
    if (p.hasAction && Inside(p.action, x, y)) {
        if (s && !s->opBusy) f->owner->StagePaths(f->PathsOf(it), it.section == 1);
        return;
    }
    if (p.hasDiscard && Inside(p.discard, x, y)) {
        if (const FileChange* c = f->ChangeOf(it)) f->owner->DiscardFile(*c);
        return;
    }
    if (it.file < 0) {  // a header or a folder opens or closes
        const std::string key = f->KeyOf(it);
        if (!f->collapsed.erase(key)) f->collapsed.insert(key);
        f->Flatten();
        f->Refresh();
        return;
    }
    (void)n;
    f->Open(row);
}

static gboolean OnKey(GtkEventControllerKey*, guint key, guint, GdkModifierType state, gpointer data) {
    FileList* f = static_cast<FileList*>(data);
    switch (key) {
    case GDK_KEY_Up: case GDK_KEY_KP_Up: f->Step(-1); return TRUE;
    case GDK_KEY_Down: case GDK_KEY_KP_Down: f->Step(1); return TRUE;
    case GDK_KEY_Menu: case GDK_KEY_F10: {
        if (key == GDK_KEY_F10 && !(state & GDK_SHIFT_MASK)) return FALSE;
        for (size_t i = 0; i < f->rows.size(); ++i)
            if (f->Shown(*f->rows[i].item)) f->ContextMenu((int)i, 40, 3 + (i + 1) * f->RowH() - f->ScrollY());
        return TRUE;
    }
    default: return FALSE;
    }
}

static gboolean OnTooltip(GtkWidget*, int x, int y, gboolean keyboard, GtkTooltip* tip, gpointer data) {
    FileList* f = static_cast<FileList*>(data);
    const int row = keyboard ? -1 : f->RowAt(y);
    if (row < 0) return FALSE;
    const Item& it = *f->rows[(size_t)row].item;
    const Parts p = f->PartsOf(row, f->Width());
    std::string text;
    if (p.hasDiscard && Inside(p.discard, x, y)) text = "Discard changes\u2026";
    else if (it.file >= 0) {
        const FileChange* c = f->ChangeOf(it);
        text = c && !c->oldPath.empty() ? U8(c->oldPath) + " \u2192 " + U8(c->path) : U8(it.path);
    }
    if (text.empty()) return FALSE;
    gtk_tooltip_set_text(tip, text.c_str());
    return TRUE;
}

GtkWidget* MakeFileList(RepoTab* owner, bool wip, FileList** out) {
    FileList* f = new FileList(owner, wip);
    *out = f;
    GtkWidget* sw = MakeCanvas(f, Scroll::Vertical);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_widget_set_focusable(f->widget, TRUE);
    gtk_widget_set_has_tooltip(f->widget, TRUE);
    g_signal_connect(f->widget, "query-tooltip", G_CALLBACK(OnTooltip), f);
    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(OnPressed), f);
    gtk_widget_add_controller(f->widget, GTK_EVENT_CONTROLLER(click));
    GtkEventController* key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(OnKey), f);
    gtk_widget_add_controller(f->widget, key);
    GtkEventController* motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(+[](GtkEventControllerMotion*, double x, double y, gpointer data) {
        UpdateHover(static_cast<FileList*>(data), x, y);
    }), f);
    g_signal_connect(motion, "leave", G_CALLBACK(+[](GtkEventControllerMotion*, gpointer data) {
        FileList* fl = static_cast<FileList*>(data);
        fl->hoverRow = -1;
        fl->hoverPart = 0;
        fl->Redraw();
    }), f);
    gtk_widget_add_controller(f->widget, motion);
    GtkEventController* focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "enter", G_CALLBACK(+[](GtkEventControllerFocus*, gpointer data) {
        static_cast<FileList*>(data)->focused = true;
        static_cast<FileList*>(data)->Redraw();
    }), f);
    g_signal_connect(focus, "leave", G_CALLBACK(+[](GtkEventControllerFocus*, gpointer data) {
        static_cast<FileList*>(data)->focused = false;
        static_cast<FileList*>(data)->Redraw();
    }), f);
    gtk_widget_add_controller(f->widget, focus);
    return sw;
}

namespace files {

void Reload(FileList* f, bool force) {
    if (f) f->Rebuild(force);
}

void SetTree(FileList* f, bool tree) {
    if (!f || f->tree == tree) return;
    f->tree = tree;
    f->Rebuild(true);
}

void SyncSelection(FileList* f) {
    if (f) f->Redraw();
}

bool HasRows(FileList* f) { return f && !f->rows.empty(); }

}  // namespace files
