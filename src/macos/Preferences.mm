// Settings: the preferences by category in the standard settings window, a toolbar of panes. General,
// Appearance, Git, and Integrations: the accounts on GitHub, GitLab and Gitea / Forgejo, connected by signing in
// with the browser (github.com) or with a token, and kept in the keychain.

#import "MacUI.h"

void ApplyAppearance() {
    const std::wstring theme = AppSettings().Get(L"theme");
    NSApp.appearance = theme == L"light"  ? [NSAppearance appearanceNamed:NSAppearanceNameAqua]
                       : theme == L"dark" ? [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]
                                          : nil;  // the system's
}

static void SettingsChanged() {
    AppSettings().Save();
    [NSNotificationCenter.defaultCenter postNotificationName:MGSettingsChangedNotification object:nil];
}

static NSTextField* RowLabel(NSString* text) {
    return MakeLabel(text, nil, nil);
}

static NSTextField* HelpLabel(NSString* text, CGFloat width) {
    NSTextField* l = [NSTextField wrappingLabelWithString:text];
    l.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    l.textColor = NSColor.secondaryLabelColor;
    l.preferredMaxLayoutWidth = width;
    return l;
}

static NSGridView* Grid(NSArray<NSArray<NSView*>*>* rows) {
    NSGridView* grid = [NSGridView gridViewWithViews:rows];
    grid.rowSpacing = 10;
    grid.columnSpacing = 8;
    grid.rowAlignment = NSGridRowAlignmentFirstBaseline;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
    return grid;
}

// A pane: its content with the margins of a settings window, as big as it needs.
@interface MGPane : NSViewController
- (void)setContent:(NSView*)content;
@end

@implementation MGPane

- (void)setContent:(NSView*)content {
    NSView* v = [[NSView alloc] init];
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [v addSubview:content];
    [NSLayoutConstraint activateConstraints:@[
        [content.topAnchor constraintEqualToAnchor:v.topAnchor constant:22],
        [content.bottomAnchor constraintEqualToAnchor:v.bottomAnchor constant:-22],
        [content.leadingAnchor constraintEqualToAnchor:v.leadingAnchor constant:30],
        [content.trailingAnchor constraintEqualToAnchor:v.trailingAnchor constant:-30],
        [v.widthAnchor constraintGreaterThanOrEqualToConstant:560],
    ]];
    self.view = v;
}

- (void)viewWillAppear {
    [super viewWillAppear];
    self.preferredContentSize = self.view.fittingSize;
}

@end

// ---------------------------------------------------------------- General

@interface MGGeneralPane : MGPane
@end

@implementation MGGeneralPane {
    NSButton* _reopen;
    NSPopUpButton* _pull;
    NSPopUpButton* _diff;
}

- (void)loadView {
    self.title = @"General";
    _reopen = [NSButton checkboxWithTitle:@"Reopen the repositories that were open" target:self action:@selector(changed:)];
    _pull = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (const std::wstring& label : PullModeLabels()) [_pull addItemWithTitle:NS(label)];
    _diff = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_diff addItemsWithTitles:@[ @"Unified", @"Side by side" ]];
    for (NSPopUpButton* p in @[ _pull, _diff ]) {
        p.target = self;
        p.action = @selector(changed:);
    }
    [self setContent:Grid(@[
        @[ RowLabel(@"At launch:"), _reopen ],
        @[ RowLabel(@"Pull:"), _pull ],
        @[ NSGridCell.emptyContentView, HelpLabel(NS(PullModeHelp()), 380) ],
        @[ RowLabel(@"Diffs:"), _diff ],
    ])];
}

- (void)viewWillAppear {
    const Settings& s = AppSettings();
    _reopen.state = s.restoreTabs ? NSControlStateValueOn : NSControlStateValueOff;
    [_pull selectItemAtIndex:std::clamp(s.pullMode, 0, 2)];
    [_diff selectItemAtIndex:s.diffSplit ? 1 : 0];
    [super viewWillAppear];
}

- (void)changed:(id)sender {
    Settings& s = AppSettings();
    s.restoreTabs = _reopen.state == NSControlStateValueOn;
    s.pullMode = (int)_pull.indexOfSelectedItem;
    s.diffSplit = _diff.indexOfSelectedItem == 1;
    SettingsChanged();
}

@end

// ---------------------------------------------------------------- Appearance

@interface MGAppearancePane : MGPane
@end

@implementation MGAppearancePane {
    NSPopUpButton* _theme;
}

- (void)loadView {
    self.title = @"Appearance";
    _theme = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_theme addItemsWithTitles:@[ @"System", @"Light", @"Dark" ]];
    _theme.target = self;
    _theme.action = @selector(changed:);
    [self setContent:Grid(@[
        @[ RowLabel(@"Appearance:"), _theme ],
        @[ NSGridCell.emptyContentView, HelpLabel(@"System follows the light or dark appearance chosen in System Settings.", 380) ],
    ])];
}

- (void)viewWillAppear {
    const std::wstring theme = AppSettings().Get(L"theme");
    [_theme selectItemAtIndex:theme == L"light" ? 1 : theme == L"dark" ? 2 : 0];
    [super viewWillAppear];
}

- (void)changed:(id)sender {
    const NSInteger i = _theme.indexOfSelectedItem;
    AppSettings().Set(L"theme", i == 1 ? L"light" : i == 2 ? L"dark" : L"system");
    ApplyAppearance();
    SettingsChanged();
}

@end

// ---------------------------------------------------------------- Git

@interface MGGitPane : MGPane <NSTableViewDataSource, NSTableViewDelegate>
@end

@implementation MGGitPane {
    NSTextField* _git;
    NSTableView* _trusted;
    NSButton* _remove;
}

- (void)loadView {
    self.title = @"Git";
    _git = MakeLabel(@"Checking…", nil, NSColor.secondaryLabelColor);
    _git.selectable = YES;
    _trusted = [[NSTableView alloc] init];
    NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:@"folder"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    [_trusted addTableColumn:col];
    _trusted.headerView = nil;
    _trusted.style = NSTableViewStyleFullWidth;
    _trusted.usesAlternatingRowBackgroundColors = YES;
    _trusted.dataSource = self;
    _trusted.delegate = self;
    NSScrollView* scroll = [[NSScrollView alloc] init];
    scroll.documentView = _trusted;
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSBezelBorder;
    [scroll.widthAnchor constraintEqualToConstant:380].active = YES;
    [scroll.heightAnchor constraintEqualToConstant:110].active = YES;
    _remove = [NSButton buttonWithTitle:@"Remove" target:self action:@selector(removeTrusted:)];
    _remove.controlSize = NSControlSizeSmall;
    NSStackView* list = [NSStackView stackViewWithViews:@[ scroll, _remove, HelpLabel(NS(TrustedHelp()), 380) ]];
    list.orientation = NSUserInterfaceLayoutOrientationVertical;
    list.alignment = NSLayoutAttributeLeading;
    list.spacing = 6;
    NSGridView* grid = Grid(@[ @[ RowLabel(@"Git:"), _git ], @[ RowLabel(@"Trusted folders:"), list ] ]);
    [grid rowAtIndex:1].yPlacement = NSGridCellPlacementTop;
    [grid cellForView:list].rowAlignment = NSGridRowAlignmentNone;
    [self setContent:grid];
}

- (void)viewWillAppear {
    [_trusted reloadData];
    [self updateRemove];
    __weak NSTextField* git = _git;
    std::thread([git] {
        const std::wstring version = GitVersion(), path = GitPath(false);
        RunOnMain([git, version, path] {
            git.stringValue = version.empty() ? @"Not found: install the Xcode Command Line Tools (xcode-select --install) or Git from Homebrew"
                                              : [NSString stringWithFormat:@"%@  (%@)", NS(version), NS(path)];
        });
    }).detach();
    [super viewWillAppear];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tv {
    return (NSInteger)AppSettings().trusted.size();
}

- (NSView*)tableView:(NSTableView*)tv viewForTableColumn:(NSTableColumn*)col row:(NSInteger)row {
    NSTableCellView* cell = [tv makeViewWithIdentifier:@"folder" owner:self];
    if (!cell) {
        cell = [[NSTableCellView alloc] init];
        cell.identifier = @"folder";
        NSTextField* t = MakeLabel(@"", nil, nil);
        t.lineBreakMode = NSLineBreakByTruncatingMiddle;
        t.translatesAutoresizingMaskIntoConstraints = NO;
        [cell addSubview:t];
        cell.textField = t;
        [NSLayoutConstraint activateConstraints:@[
            [t.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:4],
            [t.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-4],
            [t.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
        ]];
    }
    const std::vector<std::wstring>& trusted = AppSettings().trusted;
    cell.textField.stringValue = row < (NSInteger)trusted.size() ? NS(trusted[(size_t)row]) : @"";
    cell.textField.toolTip = cell.textField.stringValue;
    return cell;
}

- (void)tableViewSelectionDidChange:(NSNotification*)note {
    [self updateRemove];
}

- (void)updateRemove {
    _remove.enabled = _trusted.selectedRow >= 0;
}

- (void)removeTrusted:(id)sender {
    std::vector<std::wstring>& trusted = AppSettings().trusted;
    const NSInteger row = _trusted.selectedRow;
    if (row < 0 || row >= (NSInteger)trusted.size()) return;
    trusted.erase(trusted.begin() + row);
    SettingsChanged();
    [_trusted reloadData];
    [self updateRemove];
}

@end

// ---------------------------------------------------------------- Integrations

// The initials of an account on a color of its own.
@interface MGInitialsView : NSView
@property(nonatomic) Account account;
@end

@implementation MGInitialsView

- (void)drawRect:(NSRect)dirty {
    Author a;  // the same initials and color as in the Windows app
    a.initials = Initials(_account.name.empty() ? _account.login : _account.name, L"");
    a.color = AvatarColor(_account.Key());
    DrawAvatar(NSInsetRect(self.bounds, 1, 1), a);
}

@end

@interface MGAccountCell : NSTableCellView
@property(nonatomic, strong) MGInitialsView* avatar;
@property(nonatomic, strong) NSTextField* title;
@property(nonatomic, strong) NSTextField* detail;
@end

@implementation MGAccountCell

- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    _avatar = [[MGInitialsView alloc] init];
    _title = MakeLabel(@"", [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold], NSColor.labelColor);
    _detail = MakeLabel(@"", [NSFont systemFontOfSize:11], NSColor.secondaryLabelColor);
    for (NSView* v in @[ _avatar, _title, _detail ]) {
        v.translatesAutoresizingMaskIntoConstraints = NO;
        [self addSubview:v];
    }
    [NSLayoutConstraint activateConstraints:@[
        [_avatar.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:8],
        [_avatar.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [_avatar.widthAnchor constraintEqualToConstant:30],
        [_avatar.heightAnchor constraintEqualToConstant:30],
        [_title.leadingAnchor constraintEqualToAnchor:_avatar.trailingAnchor constant:10],
        [_title.trailingAnchor constraintLessThanOrEqualToAnchor:self.trailingAnchor constant:-8],
        [_title.bottomAnchor constraintEqualToAnchor:self.centerYAnchor constant:1],
        [_detail.leadingAnchor constraintEqualToAnchor:_title.leadingAnchor],
        [_detail.trailingAnchor constraintLessThanOrEqualToAnchor:self.trailingAnchor constant:-8],
        [_detail.topAnchor constraintEqualToAnchor:self.centerYAnchor constant:2],
    ]];
    return self;
}

@end

// The sheet that connects an account: the service, its server, then signing in with the browser (a code to
// enter on GitHub, where Mangrove has an OAuth app) or a token; either way the token is checked with the server.
@interface MGAddAccountController : NSWindowController <NSTextFieldDelegate>
- (instancetype)initWithDone:(void (^)(NSString* note))done;
- (void)showSampleCode;  // the code view without asking GitHub (interface test)
@end

@implementation MGAddAccountController {
    void (^_done)(NSString*);
    NSPopUpButton* _service;
    NSTextField* _server;
    NSTextField* _help;
    NSSecureTextField* _token;
    NSProgressIndicator* _spinner;
    NSTextField* _status;
    NSButton* _connect;
    NSButton* _cancel;
    BOOL _busy;
    // signing in with the browser
    NSGridView* _grid;
    NSButton* _browser;
    NSStackView* _codeView;
    NSTextField* _codeIntro;
    NSTextField* _code;
    NSButton* _open;
    NSProgressIndicator* _codeSpinner;
    NSTextField* _codeStatus;
    NSString* _verifyUrl;
    std::shared_ptr<std::atomic<bool>> _signInCancel;  // set to stop the sign-in's worker thread
    BOOL _signingIn;
}

- (void)dealloc {
    if (_signInCancel) *_signInCancel = true;  // the sheet went away: no one waits for the authorization any more
}

- (instancetype)initWithDone:(void (^)(NSString*))done {
    NSWindow* w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 520, 300) styleMask:NSWindowStyleMaskTitled
                                                backing:NSBackingStoreBuffered
                                                  defer:YES];
    w.releasedWhenClosed = NO;
    if (!(self = [super initWithWindow:w])) return nil;
    _done = done;
    NSTextField* title = MakeLabel(@"Add an Account", [NSFont systemFontOfSize:15 weight:NSFontWeightSemibold], NSColor.labelColor);
    _service = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (const ForgeInfo& f : Forges()) [_service addItemWithTitle:NS(f.name)];
    _service.target = self;
    _service.action = @selector(serviceChanged:);
    _server = [NSTextField textFieldWithString:@""];
    _server.placeholderString = @"https://";
    _server.delegate = self;
    [_server.widthAnchor constraintEqualToConstant:320].active = YES;
    _browser = [NSButton buttonWithTitle:@"Sign In with Browser…" target:self action:@selector(signIn:)];
    NSButton* create = [NSButton buttonWithTitle:@"Create a Token…" target:self action:@selector(createToken:)];
    _help = HelpLabel(@"", 320);
    _token = [[NSSecureTextField alloc] init];
    _token.placeholderString = @"Paste the token here";
    [_token.widthAnchor constraintEqualToConstant:320].active = YES;
    w.initialFirstResponder = _token;  // the server is already filled in for the common case
    _spinner = [[NSProgressIndicator alloc] init];
    _spinner.style = NSProgressIndicatorStyleSpinning;
    _spinner.controlSize = NSControlSizeSmall;
    _spinner.displayedWhenStopped = NO;
    _status = HelpLabel(@"", 300);
    NSStackView* status = [NSStackView stackViewWithViews:@[ _spinner, _status ]];
    status.spacing = 6;
    _grid = Grid(@[
        @[ RowLabel(@"Service:"), _service ],
        @[ RowLabel(@"Server:"), _server ],
        @[ NSGridCell.emptyContentView, HelpLabel(@"Change it for GitHub Enterprise, a self-managed GitLab or another Gitea / Forgejo server.", 320) ],
        @[ RowLabel(@"Sign in:"), _browser ],
        @[ NSGridCell.emptyContentView, HelpLabel(@"Mangrove shows a code to enter on GitHub: once you authorize Mangrove there, the "
                                                  @"account is connected, with no token to copy.", 320) ],
        @[ RowLabel(@"Token:"), create ],
        @[ NSGridCell.emptyContentView, _help ],
        @[ NSGridCell.emptyContentView, _token ],
        @[ NSGridCell.emptyContentView, status ],
    ]);

    // Signing in: the code to enter on GitHub, in place of the token.
    _codeIntro = MakeLabel(@"Enter this code on GitHub, then authorize Mangrove:", nil, nil);
    _code = MakeLabel(@"", [NSFont monospacedSystemFontOfSize:26 weight:NSFontWeightSemibold], NSColor.labelColor);
    _code.selectable = YES;
    _open = [NSButton buttonWithTitle:@"Copy Code and Open GitHub" target:self action:@selector(openSignInPage:)];
    _codeSpinner = [[NSProgressIndicator alloc] init];
    _codeSpinner.style = NSProgressIndicatorStyleSpinning;
    _codeSpinner.controlSize = NSControlSizeSmall;
    _codeSpinner.displayedWhenStopped = NO;
    _codeStatus = HelpLabel(@"", 360);
    NSStackView* codeStatus = [NSStackView stackViewWithViews:@[ _codeSpinner, _codeStatus ]];
    codeStatus.spacing = 6;
    _codeView = [NSStackView stackViewWithViews:@[ _codeIntro, _code, _open, codeStatus ]];
    _codeView.orientation = NSUserInterfaceLayoutOrientationVertical;
    _codeView.alignment = NSLayoutAttributeLeading;
    _codeView.spacing = 12;
    _codeView.hidden = YES;
    _cancel = [NSButton buttonWithTitle:@"Cancel" target:self action:@selector(cancel:)];
    _cancel.keyEquivalent = @"\033";
    _connect = [NSButton buttonWithTitle:@"Connect" target:self action:@selector(connect:)];
    _connect.keyEquivalent = @"\r";
    NSStackView* buttons = [NSStackView stackViewWithViews:@[ _cancel, _connect ]];
    buttons.spacing = 10;
    NSStackView* all = [NSStackView stackViewWithViews:@[ title, _grid, _codeView, buttons ]];
    all.orientation = NSUserInterfaceLayoutOrientationVertical;
    all.alignment = NSLayoutAttributeLeading;
    all.spacing = 16;
    all.translatesAutoresizingMaskIntoConstraints = NO;
    NSView* v = w.contentView;
    [v addSubview:all];
    [NSLayoutConstraint activateConstraints:@[
        [all.topAnchor constraintEqualToAnchor:v.topAnchor constant:20],
        [all.leadingAnchor constraintEqualToAnchor:v.leadingAnchor constant:24],
        [all.trailingAnchor constraintEqualToAnchor:v.trailingAnchor constant:-24],
        [all.bottomAnchor constraintEqualToAnchor:v.bottomAnchor constant:-20],
        [buttons.trailingAnchor constraintEqualToAnchor:all.trailingAnchor],
    ]];
    [self serviceChanged:nil];
    return self;
}

- (Forge)forge {
    const NSInteger i = std::clamp<NSInteger>(_service.indexOfSelectedItem, 0, (NSInteger)Forges().size() - 1);
    return Forges()[(size_t)i].forge;
}

- (void)serviceChanged:(id)sender {
    const ForgeInfo& f = InfoOf([self forge]);
    _server.stringValue = NS(f.server);
    _help.stringValue = NS(f.tokenHelp);
    _status.stringValue = @"";
    [self updateBrowser];
}

// Signing in with the browser is offered where Mangrove has an OAuth app (github.com).
- (void)updateBrowser {
    const BOOL available = !_signingIn && BrowserSignInAvailable([self forge], WS(_server.stringValue));
    [_grid rowAtIndex:3].hidden = !available;
    [_grid rowAtIndex:4].hidden = !available;
}

- (void)controlTextDidChange:(NSNotification*)note {
    if (note.object == _server) [self updateBrowser];
}

- (void)createToken:(id)sender {
    std::wstring why;
    const std::wstring server = NormalizeServer(WS(_server.stringValue), &why);
    if (server.empty()) {
        [self showStatus:NS(why) error:YES];
        return;
    }
    NSURL* url = [NSURL URLWithString:NS(TokenPageUrl([self forge], server))];
    if (url) [NSWorkspace.sharedWorkspace openURL:url];
}

- (void)showStatus:(NSString*)text error:(BOOL)error {
    _status.stringValue = text;
    _status.textColor = error ? NSColor.systemRedColor : NSColor.secondaryLabelColor;
}

- (void)setBusy:(BOOL)busy {
    _busy = busy;
    for (NSControl* c in @[ _service, _server, _token, _connect, _browser ]) c.enabled = !busy;
    if (busy) [_spinner startAnimation:nil];
    else [_spinner stopAnimation:nil];
}

- (void)connect:(id)sender {
    if (_busy) return;
    const Forge f = [self forge];
    const std::wstring server = WS(_server.stringValue);
    const std::string token = Narrow(WS(_token.stringValue));
    [self setBusy:YES];
    [self showStatus:@"Checking the token…" error:NO];
    __weak MGAddAccountController* weakSelf = self;
    std::thread([weakSelf, f, server, token] {
        const TokenCheck check = CheckToken(f, server, token);
        RunOnMain([weakSelf, check, token] {
            MGAddAccountController* me = weakSelf;
            if (me) [me finish:check token:token];
        });
    }).detach();
}

- (void)finish:(const TokenCheck&)check token:(const std::string&)token {
    [self setBusy:NO];
    std::wstring error = check.error;
    if (check.ok && AddAccount(check.account, token, &error)) {
        _token.stringValue = @"";
        [self.window.sheetParent endSheet:self.window returnCode:NSModalResponseOK];
        if (_done) _done(NS(check.note));
        return;
    }
    [self showStatus:NS(error) error:YES];
    [self.window makeFirstResponder:_token];
}

// A worker thread asks GitHub for a code, then waits for the user to authorize Mangrove on GitHub
// (src/core/accounts.cpp). Cancel goes back to the form.
- (void)signIn:(id)sender {
    if (_busy || _signingIn) return;
    const Forge f = [self forge];
    const std::wstring server = WS(_server.stringValue);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    _signInCancel = cancel;
    [self showCode:nil url:nil];
    __weak MGAddAccountController* weakSelf = self;
    std::thread([weakSelf, f, server, cancel] {
        std::string token;
        const TokenCheck check = BrowserSignIn(
            f, server,
            [weakSelf, cancel](const DeviceCode& code) {
                const std::wstring userCode = code.userCode, url = code.verifyUrl;
                RunOnMain([weakSelf, cancel, userCode, url] {
                    MGAddAccountController* me = weakSelf;
                    if (me && !*cancel) [me showCode:NS(userCode) url:NS(url)];
                });
            },
            *cancel, &token);
        RunOnMain([weakSelf, cancel, check, token] {
            MGAddAccountController* me = weakSelf;
            if (me && !*cancel) [me signedIn:check token:token];
        });
    }).detach();
}

// The code to enter on GitHub (nil while it is being asked for), in place of the token.
- (void)showCode:(NSString*)code url:(NSString*)url {
    _signingIn = YES;
    _verifyUrl = url;
    for (NSInteger i = 3; i < _grid.numberOfRows; ++i) [_grid rowAtIndex:i].hidden = YES;
    _service.enabled = _server.enabled = NO;
    _codeView.hidden = NO;
    _codeIntro.hidden = _code.hidden = _open.hidden = code == nil;
    _code.stringValue = code ?: @"";
    _codeStatus.stringValue = code ? @"Waiting for you to authorize Mangrove on GitHub…" : @"Asking GitHub for a code…";
    [_codeSpinner startAnimation:nil];
    _connect.hidden = YES;
    _connect.keyEquivalent = @"";
    _open.keyEquivalent = @"\r";
    [self fitWindow];
}

// The sheet takes the size of what it shows: the form, or the code.
- (void)fitWindow {
    NSView* v = self.window.contentView;
    [v layoutSubtreeIfNeeded];
    [self.window setContentSize:v.fittingSize];
}

- (void)endSignIn {
    if (_signInCancel) *_signInCancel = true;  // the worker stops at its next look, and its results are dropped
    _signInCancel.reset();
    _signingIn = NO;
    [_codeSpinner stopAnimation:nil];
    _codeView.hidden = YES;
    _open.keyEquivalent = @"";
    _connect.keyEquivalent = @"\r";
    _connect.hidden = NO;
    _service.enabled = _server.enabled = YES;
    for (NSInteger i = 3; i < _grid.numberOfRows; ++i) [_grid rowAtIndex:i].hidden = NO;
    [self updateBrowser];
    [self fitWindow];
}

// The code goes to the clipboard, ready to paste on the page; the page is always the server's (BrowserSignIn
// checks it).
- (void)openSignInPage:(id)sender {
    if (!_verifyUrl.length || !_code.stringValue.length) return;
    [NSPasteboard.generalPasteboard clearContents];
    [NSPasteboard.generalPasteboard setString:_code.stringValue forType:NSPasteboardTypeString];
    NSURL* url = [NSURL URLWithString:_verifyUrl];
    if (url) [NSWorkspace.sharedWorkspace openURL:url];
}

- (void)signedIn:(const TokenCheck&)check token:(const std::string&)token {
    [self endSignIn];
    [self finish:check token:token];
}

- (void)showSampleCode {
    [self showCode:@"WDJB-MJHT" url:@"https://github.com/login/device"];
}

- (void)cancel:(id)sender {
    if (_signingIn) {  // back to the form
        [self endSignIn];
        return;
    }
    _token.stringValue = @"";
    [self.window.sheetParent endSheet:self.window returnCode:NSModalResponseCancel];
}

@end

@interface MGIntegrationsPane : MGPane <NSTableViewDataSource, NSTableViewDelegate>
- (void)addAccount:(id)sender;
@end

@implementation MGIntegrationsPane {
    NSTableView* _table;
    NSButton* _remove;
    NSTextField* _note;
    std::vector<Account> _accounts;
    MGAddAccountController* _sheet;
}

- (void)loadView {
    self.title = @"Integrations";
    _table = [[NSTableView alloc] init];
    NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:@"account"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    [_table addTableColumn:col];
    _table.headerView = nil;
    _table.style = NSTableViewStyleFullWidth;
    _table.rowHeight = 46;
    _table.dataSource = self;
    _table.delegate = self;
    NSScrollView* scroll = [[NSScrollView alloc] init];
    scroll.documentView = _table;
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSBezelBorder;
    [scroll.widthAnchor constraintEqualToConstant:500].active = YES;
    [scroll.heightAnchor constraintEqualToConstant:190].active = YES;
    NSButton* add = [NSButton buttonWithImage:Symbol(@"plus", 12, NSFontWeightMedium) target:self action:@selector(addAccount:)];
    _remove = [NSButton buttonWithImage:Symbol(@"minus", 12, NSFontWeightMedium) target:self action:@selector(removeAccount:)];
    for (NSButton* b in @[ add, _remove ]) {
        b.bezelStyle = NSBezelStyleSmallSquare;
        [b.widthAnchor constraintEqualToConstant:26].active = YES;
    }
    add.toolTip = @"Add an account";
    _remove.toolTip = @"Remove the selected account";
    NSStackView* buttons = [NSStackView stackViewWithViews:@[ add, _remove ]];
    buttons.spacing = 0;
    _note = HelpLabel(@"", 500);
    NSStackView* all = [NSStackView stackViewWithViews:@[ HelpLabel(NS(AccountsIntro()), 500), scroll, buttons, _note ]];
    all.orientation = NSUserInterfaceLayoutOrientationVertical;
    all.alignment = NSLayoutAttributeLeading;
    all.spacing = 8;
    [all setCustomSpacing:14 afterView:all.arrangedSubviews[0]];
    [self setContent:all];
}

- (void)viewWillAppear {
    [self reload];
    [super viewWillAppear];
}

- (void)reload {
    _accounts = LoadAccounts();
    [_table reloadData];
    _remove.enabled = _table.selectedRow >= 0;
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tv {
    return (NSInteger)_accounts.size();
}

- (NSView*)tableView:(NSTableView*)tv viewForTableColumn:(NSTableColumn*)col row:(NSInteger)row {
    MGAccountCell* cell = [tv makeViewWithIdentifier:@"account" owner:self];
    if (!cell) {
        cell = [[MGAccountCell alloc] initWithFrame:NSZeroRect];
        cell.identifier = @"account";
    }
    if (row < 0 || row >= (NSInteger)_accounts.size()) return cell;
    const Account& a = _accounts[(size_t)row];
    cell.avatar.account = a;
    cell.avatar.needsDisplay = YES;
    cell.title.stringValue = a.name.empty() ? NS(a.login) : [NSString stringWithFormat:@"%@ (%@)", NS(a.name), NS(a.login)];
    cell.detail.stringValue = [NSString stringWithFormat:@"%@ · %@", NS(InfoOf(a.forge).name), NS(a.Host())];
    return cell;
}

- (void)tableViewSelectionDidChange:(NSNotification*)note {
    _remove.enabled = _table.selectedRow >= 0;
}

- (void)addAccount:(id)sender {
    if (_sheet || !self.view.window) return;
    __weak MGIntegrationsPane* weakSelf = self;
    _sheet = [[MGAddAccountController alloc] initWithDone:^(NSString* note) {
        MGIntegrationsPane* me = weakSelf;
        if (!me) return;
        [me reload];
        me->_note.stringValue = note.length ? note : @"";
        me->_note.textColor = NSColor.systemOrangeColor;
    }];
    [self.view.window beginSheet:_sheet.window
               completionHandler:^(NSModalResponse r) {
                   MGIntegrationsPane* me = weakSelf;
                   if (me) me->_sheet = nil;
               }];
}

- (void)removeAccount:(id)sender {
    const NSInteger row = _table.selectedRow;
    if (row < 0 || row >= (NSInteger)_accounts.size()) return;
    const Account a = _accounts[(size_t)row];
    __weak MGIntegrationsPane* weakSelf = self;
    RunPrompt(self.view.window, RemoveAccountPrompt(a), ^(int b) {
        if (b != 0) return;
        RemoveAccount(a);
        [weakSelf reload];
    });
}

- (void)closeSheet {
    if (_sheet) [self.view.window endSheet:_sheet.window returnCode:NSModalResponseCancel];
}

- (NSWindow*)sheetWindow {
    return _sheet.window;
}

- (void)showSignInSample {
    [_sheet showSampleCode];
}

@end

// ---------------------------------------------------------------- the window

@implementation MGPreferencesController {
    NSTabViewController* _tabs;
    MGIntegrationsPane* _integrations;
}

+ (instancetype)shared {
    static MGPreferencesController* shared;
    if (!shared) shared = [[MGPreferencesController alloc] init];
    return shared;
}

- (instancetype)init {
    NSTabViewController* tabs = [[NSTabViewController alloc] init];
    tabs.tabStyle = NSTabViewControllerTabStyleToolbar;
    MGIntegrationsPane* integrations = [[MGIntegrationsPane alloc] initWithNibName:nil bundle:nil];
    const struct {
        NSViewController* pane;
        NSString* label;
        NSString* symbol;
        NSString* ident;
    } panes[] = {
        { [[MGGeneralPane alloc] initWithNibName:nil bundle:nil], @"General", @"gearshape", @"general" },
        { [[MGAppearancePane alloc] initWithNibName:nil bundle:nil], @"Appearance", @"paintpalette", @"appearance" },
        { [[MGGitPane alloc] initWithNibName:nil bundle:nil], @"Git", @"arrow.triangle.branch", @"git" },
        { integrations, @"Integrations", @"person.crop.circle", @"integrations" },
    };
    for (const auto& p : panes) {
        NSTabViewItem* item = [NSTabViewItem tabViewItemWithViewController:p.pane];
        item.label = p.label;
        item.image = [NSImage imageWithSystemSymbolName:p.symbol accessibilityDescription:p.label];
        item.identifier = p.ident;
        [tabs addTabViewItem:item];
    }
    NSWindow* w = [NSWindow windowWithContentViewController:tabs];
    w.styleMask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable;
    w.toolbarStyle = NSWindowToolbarStylePreference;
    w.releasedWhenClosed = NO;
    w.restorable = NO;
    if (!(self = [super initWithWindow:w])) return nil;
    _tabs = tabs;
    _integrations = integrations;
    [w center];
    return self;
}

- (void)showPane:(NSString*)identifier {
    [self showWindow:nil];
    for (NSInteger i = 0; i < (NSInteger)_tabs.tabViewItems.count; ++i)
        if ([_tabs.tabViewItems[(NSUInteger)i].identifier isEqual:identifier]) _tabs.selectedTabViewItemIndex = i;
}

- (void)openAddAccount {
    [self showPane:@"integrations"];
    [_integrations addAccount:nil];
}

- (void)closeSheets {
    [_integrations closeSheet];
}

- (NSWindow*)sheetWindow {
    return [_integrations sheetWindow];
}

- (void)showSignInSample {
    [_integrations showSignInSample];
}

@end
