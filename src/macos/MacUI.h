#pragma once

// The macOS interface: AppKit, in Objective-C++ so it calls the shared core (src/core) directly. Each window
// (or window tab) shows one repository through a RepoSession; this layer only presents it and asks before
// actions that lose data. Texts and questions come from present.h, as on Windows.

#import <Cocoa/Cocoa.h>

#include "navtree.h"
#include "platform.h"
#include "present.h"
#include "session.h"
#include "settings.h"
#include "text.h"

// SDK features newer than the deployment target (macOS 12) are compiled only with an SDK that has them,
// and used only on systems that have them (@available).
#if defined(__MAC_14_0) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_14_0
#define MG_SDK_14 1
#else
#define MG_SDK_14 0
#endif
#if defined(__MAC_26_0) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_26_0
#define MG_SDK_26 1
#else
#define MG_SDK_26 0
#endif

// ---------------------------------------------------------------- helpers (Bridge.mm)

NSString* NS(std::wstring_view s);  // wide text (UTF-32 on macOS)
NSString* NSUtf8(std::string_view s);
std::wstring WS(NSString* s);
NSUInteger UTF16Length(std::wstring_view s);  // the NSString length of wide text

NSColor* LaneColor(int lane);          // graph lanes; lane 0 is the accent color
NSColor* StatusColor(wchar_t status);  // the color of a file's status letter
NSColor* AddedColor();
NSColor* DeletedColor();
NSImage* Symbol(NSString* name, CGFloat pointSize, NSFontWeight weight);  // an SF Symbol, as a template image
void DrawSymbol(NSString* name, NSRect r, CGFloat pointSize, NSColor* color);  // tinted, centered in r
void DrawText(NSString* s, NSRect r, NSFont* font, NSColor* color);           // one line, truncated, centered vertically
CGFloat TextWidth(NSString* s, NSFont* font);
void DrawAvatar(NSRect r, const Author& a);
NSTextField* MakeLabel(NSString* text, NSFont* font, NSColor* color);
void RunPrompt(NSWindow* window, const Prompt& p, void (^done)(int button));  // a sheet; done gets the button's index
void InstallMainDispatcher();  // RunOnMain posts to the main queue

extern NSString* const MGSettingsChangedNotification;  // pull mode or diff layout changed

// A view that opens the folders dropped on it.
@interface MGDropView : NSView
@property(nonatomic, copy) void (^onDrop)(NSArray<NSString*>* folders);
@end

// ---------------------------------------------------------------- controllers

@class MGRepoWindowController;

// The navigation sidebar: pages, branches, remotes, tags, stashes, submodules (Sidebar.mm).
@interface MGSidebarController : NSViewController <NSOutlineViewDataSource, NSOutlineViewDelegate, NSMenuDelegate>
- (instancetype)initWithOwner:(MGRepoWindowController*)owner;
- (void)reload;         // new data
- (void)refreshCounts;  // the working tree changed
- (void)syncSelection;  // follow the history selection
@end

// The history: branch and tag chips, the graph, two-line commit rows, lines changed (History.mm).
@interface MGHistoryController : NSViewController <NSTableViewDataSource, NSTableViewDelegate, NSMenuDelegate>
- (instancetype)initWithOwner:(MGRepoWindowController*)owner;
- (void)reload;                         // new data or graph
- (void)syncSelection;                  // select the session's row
- (void)revealSelection;                // scroll the selected row into view
- (void)redrawRows;                     // search results changed
- (void)reloadStats;                    // line counts arrived
- (void)statusChanged:(int)wipRowDelta; // the working tree changed
- (void)focusTable;
@end

// Commit details, or the uncommitted changes with the commit message (Details.mm).
@interface MGDetailsController : NSViewController <NSOutlineViewDataSource, NSOutlineViewDelegate, NSMenuDelegate, NSTextFieldDelegate,
                                                   NSTextViewDelegate>
- (instancetype)initWithOwner:(MGRepoWindowController*)owner;
- (void)reload;           // the selection, its details or the working tree changed
- (void)refreshState;     // operations started or ended, the diff opened or closed
- (void)applySuggestion;  // the session offers a commit message
- (void)clearMessage;     // committed
- (void)focusSummary;
@end

// The diff of one file, with hunk and line staging (DiffView.mm).
@interface MGDiffController : NSViewController
- (instancetype)initWithOwner:(MGRepoWindowController*)owner;
- (void)reload;  // the diff, its layout or its selection changed
@end

// One repository in a window, or in a tab of a window (RepoWindow.mm).
@interface MGRepoWindowController : NSWindowController <NSWindowDelegate, NSToolbarDelegate, NSSearchFieldDelegate, NSMenuDelegate,
                                                        NSMenuItemValidation, NSToolbarItemValidation>
- (instancetype)initWithPath:(NSString*)path;  // nil: the start page
@property(nonatomic, readonly) RepoSession* session;  // null on the start page
- (void)openRepository:(NSString*)path;

// selection
- (void)selectRow:(NSInteger)row;      // the history table changed its selection
- (void)selectCommit:(int32_t)commit;  // and scrolls the history to it
- (void)selectWorkingTree;
- (void)reviewChanges;                 // the uncommitted changes, with the cursor in the commit message
- (void)setHeadOnly:(BOOL)headOnly;

// actions; the ones that lose data ask first
- (void)openDiff:(int)section path:(const std::wstring&)path;
- (void)checkoutRef:(int)ref;
- (void)checkoutCommit:(int32_t)commit;  // a branch at the commit, else the commit itself
- (void)newBranchAt:(int32_t)commit;     // asks for the name; -1: at HEAD
- (void)revertCommit:(int32_t)commit;
- (void)dropStash:(int)index;
- (void)discardFile:(const FileChange&)file;
- (void)stagePaths:(const std::vector<std::wstring>&)paths stage:(BOOL)stage;
- (void)applyPatch:(const PatchJob&)job;
- (void)copyText:(NSString*)text;
- (void)revealFile:(const std::wstring&)relativePath;
- (void)fillCommitMenu:(NSMenu*)menu commit:(int32_t)commit;  // -1: the uncommitted changes
- (void)fillRefMenu:(NSMenu*)menu ref:(int)ref;
- (void)fillStashMenu:(NSMenu*)menu stash:(int)index;

// menu and toolbar commands
- (IBAction)fetch:(id)sender;
- (IBAction)pull:(id)sender;
- (IBAction)push:(id)sender;
- (IBAction)newBranch:(id)sender;
- (IBAction)stash:(id)sender;
- (IBAction)popStash:(id)sender;
- (IBAction)undoGit:(id)sender;
- (IBAction)redoGit:(id)sender;
- (IBAction)refresh:(id)sender;
- (IBAction)openTerminal:(id)sender;
- (IBAction)revealRepository:(id)sender;
- (IBAction)copyRepositoryPath:(id)sender;
- (IBAction)focusSearch:(id)sender;
- (IBAction)findNextMatch:(id)sender;
- (IBAction)findPreviousMatch:(id)sender;
- (IBAction)toggleDetails:(id)sender;
- (IBAction)toggleHeadOnly:(id)sender;
- (IBAction)showHistoryPage:(id)sender;
- (IBAction)showChangesPage:(id)sender;
- (IBAction)setDiffLayout:(id)sender;  // tag 0 unified, 1 side by side
- (IBAction)closeDiff:(id)sender;
@end

// Settings, by category: General, Appearance, Git, Integrations (Preferences.mm).
@interface MGPreferencesController : NSWindowController
+ (instancetype)shared;
- (void)showPane:(NSString*)identifier;  // general, appearance, git, integrations
- (void)openAddAccount;                  // the sheet that connects an account
- (void)closeSheets;
- (NSWindow*)sheetWindow;                // the open sheet, or nil
- (void)showSignInSample;                // the sheet signing in with the browser, with a sample code (interface test)
@end

void ApplyAppearance();  // light, dark or the system's, as the settings say

@interface MGAppDelegate : NSObject <NSApplicationDelegate, NSMenuDelegate, NSMenuItemValidation>
- (void)openRepository:(NSString*)path from:(MGRepoWindowController*)origin;
- (void)windowClosed:(MGRepoWindowController*)controller;
- (void)saveSettings;
- (IBAction)newWindow:(id)sender;
- (IBAction)newWindowForTab:(id)sender;
- (IBAction)openDocument:(id)sender;
- (IBAction)showSettings:(id)sender;
- (IBAction)setPullMode:(id)sender;  // tag: Settings::pullMode
@end

MGAppDelegate* App();
