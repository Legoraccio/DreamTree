#pragma once

// The Linux interface: GTK 4 and libadwaita, in C++ that calls the shared core (src/core) directly. Each tab of
// a window shows one repository through a RepoSession; this layer only presents it and asks before actions
// that lose data. Texts and questions come from present.h, as on Windows and macOS.
//
// The lists that can be long (the history, the diff, the sidebar and the file lists) are drawn by their own
// widget, Canvas, which only draws the rows on screen; the rest are GTK and libadwaita widgets.

#include <adwaita.h>

#include "navtree.h"
#include "platform.h"
#include "present.h"
#include "session.h"
#include "settings.h"
#include "text.h"

static_assert(sizeof(wchar_t) == 4, "wide text is UTF-32 on Linux");

inline constexpr char kAppId[] = "io.github.legoraccio.Mangrove";
inline constexpr char kResourceBase[] = "/io/github/legoraccio/Mangrove";
inline constexpr char kVersion[] = "1.1.0";

// ---------------------------------------------------------------- text (ui.cpp)

inline std::string U8(std::wstring_view s) { return Narrow(s); }
inline std::wstring WS(const char* s) { return s ? Widen(s) : std::wstring(); }
std::string TitleCase(const std::wstring& s);  // "Force push" -> "Force Push": buttons, as GNOME writes them
std::string Esc(std::string_view s);           // escaped for Pango markup

// ---------------------------------------------------------------- colors (ui.cpp)

// The style's colors (libadwaita's named colors), read again when it changes (light or dark, accent).
struct Palette {
    bool dark = false;
    GdkRGBA fg{}, viewBg{}, accent{}, accentText{}, success{}, error{}, warning{};
    GdkRGBA Fg(float alpha) const;  // the foreground at this opacity
};
const Palette& Pal();
void UpdatePalette();
GdkRGBA Rgb(uint32_t rrggbb, float alpha = 1);
GdkRGBA WithAlpha(GdkRGBA c, float alpha);  // c with its opacity multiplied
GdkRGBA LaneColor(int lane);                // graph lanes; lane 0 is the accent color
GdkRGBA StatusColor(wchar_t status);        // the letter of a changed file
GdkRGBA AddedColor();
GdkRGBA DeletedColor();

// ---------------------------------------------------------------- fonts (ui.cpp)

// The interface font (gtk-font-name) and the sizes and weights drawn with it.
struct Fonts {
    PangoFontDescription *ui = nullptr, *medium = nullptr, *bold = nullptr, *small = nullptr, *smallBold = nullptr;
    PangoFontDescription *mono = nullptr, *monoSmall = nullptr, *monoSmallBold = nullptr;
};
const Fonts& Font();
void ResetFonts();  // the font settings changed

// ---------------------------------------------------------------- drawing (ui.cpp)

namespace draw {
void Rect(GtkSnapshot* s, float x, float y, float w, float h, const GdkRGBA& c);
void Round(GtkSnapshot* s, float x, float y, float w, float h, float radius, const GdkRGBA& c);
void RoundBorder(GtkSnapshot* s, float x, float y, float w, float h, float radius, float width, const GdkRGBA& c);
void Circle(GtkSnapshot* s, float cx, float cy, float r, const GdkRGBA& c);
void Ring(GtkSnapshot* s, float cx, float cy, float r, float width, const GdkRGBA& c, float dash = 0);
// One line of text in the box (x, y, w, h): ellipsized, centered vertically, at the left or at the right of the
// box. Returns the width it took.
float Text(GtkSnapshot* s, GtkWidget* w, std::string_view text, const PangoFontDescription* font, const GdkRGBA& c, float x,
           float y, float width, float height, PangoEllipsizeMode mode = PANGO_ELLIPSIZE_END, bool right = false);
float TextWidth(GtkWidget* w, std::string_view text, const PangoFontDescription* font);
float LineHeight(GtkWidget* w, const PangoFontDescription* font);
// A symbolic icon of the theme (or of Mangrove's resources) in the color c, size x size pixels at (x, y).
void Icon(GtkSnapshot* s, GtkWidget* w, const char* name, float x, float y, float size, const GdkRGBA& c);
void Avatar(GtkSnapshot* s, GtkWidget* w, float x, float y, float size, const std::wstring& initials, uint32_t rrggbb);
// A flat button drawn in a list: a rounded label, brighter under the pointer.
void Button(GtkSnapshot* s, GtkWidget* w, const graphene_rect_t& r, std::string_view label, bool hover, bool enabled);
float ButtonWidth(GtkWidget* w, std::string_view label);
}  // namespace draw

inline bool Inside(const graphene_rect_t& r, double x, double y) {
    return x >= r.origin.x && y >= r.origin.y && x < r.origin.x + r.size.width && y < r.origin.y + r.size.height;
}

// ---------------------------------------------------------------- Canvas (ui.cpp)

// A widget drawn by a C++ object: only what is on screen, scrolled by a GtkScrolledWindow (it implements
// GtkScrollable). The widget owns the object: it goes with the widget.
class Canvas {
public:
    virtual ~Canvas();
    GtkWidget* widget = nullptr;    // the drawing widget
    GtkWidget* scroller = nullptr;  // the scrolled window around it (null when it doesn't scroll)

    virtual void Draw(GtkSnapshot* s, float width, float height) = 0;  // the visible part, from the top left of the view
    virtual double ContentHeight() { return 0; }
    virtual double ContentWidth() { return 0; }  // 0: as wide as the view
    virtual void Resized() {}
    virtual void Scrolled() {}

    float Width() const;
    float Height() const;
    double ScrollY() const;
    double ScrollX() const;
    void SetScrollY(double y);
    void SetScrollX(double x);
    void Refresh();  // the size of the content changed: the scroll bars, and a redraw
    void Redraw();
    std::weak_ptr<bool> Alive() const { return alive; }  // expires with the canvas: for work queued for later

private:
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};
enum class Scroll { None, Vertical, Both };
// Makes the widget of a canvas (and the scrolled window around it); returns the outermost one.
GtkWidget* MakeCanvas(Canvas* c, Scroll scroll);
void RedrawAllCanvases();  // the colors or the fonts changed

// ---------------------------------------------------------------- menus and questions (ui.cpp)

// A context menu built when it opens, each item with its own function and enabled state.
class Menu {
public:
    Menu();
    ~Menu();
    Menu(const Menu&) = delete;
    Menu& operator=(const Menu&) = delete;
    void Add(const std::string& label, std::function<void()> fn, bool enabled = true);
    void Section();  // what follows comes after a separator
    bool Empty() const { return count == 0; }
    void PopupAt(GtkWidget* parent, double x, double y);
    void PopupUnder(GtkWidget* button);

private:
    GMenu* root = nullptr;
    GMenu* section = nullptr;
    GSimpleActionGroup* group = nullptr;
    std::vector<std::function<void()>>* fns = nullptr;  // owned by the action group
    int count = 0;
};

// An alert dialog with the prompt's buttons (the last one cancels); done gets the button's index.
void RunPrompt(GtkWidget* parent, const Prompt& p, std::function<void(int)> done);

// ---------------------------------------------------------------- the system (ui.cpp)

void InstallMainDispatcher();  // RunOnMain posts to the GLib main loop
void Later(std::function<void()> fn);                  // on the main loop, once it is idle
guint After(unsigned ms, std::function<void()> fn);   // in ms milliseconds; the source's id
void CopyToClipboard(GtkWidget* w, const std::string& text);
void OpenUri(GtkWidget* w, const std::string& uri);
void OpenFolder(GtkWidget* w, const std::wstring& path);  // in the file manager
void RevealFile(GtkWidget* w, const std::wstring& path);  // its folder in the file manager, with the file selected
bool OpenTerminal(const std::wstring& dir);
std::string FileUri(const std::wstring& path);
GtkWidget* MakeLabel(const std::string& text, const char* cssClass = nullptr);  // a label, left aligned, ellipsized
void AddClass(GtkWidget* w, const char* cssClass);
void SetClass(GtkWidget* w, const char* cssClass, bool on);
void ApplyColorScheme();  // light, dark or the system's, as the settings say

// ---------------------------------------------------------------- the views of a repository

class RepoTab;

// The history: branch and tag chips, the graph, two-line commit rows, lines changed (history.cpp).
class HistoryView;
GtkWidget* MakeHistory(RepoTab* owner, HistoryView** out);
namespace history {
void Reload(HistoryView* v);          // new data or graph
void SyncSelection(HistoryView* v);   // the session's selection changed
void RevealSelection(HistoryView* v); // scroll the selected row into view
void Redraw(HistoryView* v);          // search results or line counts changed
void StatusChanged(HistoryView* v, int wipRowDelta);
void Focus(HistoryView* v);
}  // namespace history

// The sidebar: the pages, the branches, the remotes, the tags, the stashes, the submodules (sidebar.cpp).
class SidebarView;
GtkWidget* MakeSidebar(RepoTab* owner, SidebarView** out);
namespace sidebar {
void Reload(SidebarView* v);
void RefreshCounts(SidebarView* v);
void SyncSelection(SidebarView* v);
}  // namespace sidebar

// A list of changed files, as a list or as a tree of folders: the files of a commit (section 0), or the
// unstaged (1) and staged (2) files under their headers (files.cpp).
class FileList;
GtkWidget* MakeFileList(RepoTab* owner, bool wip, FileList** out);
namespace files {
void Reload(FileList* f, bool force);  // the session's files changed (force: rebuild even when they look the same)
void SetTree(FileList* f, bool tree);
void SyncSelection(FileList* f);       // the file of the open diff is the selected one
bool HasRows(FileList* f);
}  // namespace files

// Commit details, or the uncommitted changes with the commit message (details.cpp).
class DetailsPane;
GtkWidget* MakeDetails(RepoTab* owner, DetailsPane** out);
namespace details {
void Reload(DetailsPane* d);          // the selection, its details or the working tree changed
void RefreshState(DetailsPane* d);    // operations started or ended, the diff opened or closed
void ApplySuggestion(DetailsPane* d); // the session offers a commit message
void ClearMessage(DetailsPane* d);    // committed
void FocusSummary(DetailsPane* d);
}  // namespace details

// The diff of one file, with hunk and line staging (diffview.cpp).
class DiffPane;
GtkWidget* MakeDiff(RepoTab* owner, DiffPane** out);
namespace diff {
void Reload(DiffPane* d);  // the diff, its layout or its selection changed
void Focus(DiffPane* d);
}  // namespace diff

// ---------------------------------------------------------------- a repository in a tab (window.cpp)

class RepoTab {
public:
    explicit RepoTab(const std::wstring& path);  // empty: the start page
    ~RepoTab();
    RepoSession* session() const { return sess.get(); }
    std::shared_ptr<RepoSession> sess;
    GtkWidget* root = nullptr;  // the tab's content
    std::wstring Title() const;
    std::wstring Subtitle() const;
    bool Busy() const;
    void Open(const std::wstring& path);

    // the views, while they exist (each one clears its pointer when it goes)
    HistoryView* history = nullptr;
    SidebarView* side = nullptr;
    DetailsPane* details = nullptr;
    DiffPane* diffPane = nullptr;
    GtkWidget* sidePane = nullptr;
    GtkWidget* detailsPane = nullptr;
    GtkWidget* outerPaned = nullptr;
    GtkWidget* innerPaned = nullptr;
    GtkWidget* center = nullptr;  // a stack: history, diff, message
    GtkWidget* historyWidget = nullptr;
    GtkWidget* diffWidget = nullptr;

    // selection
    void SelectRow(int32_t row);       // the history's selection changed
    void SelectCommit(int32_t commit); // and scrolls the history to it
    void SelectWorkingTree();
    void ReviewChanges();              // the uncommitted changes, with the cursor in the commit message
    void SetHeadOnly(bool headOnly);
    void ShowHistoryPage();
    void SearchStep(int dir);          // the next (1) or previous (-1) match
    void MoveSelection(int by);        // up or down the history (the arrows in the search field)

    // actions; the ones that lose data ask first
    void OpenDiff(int section, const std::wstring& path);
    void CheckoutRef(int ref);
    void CheckoutCommit(int32_t commit);  // a branch at the commit, else the commit itself
    void NewBranchAt(int32_t commit);     // asks for the name; -1: at HEAD
    void RevertCommit(int32_t commit);
    void DropStash(int index);
    void DiscardFile(const FileChange& f);
    void StagePaths(const std::vector<std::wstring>& paths, bool stage);
    void ApplyPatch(const PatchJob& job);
    void Copy(const std::string& text);
    void RevealInFiles(const std::wstring& relativePath);
    void FillCommitMenu(Menu& m, int32_t commit);  // -1: the uncommitted changes
    void FillRefMenu(Menu& m, int ref);
    void FillStashMenu(Menu& m, int index);
    void Notify(const std::wstring& title, const std::wstring& message, bool error);
    void Escape();      // Esc: drops the selected lines, then closes the diff
    void Refresh();
    void Terminal();
    void ToggleDetails();
    void ToggleSidebar();
    bool DetailsShown() const;
    bool SidebarShown() const;
    void SettingsChanged();  // pull mode or diff layout changed
    GtkWidget* Toplevel() const;

private:
    void BuildStart();
    void BuildRepository();
    void SessionEvent_(SessionEvent e);
    void UpdateContent();
    void SelectionChanged();
    void Changed();  // the window shows this tab's state anew
    GtkWidget* startPage = nullptr;
    GtkWidget* messagePage = nullptr;
    std::wstring messageKey;
    GtkWidget* banner = nullptr;  // the notices, over the center
    GtkWidget* bannerIcon = nullptr;
    GtkWidget* bannerTitle = nullptr;
    GtkWidget* bannerMessage = nullptr;
    guint bannerTimer = 0;
    bool revealed = false;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    friend struct TabAccess;
};

RepoTab* TabOf(GtkWidget* tabRoot);

// ---------------------------------------------------------------- windows and the application (window.cpp, app.cpp)

GtkWidget* NewMainWindow(GtkApplication* app);                           // empty: no tab yet
RepoTab* AddTab(GtkWidget* window, const std::wstring& path, bool select);  // empty path: the start page
void SelectTab(GtkWidget* window, RepoTab* tab);
RepoTab* CurrentTab(GtkWidget* window);
std::vector<RepoTab*> TabsOf(GtkWidget* window);
void UpdateWindow(GtkWidget* window);  // the current tab's state: title, buttons, actions
GtkWidget* ActiveMainWindow();
std::vector<GtkWidget*> MainWindows();
void OpenRepository(const std::wstring& path, GtkWidget* window);  // in a tab of window (the start page becomes it)
void ChooseRepository(GtkWidget* window);                         // the folder dialog
void SaveTabs(GtkWidget* closing = nullptr);                      // the tabs to reopen (without those of a closing window)
void PrepareQuit();                                               // saves the tabs, which the windows then close
void BroadcastSettingsChanged();                                  // every tab reloads what depends on the settings
void RunSnapshots();                                              // MANGROVE_SNAPSHOT (app.cpp)

// ---------------------------------------------------------------- settings (preferences.cpp)

void ShowPreferences(GtkWidget* parent, const char* page);  // page: general, appearance, git, integrations (or null)
namespace prefs {
AdwDialog* Open();       // the dialog on screen, or null
void OpenAddAccount();   // the dialog that connects an account
void ShowSignInSample(); // signing in with the browser, with a sample code (interface tests)
void CloseAll();
}  // namespace prefs
