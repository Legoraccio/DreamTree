// One repository window (or window tab): the toolbar, the split view with the sidebar, the history or the
// diff, and the details; the session's events; the questions asked before actions that lose data.

#import "MacUI.h"

static NSToolbarItemIdentifier const kFetch = @"fetch", kPull = @"pull", kPush = @"push", kBranch = @"branch", kStash = @"stash",
                                     kPop = @"pop", kMore = @"more", kSearch = @"search", kActivity = @"activity", kDetails = @"details";

// ---------------------------------------------------------------- notices

// A short message at the bottom of the history: the outcome of an operation.
@interface MGBanner : NSVisualEffectView
- (void)showTitle:(NSString*)title message:(NSString*)message error:(BOOL)error;
@end

@implementation MGBanner {
    NSImageView* _icon;
    NSTextField* _title;
    NSTextField* _message;
    NSUInteger _shown;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.material = NSVisualEffectMaterialPopover;
    self.blendingMode = NSVisualEffectBlendingModeWithinWindow;
    self.state = NSVisualEffectStateActive;
    self.wantsLayer = YES;
    self.layer.cornerRadius = 10;
    self.layer.masksToBounds = YES;
    self.hidden = YES;
    _icon = [[NSImageView alloc] init];
    _title = MakeLabel(@"", [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold], NSColor.labelColor);
    _message = [NSTextField wrappingLabelWithString:@""];
    _message.font = [NSFont systemFontOfSize:12];
    _message.textColor = NSColor.secondaryLabelColor;
    _message.preferredMaxLayoutWidth = 380;
    _message.maximumNumberOfLines = 5;
    NSStackView* text = [NSStackView stackViewWithViews:@[ _title, _message ]];
    text.orientation = NSUserInterfaceLayoutOrientationVertical;
    text.alignment = NSLayoutAttributeLeading;
    text.spacing = 2;
    NSStackView* row = [NSStackView stackViewWithViews:@[ _icon, text ]];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.alignment = NSLayoutAttributeTop;
    row.spacing = 10;
    row.edgeInsets = NSEdgeInsetsMake(10, 12, 11, 16);
    row.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:row];
    [NSLayoutConstraint activateConstraints:@[
        [row.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [row.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [row.topAnchor constraintEqualToAnchor:self.topAnchor],
        [row.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
    ]];
    return self;
}

- (void)showTitle:(NSString*)title message:(NSString*)message error:(BOOL)error {
    _title.stringValue = title;
    _message.stringValue = message;
    _message.hidden = message.length == 0;
    _icon.image = Symbol(error ? @"exclamationmark.triangle.fill" : @"checkmark.circle.fill", 15, NSFontWeightRegular);
    _icon.contentTintColor = error ? NSColor.systemOrangeColor : NSColor.systemGreenColor;
    self.alphaValue = 1;
    self.hidden = NO;
    const NSUInteger shown = ++_shown;
    __weak MGBanner* weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((error ? 8 : 4) * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        MGBanner* me = weakSelf;
        if (me && me->_shown == shown) [me dismiss];
    });
}

- (void)dismiss {
    ++_shown;
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext* ctx) {
        ctx.duration = 0.25;
        self.animator.alphaValue = 0;
    }
        completionHandler:^{
            if (self.alphaValue < 0.01) self.hidden = YES;
        }];
}

- (void)mouseDown:(NSEvent*)event {
    [self dismiss];
}

@end

// ---------------------------------------------------------------- the center of the window

// Holds the history, the diff or a message (loading, errors), one at a time, and the notices above them.
@interface MGCenterController : NSViewController
@property(nonatomic, readonly) MGBanner* banner;
@property(nonatomic, weak) MGRepoWindowController* owner;
- (void)show:(NSView*)pane;
- (void)remove:(NSView*)pane;
@end

@implementation MGCenterController {
    NSMutableArray<NSView*>* _panes;
}

- (void)loadView {
    MGDropView* v = [[MGDropView alloc] initWithFrame:NSMakeRect(0, 0, 720, 520)];
    __weak MGCenterController* weakSelf = self;
    v.onDrop = ^(NSArray<NSString*>* folders) {
        for (NSString* f in folders) [App() openRepository:f from:weakSelf.owner];
    };
    self.view = v;
    _panes = [NSMutableArray array];
    _banner = [[MGBanner alloc] initWithFrame:NSZeroRect];
    _banner.translatesAutoresizingMaskIntoConstraints = NO;
    [v addSubview:_banner];
    [NSLayoutConstraint activateConstraints:@[
        [_banner.centerXAnchor constraintEqualToAnchor:v.centerXAnchor],
        [_banner.bottomAnchor constraintEqualToAnchor:v.bottomAnchor constant:-18],
        [_banner.leadingAnchor constraintGreaterThanOrEqualToAnchor:v.leadingAnchor constant:16],
    ]];
}

- (void)show:(NSView*)pane {
    NSView* v = self.view;
    if (pane && ![_panes containsObject:pane]) {
        pane.translatesAutoresizingMaskIntoConstraints = NO;
        [v addSubview:pane positioned:NSWindowBelow relativeTo:_banner];
        [NSLayoutConstraint activateConstraints:@[
            [pane.leadingAnchor constraintEqualToAnchor:v.leadingAnchor],
            [pane.trailingAnchor constraintEqualToAnchor:v.trailingAnchor],
            [pane.topAnchor constraintEqualToAnchor:v.topAnchor],
            [pane.bottomAnchor constraintEqualToAnchor:v.bottomAnchor],
        ]];
        [_panes addObject:pane];
    }
    for (NSView* p in _panes) p.hidden = p != pane;
}

- (void)remove:(NSView*)pane {
    if (!pane) return;
    [pane removeFromSuperview];
    [_panes removeObject:pane];
}

@end

// A centered message with optional buttons: loading, or a repository that can't be opened.
static NSView* MessageView(NSString* title, NSString* detail, BOOL spinner, NSArray<NSButton*>* buttons) {
    NSMutableArray<NSView*>* views = [NSMutableArray array];
    if (spinner) {
        NSProgressIndicator* p = [[NSProgressIndicator alloc] init];
        p.style = NSProgressIndicatorStyleSpinning;
        [p startAnimation:nil];
        [views addObject:p];
    }
    NSFont* font = spinner ? [NSFont systemFontOfSize:13] : [NSFont systemFontOfSize:17 weight:NSFontWeightSemibold];
    NSTextField* t = MakeLabel(title, font, spinner ? NSColor.secondaryLabelColor : NSColor.labelColor);
    t.alignment = NSTextAlignmentCenter;
    [views addObject:t];
    if (detail.length) {
        NSTextField* d = [NSTextField wrappingLabelWithString:detail];
        d.textColor = NSColor.secondaryLabelColor;
        d.alignment = NSTextAlignmentCenter;
        d.preferredMaxLayoutWidth = 440;
        d.selectable = YES;
        [views addObject:d];
    }
    if (buttons.count) {
        NSStackView* row = [NSStackView stackViewWithViews:buttons];
        row.spacing = 10;
        [views addObject:row];
    }
    NSStackView* stack = [NSStackView stackViewWithViews:views];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeCenterX;
    stack.spacing = 12;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    NSView* v = [[NSView alloc] init];
    [v addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.centerXAnchor constraintEqualToAnchor:v.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:v.centerYAnchor],
        [stack.leadingAnchor constraintGreaterThanOrEqualToAnchor:v.leadingAnchor constant:24],
    ]];
    return v;
}

// ---------------------------------------------------------------- start page

@interface MGStartController : NSViewController
- (instancetype)initWithOwner:(MGRepoWindowController*)owner;
@end

@implementation MGStartController {
    __weak MGRepoWindowController* _owner;
}

- (instancetype)initWithOwner:(MGRepoWindowController*)owner {
    if ((self = [super initWithNibName:nil bundle:nil])) _owner = owner;
    return self;
}

- (void)loadView {
    MGDropView* v = [[MGDropView alloc] initWithFrame:NSMakeRect(0, 0, 900, 600)];
    __weak MGRepoWindowController* owner = _owner;
    v.onDrop = ^(NSArray<NSString*>* folders) {
        for (NSString* f in folders) [App() openRepository:f from:owner];
    };
    NSImageView* icon = [NSImageView imageViewWithImage:NSApp.applicationIconImage];
    [icon.widthAnchor constraintEqualToConstant:96].active = YES;
    [icon.heightAnchor constraintEqualToConstant:96].active = YES;
    NSTextField* title = MakeLabel(@"Mangrove", [NSFont systemFontOfSize:26 weight:NSFontWeightBold], NSColor.labelColor);
    NSTextField* sub = MakeLabel(@"Open the folder of a Git repository, or drop it here.", [NSFont systemFontOfSize:13], NSColor.secondaryLabelColor);
    NSButton* open = [NSButton buttonWithTitle:@"Open Repository…" target:nil action:@selector(openDocument:)];
    open.keyEquivalent = @"\r";
    open.controlSize = NSControlSizeLarge;
    NSMutableArray<NSView*>* views = [NSMutableArray arrayWithObjects:icon, title, sub, open, nil];

    const std::vector<std::wstring>& recent = AppSettings().recent;
    NSMutableArray<NSView*>* rows = [NSMutableArray array];
    for (size_t i = 0; i < recent.size() && rows.count < 8; ++i) {
        if (!DirExists(recent[i])) continue;
        NSString* path = NS(recent[i]);
        NSMutableAttributedString* text = [[NSMutableAttributedString alloc]
            initWithString:path.lastPathComponent
                attributes:@{ NSFontAttributeName : [NSFont systemFontOfSize:13 weight:NSFontWeightMedium], NSForegroundColorAttributeName : NSColor.labelColor }];
        [text appendAttributedString:[[NSAttributedString alloc]
                                         initWithString:[@"   " stringByAppendingString:path.stringByDeletingLastPathComponent.stringByAbbreviatingWithTildeInPath]
                                             attributes:@{ NSFontAttributeName : [NSFont systemFontOfSize:11], NSForegroundColorAttributeName : NSColor.secondaryLabelColor }]];
        NSButton* b = [NSButton buttonWithTitle:@"" target:self action:@selector(openRecent:)];
        b.bordered = NO;
        b.attributedTitle = text;
        b.alignment = NSTextAlignmentLeft;
        b.image = [NSWorkspace.sharedWorkspace iconForFile:path];
        b.image.size = NSMakeSize(18, 18);
        b.imagePosition = NSImageLeading;
        b.imageHugsTitle = YES;
        b.toolTip = path;
        b.identifier = path;
        b.lineBreakMode = NSLineBreakByTruncatingMiddle;
        [b.widthAnchor constraintLessThanOrEqualToConstant:460].active = YES;
        [rows addObject:b];
    }
    if (rows.count) {
        NSTextField* caption = MakeLabel(@"Recent", [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold], NSColor.tertiaryLabelColor);
        NSStackView* list = [NSStackView stackViewWithViews:[@[ caption ] arrayByAddingObjectsFromArray:rows]];
        list.orientation = NSUserInterfaceLayoutOrientationVertical;
        list.alignment = NSLayoutAttributeLeading;
        list.spacing = 8;
        [views addObject:list];
    }
    NSStackView* stack = [NSStackView stackViewWithViews:views];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeCenterX;
    stack.spacing = 14;
    [stack setCustomSpacing:28 afterView:open];
    [stack setCustomSpacing:4 afterView:title];
    [stack setCustomSpacing:22 afterView:sub];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [v addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.centerXAnchor constraintEqualToAnchor:v.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:v.centerYAnchor],
    ]];
    self.view = v;
}

- (void)openRecent:(NSButton*)b {
    [App() openRepository:b.identifier from:_owner];
}

@end

// ---------------------------------------------------------------- new branch

// Asks for the name of a new branch, checking it while it is typed.
@interface MGBranchPrompt : NSObject <NSTextFieldDelegate>
@end

@implementation MGBranchPrompt {
    std::weak_ptr<RepoSession> _session;
    int32_t _commit;
    NSAlert* _alert;
    NSTextField* _field;
    NSTextField* _error;
}

- (instancetype)initWithSession:(const std::shared_ptr<RepoSession>&)session commit:(int32_t)commit {
    if ((self = [super init])) {
        _session = session;
        _commit = commit;
    }
    return self;
}

- (void)runInWindow:(NSWindow*)window done:(void (^)(NSString* error))done {
    std::shared_ptr<RepoSession> s = _session.lock();
    if (!s || !s->data) return;
    const RepoData& d = *s->data;
    const int32_t at = _commit >= 0 ? _commit : d.headCommit;
    NSString* where = at >= 0 && at < (int32_t)d.commits.size()
                          ? [NSString stringWithFormat:@"The branch starts at %@ “%@” and is checked out.", NS(d.commits[(size_t)at].id.Hex(7)),
                                                       NS(CommitSubject(d, d.commits[(size_t)at]))]
                          : @"The branch is checked out.";
    _alert = [[NSAlert alloc] init];
    _alert.messageText = @"New Branch";
    _alert.informativeText = where;
    [_alert addButtonWithTitle:@"Create Branch"];
    [_alert addButtonWithTitle:@"Cancel"];
    _field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 22, 300, 24)];
    _field.placeholderString = @"feature/name";
    _field.delegate = self;
    _error = MakeLabel(@"", [NSFont systemFontOfSize:11], NSColor.systemRedColor);
    _error.frame = NSMakeRect(0, 0, 300, 16);
    NSView* box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 300, 46)];
    [box addSubview:_field];
    [box addSubview:_error];
    _alert.accessoryView = box;
    _alert.buttons[0].enabled = NO;
    [_alert layout];
    _alert.window.initialFirstResponder = _field;
    MGBranchPrompt* keep = self;  // alive until the sheet ends
    [_alert beginSheetModalForWindow:window
                   completionHandler:^(NSModalResponse r) {
                       if (r != NSAlertFirstButtonReturn) return;
                       std::shared_ptr<RepoSession> session = keep->_session.lock();
                       if (!session) return;
                       const std::wstring err = session->CreateBranch(WS(keep->_field.stringValue), keep->_commit);
                       if (!err.empty() && done) done(NS(err));
                   }];
}

- (void)controlTextDidChange:(NSNotification*)note {
    NSString* v = _field.stringValue;
    if ([v containsString:@" "]) _field.stringValue = [v stringByReplacingOccurrencesOfString:@" " withString:@"-"];  // as git would refuse spaces
    std::shared_ptr<RepoSession> s = _session.lock();
    const std::wstring name = WS(_field.stringValue);
    const std::wstring err = name.empty() || !s ? std::wstring() : s->CheckNewBranch(name);
    _error.stringValue = NS(err);
    _alert.buttons[0].enabled = !name.empty() && err.empty();
}

@end

// ---------------------------------------------------------------- the window

@implementation MGRepoWindowController {
    std::shared_ptr<RepoSession> _session;
    NSSplitViewController* _split;
    NSSplitViewItem* _sideItem;
    NSSplitViewItem* _detailItem;
    MGSidebarController* _sidebar;
    MGHistoryController* _history;
    MGDetailsController* _details;
    MGDiffController* _diff;
    MGCenterController* _center;
    NSView* _message;          // loading or error, in place of the history
    NSString* _messageKey;     // what _message shows
    NSSearchToolbarItem* _search;
    NSProgressIndicator* _spinner;
    int _pullBadge, _pushBadge;
    BOOL _revealed;            // the selection was scrolled into view after the first load
}

- (instancetype)initWithPath:(NSString*)path {
    NSWindow* w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1240, 780)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                                                        NSWindowStyleMaskResizable | NSWindowStyleMaskFullSizeContentView
                                                backing:NSBackingStoreBuffered
                                                  defer:YES];
    w.releasedWhenClosed = NO;
    w.restorable = NO;
    w.minSize = NSMakeSize(760, 460);
    w.tabbingIdentifier = @"Mangrove";
    w.toolbarStyle = NSWindowToolbarStyleUnified;
    w.title = @"Mangrove";
    if (!(self = [super initWithWindow:w])) return nil;
    w.delegate = self;
    _pullBadge = _pushBadge = -1;
    NSToolbar* tb = [[NSToolbar alloc] initWithIdentifier:@"Mangrove.toolbar"];
    tb.delegate = self;
    tb.displayMode = NSToolbarDisplayModeIconOnly;
    tb.allowsUserCustomization = YES;
    tb.autosavesConfiguration = YES;
    w.toolbar = tb;
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(settingsChanged:) name:MGSettingsChangedNotification object:nil];
    if (path.length) [self openRepository:path];
    else [self showStart];
    [w center];
    [w setFrameAutosaveName:@"Mangrove.window"];  // only the first window gets it; tabs share their window's frame
    return self;
}

- (RepoSession*)session {
    return _session.get();
}

- (void)setContent:(NSViewController*)vc {
    const NSRect frame = self.window.frame;
    self.window.contentViewController = vc;
    [self.window setFrame:frame display:NO];
}

- (void)showStart {
    _session.reset();
    self.window.toolbar.visible = NO;  // nothing to act on yet
    [self setContent:[[MGStartController alloc] initWithOwner:self]];
    [self updateTitle];
}

- (void)openRepository:(NSString*)path {
    _session = std::make_shared<RepoSession>();
    _session->path = NormalizePath(WS(path));
    _session->title = BaseName(_session->path);
    __weak MGRepoWindowController* weakSelf = self;
    _session->onEvent = [weakSelf](SessionEvent e) { [weakSelf sessionEvent:e]; };
    [self buildRepositoryViews];
    self.window.toolbar.visible = YES;
    self.window.representedURL = [NSURL fileURLWithPath:path isDirectory:YES];
    _session->StartLoad(false);
    [self updateContent];
    [self updateTitle];
}

- (void)buildRepositoryViews {
    _sidebar = [[MGSidebarController alloc] initWithOwner:self];
    _history = [[MGHistoryController alloc] initWithOwner:self];
    _details = [[MGDetailsController alloc] initWithOwner:self];
    _diff = [[MGDiffController alloc] initWithOwner:self];
    _center = [[MGCenterController alloc] initWithNibName:nil bundle:nil];
    _center.owner = self;
    _split = [[NSSplitViewController alloc] init];
    _sideItem = [NSSplitViewItem sidebarWithViewController:_sidebar];
    _sideItem.minimumThickness = 180;
    _sideItem.maximumThickness = 380;
    NSSplitViewItem* center = [NSSplitViewItem splitViewItemWithViewController:_center];
    center.minimumThickness = 380;
    _detailItem = nil;
#if MG_SDK_14
    if (@available(macOS 14.0, *)) _detailItem = [NSSplitViewItem inspectorWithViewController:_details];
#endif
    if (!_detailItem) {
        _detailItem = [NSSplitViewItem splitViewItemWithViewController:_details];
        _detailItem.canCollapse = YES;
        _detailItem.holdingPriority = NSLayoutPriorityDefaultLow + 10;  // the history takes the extra width
    }
    _detailItem.minimumThickness = 300;
    _detailItem.maximumThickness = 640;
    _detailItem.allowsFullHeightLayout = NO;  // below the toolbar, on every macOS version
    [_split addSplitViewItem:_sideItem];
    [_split addSplitViewItem:center];
    [_split addSplitViewItem:_detailItem];
    _split.splitView.autosaveName = @"Mangrove.split";
    [self setContent:_split];
}

// ---- what the session reports

- (void)sessionEvent:(SessionEvent)e {
    RepoSession* s = _session.get();
    if (!s) return;
    switch (e) {
    case SessionEvent::Loading:
        [self updateContent];
        break;
    case SessionEvent::Loaded:
        [self updateContent];
        if (s->data) {
            [_sidebar reload];
            [_history reload];
            [_details reload];
            if (!s->lastLoadRefresh || !_revealed) [_history revealSelection];
            _revealed = YES;
        }
        [App() saveSettings];
        break;
    case SessionEvent::Status:
        [_history statusChanged:s->wipRowDelta];
        [_sidebar refreshCounts];
        [_details reload];
        break;
    case SessionEvent::Details:
        [_details reload];
        break;
    case SessionEvent::Diff:
        [self updateContent];
        [_diff reload];
        [_details refreshState];
        break;
    case SessionEvent::Search:
        [_history redrawRows];
        break;
    case SessionEvent::LineStats:
        [_history reloadStats];
        break;
    case SessionEvent::Operation:
        [_details refreshState];
        [_diff reload];
        break;
    case SessionEvent::Notice:
        [_center.banner showTitle:NS(s->notice.title) message:NS(s->notice.message) error:s->notice.error];
        break;
    case SessionEvent::Message:
        [_details applySuggestion];
        break;
    case SessionEvent::Committed:
        [_details clearMessage];
        break;
    case SessionEvent::PushRejected: {
        std::weak_ptr<RepoSession> wk = _session;
        RunPrompt(self.window, PushRejectedPrompt(), ^(int b) {
            std::shared_ptr<RepoSession> session = wk.lock();
            if (!session) return;
            if (b == 0) session->Pull();
            else if (b == 1) session->Push(true);
        });
        break;
    }
    }
    [self updateTitle];
    [self.window.toolbar validateVisibleItems];
}

// The history, the diff, or a message while there is nothing to show.
- (void)updateContent {
    RepoSession* s = _session.get();
    if (!s || !_center) return;
    if (s->data) {
        [_center remove:_message];
        _message = nil;
        _messageKey = nil;
        [_center show:s->diff.open ? _diff.view : _history.view];
        return;
    }
    NSString* key = !s->error.empty() ? NS(s->error) : @"";
    if (_message && [key isEqualToString:_messageKey]) return;
    [_center remove:_message];
    _messageKey = key;
    if (s->error.empty()) {
        _message = MessageView([NSString stringWithFormat:@"Opening %@…", NS(s->title)], nil, YES, nil);
    } else {
        NSMutableArray<NSButton*>* buttons = [NSMutableArray array];
        if (!s->untrusted.empty()) {
            NSButton* trust = [NSButton buttonWithTitle:@"Trust This Folder" target:self action:@selector(trustFolder:)];
            trust.keyEquivalent = @"\r";
            [buttons addObject:trust];
        }
        [buttons addObject:[NSButton buttonWithTitle:@"Try Again" target:self action:@selector(refresh:)]];
        [buttons addObject:[NSButton buttonWithTitle:@"Open Another…" target:nil action:@selector(openDocument:)]];
        NSString* detail = NS(s->error);
        if (!s->untrusted.empty())
            detail = [detail stringByAppendingFormat:@"\n\nTrusting it adds %@ to Git's safe directories for Mangrove only.", NS(s->untrusted)];
        _message = MessageView([NSString stringWithFormat:@"Can't open “%@”", NS(s->title)], detail, NO, buttons);
    }
    [_center show:_message];
}

- (void)updateTitle {
    RepoSession* s = _session.get();
    NSWindow* w = self.window;
    NSString* title = s ? NS(s->data ? s->data->name : s->title) : @"Mangrove";
    NSString* sub = @"";
    if (s && s->opBusy) {
        sub = [NS(s->opLabel) stringByAppendingString:@"…"];
        if (!s->opProgress.empty()) sub = [sub stringByAppendingFormat:@"  %@", NS(s->opProgress)];
    } else if (s && s->data) {
        sub = NS(HistorySubtitle(*s->data));
    }
    if (![w.title isEqualToString:title]) w.title = title;
    if (![w.subtitle isEqualToString:sub]) w.subtitle = sub;
    if (s && (s->opBusy || s->loading)) [_spinner startAnimation:nil];
    else [_spinner stopAnimation:nil];
}

- (void)settingsChanged:(NSNotification*)note {
    [_diff reload];
    [self.window.toolbar validateVisibleItems];
}

// ---- selection

- (void)selectionChanged {
    [_sidebar syncSelection];
    [_details reload];
    [self updateContent];
    [_history redrawRows];  // the search status counts from the selection
    [self.window.toolbar validateVisibleItems];
}

- (void)selectRow:(NSInteger)row {
    RepoSession* s = _session.get();
    if (!s || !s->graph || row < 0 || row >= (NSInteger)s->graph->rows.size()) return;
    if (row == s->SelectedRow() && !s->diff.open) return;
    s->SelectRow((int32_t)row);
    [self selectionChanged];
}

- (void)selectCommit:(int32_t)commit {
    RepoSession* s = _session.get();
    if (!s || !s->data || commit < 0 || commit >= (int32_t)s->data->commits.size()) return;
    s->Select(commit, false);
    [_history syncSelection];
    [_history revealSelection];
    [self selectionChanged];
}

- (void)selectWorkingTree {
    RepoSession* s = _session.get();
    if (!s || !s->graph || !s->graph->hasWip) return;
    s->Select(-1, true);
    [_history syncSelection];
    [_history revealSelection];
    [self selectionChanged];
}

- (void)reviewChanges {
    [self selectWorkingTree];
    if (_detailItem.collapsed) _detailItem.animator.collapsed = NO;
    [_details focusSummary];
}

- (void)setHeadOnly:(BOOL)headOnly {
    RepoSession* s = _session.get();
    if (!s || !s->data || s->headOnly == (bool)headOnly) return;
    s->headOnly = headOnly;
    s->StartLoad(true);
}

// ---- actions

- (void)openDiff:(int)section path:(const std::wstring&)path {
    if (_session) _session->OpenDiff(section, path);  // the Diff event shows it
}

- (void)checkoutRef:(int)ref {
    if (_session) _session->CheckoutRef(ref);
}

- (void)checkoutCommit:(int32_t)commit {
    RepoSession* s = _session.get();
    if (!s || !s->CanCheckout(commit, nullptr)) return;
    const int target = s->CheckoutTarget(commit);
    if (target >= 0) s->CheckoutRef(target);
    else s->CheckoutCommit(commit);
}

- (void)newBranchAt:(int32_t)commit {
    RepoSession* s = _session.get();
    if (!s || !s->CanRun(ActBranch, nullptr)) return;
    __weak MGCenterController* center = _center;
    [[[MGBranchPrompt alloc] initWithSession:_session commit:commit] runInWindow:self.window
                                                                            done:^(NSString* error) {
                                                                                [center.banner showTitle:@"Couldn't create the branch" message:error error:YES];
                                                                            }];
}

- (void)revertCommit:(int32_t)commit {
    RepoSession* s = _session.get();
    if (!s || !s->data) return;
    const std::wstring why = s->CanRevert(commit);
    if (!why.empty()) {
        [_center.banner showTitle:@"Can't revert" message:NS(why) error:YES];
        return;
    }
    std::weak_ptr<RepoSession> wk = _session;
    RunPrompt(self.window, RevertPrompt(*s->data, commit), ^(int b) {
        std::shared_ptr<RepoSession> session = wk.lock();
        if (session && b == 0) session->Revert(commit);
    });
}

- (void)dropStash:(int)index {
    RepoSession* s = _session.get();
    if (!s || !s->data || s->opBusy || index < 0 || index >= (int)s->data->stashes.size()) return;
    std::weak_ptr<RepoSession> wk = _session;
    RunPrompt(self.window, DropStashPrompt(s->data->stashes[(size_t)index]), ^(int b) {
        std::shared_ptr<RepoSession> session = wk.lock();
        if (session && b == 0) session->DropStash(index);
    });
}

- (void)discardFile:(const FileChange&)file {
    RepoSession* s = _session.get();
    if (!s || file.status == L'U') return;
    std::weak_ptr<RepoSession> wk = _session;
    const FileChange f = file;
    RunPrompt(self.window, DiscardPrompt(f), ^(int b) {
        std::shared_ptr<RepoSession> session = wk.lock();
        if (session && b == 0) session->Discard(f);
    });
}

- (void)stagePaths:(const std::vector<std::wstring>&)paths stage:(BOOL)stage {
    if (_session) _session->Stage(paths, stage);
}

- (void)applyPatch:(const PatchJob&)job {
    RepoSession* s = _session.get();
    if (!s || job.patch.empty()) return;
    if (!job.discard) {
        s->ApplyPatch(job);
        return;
    }
    std::weak_ptr<RepoSession> wk = _session;
    const PatchJob j = job;
    RunPrompt(self.window, DiscardPatchPrompt(job, s->diff.req.path), ^(int b) {
        std::shared_ptr<RepoSession> session = wk.lock();
        if (session && b == 0) session->ApplyPatch(j);
    });
}

- (void)copyText:(NSString*)text {
    [NSPasteboard.generalPasteboard clearContents];
    [NSPasteboard.generalPasteboard setString:text forType:NSPasteboardTypeString];
}

- (void)revealFile:(const std::wstring&)relativePath {
    RepoSession* s = _session.get();
    if (!s || !s->data) return;
    NSString* path = NS(PathJoin(s->data->root, relativePath));
    if ([NSFileManager.defaultManager fileExistsAtPath:path])
        [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:path] ]];
    else  // deleted: the folder that held it
        [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:path.stringByDeletingLastPathComponent isDirectory:YES]];
}

- (void)trustFolder:(id)sender {
    if (_session) _session->Trust();
}

// ---- context menus

- (NSMenuItem*)add:(NSMenu*)m title:(NSString*)title action:(SEL)action tag:(NSInteger)tag enabled:(BOOL)enabled {
    NSMenuItem* i = [m addItemWithTitle:title action:action keyEquivalent:@""];
    i.target = self;
    i.tag = tag;
    i.enabled = enabled;
    return i;
}

- (void)fillCommitMenu:(NSMenu*)m commit:(int32_t)c {
    [m removeAllItems];
    m.autoenablesItems = NO;
    RepoSession* s = _session.get();
    if (!s || !s->data) return;
    const RepoData& d = *s->data;
    const bool busy = s->opBusy;
    if (c < 0) {
        [self add:m title:@"Review and Commit" action:@selector(menuReview:) tag:0 enabled:s->graph && s->graph->hasWip];
        [m addItem:NSMenuItem.separatorItem];
        [self add:m title:@"Stash All Changes" action:@selector(stash:) tag:0 enabled:s->CanRun(ActStash, nullptr)];
        [self add:m title:@"New Branch…" action:@selector(menuNewBranch:) tag:-1 enabled:s->CanRun(ActBranch, nullptr)];
        return;
    }
    if (c >= (int32_t)d.commits.size()) return;
    const Commit& cm = d.commits[(size_t)c];
    if (auto it = d.labels.find(c); it != d.labels.end()) {
        int n = 0;
        for (const Label& L : it->second) {
            if (L.tag || n >= 6) continue;
            const Ref& r = d.refs[(size_t)L.ref];
            NSString* name = NS(r.kind == RefKind::Remote && !L.local ? r.name : r.branch);
            [self add:m title:[@"Check Out " stringByAppendingString:name] action:@selector(menuCheckoutRef:) tag:L.ref enabled:!busy && !r.head];
            ++n;
        }
    }
    const bool stash = cm.flags & CF_STASH;
    [self add:m title:@"New Branch Here…" action:@selector(menuNewBranch:) tag:c enabled:!busy];
    [self add:m title:@"Check Out This Commit (Detached HEAD)" action:@selector(menuCheckoutDetached:) tag:c enabled:!busy && !stash];
    [self add:m title:@"Revert This Commit…" action:@selector(menuRevert:) tag:c enabled:!busy && !stash && !d.unborn];
    [m addItem:NSMenuItem.separatorItem];
    [self add:m title:@"Copy Commit ID" action:@selector(menuCopyId:) tag:c enabled:YES];
    [self add:m title:@"Copy Short ID" action:@selector(menuCopyShortId:) tag:c enabled:YES];
    [self add:m title:@"Copy Message" action:@selector(menuCopyMessage:) tag:c enabled:YES];
    if (cm.parentCount && d.Parents(cm)[0] >= 0) {
        [m addItem:NSMenuItem.separatorItem];
        [self add:m title:@"Go to Parent" action:@selector(menuGoTo:) tag:d.Parents(cm)[0] enabled:YES];
    }
}

- (void)fillRefMenu:(NSMenu*)m ref:(int)ri {
    [m removeAllItems];
    m.autoenablesItems = NO;
    RepoSession* s = _session.get();
    if (!s || !s->data || ri < 0 || ri >= (int)s->data->refs.size()) return;
    const RepoData& d = *s->data;
    const Ref& r = d.refs[(size_t)ri];
    const bool busy = s->opBusy;
    NSString* checkout = @"Check Out";
    if (r.kind == RefKind::Remote) {
        bool hasLocal = false;
        for (const Ref& x : d.refs) hasLocal |= x.kind == RefKind::Local && x.name == r.branch;
        checkout = hasLocal ? [@"Check Out " stringByAppendingString:NS(r.branch)]
                            : [NSString stringWithFormat:@"Check Out as New Local Branch “%@”", NS(r.branch)];
    } else if (r.kind == RefKind::Tag) {
        checkout = @"Check Out (Detached HEAD)";
    }
    [self add:m title:checkout action:@selector(menuCheckoutRef:) tag:ri enabled:!busy && !r.head];
    [self add:m title:@"New Branch Here…" action:@selector(menuNewBranch:) tag:r.commit enabled:!busy && r.commit >= 0];
    [m addItem:NSMenuItem.separatorItem];
    NSString* copyTitle = r.kind == RefKind::Tag ? @"Copy Tag Name" : @"Copy Branch Name";
    NSMenuItem* copyItem = [self add:m title:copyTitle action:@selector(menuCopyText:) tag:0 enabled:YES];
    copyItem.representedObject = NS(r.name);
}

- (void)fillStashMenu:(NSMenu*)m stash:(int)index {
    [m removeAllItems];
    m.autoenablesItems = NO;
    RepoSession* s = _session.get();
    if (!s || !s->data || index < 0 || index >= (int)s->data->stashes.size()) return;
    const bool busy = s->opBusy;
    [self add:m title:@"Apply Stash" action:@selector(menuApplyStash:) tag:index enabled:!busy];
    [self add:m title:@"Pop Stash" action:@selector(menuPopStash:) tag:index enabled:!busy];
    [m addItem:NSMenuItem.separatorItem];
    [self add:m title:@"Delete Stash…" action:@selector(menuDropStash:) tag:index enabled:!busy];
}

- (void)menuReview:(NSMenuItem*)i { [self reviewChanges]; }
- (void)menuNewBranch:(NSMenuItem*)i { [self newBranchAt:(int32_t)i.tag]; }
- (void)menuCheckoutRef:(NSMenuItem*)i { [self checkoutRef:(int)i.tag]; }
- (void)menuCheckoutDetached:(NSMenuItem*)i {
    if (_session) _session->CheckoutCommit((int32_t)i.tag);
}
- (void)menuRevert:(NSMenuItem*)i { [self revertCommit:(int32_t)i.tag]; }
- (void)menuGoTo:(NSMenuItem*)i { [self selectCommit:(int32_t)i.tag]; }
- (void)menuCopyText:(NSMenuItem*)i { [self copyText:i.representedObject]; }
- (void)menuApplyStash:(NSMenuItem*)i {
    if (_session) _session->Pop((int)i.tag, true);
}
- (void)menuPopStash:(NSMenuItem*)i {
    if (_session) _session->Pop((int)i.tag, false);
}
- (void)menuDropStash:(NSMenuItem*)i { [self dropStash:(int)i.tag]; }

- (void)copyCommit:(int32_t)c chars:(int)n {
    RepoSession* s = _session.get();
    if (s && s->data && c >= 0 && c < (int32_t)s->data->commits.size()) [self copyText:NS(s->data->commits[(size_t)c].id.Hex(n))];
}
- (void)menuCopyId:(NSMenuItem*)i { [self copyCommit:(int32_t)i.tag chars:40]; }
- (void)menuCopyShortId:(NSMenuItem*)i { [self copyCommit:(int32_t)i.tag chars:7]; }
- (void)menuCopyMessage:(NSMenuItem*)i {
    if (_session) [self copyText:NS(CommitMessage(*_session, (int32_t)i.tag))];
}

// The toolbar's "More" menu.
- (void)menuNeedsUpdate:(NSMenu*)m {
    [m removeAllItems];
    m.autoenablesItems = YES;  // validated like the menu bar
    [m addItemWithTitle:@"Undo" action:@selector(undoGit:) keyEquivalent:@""];
    [m addItemWithTitle:@"Redo" action:@selector(redoGit:) keyEquivalent:@""];
    [m addItem:NSMenuItem.separatorItem];
    [m addItemWithTitle:@"Pop Stash" action:@selector(popStash:) keyEquivalent:@""];
    [m addItemWithTitle:@"Open in Terminal" action:@selector(openTerminal:) keyEquivalent:@""];
    [m addItem:NSMenuItem.separatorItem];
    NSArray<NSString*>* modes = @[ @"Pull, Fast-Forward If Possible", @"Pull, Fast-Forward Only", @"Pull with Rebase" ];
    for (NSUInteger k = 0; k < modes.count; ++k) [m addItemWithTitle:modes[k] action:@selector(pullWithMode:) keyEquivalent:@""].tag = (NSInteger)k;
    [m addItem:NSMenuItem.separatorItem];
    [m addItemWithTitle:@"Refresh" action:@selector(refresh:) keyEquivalent:@""];
    [m addItemWithTitle:@"Show in Finder" action:@selector(revealRepository:) keyEquivalent:@""];
    [m addItemWithTitle:@"Copy Repository Path" action:@selector(copyRepositoryPath:) keyEquivalent:@""];
    for (NSMenuItem* i in m.itemArray)
        if (!i.separatorItem) i.target = self;
}

// The chosen pull mode becomes the one of the Pull button.
- (void)pullWithMode:(NSMenuItem*)i {
    AppSettings().pullMode = (int)i.tag;
    AppSettings().Save();
    if (_session) _session->Pull();
}

// ---- commands

- (IBAction)fetch:(id)sender {
    if (_session) _session->Fetch();
}
- (IBAction)pull:(id)sender {
    if (_session) _session->Pull();
}
- (IBAction)push:(id)sender {
    if (_session) _session->Push(false);
}
- (IBAction)stash:(id)sender {
    if (_session) _session->Stash();
}
- (IBAction)popStash:(id)sender {
    if (_session) _session->Pop(0, false);
}
- (IBAction)undoGit:(id)sender {
    if (_session) _session->Undo();
}
- (IBAction)redoGit:(id)sender {
    if (_session) _session->Redo();
}
- (IBAction)newBranch:(id)sender {
    if (_session) [self newBranchAt:_session->selWip ? -1 : _session->selCommit];
}
- (IBAction)refresh:(id)sender {
    if (_session && !_session->IsStart()) _session->StartLoad(_session->data != nullptr);
}

- (IBAction)openTerminal:(id)sender {
    if (!_session || !_session->data) return;
    NSURL* terminal = [NSWorkspace.sharedWorkspace URLForApplicationWithBundleIdentifier:@"com.apple.Terminal"];
    if (!terminal) return;
    [NSWorkspace.sharedWorkspace openURLs:@[ [NSURL fileURLWithPath:NS(_session->data->root) isDirectory:YES] ]
                     withApplicationAtURL:terminal
                            configuration:[NSWorkspaceOpenConfiguration configuration]
                        completionHandler:nil];
}

- (IBAction)revealRepository:(id)sender {
    if (_session && _session->data) [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:NS(_session->data->root) isDirectory:YES]];
}

- (IBAction)copyRepositoryPath:(id)sender {
    if (_session && _session->data) [self copyText:NS(_session->data->root)];
}

- (IBAction)copy:(id)sender {  // the history has the focus: the selected commit's id
    RepoSession* s = _session.get();
    if (s && s->data && !s->selWip) [self copyCommit:s->selCommit chars:40];
}

- (IBAction)focusSearch:(id)sender {
    if (!_session || !_session->data) return;
    _session->CloseDiff();
    if (_search) [_search beginSearchInteraction];
}

- (void)searchStep:(int)dir {
    RepoSession* s = _session.get();
    if (!s) return;
    const int32_t row = s->FindMatch(dir);
    if (row < 0) {
        NSBeep();
        return;
    }
    s->SelectRow(row);
    [_history syncSelection];
    [_history revealSelection];
    [self selectionChanged];
}

- (IBAction)findNextMatch:(id)sender { [self searchStep:1]; }
- (IBAction)findPreviousMatch:(id)sender { [self searchStep:-1]; }

- (void)searchChanged:(NSSearchField*)field {
    if (_session) _session->SetSearch(WS(field.stringValue));
}

// Enter goes to the next match (Shift+Enter: the previous one); the arrows move through the history.
- (BOOL)control:(NSControl*)control textView:(NSTextView*)tv doCommandBySelector:(SEL)cmd {
    if (!_search || control != _search.searchField) return NO;
    if (cmd == @selector(insertNewline:)) {
        [self searchStep:(NSApp.currentEvent.modifierFlags & NSEventModifierFlagShift) ? -1 : 1];
        return YES;
    }
    if (cmd == @selector(moveDown:) || cmd == @selector(moveUp:)) {
        RepoSession* s = _session.get();
        if (!s || !s->graph || s->graph->rows.empty()) return YES;
        const int32_t row = std::clamp(s->SelectedRow() + (cmd == @selector(moveDown:) ? 1 : -1), 0, (int32_t)s->graph->rows.size() - 1);
        s->SelectRow(row);
        [_history syncSelection];
        [_history revealSelection];
        [self selectionChanged];
        return YES;
    }
    return NO;
}

- (IBAction)toggleDetails:(id)sender {
    if (_detailItem) _detailItem.animator.collapsed = !_detailItem.collapsed;
}

- (IBAction)toggleHeadOnly:(id)sender {
    if (_session) [self setHeadOnly:!_session->headOnly];
}

- (IBAction)showHistoryPage:(id)sender {
    RepoSession* s = _session.get();
    if (!s || !s->data) return;
    s->CloseDiff();
    if (s->selWip && s->data->headCommit >= 0) [self selectCommit:s->data->headCommit];
    [_history focusTable];
}

- (IBAction)showChangesPage:(id)sender {
    [self reviewChanges];
}

- (IBAction)setDiffLayout:(NSMenuItem*)sender {
    AppSettings().diffSplit = sender.tag == 1;
    AppSettings().Save();
    [NSNotificationCenter.defaultCenter postNotificationName:MGSettingsChangedNotification object:nil];
}

- (IBAction)closeDiff:(id)sender {
    if (_session) _session->CloseDiff();
}

// Esc: drops the selected lines, then closes the diff.
- (void)cancelOperation:(id)sender {
    RepoSession* s = _session.get();
    if (!s || !s->diff.open) return;
    if (s->diff.selCount) s->ClearDiffSelection();
    else s->CloseDiff();
}

// ---- validation

- (BOOL)validateAction:(SEL)a tip:(NSString* __autoreleasing*)tip {
    RepoSession* s = _session.get();
    int act = -1;
    if (a == @selector(fetch:)) act = ActFetch;
    else if (a == @selector(pull:)) act = ActPull;
    else if (a == @selector(push:)) act = ActPush;
    else if (a == @selector(newBranch:)) act = ActBranch;
    else if (a == @selector(stash:)) act = ActStash;
    else if (a == @selector(popStash:)) act = ActPop;
    else if (a == @selector(undoGit:)) act = ActUndo;
    else if (a == @selector(redoGit:)) act = ActRedo;
    else if (a == @selector(openTerminal:)) act = ActTerminal;
    if (act >= 0) {
        std::wstring why;
        const bool ok = s && s->CanRun(act, &why);
        if (tip) *tip = s ? NS(why) : nil;
        return ok;
    }
    const bool repo = s && s->data;
    if (a == @selector(refresh:)) return s && !s->IsStart();
    if (a == @selector(revealRepository:) || a == @selector(copyRepositoryPath:) || a == @selector(toggleHeadOnly:) ||
        a == @selector(focusSearch:) || a == @selector(showHistoryPage:))
        return repo;
    if (a == @selector(pullWithMode:)) return s && s->CanRun(ActPull, nullptr);
    if (a == @selector(findNextMatch:) || a == @selector(findPreviousMatch:)) return repo && s->Searching() && s->searchCount > 0;
    if (a == @selector(showChangesPage:)) return repo && s->graph && s->graph->hasWip;
    if (a == @selector(copy:)) return repo && !s->selWip && s->selCommit >= 0;
    if (a == @selector(toggleDetails:)) return _detailItem != nil;
    if (a == @selector(closeDiff:)) return repo && s->diff.open;
    return YES;
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    SEL a = item.action;
    RepoSession* s = _session.get();
    if (a == @selector(undoGit:) || a == @selector(redoGit:)) {
        const bool undo = a == @selector(undoGit:);
        const std::vector<UndoEntry>* stack = s ? (undo ? &s->undo : &s->redo) : nullptr;
        item.title = stack && !stack->empty() ? [NSString stringWithFormat:@"%@ %@", undo ? @"Undo" : @"Redo", NS(stack->back().label)]
                                              : (undo ? @"Undo" : @"Redo");
    } else if (a == @selector(popStash:)) {
        item.title = s && s->data && !s->data->stashes.empty() ? [@"Pop " stringByAppendingString:NS(s->data->stashes[0].name)] : @"Pop Stash";
    } else if (a == @selector(toggleDetails:)) {
        item.title = _detailItem && !_detailItem.collapsed ? @"Hide Details" : @"Show Details";
    } else if (a == @selector(toggleHeadOnly:)) {
        item.state = s && s->headOnly ? NSControlStateValueOn : NSControlStateValueOff;
    } else if (a == @selector(setDiffLayout:)) {
        item.state = (item.tag == 1) == AppSettings().diffSplit ? NSControlStateValueOn : NSControlStateValueOff;
    } else if (a == @selector(pullWithMode:)) {
        item.state = item.tag == AppSettings().pullMode ? NSControlStateValueOn : NSControlStateValueOff;
    }
    return [self validateAction:a tip:nil];
}

- (BOOL)validateToolbarItem:(NSToolbarItem*)item {
    NSString* tip = nil;
    const BOOL ok = [self validateAction:item.action tip:&tip];
    if (tip.length && ![item.toolTip isEqualToString:tip]) item.toolTip = tip;
    [self updateBadge:item];
    return ok;
}

// Pull and Push show how many commits they would bring in or send (macOS 26).
- (void)updateBadge:(NSToolbarItem*)item {
#if MG_SDK_26
    if (@available(macOS 26.0, *)) {
        const bool pull = [item.itemIdentifier isEqualToString:kPull], push = [item.itemIdentifier isEqualToString:kPush];
        if (!pull && !push) return;
        int n = 0;
        if (RepoSession* s = _session.get(); s && s->data)
            if (const Ref* h = HeadRef(*s->data)) n = pull ? h->behind : h->ahead;
        int& shown = pull ? _pullBadge : _pushBadge;
        if (shown == n) return;
        shown = n;
        item.badge = n > 0 ? [NSItemBadge badgeWithCount:n] : nil;
    }
#else
    (void)item;
#endif
}

// ---- toolbar

- (NSArray<NSToolbarItemIdentifier>*)toolbarDefaultItemIdentifiers:(NSToolbar*)toolbar {
    return @[
        NSToolbarToggleSidebarItemIdentifier, NSToolbarSidebarTrackingSeparatorItemIdentifier, kFetch, kPull, kPush, NSToolbarSpaceItemIdentifier,
        kBranch, kStash, kPop, NSToolbarFlexibleSpaceItemIdentifier, kSearch, kMore, kDetails
    ];
}

- (NSArray<NSToolbarItemIdentifier>*)toolbarAllowedItemIdentifiers:(NSToolbar*)toolbar {
    return @[
        NSToolbarToggleSidebarItemIdentifier, NSToolbarSidebarTrackingSeparatorItemIdentifier, kFetch, kPull, kPush, kBranch, kStash, kPop,
        kActivity, kSearch, kMore, kDetails, NSToolbarSpaceItemIdentifier, NSToolbarFlexibleSpaceItemIdentifier
    ];
}

- (NSToolbarItem*)toolbar:(NSToolbar*)toolbar itemForItemIdentifier:(NSToolbarItemIdentifier)ident willBeInsertedIntoToolbar:(BOOL)inserted {
    if ([ident isEqualToString:kSearch]) {
        NSSearchToolbarItem* item = [[NSSearchToolbarItem alloc] initWithItemIdentifier:ident];
        item.label = @"Search";
        item.searchField.placeholderString = @"Search Commits";
        item.searchField.sendsSearchStringImmediately = YES;
        item.searchField.delegate = self;
        item.searchField.target = self;
        item.searchField.action = @selector(searchChanged:);
        item.preferredWidthForSearchField = 220;
        if (inserted) _search = item;
        return item;
    }
    if ([ident isEqualToString:kActivity]) {
        NSToolbarItem* item = [[NSToolbarItem alloc] initWithItemIdentifier:ident];
        NSProgressIndicator* spinner = [[NSProgressIndicator alloc] init];
        spinner.style = NSProgressIndicatorStyleSpinning;
        spinner.controlSize = NSControlSizeSmall;
        spinner.displayedWhenStopped = NO;
        item.view = spinner;
        item.label = @"Activity";
        if (inserted) {
            _spinner = spinner;
            [self updateTitle];
        }
        return item;
    }
    if ([ident isEqualToString:kMore]) {
        NSMenuToolbarItem* item = [[NSMenuToolbarItem alloc] initWithItemIdentifier:ident];
        item.image = Symbol(@"ellipsis.circle", 15, NSFontWeightRegular);
        item.label = @"More";
        item.toolTip = @"More actions";
        item.showsIndicator = NO;
        NSMenu* menu = [[NSMenu alloc] initWithTitle:@"More"];
        menu.delegate = self;
        item.menu = menu;
        return item;
    }
    NSToolbarItem* item = [[NSToolbarItem alloc] initWithItemIdentifier:ident];
    item.target = self;
    item.bordered = YES;
    auto set = [&](NSString* label, NSString* symbol, SEL action) {
        item.label = label;
        item.paletteLabel = label;
        item.toolTip = label;
        item.image = Symbol(symbol, 15, NSFontWeightRegular);
        item.action = action;
    };
    if ([ident isEqualToString:kFetch]) set(@"Fetch", @"arrow.triangle.2.circlepath", @selector(fetch:));
    else if ([ident isEqualToString:kPull]) set(@"Pull", @"arrow.down.circle", @selector(pull:));
    else if ([ident isEqualToString:kPush]) set(@"Push", @"arrow.up.circle", @selector(push:));
    else if ([ident isEqualToString:kBranch]) set(@"New Branch", @"arrow.triangle.branch", @selector(newBranch:));
    else if ([ident isEqualToString:kStash]) set(@"Stash", @"tray.and.arrow.down", @selector(stash:));
    else if ([ident isEqualToString:kPop]) set(@"Pop Stash", @"tray.and.arrow.up", @selector(popStash:));
    else if ([ident isEqualToString:kDetails]) set(@"Details", @"sidebar.right", @selector(toggleDetails:));
    else return nil;
    return item;
}

// ---- window

- (void)windowWillClose:(NSNotification*)note {
    if (_session) _session->onEvent = nullptr;
    [App() windowClosed:self];
}

- (void)windowDidBecomeKey:(NSNotification*)note {
    [App() saveSettings];  // the active tab
}

@end
