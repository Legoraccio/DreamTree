// The application: one instance per settings file (a second start hands its folders to the first one), the
// windows and the tabs reopened at launch, the application's actions and shortcuts, the About dialog, and the
// interface snapshots of the tests (MANGROVE_SNAPSHOT).

#include "LinuxUI.h"

#include <cstdio>

namespace {

bool g_started = false;

void StartWindows(GtkApplication* app) {
    g_started = true;
    GtkWidget* w = NewMainWindow(app);
    // A copy: opening tabs saves the settings, which rewrites the list.
    const std::vector<std::wstring> tabs = AppSettings().restoreTabs ? AppSettings().tabs : std::vector<std::wstring>();
    const int active = AppSettings().activeTab;
    RepoTab* activeTab = nullptr;
    int added = 0;
    for (size_t i = 0; i < tabs.size(); ++i) {
        if (!DirExists(tabs[i])) continue;
        RepoTab* t = AddTab(w, tabs[i], added == 0);
        if ((int)i == active) activeTab = t;
        ++added;
    }
    if (!added) AddTab(w, L"", true);
    else if (activeTab) SelectTab(w, activeTab);
    gtk_window_present(GTK_WINDOW(w));
    if (!EnvVar("MANGROVE_SNAPSHOT").empty()) RunSnapshots();
}

void OnActivate(GApplication* app, gpointer) {
    if (!g_started) {
        StartWindows(GTK_APPLICATION(app));
        return;
    }
    GtkWidget* w = ActiveMainWindow();  // started again without a folder: the window comes to the front
    if (!w) {
        w = NewMainWindow(GTK_APPLICATION(app));
        AddTab(w, L"", true);
    }
    gtk_window_present(GTK_WINDOW(w));
}

void OnOpen(GApplication* app, GFile** files, int n, const char*, gpointer) {
    if (!g_started) StartWindows(GTK_APPLICATION(app));
    for (int i = 0; i < n; ++i) {
        char* p = g_file_get_path(files[i]);
        if (p && DirExists(Widen(p))) OpenRepository(Widen(p), ActiveMainWindow());
        g_free(p);
    }
    if (GtkWidget* w = ActiveMainWindow()) gtk_window_present(GTK_WINDOW(w));
}

// ---- application actions

void OnNewWindow(GSimpleAction*, GVariant*, gpointer app) {
    GtkWidget* w = NewMainWindow(GTK_APPLICATION(app));
    AddTab(w, L"", true);
    gtk_window_present(GTK_WINDOW(w));
}

void OnPreferences(GSimpleAction*, GVariant*, gpointer) { ShowPreferences(ActiveMainWindow(), nullptr); }

void OnAbout(GSimpleAction*, GVariant*, gpointer) {
    AdwDialog* d = adw_about_dialog_new();
    AdwAboutDialog* a = ADW_ABOUT_DIALOG(d);
    adw_about_dialog_set_application_name(a, "Mangrove");
    adw_about_dialog_set_application_icon(a, kAppId);
    adw_about_dialog_set_version(a, kVersion);
    adw_about_dialog_set_comments(a, "A Git client that stays light: native, fast, and small.");
    adw_about_dialog_set_developer_name(a, "Roberto Di Miceli");
    adw_about_dialog_set_website(a, "https://github.com/Legoraccio/Mangrove");
    adw_about_dialog_set_issue_url(a, "https://github.com/Legoraccio/Mangrove/issues");
    adw_about_dialog_set_copyright(a, "\u00A9 2026 Roberto Di Miceli");
    adw_about_dialog_set_license_type(a, GTK_LICENSE_CUSTOM);
    adw_about_dialog_set_license(a, "Mangrove is distributed under the PolyForm Noncommercial License 1.0.0: free to use, change and share "
                                    "for any noncommercial purpose. See LICENSE.md, or https://polyformproject.org/licenses/noncommercial/1.0.0");
    const std::wstring git = GitVersionIfReady();
    if (!git.empty()) adw_about_dialog_set_debug_info(a, (U8(git) + "\nGTK " + std::to_string(gtk_get_major_version()) + "." +
                                                          std::to_string(gtk_get_minor_version()) + "." + std::to_string(gtk_get_micro_version()) +
                                                          ", libadwaita " + std::to_string(adw_get_major_version()) + "." +
                                                          std::to_string(adw_get_minor_version()) + "." + std::to_string(adw_get_micro_version()))
                                                             .c_str());
    adw_dialog_present(d, ActiveMainWindow());
}

void OnQuit(GSimpleAction*, GVariant*, gpointer app) {
    PrepareQuit();
    g_application_quit(G_APPLICATION(app));
}

void OnOpenRecent(GSimpleAction*, GVariant* v, gpointer) {
    const char* path = g_variant_get_string(v, nullptr);
    if (path && DirExists(Widen(path))) OpenRepository(Widen(path), ActiveMainWindow());
}

void OnClearRecent(GSimpleAction*, GVariant*, gpointer) {
    AppSettings().recent.clear();
    AppSettings().Save();
}

const GActionEntry kAppActions[] = {
    { "new-window", OnNewWindow, nullptr, nullptr, nullptr, {} },
    { "preferences", OnPreferences, nullptr, nullptr, nullptr, {} },
    { "about", OnAbout, nullptr, nullptr, nullptr, {} },
    { "quit", OnQuit, nullptr, nullptr, nullptr, {} },
    { "open-recent", OnOpenRecent, "s", nullptr, nullptr, {} },
    { "clear-recent", OnClearRecent, nullptr, nullptr, nullptr, {} },
};

const struct {
    const char* action;
    const char* accels[3];
} kAccels[] = {
    { "app.new-window", { "<Control>n" } },
    { "app.preferences", { "<Control>comma" } },
    { "app.quit", { "<Control>q" } },
    { "win.new-tab", { "<Control>t" } },
    { "win.open", { "<Control>o" } },
    { "win.close-tab", { "<Control>w" } },
    { "win.find", { "<Control>f" } },
    { "win.find-next", { "<Control>g" } },
    { "win.find-prev", { "<Control><Shift>g" } },
    { "win.fetch", { "<Control><Shift>f" } },
    { "win.pull", { "<Control><Shift>p" } },
    { "win.push", { "<Control>p" } },
    { "win.new-branch", { "<Control><Shift>n" } },
    { "win.stash", { "<Control><Alt>s" } },
    { "win.pop", { "<Control><Alt><Shift>s" } },
    { "win.undo-git", { "<Control><Alt>z" } },
    { "win.redo-git", { "<Control><Alt><Shift>z" } },
    { "win.refresh", { "<Control>r", "F5" } },
    { "win.history-page", { "<Control>1" } },
    { "win.changes-page", { "<Control>2" } },
    { "win.show-sidebar", { "F9" } },
    { "win.show-details", { "<Control><Alt>i" } },
    { "win.head-only", { "<Control><Alt>b" } },
};

// The style changed (light or dark, the accent color): the drawn views take its colors.
void StyleChanged(GObject*, GParamSpec*, gpointer) {
    Later([] {
        UpdatePalette();
        RedrawAllCanvases();
        BroadcastSettingsChanged();
    });
}

void OnStartup(GApplication* app, gpointer) {
    g_action_map_add_action_entries(G_ACTION_MAP(app), kAppActions, G_N_ELEMENTS(kAppActions), app);
    for (const auto& a : kAccels) {
        const char* accels[4] = { a.accels[0], a.accels[1], a.accels[2], nullptr };
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), a.action, accels);
    }
    gtk_window_set_default_icon_name(kAppId);
    ApplyColorScheme();
    AdwStyleManager* sm = adw_style_manager_get_default();
    g_signal_connect(sm, "notify::dark", G_CALLBACK(StyleChanged), nullptr);
    g_signal_connect(sm, "notify::high-contrast", G_CALLBACK(StyleChanged), nullptr);
#if ADW_CHECK_VERSION(1, 6, 0)
    g_signal_connect(sm, "notify::accent-color", G_CALLBACK(StyleChanged), nullptr);
#endif
    g_signal_connect(gtk_settings_get_default(), "notify::gtk-font-name", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer) {
        ResetFonts();
        RedrawAllCanvases();
    }), nullptr);
    std::thread([] { GitVersion(); }).detach();  // the first git call is the slow one
}

// ---- interface snapshots

GtkWidget* g_snapWindow = nullptr;

// The window as it draws itself (with the dialogs it shows) as a PNG file.
bool SavePng(GtkWidget* w, const std::string& path) {
    const int width = gtk_widget_get_width(w), height = gtk_widget_get_height(w);
    if (width <= 0 || height <= 0) return false;
    GdkPaintable* paintable = gtk_widget_paintable_new(w);
    GtkSnapshot* s = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, s, width, height);
    GskRenderNode* node = gtk_snapshot_free_to_node(s);
    g_object_unref(paintable);
    if (!node) return false;
    GskRenderer* renderer = gtk_native_get_renderer(gtk_widget_get_native(w));
    graphene_rect_t bounds;
    graphene_rect_init(&bounds, 0, 0, (float)width, (float)height);
    GdkTexture* texture = renderer ? gsk_renderer_render_texture(renderer, node, &bounds) : nullptr;
    gsk_render_node_unref(node);
    const bool ok = texture && gdk_texture_save_to_png(texture, path.c_str());
    if (texture) g_object_unref(texture);
    return ok;
}

void Snap(const char* name) {
    const std::string path = U8(EnvVar("MANGROVE_SNAPSHOT")) + "/" + name;
    const bool ok = g_snapWindow && SavePng(g_snapWindow, path);
    fprintf(stderr, "snapshot %s: %s\n", name, ok ? "saved" : "FAILED");
}

void Step(int step, int tries);

void Next(unsigned ms, int step, int tries = 0) {
    After(ms, [step, tries] { Step(step, tries); });
}

// Each state is set, given time to load, then pictured.
void Step(int step, int tries) {
    GtkWidget* w = g_snapWindow;
    RepoTab* t = w ? CurrentTab(w) : nullptr;
    RepoSession* s = t ? t->session() : nullptr;
    if (!s || !s->data || s->loading || s->opBusy) {  // still loading
        if (tries >= 80) {
            fprintf(stderr, "snapshots: the repository did not load: %s\n", s ? U8(s->error).c_str() : "no repository tab");
            g_application_quit(g_application_get_default());
            return;
        }
        Next(500, step, tries + 1);
        return;
    }
    static const char* const kPages[] = { "general", "appearance", "git", "integrations" };
    static const char* const kNames[] = { "5-settings-general.png", "6-settings-appearance.png", "7-settings-git.png", "8-settings-integrations.png" };
    switch (step) {
    case 0:  // the uncommitted changes, selected after loading
        Next(1500, 1);
        return;
    case 1:
        Snap("1-changes.png");
        Next(500, 2);
        return;
    case 2:  // a commit with its files
        t->SelectCommit(s->data->headCommit);
        Next(1500, 3);
        return;
    case 3:
        if (!s->details && tries < 20) {
            Next(500, 3, tries + 1);
            return;
        }
        Snap("2-commit.png");
        Next(500, 4);
        return;
    case 4: {  // the diff of a modified file, from the uncommitted changes
        if (!s->status || s->status->unstaged.empty()) {
            Next(100, 10);
            return;
        }
        t->SelectWorkingTree();
        const FileChange* pick = &s->status->unstaged[0];
        for (const FileChange& f : s->status->unstaged)
            if (f.status == L'M') {
                pick = &f;
                break;
            }
        t->OpenDiff(1, pick->path);
        Next(1500, 5);
        return;
    }
    case 5:
        if (!s->diff.diff && tries < 20) {
            Next(500, 5, tries + 1);
            return;
        }
        Snap("3-diff.png");
        Next(500, 6);
        return;
    case 6:  // the other layout
        AppSettings().diffSplit = !AppSettings().diffSplit;
        BroadcastSettingsChanged();
        Next(1000, 7);
        return;
    case 7:
        Snap("4-diff-split.png");
        Next(500, 8);
        return;
    case 8:
        AppSettings().diffSplit = !AppSettings().diffSplit;
        BroadcastSettingsChanged();
        t->SelectWorkingTree();
        Next(500, 10);
        return;
    case 10: case 12: case 14: case 16:  // the settings, page by page
        ShowPreferences(w, kPages[(step - 10) / 2]);
        Next(1000, step + 1);
        return;
    case 11: case 13: case 15: case 17:
        Snap(kNames[(step - 11) / 2]);
        Next(500, step + 1);
        return;
    case 18:  // the dialog that connects an account
        prefs::OpenAddAccount();
        Next(1500, 19);
        return;
    case 19:
        Snap("9-add-account.png");
        Next(500, 20);
        return;
    case 20:  // signing in with the browser: the code to enter on GitHub
        prefs::ShowSignInSample();
        Next(1000, 21);
        return;
    case 21:
        Snap("10-sign-in.png");
        Next(500, 22);
        return;
    case 22:
        prefs::CloseAll();
        Next(800, 23);
        return;
    case 23:
        Snap("11-main.png");
        Next(300, 99);
        return;
    default:
        fprintf(stderr, "snapshots: done\n");
        PrepareQuit();
        g_application_quit(g_application_get_default());
    }
}

}  // namespace

// MANGROVE_SNAPSHOT=<folder>: pictures of the first window at a few steps (the uncommitted changes, a commit, a
// diff unified and side by side, the settings) as PNG files, then quit. The CI smoke test of the interface.
void RunSnapshots() {
    g_snapWindow = ActiveMainWindow();
    if (g_snapWindow) {
        gtk_window_unmaximize(GTK_WINDOW(g_snapWindow));
        gtk_window_set_default_size(GTK_WINDOW(g_snapWindow), 1280, 800);
    }
    Next(500, 0);
}

int main(int argc, char** argv) {
    AppSettings().Load();
    InstallMainDispatcher();
    // One instance per settings file: another one (MANGROVE_SETTINGS) is another profile, with its own instance.
    std::string id = kAppId;
    if (!EnvVar("MANGROVE_SETTINGS").empty()) {
        char suffix[16];
        snprintf(suffix, sizeof suffix, ".S%08x", (unsigned)Fnv1aW(SettingsFile()));
        id += suffix;
    }
    GApplicationFlags flags = G_APPLICATION_HANDLES_OPEN;
    if (!EnvVar("MANGROVE_SNAPSHOT").empty()) flags = (GApplicationFlags)(flags | G_APPLICATION_NON_UNIQUE);
    AdwApplication* app = adw_application_new(id.c_str(), flags);
    g_application_set_resource_base_path(G_APPLICATION(app), kResourceBase);
    g_set_application_name("Mangrove");
    g_signal_connect(app, "startup", G_CALLBACK(OnStartup), nullptr);
    g_signal_connect(app, "activate", G_CALLBACK(OnActivate), nullptr);
    g_signal_connect(app, "open", G_CALLBACK(OnOpen), nullptr);
    const int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
