// The sidebar: a source list with the two pages (uncommitted changes, history), then the branches, the
// remotes, the tags, the stashes and the submodules. Picking an item selects its commit in the history.

#import "MacUI.h"

typedef NS_ENUM(NSInteger, DTSideKind) { DTSideGroup, DTSidePage, DTSideFolder, DTSideRemote, DTSideBranch, DTSideTag, DTSideStash, DTSideSubmodule };

@interface DTSideItem : NSObject
@property(nonatomic) DTSideKind kind;
@property(nonatomic, copy) NSString* title;
@property(nonatomic, copy) NSString* key;  // the same across reloads: expansion and selection follow it
@property(nonatomic) NSInteger index;      // the page, ref or stash
@property(nonatomic) NSInteger count;      // folders: the branches below
@property(nonatomic, strong) NSMutableArray<DTSideItem*>* children;
@end

@implementation DTSideItem
@end

static DTSideItem* Item(DTSideKind kind, NSString* title, NSString* key, NSInteger index) {
    DTSideItem* it = [[DTSideItem alloc] init];
    it.kind = kind;
    it.title = title;
    it.key = key;
    it.index = index;
    it.children = [NSMutableArray array];
    return it;
}

// Folders and leaves of a tree from navtree.h, under parent.
static void AddTree(DTSideItem* parent, const TreeNode& n, DTSideKind leaf) {
    for (const TreeNode& c : n.children) {
        NSString* title = NS(c.name);
        NSString* key = [NSString stringWithFormat:@"%@/%@", parent.key, title];
        if (c.IsFolder()) {
            DTSideItem* folder = Item(DTSideFolder, title, key, -1);
            folder.count = c.count;
            AddTree(folder, c, leaf);
            [parent.children addObject:folder];
        } else {
            [parent.children addObject:Item(leaf, title, key, c.index)];
        }
    }
}

static NSString* AheadBehind(const Ref& r) {
    NSMutableArray<NSString*>* parts = [NSMutableArray array];
    if (r.ahead) [parts addObject:[NSString stringWithFormat:@"↑%d", r.ahead]];
    if (r.behind) [parts addObject:[NSString stringWithFormat:@"↓%d", r.behind]];
    if (r.gone) [parts addObject:@"gone"];
    return [parts componentsJoinedByString:@" "];
}

// An icon, a name and a number or note at the right.
@interface DTSideCell : NSTableCellView
@property(nonatomic, strong) NSTextField* note;
@end

@implementation DTSideCell

- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    NSImageView* icon = [[NSImageView alloc] init];
    NSTextField* text = MakeLabel(@"", nil, nil);
    _note = MakeLabel(@"", [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular], NSColor.secondaryLabelColor);
    _note.alignment = NSTextAlignmentRight;
    for (NSView* v in @[ icon, text, _note ]) {
        v.translatesAutoresizingMaskIntoConstraints = NO;
        [self addSubview:v];
    }
    self.imageView = icon;
    self.textField = text;
    [text setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_note setContentCompressionResistancePriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_note setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    [NSLayoutConstraint activateConstraints:@[
        [icon.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:1],
        [icon.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [icon.widthAnchor constraintEqualToConstant:18],
        [text.leadingAnchor constraintEqualToAnchor:icon.trailingAnchor constant:5],
        [text.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [_note.leadingAnchor constraintGreaterThanOrEqualToAnchor:text.trailingAnchor constant:6],
        [_note.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-4],
        [_note.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
    ]];
    return self;
}

@end

@implementation DTSidebarController {
    __weak DTRepoWindowController* _owner;
    NSOutlineView* _outline;
    NSArray<DTSideItem*>* _roots;
    DTSideItem* _changes;
    DTSideItem* _history;
    NSMutableSet<NSString*>* _collapsedGroups;
    NSMutableSet<NSString*>* _expandedFolders;
    BOOL _syncing;
    BOOL _loadedOnce;
}

- (instancetype)initWithOwner:(DTRepoWindowController*)owner {
    if ((self = [super initWithNibName:nil bundle:nil])) {
        _owner = owner;
        _collapsedGroups = [NSMutableSet setWithArray:@[ @"remotes", @"tags", @"stashes", @"submodules" ]];
        _expandedFolders = [NSMutableSet set];
    }
    return self;
}

- (void)loadView {
    _outline = [[NSOutlineView alloc] init];
    NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:@"main"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    [_outline addTableColumn:col];
    _outline.outlineTableColumn = col;
    _outline.headerView = nil;
    _outline.style = NSTableViewStyleSourceList;
    _outline.floatsGroupRows = NO;
    _outline.autoresizesOutlineColumn = NO;
    _outline.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle;
    _outline.dataSource = self;
    _outline.delegate = self;
    _outline.target = self;
    _outline.doubleAction = @selector(doubleClicked:);
    _outline.menu = [[NSMenu alloc] initWithTitle:@"Sidebar"];
    _outline.menu.delegate = self;
    NSScrollView* scroll = [[NSScrollView alloc] init];
    scroll.documentView = _outline;
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers = YES;
    scroll.drawsBackground = NO;
    self.view = scroll;
    [self reload];
}

// ---- items

- (void)reload {
    if (!_outline) return;  // built when the view loads
    RepoSession* s = _owner.session;
    DTSideItem* selected = [_outline itemAtRow:_outline.selectedRow];
    NSString* selectedKey = selected.key;
    _changes = Item(DTSidePage, @"Changes", @"page:changes", 0);
    _history = Item(DTSidePage, @"History", @"page:history", 1);
    NSMutableArray<DTSideItem*>* roots = [NSMutableArray arrayWithObjects:_changes, _history, nil];
    if (s && s->data) {
        const RepoData& d = *s->data;
        DTSideItem* branches = Item(DTSideGroup, @"Branches", @"branches", -1);
        AddTree(branches, LocalBranchTree(d), DTSideBranch);
        [roots addObject:branches];
        if (!d.remotes.empty()) {
            DTSideItem* remotes = Item(DTSideGroup, @"Remotes", @"remotes", -1);
            const TreeNode tree = RemoteTree(d);
            for (const TreeNode& r : tree.children) {
                DTSideItem* remote = Item(DTSideRemote, NS(r.name), [@"remotes/" stringByAppendingString:NS(r.name)], -1);
                AddTree(remote, r, DTSideBranch);
                [remotes.children addObject:remote];
            }
            [roots addObject:remotes];
        }
        const TreeNode tags = TagTree(d);
        if (!tags.children.empty()) {
            DTSideItem* group = Item(DTSideGroup, @"Tags", @"tags", -1);
            AddTree(group, tags, DTSideTag);
            [roots addObject:group];
        }
        if (!d.stashes.empty()) {
            DTSideItem* group = Item(DTSideGroup, @"Stashes", @"stashes", -1);
            for (size_t i = 0; i < d.stashes.size(); ++i) {
                NSString* name = NS(d.stashes[i].name);
                NSString* message = NS(d.stashes[i].message);
                [group.children addObject:Item(DTSideStash, message.length ? message : name, [@"stash/" stringByAppendingString:name], (NSInteger)i)];
            }
            [roots addObject:group];
        }
        if (!d.submodules.empty()) {
            DTSideItem* group = Item(DTSideGroup, @"Submodules", @"submodules", -1);
            for (size_t i = 0; i < d.submodules.size(); ++i) {
                NSString* name = NS(d.submodules[i]);
                [group.children addObject:Item(DTSideSubmodule, name, [@"submodule/" stringByAppendingString:name], (NSInteger)i)];
            }
            [roots addObject:group];
        }
    }
    _roots = roots;
    _syncing = YES;
    [_outline reloadData];
    for (DTSideItem* group in _roots)
        if (group.kind == DTSideGroup && ![_collapsedGroups containsObject:group.key]) [_outline expandItem:group];
    [self expandFolders:_roots];
    if (s && s->data && !_loadedOnce) [self revealHeadBranch];
    _loadedOnce = s && s->data;
    _syncing = NO;
    // the item picked before, while its commit is still the selected one
    DTSideItem* again = selectedKey ? [self itemWithKey:selectedKey among:_roots] : nil;
    const NSInteger againRow = again ? [_outline rowForItem:again] : -1;
    if (againRow >= 0 && [self commitOf:again] >= 0 && s && !s->selWip && [self commitOf:again] == s->selCommit) {
        _syncing = YES;
        [_outline selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)againRow] byExtendingSelection:NO];
        _syncing = NO;
    }
    [self syncSelection];
}

- (void)expandFolders:(NSArray<DTSideItem*>*)items {
    for (DTSideItem* it in items) {
        if ((it.kind == DTSideFolder || it.kind == DTSideRemote) && [_expandedFolders containsObject:it.key]) [_outline expandItem:it];
        if (it.children.count) [self expandFolders:it.children];
    }
}

// The checked-out branch is visible after the first load, even inside folders.
- (void)revealHeadBranch {
    RepoSession* s = _owner.session;
    for (DTSideItem* group in _roots) {
        if (![group.key isEqualToString:@"branches"]) continue;
        NSMutableArray<DTSideItem*>* path = [NSMutableArray array];
        if ([self findHead:group session:s path:path])
            for (DTSideItem* it in path) [_outline expandItem:it];
    }
}

- (BOOL)findHead:(DTSideItem*)item session:(RepoSession*)s path:(NSMutableArray<DTSideItem*>*)path {
    for (DTSideItem* c in item.children) {
        if (c.kind == DTSideBranch && s->data->refs[(size_t)c.index].head) return YES;
        if (c.kind == DTSideFolder) {
            [path addObject:c];
            if ([self findHead:c session:s path:path]) return YES;
            [path removeLastObject];
        }
    }
    return NO;
}

- (DTSideItem*)itemWithKey:(NSString*)key among:(NSArray<DTSideItem*>*)items {
    for (DTSideItem* it in items) {
        if ([it.key isEqualToString:key]) return it;
        if (DTSideItem* found = [self itemWithKey:key among:it.children]) return found;
    }
    return nil;
}

- (int32_t)commitOf:(DTSideItem*)it {
    RepoSession* s = _owner.session;
    if (!s || !s->data) return -1;
    if ((it.kind == DTSideBranch || it.kind == DTSideTag) && it.index >= 0 && it.index < (NSInteger)s->data->refs.size())
        return s->data->refs[(size_t)it.index].commit;
    if (it.kind == DTSideStash && it.index >= 0 && it.index < (NSInteger)s->data->stashes.size()) return s->data->stashes[(size_t)it.index].commit;
    return -1;
}

- (void)refreshCounts {
    if (_changes) [_outline reloadItem:_changes];
    [self syncSelection];
}

// The pages follow the history: Uncommitted Changes while they are selected, else the picked branch, tag
// or stash while its commit is the selected one, else History.
- (void)syncSelection {
    RepoSession* s = _owner.session;
    DTSideItem* current = [_outline itemAtRow:_outline.selectedRow];
    DTSideItem* want = nil;
    if (s && s->data) {
        if (s->selWip) want = _changes;
        else if (current && current.kind != DTSidePage && s->selCommit >= 0 && [self commitOf:current] == s->selCommit) want = current;
        else want = _history;
    }
    if (want == current) return;
    const NSInteger row = want ? [_outline rowForItem:want] : -1;
    _syncing = YES;
    if (row >= 0) [_outline selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row] byExtendingSelection:NO];
    else [_outline deselectAll:nil];
    _syncing = NO;
}

// ---- outline view

- (NSInteger)outlineView:(NSOutlineView*)ov numberOfChildrenOfItem:(id)item {
    return item ? (NSInteger)((DTSideItem*)item).children.count : (NSInteger)_roots.count;
}

- (id)outlineView:(NSOutlineView*)ov child:(NSInteger)index ofItem:(id)item {
    return item ? ((DTSideItem*)item).children[(NSUInteger)index] : _roots[(NSUInteger)index];
}

- (BOOL)outlineView:(NSOutlineView*)ov isItemExpandable:(id)item {
    return ((DTSideItem*)item).children.count > 0;
}

- (BOOL)outlineView:(NSOutlineView*)ov isGroupItem:(id)item {
    return ((DTSideItem*)item).kind == DTSideGroup;
}

- (BOOL)outlineView:(NSOutlineView*)ov shouldSelectItem:(id)item {
    DTSideItem* it = item;
    RepoSession* s = _owner.session;
    switch (it.kind) {
    case DTSidePage: return s && s->data && (it.index == 1 || (s->graph && s->graph->hasWip));
    case DTSideBranch:
    case DTSideTag:
    case DTSideStash: return [self commitOf:it] >= 0;
    default: return NO;
    }
}

- (NSView*)outlineView:(NSOutlineView*)ov viewForTableColumn:(NSTableColumn*)col item:(id)item {
    DTSideItem* it = item;
    if (it.kind == DTSideGroup) {
        NSTableCellView* v = [ov makeViewWithIdentifier:@"group" owner:self];
        if (!v) {
            v = [[NSTableCellView alloc] init];
            v.identifier = @"group";
            NSTextField* t = MakeLabel(@"", [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold], NSColor.secondaryLabelColor);
            t.translatesAutoresizingMaskIntoConstraints = NO;
            [v addSubview:t];
            v.textField = t;
            [NSLayoutConstraint activateConstraints:@[
                [t.leadingAnchor constraintEqualToAnchor:v.leadingAnchor constant:2],
                [t.trailingAnchor constraintLessThanOrEqualToAnchor:v.trailingAnchor],
                [t.centerYAnchor constraintEqualToAnchor:v.centerYAnchor],
            ]];
        }
        v.textField.stringValue = it.title;
        return v;
    }
    DTSideCell* v = [ov makeViewWithIdentifier:@"item" owner:self];
    if (!v) {
        v = [[DTSideCell alloc] initWithFrame:NSZeroRect];
        v.identifier = @"item";
    }
    [self configure:v item:it];
    return v;
}

- (void)configure:(DTSideCell*)v item:(DTSideItem*)it {
    RepoSession* s = _owner.session;
    const RepoData* d = s ? s->data.get() : nullptr;
    NSString* symbol = @"circle";
    NSString* note = @"";
    NSFont* font = [NSFont systemFontOfSize:NSFont.systemFontSize];
    NSColor* color = NSColor.labelColor;
    switch (it.kind) {
    case DTSidePage:
        symbol = it.index == 0 ? @"square.and.pencil" : @"clock.arrow.circlepath";
        if (it.index == 0 && s && s->status && s->status->Dirty()) note = [NSString stringWithFormat:@"%d", s->status->ChangedFiles()];
        if (it.index == 0 && !(s && s->graph && s->graph->hasWip)) color = NSColor.secondaryLabelColor;
        break;
    case DTSideFolder:
        symbol = @"folder";
        note = [NSString stringWithFormat:@"%ld", (long)it.count];
        break;
    case DTSideRemote: symbol = @"cloud"; break;
    case DTSideBranch:
        if (d && it.index >= 0 && it.index < (NSInteger)d->refs.size()) {
            const Ref& r = d->refs[(size_t)it.index];
            symbol = r.head ? @"checkmark.circle.fill" : @"arrow.triangle.branch";
            note = AheadBehind(r);
            if (r.head) font = [NSFont systemFontOfSize:NSFont.systemFontSize weight:NSFontWeightSemibold];
            if (r.commit < 0) color = NSColor.secondaryLabelColor;
        }
        break;
    case DTSideTag: symbol = @"tag"; break;
    case DTSideStash:
        symbol = @"tray";
        if (d && it.index >= 0 && it.index < (NSInteger)d->stashes.size()) note = NS(d->stashes[(size_t)it.index].name);
        break;
    case DTSideSubmodule: symbol = @"shippingbox"; break;
    case DTSideGroup: break;
    }
    v.imageView.image = Symbol(symbol, 13, NSFontWeightRegular);
    v.textField.stringValue = it.title;
    v.textField.font = font;
    v.textField.textColor = color;
    v.textField.toolTip = it.kind == DTSideStash || it.kind == DTSideBranch || it.kind == DTSideTag ? it.title : nil;
    v.note.stringValue = note;
}

- (void)outlineViewSelectionDidChange:(NSNotification*)note {
    if (_syncing) return;
    DTSideItem* it = [_outline itemAtRow:_outline.selectedRow];
    if (!it) return;
    if (it.kind == DTSidePage) {
        if (it.index == 0) [_owner selectWorkingTree];
        else [_owner showHistoryPage:nil];
        return;
    }
    const int32_t c = [self commitOf:it];
    if (c >= 0) [_owner selectCommit:c];
}

- (void)outlineViewItemDidExpand:(NSNotification*)note {
    if (_syncing) return;
    DTSideItem* it = note.userInfo[@"NSObject"];
    if (it.kind == DTSideGroup) [_collapsedGroups removeObject:it.key];
    else [_expandedFolders addObject:it.key];
}

- (void)outlineViewItemDidCollapse:(NSNotification*)note {
    if (_syncing) return;
    DTSideItem* it = note.userInfo[@"NSObject"];
    if (it.kind == DTSideGroup) [_collapsedGroups addObject:it.key];
    else [_expandedFolders removeObject:it.key];
}

// Double-click a branch or tag: check it out; a folder: open or close it.
- (void)doubleClicked:(id)sender {
    DTSideItem* it = [_outline itemAtRow:_outline.clickedRow];
    if (it.kind == DTSideBranch || it.kind == DTSideTag) [_owner checkoutRef:(int)it.index];
    else if (it.kind == DTSideFolder || it.kind == DTSideRemote) {
        if ([_outline isItemExpanded:it]) [_outline.animator collapseItem:it];
        else [_outline.animator expandItem:it];
    }
}

- (void)menuNeedsUpdate:(NSMenu*)menu {
    [menu removeAllItems];
    DTSideItem* it = [_outline itemAtRow:_outline.clickedRow];
    if (it.kind == DTSideBranch || it.kind == DTSideTag) [_owner fillRefMenu:menu ref:(int)it.index];
    else if (it.kind == DTSideStash) [_owner fillStashMenu:menu stash:(int)it.index];
}

@end
