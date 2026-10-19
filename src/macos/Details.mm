// The details of the selection: a commit (message, author, actions, changed files), or the uncommitted
// changes (unstaged and staged files, the commit message and the commit button).

#import "MacUI.h"

// ---------------------------------------------------------------- file lists

@interface DTFileItem : NSObject
@property(nonatomic) int section;           // 0 a commit's files, 1 unstaged, 2 staged
@property(nonatomic) NSInteger file;        // index in the section's files; -1 for folders and section headers
@property(nonatomic) BOOL header;           // "Unstaged" / "Staged"
@property(nonatomic) NSInteger count;       // headers and folders: files below
@property(nonatomic, copy) NSString* name;  // file name, folder (tree) or section title
@property(nonatomic, copy) NSString* dir;   // list: the file's folder, shown dimmed
@property(nonatomic, copy) NSString* path;  // in the repository; folders: their path
@property(nonatomic, strong) NSMutableArray<DTFileItem*>* children;
@end

@implementation DTFileItem
@end

static void AddNodes(NSMutableArray<DTFileItem*>* out, const TreeNode& n, int section, const std::vector<FileChange>& files, NSString* prefix) {
    for (const TreeNode& c : n.children) {
        DTFileItem* f = [[DTFileItem alloc] init];
        f.section = section;
        f.name = NS(c.name);
        f.dir = @"";
        if (c.IsFolder()) {
            f.file = -1;
            f.count = c.count;
            f.path = prefix.length ? [NSString stringWithFormat:@"%@/%@", prefix, f.name] : f.name;
            f.children = [NSMutableArray array];
            AddNodes(f.children, c, section, files, f.path);
        } else {
            f.file = c.index;
            f.path = NS(files[(size_t)c.index].path);
        }
        [out addObject:f];
    }
}

static NSMutableArray<DTFileItem*>* FileItems(const std::vector<FileChange>& files, int section, bool tree) {
    NSMutableArray<DTFileItem*>* out = [NSMutableArray array];
    if (tree) {
        AddNodes(out, FileTree(files), section, files, @"");
        return out;
    }
    for (size_t i = 0; i < files.size(); ++i) {
        const std::wstring& p = files[i].path;
        const size_t slash = p.find_last_of(L'/');
        DTFileItem* f = [[DTFileItem alloc] init];
        f.section = section;
        f.file = (NSInteger)i;
        f.name = NS(slash == std::wstring::npos ? p : p.substr(slash + 1));
        f.dir = slash == std::wstring::npos ? @"" : NS(p.substr(0, slash + 1));
        f.path = NS(p);
        [out addObject:f];
    }
    return out;
}

// An outline view that knows the row under the pointer: file rows show their discard button then.
@interface DTHoverOutlineView : NSOutlineView
@property(nonatomic) NSInteger hoverRow;
- (void)setHover:(NSInteger)row;
@end

@protocol DTHoverable
- (void)setHovered:(BOOL)hovered;
@end

@implementation DTHoverOutlineView {
    NSTrackingArea* _area;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) _hoverRow = -1;
    return self;
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_area) [self removeTrackingArea:_area];
    _area = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                         options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                           owner:self
                                        userInfo:nil];
    [self addTrackingArea:_area];
}

- (void)setHover:(NSInteger)row {
    if (row == _hoverRow) return;
    const NSInteger old = _hoverRow;
    _hoverRow = row;
    for (NSInteger r : { old, row }) {
        if (r < 0 || r >= self.numberOfRows) continue;
        id v = [self viewAtColumn:0 row:r makeIfNecessary:NO];
        if ([v conformsToProtocol:@protocol(DTHoverable)]) [(id<DTHoverable>)v setHovered:r == _hoverRow];
    }
}

- (void)mouseMoved:(NSEvent*)event {
    [super mouseMoved:event];
    [self setHover:[self rowAtPoint:[self convertPoint:event.locationInWindow fromView:nil]]];
}

- (void)mouseExited:(NSEvent*)event {
    [super mouseExited:event];
    [self setHover:-1];
}

- (void)scrollWheel:(NSEvent*)event {
    [super scrollWheel:event];
    [self setHover:[self rowAtPoint:[self convertPoint:event.locationInWindow fromView:nil]]];
}

@end

// A changed file: status letter, folder and name, lines changed, and the stage / discard buttons; or a
// section header ("Unstaged 3", Stage All), or a folder of the tree.
@interface DTFileCell : NSTableCellView <DTHoverable>
@property(nonatomic, strong) NSTextField* letter;
@property(nonatomic, strong) NSTextField* name;
@property(nonatomic, strong) NSTextField* dir;
@property(nonatomic, strong) NSTextField* delta;
@property(nonatomic, strong) NSButton* action;   // stage / unstage
@property(nonatomic, strong) NSButton* discard;  // shown while hovered
@property(nonatomic) BOOL canDiscard;
@property(nonatomic) BOOL hovered;
@end

@implementation DTFileCell

- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    _letter = MakeLabel(@"", [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightBold], nil);
    _letter.alignment = NSTextAlignmentCenter;
    _name = MakeLabel(@"", [NSFont systemFontOfSize:13], NSColor.labelColor);
    _dir = MakeLabel(@"", [NSFont systemFontOfSize:13], NSColor.secondaryLabelColor);
    _dir.lineBreakMode = NSLineBreakByTruncatingHead;
    _delta = MakeLabel(@"", [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular], NSColor.secondaryLabelColor);
    _action = [NSButton buttonWithTitle:@"" target:nil action:nil];
    _action.bezelStyle = NSBezelStyleInline;
    _action.controlSize = NSControlSizeSmall;
    _action.font = [NSFont systemFontOfSize:11];
    _discard = [NSButton buttonWithImage:Symbol(@"trash", 12, NSFontWeightRegular) target:nil action:nil];
    _discard.bordered = NO;
    _discard.toolTip = @"Discard changes…";
    _discard.hidden = YES;
    NSImageView* icon = [[NSImageView alloc] init];
    for (NSView* v in @[ _letter, _name, _dir, _delta, _action, _discard, icon ]) [self addSubview:v];
    self.imageView = icon;  // (a weak outlet: the cell's subviews keep it)
    return self;
}

- (void)setHovered:(BOOL)hovered {
    _hovered = hovered;
    _discard.hidden = !(hovered && _canDiscard);
    self.needsLayout = YES;
}

- (void)layout {
    [super layout];
    const NSRect b = self.bounds;
    const CGFloat cy = NSMidY(b);
    CGFloat x = 2, right = NSMaxX(b) - 4;
    auto place = [&](NSView* v, CGFloat left, CGFloat w) {
        const CGFloat h = std::min<CGFloat>(v.fittingSize.height, b.size.height);
        v.frame = NSMakeRect(left, round(cy - h / 2), std::max<CGFloat>(0, w), h);
    };
    if (!_action.hidden) {
        const CGFloat w = _action.fittingSize.width;
        place(_action, right - w, w);
        right -= w + 6;
    }
    if (!_discard.hidden) {
        place(_discard, right - 18, 18);
        right -= 24;
    }
    if (!_letter.hidden) {
        place(_letter, x, 16);
        x += 22;
    }
    if (!self.imageView.hidden) {
        place(self.imageView, x, 16);
        x += 22;
    }
    if (_delta.stringValue.length && !_delta.hidden) {
        const CGFloat w = _delta.fittingSize.width;
        place(_delta, right - w, w);
        right -= w + 8;
    }
    const CGFloat room = std::max<CGFloat>(0, right - x);
    const CGFloat nameW = std::min(_name.fittingSize.width, room);
    const CGFloat dirW = _dir.stringValue.length ? std::min(_dir.fittingSize.width, room - nameW) : 0;
    if (dirW > 24) {
        place(_dir, x, dirW);
        place(_name, x + dirW, room - dirW);
        _dir.hidden = NO;
    } else {
        place(_name, x, room);
        _dir.hidden = YES;
    }
}

@end

// A label that lets clicks through to what is under it (the placeholder of a text view).
@interface DTPassThroughLabel : NSTextField
@end

@implementation DTPassThroughLabel

- (NSView*)hitTest:(NSPoint)point {
    return nil;
}

@end

// The author's initials on their color.
@interface DTAvatarView : NSView
- (void)setAuthor:(const Author&)author;
@end

@implementation DTAvatarView {
    Author _author;
}

- (void)setAuthor:(const Author&)author {
    _author = author;
    self.needsDisplay = YES;
}

- (void)drawRect:(NSRect)dirty {
    DrawAvatar(self.bounds, _author);
}

@end

// ---------------------------------------------------------------- controller

@implementation DTDetailsController {
    __weak DTRepoWindowController* _owner;
    BOOL _tree;  // file lists as a tree of folders
    BOOL _syncing;
    BOOL _reloading;
    NSTextField* _empty;

    // a commit
    NSView* _commitPane;
    NSStackView* _info;
    NSTextField* _kind;
    NSButton* _idButton;
    NSButton* _more;
    NSTextField* _summary;
    NSScrollView* _bodyScroll;
    NSTextView* _body;
    NSLayoutConstraint* _bodyHeight;
    DTAvatarView* _avatar;
    NSTextField* _authorName;
    NSStackView* _dateLine;
    NSTextField* _committer;
    NSButton* _act[3];
    NSTextField* _filesStats;
    NSButton* _treeButton;
    DTHoverOutlineView* _commitList;
    NSScrollView* _commitListScroll;
    NSTextField* _filesNote;  // loading, errors, no files
    NSArray<DTFileItem*>* _commitItems;
    std::weak_ptr<CommitDetails> _shownDetails;
    BOOL _commitTree;  // what the lists show
    BOOL _wipTree;

    // the uncommitted changes
    NSView* _wipPane;
    NSTextField* _branch;
    NSTextField* _wipCount;
    DTHoverOutlineView* _wipList;
    NSArray<DTFileItem*>* _wipItems;
    std::weak_ptr<WorkStatus> _shownStatus;
    NSMutableSet<NSString*>* _collapsed;  // sections and folders closed by the user
    NSButton* _amend;
    NSTextField* _summaryField;
    NSTextField* _left;  // characters left in the summary line
    NSTextView* _bodyField;
    NSTextField* _bodyPlaceholder;
    NSButton* _commitButton;
    NSString* _amendPrefill;  // the message put in when amend was turned on
}

- (instancetype)initWithOwner:(DTRepoWindowController*)owner {
    if ((self = [super initWithNibName:nil bundle:nil])) {
        _owner = owner;
        _collapsed = [NSMutableSet set];
    }
    return self;
}

static NSScrollView* Scrolling(NSView* doc) {
    NSScrollView* s = [[NSScrollView alloc] init];
    s.documentView = doc;
    s.hasVerticalScroller = YES;
    s.autohidesScrollers = YES;
    s.drawsBackground = NO;
    s.translatesAutoresizingMaskIntoConstraints = NO;
    return s;
}

- (DTHoverOutlineView*)makeFileList {
    DTHoverOutlineView* ov = [[DTHoverOutlineView alloc] initWithFrame:NSZeroRect];
    NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:@"file"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    [ov addTableColumn:col];
    ov.outlineTableColumn = col;
    ov.headerView = nil;
    ov.style = NSTableViewStylePlain;
    ov.backgroundColor = NSColor.clearColor;
    ov.rowHeight = 26;
    ov.indentationPerLevel = 14;
    ov.autoresizesOutlineColumn = NO;
    ov.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle;
    ov.dataSource = self;
    ov.delegate = self;
    ov.menu = [[NSMenu alloc] initWithTitle:@"File"];
    ov.menu.delegate = self;
    return ov;
}

- (void)loadView {
    NSView* v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 360, 600)];
    _empty = MakeLabel(@"Select a commit to see its details", [NSFont systemFontOfSize:13], NSColor.secondaryLabelColor);
    _empty.alignment = NSTextAlignmentCenter;
    _empty.translatesAutoresizingMaskIntoConstraints = NO;
    [v addSubview:_empty];
    [NSLayoutConstraint activateConstraints:@[
        [_empty.centerXAnchor constraintEqualToAnchor:v.centerXAnchor],
        [_empty.centerYAnchor constraintEqualToAnchor:v.centerYAnchor],
    ]];
    [self buildCommitPane:v];
    [self buildWipPane:v];
    self.view = v;
    [self reload];
}

- (void)pin:(NSView*)pane into:(NSView*)v {
    pane.translatesAutoresizingMaskIntoConstraints = NO;
    [v addSubview:pane];
    [NSLayoutConstraint activateConstraints:@[
        [pane.leadingAnchor constraintEqualToAnchor:v.leadingAnchor],
        [pane.trailingAnchor constraintEqualToAnchor:v.trailingAnchor],
        [pane.topAnchor constraintEqualToAnchor:v.safeAreaLayoutGuide.topAnchor],
        [pane.bottomAnchor constraintEqualToAnchor:v.bottomAnchor],
    ]];
}

- (void)buildCommitPane:(NSView*)v {
    _commitPane = [[NSView alloc] init];
    [self pin:_commitPane into:v];

    _kind = MakeLabel(@"Commit", [NSFont systemFontOfSize:11], NSColor.secondaryLabelColor);
    _idButton = [NSButton buttonWithTitle:@"" image:Symbol(@"doc.on.doc", 10, NSFontWeightRegular) target:self action:@selector(copyId:)];
    _idButton.bezelStyle = NSBezelStyleInline;
    _idButton.imagePosition = NSImageTrailing;
    _idButton.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    _idButton.toolTip = @"Copy the commit ID";
    _more = [NSButton buttonWithImage:Symbol(@"ellipsis.circle", 15, NSFontWeightRegular) target:self action:@selector(showMore:)];
    _more.bordered = NO;
    _more.toolTip = @"More actions";
    NSView* spacer = [[NSView alloc] init];
    [spacer setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView* top = [NSStackView stackViewWithViews:@[ _kind, _idButton, spacer, _more ]];
    top.spacing = 8;
    top.distribution = NSStackViewDistributionFill;

    _summary = [NSTextField wrappingLabelWithString:@""];
    _summary.font = [NSFont systemFontOfSize:15 weight:NSFontWeightSemibold];
    _summary.selectable = YES;

    _bodyScroll = [NSTextView scrollableTextView];
    _bodyScroll.drawsBackground = NO;
    _bodyScroll.autohidesScrollers = YES;
    _bodyScroll.translatesAutoresizingMaskIntoConstraints = NO;
    _body = _bodyScroll.documentView;
    _body.editable = NO;
    _body.selectable = YES;
    _body.drawsBackground = NO;
    _body.font = [NSFont systemFontOfSize:12.5];
    _body.textColor = NSColor.secondaryLabelColor;
    _body.textContainerInset = NSMakeSize(0, 2);
    _body.textContainer.lineFragmentPadding = 0;
    _bodyHeight = [_bodyScroll.heightAnchor constraintEqualToConstant:40];
    _bodyHeight.active = YES;

    _avatar = [[DTAvatarView alloc] initWithFrame:NSMakeRect(0, 0, 30, 30)];
    [_avatar.widthAnchor constraintEqualToConstant:30].active = YES;
    [_avatar.heightAnchor constraintEqualToConstant:30].active = YES;
    _authorName = MakeLabel(@"", [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold], NSColor.labelColor);
    _dateLine = [NSStackView stackViewWithViews:@[]];
    _dateLine.spacing = 4;
    NSStackView* who = [NSStackView stackViewWithViews:@[ _authorName, _dateLine ]];
    who.orientation = NSUserInterfaceLayoutOrientationVertical;
    who.alignment = NSLayoutAttributeLeading;
    who.spacing = 2;
    NSStackView* author = [NSStackView stackViewWithViews:@[ _avatar, who ]];
    author.spacing = 10;
    author.alignment = NSLayoutAttributeCenterY;

    _committer = MakeLabel(@"", [NSFont systemFontOfSize:11], NSColor.secondaryLabelColor);

    for (int k = 0; k < 3; ++k) {
        _act[k] = [NSButton buttonWithTitle:@"" target:self action:@selector(detailAction:)];
        _act[k].tag = k;
    }
    NSStackView* actions = [NSStackView stackViewWithViews:@[ _act[0], _act[1], _act[2] ]];
    actions.spacing = 8;

    NSBox* line = [[NSBox alloc] init];
    line.boxType = NSBoxSeparator;

    NSTextField* filesTitle = MakeLabel(@"Changed Files", [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold], NSColor.labelColor);
    _filesStats = MakeLabel(@"", [NSFont systemFontOfSize:11], NSColor.secondaryLabelColor);
    _treeButton = [NSButton buttonWithImage:Symbol(@"list.bullet.indent", 12, NSFontWeightRegular) target:self action:@selector(toggleTree:)];
    _treeButton.bordered = NO;
    NSView* spacer2 = [[NSView alloc] init];
    [spacer2 setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView* filesHeader = [NSStackView stackViewWithViews:@[ filesTitle, spacer2, _filesStats, _treeButton ]];
    filesHeader.spacing = 8;
    filesHeader.distribution = NSStackViewDistributionFill;

    _info = [NSStackView stackViewWithViews:@[ top, _summary, _bodyScroll, author, _committer, actions, line, filesHeader ]];
    _info.orientation = NSUserInterfaceLayoutOrientationVertical;
    _info.alignment = NSLayoutAttributeLeading;
    _info.spacing = 10;
    [_info setCustomSpacing:14 afterView:_bodyScroll];
    [_info setCustomSpacing:16 afterView:actions];
    [_info setCustomSpacing:12 afterView:line];
    _info.translatesAutoresizingMaskIntoConstraints = NO;
    [_commitPane addSubview:_info];

    _commitList = [self makeFileList];
    _commitListScroll = Scrolling(_commitList);
    [_commitPane addSubview:_commitListScroll];
    _filesNote = MakeLabel(@"", [NSFont systemFontOfSize:12], NSColor.secondaryLabelColor);
    _filesNote.translatesAutoresizingMaskIntoConstraints = NO;
    [_commitPane addSubview:_filesNote];

    [NSLayoutConstraint activateConstraints:@[
        [_info.topAnchor constraintEqualToAnchor:_commitPane.topAnchor constant:14],
        [_info.leadingAnchor constraintEqualToAnchor:_commitPane.leadingAnchor constant:16],
        [_info.trailingAnchor constraintEqualToAnchor:_commitPane.trailingAnchor constant:-16],
        [top.widthAnchor constraintEqualToAnchor:_info.widthAnchor],
        [_bodyScroll.widthAnchor constraintEqualToAnchor:_info.widthAnchor],
        [line.widthAnchor constraintEqualToAnchor:_info.widthAnchor],
        [filesHeader.widthAnchor constraintEqualToAnchor:_info.widthAnchor],
        [_commitListScroll.topAnchor constraintEqualToAnchor:_info.bottomAnchor constant:6],
        [_commitListScroll.leadingAnchor constraintEqualToAnchor:_commitPane.leadingAnchor constant:8],
        [_commitListScroll.trailingAnchor constraintEqualToAnchor:_commitPane.trailingAnchor constant:-8],
        [_commitListScroll.bottomAnchor constraintEqualToAnchor:_commitPane.bottomAnchor],
        [_filesNote.topAnchor constraintEqualToAnchor:_info.bottomAnchor constant:10],
        [_filesNote.leadingAnchor constraintEqualToAnchor:_info.leadingAnchor],
        [_filesNote.trailingAnchor constraintLessThanOrEqualToAnchor:_info.trailingAnchor],
    ]];
}

- (void)buildWipPane:(NSView*)v {
    _wipPane = [[NSView alloc] init];
    [self pin:_wipPane into:v];

    NSTextField* where = MakeLabel(@"Working tree", [NSFont systemFontOfSize:11], NSColor.secondaryLabelColor);
    NSImageView* icon = [NSImageView imageViewWithImage:Symbol(@"arrow.triangle.branch", 11, NSFontWeightRegular)];
    icon.contentTintColor = NSColor.controlAccentColor;
    _branch = MakeLabel(@"", [NSFont systemFontOfSize:12 weight:NSFontWeightMedium], NSColor.labelColor);
    NSStackView* top = [NSStackView stackViewWithViews:@[ where, icon, _branch ]];
    top.spacing = 6;
    NSTextField* title = MakeLabel(@"Uncommitted Changes", [NSFont systemFontOfSize:15 weight:NSFontWeightSemibold], NSColor.labelColor);
    _wipCount = MakeLabel(@"", [NSFont systemFontOfSize:11], NSColor.secondaryLabelColor);
    NSButton* tree = [NSButton buttonWithImage:Symbol(@"list.bullet.indent", 12, NSFontWeightRegular) target:self action:@selector(toggleTree:)];
    tree.bordered = NO;
    tree.toolTip = @"Show as a tree";
    NSView* spacer = [[NSView alloc] init];
    [spacer setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView* countRow = [NSStackView stackViewWithViews:@[ _wipCount, spacer, tree ]];
    countRow.distribution = NSStackViewDistributionFill;
    NSStackView* head = [NSStackView stackViewWithViews:@[ top, title, countRow ]];
    head.orientation = NSUserInterfaceLayoutOrientationVertical;
    head.alignment = NSLayoutAttributeLeading;
    head.spacing = 6;
    [head setCustomSpacing:12 afterView:top];
    head.translatesAutoresizingMaskIntoConstraints = NO;
    [_wipPane addSubview:head];

    _wipList = [self makeFileList];
    NSScrollView* listScroll = Scrolling(_wipList);
    [_wipPane addSubview:listScroll];

    // the commit message
    NSBox* line = [[NSBox alloc] init];
    line.boxType = NSBoxSeparator;
    line.translatesAutoresizingMaskIntoConstraints = NO;
    [_wipPane addSubview:line];
    _amend = [NSButton checkboxWithTitle:@"Amend previous commit" target:self action:@selector(amendToggled:)];
    _summaryField = [NSTextField textFieldWithString:@""];
    _summaryField.placeholderString = @"Summary (required)";
    _summaryField.delegate = self;
    _left = MakeLabel(@"72", [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular], NSColor.tertiaryLabelColor);
    [_left setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView* summaryRow = [NSStackView stackViewWithViews:@[ _summaryField, _left ]];
    summaryRow.spacing = 8;
    summaryRow.distribution = NSStackViewDistributionFill;
    NSScrollView* bodyScroll = [NSTextView scrollableTextView];
    bodyScroll.borderType = NSBezelBorder;
    bodyScroll.translatesAutoresizingMaskIntoConstraints = NO;
    _bodyField = bodyScroll.documentView;
    _bodyField.font = [NSFont systemFontOfSize:13];
    _bodyField.richText = NO;
    _bodyField.allowsUndo = YES;
    _bodyField.automaticQuoteSubstitutionEnabled = NO;
    _bodyField.automaticDashSubstitutionEnabled = NO;
    _bodyField.automaticTextReplacementEnabled = NO;
    _bodyField.textContainerInset = NSMakeSize(2, 4);
    _bodyField.delegate = self;
    _bodyPlaceholder = [DTPassThroughLabel labelWithString:@"Description"];
    _bodyPlaceholder.font = [NSFont systemFontOfSize:13];
    _bodyPlaceholder.textColor = NSColor.placeholderTextColor;
    _bodyPlaceholder.translatesAutoresizingMaskIntoConstraints = NO;
    _commitButton = [NSButton buttonWithTitle:@"Commit" target:self action:@selector(commit:)];
    _commitButton.keyEquivalent = @"\r";
    _commitButton.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    _commitButton.bezelColor = NSColor.controlAccentColor;
    _commitButton.toolTip = @"Commit (⌘↩)";
    NSStackView* box = [NSStackView stackViewWithViews:@[ _amend, summaryRow, bodyScroll, _commitButton ]];
    box.orientation = NSUserInterfaceLayoutOrientationVertical;
    box.alignment = NSLayoutAttributeLeading;
    box.spacing = 10;
    box.translatesAutoresizingMaskIntoConstraints = NO;
    [_wipPane addSubview:box];
    [_wipPane addSubview:_bodyPlaceholder];

    [NSLayoutConstraint activateConstraints:@[
        [head.topAnchor constraintEqualToAnchor:_wipPane.topAnchor constant:14],
        [head.leadingAnchor constraintEqualToAnchor:_wipPane.leadingAnchor constant:16],
        [head.trailingAnchor constraintEqualToAnchor:_wipPane.trailingAnchor constant:-16],
        [countRow.widthAnchor constraintEqualToAnchor:head.widthAnchor],
        [listScroll.topAnchor constraintEqualToAnchor:head.bottomAnchor constant:8],
        [listScroll.leadingAnchor constraintEqualToAnchor:_wipPane.leadingAnchor constant:8],
        [listScroll.trailingAnchor constraintEqualToAnchor:_wipPane.trailingAnchor constant:-8],
        [listScroll.bottomAnchor constraintEqualToAnchor:line.topAnchor],
        [line.leadingAnchor constraintEqualToAnchor:_wipPane.leadingAnchor],
        [line.trailingAnchor constraintEqualToAnchor:_wipPane.trailingAnchor],
        [line.heightAnchor constraintEqualToConstant:1],
        [box.topAnchor constraintEqualToAnchor:line.bottomAnchor constant:12],
        [box.leadingAnchor constraintEqualToAnchor:_wipPane.leadingAnchor constant:16],
        [box.trailingAnchor constraintEqualToAnchor:_wipPane.trailingAnchor constant:-16],
        [box.bottomAnchor constraintEqualToAnchor:_wipPane.bottomAnchor constant:-16],
        [summaryRow.widthAnchor constraintEqualToAnchor:box.widthAnchor],
        [bodyScroll.widthAnchor constraintEqualToAnchor:box.widthAnchor],
        [bodyScroll.heightAnchor constraintEqualToConstant:84],
        [_commitButton.widthAnchor constraintEqualToAnchor:box.widthAnchor],
        [_bodyPlaceholder.leadingAnchor constraintEqualToAnchor:bodyScroll.leadingAnchor constant:6],
        [_bodyPlaceholder.topAnchor constraintEqualToAnchor:bodyScroll.topAnchor constant:5],
    ]];
}

// Wrapping labels learn their width here.
- (void)viewDidLayout {
    [super viewDidLayout];
    const CGFloat w = _info.frame.size.width;
    if (w > 0 && fabs(_summary.preferredMaxLayoutWidth - w) > 0.5) {
        _summary.preferredMaxLayoutWidth = w;
        [self fitBody];
    }
}

// The commit's description: as tall as its text, up to a limit (it scrolls past it).
- (void)fitBody {
    NSLayoutManager* lm = _body.layoutManager;
    [lm ensureLayoutForTextContainer:_body.textContainer];
    const CGFloat h = ceil([lm usedRectForTextContainer:_body.textContainer].size.height) + 4;
    const CGFloat height = std::min<CGFloat>(h, 170);
    if (fabs(_bodyHeight.constant - height) > 0.5) _bodyHeight.constant = height;
}

// ---- reload

- (void)reload {
    if (!_commitPane) return;  // built when the view loads
    RepoSession* s = _owner.session;
    const bool wip = s && s->data && s->selWip && s->graph && s->graph->hasWip;
    const bool commit = s && s->data && !wip && !s->selWip && s->selCommit >= 0 && s->selCommit < (int32_t)s->data->commits.size();
    _wipPane.hidden = !wip;
    _commitPane.hidden = !commit;
    _empty.hidden = wip || commit;
    _empty.stringValue = s && s->data ? @"Select a commit to see its details" : @"";
    if (commit) [self reloadCommit];
    if (wip) [self reloadWip];
    [self refreshState];
}

- (void)reloadCommit {
    RepoSession* s = _owner.session;
    const RepoData& d = *s->data;
    const Commit& cm = d.commits[(size_t)s->selCommit];
    const std::shared_ptr<CommitDetails> det = s->details;
    const bool ok = det && det->ok && det->id == cm.id;
    const bool stash = (cm.flags & CF_STASH) && cm.stash >= 0 && cm.stash < (int32_t)d.stashes.size();

    _kind.stringValue = stash ? @"Stash" : @"Commit";
    _idButton.title = NS(stash ? d.stashes[(size_t)cm.stash].name : cm.id.Hex(7));
    _summary.stringValue = NS(stash ? d.stashes[(size_t)cm.stash].message : ok ? det->summary : CommitSubject(d, cm));
    NSString* body = ok && !stash ? NS(det->body) : @"";
    if (![_body.string isEqualToString:body]) _body.string = body;
    _bodyScroll.hidden = body.length == 0;
    [self fitBody];

    const Author& au = d.authors[cm.author];
    [_avatar setAuthor:au];
    _authorName.stringValue = NS(au.name);
    _authorName.toolTip = NS(au.email);
    for (NSView* v in _dateLine.arrangedSubviews.copy) [v removeFromSuperview];
    NSFont* small = [NSFont systemFontOfSize:11];
    NSString* date = NS(FormatDateLong(cm.authorTime));
    const int32_t* ps = d.Parents(cm);
    [_dateLine addArrangedSubview:MakeLabel([date stringByAppendingString:cm.parentCount == 0 ? @" · root commit"
                                                                       : cm.parentCount > 1 ? @" · parents" : @" · parent"],
                                            small, NSColor.secondaryLabelColor)];
    for (int k = 0; k < (int)cm.parentCount && k < 8; ++k) {
        NSString* hex = ps[k] >= 0 ? NS(d.commits[(size_t)ps[k]].id.Hex(7))
                        : ok && k < (int)det->parents.size() ? NS(det->parents[(size_t)k].Hex(7))
                                                            : @"???????";
        NSButton* b = [NSButton buttonWithTitle:@"" target:self action:@selector(goToParent:)];
        b.bordered = NO;
        b.attributedTitle = [[NSAttributedString alloc]
            initWithString:hex
                attributes:@{ NSFontAttributeName : [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular],
                              NSForegroundColorAttributeName : ps[k] >= 0 ? NSColor.linkColor : NSColor.tertiaryLabelColor }];
        b.tag = ps[k];
        b.enabled = ps[k] >= 0;
        b.toolTip = ps[k] >= 0 ? NS(CommitSubject(d, d.commits[(size_t)ps[k]])) : @"Not loaded";
        [_dateLine addArrangedSubview:b];
    }
    const std::wstring by = ok ? CommitterNote(d, cm, *det) : std::wstring();
    _committer.stringValue = NS(by);
    _committer.hidden = by.empty();

    if (stash) {
        NSArray<NSString*>* titles = @[ @"Pop", @"Apply", @"Delete…" ];
        for (int k = 0; k < 3; ++k) _act[k].title = titles[(NSUInteger)k];
    } else {
        _act[0].title = @"Check Out";
        _act[1].title = @"New Branch…";
        _act[2].title = @"Revert…";
    }

    // changed files
    _filesStats.stringValue = ok ? NS(FilesSummary(*det)) : @"";
    _treeButton.toolTip = _tree ? @"Show as a list" : @"Show as a tree";
    if (!ok) {
        _filesNote.stringValue = det && !det->ok && det->id == cm.id ? NS(det->error) : @"Loading changes…";
        _filesNote.textColor = det && !det->ok ? NSColor.systemRedColor : NSColor.secondaryLabelColor;
    } else {
        _filesNote.stringValue = det->files.empty() ? @"No file changes" : @"";
        _filesNote.textColor = NSColor.secondaryLabelColor;
    }
    _filesNote.hidden = _filesNote.stringValue.length == 0;
    if (!ok) {
        _commitItems = @[];
        _shownDetails.reset();
        [_commitList reloadData];
    } else if (_shownDetails.lock() != det || _commitTree != _tree) {
        _shownDetails = det;
        _commitTree = _tree;
        _commitItems = FileItems(det->files, 0, _tree);
        [self reloadList:_commitList];
        [_commitList scrollRowToVisible:0];
    }
}

- (void)reloadWip {
    RepoSession* s = _owner.session;
    const RepoData& d = *s->data;
    const std::shared_ptr<WorkStatus> st = s->status;
    _branch.stringValue = d.headBranch.empty() ? @"detached HEAD" : NS(d.headBranch);
    _wipCount.stringValue = st ? NS(Plural((size_t)st->ChangedFiles(), L"file changed", L"files changed")) : @"Reading the working tree…";
    _amend.enabled = !d.unborn;
    if (!st) {
        _wipItems = @[];
        _shownStatus.reset();
        [_wipList reloadData];
        return;
    }
    if (_shownStatus.lock() == st && _wipTree == _tree && _wipItems.count) return;
    _shownStatus = st;
    _wipTree = _tree;
    NSMutableArray<DTFileItem*>* roots = [NSMutableArray array];
    for (int section = 1; section <= 2; ++section) {
        const std::vector<FileChange>& files = section == 1 ? st->unstaged : st->staged;
        DTFileItem* h = [[DTFileItem alloc] init];
        h.header = YES;
        h.section = section;
        h.file = -1;
        h.count = (NSInteger)files.size();
        h.name = section == 1 ? @"Unstaged" : @"Staged";
        h.path = @"";
        h.children = FileItems(files, section, _tree);
        [roots addObject:h];
    }
    _wipItems = roots;
    [self reloadList:_wipList];
}

// Reloads a list and opens what the user did not close.
- (void)reloadList:(DTHoverOutlineView*)ov {
    _reloading = YES;
    [ov reloadData];
    [ov setHover:-1];
    NSArray<DTFileItem*>* roots = ov == _wipList ? _wipItems : _commitItems;
    [self expand:roots inList:ov];
    _reloading = NO;
    [self syncFileSelection];
}

- (NSString*)keyOf:(DTFileItem*)it {
    return [NSString stringWithFormat:@"%d:%@:%@", it.section, it.header ? @"#" : @"", it.path];
}

- (void)expand:(NSArray<DTFileItem*>*)items inList:(NSOutlineView*)ov {
    for (DTFileItem* it in items) {
        if (!it.children.count || [_collapsed containsObject:[self keyOf:it]]) continue;
        [ov expandItem:it];
        [self expand:it.children inList:ov];
    }
}

- (DTFileItem*)itemForSection:(int)section path:(const std::wstring&)path among:(NSArray<DTFileItem*>*)items {
    for (DTFileItem* it in items) {
        if (it.file >= 0 && it.section == section && WS(it.path) == path) return it;
        if (DTFileItem* found = [self itemForSection:section path:path among:it.children]) return found;
    }
    return nil;
}

// The file shown in the diff is the selected one.
- (void)syncFileSelection {
    RepoSession* s = _owner.session;
    for (DTHoverOutlineView* ov in @[ _commitList, _wipList ]) {
        NSArray<DTFileItem*>* roots = ov == _wipList ? _wipItems : _commitItems;
        DTFileItem* want = s && s->diff.open ? [self itemForSection:s->diff.req.section path:s->diff.req.path among:roots] : nil;
        const NSInteger row = want ? [ov rowForItem:want] : -1;
        if (row == ov.selectedRow) continue;
        _syncing = YES;
        if (row >= 0) [ov selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row] byExtendingSelection:NO];
        else [ov deselectAll:nil];
        _syncing = NO;
    }
}

// ---- state of the buttons

- (void)refreshState {
    if (!_commitPane) return;
    RepoSession* s = _owner.session;
    if (!s || !s->data) return;
    const RepoData& d = *s->data;
    const bool busy = s->opBusy;
    if (!_commitPane.hidden && s->selCommit >= 0 && s->selCommit < (int32_t)d.commits.size()) {
        const Commit& cm = d.commits[(size_t)s->selCommit];
        if (cm.flags & CF_STASH) {
            for (int k = 0; k < 3; ++k) _act[k].enabled = !busy;
            _act[0].toolTip = @"Apply the stash to the working tree and delete it";
            _act[1].toolTip = @"Apply the stash and keep it";
            _act[2].toolTip = @"Delete the stash";
        } else {
            std::wstring tip, branchTip;
            _act[0].enabled = s->CanCheckout(s->selCommit, &tip);
            _act[0].toolTip = NS(tip);
            _act[1].enabled = s->CanRun(ActBranch, &branchTip);
            _act[1].toolTip = @"Create a branch at this commit";
            const std::wstring why = s->CanRevert(s->selCommit);
            _act[2].enabled = why.empty();
            _act[2].toolTip = why.empty() ? @"Create a commit that undoes this one" : NS(why);
        }
    }
    if (!_wipPane.hidden) {
        const size_t staged = s->status ? s->status->staged.size() : 0;
        const bool amend = _amend.state == NSControlStateValueOn && !d.unborn;
        const bool committing = busy && s->opButton == kCommitOp;
        const bool hasSummary = !TrimW(WS(_summaryField.stringValue)).empty();
        _commitButton.title = committing ? (amend ? @"Amending…" : @"Committing…") : NS(CommitButtonLabel(staged, amend));
        _commitButton.enabled = !busy && hasSummary && (amend || staged > 0);
        const int left = 72 - (int)_summaryField.stringValue.length;
        _left.stringValue = [NSString stringWithFormat:@"%d", left];
        _left.textColor = left < 0 ? NSColor.systemRedColor : left < 8 ? NSColor.systemOrangeColor : NSColor.tertiaryLabelColor;
        _bodyPlaceholder.hidden = _bodyField.string.length > 0;
    }
    [self syncFileSelection];
}

// ---- the commit message

- (void)controlTextDidChange:(NSNotification*)note {
    [self refreshState];
}

- (void)textDidChange:(NSNotification*)note {
    [self refreshState];
}

// Enter in the summary moves to the description.
- (BOOL)control:(NSControl*)control textView:(NSTextView*)tv doCommandBySelector:(SEL)cmd {
    if (control == _summaryField && cmd == @selector(insertNewline:)) {
        [self.view.window makeFirstResponder:_bodyField];
        return YES;
    }
    return NO;
}

- (void)amendToggled:(NSButton*)b {
    RepoSession* s = _owner.session;
    if (!s || !s->data || s->data->unborn) {
        b.state = NSControlStateValueOff;
        return;
    }
    if (b.state == NSControlStateValueOn) {
        // start from the message being amended (it arrives as a suggestion)
        if (!_summaryField.stringValue.length && !_bodyField.string.length) s->RequestHeadMessage();
    } else {
        NSString* now = [NSString stringWithFormat:@"%@\n%@", _summaryField.stringValue, _bodyField.string];
        if (_amendPrefill.length && [now isEqualToString:_amendPrefill]) {
            _summaryField.stringValue = @"";
            _bodyField.string = @"";
        }
        _amendPrefill = nil;
    }
    [self refreshState];
}

- (void)applySuggestion {
    RepoSession* s = _owner.session;
    if (!s || _summaryField.stringValue.length || _bodyField.string.length) return;  // never over what was typed
    const MessageSuggestion& m = s->suggestion;
    if (m.amend && _amend.state != NSControlStateValueOn) return;
    _summaryField.stringValue = NS(m.summary);
    _bodyField.string = NS(m.body);
    if (m.amend) _amendPrefill = [NSString stringWithFormat:@"%@\n%@", _summaryField.stringValue, _bodyField.string];
    [self refreshState];
}

- (void)clearMessage {
    _summaryField.stringValue = @"";
    _bodyField.string = @"";
    _amend.state = NSControlStateValueOff;
    _amendPrefill = nil;
    [self refreshState];
}

- (void)focusSummary {
    [self view];
    [self.view.window makeFirstResponder:_summaryField];
}

- (void)commit:(id)sender {
    RepoSession* s = _owner.session;
    if (!s) return;
    const RepoSession::CommitResult r =
        s->Commit(WS(_summaryField.stringValue), WS(_bodyField.string), _amend.state == NSControlStateValueOn);
    if (r == RepoSession::CommitNoSummary) [self.view.window makeFirstResponder:_summaryField];
    [self refreshState];
}

// ---- commit actions

- (void)copyId:(id)sender {
    RepoSession* s = _owner.session;
    if (s && s->data && s->selCommit >= 0 && s->selCommit < (int32_t)s->data->commits.size())
        [_owner copyText:NS(s->data->commits[(size_t)s->selCommit].id.Hex())];
}

- (void)showMore:(NSButton*)sender {
    RepoSession* s = _owner.session;
    if (!s) return;
    NSMenu* m = [[NSMenu alloc] initWithTitle:@"Commit"];
    [_owner fillCommitMenu:m commit:s->selCommit];
    [m popUpMenuPositioningItem:nil atLocation:NSMakePoint(0, sender.isFlipped ? NSMaxY(sender.bounds) + 4 : -4) inView:sender];
}

- (void)goToParent:(NSButton*)sender {
    [_owner selectCommit:(int32_t)sender.tag];
}

- (void)detailAction:(NSButton*)sender {
    RepoSession* s = _owner.session;
    if (!s || !s->data || s->selCommit < 0 || s->selCommit >= (int32_t)s->data->commits.size()) return;
    const Commit& cm = s->data->commits[(size_t)s->selCommit];
    if (cm.flags & CF_STASH) {
        if (cm.stash < 0) return;
        if (sender.tag == 0) s->Pop(cm.stash, false);
        else if (sender.tag == 1) s->Pop(cm.stash, true);
        else [_owner dropStash:cm.stash];
        return;
    }
    if (sender.tag == 0) [_owner checkoutCommit:s->selCommit];
    else if (sender.tag == 1) [_owner newBranchAt:s->selCommit];
    else [_owner revertCommit:s->selCommit];
}

- (void)toggleTree:(id)sender {
    _tree = !_tree;
    [self reload];
}

// ---- file lists

- (NSArray<DTFileItem*>*)rootsOf:(NSOutlineView*)ov {
    return ov == _wipList ? _wipItems : _commitItems;
}

- (NSInteger)outlineView:(NSOutlineView*)ov numberOfChildrenOfItem:(id)item {
    return item ? (NSInteger)((DTFileItem*)item).children.count : (NSInteger)[self rootsOf:ov].count;
}

- (id)outlineView:(NSOutlineView*)ov child:(NSInteger)index ofItem:(id)item {
    return item ? ((DTFileItem*)item).children[(NSUInteger)index] : [self rootsOf:ov][(NSUInteger)index];
}

- (BOOL)outlineView:(NSOutlineView*)ov isItemExpandable:(id)item {
    return ((DTFileItem*)item).children.count > 0;
}

- (BOOL)outlineView:(NSOutlineView*)ov shouldSelectItem:(id)item {
    return ((DTFileItem*)item).file >= 0;
}

- (const FileChange*)changeOf:(DTFileItem*)it {
    RepoSession* s = _owner.session;
    if (!s || it.file < 0) return nullptr;
    const std::vector<FileChange>* files = nullptr;
    if (it.section == 0) files = s->details ? &s->details->files : nullptr;
    else if (s->status) files = it.section == 1 ? &s->status->unstaged : &s->status->staged;
    return files && it.file < (NSInteger)files->size() ? &(*files)[(size_t)it.file] : nullptr;
}

- (NSView*)outlineView:(NSOutlineView*)ov viewForTableColumn:(NSTableColumn*)col item:(id)item {
    DTFileItem* it = item;
    DTFileCell* c = [ov makeViewWithIdentifier:@"file" owner:self];
    if (!c) {
        c = [[DTFileCell alloc] initWithFrame:NSZeroRect];
        c.identifier = @"file";
        c.action.target = self;
        c.action.action = @selector(rowAction:);
        c.discard.target = self;
        c.discard.action = @selector(rowDiscard:);
    }
    const bool wip = it.section > 0;
    c.toolTip = nil;
    c.letter.hidden = YES;
    c.imageView.hidden = YES;
    c.dir.stringValue = @"";
    c.delta.stringValue = @"";
    c.name.font = [NSFont systemFontOfSize:13];
    c.name.textColor = NSColor.labelColor;
    c.canDiscard = NO;
    c.action.hidden = !wip;
    if (it.header) {
        c.name.stringValue = [NSString stringWithFormat:@"%@  %ld", it.name, (long)it.count];
        c.name.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
        c.action.title = it.section == 1 ? @"Stage All" : @"Unstage All";
        c.action.hidden = it.count == 0;
    } else if (it.file < 0) {  // a folder of the tree
        c.imageView.hidden = NO;
        c.imageView.image = Symbol(@"folder", 12, NSFontWeightRegular);
        c.imageView.contentTintColor = NSColor.secondaryLabelColor;
        c.name.stringValue = it.name;
        c.name.textColor = NSColor.secondaryLabelColor;
        c.action.title = it.section == 1 ? @"Stage" : @"Unstage";
    } else {
        const FileChange* f = [self changeOf:it];
        const wchar_t status = f ? f->status : L'M';
        c.letter.hidden = NO;
        c.letter.stringValue = NS(std::wstring(1, StatusLetter(status)));
        c.letter.textColor = StatusColor(status);
        c.name.stringValue = it.name;
        c.dir.stringValue = it.dir;
        if (f) c.delta.stringValue = NS(LineDelta(f->added, f->deleted));
        c.action.title = it.section == 1 ? @"Stage" : @"Unstage";
        c.canDiscard = it.section == 1 && status != L'U';
        c.toolTip = f && !f->oldPath.empty() ? [NSString stringWithFormat:@"%@ → %@", NS(f->oldPath), NS(f->path)] : it.path;
    }
    c.hovered = [ov isKindOfClass:DTHoverOutlineView.class] && [ov rowForItem:it] == ((DTHoverOutlineView*)ov).hoverRow;
    c.needsLayout = YES;
    return c;
}

- (void)outlineViewSelectionDidChange:(NSNotification*)note {
    if (_syncing || _reloading) return;
    NSOutlineView* ov = note.object;
    DTFileItem* it = [ov itemAtRow:ov.selectedRow];
    if (it && it.file >= 0) [_owner openDiff:it.section path:WS(it.path)];
}

- (void)outlineViewItemDidExpand:(NSNotification*)note {
    if (!_reloading) [_collapsed removeObject:[self keyOf:note.userInfo[@"NSObject"]]];
}

- (void)outlineViewItemDidCollapse:(NSNotification*)note {
    if (!_reloading) [_collapsed addObject:[self keyOf:note.userInfo[@"NSObject"]]];
}

- (DTFileItem*)itemForButton:(NSButton*)b {
    for (NSOutlineView* ov in @[ _wipList, _commitList ]) {
        const NSInteger row = [ov rowForView:b];
        if (row >= 0) return [ov itemAtRow:row];
    }
    return nil;
}

// The paths a row stands for: a file (and the old name of a rename), a folder, or everything (headers).
- (std::vector<std::wstring>)pathsOf:(DTFileItem*)it {
    std::vector<std::wstring> paths;
    if (it.header) return paths;
    paths.push_back(WS(it.path));
    if (const FileChange* f = [self changeOf:it]; f && !f->oldPath.empty()) paths.push_back(f->oldPath);
    return paths;
}

- (void)rowAction:(NSButton*)b {
    DTFileItem* it = [self itemForButton:b];
    if (it && it.section > 0) [_owner stagePaths:[self pathsOf:it] stage:it.section == 1];
}

- (void)rowDiscard:(NSButton*)b {
    DTFileItem* it = [self itemForButton:b];
    if (const FileChange* f = it ? [self changeOf:it] : nullptr) [_owner discardFile:*f];
}

- (void)menuNeedsUpdate:(NSMenu*)menu {
    [menu removeAllItems];
    menu.autoenablesItems = NO;
    NSOutlineView* ov = nil;
    for (NSOutlineView* x in @[ _wipList, _commitList ])
        if (x.menu == menu) ov = x;
    DTFileItem* it = ov ? [ov itemAtRow:ov.clickedRow] : nil;
    if (!it) return;
    auto add = [&](NSString* title, SEL action) {
        NSMenuItem* i = [menu addItemWithTitle:title action:action keyEquivalent:@""];
        i.target = self;
        i.representedObject = it;
        return i;
    };
    if (it.section > 0) {
        const bool stage = it.section == 1;
        if (it.header) add(stage ? @"Stage All" : @"Unstage All", @selector(menuStage:));
        else add(stage ? @"Stage" : @"Unstage", @selector(menuStage:));
        const FileChange* f = [self changeOf:it];
        if (stage && f && f->status != L'U') add(@"Discard Changes…", @selector(menuDiscard:));
    }
    if (it.header) return;
    if (menu.numberOfItems) [menu addItem:NSMenuItem.separatorItem];
    add(@"Copy Path", @selector(menuCopyPath:));
    add(@"Show in Finder", @selector(menuReveal:));
}

- (void)menuStage:(NSMenuItem*)i {
    DTFileItem* it = i.representedObject;
    [_owner stagePaths:[self pathsOf:it] stage:it.section == 1];
}

- (void)menuDiscard:(NSMenuItem*)i {
    if (const FileChange* f = [self changeOf:i.representedObject]) [_owner discardFile:*f];
}

- (void)menuCopyPath:(NSMenuItem*)i {
    [_owner copyText:((DTFileItem*)i.representedObject).path];
}

- (void)menuReveal:(NSMenuItem*)i {
    [_owner revealFile:WS(((DTFileItem*)i.representedObject).path)];
}

@end
