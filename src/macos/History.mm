// The history: a table with the branch and tag chips, the graph, two-line commit rows and the lines each
// commit changed. Cells are drawn from the session's data when they are on screen; nothing is copied.

#import "MacUI.h"

namespace {

enum Column : int { kRefs, kGraph, kCommit, kChanges };

constexpr CGFloat kRowH = 44;
constexpr CGFloat kLaneW = 14;   // distance between graph lanes
constexpr CGFloat kLaneX0 = 13;  // the first lane, from the column's left edge
constexpr CGFloat kNodeR = 4.5;

}  // namespace

@interface MGHistoryCell : NSTableCellView
@property(nonatomic) Column column;
@property(nonatomic) NSInteger row;
@property(nonatomic, weak) MGHistoryController* owner;
@end

@interface MGHistoryController ()
- (void)drawCell:(MGHistoryCell*)cell;
@end

@implementation MGHistoryCell

- (BOOL)isFlipped {
    return YES;
}

- (void)drawRect:(NSRect)dirty {
    [self.owner drawCell:self];
}

@end

// A selection that leaves the text and the graph colors as they are: a tinted rounded rectangle.
@interface MGHistoryRowView : NSTableRowView
@end

@implementation MGHistoryRowView

- (void)drawSelectionInRect:(NSRect)dirty {
    [[NSColor.controlAccentColor colorWithAlphaComponent:self.emphasized ? 0.22 : 0.12] setFill];
    [[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds, 6, 1) xRadius:6 yRadius:6] fill];
}

- (NSBackgroundStyle)interiorBackgroundStyle {
    return NSBackgroundStyleNormal;
}

@end

// ---------------------------------------------------------------- drawing

static void DrawGraph(const Graph& g, const RepoData& d, const GraphRow& row, NSRect b) {
    auto X = [&](int lane) { return round(b.origin.x + kLaneX0 + lane * kLaneW); };
    const CGFloat y0 = NSMinY(b), y1 = NSMaxY(b), cy = round(NSMidY(b));
    const CGFloat nx = X(row.lane);
    const CGFloat gap = kNodeR + 1;  // lines stop at the node, so hollow nodes stay hollow
    for (uint32_t k = 0; k < row.segCount; ++k) {
        const Seg& s = g.segs[row.segOff + k];
        const CGFloat lx = X(s.lane);
        NSBezierPath* p = [NSBezierPath bezierPath];
        p.lineWidth = 1.6;
        if (s.dashed) {  // uncommitted changes and stashes
            const CGFloat dash[] = { 3, 3 };
            [p setLineDash:dash count:2 phase:0];
        }
        if (s.type == SEG_PASS) {
            [p moveToPoint:NSMakePoint(lx, y0)];
            [p lineToPoint:NSMakePoint(lx, y1)];
        } else if (s.lane == row.lane) {
            if (s.type == SEG_IN) {
                [p moveToPoint:NSMakePoint(lx, y0)];
                [p lineToPoint:NSMakePoint(lx, cy - gap)];
            } else {
                [p moveToPoint:NSMakePoint(nx, cy + gap)];
                [p lineToPoint:NSMakePoint(nx, y1)];
            }
        } else if (s.type == SEG_IN) {  // from a lane above into the node
            const CGFloat h = cy - y0;
            [p moveToPoint:NSMakePoint(lx, y0)];
            [p curveToPoint:NSMakePoint(nx, cy - gap) controlPoint1:NSMakePoint(lx, y0 + h * 0.57) controlPoint2:NSMakePoint(nx, cy - h * 0.57)];
        } else {  // from the node down into another lane
            const CGFloat h = y1 - cy;
            [p moveToPoint:NSMakePoint(nx, cy + gap)];
            [p curveToPoint:NSMakePoint(lx, y1) controlPoint1:NSMakePoint(nx, cy + h * 0.57) controlPoint2:NSMakePoint(lx, y1 - h * 0.57)];
        }
        [(s.dashed ? NSColor.tertiaryLabelColor : LaneColor(s.lane)) setStroke];
        [p stroke];
    }
    // nodes: dots for commits, rings for merges, a dashed ring for the uncommitted changes, a square for stashes
    if (row.commit < 0) {
        NSBezierPath* c = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(nx - 5, cy - 5, 10, 10)];
        c.lineWidth = 1.5;
        const CGFloat dash[] = { 2.4, 2 };
        [c setLineDash:dash count:2 phase:0];
        [NSColor.secondaryLabelColor setStroke];
        [c stroke];
        return;
    }
    const Commit& cm = d.commits[(size_t)row.commit];
    NSColor* lc = LaneColor(row.lane);
    if (cm.flags & CF_STASH) {
        NSBezierPath* q = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(nx - 4.5, cy - 4.5, 9, 9) xRadius:2 yRadius:2];
        q.lineWidth = 1.5;
        [NSColor.secondaryLabelColor setStroke];
        [q stroke];
    } else if (cm.flags & CF_MERGE) {
        NSBezierPath* c = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(nx - 4, cy - 4, 8, 8)];
        c.lineWidth = 1.8;
        [lc setStroke];
        [c stroke];
    } else {
        [lc setFill];
        [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(nx - kNodeR, cy - kNodeR, 2 * kNodeR, 2 * kNodeR)] fill];
    }
}

// The first branch or tag of a commit as a chip, with local / remote / tag icons and "+N" for the others.
static void DrawChip(const RepoData& d, const GraphRow& row, NSRect b) {
    if (row.commit < 0) return;
    auto it = d.labels.find(row.commit);
    if (it == d.labels.end() || it->second.empty()) return;
    const Label& L = it->second[0];
    const int more = (int)it->second.size() - 1;
    static NSFont* font = [NSFont systemFontOfSize:11];
    static NSFont* bold = [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold];
    NSString* text = NS(LabelText(d, L));
    NSString* moreText = more > 0 ? [NSString stringWithFormat:@"+%d", more] : nil;
    const int icons = (L.local ? 1 : 0) + (L.remote ? 1 : 0) + (L.tag ? 1 : 0);
    const CGFloat padX = 7, gap = 4, iconW = 12;
    const CGFloat textW = TextWidth(text, font), moreW = moreText ? TextWidth(moreText, bold) : 0;
    const CGFloat w = std::min(padX + icons * (iconW + gap) + textW + (more > 0 ? gap + moreW : 0) + padX, b.size.width - 16);
    if (w < 34) return;
    const NSRect chip = NSMakeRect(NSMaxX(b) - 8 - w, round(NSMidY(b) - 10), w, 20);
    NSColor* lc = L.tag ? NSColor.secondaryLabelColor : LaneColor(row.lane);
    NSBezierPath* p = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(chip, 0.5, 0.5) xRadius:9.5 yRadius:9.5];
    if (L.head) {  // checked out
        [[lc colorWithAlphaComponent:0.18] setFill];
        [p fill];
    }
    p.lineWidth = 1;
    [[lc colorWithAlphaComponent:L.tag ? 0.45 : 0.7] setStroke];
    [p stroke];
    CGFloat x = chip.origin.x + padX;
    auto icon = [&](NSString* name) {
        DrawSymbol(name, NSMakeRect(x, chip.origin.y, iconW, chip.size.height), 9.5, lc);
        x += iconW + gap;
    };
    if (L.local) icon(@"laptopcomputer");
    if (L.remote) icon(@"cloud");
    if (L.tag) icon(@"tag");
    const CGFloat textR = NSMaxX(chip) - padX - (more > 0 ? moreW + gap : 0);
    DrawText(text, NSMakeRect(x, chip.origin.y, std::max<CGFloat>(0, textR - x) + 1, chip.size.height), font, NSColor.labelColor);
    if (more > 0) DrawText(moreText, NSMakeRect(textR + gap, chip.origin.y, moreW + 2, chip.size.height), bold, NSColor.secondaryLabelColor);
}

// ---------------------------------------------------------------- controller

@implementation MGHistoryController {
    __weak MGRepoWindowController* _owner;
    NSTableView* _table;
    NSScrollView* _scroll;
    NSSegmentedControl* _scope;
    NSTextField* _matches;
    NSButton* _prev;
    NSButton* _next;
    BOOL _syncing;
    std::vector<Oid> _wantStats;
    BOOL _statsQueued;
}

- (instancetype)initWithOwner:(MGRepoWindowController*)owner {
    if ((self = [super initWithNibName:nil bundle:nil])) _owner = owner;
    return self;
}

- (void)loadView {
    NSView* v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 720, 520)];

    // scope and search results
    _scope = [NSSegmentedControl segmentedControlWithLabels:@[ @"All Branches", @"Current Branch" ]
                                               trackingMode:NSSegmentSwitchTrackingSelectOne
                                                     target:self
                                                     action:@selector(scopeChanged:)];
    _scope.controlSize = NSControlSizeSmall;
    _scope.font = [NSFont systemFontOfSize:11];
    _scope.selectedSegment = 0;
    _matches = MakeLabel(@"", [NSFont systemFontOfSize:11], NSColor.secondaryLabelColor);
    _prev = [NSButton buttonWithImage:Symbol(@"chevron.up", 11, NSFontWeightSemibold) target:nil action:@selector(findPreviousMatch:)];
    _next = [NSButton buttonWithImage:Symbol(@"chevron.down", 11, NSFontWeightSemibold) target:nil action:@selector(findNextMatch:)];
    _prev.toolTip = @"Previous match (⇧⌘G)";
    _next.toolTip = @"Next match (⌘G)";
    NSView* bar = [[NSView alloc] init];
    for (NSView* x in @[ _scope, _matches, _prev, _next ]) {
        x.translatesAutoresizingMaskIntoConstraints = NO;
        [bar addSubview:x];
    }
    for (NSButton* b in @[ _prev, _next ]) {
        b.bordered = NO;
        b.hidden = YES;
    }
    NSBox* line = [[NSBox alloc] init];
    line.boxType = NSBoxSeparator;

    // the table
    _table = [[NSTableView alloc] init];
    _table.style = NSTableViewStyleFullWidth;
    _table.rowHeight = kRowH;
    _table.intercellSpacing = NSMakeSize(0, 0);
    _table.allowsMultipleSelection = NO;
    _table.allowsEmptySelection = YES;
    _table.allowsColumnReordering = NO;
    _table.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle;  // only the commit column takes part
    _table.gridStyleMask = NSTableViewGridNone;
    auto column = [&](NSString* ident, NSString* title, CGFloat width, CGFloat minWidth, bool stretch) {
        NSTableColumn* c = [[NSTableColumn alloc] initWithIdentifier:ident];
        c.title = title;
        c.width = width;
        c.minWidth = minWidth;
        c.resizingMask = stretch ? NSTableColumnAutoresizingMask | NSTableColumnUserResizingMask : NSTableColumnUserResizingMask;
        [self->_table addTableColumn:c];
        return c;
    };
    NSTableColumn* refs = column(@"refs", @"Branch / Tag", std::clamp<CGFloat>(AppSettings().GetFloat(L"colRef", 150), 40, 480), 40, false);
    refs.headerCell.alignment = NSTextAlignmentRight;
    column(@"graph", @"", 80, 26, false);
    column(@"commit", @"Commit", 420, 110, true);
    column(@"changes", @"Changes", 92, 50, false).headerCell.alignment = NSTextAlignmentRight;
    _table.dataSource = self;
    _table.delegate = self;
    _table.target = self;
    _table.doubleAction = @selector(doubleClicked:);
    _table.menu = [[NSMenu alloc] initWithTitle:@"Commit"];
    _table.menu.delegate = self;
    _scroll = [[NSScrollView alloc] init];
    _scroll.documentView = _table;
    _scroll.hasVerticalScroller = YES;
    _scroll.hasHorizontalScroller = NO;
    _scroll.autohidesScrollers = YES;

    for (NSView* x in @[ bar, line, _scroll ]) {
        x.translatesAutoresizingMaskIntoConstraints = NO;
        [v addSubview:x];
    }
    [NSLayoutConstraint activateConstraints:@[
        [bar.topAnchor constraintEqualToAnchor:v.safeAreaLayoutGuide.topAnchor],
        [bar.leadingAnchor constraintEqualToAnchor:v.leadingAnchor],
        [bar.trailingAnchor constraintEqualToAnchor:v.trailingAnchor],
        [bar.heightAnchor constraintEqualToConstant:34],
        [_scope.leadingAnchor constraintEqualToAnchor:bar.leadingAnchor constant:12],
        [_scope.centerYAnchor constraintEqualToAnchor:bar.centerYAnchor],
        [_next.trailingAnchor constraintEqualToAnchor:bar.trailingAnchor constant:-10],
        [_next.centerYAnchor constraintEqualToAnchor:bar.centerYAnchor],
        [_prev.trailingAnchor constraintEqualToAnchor:_next.leadingAnchor constant:-4],
        [_prev.centerYAnchor constraintEqualToAnchor:bar.centerYAnchor],
        [_matches.trailingAnchor constraintEqualToAnchor:_prev.leadingAnchor constant:-8],
        [_matches.centerYAnchor constraintEqualToAnchor:bar.centerYAnchor],
        [_matches.leadingAnchor constraintGreaterThanOrEqualToAnchor:_scope.trailingAnchor constant:12],
        [line.topAnchor constraintEqualToAnchor:bar.bottomAnchor],
        [line.leadingAnchor constraintEqualToAnchor:v.leadingAnchor],
        [line.trailingAnchor constraintEqualToAnchor:v.trailingAnchor],
        [line.heightAnchor constraintEqualToConstant:1],
        [_scroll.topAnchor constraintEqualToAnchor:line.bottomAnchor],
        [_scroll.leadingAnchor constraintEqualToAnchor:v.leadingAnchor],
        [_scroll.trailingAnchor constraintEqualToAnchor:v.trailingAnchor],
        [_scroll.bottomAnchor constraintEqualToAnchor:v.bottomAnchor],
    ]];
    self.view = v;
}

// ---- data

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tv {
    RepoSession* s = _owner.session;
    return s && s->graph && s->data ? (NSInteger)s->graph->rows.size() : 0;
}

- (NSView*)tableView:(NSTableView*)tv viewForTableColumn:(NSTableColumn*)col row:(NSInteger)row {
    NSString* ident = col.identifier;
    MGHistoryCell* cell = [tv makeViewWithIdentifier:ident owner:self];
    if (!cell) {
        cell = [[MGHistoryCell alloc] initWithFrame:NSMakeRect(0, 0, col.width, kRowH)];
        cell.identifier = ident;
        cell.owner = self;
        cell.column = [ident isEqualToString:@"refs"] ? kRefs : [ident isEqualToString:@"graph"] ? kGraph : [ident isEqualToString:@"commit"] ? kCommit : kChanges;
    }
    cell.row = row;
    cell.toolTip = cell.column == kRefs ? [self refsTipForRow:row] : nil;
    cell.needsDisplay = YES;
    return cell;
}

- (NSTableRowView*)tableView:(NSTableView*)tv rowViewForRow:(NSInteger)row {
    MGHistoryRowView* v = [tv makeViewWithIdentifier:@"row" owner:self];
    if (!v) {
        v = [[MGHistoryRowView alloc] init];
        v.identifier = @"row";
    }
    return v;
}

// Every branch and tag of the commit, and how each relates to its remote.
- (NSString*)refsTipForRow:(NSInteger)row {
    RepoSession* s = _owner.session;
    if (!s || !s->graph || !s->data || row < 0 || row >= (NSInteger)s->graph->rows.size()) return nil;
    const RepoData& d = *s->data;
    auto it = d.labels.find(s->graph->rows[(size_t)row].commit);
    if (it == d.labels.end() || it->second.empty()) return nil;
    NSMutableArray<NSString*>* lines = [NSMutableArray array];
    for (const Label& L : it->second) [lines addObject:[NSString stringWithFormat:@"%@  —  %@", NS(L.text), NS(RefNote(d, L))]];
    return [lines componentsJoinedByString:@"\n"];
}

- (void)drawCell:(MGHistoryCell*)cell {
    RepoSession* s = _owner.session;
    if (!s || !s->graph || !s->data || cell.row < 0 || cell.row >= (NSInteger)s->graph->rows.size()) return;
    const Graph& g = *s->graph;
    const RepoData& d = *s->data;
    const GraphRow& gr = g.rows[(size_t)cell.row];
    const NSRect b = cell.bounds;
    // search: the commits that don't match fade
    const bool dim = s->Searching() && (gr.commit < 0 || !s->searchHits[(size_t)gr.commit]);
    CGContextRef cg = NSGraphicsContext.currentContext.CGContext;
    CGContextSaveGState(cg);
    if (dim) CGContextSetAlpha(cg, 0.35);
    switch (cell.column) {
    case kGraph: DrawGraph(g, d, gr, b); break;
    case kRefs: DrawChip(d, gr, b); break;
    case kCommit: [self drawCommit:gr session:s rect:b]; break;
    case kChanges: [self drawChanges:gr session:s rect:b]; break;
    }
    CGContextRestoreGState(cg);
}

- (void)drawCommit:(const GraphRow&)gr session:(RepoSession*)s rect:(NSRect)b {
    static NSFont* titleFont = [NSFont systemFontOfSize:13];
    static NSFont* wipFont = [NSFont systemFontOfSize:13 weight:NSFontWeightMedium];
    static NSFont* small = [NSFont systemFontOfSize:11];
    static NSFont* mono = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    const RepoData& d = *s->data;
    const CGFloat x = b.origin.x + 6, w = b.size.width - 12;
    const NSRect title = NSMakeRect(x, b.origin.y + 5, w, 19), meta = NSMakeRect(x, b.origin.y + 24, w, 15);
    if (gr.commit < 0) {
        DrawText(@"Uncommitted Changes", title, wipFont, NSColor.labelColor);
        DrawText(NS(WipSummary(d, s->status.get())), meta, small, NSColor.secondaryLabelColor);
        return;
    }
    const Commit& cm = d.commits[(size_t)gr.commit];
    DrawText(NS(CommitSubject(d, cm)), title, titleFont, NSColor.labelColor);
    NSString* m = NS(CommitMeta(d, cm));  // "Ada · 5 minutes ago · ", then the short id when it fits
    const CGFloat mw = TextWidth(m, small);
    if (mw + 30 < w) {
        DrawText(m, meta, small, NSColor.secondaryLabelColor);
        DrawText(NS(cm.id.Hex(7)), NSMakeRect(x + mw, meta.origin.y, w - mw, meta.size.height), mono, NSColor.secondaryLabelColor);
    } else {
        static NSCharacterSet* tail = [NSCharacterSet characterSetWithCharactersInString:@" ·"];
        DrawText([m stringByTrimmingCharactersInSet:tail], meta, small, NSColor.secondaryLabelColor);
    }
}

// Lines added and removed; loaded in the background for the rows on screen (merges and stashes have none).
- (void)drawChanges:(const GraphRow&)gr session:(RepoSession*)s rect:(NSRect)b {
    if (gr.commit < 0) return;
    const Commit& cm = s->data->commits[(size_t)gr.commit];
    if (cm.flags & (CF_MERGE | CF_STASH)) return;
    auto st = s->lineStats.find(cm.id);
    if (st == s->lineStats.end()) {
        [self wantStats:cm.id];
        return;
    }
    if (st->second.added < 0) return;  // on its way
    static NSFont* mono = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    NSString* plus = [NSString stringWithFormat:@"+%d", st->second.added];
    NSString* minus = [NSString stringWithFormat:@"−%d", st->second.deleted];
    const CGFloat pw = TextWidth(plus, mono), mw = TextWidth(minus, mono);
    const CGFloat right = NSMaxX(b) - 12;
    DrawText(minus, NSMakeRect(right - mw, b.origin.y, mw + 2, b.size.height), mono, DeletedColor());
    DrawText(plus, NSMakeRect(right - mw - 7 - pw, b.origin.y, pw + 2, b.size.height), mono, AddedColor());
}

- (void)wantStats:(const Oid&)oid {
    if (_wantStats.size() >= 64 || std::find(_wantStats.begin(), _wantStats.end(), oid) != _wantStats.end()) return;
    _wantStats.push_back(oid);
    if (_statsQueued) return;
    _statsQueued = YES;
    dispatch_async(dispatch_get_main_queue(), ^{
        [self requestStats];
    });
}

// One request for the rows drawn since the last one, plus the rows just below, so they are there when scrolling.
- (void)requestStats {
    _statsQueued = NO;
    RepoSession* s = _owner.session;
    if (!s || !s->graph || !s->data) {
        _wantStats.clear();
        return;
    }
    const NSRange visible = [_table rowsInRect:_table.visibleRect];
    for (NSUInteger r = NSMaxRange(visible); r < NSMaxRange(visible) + 24 && r < s->graph->rows.size() && _wantStats.size() < 64; ++r) {
        const int32_t c = s->graph->rows[r].commit;
        if (c < 0) continue;
        const Commit& cm = s->data->commits[(size_t)c];
        if (!(cm.flags & (CF_MERGE | CF_STASH)) && !s->lineStats.count(cm.id) &&
            std::find(_wantStats.begin(), _wantStats.end(), cm.id) == _wantStats.end())
            _wantStats.push_back(cm.id);
    }
    if (!_wantStats.empty() && !s->statsInflight) s->RequestLineStats(std::move(_wantStats));
    _wantStats.clear();  // asked again when the rows are drawn after the answer
}

// ---- updates

- (void)reload {
    RepoSession* s = _owner.session;
    if (s && s->graph && !s->lastLoadRefresh) {  // a new repository or scope: a graph column as wide as its lanes
        NSTableColumn* c = [_table tableColumnWithIdentifier:@"graph"];
        c.width = std::clamp<CGFloat>(kLaneX0 * 2 + (s->graph->maxLanes - 1) * kLaneW, 40, 260);
        [self fitColumns];
    }
    _syncing = YES;
    [_table reloadData];
    _syncing = NO;
    _scope.selectedSegment = s && s->headOnly ? 1 : 0;
    [self syncSelection];
}

- (void)syncSelection {
    RepoSession* s = _owner.session;
    const NSInteger row = s ? s->SelectedRow() : -1;
    _syncing = YES;
    if (row >= 0 && row < _table.numberOfRows) {
        if (_table.selectedRow != row) [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row] byExtendingSelection:NO];
    } else {
        [_table deselectAll:nil];
    }
    _syncing = NO;
    [self updateMatches];
}

- (void)revealSelection {
    RepoSession* s = _owner.session;
    const NSInteger row = s ? s->SelectedRow() : -1;
    if (row < 0 || row >= _table.numberOfRows) return;
    const NSRect visible = _scroll.contentView.documentVisibleRect, r = [_table rectOfRow:row];
    if (NSContainsRect(visible, r)) return;
    // a jump: the row in the middle of the view (the rectangle around it is as tall as the view)
    [_table scrollRectToVisible:NSInsetRect(r, 0, -std::max<CGFloat>(0, (NSHeight(visible) - NSHeight(r)) / 2))];
}

- (void)redrawRows {
    [_table enumerateAvailableRowViewsUsingBlock:^(NSTableRowView* rv, NSInteger row) {
        for (NSInteger c = 0; c < rv.numberOfColumns; ++c) [[rv viewAtColumn:c] setNeedsDisplay:YES];
    }];
    [self updateMatches];
}

- (void)updateMatches {
    RepoSession* s = _owner.session;
    _matches.stringValue = s ? NS(SearchStatus(*s)) : @"";
    _matches.textColor = s && s->Searching() && s->searchCount == 0 ? NSColor.systemRedColor : NSColor.secondaryLabelColor;
    const BOOL any = s && s->Searching() && s->searchCount > 0;
    _prev.hidden = !any;
    _next.hidden = !any;
}

- (void)reloadStats {
    const NSInteger col = [_table columnWithIdentifier:@"changes"];
    if (col < 0) return;
    [_table enumerateAvailableRowViewsUsingBlock:^(NSTableRowView* rv, NSInteger row) {
        if (col < rv.numberOfColumns) [[rv viewAtColumn:col] setNeedsDisplay:YES];
    }];
}

- (void)statusChanged:(int)delta {
    if (delta != 0) {
        // The "uncommitted changes" row came or went: the rows below keep their place on screen.
        NSClipView* clip = _scroll.contentView;
        NSPoint o = clip.bounds.origin;
        _syncing = YES;
        [_table reloadData];
        _syncing = NO;
        if (o.y > 0) {
            o.y = std::max<CGFloat>(0, o.y + delta * kRowH);
            [clip scrollToPoint:o];
            [_scroll reflectScrolledClipView:clip];
        }
        [self syncSelection];
        return;
    }
    RepoSession* s = _owner.session;
    if (s && s->graph && s->graph->hasWip && _table.numberOfRows > 0)  // its counts
        [_table reloadDataForRowIndexes:[NSIndexSet indexSetWithIndex:0]
                          columnIndexes:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, (NSUInteger)_table.numberOfColumns)]];
}

// The columns as wide as the view (the table has no horizontal scroller): the commit column takes or gives
// the difference, down to its minimum.
- (void)fitColumns {
    const CGFloat avail = NSWidth(_scroll.contentView.bounds) - 8;  // the inspector's rounded edge covers a few points
    NSTableColumn* commit = [_table tableColumnWithIdentifier:@"commit"];
    if (avail <= 0 || !commit) return;
    CGFloat total = 0;
    for (NSTableColumn* c in _table.tableColumns) total += c.width;
    const CGFloat want = std::max(commit.minWidth, commit.width + avail - total);
    if (fabs(want - commit.width) > 0.5) commit.width = want;
}

- (void)viewDidLayout {
    [super viewDidLayout];
    [self fitColumns];
}

- (void)focusTable {
    [self.view.window makeFirstResponder:_table];
}

// ---- interaction

- (void)tableViewSelectionDidChange:(NSNotification*)note {
    if (_syncing) return;
    const NSInteger row = _table.selectedRow;
    if (row >= 0) [_owner selectRow:row];
    else [self syncSelection];  // a click on empty space keeps the selection
}

- (void)tableViewColumnDidResize:(NSNotification*)note {
    NSTableColumn* c = note.userInfo[@"NSTableColumn"];
    if ([c.identifier isEqualToString:@"refs"]) AppSettings().SetFloat(L"colRef", (float)c.width, 0);
}

// Double-click a chip: check its branch out. Double-click the uncommitted changes: review them.
- (void)doubleClicked:(id)sender {
    RepoSession* s = _owner.session;
    const NSInteger row = _table.clickedRow;
    if (!s || !s->graph || !s->data || row < 0 || row >= (NSInteger)s->graph->rows.size()) return;
    const int32_t c = s->graph->rows[(size_t)row].commit;
    if (c < 0) {
        [_owner reviewChanges];
        return;
    }
    if (_table.clickedColumn != [_table columnWithIdentifier:@"refs"]) return;
    auto it = s->data->labels.find(c);
    if (it != s->data->labels.end() && !it->second.empty() && !it->second[0].tag) [_owner checkoutRef:it->second[0].ref];
}

- (void)scopeChanged:(NSSegmentedControl*)sc {
    [_owner setHeadOnly:sc.selectedSegment == 1];
}

- (void)menuNeedsUpdate:(NSMenu*)menu {
    RepoSession* s = _owner.session;
    const NSInteger row = _table.clickedRow;
    if (!s || !s->graph || row < 0 || row >= (NSInteger)s->graph->rows.size()) {
        [menu removeAllItems];
        return;
    }
    [_owner fillCommitMenu:menu commit:s->graph->rows[(size_t)row].commit];
}

@end
