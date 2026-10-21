// The details of the selection: a commit (message, author, actions, changed files), or the uncommitted changes
// (unstaged and staged files, the commit message and the commit button).

#include "LinuxUI.h"

namespace {

// The author's initials on their color.
class AvatarView : public Canvas {
public:
    Author author;
    void Draw(GtkSnapshot* s, float w, float h) override {
        if (!author.initials.empty() || author.color) draw::Avatar(s, widget, 0, 0, std::min(w, h), author.initials, author.color);
    }
};

GtkWidget* Box(GtkOrientation o, int spacing, const char* cssClass = nullptr) {
    GtkWidget* b = gtk_box_new(o, spacing);
    if (cssClass) AddClass(b, cssClass);
    return b;
}

GtkWidget* Spacer() {
    GtkWidget* s = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(s, TRUE);
    return s;
}

void Margins(GtkWidget* w, int top, int side, int bottom) {
    gtk_widget_set_margin_top(w, top);
    gtk_widget_set_margin_bottom(w, bottom);
    gtk_widget_set_margin_start(w, side);
    gtk_widget_set_margin_end(w, side);
}

GtkWidget* WrapLabel(const char* cssClass) {
    GtkWidget* l = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    if (cssClass) AddClass(l, cssClass);
    return l;
}

}  // namespace

class DetailsPane {
public:
    explicit DetailsPane(RepoTab* o) : owner(o) {}
    ~DetailsPane() {
        if (owner && owner->details == this) owner->details = nullptr;
    }
    RepoTab* owner;
    GtkWidget* root = nullptr;  // a stack: empty, commit, wip
    GtkWidget* empty = nullptr;
    bool tree = false;
    bool syncing = false;

    // a commit
    GtkWidget* kind = nullptr;
    GtkWidget* idButton = nullptr;
    GtkWidget* idLabel = nullptr;
    GtkWidget* more = nullptr;
    GtkWidget* summary = nullptr;
    GtkWidget* bodyScroll = nullptr;
    GtkWidget* body = nullptr;
    AvatarView* avatar = nullptr;
    GtkWidget* authorName = nullptr;
    GtkWidget* dateLine = nullptr;
    GtkWidget* committer = nullptr;
    GtkWidget* act[3] = {};
    GtkWidget* filesStats = nullptr;
    GtkWidget* commitTree = nullptr;
    GtkWidget* filesNote = nullptr;
    FileList* commitFiles = nullptr;

    // the uncommitted changes
    GtkWidget* branch = nullptr;
    GtkWidget* wipCount = nullptr;
    GtkWidget* wipTree = nullptr;
    FileList* wipFiles = nullptr;
    GtkWidget* amend = nullptr;
    GtkWidget* summaryEntry = nullptr;
    GtkWidget* left = nullptr;
    GtkWidget* bodyView = nullptr;
    GtkWidget* bodyPlaceholder = nullptr;
    GtkWidget* commitButton = nullptr;
    std::string amendPrefill;  // the message put in when amend was turned on

    void Build();
    GtkWidget* BuildCommit();
    GtkWidget* BuildWip();
    void Reload();
    void ReloadCommit();
    void ReloadWip();
    void RefreshState();
    std::string BodyText() const {
        GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(bodyView));
        GtkTextIter s, e;
        gtk_text_buffer_get_bounds(b, &s, &e);
        char* t = gtk_text_buffer_get_text(b, &s, &e, FALSE);
        std::string r = t ? t : "";
        g_free(t);
        return r;
    }
    void SetBody(const std::string& text) { gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(bodyView)), text.c_str(), -1); }
    std::string SummaryText() const { return gtk_editable_get_text(GTK_EDITABLE(summaryEntry)); }
    bool Amending() const { return gtk_check_button_get_active(GTK_CHECK_BUTTON(amend)); }
    void DoCommit();
    void AmendToggled();
    void Action(int k);
    void SetTree(bool t);
};

// ---------------------------------------------------------------- building

GtkWidget* DetailsPane::BuildCommit() {
    GtkWidget* page = Box(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget* info = Box(GTK_ORIENTATION_VERTICAL, 10);
    Margins(info, 14, 16, 0);

    GtkWidget* top = Box(GTK_ORIENTATION_HORIZONTAL, 8);
    kind = MakeLabel("Commit", "caption dim-label");
    idButton = gtk_button_new();
    GtkWidget* idBox = Box(GTK_ORIENTATION_HORIZONTAL, 6);
    idLabel = MakeLabel("", "monospace caption");
    gtk_box_append(GTK_BOX(idBox), idLabel);
    gtk_box_append(GTK_BOX(idBox), gtk_image_new_from_icon_name("edit-copy-symbolic"));
    gtk_button_set_child(GTK_BUTTON(idButton), idBox);
    AddClass(idButton, "flat mg-small");
    gtk_widget_set_tooltip_text(idButton, "Copy the commit ID");
    g_signal_connect(idButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        DetailsPane* d = static_cast<DetailsPane*>(data);
        RepoSession* s = d->owner->session();
        if (s && s->data && s->selCommit >= 0 && s->selCommit < (int32_t)s->data->commits.size())
            d->owner->Copy(U8(s->data->commits[(size_t)s->selCommit].id.Hex()));
    }), this);
    more = gtk_button_new_from_icon_name("view-more-symbolic");
    AddClass(more, "flat circular");
    gtk_widget_set_tooltip_text(more, "More actions");
    g_signal_connect(more, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer data) {
        DetailsPane* d = static_cast<DetailsPane*>(data);
        RepoSession* s = d->owner->session();
        if (!s) return;
        Menu m;
        d->owner->FillCommitMenu(m, s->selCommit);
        m.PopupUnder(GTK_WIDGET(b));
    }), this);
    gtk_box_append(GTK_BOX(top), kind);
    gtk_box_append(GTK_BOX(top), idButton);
    gtk_box_append(GTK_BOX(top), Spacer());
    gtk_box_append(GTK_BOX(top), more);
    gtk_box_append(GTK_BOX(info), top);

    summary = WrapLabel("title-4");
    gtk_box_append(GTK_BOX(info), summary);

    body = WrapLabel("dim-label");
    gtk_label_set_yalign(GTK_LABEL(body), 0);
    bodyScroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(bodyScroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(bodyScroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(bodyScroll), 170);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(bodyScroll), body);
    gtk_box_append(GTK_BOX(info), bodyScroll);

    GtkWidget* author = Box(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_top(author, 4);
    avatar = new AvatarView();
    GtkWidget* av = MakeCanvas(avatar, Scroll::None);
    gtk_widget_set_size_request(av, 32, 32);
    gtk_widget_set_valign(av, GTK_ALIGN_CENTER);
    GtkWidget* who = Box(GTK_ORIENTATION_VERTICAL, 2);
    authorName = MakeLabel("", "heading");
    dateLine = MakeLabel("", "caption dim-label");
    g_signal_connect(dateLine, "activate-link", G_CALLBACK(+[](GtkLabel*, const char* uri, gpointer data) -> gboolean {
        if (!g_str_has_prefix(uri, "commit:")) return FALSE;
        static_cast<DetailsPane*>(data)->owner->SelectCommit((int32_t)atoi(uri + 7));
        return TRUE;
    }), this);
    gtk_box_append(GTK_BOX(who), authorName);
    gtk_box_append(GTK_BOX(who), dateLine);
    gtk_box_append(GTK_BOX(author), av);
    gtk_box_append(GTK_BOX(author), who);
    gtk_box_append(GTK_BOX(info), author);

    committer = MakeLabel("", "caption dim-label");
    gtk_box_append(GTK_BOX(info), committer);

    GtkWidget* actions = Box(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(actions, 4);
    for (int k = 0; k < 3; ++k) {
        act[k] = gtk_button_new_with_label("");
        g_object_set_data(G_OBJECT(act[k]), "mg-k", GINT_TO_POINTER(k));
        g_signal_connect(act[k], "clicked", G_CALLBACK(+[](GtkButton* b, gpointer data) {
            static_cast<DetailsPane*>(data)->Action(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "mg-k")));
        }), this);
        gtk_box_append(GTK_BOX(actions), act[k]);
    }
    gtk_box_append(GTK_BOX(info), actions);

    GtkWidget* line = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(line, 8);
    gtk_box_append(GTK_BOX(info), line);

    GtkWidget* filesHeader = Box(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(filesHeader, 2);
    gtk_box_append(GTK_BOX(filesHeader), MakeLabel("Changed Files", "heading"));
    gtk_box_append(GTK_BOX(filesHeader), Spacer());
    filesStats = MakeLabel("", "caption dim-label");
    gtk_box_append(GTK_BOX(filesHeader), filesStats);
    commitTree = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(commitTree), "format-indent-more-symbolic");
    AddClass(commitTree, "flat mg-small");
    gtk_box_append(GTK_BOX(filesHeader), commitTree);
    gtk_box_append(GTK_BOX(info), filesHeader);
    gtk_box_append(GTK_BOX(page), info);

    filesNote = MakeLabel("", "dim-label");
    Margins(filesNote, 10, 16, 0);
    gtk_box_append(GTK_BOX(page), filesNote);
    GtkWidget* list = MakeFileList(owner, false, &commitFiles);
    Margins(list, 4, 8, 0);
    gtk_box_append(GTK_BOX(page), list);
    return page;
}

GtkWidget* DetailsPane::BuildWip() {
    GtkWidget* page = Box(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget* head = Box(GTK_ORIENTATION_VERTICAL, 6);
    Margins(head, 14, 16, 0);
    GtkWidget* top = Box(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(top), MakeLabel("Working tree", "caption dim-label"));
    GtkWidget* icon = gtk_image_new_from_icon_name("mg-branch-symbolic");
    AddClass(icon, "accent");
    gtk_box_append(GTK_BOX(top), icon);
    branch = MakeLabel("", "mg-medium");
    gtk_box_append(GTK_BOX(top), branch);
    gtk_box_append(GTK_BOX(head), top);
    GtkWidget* title = MakeLabel("Uncommitted Changes", "title-4");
    gtk_widget_set_margin_top(title, 6);
    gtk_box_append(GTK_BOX(head), title);
    GtkWidget* countRow = Box(GTK_ORIENTATION_HORIZONTAL, 8);
    wipCount = MakeLabel("", "caption dim-label");
    gtk_box_append(GTK_BOX(countRow), wipCount);
    gtk_box_append(GTK_BOX(countRow), Spacer());
    wipTree = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(wipTree), "format-indent-more-symbolic");
    AddClass(wipTree, "flat mg-small");
    gtk_box_append(GTK_BOX(countRow), wipTree);
    gtk_box_append(GTK_BOX(head), countRow);
    gtk_box_append(GTK_BOX(page), head);

    GtkWidget* list = MakeFileList(owner, true, &wipFiles);
    Margins(list, 6, 8, 0);
    gtk_box_append(GTK_BOX(page), list);

    // the commit message
    gtk_box_append(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    GtkWidget* box = Box(GTK_ORIENTATION_VERTICAL, 10);
    Margins(box, 12, 16, 16);
    amend = gtk_check_button_new_with_label("Amend previous commit");
    g_signal_connect(amend, "toggled", G_CALLBACK(+[](GtkCheckButton*, gpointer data) { static_cast<DetailsPane*>(data)->AmendToggled(); }), this);
    gtk_box_append(GTK_BOX(box), amend);
    GtkWidget* summaryRow = Box(GTK_ORIENTATION_HORIZONTAL, 8);
    summaryEntry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(summaryEntry), "Summary (required)");
    gtk_widget_set_hexpand(summaryEntry, TRUE);
    g_signal_connect(summaryEntry, "changed", G_CALLBACK(+[](GtkEditable*, gpointer data) { static_cast<DetailsPane*>(data)->RefreshState(); }), this);
    g_signal_connect(summaryEntry, "activate", G_CALLBACK(+[](GtkEntry*, gpointer data) {  // Enter moves to the description
        gtk_widget_grab_focus(static_cast<DetailsPane*>(data)->bodyView);
    }), this);
    left = MakeLabel("72", "caption dim-label numeric");
    gtk_box_append(GTK_BOX(summaryRow), summaryEntry);
    gtk_box_append(GTK_BOX(summaryRow), left);
    gtk_box_append(GTK_BOX(box), summaryRow);

    bodyView = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(bodyView), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(bodyView), FALSE);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(bodyView), 7);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(bodyView), 7);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(bodyView), 9);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(bodyView), 9);
    g_signal_connect(gtk_text_view_get_buffer(GTK_TEXT_VIEW(bodyView)), "changed",
                     G_CALLBACK(+[](GtkTextBuffer*, gpointer data) { static_cast<DetailsPane*>(data)->RefreshState(); }), this);
    GtkWidget* bodyScroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(bodyScroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(bodyScroller), bodyView);
    gtk_widget_set_size_request(bodyScroller, -1, 90);
    AddClass(bodyScroller, "mg-textbox");
    GtkWidget* overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), bodyScroller);
    bodyPlaceholder = MakeLabel("Description", "dim-label");
    gtk_widget_set_can_target(bodyPlaceholder, FALSE);
    gtk_widget_set_halign(bodyPlaceholder, GTK_ALIGN_START);
    gtk_widget_set_valign(bodyPlaceholder, GTK_ALIGN_START);
    gtk_widget_set_margin_start(bodyPlaceholder, 10);
    gtk_widget_set_margin_top(bodyPlaceholder, 7);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), bodyPlaceholder);
    gtk_box_append(GTK_BOX(box), overlay);

    commitButton = gtk_button_new_with_label("Commit");
    AddClass(commitButton, "suggested-action");
    gtk_widget_set_tooltip_text(commitButton, "Commit (Ctrl+Enter)");
    g_signal_connect(commitButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<DetailsPane*>(data)->DoCommit(); }), this);
    gtk_box_append(GTK_BOX(box), commitButton);
    gtk_box_append(GTK_BOX(page), box);

    // Ctrl+Enter commits, from the summary or from the description
    GtkEventController* keys = gtk_shortcut_controller_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    for (const char* trigger : { "<Control>Return", "<Control>KP_Enter" }) {
        GtkShortcutAction* a = gtk_callback_action_new(
            [](GtkWidget*, GVariant*, gpointer data) -> gboolean {
                static_cast<DetailsPane*>(data)->DoCommit();
                return TRUE;
            },
            this, nullptr);
        gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(keys), gtk_shortcut_new(gtk_shortcut_trigger_parse_string(trigger), a));
    }
    gtk_widget_add_controller(box, keys);
    return page;
}

void DetailsPane::Build() {
    root = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(root), GTK_STACK_TRANSITION_TYPE_NONE);
    empty = gtk_label_new("");
    AddClass(empty, "dim-label");
    gtk_stack_add_named(GTK_STACK(root), empty, "empty");
    gtk_stack_add_named(GTK_STACK(root), BuildCommit(), "commit");
    gtk_stack_add_named(GTK_STACK(root), BuildWip(), "wip");
    for (GtkWidget* t : { commitTree, wipTree })
        g_signal_connect(t, "toggled", G_CALLBACK(+[](GtkToggleButton* b, gpointer data) {
            DetailsPane* d = static_cast<DetailsPane*>(data);
            if (!d->syncing) d->SetTree(gtk_toggle_button_get_active(b));
        }), this);
    SetTree(false);
}

// ---------------------------------------------------------------- reload

void DetailsPane::SetTree(bool t) {
    tree = t;
    syncing = true;
    for (GtkWidget* b : { commitTree, wipTree }) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), t);
        gtk_widget_set_tooltip_text(b, t ? "Show as a list" : "Show as a tree");
    }
    syncing = false;
    files::SetTree(commitFiles, t);
    files::SetTree(wipFiles, t);
}

void DetailsPane::Reload() {
    RepoSession* s = owner->session();
    const bool wip = s && s->data && s->selWip && s->graph && s->graph->hasWip;
    const bool commit = s && s->data && !wip && !s->selWip && s->selCommit >= 0 && s->selCommit < (int32_t)s->data->commits.size();
    gtk_label_set_text(GTK_LABEL(empty), s && s->data ? "Select a commit to see its details" : "");
    gtk_stack_set_visible_child_name(GTK_STACK(root), wip ? "wip" : commit ? "commit" : "empty");
    if (commit) ReloadCommit();
    if (wip) ReloadWip();
    RefreshState();
}

void DetailsPane::ReloadCommit() {
    RepoSession* s = owner->session();
    const RepoData& d = *s->data;
    const Commit& cm = d.commits[(size_t)s->selCommit];
    const std::shared_ptr<CommitDetails> det = s->details;
    const bool ok = det && det->ok && det->id == cm.id;
    const bool stash = (cm.flags & CF_STASH) && cm.stash >= 0 && cm.stash < (int32_t)d.stashes.size();

    gtk_label_set_text(GTK_LABEL(kind), stash ? "Stash" : "Commit");
    gtk_label_set_text(GTK_LABEL(idLabel), U8(stash ? d.stashes[(size_t)cm.stash].name : cm.id.Hex(7)).c_str());
    gtk_label_set_text(GTK_LABEL(summary), U8(stash ? d.stashes[(size_t)cm.stash].message : ok ? det->summary : CommitSubject(d, cm)).c_str());
    const std::string bodyText = ok && !stash ? U8(det->body) : "";
    if (bodyText != gtk_label_get_text(GTK_LABEL(body))) gtk_label_set_text(GTK_LABEL(body), bodyText.c_str());
    gtk_widget_set_visible(bodyScroll, !bodyText.empty());

    const Author& au = d.authors[cm.author];
    avatar->author = au;
    avatar->Redraw();
    gtk_label_set_text(GTK_LABEL(authorName), U8(au.name).c_str());
    gtk_widget_set_tooltip_text(authorName, U8(au.email).c_str());
    std::string line = Esc(U8(FormatDateLong(cm.authorTime)));
    line += cm.parentCount == 0 ? " \u00B7 root commit" : cm.parentCount > 1 ? " \u00B7 parents" : " \u00B7 parent";
    const int32_t* ps = d.Parents(cm);
    for (int k = 0; k < (int)cm.parentCount && k < 8; ++k) {
        if (ps[k] >= 0) {
            const Commit& p = d.commits[(size_t)ps[k]];
            line += " <a href=\"commit:" + std::to_string(ps[k]) + "\" title=\"" + Esc(U8(CommitSubject(d, p))) + "\"><tt>" + U8(p.id.Hex(7)) + "</tt></a>";
        } else {
            const std::string hex = ok && k < (int)det->parents.size() ? U8(det->parents[(size_t)k].Hex(7)) : "???????";
            line += " <tt>" + hex + "</tt>";
        }
    }
    gtk_label_set_markup(GTK_LABEL(dateLine), line.c_str());
    const std::wstring by = ok ? CommitterNote(d, cm, *det) : std::wstring();
    gtk_label_set_text(GTK_LABEL(committer), U8(by).c_str());
    gtk_widget_set_visible(committer, !by.empty());

    static const char* const kStash[] = { "Pop", "Apply", "Delete\u2026" };
    static const char* const kCommit[] = { "Check Out", "New Branch\u2026", "Revert\u2026" };
    for (int k = 0; k < 3; ++k) gtk_button_set_label(GTK_BUTTON(act[k]), stash ? kStash[k] : kCommit[k]);

    // changed files
    gtk_label_set_text(GTK_LABEL(filesStats), ok ? U8(FilesSummary(*det)).c_str() : "");
    std::string note;
    bool error = false;
    if (!ok) {
        error = det && !det->ok && det->id == cm.id;
        note = error ? U8(det->error) : "Loading changes\u2026";
    } else if (det->files.empty()) {
        note = "No file changes";
    }
    gtk_label_set_text(GTK_LABEL(filesNote), note.c_str());
    SetClass(filesNote, "error", error);
    gtk_widget_set_visible(filesNote, !note.empty());
    files::Reload(commitFiles, false);
}

void DetailsPane::ReloadWip() {
    RepoSession* s = owner->session();
    const RepoData& d = *s->data;
    const std::shared_ptr<WorkStatus> st = s->status;
    gtk_label_set_text(GTK_LABEL(branch), d.headBranch.empty() ? "detached HEAD" : U8(d.headBranch).c_str());
    gtk_label_set_text(GTK_LABEL(wipCount), st ? U8(Plural((size_t)st->ChangedFiles(), L"file changed", L"files changed")).c_str()
                                               : "Reading the working tree\u2026");
    gtk_widget_set_sensitive(amend, !d.unborn);
    files::Reload(wipFiles, false);
}

// ---- the state of the buttons

void DetailsPane::RefreshState() {
    RepoSession* s = owner->session();
    if (!s || !s->data) return;
    const RepoData& d = *s->data;
    const bool busy = s->opBusy;
    const char* page = gtk_stack_get_visible_child_name(GTK_STACK(root));
    if (page && !strcmp(page, "commit") && s->selCommit >= 0 && s->selCommit < (int32_t)d.commits.size()) {
        const Commit& cm = d.commits[(size_t)s->selCommit];
        if (cm.flags & CF_STASH) {
            static const char* const tips[] = { "Apply the stash to the working tree and delete it", "Apply the stash and keep it", "Delete the stash" };
            for (int k = 0; k < 3; ++k) {
                gtk_widget_set_sensitive(act[k], !busy);
                gtk_widget_set_tooltip_text(act[k], tips[k]);
            }
        } else {
            std::wstring tip, branchTip;
            gtk_widget_set_sensitive(act[0], s->CanCheckout(s->selCommit, &tip));
            gtk_widget_set_tooltip_text(act[0], U8(tip).c_str());
            gtk_widget_set_sensitive(act[1], s->CanRun(ActBranch, &branchTip));
            gtk_widget_set_tooltip_text(act[1], "Create a branch at this commit");
            const std::wstring why = s->CanRevert(s->selCommit);
            gtk_widget_set_sensitive(act[2], why.empty());
            gtk_widget_set_tooltip_text(act[2], why.empty() ? "Create a commit that undoes this one" : U8(why).c_str());
        }
    }
    if (page && !strcmp(page, "wip")) {
        const size_t staged = s->status ? s->status->staged.size() : 0;
        const bool amending = Amending() && !d.unborn;
        const bool committing = busy && s->opButton == kCommitOp;
        const std::string sum = SummaryText();
        const bool hasSummary = !TrimW(Widen(sum)).empty();
        gtk_button_set_label(GTK_BUTTON(commitButton),
                             committing ? (amending ? "Amending\u2026" : "Committing\u2026") : U8(CommitButtonLabel(staged, amending)).c_str());
        gtk_widget_set_sensitive(commitButton, !busy && hasSummary && (amending || staged > 0));
        const int remaining = 72 - (int)g_utf8_strlen(sum.c_str(), -1);
        gtk_label_set_text(GTK_LABEL(left), std::to_string(remaining).c_str());
        SetClass(left, "error", remaining < 0);
        SetClass(left, "warning", remaining >= 0 && remaining < 8);
        SetClass(left, "dim-label", remaining >= 8);
        gtk_widget_set_visible(bodyPlaceholder, gtk_text_buffer_get_char_count(gtk_text_view_get_buffer(GTK_TEXT_VIEW(bodyView))) == 0);
    }
    files::SyncSelection(commitFiles);
    files::SyncSelection(wipFiles);
}

// ---- the commit message

void DetailsPane::AmendToggled() {
    RepoSession* s = owner->session();
    if (!s || !s->data || s->data->unborn) {
        if (Amending()) gtk_check_button_set_active(GTK_CHECK_BUTTON(amend), FALSE);
        return;
    }
    if (Amending()) {
        // start from the message being amended (it arrives as a suggestion)
        if (SummaryText().empty() && BodyText().empty()) s->RequestHeadMessage();
    } else {
        if (!amendPrefill.empty() && SummaryText() + "\n" + BodyText() == amendPrefill) {
            gtk_editable_set_text(GTK_EDITABLE(summaryEntry), "");
            SetBody("");
        }
        amendPrefill.clear();
    }
    RefreshState();
}

void DetailsPane::DoCommit() {
    RepoSession* s = owner->session();
    if (!s) return;
    const RepoSession::CommitResult r = s->Commit(Widen(SummaryText()), Widen(BodyText()), Amending());
    if (r == RepoSession::CommitNoSummary) gtk_widget_grab_focus(summaryEntry);
    RefreshState();
}

void DetailsPane::Action(int k) {
    RepoSession* s = owner->session();
    if (!s || !s->data || s->selCommit < 0 || s->selCommit >= (int32_t)s->data->commits.size()) return;
    const Commit& cm = s->data->commits[(size_t)s->selCommit];
    if (cm.flags & CF_STASH) {
        if (cm.stash < 0) return;
        if (k == 0) s->Pop(cm.stash, false);
        else if (k == 1) s->Pop(cm.stash, true);
        else owner->DropStash(cm.stash);
        return;
    }
    if (k == 0) owner->CheckoutCommit(s->selCommit);
    else if (k == 1) owner->NewBranchAt(s->selCommit);
    else owner->RevertCommit(s->selCommit);
}

GtkWidget* MakeDetails(RepoTab* owner, DetailsPane** out) {
    DetailsPane* d = new DetailsPane(owner);
    *out = d;
    d->Build();
    g_object_set_data_full(G_OBJECT(d->root), "mg-details", d, [](gpointer p) { delete static_cast<DetailsPane*>(p); });
    return d->root;
}

namespace details {

void Reload(DetailsPane* d) {
    if (d) d->Reload();
}

void RefreshState(DetailsPane* d) {
    if (d) d->RefreshState();
}

void ApplySuggestion(DetailsPane* d) {
    RepoSession* s = d ? d->owner->session() : nullptr;
    if (!s || !d->SummaryText().empty() || !d->BodyText().empty()) return;  // never over what was typed
    const MessageSuggestion& m = s->suggestion;
    if (m.amend && !d->Amending()) return;
    gtk_editable_set_text(GTK_EDITABLE(d->summaryEntry), U8(m.summary).c_str());
    d->SetBody(U8(m.body));
    if (m.amend) d->amendPrefill = d->SummaryText() + "\n" + d->BodyText();
    d->RefreshState();
}

void ClearMessage(DetailsPane* d) {
    if (!d) return;
    gtk_editable_set_text(GTK_EDITABLE(d->summaryEntry), "");
    d->SetBody("");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(d->amend), FALSE);
    d->amendPrefill.clear();
    d->RefreshState();
}

void FocusSummary(DetailsPane* d) {
    if (d) gtk_widget_grab_focus(d->summaryEntry);
}

}  // namespace details
