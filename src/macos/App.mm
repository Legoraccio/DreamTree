// The application: menus, the Settings window, opening repositories in windows and tabs, and the
// repositories reopened at launch.

#import "MacUI.h"

static DTAppDelegate* g_app;  // NSApplication keeps its delegate weakly

DTAppDelegate* App() { return g_app; }

int main(int argc, const char* argv[]) {
    (void)argc;
    (void)argv;
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        g_app = [[DTAppDelegate alloc] init];
        app.delegate = g_app;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
    }
    return 0;
}

// ---------------------------------------------------------------- Settings window

@interface DTSettingsController : NSWindowController
@end

@implementation DTSettingsController {
    NSPopUpButton* _pull;
    NSPopUpButton* _diff;
    NSTextField* _git;
}

- (instancetype)init {
    NSWindow* w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 460, 150)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                backing:NSBackingStoreBuffered
                                                  defer:YES];
    w.title = @"Settings";
    w.releasedWhenClosed = NO;
    if (!(self = [super initWithWindow:w])) return nil;
    _pull = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_pull addItemsWithTitles:@[ @"Merge (fast-forward if possible)", @"Fast-forward only", @"Rebase" ]];
    _diff = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_diff addItemsWithTitles:@[ @"Unified", @"Side by side" ]];
    for (NSPopUpButton* p in @[ _pull, _diff ]) {
        p.target = self;
        p.action = @selector(changed:);
    }
    _git = [NSTextField labelWithString:@"Checking…"];
    _git.textColor = NSColor.secondaryLabelColor;
    _git.selectable = YES;
    NSGridView* grid = [NSGridView gridViewWithViews:@[
        @[ [NSTextField labelWithString:@"Pull:"], _pull ],
        @[ [NSTextField labelWithString:@"Diffs:"], _diff ],
        @[ [NSTextField labelWithString:@"Git:"], _git ],
    ]];
    grid.translatesAutoresizingMaskIntoConstraints = NO;
    grid.rowSpacing = 12;
    grid.columnSpacing = 8;
    grid.rowAlignment = NSGridRowAlignmentFirstBaseline;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
    NSView* v = w.contentView;
    [v addSubview:grid];
    [NSLayoutConstraint activateConstraints:@[
        [grid.topAnchor constraintEqualToAnchor:v.topAnchor constant:24],
        [grid.bottomAnchor constraintEqualToAnchor:v.bottomAnchor constant:-24],
        [grid.leadingAnchor constraintEqualToAnchor:v.leadingAnchor constant:40],
        [grid.trailingAnchor constraintLessThanOrEqualToAnchor:v.trailingAnchor constant:-40],
    ]];
    return self;
}

- (void)showWindow:(id)sender {
    [_pull selectItemAtIndex:std::clamp(AppSettings().pullMode, 0, 2)];
    [_diff selectItemAtIndex:AppSettings().diffSplit ? 1 : 0];
    __weak NSTextField* git = _git;
    std::thread([git] {
        const std::wstring version = GitVersion(), path = GitPath(false);
        RunOnMain([git, version, path] {
            git.stringValue = version.empty() ? @"Not found: install the Xcode Command Line Tools or Git from Homebrew"
                                              : [NSString stringWithFormat:@"%@ (%@)", NS(version), NS(path)];
        });
    }).detach();
    if (!self.window.visible) [self.window center];
    [super showWindow:sender];
}

- (void)changed:(id)sender {
    Settings& s = AppSettings();
    s.pullMode = (int)_pull.indexOfSelectedItem;
    s.diffSplit = _diff.indexOfSelectedItem == 1;
    s.Save();
    [NSNotificationCenter.defaultCenter postNotificationName:DTSettingsChangedNotification object:nil];
}

@end

// ---------------------------------------------------------------- menus

static NSMenuItem* Item(NSMenu* m, NSString* title, SEL action, NSString* key,
                        NSEventModifierFlags mods = NSEventModifierFlagCommand) {
    NSMenuItem* i = [m addItemWithTitle:title action:action keyEquivalent:key ? key : @""];
    if (key) i.keyEquivalentModifierMask = mods;
    return i;
}

static NSMenu* Submenu(NSMenu* parent, NSString* title) {
    NSMenu* m = [[NSMenu alloc] initWithTitle:title];
    [parent addItemWithTitle:title action:nil keyEquivalent:@""].submenu = m;
    return m;
}

static constexpr NSEventModifierFlags kCmd = NSEventModifierFlagCommand, kShift = NSEventModifierFlagShift,
                                      kOpt = NSEventModifierFlagOption, kCtrl = NSEventModifierFlagControl;

// ---------------------------------------------------------------- the application

@implementation DTAppDelegate {
    NSMutableArray<DTRepoWindowController*>* _windows;
    DTSettingsController* _settings;
    NSMenu* _recent;
}

- (instancetype)init {
    if ((self = [super init])) _windows = [NSMutableArray array];
    return self;
}

- (void)applicationWillFinishLaunching:(NSNotification*)note {
    InstallMainDispatcher();
    AppSettings().Load();
    NSApp.mainMenu = [self mainMenu];
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
    std::thread([] { GitVersion(); }).detach();  // the first git call is the slow one
    [self restoreTabs];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {
    return NO;
}

- (BOOL)applicationShouldHandleReopen:(NSApplication*)app hasVisibleWindows:(BOOL)visible {
    if (!visible && _windows.count == 0) [self newWindow:nil];
    return YES;
}

- (BOOL)applicationSupportsSecureRestorableState:(NSApplication*)app {
    return YES;
}

- (void)applicationWillTerminate:(NSNotification*)note {
    [self saveSettings];
}

- (void)application:(NSApplication*)app openURLs:(NSArray<NSURL*>*)urls {
    DTRepoWindowController* origin = [self keyController];
    for (NSURL* u in urls) {
        NSNumber* dir = nil;
        [u getResourceValue:&dir forKey:NSURLIsDirectoryKey error:nil];
        if (dir.boolValue) [self openRepository:u.path from:origin];
    }
}

// The repositories open when DreamTree quit, as tabs of one window.
- (void)restoreTabs {
    const Settings& s = AppSettings();
    DTRepoWindowController* opened = _windows.lastObject;  // a folder opened at launch
    NSWindow* anchor = opened.window;
    DTRepoWindowController* active = nil;
    for (size_t i = 0; i < s.tabs.size(); ++i) {
        NSString* path = NS(s.tabs[i]);
        if (!DirExists(s.tabs[i]) || [self controllerFor:path]) continue;
        DTRepoWindowController* c = [self newController:path];
        if (anchor) [anchor.tabGroup addWindow:c.window];
        else {
            [c showWindow:nil];
            anchor = c.window;
        }
        if ((int)i == s.activeTab) active = c;
    }
    if (!anchor) {
        DTRepoWindowController* c = [self newController:nil];
        [c showWindow:nil];
        active = c;
    }
    DTRepoWindowController* front = opened ? opened : active;
    if (front) [front.window makeKeyAndOrderFront:nil];
}

- (DTRepoWindowController*)newController:(NSString*)path {
    DTRepoWindowController* c = [[DTRepoWindowController alloc] initWithPath:path];
    [_windows addObject:c];
    return c;
}

- (DTRepoWindowController*)keyController {
    id c = NSApp.keyWindow.windowController;
    if (![c isKindOfClass:DTRepoWindowController.class]) c = NSApp.mainWindow.windowController;
    return [c isKindOfClass:DTRepoWindowController.class] ? c : nil;
}

- (DTRepoWindowController*)controllerFor:(NSString*)path {
    const std::wstring p = NormalizePath(WS(path.stringByStandardizingPath));
    for (DTRepoWindowController* c in _windows) {
        const RepoSession* s = c.session;
        if (s && (s->path == p || (s->data && s->data->root == p))) return c;
    }
    return nil;
}

- (void)openRepository:(NSString*)path from:(DTRepoWindowController*)origin {
    path = path.stringByStandardizingPath;
    if (DTRepoWindowController* open = [self controllerFor:path]) {
        [open.window makeKeyAndOrderFront:nil];
        return;
    }
    if (origin && !origin.session) {  // the start page becomes the repository
        [origin openRepository:path];
        [origin.window makeKeyAndOrderFront:nil];
        return;
    }
    DTRepoWindowController* c = [self newController:path];
    if (origin.window.visible) [origin.window addTabbedWindow:c.window ordered:NSWindowAbove];
    [c.window makeKeyAndOrderFront:nil];
}

- (void)windowClosed:(DTRepoWindowController*)controller {
    // Later: the controller is still running the close.
    dispatch_async(dispatch_get_main_queue(), ^{
        [self->_windows removeObject:controller];
        [self saveSettings];
    });
}

- (void)saveSettings {
    Settings& s = AppSettings();
    s.tabs.clear();
    s.activeTab = 0;
    NSWindow* key = NSApp.keyWindow ? NSApp.keyWindow : NSApp.mainWindow;
    NSMutableSet<NSWindow*>* seen = [NSMutableSet set];
    auto add = [&](NSWindow* w) {
        if ([seen containsObject:w]) return;
        [seen addObject:w];
        DTRepoWindowController* c = [w.windowController isKindOfClass:DTRepoWindowController.class] ? w.windowController : nil;
        if (!c || ![_windows containsObject:c] || !c.session) return;
        if (w == key) s.activeTab = (int)s.tabs.size();
        s.tabs.push_back(c.session->path);
    };
    for (NSWindow* w in NSApp.orderedWindows) {  // front to back, each with its tabs in order
        NSArray<NSWindow*>* tabs = w.tabbedWindows;
        if (tabs) for (NSWindow* t in tabs) add(t);
        else add(w);
    }
    for (DTRepoWindowController* c in _windows) add(c.window);  // minimized windows
    s.Save();
}

// ---- commands

- (IBAction)newWindow:(id)sender {
    [[self newController:nil] showWindow:nil];
}

- (IBAction)newWindowForTab:(id)sender {
    DTRepoWindowController* c = [self newController:nil];
    NSWindow* key = [self keyController].window;
    if (key.visible) [key addTabbedWindow:c.window ordered:NSWindowAbove];
    [c.window makeKeyAndOrderFront:nil];
}

- (IBAction)openDocument:(id)sender {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = YES;
    panel.canChooseFiles = NO;
    panel.allowsMultipleSelection = YES;
    panel.prompt = @"Open";
    panel.message = @"Choose the folder of a Git repository.";
    DTRepoWindowController* origin = [self keyController];
    __weak NSOpenPanel* weakPanel = panel;
    void (^done)(NSModalResponse) = ^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        for (NSURL* url in weakPanel.URLs) [self openRepository:url.path from:origin];
    };
    if (origin.window.visible) [panel beginSheetModalForWindow:origin.window completionHandler:done];
    else [panel beginWithCompletionHandler:done];
}

- (void)openRecent:(NSMenuItem*)item {
    [self openRepository:item.representedObject from:[self keyController]];
}

- (void)clearRecent:(id)sender {
    AppSettings().recent.clear();
    AppSettings().Save();
}

- (IBAction)showSettings:(id)sender {
    if (!_settings) _settings = [[DTSettingsController alloc] init];
    [_settings showWindow:nil];
}

- (IBAction)setPullMode:(NSMenuItem*)sender {
    AppSettings().pullMode = (int)sender.tag;
    AppSettings().Save();
    [NSNotificationCenter.defaultCenter postNotificationName:DTSettingsChangedNotification object:nil];
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if (item.action == @selector(setPullMode:)) item.state = item.tag == AppSettings().pullMode ? NSControlStateValueOn : NSControlStateValueOff;
    return YES;
}

// Open Recent, filled when it opens.
- (void)menuNeedsUpdate:(NSMenu*)menu {
    if (menu != _recent) return;
    [menu removeAllItems];
    const std::vector<std::wstring>& recent = AppSettings().recent;
    for (size_t i = 0; i < recent.size() && i < 15; ++i) {
        NSString* path = NS(recent[i]);
        NSMenuItem* it = [menu addItemWithTitle:path.lastPathComponent action:@selector(openRecent:) keyEquivalent:@""];
        it.target = self;
        it.representedObject = path;
        it.toolTip = path.stringByAbbreviatingWithTildeInPath;
        it.image = [NSWorkspace.sharedWorkspace iconForFile:path];
        it.image.size = NSMakeSize(16, 16);
    }
    if (menu.numberOfItems) [menu addItem:NSMenuItem.separatorItem];
    NSMenuItem* clear = [menu addItemWithTitle:@"Clear Menu" action:@selector(clearRecent:) keyEquivalent:@""];
    clear.target = self;
    clear.enabled = !recent.empty();
}

- (NSMenu*)mainMenu {
    NSMenu* bar = [[NSMenu alloc] initWithTitle:@"Main Menu"];

    NSMenu* app = Submenu(bar, @"DreamTree");
    Item(app, @"About DreamTree", @selector(orderFrontStandardAboutPanel:), nil);
    [app addItem:NSMenuItem.separatorItem];
    Item(app, @"Settings…", @selector(showSettings:), @",");
    [app addItem:NSMenuItem.separatorItem];
    NSMenu* services = Submenu(app, @"Services");
    NSApp.servicesMenu = services;
    [app addItem:NSMenuItem.separatorItem];
    Item(app, @"Hide DreamTree", @selector(hide:), @"h");
    Item(app, @"Hide Others", @selector(hideOtherApplications:), @"h", kCmd | kOpt);
    Item(app, @"Show All", @selector(unhideAllApplications:), nil);
    [app addItem:NSMenuItem.separatorItem];
    Item(app, @"Quit DreamTree", @selector(terminate:), @"q");

    NSMenu* file = Submenu(bar, @"File");
    Item(file, @"New Window", @selector(newWindow:), @"n");
    Item(file, @"New Tab", @selector(newWindowForTab:), @"t");
    Item(file, @"Open…", @selector(openDocument:), @"o");
    _recent = Submenu(file, @"Open Recent");
    _recent.delegate = self;
    [file addItem:NSMenuItem.separatorItem];
    Item(file, @"Close", @selector(performClose:), @"w");

    NSMenu* edit = Submenu(bar, @"Edit");
    Item(edit, @"Undo", @selector(undo:), @"z");
    Item(edit, @"Redo", @selector(redo:), @"z", kCmd | kShift);
    [edit addItem:NSMenuItem.separatorItem];
    Item(edit, @"Cut", @selector(cut:), @"x");
    Item(edit, @"Copy", @selector(copy:), @"c");
    Item(edit, @"Paste", @selector(paste:), @"v");
    Item(edit, @"Select All", @selector(selectAll:), @"a");
    [edit addItem:NSMenuItem.separatorItem];
    NSMenu* find = Submenu(edit, @"Find");
    Item(find, @"Find Commits…", @selector(focusSearch:), @"f");
    Item(find, @"Find Next", @selector(findNextMatch:), @"g");
    Item(find, @"Find Previous", @selector(findPreviousMatch:), @"g", kCmd | kShift);

    NSMenu* view = Submenu(bar, @"View");
    Item(view, @"Show Sidebar", @selector(toggleSidebar:), @"s", kCmd | kCtrl);
    Item(view, @"Hide Details", @selector(toggleDetails:), @"i", kCmd | kOpt);
    Item(view, @"Hide Toolbar", @selector(toggleToolbarShown:), @"t", kCmd | kOpt);
    Item(view, @"Customize Toolbar…", @selector(runToolbarCustomizationPalette:), nil);
    [view addItem:NSMenuItem.separatorItem];
    Item(view, @"History", @selector(showHistoryPage:), @"1");
    Item(view, @"Uncommitted Changes", @selector(showChangesPage:), @"2");
    Item(view, @"Current Branch Only", @selector(toggleHeadOnly:), @"b", kCmd | kOpt);
    [view addItem:NSMenuItem.separatorItem];
    Item(view, @"Unified Diff", @selector(setDiffLayout:), nil).tag = 0;
    Item(view, @"Side-by-Side Diff", @selector(setDiffLayout:), nil).tag = 1;
    [view addItem:NSMenuItem.separatorItem];
    Item(view, @"Enter Full Screen", @selector(toggleFullScreen:), @"f", kCmd | kCtrl);

    NSMenu* repo = Submenu(bar, @"Repository");
    Item(repo, @"Fetch", @selector(fetch:), @"f", kCmd | kShift);
    Item(repo, @"Pull", @selector(pull:), @"p", kCmd | kShift);
    Item(repo, @"Push", @selector(push:), @"p");
    [repo addItem:NSMenuItem.separatorItem];
    Item(repo, @"New Branch…", @selector(newBranch:), @"n", kCmd | kShift);
    Item(repo, @"Stash Changes", @selector(stash:), @"s", kCmd | kOpt);
    Item(repo, @"Pop Stash", @selector(popStash:), @"s", kCmd | kOpt | kShift);
    [repo addItem:NSMenuItem.separatorItem];
    Item(repo, @"Undo", @selector(undoGit:), @"z", kCmd | kOpt);
    Item(repo, @"Redo", @selector(redoGit:), @"z", kCmd | kOpt | kShift);
    [repo addItem:NSMenuItem.separatorItem];
    NSMenu* pull = Submenu(repo, @"Pull Mode");
    Item(pull, @"Merge (Fast-Forward If Possible)", @selector(setPullMode:), nil).tag = 0;
    Item(pull, @"Fast-Forward Only", @selector(setPullMode:), nil).tag = 1;
    Item(pull, @"Rebase", @selector(setPullMode:), nil).tag = 2;
    [repo addItem:NSMenuItem.separatorItem];
    Item(repo, @"Refresh", @selector(refresh:), @"r");
    Item(repo, @"Open in Terminal", @selector(openTerminal:), nil);
    Item(repo, @"Show in Finder", @selector(revealRepository:), nil);
    Item(repo, @"Copy Repository Path", @selector(copyRepositoryPath:), nil);

    NSMenu* window = Submenu(bar, @"Window");
    Item(window, @"Minimize", @selector(performMiniaturize:), @"m");
    Item(window, @"Zoom", @selector(performZoom:), nil);
    [window addItem:NSMenuItem.separatorItem];
    Item(window, @"Bring All to Front", @selector(arrangeInFront:), nil);
    NSApp.windowsMenu = window;

    NSApp.helpMenu = Submenu(bar, @"Help");  // the system puts its menu search here
    return bar;
}

@end
