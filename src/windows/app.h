#pragma once
#include "common.h"
#include "gfx.h"
#include "navtree.h"
#include "platform.h"
#include "present.h"
#include "session.h"
#include "settings.h"
#include "text.h"
#include "theme.h"

// Pixels per DIP (monitor DPI x user zoom). All layout is computed in physical pixels.
extern float g_S;
inline float D(float v) { return v * g_S; }
inline float DR(float v) { return std::round(v * g_S); }

constexpr float kGraphRowH = 56.f;  // history row height (DIP)

// Interactive regions registered while painting (immediate-mode hit testing).
enum class Hit : uint8_t {
    None,
    Tab, TabClose, TabNew,
    RepoMenu, BranchMenu, ToolButton, MoreMenu,
    InlineEdit, DialogBlock, DialogButton, ToastClose,
    WipAction, CommitField, CommitAmend, CommitButton,
    DiffClose, DiffMode, DiffNav, DiffFileStage, DiffHunk, DiffBody, DiffLine, DiffLineAction,
    NavPage, SideHeader, SideItem, SettingsButton,
    SplitDetail, ColSep,
    HistorySearch, SearchClear, ScopeMenu, ReviewCommit,
    GraphBody, GraphLabel, RefPopup, RefPopupRow,
    Scroll,
    DetailMode, DetailSection, DetailFolder, DetailSha, DetailParent, DetailFile, DetailMore, DetailAction,
    StartOpen, StartRecent, StartRemove,
    Trust, Retry, CloseTab,
};

struct Zone {
    RectF r;
    Hit id = Hit::None;
    int a = 0, b = 0;
};

// Smooth scrolling state. Doubles keep sub-pixel precision with millions of rows.
struct Scroller {
    double pos = 0, target = 0, content = 0, view = 0;
    double Max() const { return std::max(0.0, content - view); }
    void Clamp() { target = std::clamp(target, 0.0, Max()); pos = std::clamp(pos, 0.0, Max()); }
    void By(double d) { target = std::clamp(target + d, 0.0, Max()); }
    void To(double v, bool snap) { target = std::clamp(v, 0.0, Max()); if (snap) pos = target; }
    bool Step(float dtMs);  // returns true while animating
};

// Scrollbar thumb geometry (shared by painting and dragging). False when nothing to scroll.
bool ScrollThumb(const RectF& area, const Scroller& s, RectF& thumb);

// Items of the navigation pane: pages, expandable sections with their branches, tags, stashes, remotes and
// submodules, and the recent branches.
enum class SideKind : uint8_t { Page, Header, Folder, Remote, Branch, Tag, Stash, Submodule, Divider, Caption, Recent, Empty };

struct SideItem {
    SideKind kind = SideKind::Empty;
    uint8_t depth = 0, section = 0;  // section: 0 branches, 1 remotes, 2 stashes, 3 tags, 4 submodules; page: 0 changes, 1 history
    bool expanded = false, head = false;
    int32_t index = -1;  // ref, stash or submodule index
    int count = 0;
    float top = 0, h = 0;  // position in the pane, DIP
    std::wstring text, sub, key;
};

struct FileRow {
    uint8_t section = 0;  // 0 commit files, 1 unstaged, 2 staged
    bool folder = false, expanded = true;
    uint8_t depth = 0;
    int32_t file = -1;
    std::wstring name, dir, key;
    std::wstring full;  // repository-relative path of the file or folder
};

// Text field editing: caret, selection, word jumps, clipboard. Multi-line fields wrap; the layout
// painted last is kept for mouse hit testing and Up/Down navigation.
struct TextEdit {
    std::wstring text;
    size_t caret = 0, anchor = 0;  // the selection spans [min, max) of the two
    bool multiline = false;
    size_t maxLen = 1000;
    float scroll = 0;              // horizontal (single line) or vertical (multi-line) scroll, pixels
    ComPtr<IDWriteTextLayout> layout;
    D2D1_POINT_2F origin{};        // where `layout` was drawn

    size_t SelStart() const { return std::min(caret, anchor); }
    size_t SelEnd() const { return std::max(caret, anchor); }
    bool HasSel() const { return caret != anchor; }
    void Set(const std::wstring& s) { text = s; caret = anchor = s.size(); scroll = 0; }
    void SelectAll() { anchor = 0; caret = text.size(); }
    void Insert(std::wstring_view s);                     // replaces the selection
    size_t HitTest(float x, float y) const;               // text index under a point
    void SelectWordAt(size_t i);
    bool OnKey(UINT vk, bool ctrl, bool shift, HWND clipboardOwner);  // true when the key was used
};

struct Toast {
    std::wstring title, message;
    bool error = false;
    double born = 0, ttl = 0;  // ttl 0 = stays until closed
};

struct DialogState {
    bool active = false;
    std::wstring title, message;
    std::vector<std::wstring> buttons;
    int defButton = 0;                  // activated by Enter
    int danger = -1;                    // button drawn as destructive
    std::function<void(int)> onResult;  // button index, -1 when cancelled
};

// ---- drawing shared by the windows (views.cpp)
void DrawLogo(Gfx& gfx, float cx, float cy, float s);
void DrawBranchIcon(Gfx& gfx, float cx, float cy, float s, const Color& c);
void DrawAvatar(Gfx& gfx, float cx, float cy, float r, const Author& a);
void DrawSpinner(Gfx& gfx, float cx, float cy, float r, const Color* c = nullptr);
// A button with a label: 0 standard (subtle fill and border), 1 accent, 2 subtle (fill on hover only).
void DrawButton(Gfx& gfx, const RectF& r, std::wstring_view label, int style, bool hot, bool enabled, F font = F::UI);
// A text box frame (WinUI): subtle fill and border, an accent bar at the bottom while it has the focus.
void TextBoxFrame(Gfx& gfx, const RectF& r, bool focused, bool error = false);
// A text field's content; masked shows bullets instead of the characters (tokens, passwords).
void DrawEdit(Gfx& gfx, TextEdit& e, const RectF& area, F font, std::wstring_view placeholder, bool focused, bool masked = false);
void DrawDialog(Gfx& gfx, const DialogState& dialog, int hotButton, std::vector<RectF>& buttons);

// The frame of a top-level window (dark or light, the Mangrove border) and its menus, by the palette in use.
void ThemeWindowFrame(HWND h, th::Theme theme);

// ---- the Settings window (settingswindow.cpp): General, Appearance, Git and Integrations (accounts)
void ShowSettings(int page = -1);  // 0 General, 1 Appearance, 2 Git, 3 Integrations; -1 the page shown last
void CloseSettings();
void RefreshSettings(bool theme);  // repaint it after a change made elsewhere (theme: the palette changed)
HWND SettingsWindowHandle();       // null while it is closed
void SettingsForSnapshot(int state);

// ---- pictures of the windows for the interface smoke test (snapshots.cpp, MANGROVE_SNAPSHOT)
void StartSnapshots();
bool SnapshotTimer(UINT_PTR id);  // true when the timer was the snapshots'

// How the diff of the session (RepoSession::diff) is shown: scrolling and the text layouts of its lines.
struct DiffViewUi {
    Scroller vs;
    float hscroll = 0;
    // Text layouts of the lines drawn recently, reused across frames.
    std::unordered_map<uint32_t, ComPtr<IDWriteTextLayout>> layouts;
    const FileDiff* layoutsFor = nullptr;
    float layoutsScale = 0;
    RectF body;  // last painted text area (drag selection scrolls past its edges)
};

// A tab: a repository session (shared with the macOS app) and how this window shows it.
struct RepoTab : RepoSession {
    std::weak_ptr<WorkStatus> countedStatus;  // the status changedFiles was counted for
    int changedFiles = 0;                     // distinct paths with changes, staged or not

    // history
    Scroller gs;
    int32_t hoverRow = -1;
    float graphW = 0;  // DIP, 0 = automatic
    TextEdit search{ L"", 0, 0, false, 200 };

    // navigation pane
    Scroller ss;
    std::unordered_set<std::wstring> collapsed;
    std::vector<SideItem> side;
    bool sideDirty = true;
    bool sideCompact = false;  // the pane `side` was built for

    // details panel
    Scroller ds;
    bool treeMode = false;
    std::unordered_set<std::wstring> collapsedDirs;
    bool unstagedCollapsed = false, stagedCollapsed = false;
    std::vector<FileRow> fileRows[3];  // per section: commit files, unstaged, staged
    const void* fileRowsSrc[3] = {};
    int fileRowsMode[3] = { -1, -1, -1 };
    uint64_t fileRowsVer[3] = {};
    uint64_t collapseVer = 1;

    DiffViewUi diffUi;

    // commit box (uncommitted changes)
    TextEdit commitSummary;
    TextEdit commitBody{ L"", 0, 0, true, 20000 };  // multi-line
    bool amend = false;
    std::wstring amendPrefill;  // message loaded for amend, to clear it again if left untouched
};

class App {
public:
    bool Create(HINSTANCE inst, int show);
    LRESULT Proc(HWND h, UINT m, WPARAM w, LPARAM l);
    void Invalidate() { if (hwnd) InvalidateRect(hwnd, nullptr, FALSE); }

    HWND hwnd = nullptr;
    Gfx gfx;

    // ---- views (views.cpp)
    void Paint();
    void Layout();
    void PaintTitle();
    void PaintCommandBar();
    void PaintStart(RepoTab& t, const RectF& r);
    void PaintLoading(RepoTab& t, const RectF& r);
    void PaintError(RepoTab& t, const RectF& r);
    void PaintNav(RepoTab& t);
    void PaintHistory(RepoTab& t);
    void PaintHistoryHeader(RepoTab& t, const RectF& r);
    void PaintRefPopup(RepoTab& t);
    void PaintDetails(RepoTab& t);
    void PaintCommitDetails(RepoTab& t, const RectF& r);
    void PaintWipDetails(RepoTab& t, const RectF& r);
    float PaintFileRows(RepoTab& t, const RectF& clip, float x, float w, float y, int section, const std::vector<FileChange>& files);
    void PaintScrollbar(const RectF& area, Scroller& s, int id);
    void PaintDebug();

    void Avatar(float cx, float cy, float r, const Author& a) { DrawAvatar(gfx, cx, cy, r, a); }
    void BranchIcon(float cx, float cy, float s, const Color& c) { DrawBranchIcon(gfx, cx, cy, s, c); }
    void FileIcon(wchar_t status, float cx, float cy);  // the change as a colored letter: M, A, D, R, U (untracked), C (conflict)
    void Logo(float cx, float cy, float s) { DrawLogo(gfx, cx, cy, s); }
    void Spinner(float cx, float cy, float r, const Color* c = nullptr) { DrawSpinner(gfx, cx, cy, r, c); }
    void Button(const RectF& r, std::wstring_view label, int style, bool hot, bool enabled, F font = F::UI) {
        DrawButton(gfx, r, label, style, hot, enabled, font);
    }

    struct Cols {
        float rowX, rowR;     // row highlight
        float refX, refW, graphX, graphW, msgX, statR;
    };
    Cols ComputeCols(RepoTab& t, const RectF& area);
    float LaneX(const Cols& c, int lane) const;

    // ---- zones / hover
    std::vector<Zone> zones;
    Zone hover;
    void AddZone(const RectF& r, Hit id, int a = 0, int b = 0) { zones.push_back({ r, id, a, b }); }
    const Zone* ZoneAt(float x, float y) const;
    bool Hov(Hit id, int a = 0) const { return hover.id == id && hover.a == a; }
    bool HovAny(Hit id) const { return hover.id == id; }

    // ---- layout (pixels)
    RectF rcTitle, rcToolbar, rcNav, rcCard, rcGraph, rcDetail, rcGraphBody;
    RectF caption;  // DWM caption buttons, client coordinates
    float detailW = 360;  // DIP
    float colRef = 188;
    bool sideVisible = true, detailVisible = true;  // navigation pane expanded (else icons only), details pane shown
    bool navCompact = false;                        // icons only: by choice or for lack of room
    float zoom = 1.f;
    UINT dpi = 96;
    float MainScale() const { return (dpi ? dpi : 96) / 96.f * zoom; }  // this window's g_S
    bool windowActive = true;

    // ---- tabs
    std::vector<std::shared_ptr<RepoTab>> tabs;
    int active = 0;
    RepoTab* Active() { return active >= 0 && active < (int)tabs.size() ? tabs[(size_t)active].get() : nullptr; }
    std::shared_ptr<RepoTab> ActivePtr() { return active >= 0 && active < (int)tabs.size() ? tabs[(size_t)active] : nullptr; }
    std::shared_ptr<RepoTab> PtrOf(const RepoTab* t);
    void NewTab(const std::wstring& path, bool activate = true);
    void CloseTab(int i);
    void ActivateTab(int i);
    void OpenPath(const std::wstring& path);
    void OpenDialog();

    // ---- sessions (app.cpp): each tab's RepoSession reports here
    std::shared_ptr<RepoTab> MakeTab(const std::wstring& path);
    void OnSessionEvent(RepoTab& t, SessionEvent e);
    void TrustActive();

    // ---- selection & navigation
    void SelectRow(RepoTab& t, int32_t row, bool ensureVisible = true);
    void SelectCommit(RepoTab& t, int32_t commit);
    void EnsureRowVisible(RepoTab& t, int32_t row);
    int32_t RowAt(const RepoTab& t, float y) const;
    void BuildSide(RepoTab& t);
    void BuildFileRows(RepoTab& t, int section, const std::vector<FileChange>& files, std::vector<FileRow>& out);
    void SideClick(RepoTab& t, int index);

    // ---- input
    void OnMouseMove(float x, float y);
    void OnMouseDown(float x, float y, bool dbl);
    void OnMouseUp(float x, float y);
    void OnRightClick(float x, float y);
    void OnMiddleClick(float x, float y);
    void OnWheel(float delta, float x, float y, bool horizontal);
    bool OnKey(UINT vk);
    void OnChar(wchar_t ch);
    void UpdateHover();
    LRESULT HitTestNC(LPARAM l);
    LPCWSTR CursorForHover() const;
    bool IsTipZone(Hit id) const;  // zones that show a tooltip
    float mx = -1, my = -1;
    bool tracking = false;
    bool searchFocus = false;  // the history search box

    enum class Drag { None, Detail, ColRef, ColGraph, ScrollGraph, ScrollSide, ScrollDetail, ScrollDiff, TextSelect, DiffSelect };
    Drag drag = Drag::None;
    int dragEdit = 0;   // text field being drag-selected (see EditById)
    int editFocus = 0;  // focused commit field of the active tab: 1 summary, 2 description
    TextEdit* EditById(int id);  // 1 summary, 2 description, 3 new-branch name, 4 search
    float dragStart = 0, dragOrig = 0, dragGrab = 0;
    RectF dragArea;
    RectF scrollArea[4];  // graph, sidebar, details, diff (set while painting)

    // ---- menus / commands
    void ShowRepoMenu(const RectF& r);
    void ShowBranchMenu(const RectF& r);
    void ShowCommitMenu(RepoTab& t, int32_t row, float x, float y);
    void ShowSideMenu(RepoTab& t, int index, float x, float y);
    void ShowRefMenu(RepoTab& t, int refIndex, float x, float y);
    void ShowSectionMenu(RepoTab& t, int section, const RectF& r);
    void ShowMoreMenu(const RectF& r);
    void ShowScopeMenu(const RectF& r);
    void ToolAction(int id);
    void CheckoutFromDetails(RepoTab& t);  // the "Check out" button: a branch at the commit, else the commit itself
    void ReviewChanges(RepoTab& t);        // select the uncommitted changes and focus the commit summary

    // ---- history search
    void SearchStep(RepoTab& t, int dir);  // select the next (1) or previous (-1) match

    // Branches and tags of a row, listed while their chip is hovered.
    struct RefPopupState {
        int32_t row = -1, commit = -1;
        RectF chip;         // chip on screen (set while painting)
        double since = 0;   // hover start: the list opens after a short delay
    } refPopup;

    // ---- theme
    th::Theme theme = th::Theme::System;
    std::wstring themeApplied;  // what ApplyTheme last produced (repeated system notifications change nothing)
    void ApplyTheme(bool force = false);  // palette, window frame and menus; also when Windows settings change

    // ---- git operations (actions.cpp): the session runs them; confirmations and messages are shown here
    enum WipAct { kStageRow = 1, kUnstageRow, kDiscardRow, kStageAll, kUnstageAll };
    void DoRevert(RepoTab& t, int32_t commit);
    void DoDiscard(RepoTab& t, const FileChange& f);
    void DoDropStash(int stashIndex);
    void DoCommit();
    void ToggleAmend(RepoTab& t);
    void ShowFileMenu(RepoTab& t, int section, int row, float x, float y);
    void FocusEdit(int id);  // 0 clears the commit field focus

    // ---- diff view (diffview.cpp)
    bool diffSplit = false;  // side-by-side instead of unified
    void OpenDiff(RepoTab& t, int section, const std::wstring& path);
    void CloseDiff(RepoTab& t);
    void SetDiffMode(bool split);
    void PaintDiff(RepoTab& t);
    void DiffJump(RepoTab& t, int dir);  // previous / next hunk
    bool DiffKey(RepoTab& t, UINT vk);
    // Stage or unstage (by the side shown), or discard from the working tree, a hunk or the selected lines.
    void DoHunkAction(RepoTab& t, int hunk, bool discard);
    void DoLinesAction(RepoTab& t, bool discard);
    void ApplyDiffPatch(RepoTab& t, const PatchJob& job);  // discarding asks first
    void SelectDiffLine(RepoTab& t, int32_t line, bool extend, bool toggle);
    void DragDiffSelection(RepoTab& t, float x, float y);
    void ClearDiffSelection(RepoTab& t);
    // Command bar buttons (Action): 0 undo, 1 redo, 2 pull, 3 push, 4 new branch, 5 stash, 6 pop, 7 terminal, 8 fetch.
    bool ToolEnabled(RepoTab* t, int button, std::wstring* tip) { return t && t->CanRun(button, tip); }
    void BeginBranchInput(RepoTab& t, int32_t commit);
    void CommitBranchInput();
    void CancelBranchInput();

    // inline "new branch" name field shown in the BRANCH / TAG column
    struct InlineInput {
        bool active = false;
        std::weak_ptr<RepoTab> tab;
        int32_t commit = -1;  // -1 = HEAD
        TextEdit edit;
        std::wstring error;
        RectF rect;           // last painted position
    } inlineInput;

    // notifications and modal dialog
    std::vector<Toast> toasts;
    DialogState dialog;
    void ShowToast(const std::wstring& title, const std::wstring& message, bool error);
    void ShowDialog(const std::wstring& title, const std::wstring& message, std::vector<std::wstring> buttons, int defButton,
                    int danger, std::function<void(int)> onResult);
    void ShowDialog(const Prompt& p, std::function<void(int)> onResult) {
        ShowDialog(p.title, p.message, p.buttons, p.defaultButton, p.danger, std::move(onResult));
    }
    void CloseDialog(int result);
    void PaintToasts();
    void PaintDialog();
    void PaintTooltip();
    void PaintInlineInput(RepoTab& t, const RectF& box);
    void PaintEdit(TextEdit& e, const RectF& area, F font, std::wstring_view placeholder, bool focused) {
        DrawEdit(gfx, e, area, font, placeholder, focused);
    }
    void PaintCommitBox(RepoTab& t, const RectF& r);
    float CommitBoxHeight() const;
    std::wstring tipText;  // tooltip of the hovered control
    RectF tipAnchor;
    double hoverSince = 0;
    void SetZoom(float z);
    void UpdateScale();
    void UpdateCaption();

    // ---- animation
    double lastFrame = 0;
    double frameMs = 0;  // CPU time spent building the last frame
    bool animating = false;

    // ---- settings (the shared Settings, plus this window's own keys)
    void LoadSettings();
    void SaveSettings();
    WINDOWPLACEMENT savedPlacement{};
    bool hasPlacement = false;
};

extern App* g_app;
