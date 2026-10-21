// The sidebar: the two pages (uncommitted changes, history), then the branches, the remotes, the tags, the
// stashes and the submodules, as a tree that opens and closes. Picking an item selects its commit in the history.

#include "LinuxUI.h"

#include <set>

namespace {

enum class Kind : uint8_t { Group, Page, Folder, Remote, Branch, Tag, Stash, Submodule };

struct Item {
    Kind kind = Kind::Page;
    std::string title;
    std::string key;  // the same across reloads: expansion and selection follow it
    int index = -1;   // the page, ref, stash or submodule
    int count = 0;    // folders: the branches below
    std::vector<Item> children;
};

Item MakeItem(Kind kind, std::string title, std::string key, int index) {
    Item it;
    it.kind = kind;
    it.title = std::move(title);
    it.key = std::move(key);
    it.index = index;
    return it;
}

// Folders and leaves of a tree from navtree.h, under parent.
void AddTree(Item& parent, const TreeNode& n, Kind leaf) {
    for (const TreeNode& c : n.children) {
        const std::string title = U8(c.name);
        const std::string key = parent.key + "/" + title;
        if (c.IsFolder()) {
            Item folder = MakeItem(Kind::Folder, title, key, -1);
            folder.count = c.count;
            AddTree(folder, c, leaf);
            parent.children.push_back(std::move(folder));
        } else {
            parent.children.push_back(MakeItem(leaf, title, key, c.index));
        }
    }
}

std::string AheadBehind(const Ref& r) {
    std::string s;
    auto add = [&](const std::string& part) {
        if (!s.empty()) s += " ";
        s += part;
    };
    if (r.ahead) add("\u2191" + std::to_string(r.ahead));
    if (r.behind) add("\u2193" + std::to_string(r.behind));
    if (r.gone) add("gone");
    return s;
}

struct Row {
    const Item* item;
    int depth;
    float y, h;
};

}  // namespace

class SidebarView : public Canvas {
public:
    explicit SidebarView(RepoTab* o) : owner(o) {}
    ~SidebarView() override {
        if (owner && owner->side == this) owner->side = nullptr;
    }
    RepoTab* owner;
    std::vector<Item> roots;
    std::vector<Row> rows;
    std::set<std::string> collapsedGroups{ "remotes", "tags", "stashes", "submodules" };
    std::set<std::string> expandedFolders;
    std::string selected;  // the key of the selected item
    int hoverRow = -1;
    bool loadedOnce = false;
    bool focused = false;
    float height = 0;

    float RowH() const { return std::max(32.0f, roundf(draw::LineHeight(widget, Font().ui) + 14)); }
    double ContentHeight() override { return height; }
    void Draw(GtkSnapshot* s, float w, float h) override;
    void Scrolled() override { hoverRow = -1; }

    void Rebuild();
    void Flatten();
    void RevealHead();
    void Sync();
    void Activate(const Item& it);
    void Step(int dir);
    int RowAt(double y) const {
        const double cy = y + ScrollY();
        for (size_t i = 0; i < rows.size(); ++i)
            if (cy >= rows[i].y && cy < rows[i].y + rows[i].h) return (int)i;
        return -1;
    }
    const Item* Find(const std::string& key, const std::vector<Item>& items) const {
        for (const Item& it : items) {
            if (it.key == key) return &it;
            if (const Item* f = Find(key, it.children)) return f;
        }
        return nullptr;
    }
    int32_t CommitOf(const Item& it) const {
        RepoSession* s = owner->session();
        if (!s || !s->data) return -1;
        const RepoData& d = *s->data;
        if ((it.kind == Kind::Branch || it.kind == Kind::Tag) && it.index >= 0 && it.index < (int)d.refs.size()) return d.refs[(size_t)it.index].commit;
        if (it.kind == Kind::Stash && it.index >= 0 && it.index < (int)d.stashes.size()) return d.stashes[(size_t)it.index].commit;
        return -1;
    }
    bool Selectable(const Item& it) const {
        RepoSession* s = owner->session();
        switch (it.kind) {
        case Kind::Page: return s && s->data && (it.index == 1 || (s->graph && s->graph->hasWip));
        case Kind::Branch:
        case Kind::Tag:
        case Kind::Stash: return CommitOf(it) >= 0;
        default: return false;
        }
    }
};

// ---------------------------------------------------------------- items

void SidebarView::Rebuild() {
    RepoSession* s = owner->session();
    roots.clear();
    roots.push_back(MakeItem(Kind::Page, "Changes", "page:changes", 0));
    roots.push_back(MakeItem(Kind::Page, "History", "page:history", 1));
    if (s && s->data) {
        const RepoData& d = *s->data;
        Item branches = MakeItem(Kind::Group, "Branches", "branches", -1);
        AddTree(branches, LocalBranchTree(d), Kind::Branch);
        roots.push_back(std::move(branches));
        if (!d.remotes.empty()) {
            Item remotes = MakeItem(Kind::Group, "Remotes", "remotes", -1);
            const TreeNode tree = RemoteTree(d);
            for (const TreeNode& r : tree.children) {
                Item remote = MakeItem(Kind::Remote, U8(r.name), "remotes/" + U8(r.name), -1);
                AddTree(remote, r, Kind::Branch);
                remotes.children.push_back(std::move(remote));
            }
            roots.push_back(std::move(remotes));
        }
        const TreeNode tags = TagTree(d);
        if (!tags.children.empty()) {
            Item group = MakeItem(Kind::Group, "Tags", "tags", -1);
            AddTree(group, tags, Kind::Tag);
            roots.push_back(std::move(group));
        }
        if (!d.stashes.empty()) {
            Item group = MakeItem(Kind::Group, "Stashes", "stashes", -1);
            for (size_t i = 0; i < d.stashes.size(); ++i) {
                const std::string name = U8(d.stashes[i].name), message = U8(d.stashes[i].message);
                group.children.push_back(MakeItem(Kind::Stash, message.empty() ? name : message, "stash/" + name, (int)i));
            }
            roots.push_back(std::move(group));
        }
        if (!d.submodules.empty()) {
            Item group = MakeItem(Kind::Group, "Submodules", "submodules", -1);
            for (size_t i = 0; i < d.submodules.size(); ++i) {
                const std::string name = U8(d.submodules[i]);
                group.children.push_back(MakeItem(Kind::Submodule, name, "submodule/" + name, (int)i));
            }
            roots.push_back(std::move(group));
        }
    }
    if (s && s->data && !loadedOnce) RevealHead();
    loadedOnce = s && s->data;
    Flatten();
}

void SidebarView::Flatten() {
    rows.clear();
    hoverRow = -1;
    const float rh = RowH();
    float y = 6;
    std::function<void(const std::vector<Item>&, int)> add = [&](const std::vector<Item>& items, int depth) {
        for (const Item& it : items) {
            if (it.kind == Kind::Group) {
                y += 10;
                rows.push_back({ &it, 0, y, rh - 4 });
                y += rh - 4;
                if (!collapsedGroups.count(it.key)) add(it.children, 0);
                continue;
            }
            rows.push_back({ &it, depth, y, rh });
            y += rh;
            if ((it.kind == Kind::Folder || it.kind == Kind::Remote) && expandedFolders.count(it.key)) add(it.children, depth + 1);
        }
    };
    add(roots, 0);
    height = y + 10;
}

// The checked-out branch is visible after the first load, even inside folders.
void SidebarView::RevealHead() {
    RepoSession* s = owner->session();
    std::vector<std::string> path;
    std::function<bool(const Item&)> find = [&](const Item& item) {
        for (const Item& c : item.children) {
            if (c.kind == Kind::Branch && s->data->refs[(size_t)c.index].head) return true;
            if (c.kind == Kind::Folder) {
                path.push_back(c.key);
                if (find(c)) return true;
                path.pop_back();
            }
        }
        return false;
    };
    for (const Item& group : roots)
        if (group.key == "branches" && find(group))
            for (const std::string& k : path) expandedFolders.insert(k);
}

// The pages follow the history: Changes while the uncommitted changes are selected, else the picked branch, tag
// or stash while its commit is the selected one, else History.
void SidebarView::Sync() {
    RepoSession* s = owner->session();
    std::string want;
    if (s && s->data) {
        const Item* cur = Find(selected, roots);
        if (s->selWip) want = "page:changes";
        else if (cur && cur->kind != Kind::Page && s->selCommit >= 0 && CommitOf(*cur) == s->selCommit) want = selected;
        else want = "page:history";
    }
    if (want == selected) return;
    selected = want;
    Redraw();
}

void SidebarView::Activate(const Item& it) {
    if (!Selectable(it)) return;
    selected = it.key;
    Redraw();
    if (it.kind == Kind::Page) {
        if (it.index == 0) owner->SelectWorkingTree();
        else owner->ShowHistoryPage();
        return;
    }
    const int32_t c = CommitOf(it);
    if (c >= 0) owner->SelectCommit(c);
}

// The arrows: the selectable item above or below the selected one.
void SidebarView::Step(int dir) {
    int cur = -1;
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].item->key == selected) cur = (int)i;
    for (int i = cur + dir; i >= 0 && i < (int)rows.size(); i += dir) {
        if (!Selectable(*rows[(size_t)i].item)) continue;
        Activate(*rows[(size_t)i].item);
        const Row& r = rows[(size_t)i];
        if (r.y < ScrollY()) SetScrollY(r.y - 6);
        else if (r.y + r.h > ScrollY() + Height()) SetScrollY(r.y + r.h - Height() + 6);
        return;
    }
}

// ---------------------------------------------------------------- drawing

void SidebarView::Draw(GtkSnapshot* s, float w, float h) {
    RepoSession* ss = owner->session();
    const RepoData* d = ss ? ss->data.get() : nullptr;
    const double top = ScrollY();
    const GdkRGBA dim = Pal().Fg(0.55f);
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        const float y = (float)(r.y - top);
        if (y + r.h < 0) continue;
        if (y > h) break;
        const Item& it = *r.item;
        const bool hover = (int)i == hoverRow;
        if (it.kind == Kind::Group) {
            draw::Text(s, widget, it.title, Font().smallBold, dim, 14, y, w - 48, r.h);
            if (!it.children.empty())
                draw::Icon(s, widget, collapsedGroups.count(it.key) ? "pan-end-symbolic" : "pan-down-symbolic", w - 30, y + (r.h - 16) / 2, 16,
                           Pal().Fg(hover ? 0.8f : 0.4f));
            continue;
        }
        const bool sel = it.key == selected;
        const bool folder = it.kind == Kind::Folder || it.kind == Kind::Remote;
        if (sel) draw::Round(s, 6, y + 1, w - 12, r.h - 2, 6, focused ? WithAlpha(Pal().accent, 0.24f) : Pal().Fg(hover ? 0.13f : 0.1f));
        else if (hover && (folder || Selectable(it))) draw::Round(s, 6, y + 1, w - 12, r.h - 2, 6, Pal().Fg(0.05f));

        // icon, name, and a number or note at the right
        const char* icon = "mg-branch-symbolic";
        std::string note;
        const PangoFontDescription* font = Font().ui;
        GdkRGBA color = Pal().fg, iconColor = Pal().Fg(0.8f);
        switch (it.kind) {
        case Kind::Page:
            icon = it.index == 0 ? "document-edit-symbolic" : "document-open-recent-symbolic";
            if (it.index == 0 && ss && ss->status && ss->status->Dirty()) note = std::to_string(ss->status->ChangedFiles());
            if (it.index == 0 && !(ss && ss->graph && ss->graph->hasWip)) color = iconColor = dim;
            break;
        case Kind::Folder:
            icon = "folder-symbolic";
            note = std::to_string(it.count);
            break;
        case Kind::Remote: icon = "mg-cloud-symbolic"; break;
        case Kind::Branch:
            if (d && it.index >= 0 && it.index < (int)d->refs.size()) {
                const Ref& ref = d->refs[(size_t)it.index];
                if (ref.head) {
                    icon = "object-select-symbolic";
                    iconColor = Pal().accentText;
                    font = Font().medium;
                }
                note = AheadBehind(ref);
                if (ref.commit < 0) color = iconColor = dim;
            }
            break;
        case Kind::Tag: icon = "mg-tag-symbolic"; break;
        case Kind::Stash:
            icon = "mg-stash-symbolic";
            if (d && it.index >= 0 && it.index < (int)d->stashes.size()) note = U8(d->stashes[(size_t)it.index].name);
            break;
        case Kind::Submodule: icon = "package-x-generic-symbolic"; break;
        case Kind::Group: break;
        }
        float x = it.kind == Kind::Page ? 16 : 12 + (float)r.depth * 16;
        if (it.kind != Kind::Page) {
            if (folder)
                draw::Icon(s, widget, expandedFolders.count(it.key) ? "pan-down-symbolic" : "pan-end-symbolic", x, y + (r.h - 16) / 2, 16, dim);
            x += 18;
        }
        draw::Icon(s, widget, icon, x, y + (r.h - 16) / 2, 16, iconColor);
        x += 26;
        float right = w - 16;
        if (!note.empty()) {
            const float nw = draw::TextWidth(widget, note, Font().small);
            draw::Text(s, widget, note, Font().small, dim, right - nw, y, nw + 2, r.h);
            right -= nw + 8;
        }
        draw::Text(s, widget, it.title, font, color, x, y, right - x, r.h);
    }
}

// ---------------------------------------------------------------- interaction

static void OnPressed(GtkGestureClick* g, int n, double x, double y, gpointer data) {
    SidebarView* v = static_cast<SidebarView*>(data);
    const int row = v->RowAt(y);
    if (row < 0) return;
    const Item& it = *v->rows[(size_t)row].item;
    const guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    if (button == GDK_BUTTON_SECONDARY) {
        Menu m;
        if (it.kind == Kind::Branch || it.kind == Kind::Tag) v->owner->FillRefMenu(m, it.index);
        else if (it.kind == Kind::Stash) v->owner->FillStashMenu(m, it.index);
        m.PopupAt(v->widget, x, y);
        return;
    }
    if (button != GDK_BUTTON_PRIMARY) return;
    gtk_widget_grab_focus(v->widget);
    switch (it.kind) {
    case Kind::Group:
        if (!v->collapsedGroups.erase(it.key)) v->collapsedGroups.insert(it.key);
        v->Flatten();
        v->Refresh();
        return;
    case Kind::Folder:
    case Kind::Remote:
        if (!v->expandedFolders.erase(it.key)) v->expandedFolders.insert(it.key);
        v->Flatten();
        v->Refresh();
        return;
    case Kind::Branch:
    case Kind::Tag:
        if (n == 2) {  // double-click: check it out
            v->owner->CheckoutRef(it.index);
            return;
        }
        v->Activate(it);
        return;
    default: v->Activate(it); return;
    }
}

static gboolean OnKey(GtkEventControllerKey*, guint key, guint, GdkModifierType, gpointer data) {
    SidebarView* v = static_cast<SidebarView*>(data);
    switch (key) {
    case GDK_KEY_Up: case GDK_KEY_KP_Up: v->Step(-1); return TRUE;
    case GDK_KEY_Down: case GDK_KEY_KP_Down: v->Step(1); return TRUE;
    default: return FALSE;
    }
}

static gboolean OnTooltip(GtkWidget*, int, int y, gboolean keyboard, GtkTooltip* tip, gpointer data) {
    SidebarView* v = static_cast<SidebarView*>(data);
    const int row = keyboard ? -1 : v->RowAt(y);
    if (row < 0) return FALSE;
    const Row& r = v->rows[(size_t)row];
    const Kind k = r.item->kind;
    if (k != Kind::Branch && k != Kind::Tag && k != Kind::Stash && k != Kind::Submodule) return FALSE;
    gtk_tooltip_set_text(tip, r.item->title.c_str());
    const GdkRectangle area = { 0, (int)(r.y - v->ScrollY()), (int)v->Width(), (int)r.h };
    gtk_tooltip_set_tip_area(tip, &area);
    return TRUE;
}

GtkWidget* MakeSidebar(RepoTab* owner, SidebarView** out) {
    SidebarView* v = new SidebarView(owner);
    *out = v;
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
        SidebarView* sv = static_cast<SidebarView*>(data);
        const int row = sv->RowAt(y);
        if (row != sv->hoverRow) {
            sv->hoverRow = row;
            sv->Redraw();
        }
    }), v);
    g_signal_connect(motion, "leave", G_CALLBACK(+[](GtkEventControllerMotion*, gpointer data) {
        SidebarView* sv = static_cast<SidebarView*>(data);
        sv->hoverRow = -1;
        sv->Redraw();
    }), v);
    gtk_widget_add_controller(v->widget, motion);
    GtkEventController* focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "enter", G_CALLBACK(+[](GtkEventControllerFocus*, gpointer data) {
        static_cast<SidebarView*>(data)->focused = true;
        static_cast<SidebarView*>(data)->Redraw();
    }), v);
    g_signal_connect(focus, "leave", G_CALLBACK(+[](GtkEventControllerFocus*, gpointer data) {
        static_cast<SidebarView*>(data)->focused = false;
        static_cast<SidebarView*>(data)->Redraw();
    }), v);
    gtk_widget_add_controller(v->widget, focus);
    v->Rebuild();
    return sw;
}

namespace sidebar {

void Reload(SidebarView* v) {
    if (!v) return;
    v->Rebuild();
    v->Sync();
    v->Refresh();
}

void RefreshCounts(SidebarView* v) {
    if (!v) return;
    v->Sync();
    v->Redraw();
}

void SyncSelection(SidebarView* v) {
    if (v) v->Sync();
}

}  // namespace sidebar
