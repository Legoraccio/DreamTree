// Settings, by category: General, Appearance, Git, and Integrations: the accounts on GitHub, GitLab and Gitea /
// Forgejo, connected by signing in with the browser (github.com) or with a token, and kept in the system keyring.

#include "LinuxUI.h"

namespace {

// The initials of an account on a color of its own, as in the other apps.
class InitialsView : public Canvas {
public:
    Account account;
    void Draw(GtkSnapshot* s, float w, float h) override {
        draw::Avatar(s, widget, 1, 1, std::min(w, h) - 2, Initials(account.name.empty() ? account.login : account.name, L""), AvatarColor(account.Key()));
    }
};

struct Prefs {
    AdwPreferencesDialog* dialog = nullptr;
    GtkWidget* gitRow = nullptr;
    AdwPreferencesGroup* trusted = nullptr;
    std::vector<GtkWidget*> trustedRows;
    AdwPreferencesGroup* accounts = nullptr;
    std::vector<GtkWidget*> accountRows;
    GtkWidget* note = nullptr;
    AdwDialog* add = nullptr;  // the dialog that connects an account, while it is open
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};
Prefs* g_prefs = nullptr;  // the dialog on screen

GtkWidget* Page(const char* name, const char* title, const char* icon) {
    GtkWidget* page = adw_preferences_page_new();
    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page), title);
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page), icon);
    adw_preferences_page_set_name(ADW_PREFERENCES_PAGE(page), name);
    return page;
}

GtkWidget* Group(GtkWidget* page, const char* title, const std::string& description = "") {
    GtkWidget* g = adw_preferences_group_new();
    if (title) adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g), title);
    if (!description.empty()) adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(g), Esc(description).c_str());
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g));
    return g;
}

GtkWidget* Combo(const char* title, const std::vector<std::string>& items, int selected) {
    GtkWidget* row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    std::vector<const char*> strs;
    for (const std::string& s : items) strs.push_back(s.c_str());
    strs.push_back(nullptr);
    GtkStringList* model = gtk_string_list_new(strs.data());
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
    g_object_unref(model);
    adw_combo_row_set_selected(ADW_COMBO_ROW(row), (guint)std::max(0, selected));
    return row;
}

void Saved() {
    AppSettings().Save();
    BroadcastSettingsChanged();
}

// ---------------------------------------------------------------- General

GtkWidget* GeneralPage() {
    GtkWidget* page = Page("general", "General", "preferences-system-symbolic");
    const Settings& s = AppSettings();
    GtkWidget* g1 = Group(page, "Startup");
    GtkWidget* reopen = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(reopen), "Reopen Repositories");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(reopen), "Open the repositories that were open when Mangrove quit");
    adw_switch_row_set_active(ADW_SWITCH_ROW(reopen), s.restoreTabs);
    g_signal_connect(reopen, "notify::active", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer) {
        AppSettings().restoreTabs = adw_switch_row_get_active(ADW_SWITCH_ROW(o));
        Saved();
    }), nullptr);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1), reopen);

    GtkWidget* g2 = Group(page, "Pull", U8(PullModeHelp()));
    std::vector<std::string> modes;
    for (const std::wstring& m : PullModeLabels()) modes.push_back(U8(m));
    GtkWidget* pull = Combo("Pull Mode", modes, std::clamp(s.pullMode, 0, 2));
    adw_combo_row_set_use_subtitle(ADW_COMBO_ROW(pull), TRUE);  // the long labels under the title, not cut
    g_signal_connect(pull, "notify::selected", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer) {
        AppSettings().pullMode = std::clamp((int)adw_combo_row_get_selected(ADW_COMBO_ROW(o)), 0, 2);
        Saved();
    }), nullptr);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), pull);

    GtkWidget* g3 = Group(page, "Diffs");
    GtkWidget* layout = Combo("Layout", { "Unified", "Side by Side" }, s.diffSplit ? 1 : 0);
    g_signal_connect(layout, "notify::selected", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer) {
        AppSettings().diffSplit = adw_combo_row_get_selected(ADW_COMBO_ROW(o)) == 1;
        Saved();
    }), nullptr);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g3), layout);
    return page;
}

// ---------------------------------------------------------------- Appearance

GtkWidget* AppearancePage() {
    GtkWidget* page = Page("appearance", "Appearance", "preferences-desktop-appearance-symbolic");
    GtkWidget* g = Group(page, "Style", "Follow System uses the light or dark style chosen in the system's settings.");
    const std::wstring theme = AppSettings().Get(L"theme");
    GtkWidget* row = Combo("Style", { "Follow System", "Light", "Dark" }, theme == L"light" ? 1 : theme == L"dark" ? 2 : 0);
    g_signal_connect(row, "notify::selected", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer) {
        const guint i = adw_combo_row_get_selected(ADW_COMBO_ROW(o));
        AppSettings().Set(L"theme", i == 1 ? L"light" : i == 2 ? L"dark" : L"system");
        ApplyColorScheme();
        Saved();
    }), nullptr);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g), row);
    return page;
}

// ---------------------------------------------------------------- Git

void ReloadTrusted(Prefs* p) {
    for (GtkWidget* r : p->trustedRows) adw_preferences_group_remove(p->trusted, r);
    p->trustedRows.clear();
    const std::vector<std::wstring>& trusted = AppSettings().trusted;
    if (trusted.empty()) {
        GtkWidget* row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "<span alpha=\"55%\">No trusted folders</span>");
        adw_preferences_group_add(p->trusted, row);
        p->trustedRows.push_back(row);
        return;
    }
    for (size_t i = 0; i < trusted.size(); ++i) {
        GtkWidget* row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), Esc(U8(trusted[i])).c_str());
        adw_preferences_row_set_title_selectable(ADW_PREFERENCES_ROW(row), TRUE);
        GtkWidget* remove = gtk_button_new_from_icon_name("user-trash-symbolic");
        AddClass(remove, "flat circular");
        gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(remove, "Stop trusting this folder");
        g_object_set_data_full(G_OBJECT(remove), "mg-path", g_strdup(U8(trusted[i]).c_str()), g_free);
        g_signal_connect(remove, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer data) {
            const std::wstring path = Widen(static_cast<const char*>(g_object_get_data(G_OBJECT(b), "mg-path")));
            std::vector<std::wstring>& t = AppSettings().trusted;
            t.erase(std::remove(t.begin(), t.end(), path), t.end());
            AppSettings().Save();
            ReloadTrusted(static_cast<Prefs*>(data));
        }), p);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove);
        adw_preferences_group_add(p->trusted, row);
        p->trustedRows.push_back(row);
    }
}

GtkWidget* GitPage(Prefs* p) {
    GtkWidget* page = Page("git", "Git", "mg-branch-symbolic");
    GtkWidget* g1 = Group(page, "Git");
    p->gitRow = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->gitRow), "Version");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(p->gitRow), "Checking\u2026");
    adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(p->gitRow), TRUE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1), p->gitRow);
    std::weak_ptr<bool> a = p->alive;
    std::thread([p, a] {
        const std::wstring version = GitVersion(), path = GitPath(false);
        RunOnMain([p, a, version, path] {
            if (!a.lock()) return;
            const std::string text = version.empty() ? "Not found: install it with your package manager (for example: sudo apt install git)"
                                                     : U8(version) + "  (" + U8(path) + ")";
            adw_action_row_set_subtitle(ADW_ACTION_ROW(p->gitRow), Esc(text).c_str());
        });
    }).detach();
    p->trusted = ADW_PREFERENCES_GROUP(Group(page, "Trusted Folders", U8(TrustedHelp())));
    ReloadTrusted(p);
    return page;
}

// ---------------------------------------------------------------- Integrations

void ReloadAccounts(Prefs* p);

// The dialog that connects an account: the service, its server, then signing in with the browser (a code to enter
// on GitHub, where Mangrove has an OAuth app) or a token; either way the token is checked with the server.
struct AddAccount {
    Prefs* prefs = nullptr;
    std::weak_ptr<bool> prefsAlive;
    AdwDialog* dialog = nullptr;
    GtkWidget* stack = nullptr;
    GtkWidget* service = nullptr;
    GtkWidget* server = nullptr;
    GtkWidget* browserGroup = nullptr;
    GtkWidget* tokenGroup = nullptr;
    GtkWidget* token = nullptr;
    GtkWidget* spinner = nullptr;
    GtkWidget* status = nullptr;
    GtkWidget* connect = nullptr;
    GtkWidget* cancel = nullptr;
    // signing in with the browser
    GtkWidget* codeIntro = nullptr;
    GtkWidget* code = nullptr;
    GtkWidget* open = nullptr;
    GtkWidget* codeSpinner = nullptr;
    GtkWidget* codeStatus = nullptr;
    std::string verifyUrl;
    std::shared_ptr<std::atomic<bool>> signInCancel;  // set to stop the sign-in's worker thread
    bool signingIn = false;
    bool busy = false;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    Forge SelectedForge() const {
        const guint i = adw_combo_row_get_selected(ADW_COMBO_ROW(service));
        return Forges()[std::min<size_t>(i, Forges().size() - 1)].forge;
    }
    std::wstring Server() const { return Widen(gtk_editable_get_text(GTK_EDITABLE(server))); }
    void ServiceChanged() {
        const ForgeInfo& f = InfoOf(SelectedForge());
        gtk_editable_set_text(GTK_EDITABLE(server), U8(f.server).c_str());
        adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(tokenGroup), Esc(U8(f.tokenHelp)).c_str());
        ShowStatus("", false);
        UpdateBrowser();
    }
    // Signing in with the browser is offered where Mangrove has an OAuth app (github.com).
    void UpdateBrowser() { gtk_widget_set_visible(browserGroup, !signingIn && BrowserSignInAvailable(SelectedForge(), Server())); }
    void ShowStatus(const std::string& text, bool error) {
        gtk_label_set_text(GTK_LABEL(status), text.c_str());
        SetClass(status, "error", error);
        SetClass(status, "dim-label", !error);
        gtk_widget_set_visible(status, !text.empty());
    }
    void SetBusy(bool b) {
        busy = b;
        for (GtkWidget* w : { service, server, token, connect, browserGroup }) gtk_widget_set_sensitive(w, !b);
        gtk_widget_set_visible(spinner, b);
        if (b) gtk_spinner_start(GTK_SPINNER(spinner));
        else gtk_spinner_stop(GTK_SPINNER(spinner));
    }
    void CreateToken() {
        std::wstring why;
        const std::wstring srv = NormalizeServer(Server(), &why);
        if (srv.empty()) {
            ShowStatus(U8(why), true);
            return;
        }
        OpenUri(GTK_WIDGET(dialog), U8(TokenPageUrl(SelectedForge(), srv)));
    }
    void Connect() {
        if (busy || signingIn) return;
        const Forge f = SelectedForge();
        const std::wstring srv = Server();
        const std::string tok = gtk_editable_get_text(GTK_EDITABLE(token));
        SetBusy(true);
        ShowStatus("Checking the token\u2026", false);
        std::weak_ptr<bool> a = alive;
        std::thread([this, a, f, srv, tok] {
            const TokenCheck check = CheckToken(f, srv, tok);
            RunOnMain([this, a, check, tok] {
                if (a.lock()) Finish(check, tok);
            });
        }).detach();
    }
    void Finish(const TokenCheck& check, const std::string& tok) {
        SetBusy(false);
        std::wstring error = check.error;
        if (check.ok && ::AddAccount(check.account, tok, &error)) {
            gtk_editable_set_text(GTK_EDITABLE(token), "");
            Prefs* p = prefsAlive.lock() ? prefs : nullptr;
            const std::wstring note = check.note;
            adw_dialog_close(dialog);
            if (p) {
                ReloadAccounts(p);
                gtk_label_set_text(GTK_LABEL(p->note), U8(note).c_str());
                gtk_widget_set_visible(p->note, !note.empty());
            }
            return;
        }
        ShowStatus(U8(error), true);
        gtk_widget_grab_focus(token);
    }
    // A worker thread asks GitHub for a code, then waits for the user to authorize Mangrove on GitHub
    // (src/core/accounts.cpp). Cancel goes back to the form.
    void SignIn() {
        if (busy || signingIn) return;
        const Forge f = SelectedForge();
        const std::wstring srv = Server();
        auto stop = std::make_shared<std::atomic<bool>>(false);
        signInCancel = stop;
        ShowCode("", "");
        std::weak_ptr<bool> a = alive;
        std::thread([this, a, f, srv, stop] {
            std::string tok;
            const TokenCheck check = BrowserSignIn(
                f, srv,
                [this, a, stop](const DeviceCode& dc) {
                    const std::string userCode = U8(dc.userCode), url = U8(dc.verifyUrl);
                    RunOnMain([this, a, stop, userCode, url] {
                        if (a.lock() && !*stop) ShowCode(userCode, url);
                    });
                },
                *stop, &tok);
            RunOnMain([this, a, stop, check, tok] {
                if (!a.lock() || *stop) return;
                EndSignIn();
                Finish(check, tok);
            });
        }).detach();
    }
    // The code to enter on GitHub (empty while it is being asked for), in place of the form.
    void ShowCode(const std::string& c, const std::string& url) {
        signingIn = true;
        verifyUrl = url;
        gtk_stack_set_visible_child_name(GTK_STACK(stack), "code");
        for (GtkWidget* w : { codeIntro, code, open }) gtk_widget_set_visible(w, !c.empty());
        gtk_label_set_text(GTK_LABEL(code), c.c_str());
        gtk_label_set_text(GTK_LABEL(codeStatus), c.empty() ? "Asking GitHub for a code\u2026" : "Waiting for you to authorize Mangrove on GitHub\u2026");
        gtk_spinner_start(GTK_SPINNER(codeSpinner));
        gtk_widget_set_visible(connect, FALSE);
        if (!c.empty()) {
            adw_dialog_set_default_widget(dialog, open);
            adw_dialog_set_focus(dialog, open);
        }
    }
    void EndSignIn() {
        if (signInCancel) *signInCancel = true;  // the worker stops at its next look, and its results are dropped
        signInCancel.reset();
        signingIn = false;
        gtk_spinner_stop(GTK_SPINNER(codeSpinner));
        gtk_stack_set_visible_child_name(GTK_STACK(stack), "form");
        gtk_widget_set_visible(connect, TRUE);
        adw_dialog_set_default_widget(dialog, connect);
        UpdateBrowser();
    }
    // The code goes to the clipboard, ready to paste on the page; the page is always the server's (BrowserSignIn
    // checks it).
    void OpenSignInPage() {
        const char* c = gtk_label_get_text(GTK_LABEL(code));
        if (verifyUrl.empty() || !c || !*c) return;
        CopyToClipboard(GTK_WIDGET(dialog), c);
        OpenUri(GTK_WIDGET(dialog), verifyUrl);
    }
    void Cancel() {
        if (signingIn) {  // back to the form
            EndSignIn();
            return;
        }
        gtk_editable_set_text(GTK_EDITABLE(token), "");
        adw_dialog_close(dialog);
    }
};

AddAccount* g_add = nullptr;

GtkWidget* ActivatableRow(const char* title, const char* subtitle, const char* icon) {
    GtkWidget* row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle) adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name(icon));
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    return row;
}

void OpenAddAccountDialog(Prefs* p) {
    if (!p || p->add) return;
    AddAccount* a = new AddAccount();
    a->prefs = p;
    a->prefsAlive = p->alive;
    AdwDialog* dlg = adw_dialog_new();
    a->dialog = dlg;
    p->add = dlg;
    g_add = a;
    adw_dialog_set_title(dlg, "Add an Account");
    adw_dialog_set_content_width(dlg, 480);
    g_object_set_data_full(G_OBJECT(dlg), "mg-add", a, [](gpointer d) {
        AddAccount* aa = static_cast<AddAccount*>(d);
        if (aa->signInCancel) *aa->signInCancel = true;  // the dialog went away: no one waits for the authorization any more
        if (g_add == aa) g_add = nullptr;
        delete aa;
    });
    g_signal_connect(dlg, "closed", G_CALLBACK(+[](AdwDialog*, gpointer data) {
        Prefs* pr = static_cast<Prefs*>(data);
        if (g_prefs == pr) pr->add = nullptr;
    }), p);

    GtkWidget* view = adw_toolbar_view_new();
    GtkWidget* header = adw_header_bar_new();
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
    adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
    a->cancel = gtk_button_new_with_label("Cancel");
    a->connect = gtk_button_new_with_label("Connect");
    AddClass(a->connect, "suggested-action");
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), a->cancel);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), a->connect);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    g_signal_connect(a->cancel, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<AddAccount*>(d)->Cancel(); }), a);
    g_signal_connect(a->connect, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<AddAccount*>(d)->Connect(); }), a);

    // the form
    GtkWidget* form = adw_preferences_page_new();
    GtkWidget* g1 = adw_preferences_group_new();
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(g1), "Change the server for GitHub Enterprise, a self-managed GitLab or another Gitea / Forgejo server.");
    std::vector<std::string> names;
    for (const ForgeInfo& f : Forges()) names.push_back(U8(f.name));
    a->service = Combo("Service", names, 0);
    a->server = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(a->server), "Server");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1), a->service);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1), a->server);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(form), ADW_PREFERENCES_GROUP(g1));
    g_signal_connect(a->service, "notify::selected", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer d) { static_cast<AddAccount*>(d)->ServiceChanged(); }), a);
    g_signal_connect(a->server, "changed", G_CALLBACK(+[](GtkEditable*, gpointer d) { static_cast<AddAccount*>(d)->UpdateBrowser(); }), a);

    a->browserGroup = adw_preferences_group_new();
    GtkWidget* signIn = ActivatableRow("Sign In with the Browser\u2026",
                                       "Mangrove shows a code to enter on GitHub: once you authorize Mangrove there, the account is connected, with no token to copy.",
                                       "go-next-symbolic");
    g_signal_connect(signIn, "activated", G_CALLBACK(+[](AdwActionRow*, gpointer d) { static_cast<AddAccount*>(d)->SignIn(); }), a);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(a->browserGroup), signIn);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(form), ADW_PREFERENCES_GROUP(a->browserGroup));

    a->tokenGroup = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(a->tokenGroup), "Access Token");
    GtkWidget* create = ActivatableRow("Create a Token\u2026", nullptr, "adw-external-link-symbolic");
    g_signal_connect(create, "activated", G_CALLBACK(+[](AdwActionRow*, gpointer d) { static_cast<AddAccount*>(d)->CreateToken(); }), a);
    a->token = adw_password_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(a->token), "Token");
    g_signal_connect(a->token, "entry-activated", G_CALLBACK(+[](AdwEntryRow*, gpointer d) { static_cast<AddAccount*>(d)->Connect(); }), a);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(a->tokenGroup), create);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(a->tokenGroup), a->token);
    GtkWidget* statusRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(statusRow, 12);
    a->spinner = gtk_spinner_new();
    gtk_widget_set_visible(a->spinner, FALSE);
    a->status = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(a->status), TRUE);
    gtk_label_set_xalign(GTK_LABEL(a->status), 0);
    gtk_widget_set_visible(a->status, FALSE);
    gtk_box_append(GTK_BOX(statusRow), a->spinner);
    gtk_box_append(GTK_BOX(statusRow), a->status);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(a->tokenGroup), statusRow);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(form), ADW_PREFERENCES_GROUP(a->tokenGroup));

    // signing in: the code to enter on GitHub, in place of the form
    GtkWidget* codeBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_valign(codeBox, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(codeBox, 24);
    gtk_widget_set_margin_end(codeBox, 24);
    gtk_widget_set_margin_top(codeBox, 24);
    gtk_widget_set_margin_bottom(codeBox, 30);
    a->codeIntro = gtk_label_new("Enter this code on GitHub, then authorize Mangrove:");
    gtk_label_set_wrap(GTK_LABEL(a->codeIntro), TRUE);
    a->code = gtk_label_new("");
    gtk_label_set_selectable(GTK_LABEL(a->code), TRUE);
    AddClass(a->code, "mg-code");
    a->open = gtk_button_new_with_label("Copy Code and Open GitHub");
    AddClass(a->open, "pill suggested-action");
    gtk_widget_set_halign(a->open, GTK_ALIGN_CENTER);
    g_signal_connect(a->open, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<AddAccount*>(d)->OpenSignInPage(); }), a);
    GtkWidget* waiting = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(waiting, GTK_ALIGN_CENTER);
    a->codeSpinner = gtk_spinner_new();
    a->codeStatus = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(a->codeStatus), TRUE);
    AddClass(a->codeStatus, "dim-label");
    gtk_box_append(GTK_BOX(waiting), a->codeSpinner);
    gtk_box_append(GTK_BOX(waiting), a->codeStatus);
    for (GtkWidget* w : { a->codeIntro, a->code, a->open, waiting }) gtk_box_append(GTK_BOX(codeBox), w);

    a->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(a->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_add_named(GTK_STACK(a->stack), form, "form");
    gtk_stack_add_named(GTK_STACK(a->stack), codeBox, "code");
    gtk_stack_set_vhomogeneous(GTK_STACK(a->stack), FALSE);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), a->stack);
    adw_dialog_set_child(dlg, view);
    adw_dialog_set_default_widget(dlg, a->connect);
    a->ServiceChanged();
    adw_dialog_present(dlg, GTK_WIDGET(p->dialog));
    adw_dialog_set_focus(dlg, a->token);  // the server is already filled in for the common case
}

void ReloadAccounts(Prefs* p) {
    for (GtkWidget* r : p->accountRows) adw_preferences_group_remove(p->accounts, r);
    p->accountRows.clear();
    const std::vector<Account> accounts = LoadAccounts();
    if (accounts.empty()) {
        GtkWidget* row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "<span alpha=\"55%\">No accounts</span>");
        adw_preferences_group_add(p->accounts, row);
        p->accountRows.push_back(row);
        return;
    }
    for (const Account& acc : accounts) {
        GtkWidget* row = adw_action_row_new();
        const std::string title = acc.name.empty() ? U8(acc.login) : U8(acc.name) + " (" + U8(acc.login) + ")";
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), Esc(title).c_str());
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), Esc(U8(InfoOf(acc.forge).name) + " \u00B7 " + U8(acc.Host())).c_str());
        InitialsView* avatar = new InitialsView();
        avatar->account = acc;
        GtkWidget* av = MakeCanvas(avatar, Scroll::None);
        gtk_widget_set_size_request(av, 34, 34);
        gtk_widget_set_valign(av, GTK_ALIGN_CENTER);
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), av);
        GtkWidget* remove = gtk_button_new_from_icon_name("user-trash-symbolic");
        AddClass(remove, "flat circular");
        gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(remove, "Remove the account");
        g_object_set_data_full(G_OBJECT(remove), "mg-account", new Account(acc), [](gpointer d) { delete static_cast<Account*>(d); });
        g_signal_connect(remove, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer data) {
            const Account a = *static_cast<Account*>(g_object_get_data(G_OBJECT(b), "mg-account"));
            Prefs* pr = static_cast<Prefs*>(data);
            std::weak_ptr<bool> alive = pr->alive;
            RunPrompt(GTK_WIDGET(pr->dialog), RemoveAccountPrompt(a), [a, pr, alive](int button) {
                if (button != 0) return;
                RemoveAccount(a);
                if (alive.lock()) ReloadAccounts(pr);
            });
        }), p);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove);
        adw_preferences_group_add(p->accounts, row);
        p->accountRows.push_back(row);
    }
}

GtkWidget* IntegrationsPage(Prefs* p) {
    GtkWidget* page = Page("integrations", "Integrations", "system-users-symbolic");
    p->accounts = ADW_PREFERENCES_GROUP(Group(page, "Accounts", U8(AccountsIntro())));
    GtkWidget* add = gtk_button_new();
    GtkWidget* content = adw_button_content_new();
    adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(content), "list-add-symbolic");
    adw_button_content_set_label(ADW_BUTTON_CONTENT(content), "Add Account");
    gtk_button_set_child(GTK_BUTTON(add), content);
    AddClass(add, "flat");
    gtk_widget_set_valign(add, GTK_ALIGN_CENTER);
    g_signal_connect(add, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) { OpenAddAccountDialog(static_cast<Prefs*>(data)); }), p);
    adw_preferences_group_set_header_suffix(p->accounts, add);
    ReloadAccounts(p);
    GtkWidget* notes = Group(page, nullptr);  // what to know about the account just connected
    p->note = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(p->note), TRUE);
    gtk_label_set_xalign(GTK_LABEL(p->note), 0);
    AddClass(p->note, "warning");
    gtk_widget_set_visible(p->note, FALSE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(notes), p->note);
    return page;
}

}  // namespace

void ShowPreferences(GtkWidget* parent, const char* page) {
    if (g_prefs) {
        if (page) adw_preferences_dialog_set_visible_page_name(g_prefs->dialog, page);
        return;
    }
    Prefs* p = new Prefs();
    g_prefs = p;
    p->dialog = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    adw_dialog_set_title(ADW_DIALOG(p->dialog), "Settings");
    adw_preferences_dialog_set_search_enabled(p->dialog, FALSE);
    adw_preferences_dialog_add(p->dialog, ADW_PREFERENCES_PAGE(GeneralPage()));
    adw_preferences_dialog_add(p->dialog, ADW_PREFERENCES_PAGE(AppearancePage()));
    adw_preferences_dialog_add(p->dialog, ADW_PREFERENCES_PAGE(GitPage(p)));
    adw_preferences_dialog_add(p->dialog, ADW_PREFERENCES_PAGE(IntegrationsPage(p)));
    if (page) adw_preferences_dialog_set_visible_page_name(p->dialog, page);
    g_object_set_data_full(G_OBJECT(p->dialog), "mg-prefs", p, [](gpointer d) { delete static_cast<Prefs*>(d); });
    g_signal_connect(p->dialog, "closed", G_CALLBACK(+[](AdwDialog*, gpointer data) {
        if (g_prefs == data) g_prefs = nullptr;
    }), p);
    adw_dialog_present(ADW_DIALOG(p->dialog), parent);
}

namespace prefs {

AdwDialog* Open() { return g_prefs ? ADW_DIALOG(g_prefs->dialog) : nullptr; }

void OpenAddAccount() {
    if (!g_prefs) ShowPreferences(ActiveMainWindow(), "integrations");
    adw_preferences_dialog_set_visible_page_name(g_prefs->dialog, "integrations");
    OpenAddAccountDialog(g_prefs);
}

void ShowSignInSample() {
    if (g_add) g_add->ShowCode("WDJB-MJHT", "https://github.com/login/device");
}

void CloseAll() {
    if (g_add) adw_dialog_force_close(g_add->dialog);
    if (g_prefs) adw_dialog_force_close(ADW_DIALOG(g_prefs->dialog));
}

}  // namespace prefs
