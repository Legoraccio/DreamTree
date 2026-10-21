// A repository in a tab: the sidebar, the history or the diff, and the details; the session's events; the
// questions asked before actions that lose data. And the windows: the header bar with the actions of the current
// tab, the tab bar, the menus.

#include "LinuxUI.h"

namespace {

GtkWidget* HBox(int spacing) { return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, spacing); }
GtkWidget* VBox(int spacing) { return gtk_box_new(GTK_ORIENTATION_VERTICAL, spacing); }

std::string Tilde(const std::wstring& path) {  // a path with ~ for the home folder
    const std::wstring home = EnvVar("HOME");
    if (!home.empty() && path.size() > home.size() && path.compare(0, home.size(), home) == 0 && path[home.size()] == L'/')
        return "~" + U8(path.substr(home.size()));
    return path == home && !home.empty() ? "~" : U8(path);
}

std::wstring Canonical(const std::wstring& path) {
    char* c = g_canonicalize_filename(U8(path).c_str(), nullptr);
    std::wstring r = NormalizePath(Widen(c ? c : U8(path).c_str()));
    g_free(c);
    return r;
}

// Folders dropped on a widget open as repositories.
void AcceptFolders(GtkWidget* target) {
    GtkDropTarget* drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(+[](GtkDropTarget* t, const GValue* value, double, double, gpointer) -> gboolean {
        if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) return FALSE;
        GtkWidget* w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(t));
        GtkRoot* root = gtk_widget_get_root(w);
        bool any = false;
        for (GSList* l = gdk_file_list_get_files(static_cast<GdkFileList*>(g_value_get_boxed(value))); l; l = l->next) {
            char* p = g_file_get_path(G_FILE(l->data));
            if (p && DirExists(Widen(p))) {
                OpenRepository(Widen(p), GTK_WIDGET(root));
                any = true;
            }
            g_free(p);
        }
        return any;
    }), nullptr);
    gtk_widget_add_controller(target, GTK_EVENT_CONTROLLER(drop));
}

}  // namespace

// ---------------------------------------------------------------- the tab

RepoTab* TabOf(GtkWidget* tabRoot) { return tabRoot ? static_cast<RepoTab*>(g_object_get_data(G_OBJECT(tabRoot), "mg-tab")) : nullptr; }

RepoTab::RepoTab(const std::wstring& path) {
    root = VBox(0);
    gtk_widget_set_hexpand(root, TRUE);
    gtk_widget_set_vexpand(root, TRUE);
    g_object_set_data_full(G_OBJECT(root), "mg-tab", this, [](gpointer p) { delete static_cast<RepoTab*>(p); });
    if (path.empty()) BuildStart();
    else Open(path);
}

RepoTab::~RepoTab() {
    *alive = false;
    if (sess) sess->onEvent = nullptr;
    if (bannerTimer) g_source_remove(bannerTimer);
}

std::wstring RepoTab::Title() const {
    if (!sess) return L"Mangrove";
    return sess->data ? sess->data->name : sess->title;
}

std::wstring RepoTab::Subtitle() const {
    RepoSession* s = sess.get();
    if (!s) return L"";
    if (s->opBusy) {
        std::wstring sub = s->opLabel + L"\u2026";
        if (!s->opProgress.empty()) sub += L"  " + s->opProgress;
        return sub;
    }
    return s->data ? HistorySubtitle(*s->data) : L"";
}

bool RepoTab::Busy() const { return sess && (sess->opBusy || sess->loading); }

GtkWidget* RepoTab::Toplevel() const { return root; }

void RepoTab::Changed() {
    GtkRoot* r = gtk_widget_get_root(root);
    if (r && g_object_get_data(G_OBJECT(r), "mg-win")) UpdateWindow(GTK_WIDGET(r));
}

// ---- the start page

void RepoTab::BuildStart() {
    GtkWidget* page = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(page), kAppId);
    adw_status_page_set_title(ADW_STATUS_PAGE(page), "Mangrove");
    adw_status_page_set_description(ADW_STATUS_PAGE(page), "Open the folder of a Git repository, or drop it here.");
    GtkWidget* box = VBox(28);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    GtkWidget* open = gtk_button_new_with_label("Open Repository\u2026");
    AddClass(open, "pill suggested-action");
    gtk_widget_set_halign(open, GTK_ALIGN_CENTER);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(open), "win.open");
    gtk_box_append(GTK_BOX(box), open);

    const std::vector<std::wstring>& recent = AppSettings().recent;
    GtkWidget* list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    AddClass(list, "boxed-list");
    int rows = 0;
    for (size_t i = 0; i < recent.size() && rows < 8; ++i) {
        if (!DirExists(recent[i])) continue;
        GtkWidget* row = adw_action_row_new();
        const std::wstring& p = recent[i];
        const size_t slash = p.find_last_of(L'/');
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), Esc(U8(slash == std::wstring::npos ? p : p.substr(slash + 1))).c_str());
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), Esc(Tilde(slash == std::wstring::npos ? L"" : p.substr(0, slash))).c_str());
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("folder-symbolic"));
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("go-next-symbolic"));
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
        g_object_set_data_full(G_OBJECT(row), "mg-path", g_strdup(U8(p).c_str()), g_free);
        gtk_widget_set_tooltip_text(row, U8(p).c_str());
        gtk_list_box_append(GTK_LIST_BOX(list), row);
        ++rows;
    }
    g_signal_connect(list, "row-activated", G_CALLBACK(+[](GtkListBox* lb, GtkListBoxRow* row, gpointer) {
        const char* p = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "mg-path"));
        if (p) OpenRepository(Widen(p), GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(lb))));
    }), nullptr);
    if (rows) {
        GtkWidget* recentBox = VBox(10);
        gtk_widget_set_size_request(recentBox, 420, -1);
        gtk_box_append(GTK_BOX(recentBox), MakeLabel("Recent", "heading"));
        gtk_box_append(GTK_BOX(recentBox), list);
        gtk_box_append(GTK_BOX(box), recentBox);
    }
    adw_status_page_set_child(ADW_STATUS_PAGE(page), box);
    gtk_widget_set_vexpand(page, TRUE);
    AcceptFolders(page);
    startPage = page;
    gtk_box_append(GTK_BOX(root), page);
}

// ---- a repository

void RepoTab::Open(const std::wstring& path) {
    if (startPage) {
        gtk_box_remove(GTK_BOX(root), startPage);
        startPage = nullptr;
    }
    sess = std::make_shared<RepoSession>();
    sess->path = NormalizePath(path);
    sess->title = BaseName(sess->path);
    std::weak_ptr<bool> a = alive;
    sess->onEvent = [this, a](SessionEvent e) {
        if (a.lock()) SessionEvent_(e);
    };
    BuildRepository();
    sess->StartLoad(false);
    UpdateContent();
    Changed();
}

void RepoTab::BuildRepository() {
    GtkWidget* sideScroller = MakeSidebar(this, &side);
    sidePane = VBox(0);
    AddClass(sidePane, "sidebar-pane mg-sidebar");
    gtk_box_append(GTK_BOX(sidePane), sideScroller);
    gtk_widget_set_size_request(sidePane, 180, -1);
    gtk_widget_set_visible(sidePane, AppSettings().GetBool(L"linuxSidebar", true));

    historyWidget = MakeHistory(this, &history);
    diffWidget = MakeDiff(this, &diffPane);
    center = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(center), historyWidget, "history");
    gtk_stack_add_named(GTK_STACK(center), diffWidget, "diff");
    GtkWidget* overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), center);
    gtk_widget_set_size_request(overlay, 380, -1);
    AcceptFolders(overlay);

    // the notices, at the bottom of the center
    banner = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(banner), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_widget_set_halign(banner, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(banner, GTK_ALIGN_END);
    GtkWidget* note = HBox(10);
    AddClass(note, "mg-floating mg-notice");
    bannerIcon = gtk_image_new();
    gtk_widget_set_valign(bannerIcon, GTK_ALIGN_START);
    gtk_widget_set_margin_top(bannerIcon, 1);
    GtkWidget* text = VBox(2);
    bannerTitle = MakeLabel("", "heading");
    bannerMessage = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(bannerMessage), 0);
    gtk_label_set_wrap(GTK_LABEL(bannerMessage), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(bannerMessage), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(bannerMessage), 56);
    gtk_label_set_lines(GTK_LABEL(bannerMessage), 5);
    gtk_label_set_ellipsize(GTK_LABEL(bannerMessage), PANGO_ELLIPSIZE_END);
    AddClass(bannerMessage, "dim-label");
    gtk_box_append(GTK_BOX(text), bannerTitle);
    gtk_box_append(GTK_BOX(text), bannerMessage);
    gtk_box_append(GTK_BOX(note), bannerIcon);
    gtk_box_append(GTK_BOX(note), text);
    gtk_revealer_set_child(GTK_REVEALER(banner), note);
    GtkGesture* dismiss = gtk_gesture_click_new();
    g_signal_connect(dismiss, "pressed", G_CALLBACK(+[](GtkGestureClick*, int, double, double, gpointer data) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(data), FALSE);
    }), banner);
    gtk_widget_add_controller(note, GTK_EVENT_CONTROLLER(dismiss));
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), banner);

    detailsPane = VBox(0);
    AddClass(detailsPane, "mg-details");
    GtkWidget* det = MakeDetails(this, &details);
    gtk_widget_set_vexpand(det, TRUE);
    gtk_box_append(GTK_BOX(detailsPane), det);
    gtk_widget_set_size_request(detailsPane, 300, -1);
    gtk_widget_set_visible(detailsPane, AppSettings().GetBool(L"linuxDetails", true));

    innerPaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(innerPaned), overlay);
    gtk_paned_set_end_child(GTK_PANED(innerPaned), detailsPane);
    gtk_paned_set_resize_end_child(GTK_PANED(innerPaned), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(innerPaned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(innerPaned), FALSE);
    outerPaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(outerPaned), sidePane);
    gtk_paned_set_end_child(GTK_PANED(outerPaned), innerPaned);
    gtk_paned_set_resize_start_child(GTK_PANED(outerPaned), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(outerPaned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(outerPaned), FALSE);
    gtk_paned_set_position(GTK_PANED(outerPaned), std::clamp(AppSettings().GetInt(L"linuxSideWidth", 240), 180, 420));
    g_signal_connect(outerPaned, "notify::position", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer) {
        const int p = gtk_paned_get_position(GTK_PANED(o));
        if (p >= 180) AppSettings().SetInt(L"linuxSideWidth", p);
    }), nullptr);
    // the details keep their width: placed once the paned knows its own
    g_signal_connect(innerPaned, "notify::max-position", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer) {
        if (g_object_get_data(o, "mg-placed")) return;
        const int width = gtk_widget_get_width(GTK_WIDGET(o));
        if (width <= 0) return;
        g_object_set_data(o, "mg-placed", GINT_TO_POINTER(1));
        const int want = std::clamp(AppSettings().GetInt(L"linuxDetailsWidth", 380), 300, 640);
        gtk_paned_set_position(GTK_PANED(o), std::max(380, width - want - 1));
    }), nullptr);
    g_signal_connect(innerPaned, "notify::position", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer) {
        if (!g_object_get_data(o, "mg-placed")) return;
        const int width = gtk_widget_get_width(GTK_WIDGET(o));
        const int p = gtk_paned_get_position(GTK_PANED(o));
        if (width > 0 && width - p - 1 >= 300) AppSettings().SetInt(L"linuxDetailsWidth", width - p - 1);
    }), nullptr);
    gtk_widget_set_vexpand(outerPaned, TRUE);
    gtk_box_append(GTK_BOX(root), outerPaned);
}

// ---- what the session reports

void RepoTab::SessionEvent_(SessionEvent e) {
    RepoSession* s = sess.get();
    if (!s) return;
    switch (e) {
    case SessionEvent::Loading: UpdateContent(); break;
    case SessionEvent::Loaded:
        UpdateContent();
        if (s->data) {
            sidebar::Reload(side);
            history::Reload(history);
            details::Reload(details);
            if (!s->lastLoadRefresh || !revealed) {
                history::RevealSelection(history);
                Later([this, a = std::weak_ptr<bool>(alive)] {  // once the history has its size
                    if (a.lock()) history::RevealSelection(history);
                });
            }
            // the first time, the arrows move through the history (unless the user is already typing somewhere)
            if (!revealed && Toplevel() && CurrentTab(GTK_WIDGET(gtk_widget_get_root(root))) == this) {
                GtkWidget* focus = gtk_root_get_focus(gtk_widget_get_root(root));
                if (!focus || !GTK_IS_EDITABLE(focus)) history::Focus(history);
            }
            revealed = true;
        }
        SaveTabs();
        break;
    case SessionEvent::Status:
        history::StatusChanged(history, s->wipRowDelta);
        sidebar::RefreshCounts(side);
        details::Reload(details);
        break;
    case SessionEvent::Details: details::Reload(details); break;
    case SessionEvent::Diff:
        UpdateContent();
        diff::Reload(diffPane);
        details::RefreshState(details);
        break;
    case SessionEvent::Search: history::Redraw(history); break;
    case SessionEvent::LineStats: history::Redraw(history); break;
    case SessionEvent::Operation:
        details::RefreshState(details);
        diff::Reload(diffPane);
        break;
    case SessionEvent::Notice: Notify(s->notice.title, s->notice.message, s->notice.error); break;
    case SessionEvent::Message: details::ApplySuggestion(details); break;
    case SessionEvent::Committed: details::ClearMessage(details); break;
    case SessionEvent::PushRejected: {
        std::weak_ptr<RepoSession> wk = sess;
        RunPrompt(Toplevel(), PushRejectedPrompt(), [wk](int b) {
            std::shared_ptr<RepoSession> session = wk.lock();
            if (!session) return;
            if (b == 0) session->Pull();
            else if (b == 1) session->Push(true);
        });
        break;
    }
    }
    Changed();
}

// The history, the diff, or a message while there is nothing to show.
void RepoTab::UpdateContent() {
    RepoSession* s = sess.get();
    if (!s || !center) return;
    if (s->data) {
        if (messagePage) {
            gtk_stack_remove(GTK_STACK(center), messagePage);
            messagePage = nullptr;
            messageKey.clear();
        }
        gtk_stack_set_visible_child_name(GTK_STACK(center), s->diff.open ? "diff" : "history");
        return;
    }
    const std::wstring key = s->error.empty() ? L"loading" : L"error:" + s->error;
    if (messagePage && key == messageKey) return;
    if (messagePage) gtk_stack_remove(GTK_STACK(center), messagePage);
    messageKey = key;
    GtkWidget* page = adw_status_page_new();
    AdwStatusPage* sp = ADW_STATUS_PAGE(page);
    const std::string title = U8(s->title);
    if (s->error.empty()) {
        adw_status_page_set_title(sp, Esc("Opening " + title + "\u2026").c_str());
        GtkWidget* spinner = gtk_spinner_new();
        gtk_spinner_start(GTK_SPINNER(spinner));
        gtk_widget_set_size_request(spinner, 32, 32);
        gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
        adw_status_page_set_child(sp, spinner);
        AddClass(page, "compact");
    } else {
        adw_status_page_set_icon_name(sp, "dialog-warning-symbolic");
        adw_status_page_set_title(sp, Esc("Can't open \u201C" + title + "\u201D").c_str());
        std::string detail = U8(s->error);
        if (!s->untrusted.empty())
            detail += "\n\nTrusting it adds " + U8(s->untrusted) + " to Git's safe directories for Mangrove only.";
        adw_status_page_set_description(sp, Esc(detail).c_str());
        GtkWidget* buttons = HBox(12);
        gtk_widget_set_halign(buttons, GTK_ALIGN_CENTER);
        std::weak_ptr<bool> a = alive;
        auto add = [&](const char* label, bool suggested, void (*fn)(RepoTab*)) {
            GtkWidget* b = gtk_button_new_with_label(label);
            AddClass(b, suggested ? "pill suggested-action" : "pill");
            g_object_set_data(G_OBJECT(b), "mg-fn", (gpointer)fn);
            g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton* btn, gpointer data) {
                auto f = reinterpret_cast<void (*)(RepoTab*)>(g_object_get_data(G_OBJECT(btn), "mg-fn"));
                if (f) f(static_cast<RepoTab*>(data));
            }), this);
            gtk_box_append(GTK_BOX(buttons), b);
        };
        if (!s->untrusted.empty()) add("Trust This Folder", true, [](RepoTab* t) {
                if (t->sess) t->sess->Trust();
            });
        add("Try Again", s->untrusted.empty(), [](RepoTab* t) { t->Refresh(); });
        add("Open Another\u2026", false, [](RepoTab* t) { ChooseRepository(GTK_WIDGET(gtk_widget_get_root(t->root))); });
        adw_status_page_set_child(sp, buttons);
    }
    messagePage = page;
    gtk_stack_add_named(GTK_STACK(center), page, "message");
    gtk_stack_set_visible_child(GTK_STACK(center), page);
}

void RepoTab::Notify(const std::wstring& title, const std::wstring& message, bool error) {
    if (!banner) return;
    gtk_image_set_from_icon_name(GTK_IMAGE(bannerIcon), error ? "dialog-warning-symbolic" : "emblem-ok-symbolic");
    SetClass(bannerIcon, "warning", error);
    SetClass(bannerIcon, "success", !error);
    gtk_label_set_text(GTK_LABEL(bannerTitle), U8(title).c_str());
    std::string text = U8(message);  // git's words, up to a few lines
    size_t cut = 0;
    for (int lines = 0; cut != std::string::npos && lines < 6; ++lines) cut = text.find('\n', cut ? cut + 1 : 0);
    if (cut != std::string::npos) text = text.substr(0, cut) + "\n\u2026";
    gtk_label_set_text(GTK_LABEL(bannerMessage), text.c_str());
    gtk_widget_set_visible(bannerMessage, !message.empty());
    gtk_revealer_set_reveal_child(GTK_REVEALER(banner), TRUE);
    if (bannerTimer) g_source_remove(bannerTimer);
    bannerTimer = After(error ? 8000 : 4000, [this, a = std::weak_ptr<bool>(alive)] {
        if (!a.lock()) return;
        bannerTimer = 0;
        gtk_revealer_set_reveal_child(GTK_REVEALER(banner), FALSE);
    });
}

// ---- selection

void RepoTab::SelectionChanged() {
    sidebar::SyncSelection(side);
    details::Reload(details);
    UpdateContent();
    history::Redraw(history);  // the search status counts from the selection
    Changed();
}

void RepoTab::SelectRow(int32_t row) {
    RepoSession* s = sess.get();
    if (!s || !s->graph || row < 0 || row >= (int32_t)s->graph->rows.size()) return;
    if (row == s->SelectedRow() && !s->diff.open) return;
    s->SelectRow(row);
    SelectionChanged();
}

void RepoTab::SelectCommit(int32_t commit) {
    RepoSession* s = sess.get();
    if (!s || !s->data || commit < 0 || commit >= (int32_t)s->data->commits.size()) return;
    s->Select(commit, false);
    history::SyncSelection(history);
    history::RevealSelection(history);
    SelectionChanged();
}

void RepoTab::SelectWorkingTree() {
    RepoSession* s = sess.get();
    if (!s || !s->graph || !s->graph->hasWip) return;
    s->Select(-1, true);
    history::SyncSelection(history);
    history::RevealSelection(history);
    SelectionChanged();
}

void RepoTab::ReviewChanges() {
    SelectWorkingTree();
    if (detailsPane && !gtk_widget_get_visible(detailsPane)) ToggleDetails();
    details::FocusSummary(details);
}

void RepoTab::SetHeadOnly(bool headOnly) {
    RepoSession* s = sess.get();
    if (!s || !s->data || s->headOnly == headOnly) return;
    s->headOnly = headOnly;
    s->StartLoad(true);
    Changed();
}

void RepoTab::ShowHistoryPage() {
    RepoSession* s = sess.get();
    if (!s || !s->data) return;
    s->CloseDiff();
    if (s->selWip && s->data->headCommit >= 0) SelectCommit(s->data->headCommit);
    history::Focus(history);
}

void RepoTab::SearchStep(int dir) {
    RepoSession* s = sess.get();
    if (!s) return;
    const int32_t row = s->FindMatch(dir);
    if (row < 0) {
        gtk_widget_error_bell(root);
        return;
    }
    s->SelectRow(row);
    history::SyncSelection(history);
    history::RevealSelection(history);
    SelectionChanged();
}

void RepoTab::MoveSelection(int by) {
    RepoSession* s = sess.get();
    if (!s || !s->graph || s->graph->rows.empty()) return;
    const int32_t row = std::clamp(s->SelectedRow() + by, 0, (int32_t)s->graph->rows.size() - 1);
    s->SelectRow(row);
    history::SyncSelection(history);
    history::RevealSelection(history);
    SelectionChanged();
}

// ---- actions

void RepoTab::OpenDiff(int section, const std::wstring& path) {
    if (sess) sess->OpenDiff(section, path);  // the Diff event shows it
}

void RepoTab::CheckoutRef(int ref) {
    if (sess) sess->CheckoutRef(ref);
}

void RepoTab::CheckoutCommit(int32_t commit) {
    RepoSession* s = sess.get();
    if (!s || !s->CanCheckout(commit, nullptr)) return;
    const int target = s->CheckoutTarget(commit);
    if (target >= 0) s->CheckoutRef(target);
    else s->CheckoutCommit(commit);
}

static void NoSpaces(GtkEditable* e, const char* text, int length, int* position, gpointer) {
    std::string t(text, length < 0 ? strlen(text) : (size_t)length);
    if (t.find(' ') == std::string::npos) return;
    std::replace(t.begin(), t.end(), ' ', '-');
    g_signal_handlers_block_by_func(e, (gpointer)NoSpaces, nullptr);
    gtk_editable_insert_text(e, t.c_str(), (int)t.size(), position);
    g_signal_handlers_unblock_by_func(e, (gpointer)NoSpaces, nullptr);
    g_signal_stop_emission_by_name(e, "insert-text");
}

// Asks for the name of a new branch, checking it while it is typed.
void RepoTab::NewBranchAt(int32_t commit) {
    RepoSession* s = sess.get();
    if (!s || !s->data || !s->CanRun(ActBranch, nullptr)) return;
    const RepoData& d = *s->data;
    const int32_t at = commit >= 0 ? commit : d.headCommit;
    const std::string where = at >= 0 && at < (int32_t)d.commits.size()
                                  ? "The branch starts at " + U8(d.commits[(size_t)at].id.Hex(7)) + " \u201C" + U8(CommitSubject(d, d.commits[(size_t)at])) +
                                        "\u201D and is checked out."
                                  : "The branch is checked out.";
    AdwDialog* dlg = adw_alert_dialog_new("New Branch", where.c_str());
    AdwAlertDialog* a = ADW_ALERT_DIALOG(dlg);
    adw_alert_dialog_add_responses(a, "cancel", "Cancel", "create", "Create Branch", nullptr);
    adw_alert_dialog_set_response_appearance(a, "create", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(a, "create");
    adw_alert_dialog_set_close_response(a, "cancel");
    adw_alert_dialog_set_response_enabled(a, "create", FALSE);
    GtkWidget* box = VBox(6);
    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "feature/name");
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    GtkWidget* error = MakeLabel("", "caption error");
    gtk_label_set_wrap(GTK_LABEL(error), TRUE);
    gtk_box_append(GTK_BOX(box), entry);
    gtk_box_append(GTK_BOX(box), error);
    adw_alert_dialog_set_extra_child(a, box);

    struct State {
        std::weak_ptr<RepoSession> session;
        std::weak_ptr<bool> alive;
        RepoTab* tab;
        int32_t commit;
        GtkWidget* entry;
        GtkWidget* error;
        AdwAlertDialog* dialog;
    };
    State* st = new State{ sess, alive, this, commit, entry, error, a };
    g_object_set_data_full(G_OBJECT(dlg), "mg-state", st, [](gpointer p) { delete static_cast<State*>(p); });
    // spaces become dashes as they are typed, as git would refuse them
    g_signal_connect(gtk_editable_get_delegate(GTK_EDITABLE(entry)), "insert-text", G_CALLBACK(NoSpaces), nullptr);
    g_signal_connect(entry, "changed", G_CALLBACK(+[](GtkEditable* e, gpointer data) {
        State* st = static_cast<State*>(data);
        std::shared_ptr<RepoSession> s = st->session.lock();
        const std::wstring name = Widen(gtk_editable_get_text(e));
        const std::wstring err = name.empty() || !s ? std::wstring() : s->CheckNewBranch(name);
        gtk_label_set_text(GTK_LABEL(st->error), U8(err).c_str());
        gtk_widget_set_visible(st->error, !err.empty());
        adw_alert_dialog_set_response_enabled(st->dialog, "create", !name.empty() && err.empty());
    }), st);
    gtk_widget_set_visible(error, FALSE);
    g_signal_connect(dlg, "response", G_CALLBACK(+[](AdwAlertDialog* dlg, const char* response, gpointer) {
        State* st = static_cast<State*>(g_object_get_data(G_OBJECT(dlg), "mg-state"));
        if (!st || strcmp(response, "create") != 0) return;
        std::shared_ptr<RepoSession> s = st->session.lock();
        if (!s) return;
        const std::wstring err = s->CreateBranch(Widen(gtk_editable_get_text(GTK_EDITABLE(st->entry))), st->commit);
        if (!err.empty() && st->alive.lock()) st->tab->Notify(L"Couldn't create the branch", err, true);
    }), nullptr);
    adw_dialog_present(dlg, Toplevel());
    adw_dialog_set_focus(dlg, entry);
}

void RepoTab::RevertCommit(int32_t commit) {
    RepoSession* s = sess.get();
    if (!s || !s->data) return;
    const std::wstring why = s->CanRevert(commit);
    if (!why.empty()) {
        Notify(L"Can't revert", why, true);
        return;
    }
    std::weak_ptr<RepoSession> wk = sess;
    RunPrompt(Toplevel(), RevertPrompt(*s->data, commit), [wk, commit](int b) {
        std::shared_ptr<RepoSession> session = wk.lock();
        if (session && b == 0) session->Revert(commit);
    });
}

void RepoTab::DropStash(int index) {
    RepoSession* s = sess.get();
    if (!s || !s->data || s->opBusy || index < 0 || index >= (int)s->data->stashes.size()) return;
    std::weak_ptr<RepoSession> wk = sess;
    RunPrompt(Toplevel(), DropStashPrompt(s->data->stashes[(size_t)index]), [wk, index](int b) {
        std::shared_ptr<RepoSession> session = wk.lock();
        if (session && b == 0) session->DropStash(index);
    });
}

void RepoTab::DiscardFile(const FileChange& file) {
    if (!sess || file.status == L'U') return;
    std::weak_ptr<RepoSession> wk = sess;
    const FileChange f = file;
    RunPrompt(Toplevel(), DiscardPrompt(f), [wk, f](int b) {
        std::shared_ptr<RepoSession> session = wk.lock();
        if (session && b == 0) session->Discard(f);
    });
}

void RepoTab::StagePaths(const std::vector<std::wstring>& paths, bool stage) {
    if (sess) sess->Stage(paths, stage);
}

void RepoTab::ApplyPatch(const PatchJob& job) {
    RepoSession* s = sess.get();
    if (!s || job.patch.empty()) return;
    if (!job.discard) {
        s->ApplyPatch(job);
        return;
    }
    std::weak_ptr<RepoSession> wk = sess;
    const PatchJob j = job;
    RunPrompt(Toplevel(), DiscardPatchPrompt(job, s->diff.req.path), [wk, j](int b) {
        std::shared_ptr<RepoSession> session = wk.lock();
        if (session && b == 0) session->ApplyPatch(j);
    });
}

void RepoTab::Copy(const std::string& text) { CopyToClipboard(root, text); }

void RepoTab::RevealInFiles(const std::wstring& relativePath) {
    RepoSession* s = sess.get();
    if (!s || !s->data) return;
    const std::wstring path = PathJoin(s->data->root, relativePath);
    if (FileExists(path) || DirExists(path)) RevealFile(root, path);
    else OpenFolder(root, path.substr(0, path.find_last_of(L'/')));  // deleted: the folder that held it
}

void RepoTab::Escape() {
    RepoSession* s = sess.get();
    if (!s || !s->diff.open) return;
    if (s->diff.selCount) s->ClearDiffSelection();
    else s->CloseDiff();
}

void RepoTab::Refresh() {
    if (sess && !sess->IsStart()) sess->StartLoad(sess->data != nullptr);
}

void RepoTab::Terminal() {
    if (!sess || !sess->data) return;
    if (!OpenTerminal(sess->data->root))
        Notify(L"No terminal found", L"Install a terminal (GNOME Console, Ptyxis, Konsole, xterm\u2026) or xdg-terminal-exec.", true);
}

bool RepoTab::DetailsShown() const { return detailsPane && gtk_widget_get_visible(detailsPane); }
bool RepoTab::SidebarShown() const { return sidePane && gtk_widget_get_visible(sidePane); }

void RepoTab::ToggleDetails() {
    if (!detailsPane) return;
    const bool show = !gtk_widget_get_visible(detailsPane);
    gtk_widget_set_visible(detailsPane, show);
    AppSettings().SetBool(L"linuxDetails", show);
    Changed();
}

void RepoTab::ToggleSidebar() {
    if (!sidePane) return;
    const bool show = !gtk_widget_get_visible(sidePane);
    gtk_widget_set_visible(sidePane, show);
    AppSettings().SetBool(L"linuxSidebar", show);
    Changed();
}

void RepoTab::SettingsChanged() {
    if (sess && sess->diff.open) diff::Reload(diffPane);
    Changed();
}

// ---- context menus

void RepoTab::FillCommitMenu(Menu& m, int32_t c) {
    RepoSession* s = sess.get();
    if (!s || !s->data) return;
    const RepoData& d = *s->data;
    const bool busy = s->opBusy;
    std::weak_ptr<bool> a = alive;
    auto safe = [this, a](void (*fn)(RepoTab*, int), int arg) {
        return [this, a, fn, arg] {
            if (a.lock()) fn(this, arg);
        };
    };
    if (c < 0) {
        m.Add("Review and Commit", safe([](RepoTab* t, int) { t->ReviewChanges(); }, 0), s->graph && s->graph->hasWip);
        m.Section();
        m.Add("Stash All Changes", safe([](RepoTab* t, int) { t->sess->Stash(); }, 0), s->CanRun(ActStash, nullptr));
        m.Add("New Branch\u2026", safe([](RepoTab* t, int) { t->NewBranchAt(-1); }, 0), s->CanRun(ActBranch, nullptr));
        return;
    }
    if (c >= (int32_t)d.commits.size()) return;
    const Commit& cm = d.commits[(size_t)c];
    if (auto it = d.labels.find(c); it != d.labels.end()) {
        int n = 0;
        for (const Label& L : it->second) {
            if (L.tag || n >= 6) continue;
            const Ref& r = d.refs[(size_t)L.ref];
            const std::string name = U8(r.kind == RefKind::Remote && !L.local ? r.name : r.branch);
            m.Add("Check Out " + name, safe([](RepoTab* t, int ref) { t->CheckoutRef(ref); }, L.ref), !busy && !r.head);
            ++n;
        }
    }
    const bool stash = cm.flags & CF_STASH;
    m.Add("New Branch Here\u2026", safe([](RepoTab* t, int commit) { t->NewBranchAt(commit); }, c), !busy);
    m.Add("Check Out This Commit (Detached HEAD)", safe([](RepoTab* t, int commit) { t->sess->CheckoutCommit(commit); }, c), !busy && !stash);
    m.Add("Revert This Commit\u2026", safe([](RepoTab* t, int commit) { t->RevertCommit(commit); }, c), !busy && !stash && !d.unborn);
    m.Section();
    m.Add("Copy Commit ID", safe([](RepoTab* t, int commit) { t->Copy(U8(t->sess->data->commits[(size_t)commit].id.Hex())); }, c));
    m.Add("Copy Short ID", safe([](RepoTab* t, int commit) { t->Copy(U8(t->sess->data->commits[(size_t)commit].id.Hex(7))); }, c));
    m.Add("Copy Message", safe([](RepoTab* t, int commit) { t->Copy(U8(CommitMessage(*t->sess, commit))); }, c));
    if (cm.parentCount && d.Parents(cm)[0] >= 0) {
        m.Section();
        m.Add("Go to Parent", safe([](RepoTab* t, int parent) { t->SelectCommit(parent); }, d.Parents(cm)[0]));
    }
}

void RepoTab::FillRefMenu(Menu& m, int ri) {
    RepoSession* s = sess.get();
    if (!s || !s->data || ri < 0 || ri >= (int)s->data->refs.size()) return;
    const RepoData& d = *s->data;
    const Ref& r = d.refs[(size_t)ri];
    const bool busy = s->opBusy;
    std::weak_ptr<bool> a = alive;
    std::string checkout = "Check Out";
    if (r.kind == RefKind::Remote) {
        bool hasLocal = false;
        for (const Ref& x : d.refs) hasLocal |= x.kind == RefKind::Local && x.name == r.branch;
        checkout = hasLocal ? "Check Out " + U8(r.branch) : "Check Out as New Local Branch \u201C" + U8(r.branch) + "\u201D";
    } else if (r.kind == RefKind::Tag) {
        checkout = "Check Out (Detached HEAD)";
    }
    m.Add(checkout, [this, a, ri] {
        if (a.lock()) CheckoutRef(ri);
    }, !busy && !r.head);
    const int32_t at = r.commit;
    m.Add("New Branch Here\u2026", [this, a, at] {
        if (a.lock()) NewBranchAt(at);
    }, !busy && at >= 0);
    m.Section();
    const std::string name = U8(r.name);
    m.Add(r.kind == RefKind::Tag ? "Copy Tag Name" : "Copy Branch Name", [this, a, name] {
        if (a.lock()) Copy(name);
    });
}

void RepoTab::FillStashMenu(Menu& m, int index) {
    RepoSession* s = sess.get();
    if (!s || !s->data || index < 0 || index >= (int)s->data->stashes.size()) return;
    const bool busy = s->opBusy;
    std::weak_ptr<bool> a = alive;
    m.Add("Apply Stash", [this, a, index] {
        if (a.lock() && sess) sess->Pop(index, true);
    }, !busy);
    m.Add("Pop Stash", [this, a, index] {
        if (a.lock() && sess) sess->Pop(index, false);
    }, !busy);
    m.Section();
    m.Add("Delete Stash\u2026", [this, a, index] {
        if (a.lock()) DropStash(index);
    }, !busy);
}

// ---------------------------------------------------------------- the window

namespace {

struct MainWin {
    GtkWidget* window = nullptr;
    AdwTabView* tabs = nullptr;
    AdwTabBar* tabBar = nullptr;
    GtkWidget* header = nullptr;
    GtkWidget* title = nullptr;
    GtkWidget* search = nullptr;
    GtkWidget* spinner = nullptr;
    GtkWidget* repoButtons = nullptr;
    GtkWidget* fetch = nullptr;
    GtkWidget* pull = nullptr;
    GtkWidget* push = nullptr;
    GtkWidget* branch = nullptr;
    GtkWidget* stash = nullptr;
    GtkWidget* pop = nullptr;
    GtkWidget* pullBadge = nullptr;
    GtkWidget* pushBadge = nullptr;
    GtkWidget* sideToggle = nullptr;
    GtkWidget* detailsToggle = nullptr;
    GtkWidget* more = nullptr;
    bool syncingSearch = false;
    bool closing = false;
};

MainWin* Win(GtkWidget* w) { return w ? static_cast<MainWin*>(g_object_get_data(G_OBJECT(w), "mg-win")) : nullptr; }

RepoTab* Current(GtkWidget* window) {
    MainWin* w = Win(window);
    AdwTabPage* page = w ? adw_tab_view_get_selected_page(w->tabs) : nullptr;
    return page ? TabOf(adw_tab_page_get_child(page)) : nullptr;
}

RepoSession* CurrentSession(gpointer window) {
    RepoTab* t = Current(GTK_WIDGET(window));
    return t ? t->session() : nullptr;
}

#define WIN_ACTION(name, body)                                                    \
    void name(GSimpleAction*, GVariant*, gpointer data) {                         \
        GtkWidget* window = GTK_WIDGET(data);                                     \
        RepoTab* tab = Current(window);                                           \
        RepoSession* s = tab ? tab->session() : nullptr;                          \
        (void)window;                                                             \
        (void)s;                                                                  \
        body                                                                      \
    }

WIN_ACTION(OnNewTab, { AddTab(window, L"", true); })
WIN_ACTION(OnOpen, { ChooseRepository(window); })
WIN_ACTION(OnCloseTab, {
    MainWin* w = Win(window);
    if (AdwTabPage* page = adw_tab_view_get_selected_page(w->tabs)) adw_tab_view_close_page(w->tabs, page);
})
WIN_ACTION(OnFetch, { if (s) s->Fetch(); })
WIN_ACTION(OnPull, { if (s) s->Pull(); })
WIN_ACTION(OnPush, { if (s) s->Push(false); })
WIN_ACTION(OnNewBranch, { if (s) tab->NewBranchAt(s->selWip ? -1 : s->selCommit); })
WIN_ACTION(OnStash, { if (s) s->Stash(); })
WIN_ACTION(OnPop, { if (s) s->Pop(0, false); })
WIN_ACTION(OnUndo, { if (s) s->Undo(); })
WIN_ACTION(OnRedo, { if (s) s->Redo(); })
WIN_ACTION(OnRefresh, { if (tab) tab->Refresh(); })
WIN_ACTION(OnTerminal, { if (tab) tab->Terminal(); })
WIN_ACTION(OnReveal, { if (s && s->data) OpenFolder(window, s->data->root); })
WIN_ACTION(OnCopyPath, { if (s && s->data) tab->Copy(U8(s->data->root)); })
WIN_ACTION(OnFind, {
    if (!s || !s->data) return;
    s->CloseDiff();
    gtk_widget_grab_focus(Win(window)->search);
})
WIN_ACTION(OnFindNext, { if (tab) tab->SearchStep(1); })
WIN_ACTION(OnFindPrev, { if (tab) tab->SearchStep(-1); })
WIN_ACTION(OnHistoryPage, { if (tab) tab->ShowHistoryPage(); })
WIN_ACTION(OnChangesPage, { if (tab) tab->ReviewChanges(); })

void OnShowSidebar(GSimpleAction* a, GVariant* v, gpointer data) {
    RepoTab* tab = Current(GTK_WIDGET(data));
    if (tab && tab->SidebarShown() != (bool)g_variant_get_boolean(v)) tab->ToggleSidebar();
    g_simple_action_set_state(a, v);
}

void OnShowDetails(GSimpleAction* a, GVariant* v, gpointer data) {
    RepoTab* tab = Current(GTK_WIDGET(data));
    if (tab && tab->DetailsShown() != (bool)g_variant_get_boolean(v)) tab->ToggleDetails();
    g_simple_action_set_state(a, v);
}

void OnHeadOnly(GSimpleAction* a, GVariant* v, gpointer data) {
    g_simple_action_set_state(a, v);
    if (RepoTab* tab = Current(GTK_WIDGET(data))) tab->SetHeadOnly(g_variant_get_boolean(v));
}

void OnDiffLayout(GSimpleAction* a, GVariant* v, gpointer) {
    g_simple_action_set_state(a, v);
    const bool split = !strcmp(g_variant_get_string(v, nullptr), "split");
    if (AppSettings().diffSplit == split) return;
    AppSettings().diffSplit = split;
    AppSettings().Save();
    BroadcastSettingsChanged();
}

void OnPullMode(GSimpleAction* a, GVariant* v, gpointer) {
    g_simple_action_set_state(a, v);
    const int mode = std::clamp((int)g_variant_get_int32(v), 0, 2);
    if (AppSettings().pullMode == mode) return;
    AppSettings().pullMode = mode;
    AppSettings().Save();
    BroadcastSettingsChanged();
}

const GActionEntry kWinActions[] = {
    { "new-tab", OnNewTab, nullptr, nullptr, nullptr, {} },
    { "open", OnOpen, nullptr, nullptr, nullptr, {} },
    { "close-tab", OnCloseTab, nullptr, nullptr, nullptr, {} },
    { "fetch", OnFetch, nullptr, nullptr, nullptr, {} },
    { "pull", OnPull, nullptr, nullptr, nullptr, {} },
    { "push", OnPush, nullptr, nullptr, nullptr, {} },
    { "new-branch", OnNewBranch, nullptr, nullptr, nullptr, {} },
    { "stash", OnStash, nullptr, nullptr, nullptr, {} },
    { "pop", OnPop, nullptr, nullptr, nullptr, {} },
    { "undo-git", OnUndo, nullptr, nullptr, nullptr, {} },
    { "redo-git", OnRedo, nullptr, nullptr, nullptr, {} },
    { "refresh", OnRefresh, nullptr, nullptr, nullptr, {} },
    { "terminal", OnTerminal, nullptr, nullptr, nullptr, {} },
    { "reveal", OnReveal, nullptr, nullptr, nullptr, {} },
    { "copy-path", OnCopyPath, nullptr, nullptr, nullptr, {} },
    { "find", OnFind, nullptr, nullptr, nullptr, {} },
    { "find-next", OnFindNext, nullptr, nullptr, nullptr, {} },
    { "find-prev", OnFindPrev, nullptr, nullptr, nullptr, {} },
    { "history-page", OnHistoryPage, nullptr, nullptr, nullptr, {} },
    { "changes-page", OnChangesPage, nullptr, nullptr, nullptr, {} },
    { "show-sidebar", nullptr, nullptr, "true", OnShowSidebar, {} },
    { "show-details", nullptr, nullptr, "true", OnShowDetails, {} },
    { "head-only", nullptr, nullptr, "false", OnHeadOnly, {} },
    { "diff-layout", nullptr, "s", "'unified'", OnDiffLayout, {} },
    { "pull-mode", nullptr, "i", "0", OnPullMode, {} },
};

GSimpleAction* WinAction(GtkWidget* window, const char* name) {
    return G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(window), name));
}

GtkWidget* HeaderButton(const char* icon, const char* action, const char* tip) {
    GtkWidget* b = gtk_button_new_from_icon_name(icon);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
    gtk_widget_set_tooltip_text(b, tip);
    return b;
}

// A button with a small count at its top right (Pull and Push: commits to bring in or to send).
GtkWidget* WithBadge(GtkWidget* button, GtkWidget** badge) {
    GtkWidget* o = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(o), button);
    *badge = gtk_label_new("");
    AddClass(*badge, "mg-badge");
    gtk_widget_set_halign(*badge, GTK_ALIGN_END);
    gtk_widget_set_valign(*badge, GTK_ALIGN_START);
    gtk_widget_set_can_target(*badge, FALSE);
    gtk_widget_set_visible(*badge, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(o), *badge);
    return o;
}

GMenuItem* Item(const char* label, const char* action) { return g_menu_item_new(label, action); }

void Append(GMenu* m, const std::string& label, const char* action) {
    GMenuItem* i = Item(label.c_str(), action);
    g_menu_append_item(m, i);
    g_object_unref(i);
}

void AppendTarget(GMenu* m, const std::string& label, const char* action, GVariant* target) {
    GMenuItem* i = Item(label.c_str(), nullptr);
    g_menu_item_set_action_and_target_value(i, action, target);
    g_menu_append_item(m, i);
    g_object_unref(i);
}

void AppendSection(GMenu* m, GMenu* section) {
    g_menu_append_section(m, nullptr, G_MENU_MODEL(section));
    g_object_unref(section);
}

std::string Escaped(const std::wstring& s) {  // "_" would underline the next letter
    std::string r;
    for (char ch : U8(s)) {
        r += ch;
        if (ch == '_') r += '_';
    }
    return r;
}

// The main menu, built when it opens (Open Recent changes).
void MainMenu(GtkMenuButton* button, gpointer) {
    GMenu* m = g_menu_new();
    GMenu* s1 = g_menu_new();
    Append(s1, "New Window", "app.new-window");
    Append(s1, "New Tab", "win.new-tab");
    AppendSection(m, s1);
    GMenu* s2 = g_menu_new();
    Append(s2, "Open\u2026", "win.open");
    GMenu* recent = g_menu_new();
    GMenu* paths = g_menu_new();
    const std::vector<std::wstring>& r = AppSettings().recent;
    for (size_t i = 0; i < r.size() && i < 15; ++i) AppendTarget(paths, Escaped(BaseName(r[i])), "app.open-recent", g_variant_new_string(U8(r[i]).c_str()));
    AppendSection(recent, paths);
    GMenu* clear = g_menu_new();
    Append(clear, "Clear Menu", "app.clear-recent");
    AppendSection(recent, clear);
    g_menu_append_submenu(s2, "Open Recent", G_MENU_MODEL(recent));
    g_object_unref(recent);
    AppendSection(m, s2);
    GMenu* s3 = g_menu_new();
    Append(s3, "Settings", "app.preferences");
    Append(s3, "Keyboard Shortcuts", "win.show-help-overlay");
    Append(s3, "About Mangrove", "app.about");
    AppendSection(m, s3);
    gtk_menu_button_set_menu_model(button, G_MENU_MODEL(m));
    g_object_unref(m);
}

// The repository's menu, built when it opens: Undo and Redo say what they would do.
void MoreMenu(GtkMenuButton* button, gpointer data) {
    RepoSession* s = CurrentSession(data);
    GMenu* m = g_menu_new();
    GMenu* s1 = g_menu_new();
    Append(s1, s && !s->undo.empty() ? "Undo " + Escaped(s->undo.back().label) : "Undo", "win.undo-git");
    Append(s1, s && !s->redo.empty() ? "Redo " + Escaped(s->redo.back().label) : "Redo", "win.redo-git");
    AppendSection(m, s1);
    GMenu* s2 = g_menu_new();
    Append(s2, s && s->data && !s->data->stashes.empty() ? "Pop " + Escaped(s->data->stashes[0].name) : "Pop Stash", "win.pop");
    Append(s2, "Open in Terminal", "win.terminal");
    AppendSection(m, s2);
    GMenu* s3 = g_menu_new();
    Append(s3, "Current Branch Only", "win.head-only");
    GMenu* modes = g_menu_new();
    const char* labels[] = { "Merge (Fast-Forward If Possible)", "Fast-Forward Only", "Rebase" };
    for (int k = 0; k < 3; ++k) AppendTarget(modes, labels[k], "win.pull-mode", g_variant_new_int32(k));
    g_menu_append_submenu(s3, "Pull Mode", G_MENU_MODEL(modes));
    g_object_unref(modes);
    AppendSection(m, s3);
    GMenu* s4 = g_menu_new();
    AppendTarget(s4, "Unified Diff", "win.diff-layout", g_variant_new_string("unified"));
    AppendTarget(s4, "Side-by-Side Diff", "win.diff-layout", g_variant_new_string("split"));
    AppendSection(m, s4);
    GMenu* s5 = g_menu_new();
    Append(s5, "Refresh", "win.refresh");
    Append(s5, "Show in Files", "win.reveal");
    Append(s5, "Copy Repository Path", "win.copy-path");
    AppendSection(m, s5);
    gtk_menu_button_set_menu_model(button, G_MENU_MODEL(m));
    g_object_unref(m);
}

void BuildHeader(MainWin* w) {
    GtkWidget* header = adw_header_bar_new();
    w->header = header;

    w->sideToggle = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(w->sideToggle), "sidebar-show-symbolic");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(w->sideToggle), "win.show-sidebar");
    gtk_widget_set_tooltip_text(w->sideToggle, "Sidebar (F9)");
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), w->sideToggle);

    w->repoButtons = HBox(12);
    GtkWidget* remote = HBox(2);
    w->fetch = HeaderButton("mg-fetch-symbolic", "win.fetch", "Fetch");
    w->pull = HeaderButton("mg-pull-symbolic", "win.pull", "Pull");
    w->push = HeaderButton("mg-push-symbolic", "win.push", "Push");
    gtk_box_append(GTK_BOX(remote), w->fetch);
    gtk_box_append(GTK_BOX(remote), WithBadge(w->pull, &w->pullBadge));
    gtk_box_append(GTK_BOX(remote), WithBadge(w->push, &w->pushBadge));
    GtkWidget* local = HBox(2);
    w->branch = HeaderButton("mg-branch-symbolic", "win.new-branch", "New Branch");
    w->stash = HeaderButton("mg-stash-symbolic", "win.stash", "Stash");
    w->pop = HeaderButton("mg-pop-symbolic", "win.pop", "Pop Stash");
    gtk_box_append(GTK_BOX(local), w->branch);
    gtk_box_append(GTK_BOX(local), w->stash);
    gtk_box_append(GTK_BOX(local), w->pop);
    gtk_box_append(GTK_BOX(w->repoButtons), remote);
    gtk_box_append(GTK_BOX(w->repoButtons), local);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), w->repoButtons);
    w->spinner = gtk_spinner_new();
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), w->spinner);

    w->title = adw_window_title_new("Mangrove", "");
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), w->title);

    GtkWidget* menu = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu), "open-menu-symbolic");
    gtk_menu_button_set_primary(GTK_MENU_BUTTON(menu), TRUE);
    gtk_widget_set_tooltip_text(menu, "Main Menu");
    gtk_menu_button_set_create_popup_func(GTK_MENU_BUTTON(menu), MainMenu, nullptr, nullptr);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu);
    w->detailsToggle = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(w->detailsToggle), "sidebar-show-right-symbolic");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(w->detailsToggle), "win.show-details");
    gtk_widget_set_tooltip_text(w->detailsToggle, "Details (Ctrl+Alt+I)");
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), w->detailsToggle);
    w->more = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(w->more), "view-more-symbolic");
    gtk_widget_set_tooltip_text(w->more, "More Actions");
    gtk_menu_button_set_create_popup_func(GTK_MENU_BUTTON(w->more), MoreMenu, w->window, nullptr);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), w->more);

    w->search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(w->search), "Search Commits");
    gtk_widget_set_size_request(w->search, 220, -1);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), w->search);
    g_signal_connect(w->search, "search-changed", G_CALLBACK(+[](GtkSearchEntry* e, gpointer data) {
        MainWin* mw = static_cast<MainWin*>(data);
        if (mw->syncingSearch) return;
        if (RepoSession* s = CurrentSession(mw->window)) s->SetSearch(Widen(gtk_editable_get_text(GTK_EDITABLE(e))));
    }), w);
    g_signal_connect(w->search, "activate", G_CALLBACK(+[](GtkSearchEntry*, gpointer data) {
        if (RepoTab* t = Current(static_cast<MainWin*>(data)->window)) t->SearchStep(1);
    }), w);
    g_signal_connect(w->search, "next-match", G_CALLBACK(+[](GtkSearchEntry*, gpointer data) {
        if (RepoTab* t = Current(static_cast<MainWin*>(data)->window)) t->SearchStep(1);
    }), w);
    g_signal_connect(w->search, "previous-match", G_CALLBACK(+[](GtkSearchEntry*, gpointer data) {
        if (RepoTab* t = Current(static_cast<MainWin*>(data)->window)) t->SearchStep(-1);
    }), w);
    g_signal_connect(w->search, "stop-search", G_CALLBACK(+[](GtkSearchEntry* e, gpointer data) {
        gtk_editable_set_text(GTK_EDITABLE(e), "");
        if (RepoTab* t = Current(static_cast<MainWin*>(data)->window)) history::Focus(t->history);
    }), w);
    // Shift+Enter goes to the previous match; the arrows move through the history
    GtkEventController* keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint key, guint, GdkModifierType state, gpointer data) -> gboolean {
        RepoTab* t = Current(static_cast<MainWin*>(data)->window);
        if (!t) return FALSE;
        if ((key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) && (state & GDK_SHIFT_MASK)) {
            t->SearchStep(-1);
            return TRUE;
        }
        if (key == GDK_KEY_Up || key == GDK_KEY_Down) {
            t->MoveSelection(key == GDK_KEY_Down ? 1 : -1);
            return TRUE;
        }
        return FALSE;
    }), w);
    gtk_widget_add_controller(w->search, keys);
}

}  // namespace

RepoTab* CurrentTab(GtkWidget* window) { return Current(window); }

std::vector<RepoTab*> TabsOf(GtkWidget* window) {
    std::vector<RepoTab*> out;
    MainWin* w = Win(window);
    if (!w) return out;
    const int n = adw_tab_view_get_n_pages(w->tabs);
    for (int i = 0; i < n; ++i)
        if (RepoTab* t = TabOf(adw_tab_page_get_child(adw_tab_view_get_nth_page(w->tabs, i)))) out.push_back(t);
    return out;
}

std::vector<GtkWidget*> MainWindows() {
    std::vector<GtkWidget*> out;
    GApplication* app = g_application_get_default();
    if (!app) return out;
    for (GList* l = gtk_application_get_windows(GTK_APPLICATION(app)); l; l = l->next)  // the most recently active first
        if (Win(GTK_WIDGET(l->data)) && !Win(GTK_WIDGET(l->data))->closing) out.push_back(GTK_WIDGET(l->data));
    return out;
}

GtkWidget* ActiveMainWindow() {
    const std::vector<GtkWidget*> all = MainWindows();
    return all.empty() ? nullptr : all.front();
}

void UpdateWindow(GtkWidget* window) {
    MainWin* w = Win(window);
    if (!w || w->closing) return;
    RepoTab* t = Current(window);
    RepoSession* s = t ? t->session() : nullptr;

    // the tabs
    const int n = adw_tab_view_get_n_pages(w->tabs);
    for (int i = 0; i < n; ++i) {
        AdwTabPage* page = adw_tab_view_get_nth_page(w->tabs, i);
        RepoTab* tab = TabOf(adw_tab_page_get_child(page));
        if (!tab) continue;
        const std::string title = U8(tab->Title());
        if (title != (adw_tab_page_get_title(page) ? adw_tab_page_get_title(page) : "")) adw_tab_page_set_title(page, title.c_str());
        adw_tab_page_set_tooltip(page, tab->session() ? Esc(U8(tab->session()->path)).c_str() : "");
        adw_tab_page_set_loading(page, tab->Busy());
    }

    // the title
    const std::string title = t ? U8(t->Title()) : "Mangrove";
    adw_window_title_set_title(ADW_WINDOW_TITLE(w->title), title.c_str());
    adw_window_title_set_subtitle(ADW_WINDOW_TITLE(w->title), t ? U8(t->Subtitle()).c_str() : "");
    gtk_window_set_title(GTK_WINDOW(window), s ? (title + " \u2013 Mangrove").c_str() : "Mangrove");
    const bool repo = s && s->data;
    gtk_widget_set_visible(w->repoButtons, s != nullptr);
    gtk_widget_set_visible(w->search, s != nullptr);
    gtk_widget_set_visible(w->more, s != nullptr);
    gtk_widget_set_visible(w->sideToggle, s != nullptr);
    gtk_widget_set_visible(w->detailsToggle, s != nullptr);
    const bool busy = t && t->Busy();
    gtk_widget_set_visible(w->spinner, busy);
    if (busy) gtk_spinner_start(GTK_SPINNER(w->spinner));
    else gtk_spinner_stop(GTK_SPINNER(w->spinner));

    // the actions, with what they would do (or why they can't) in their tooltips
    const struct {
        const char* action;
        int act;
        GtkWidget* button;
    } acts[] = {
        { "fetch", ActFetch, w->fetch }, { "pull", ActPull, w->pull }, { "push", ActPush, w->push }, { "new-branch", ActBranch, w->branch },
        { "stash", ActStash, w->stash }, { "pop", ActPop, w->pop }, { "undo-git", ActUndo, nullptr }, { "redo-git", ActRedo, nullptr },
        { "terminal", ActTerminal, nullptr },
    };
    for (const auto& a : acts) {
        std::wstring tip;
        const bool ok = s && s->CanRun(a.act, &tip);
        g_simple_action_set_enabled(WinAction(window, a.action), ok);
        if (a.button && !tip.empty()) gtk_widget_set_tooltip_text(a.button, U8(tip).c_str());
    }
    g_simple_action_set_enabled(WinAction(window, "refresh"), s && !s->IsStart());
    for (const char* name : { "reveal", "copy-path", "head-only", "find", "history-page" }) g_simple_action_set_enabled(WinAction(window, name), repo);
    for (const char* name : { "find-next", "find-prev" }) g_simple_action_set_enabled(WinAction(window, name), repo && s->Searching() && s->searchCount > 0);
    g_simple_action_set_enabled(WinAction(window, "changes-page"), repo && s->graph && s->graph->hasWip);
    for (const char* name : { "show-sidebar", "show-details", "diff-layout", "pull-mode" }) g_simple_action_set_enabled(WinAction(window, name), s != nullptr);
    g_simple_action_set_state(WinAction(window, "head-only"), g_variant_new_boolean(s && s->headOnly));
    g_simple_action_set_state(WinAction(window, "show-sidebar"), g_variant_new_boolean(!t || t->SidebarShown()));
    g_simple_action_set_state(WinAction(window, "show-details"), g_variant_new_boolean(!t || t->DetailsShown()));
    g_simple_action_set_state(WinAction(window, "diff-layout"), g_variant_new_string(AppSettings().diffSplit ? "split" : "unified"));
    g_simple_action_set_state(WinAction(window, "pull-mode"), g_variant_new_int32(std::clamp(AppSettings().pullMode, 0, 2)));

    // Pull and Push show how many commits they would bring in or send
    const Ref* head = repo ? HeadRef(*s->data) : nullptr;
    const int behind = head ? head->behind : 0, ahead = head ? head->ahead : 0;
    gtk_label_set_text(GTK_LABEL(w->pullBadge), std::to_string(behind).c_str());
    gtk_widget_set_visible(w->pullBadge, behind > 0);
    gtk_label_set_text(GTK_LABEL(w->pushBadge), std::to_string(ahead).c_str());
    gtk_widget_set_visible(w->pushBadge, ahead > 0);

    // the search text of this tab
    const std::string text = s ? U8(s->searchText) : "";
    if (!gtk_widget_has_focus(w->search) && text != gtk_editable_get_text(GTK_EDITABLE(w->search))) {
        w->syncingSearch = true;
        gtk_editable_set_text(GTK_EDITABLE(w->search), text.c_str());
        w->syncingSearch = false;
    }
}

GtkWidget* NewMainWindow(GtkApplication* app) {
    GtkWidget* window = adw_application_window_new(app);
    MainWin* w = new MainWin();
    w->window = window;
    g_object_set_data_full(G_OBJECT(window), "mg-win", w, [](gpointer p) { delete static_cast<MainWin*>(p); });
    const Settings& st = AppSettings();
    gtk_window_set_default_size(GTK_WINDOW(window), std::max(800, st.GetInt(L"linuxWidth", 1240)), std::max(500, st.GetInt(L"linuxHeight", 780)));
    if (st.GetBool(L"linuxMaximized", false)) gtk_window_maximize(GTK_WINDOW(window));
    gtk_widget_set_size_request(window, 800, 500);
    gtk_window_set_title(GTK_WINDOW(window), "Mangrove");
    g_action_map_add_action_entries(G_ACTION_MAP(window), kWinActions, G_N_ELEMENTS(kWinActions), window);

    BuildHeader(w);
    w->tabs = adw_tab_view_new();
    w->tabBar = adw_tab_bar_new();
    adw_tab_bar_set_view(w->tabBar, w->tabs);
    adw_tab_bar_set_autohide(w->tabBar, TRUE);
    GtkWidget* view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), w->header);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(w->tabBar));
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(w->tabs));
    adw_toolbar_view_set_top_bar_style(ADW_TOOLBAR_VIEW(view), ADW_TOOLBAR_RAISED);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), view);

    g_signal_connect(w->tabs, "notify::selected-page", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
        MainWin* mw = Win(GTK_WIDGET(data));
        if (!mw || mw->closing) return;  // its tabs go away with it: they stay saved
        UpdateWindow(GTK_WIDGET(data));
        SaveTabs();
    }), window);
    g_signal_connect(w->tabs, "page-attached", G_CALLBACK(+[](AdwTabView*, AdwTabPage*, int, gpointer data) { UpdateWindow(GTK_WIDGET(data)); }), window);
    g_signal_connect(w->tabs, "notify::n-pages", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer data) {
        MainWin* mw = Win(GTK_WIDGET(data));
        if (!mw || mw->closing) return;
        if (adw_tab_view_get_n_pages(ADW_TAB_VIEW(o)) == 0) gtk_window_close(GTK_WINDOW(data));  // its last tab closed or moved away
        else SaveTabs();
    }), window);
    g_signal_connect(w->tabs, "create-window", G_CALLBACK(+[](AdwTabView*, gpointer data) -> AdwTabView* {
        GtkWidget* nw = NewMainWindow(gtk_window_get_application(GTK_WINDOW(data)));
        gtk_window_present(GTK_WINDOW(nw));
        return Win(nw)->tabs;
    }), window);
    g_signal_connect(window, "close-request", G_CALLBACK(+[](GtkWindow* win, gpointer) -> gboolean {
        MainWin* mw = Win(GTK_WIDGET(win));
        SaveTabs(GTK_WIDGET(win));
        mw->closing = true;
        Settings& st = AppSettings();
        const bool max = gtk_window_is_maximized(win);
        st.SetBool(L"linuxMaximized", max);
        if (!max) {
            int width = 0;
            int height = 0;
            gtk_window_get_default_size(win, &width, &height);
            if (width > 0 && height > 0) {
                st.SetInt(L"linuxWidth", width);
                st.SetInt(L"linuxHeight", height);
            }
        }
        st.Save();
        return FALSE;
    }), nullptr);
    // Esc: drops the selected lines, then closes the diff (when nothing else used the key)
    GtkEventController* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint key, guint, GdkModifierType, gpointer data) -> gboolean {
        if (key != GDK_KEY_Escape) return FALSE;
        RepoTab* t = Current(GTK_WIDGET(data));
        if (!t || !t->session() || !t->session()->diff.open) return FALSE;
        t->Escape();
        return TRUE;
    }), window);
    gtk_widget_add_controller(window, keys);
    return window;
}

RepoTab* AddTab(GtkWidget* window, const std::wstring& path, bool select) {
    MainWin* w = Win(window);
    if (!w) return nullptr;
    RepoTab* t = new RepoTab(path);
    AdwTabPage* page = adw_tab_view_append(w->tabs, t->root);
    adw_tab_page_set_title(page, U8(t->Title()).c_str());
    if (select) adw_tab_view_set_selected_page(w->tabs, page);
    UpdateWindow(window);
    return t;
}

void SelectTab(GtkWidget* window, RepoTab* tab) {
    MainWin* w = Win(window);
    if (!w || !tab) return;
    if (AdwTabPage* page = adw_tab_view_get_page(w->tabs, tab->root)) adw_tab_view_set_selected_page(w->tabs, page);
}

void OpenRepository(const std::wstring& rawPath, GtkWidget* window) {
    const std::wstring path = Canonical(rawPath);
    for (GtkWidget* win : MainWindows()) {  // already open: its tab comes to the front
        MainWin* w = Win(win);
        const int n = adw_tab_view_get_n_pages(w->tabs);
        for (int i = 0; i < n; ++i) {
            AdwTabPage* page = adw_tab_view_get_nth_page(w->tabs, i);
            RepoTab* t = TabOf(adw_tab_page_get_child(page));
            RepoSession* s = t ? t->session() : nullptr;
            if (s && (s->path == path || (s->data && s->data->root == path))) {
                adw_tab_view_set_selected_page(w->tabs, page);
                gtk_window_present(GTK_WINDOW(win));
                return;
            }
        }
    }
    if (!window || !Win(window)) window = ActiveMainWindow();
    if (!window) {
        window = NewMainWindow(GTK_APPLICATION(g_application_get_default()));
    }
    RepoTab* cur = Current(window);
    if (cur && !cur->session()) {  // the start page becomes the repository
        cur->Open(path);
        UpdateWindow(window);
    } else {
        AddTab(window, path, true);
    }
    gtk_window_present(GTK_WINDOW(window));
}

void ChooseRepository(GtkWidget* window) {
    GtkFileDialog* d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Open Repository");
    gtk_file_dialog_set_accept_label(d, "Open");
    gtk_file_dialog_select_multiple_folders(
        d, window ? GTK_WINDOW(window) : nullptr, nullptr,
        [](GObject* src, GAsyncResult* res, gpointer data) {
            GtkWidget* win = data ? GTK_WIDGET(data) : nullptr;
            GListModel* files = gtk_file_dialog_select_multiple_folders_finish(GTK_FILE_DIALOG(src), res, nullptr);
            if (files) {
                const guint n = g_list_model_get_n_items(files);
                for (guint i = 0; i < n; ++i) {
                    GFile* f = G_FILE(g_list_model_get_item(files, i));
                    if (char* p = g_file_get_path(f)) {
                        OpenRepository(Widen(p), win);
                        g_free(p);
                    }
                    g_object_unref(f);
                }
                g_object_unref(files);
            }
            if (win) g_object_unref(win);
        },
        window ? g_object_ref(window) : nullptr);
    g_object_unref(d);
}

// The repositories to reopen: the tabs of every window, the most recently active window first, with the selected
// tab of that one as the active tab. A closing window's tabs only count when it is the last one.
void SaveTabs(GtkWidget* closing) {
    Settings& st = AppSettings();
    std::vector<GtkWidget*> wins = MainWindows();
    if (closing && !(wins.size() == 1 && wins[0] == closing)) wins.erase(std::remove(wins.begin(), wins.end(), closing), wins.end());
    if (wins.empty()) return;  // quitting: the tabs were saved before the windows started to close
    st.tabs.clear();
    st.activeTab = 0;
    for (size_t k = 0; k < wins.size(); ++k) {
        RepoTab* cur = Current(wins[k]);
        for (RepoTab* t : TabsOf(wins[k])) {
            if (!t->session()) continue;
            if (k == 0 && t == cur) st.activeTab = (int)st.tabs.size();
            st.tabs.push_back(t->session()->path);
        }
    }
    st.Save();
}

void PrepareQuit() {
    SaveTabs();
    for (GtkWidget* w : MainWindows()) Win(w)->closing = true;  // what the windows do as they go is not saved
}

void BroadcastSettingsChanged() {
    for (GtkWidget* w : MainWindows()) {
        for (RepoTab* t : TabsOf(w)) t->SettingsChanged();
        UpdateWindow(w);
    }
}
