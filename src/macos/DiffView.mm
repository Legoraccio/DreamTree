// The diff of one file, shown in place of the history. Rows have a fixed height and only the visible ones
// are drawn; the view scrolls vertically in its scroll view and horizontally by itself, so the line numbers
// stay in place. Changes of the working tree or of the index can be staged, unstaged or discarded by hunk
// or by selected lines (click, Shift+click or drag, Command+click).

#import "MacUI.h"

namespace {

constexpr CGFloat kRowH = 18;

int Digits(int n) {
    int d = 1;
    while (n >= 10) {
        n /= 10;
        ++d;
    }
    return d;
}

bool IsBanner(const DiffLine& L) { return L.kind == DiffLine::Hunk || L.kind == DiffLine::Info; }

// Where things are, for the current width and layout.
struct Geo {
    bool split = false;
    CGFloat width = 0, numW = 0, markW = 16, half = 0, textW = 0;
    CGFloat textX[2] = { 0, 0 };  // unified: [0]
    NSInteger rows = 0;
};

}  // namespace

@interface DTDiffController ()
@property(nonatomic, readonly) RepoSession* session;
- (void)hunkAction:(int)hunk discard:(BOOL)discard;
- (void)escape;
@end

@interface DTDiffDocView : NSView
@property(nonatomic, weak) DTDiffController* controller;
- (void)diffChanged:(BOOL)anotherFile;
- (void)jump:(int)dir;
@end

@implementation DTDiffDocView {
    NSFont* _code;
    NSFont* _numFont;
    NSFont* _small;
    CGFloat _charW;
    CGFloat _numCharW;
    CGFloat _lineH;
    CGFloat _hscroll;
    BOOL _split;
    BOOL _dragging;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    _code = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    _numFont = [NSFont monospacedDigitSystemFontOfSize:10.5 weight:NSFontWeightRegular];
    _small = [NSFont systemFontOfSize:10.5 weight:NSFontWeightMedium];
    _charW = [@"0" sizeWithAttributes:@{ NSFontAttributeName : _code }].width;
    _numCharW = [@"0" sizeWithAttributes:@{ NSFontAttributeName : _numFont }].width;
    _lineH = ceil([[[NSLayoutManager alloc] init] defaultLineHeightForFont:_code]);
    return self;
}

- (BOOL)isFlipped {
    return YES;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (std::shared_ptr<FileDiff>)diff {
    RepoSession* s = _controller.session;
    return s && s->diff.open && s->diff.diff && s->diff.diff->ok ? s->diff.diff : nullptr;
}

- (Geo)geo:(const FileDiff&)d {
    Geo g;
    g.width = NSWidth(self.bounds);
    g.split = ShowSplit(AppSettings().diffSplit, d);
    g.numW = round(Digits(std::max(999, d.maxLineNo)) * _numCharW + 14);
    if (!g.split) {
        g.textX[0] = 2 * g.numW + g.markW;
        g.textW = g.width - g.textX[0] - 8;
        g.rows = (NSInteger)d.lines.size();
    } else {
        g.half = floor(g.width / 2);
        g.textX[0] = g.numW + g.markW;
        g.textX[1] = g.half + 1 + g.numW + g.markW;
        g.textW = g.half - g.numW - g.markW - 6;
        g.rows = (NSInteger)d.split.size();
    }
    return g;
}

- (void)clampScroll:(const FileDiff&)d geo:(const Geo&)g {
    _hscroll = std::clamp<CGFloat>(_hscroll, 0, std::max<CGFloat>(0, (CGFloat)d.maxLen * _charW + 60 - g.textW));
}

// The diff, its layout or the width changed. Another file starts from the top; a new layout keeps the same
// lines at the top.
- (void)diffChanged:(BOOL)anotherFile {
    const std::shared_ptr<FileDiff> d = [self diff];
    if (!d) return;
    const Geo g = [self geo:*d];
    NSInteger top = (NSInteger)floor(self.visibleRect.origin.y / kRowH);
    const BOOL relayout = !anotherFile && g.split != (bool)_split;
    if (relayout && !d->lines.empty() && !d->split.empty()) {
        if (g.split) {
            top = SplitRowOfLine(*d, (int)std::min<NSInteger>(top, (NSInteger)d->lines.size() - 1));
        } else {
            const SplitRow& sr = d->split[(size_t)std::min<NSInteger>(top, (NSInteger)d->split.size() - 1)];
            top = std::max(0, sr.left >= 0 ? sr.left : sr.right);
        }
    }
    _split = g.split;
    [self setFrameSize:NSMakeSize(NSWidth(self.superview.bounds), (CGFloat)(g.rows + 2) * kRowH)];
    if (anotherFile) {
        _hscroll = 0;
        [self scrollPoint:NSZeroPoint];
    } else if (relayout) {
        [self scrollPoint:NSMakePoint(0, (CGFloat)top * kRowH)];
    }
    [self clampScroll:*d geo:g];
    self.needsDisplay = YES;
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    self.needsDisplay = YES;
}

// Next (1) or previous (-1) hunk, one row below the top.
- (void)jump:(int)dir {
    const std::shared_ptr<FileDiff> d = [self diff];
    if (!d || d->hunks.empty()) return;
    const Geo g = [self geo:*d];
    const double top = self.visibleRect.origin.y / kRowH + 1.0;
    int target = -1;
    for (const DiffHunk& h : d->hunks) {
        const int row = g.split ? h.splitRow : h.line;
        if (row < 0) continue;
        if (dir > 0 && row > top + 0.5) {
            target = row;
            break;
        }
        if (dir < 0 && row < top - 0.5) target = row;
    }
    if (target >= 0) [self scrollPoint:NSMakePoint(0, (CGFloat)std::max(0, target - 1) * kRowH)];
}

// ---- drawing

static NSColor* AddBg() {
    static NSColor* c = [NSColor.systemGreenColor colorWithAlphaComponent:0.13];
    return c;
}
static NSColor* DelBg() {
    static NSColor* c = [NSColor.systemRedColor colorWithAlphaComponent:0.12];
    return c;
}
static NSColor* AddHl() {
    static NSColor* c = [NSColor.systemGreenColor colorWithAlphaComponent:0.35];
    return c;
}
static NSColor* DelHl() {
    static NSColor* c = [NSColor.systemRedColor colorWithAlphaComponent:0.33];
    return c;
}
static NSColor* GutterBg() {
    static NSColor* c = [NSColor.labelColor colorWithAlphaComponent:0.035];
    return c;
}
static NSColor* FillerBg() {
    static NSColor* c = [NSColor.labelColor colorWithAlphaComponent:0.05];
    return c;
}

// The hunk buttons of a hunk header row: Stage / Unstage, and Discard for unstaged changes.
- (void)hunkButtons:(const DiffState&)m y:(CGFloat)y width:(CGFloat)w main:(NSRect*)main discard:(NSRect*)discard {
    NSString* label = m.req.section == 1 ? @"Stage Hunk" : @"Unstage Hunk";
    const CGFloat bw = TextWidth(label, _small) + 18;
    *main = NSMakeRect(w - 12 - bw, y + 2, bw, kRowH - 4);
    *discard = m.req.section == 1 ? NSMakeRect(NSMinX(*main) - 6 - 24, y + 2, 24, kRowH - 4) : NSZeroRect;
}

- (void)drawBanner:(const FileDiff&)d line:(int32_t)idx y:(CGFloat)y state:(const DiffState&)m canApply:(bool)canApply {
    const DiffLine& L = d.lines[(size_t)idx];
    const CGFloat W = NSWidth(self.bounds);
    const bool hunk = L.kind == DiffLine::Hunk;
    [(hunk ? [NSColor.systemBlueColor colorWithAlphaComponent:0.09] : FillerBg()) setFill];
    NSRectFillUsingOperation(NSMakeRect(0, y, W, kRowH), NSCompositingOperationSourceOver);
    CGFloat right = W - 12;
    if (hunk && canApply && L.hunk >= 0) {
        NSRect main, discard;
        [self hunkButtons:m y:y width:W main:&main discard:&discard];
        const bool busy = m.HunkBusy();
        NSColor* c = m.req.section == 1 ? NSColor.systemGreenColor : NSColor.systemRedColor;
        NSBezierPath* p = [NSBezierPath bezierPathWithRoundedRect:main xRadius:4 yRadius:4];
        [[c colorWithAlphaComponent:busy ? 0.04 : 0.14] setFill];
        [p fill];
        [[c colorWithAlphaComponent:busy ? 0.25 : 0.7] setStroke];
        [p stroke];
        DrawText(m.req.section == 1 ? @"Stage Hunk" : @"Unstage Hunk", NSInsetRect(main, 9, 0), _small,
                 busy ? NSColor.tertiaryLabelColor : NSColor.labelColor);
        right = NSMinX(main) - 10;
        if (!NSIsEmptyRect(discard)) {
            DrawSymbol(@"trash", discard, 11, busy ? NSColor.tertiaryLabelColor : NSColor.secondaryLabelColor);
            right = NSMinX(discard) - 8;
        }
    }
    NSColor* tc = hunk ? NSColor.systemBlueColor : NSColor.secondaryLabelColor;
    DrawText(NS(std::wstring_view(d.text).substr(L.off, L.len)), NSMakeRect(12, y, std::max<CGFloat>(0, right - 12), kRowH),
             [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular], tc);
}

// The text of a line from x, scrolled, inside [x, x + w].
- (void)drawText:(const FileDiff&)d line:(int32_t)idx x:(CGFloat)x y:(CGFloat)y width:(CGFloat)w {
    const DiffLine& L = d.lines[(size_t)idx];
    const std::wstring_view text = std::wstring_view(d.text).substr(L.off, L.len);
    // tabs become spaces (stops every 4 columns); the changed words move with them
    std::wstring line;
    line.reserve(text.size());
    size_t hs = std::wstring::npos, he = std::wstring::npos;
    for (size_t i = 0; i < text.size(); ++i) {
        if (L.hlLen && i == L.hlStart) hs = line.size();
        if (L.hlLen && i == (size_t)L.hlStart + L.hlLen) he = line.size();
        if (text[i] == L'\t') line.append(4 - line.size() % 4, L' ');
        else if (text[i] != L'\r') line.push_back(text[i]);
    }
    if (hs != std::wstring::npos && he == std::wstring::npos) he = line.size();
    // very long lines: only the part on screen
    size_t from = 0, count = line.size();
    if (line.size() > 600) {
        from = (size_t)std::max<CGFloat>(0, floor(_hscroll / _charW) - 8);
        from = std::min(from, line.size());
        count = std::min(line.size() - from, (size_t)(w / _charW) + 24);
    }
    const CGFloat x0 = x - _hscroll + (CGFloat)from * _charW;
    const CGFloat ty = y + floor((kRowH - _lineH) / 2);
    [NSGraphicsContext saveGraphicsState];
    NSRectClip(NSMakeRect(x, y, w, kRowH));
    if (hs != std::wstring::npos && he > hs && he > from && IsChange(L)) {
        const size_t a = std::max(hs, from), b = std::min(he, from + count);
        if (b > a) {
            const CGFloat hx = x0 + TextWidth(NS(std::wstring_view(line).substr(from, a - from)), _code);
            const CGFloat hw = std::max<CGFloat>(3, TextWidth(NS(std::wstring_view(line).substr(a, b - a)), _code));
            [(L.kind == DiffLine::Added ? AddHl() : DelHl()) setFill];
            [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(hx, y + 2, hw, kRowH - 4) xRadius:2 yRadius:2] fill];
        }
    }
    NSString* s = NS(std::wstring_view(line).substr(from, count));
    NSDictionary* attrs = @{ NSFontAttributeName : _code, NSForegroundColorAttributeName : NSColor.labelColor };
    [s drawAtPoint:NSMakePoint(x0, ty) withAttributes:attrs];
    if (L.noEol) {  // what git prints as "\ No newline at end of file"
        const CGFloat end = x0 + (from == 0 ? TextWidth(s, _code) : (CGFloat)count * _charW);
        NSString* note = @"no newline at end of file";
        const NSRect pill = NSMakeRect(end + 10, y + 3, TextWidth(note, _small) + 12, kRowH - 6);
        [FillerBg() setFill];
        [[NSBezierPath bezierPathWithRoundedRect:pill xRadius:3 yRadius:3] fill];
        DrawText(note, NSInsetRect(pill, 6, 0), _small, NSColor.secondaryLabelColor);
    }
    [NSGraphicsContext restoreGraphicsState];
}

- (void)drawNumber:(int)n x:(CGFloat)x y:(CGFloat)y width:(CGFloat)w {
    if (n < 0) return;
    NSString* s = [NSString stringWithFormat:@"%d", n];
    const CGFloat sw = TextWidth(s, _numFont);
    DrawText(s, NSMakeRect(x + w - 8 - sw, y, sw + 1, kRowH), _numFont, NSColor.tertiaryLabelColor);
}

- (void)drawMarker:(const DiffLine&)L x:(CGFloat)x y:(CGFloat)y width:(CGFloat)w {
    if (L.kind == DiffLine::Added) DrawText(@"+", NSMakeRect(x + 4, y, w - 4, kRowH), _code, NSColor.systemGreenColor);
    else if (L.kind == DiffLine::Deleted) DrawText(@"−", NSMakeRect(x + 4, y, w - 4, kRowH), _code, NSColor.systemRedColor);
}

// Background, selection, line number(s) and marker of one line in a cell of a row.
- (void)drawLineCell:(const FileDiff&)d line:(int32_t)idx cell:(NSRect)cell state:(const DiffState&)m canApply:(bool)canApply
               geo:(const Geo&)g side:(int)side {
    const DiffLine& L = d.lines[(size_t)idx];
    if (L.kind == DiffLine::Added) [AddBg() setFill];
    else if (L.kind == DiffLine::Deleted) [DelBg() setFill];
    if (L.kind == DiffLine::Added || L.kind == DiffLine::Deleted) NSRectFillUsingOperation(cell, NSCompositingOperationSourceOver);
    const CGFloat px = cell.origin.x, y = cell.origin.y;
    const CGFloat markX = g.split ? px + g.numW : px + 2 * g.numW;
    if (canApply && IsChange(L) && m.Selected(idx)) {
        [[NSColor.controlAccentColor colorWithAlphaComponent:0.24] setFill];
        NSRectFillUsingOperation(cell, NSCompositingOperationSourceOver);
        [NSColor.controlAccentColor setFill];
        NSRectFill(NSMakeRect(markX, y, 3, kRowH));
    }
    if (g.split) {
        [self drawNumber:(side == 0 ? L.oldNo : L.newNo) x:px y:y width:g.numW];
    } else {
        [self drawNumber:L.oldNo x:px y:y width:g.numW];
        [self drawNumber:L.newNo x:px + g.numW y:y width:g.numW];
    }
    [self drawMarker:L x:markX y:y width:g.markW];
}

- (void)drawRect:(NSRect)dirty {
    RepoSession* s = _controller.session;
    const std::shared_ptr<FileDiff> dp = [self diff];
    if (!s || !dp || dp->lines.empty()) return;
    const FileDiff& d = *dp;
    const DiffState& m = s->diff;
    const Geo g = [self geo:d];
    const bool canApply = m.req.section > 0 && CanApplyBack(d);
    const NSInteger first = std::max<NSInteger>(0, (NSInteger)floor(NSMinY(dirty) / kRowH));
    const NSInteger last = std::min<NSInteger>(g.rows, (NSInteger)ceil(NSMaxY(dirty) / kRowH) + 1);
    // gutters with the line numbers
    [GutterBg() setFill];
    NSRectFillUsingOperation(NSMakeRect(0, NSMinY(dirty), g.split ? g.numW : 2 * g.numW, NSHeight(dirty)), NSCompositingOperationSourceOver);
    if (g.split) NSRectFillUsingOperation(NSMakeRect(g.half + 1, NSMinY(dirty), g.numW, NSHeight(dirty)), NSCompositingOperationSourceOver);
    for (NSInteger r = first; r < last; ++r) {
        const CGFloat y = (CGFloat)r * kRowH;
        if (!g.split) {
            const int32_t idx = (int32_t)r;
            if (IsBanner(d.lines[(size_t)idx])) {
                [self drawBanner:d line:idx y:y state:m canApply:canApply];
                continue;
            }
            [self drawLineCell:d line:idx cell:NSMakeRect(0, y, g.width, kRowH) state:m canApply:canApply geo:g side:0];
            [self drawText:d line:idx x:g.textX[0] y:y width:g.textW];
            continue;
        }
        const SplitRow& sr = d.split[(size_t)r];
        if (sr.left >= 0 && sr.left == sr.right && IsBanner(d.lines[(size_t)sr.left])) {
            [self drawBanner:d line:sr.left y:y state:m canApply:canApply];
            continue;
        }
        for (int side = 0; side < 2; ++side) {
            const int32_t idx = side == 0 ? sr.left : sr.right;
            const NSRect cell = NSMakeRect(side == 0 ? 0 : g.half + 1, y, g.half - (side == 1 ? 1 : 0), kRowH);
            if (idx < 0) {
                [FillerBg() setFill];
                NSRectFillUsingOperation(cell, NSCompositingOperationSourceOver);
                continue;
            }
            [self drawLineCell:d line:idx cell:cell state:m canApply:canApply geo:g side:side];
            [self drawText:d line:idx x:g.textX[side] y:y width:g.textW];
        }
    }
    if (g.split) {
        [NSColor.separatorColor setFill];
        NSRectFill(NSMakeRect(g.half, NSMinY(dirty), 1, NSHeight(dirty)));
    }
    if (d.truncated && last >= g.rows)
        DrawText(@"The diff is too large and was cut here.", NSMakeRect(16, (CGFloat)g.rows * kRowH + 4, g.width - 32, kRowH),
                 [NSFont systemFontOfSize:11], NSColor.systemOrangeColor);
}

// ---- mouse and keyboard

// The changed line at a point of a row (the side under it, in the split layout), or -1.
- (int32_t)lineAt:(NSPoint)p diff:(const FileDiff&)d geo:(const Geo&)g {
    const NSInteger row = (NSInteger)floor(p.y / kRowH);
    if (row < 0 || row >= g.rows) return -1;
    int32_t idx = (int32_t)row;
    if (g.split) {
        const SplitRow& sr = d.split[(size_t)row];
        idx = p.x < g.half ? sr.left : sr.right;
    }
    return idx >= 0 && IsChange(d.lines[(size_t)idx]) ? idx : -1;
}

// The hunk header line of a row, or -1.
- (int32_t)bannerAt:(NSInteger)row diff:(const FileDiff&)d geo:(const Geo&)g {
    if (row < 0 || row >= g.rows) return -1;
    if (!g.split) return d.lines[(size_t)row].kind == DiffLine::Hunk ? (int32_t)row : -1;
    const SplitRow& sr = d.split[(size_t)row];
    return sr.left >= 0 && sr.left == sr.right && d.lines[(size_t)sr.left].kind == DiffLine::Hunk ? sr.left : -1;
}

- (void)mouseDown:(NSEvent*)event {
    [self.window makeFirstResponder:self];
    RepoSession* s = _controller.session;
    const std::shared_ptr<FileDiff> d = [self diff];
    if (!s || !d) return;
    const Geo g = [self geo:*d];
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    const bool canApply = s->diff.req.section > 0 && CanApplyBack(*d);
    const NSInteger row = (NSInteger)floor(p.y / kRowH);
    const int32_t banner = [self bannerAt:row diff:*d geo:g];
    if (banner >= 0) {
        if (!canApply || s->diff.HunkBusy()) return;
        NSRect main, discard;
        [self hunkButtons:s->diff y:(CGFloat)row * kRowH width:g.width main:&main discard:&discard];
        const int hunk = d->lines[(size_t)banner].hunk;
        if (NSPointInRect(p, main)) [_controller hunkAction:hunk discard:NO];
        else if (NSPointInRect(p, discard)) [_controller hunkAction:hunk discard:YES];
        return;
    }
    const int32_t idx = [self lineAt:p diff:*d geo:g];
    if (!canApply || idx < 0) return;
    const NSEventModifierFlags mods = event.modifierFlags;
    const bool shift = mods & NSEventModifierFlagShift, toggle = (mods & NSEventModifierFlagCommand) && !shift;
    s->SelectDiffLine(idx, shift, toggle);
    _dragging = !toggle;
}

- (void)mouseDragged:(NSEvent*)event {
    if (!_dragging) return;
    [self autoscroll:event];  // past the edges the view scrolls; the line there joins the selection
    RepoSession* s = _controller.session;
    const std::shared_ptr<FileDiff> d = [self diff];
    if (!s || !d) return;
    const Geo g = [self geo:*d];
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    const NSRect visible = self.visibleRect;
    p.y = std::clamp<CGFloat>(p.y, NSMinY(visible) + 1, NSMaxY(visible) - 1);
    const int32_t idx = [self lineAt:p diff:*d geo:g];
    if (idx >= 0) s->SelectDiffLine(idx, true, false);
}

- (void)mouseUp:(NSEvent*)event {
    _dragging = NO;
}

// Horizontal scrolling (trackpad, Shift+wheel) moves the text; vertical scrolling goes to the scroll view.
- (void)scrollWheel:(NSEvent*)event {
    CGFloat dx = event.scrollingDeltaX, dy = event.scrollingDeltaY;
    if (!event.hasPreciseScrollingDeltas) {
        dx *= 10;
        dy *= 10;
    }
    if (dx != 0 && fabs(dx) > fabs(dy)) {
        [self scrollHorizontally:-dx];
        return;
    }
    [super scrollWheel:event];
}

- (void)scrollHorizontally:(CGFloat)by {
    const std::shared_ptr<FileDiff> d = [self diff];
    if (!d) return;
    const CGFloat before = _hscroll;
    _hscroll += by;
    [self clampScroll:*d geo:[self geo:*d]];
    if (_hscroll != before) [self setNeedsDisplayInRect:self.visibleRect];
}

- (void)scrollVertically:(CGFloat)by {
    const NSRect v = self.visibleRect;
    const CGFloat maxY = std::max<CGFloat>(0, NSHeight(self.bounds) - NSHeight(v));
    [self scrollPoint:NSMakePoint(0, std::clamp<CGFloat>(v.origin.y + by, 0, maxY))];
}

- (void)keyDown:(NSEvent*)event {
    [self interpretKeyEvents:@[ event ]];
}

- (void)insertText:(id)text {
}

- (void)cancelOperation:(id)sender {
    [_controller escape];
}
- (void)moveUp:(id)sender { [self scrollVertically:-2 * kRowH]; }
- (void)moveDown:(id)sender { [self scrollVertically:2 * kRowH]; }
- (void)moveLeft:(id)sender { [self scrollHorizontally:-_charW * 8]; }
- (void)moveRight:(id)sender { [self scrollHorizontally:_charW * 8]; }
- (void)scrollPageUp:(id)sender { [self scrollVertically:-NSHeight(self.visibleRect) * 0.9]; }
- (void)scrollPageDown:(id)sender { [self scrollVertically:NSHeight(self.visibleRect) * 0.9]; }
- (void)pageUp:(id)sender { [self scrollPageUp:sender]; }
- (void)pageDown:(id)sender { [self scrollPageDown:sender]; }
- (void)scrollToBeginningOfDocument:(id)sender { [self scrollPoint:NSZeroPoint]; }
- (void)scrollToEndOfDocument:(id)sender { [self scrollVertically:NSHeight(self.bounds)]; }
- (void)moveToBeginningOfDocument:(id)sender { [self scrollToBeginningOfDocument:sender]; }
- (void)moveToEndOfDocument:(id)sender { [self scrollToEndOfDocument:sender]; }

@end

// ---------------------------------------------------------------- controller

@implementation DTDiffController {
    __weak DTRepoWindowController* _owner;
    NSTextField* _letter;
    NSTextField* _where;
    NSTextField* _path;
    NSTextField* _stats;
    NSButton* _fileButton;
    NSButton* _prevHunk;
    NSButton* _nextHunk;
    NSSegmentedControl* _mode;
    NSScrollView* _scroll;
    DTDiffDocView* _doc;
    NSTextField* _message;
    NSString* _shownKey;  // the file on screen: another one starts from the top
    uint64_t _waitSeq;
    // the selected lines
    NSVisualEffectView* _bar;
    NSTextField* _barLabel;
    NSButton* _barMain;
    NSButton* _barDiscard;
}

- (instancetype)initWithOwner:(DTRepoWindowController*)owner {
    if ((self = [super initWithNibName:nil bundle:nil])) _owner = owner;
    return self;
}

- (RepoSession*)session {
    return _owner.session;
}

- (void)loadView {
    NSView* v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 720, 520)];

    // header: the file, then its actions
    _letter = MakeLabel(@"", [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightBold], nil);
    _where = MakeLabel(@"", [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular], NSColor.secondaryLabelColor);
    _path = MakeLabel(@"", [NSFont systemFontOfSize:13], NSColor.labelColor);
    _path.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [_path setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow - 1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    _stats = MakeLabel(@"", [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightMedium], nil);
    NSView* spacer = [[NSView alloc] init];
    [spacer setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    _fileButton = [NSButton buttonWithTitle:@"Stage File" target:self action:@selector(stageFile:)];
    _fileButton.controlSize = NSControlSizeSmall;
    _prevHunk = [NSButton buttonWithImage:Symbol(@"chevron.up", 11, NSFontWeightSemibold) target:self action:@selector(previousHunk:)];
    _nextHunk = [NSButton buttonWithImage:Symbol(@"chevron.down", 11, NSFontWeightSemibold) target:self action:@selector(nextHunk:)];
    _prevHunk.toolTip = @"Previous change";
    _nextHunk.toolTip = @"Next change";
    for (NSButton* b in @[ _prevHunk, _nextHunk ]) b.bordered = NO;
    _mode = [NSSegmentedControl segmentedControlWithLabels:@[ @"Unified", @"Split" ]
                                              trackingMode:NSSegmentSwitchTrackingSelectOne
                                                    target:self
                                                    action:@selector(modeChanged:)];
    _mode.controlSize = NSControlSizeSmall;
    NSButton* close = [NSButton buttonWithImage:Symbol(@"xmark", 11, NSFontWeightSemibold) target:self action:@selector(close:)];
    close.bordered = NO;
    close.toolTip = @"Close (Esc)";
    NSStackView* header = [NSStackView stackViewWithViews:@[ _letter, _where, _path, _stats, spacer, _fileButton, _prevHunk, _nextHunk, _mode, close ]];
    header.spacing = 8;
    header.distribution = NSStackViewDistributionFill;
    header.edgeInsets = NSEdgeInsetsMake(0, 14, 0, 12);
    [header setCustomSpacing:14 afterView:_fileButton];
    [header setCustomSpacing:14 afterView:_nextHunk];
    [header setCustomSpacing:12 afterView:_mode];
    NSBox* line = [[NSBox alloc] init];
    line.boxType = NSBoxSeparator;

    // body
    _doc = [[DTDiffDocView alloc] initWithFrame:NSMakeRect(0, 0, 720, 400)];
    _doc.controller = self;
    _doc.autoresizingMask = NSViewWidthSizable;
    _scroll = [[NSScrollView alloc] init];
    _scroll.documentView = _doc;
    _scroll.hasVerticalScroller = YES;
    _scroll.hasHorizontalScroller = NO;
    _scroll.autohidesScrollers = YES;
    _scroll.backgroundColor = NSColor.textBackgroundColor;
    _scroll.contentView.postsFrameChangedNotifications = YES;
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(widthChanged:) name:NSViewFrameDidChangeNotification object:_scroll.contentView];
    _message = MakeLabel(@"", [NSFont systemFontOfSize:13], NSColor.secondaryLabelColor);
    _message.alignment = NSTextAlignmentCenter;

    // the bar for the selected lines
    _bar = [[NSVisualEffectView alloc] init];
    _bar.material = NSVisualEffectMaterialPopover;
    _bar.blendingMode = NSVisualEffectBlendingModeWithinWindow;
    _bar.state = NSVisualEffectStateActive;
    _bar.wantsLayer = YES;
    _bar.layer.cornerRadius = 10;
    _bar.layer.masksToBounds = YES;
    _bar.hidden = YES;
    _barLabel = MakeLabel(@"", [NSFont systemFontOfSize:12], NSColor.labelColor);
    _barMain = [NSButton buttonWithTitle:@"Stage Lines" target:self action:@selector(linesAction:)];
    _barMain.bezelColor = NSColor.controlAccentColor;
    _barDiscard = [NSButton buttonWithTitle:@"Discard…" target:self action:@selector(linesAction:)];
    _barDiscard.hasDestructiveAction = YES;
    NSButton* clear = [NSButton buttonWithImage:Symbol(@"xmark.circle.fill", 13, NSFontWeightRegular) target:self action:@selector(clearLines:)];
    clear.bordered = NO;
    clear.toolTip = @"Clear the selection (Esc)";
    NSStackView* barRow = [NSStackView stackViewWithViews:@[ _barLabel, _barMain, _barDiscard, clear ]];
    barRow.spacing = 10;
    barRow.edgeInsets = NSEdgeInsetsMake(8, 16, 8, 10);
    barRow.translatesAutoresizingMaskIntoConstraints = NO;
    [_bar addSubview:barRow];

    for (NSView* x in @[ header, line, _scroll, _message, _bar ]) {
        x.translatesAutoresizingMaskIntoConstraints = NO;
        [v addSubview:x];
    }
    [NSLayoutConstraint activateConstraints:@[
        [header.topAnchor constraintEqualToAnchor:v.safeAreaLayoutGuide.topAnchor],
        [header.leadingAnchor constraintEqualToAnchor:v.leadingAnchor],
        [header.trailingAnchor constraintEqualToAnchor:v.trailingAnchor],
        [header.heightAnchor constraintEqualToConstant:38],
        [line.topAnchor constraintEqualToAnchor:header.bottomAnchor],
        [line.leadingAnchor constraintEqualToAnchor:v.leadingAnchor],
        [line.trailingAnchor constraintEqualToAnchor:v.trailingAnchor],
        [line.heightAnchor constraintEqualToConstant:1],
        [_scroll.topAnchor constraintEqualToAnchor:line.bottomAnchor],
        [_scroll.leadingAnchor constraintEqualToAnchor:v.leadingAnchor],
        [_scroll.trailingAnchor constraintEqualToAnchor:v.trailingAnchor],
        [_scroll.bottomAnchor constraintEqualToAnchor:v.bottomAnchor],
        [_message.centerXAnchor constraintEqualToAnchor:_scroll.centerXAnchor],
        [_message.centerYAnchor constraintEqualToAnchor:_scroll.centerYAnchor],
        [_message.leadingAnchor constraintGreaterThanOrEqualToAnchor:v.leadingAnchor constant:24],
        [barRow.leadingAnchor constraintEqualToAnchor:_bar.leadingAnchor],
        [barRow.trailingAnchor constraintEqualToAnchor:_bar.trailingAnchor],
        [barRow.topAnchor constraintEqualToAnchor:_bar.topAnchor],
        [barRow.bottomAnchor constraintEqualToAnchor:_bar.bottomAnchor],
        [_bar.centerXAnchor constraintEqualToAnchor:_scroll.centerXAnchor],
        [_bar.bottomAnchor constraintEqualToAnchor:v.bottomAnchor constant:-20],
    ]];
    self.view = v;
}

- (void)widthChanged:(NSNotification*)note {
    [_doc diffChanged:NO];
}

- (void)reload {
    if (!_doc) return;
    RepoSession* s = self.session;
    if (!s || !s->diff.open) return;
    const DiffState& m = s->diff;
    const std::shared_ptr<FileDiff> d = m.diff;

    // header
    _letter.stringValue = NS(std::wstring(1, StatusLetter(m.req.status)));
    _letter.textColor = StatusColor(m.req.status);
    _where.stringValue = NS(DiffWhere(m));
    const std::wstring& path = m.req.path;
    const size_t slash = path.find_last_of(L'/');
    std::wstring dir = slash == std::wstring::npos ? L"" : path.substr(0, slash + 1);
    std::wstring old = m.req.oldPath;
    if (old.empty() && d) old = d->oldPath;
    if (!old.empty() && old != path) dir = old + L"  →  " + dir;
    NSMutableParagraphStyle* para = [[NSMutableParagraphStyle alloc] init];
    para.lineBreakMode = NSLineBreakByTruncatingMiddle;
    NSMutableAttributedString* title = [[NSMutableAttributedString alloc]
        initWithString:NS(dir)
            attributes:@{ NSForegroundColorAttributeName : NSColor.secondaryLabelColor, NSFontAttributeName : [NSFont systemFontOfSize:13],
                          NSParagraphStyleAttributeName : para }];
    [title appendAttributedString:[[NSAttributedString alloc]
                                      initWithString:NS(slash == std::wstring::npos ? path : path.substr(slash + 1))
                                          attributes:@{ NSForegroundColorAttributeName : NSColor.labelColor,
                                                        NSFontAttributeName : [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold],
                                                        NSParagraphStyleAttributeName : para }]];
    _path.attributedStringValue = title;
    _path.toolTip = NS(path);
    if (d && d->ok && !d->binary && !d->lines.empty()) {
        NSMutableAttributedString* st = [[NSMutableAttributedString alloc] initWithString:[NSString stringWithFormat:@"+%d", d->added]
                                                                               attributes:@{ NSForegroundColorAttributeName : NSColor.systemGreenColor }];
        [st appendAttributedString:[[NSAttributedString alloc] initWithString:[NSString stringWithFormat:@"  −%d", d->deleted]
                                                                   attributes:@{ NSForegroundColorAttributeName : NSColor.systemRedColor }]];
        [st addAttribute:NSFontAttributeName value:[NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightMedium] range:NSMakeRange(0, st.length)];
        _stats.attributedStringValue = st;
        _stats.hidden = NO;
    } else {
        _stats.hidden = YES;
    }
    _fileButton.hidden = m.req.section == 0;
    _fileButton.title = m.req.section == 1 ? @"Stage File" : @"Unstage File";
    _fileButton.enabled = !s->opBusy;
    const bool hunks = d && d->hunks.size() > 1;
    _prevHunk.enabled = hunks;
    _nextHunk.enabled = hunks;
    _mode.selectedSegment = AppSettings().diffSplit ? 1 : 0;
    _mode.enabled = !d || ShowSplit(true, *d);  // a file only added or only deleted has one side

    // body
    const std::wstring notice = DiffNotice(m);
    if (notice.empty()) {
        _message.hidden = YES;
        _scroll.hidden = NO;
        NSString* key = [NSString stringWithFormat:@"%@|%@", NS(m.req.path), NS(m.req.commit.Hex())];
        const BOOL another = ![key isEqualToString:_shownKey];
        _shownKey = key;
        [_doc diffChanged:another];
    } else {
        _scroll.hidden = YES;
        _message.stringValue = NS(notice);
        _message.textColor = d && !d->ok ? NSColor.systemRedColor : NSColor.secondaryLabelColor;
        _message.hidden = !d;  // most diffs arrive before "Loading" is worth showing
        if (!d) {
            const uint64_t seq = _waitSeq = m.seq;
            __weak DTDiffController* weakSelf = self;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.15 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                DTDiffController* me = weakSelf;
                RepoSession* rs = me.session;
                if (me && rs && rs->diff.open && !rs->diff.diff && rs->diff.seq == seq && me->_waitSeq == seq) me->_message.hidden = NO;
            });
        }
        _shownKey = nil;
    }
    [self updateBar];
}

- (void)updateBar {
    RepoSession* s = self.session;
    const DiffState& m = s->diff;
    const bool canApply = m.diff && m.diff->ok && m.req.section > 0 && CanApplyBack(*m.diff);
    _bar.hidden = !canApply || m.selCount == 0;
    if (_bar.hidden) return;
    const bool stage = m.req.section == 1, one = m.selCount == 1;
    _barLabel.stringValue = [NSString stringWithFormat:@"%d %@ selected", m.selCount, one ? @"line" : @"lines"];
    _barMain.title = stage ? (one ? @"Stage Line" : @"Stage Lines") : (one ? @"Unstage Line" : @"Unstage Lines");
    _barDiscard.hidden = !stage;
    _barMain.enabled = !m.HunkBusy();
    _barDiscard.enabled = !m.HunkBusy();
}

// ---- actions

- (void)hunkAction:(int)hunk discard:(BOOL)discard {
    RepoSession* s = self.session;
    if (s) [_owner applyPatch:s->HunkJob(hunk, discard)];
}

- (void)linesAction:(NSButton*)sender {
    RepoSession* s = self.session;
    if (s) [_owner applyPatch:s->LinesJob(sender == _barDiscard)];
}

- (void)clearLines:(id)sender {
    if (RepoSession* s = self.session) s->ClearDiffSelection();
}

- (void)escape {
    RepoSession* s = self.session;
    if (!s) return;
    if (s->diff.selCount) s->ClearDiffSelection();
    else s->CloseDiff();
}

- (void)close:(id)sender {
    if (RepoSession* s = self.session) s->CloseDiff();
}

- (void)stageFile:(id)sender {
    RepoSession* s = self.session;
    if (!s || s->diff.req.section == 0) return;
    std::vector<std::wstring> paths{ s->diff.req.path };
    if (!s->diff.req.oldPath.empty()) paths.push_back(s->diff.req.oldPath);
    [_owner stagePaths:paths stage:s->diff.req.section == 1];
}

- (void)previousHunk:(id)sender {
    [_doc jump:-1];
}

- (void)nextHunk:(id)sender {
    [_doc jump:1];
}

- (void)modeChanged:(NSSegmentedControl*)sender {
    AppSettings().diffSplit = sender.selectedSegment == 1;
    AppSettings().Save();
    [NSNotificationCenter.defaultCenter postNotificationName:DTSettingsChangedNotification object:nil];
}

@end
