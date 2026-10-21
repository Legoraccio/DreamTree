// Small helpers shared by the controllers: text conversion, colors, symbols, drawing, alerts.

#import "MacUI.h"

static_assert(sizeof(wchar_t) == 4, "wide text is UTF-32 on macOS");

NSString* const MGSettingsChangedNotification = @"MGSettingsChanged";

// ---------------------------------------------------------------- text

NSString* NS(std::wstring_view s) {
    if (s.empty()) return @"";
    NSString* r = [[NSString alloc] initWithBytes:s.data() length:s.size() * sizeof(wchar_t) encoding:NSUTF32LittleEndianStringEncoding];
    return r ? r : @"";
}

NSString* NSUtf8(std::string_view s) {
    if (s.empty()) return @"";
    NSString* r = [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding];
    return r ? r : NS(Widen(s));  // invalid UTF-8: Widen puts U+FFFD in its place
}

std::wstring WS(NSString* s) {
    if (s.length == 0) return {};
    NSData* d = [s dataUsingEncoding:NSUTF32LittleEndianStringEncoding allowLossyConversion:YES];
    std::wstring r(d.length / sizeof(wchar_t), L'\0');
    if (!r.empty()) memcpy(r.data(), d.bytes, r.size() * sizeof(wchar_t));
    return r;
}

NSUInteger UTF16Length(std::wstring_view s) {
    NSUInteger n = 0;
    for (wchar_t c : s) n += (uint32_t)c > 0xFFFF ? 2 : 1;
    return n;
}

// ---------------------------------------------------------------- colors

NSColor* LaneColor(int lane) {
    static NSArray<NSColor*>* palette = @[ NSColor.systemPurpleColor, NSColor.systemOrangeColor, NSColor.systemGreenColor,
                                           NSColor.systemPinkColor,   NSColor.systemTealColor,   NSColor.systemIndigoColor,
                                           NSColor.systemBrownColor,  NSColor.systemYellowColor, NSColor.systemRedColor ];
    if (lane <= 0) return NSColor.controlAccentColor;
    return palette[(NSUInteger)(lane - 1) % palette.count];
}

NSColor* StatusColor(wchar_t st) {
    switch (st) {
    case L'A': case L'?': return NSColor.systemGreenColor;
    case L'D': return NSColor.systemRedColor;
    case L'R': case L'C': return NSColor.systemBlueColor;
    case L'U': return NSColor.systemPinkColor;
    default: return NSColor.systemOrangeColor;
    }
}

NSColor* AddedColor() { return NSColor.systemGreenColor; }
NSColor* DeletedColor() { return NSColor.systemRedColor; }

// ---------------------------------------------------------------- symbols and drawing

NSImage* Symbol(NSString* name, CGFloat size, NSFontWeight weight) {
    NSImage* img = [NSImage imageWithSystemSymbolName:name accessibilityDescription:nil];
    if (!img) return nil;
    return [img imageWithSymbolConfiguration:[NSImageSymbolConfiguration configurationWithPointSize:size weight:weight]];
}

void DrawSymbol(NSString* name, NSRect r, CGFloat size, NSColor* color) {
    static NSCache<NSString*, NSImage*>* cache = [] {
        NSCache* c = [[NSCache alloc] init];
        c.countLimit = 300;
        return c;
    }();
    NSString* key = [NSString stringWithFormat:@"%@|%.1f|%@", name, size, color];
    NSImage* img = [cache objectForKey:key];
    if (!img) {
        NSImage* base = [NSImage imageWithSystemSymbolName:name accessibilityDescription:nil];
        if (!base) return;
        NSImageSymbolConfiguration* cfg = [NSImageSymbolConfiguration configurationWithPointSize:size weight:NSFontWeightRegular];
        cfg = [cfg configurationByApplyingConfiguration:[NSImageSymbolConfiguration configurationWithPaletteColors:@[ color ]]];
        img = [base imageWithSymbolConfiguration:cfg];
        if (!img) return;
        [cache setObject:img forKey:key];
    }
    const NSSize sz = img.size;
    const NSRect dst = NSMakeRect(round(NSMidX(r) - sz.width / 2), round(NSMidY(r) - sz.height / 2), sz.width, sz.height);
    [img drawInRect:dst fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:nil];
}

void DrawText(NSString* s, NSRect r, NSFont* font, NSColor* color) {
    if (s.length == 0 || r.size.width < 2) return;
    static NSParagraphStyle* para = [] {
        NSMutableParagraphStyle* p = [[NSMutableParagraphStyle alloc] init];
        p.lineBreakMode = NSLineBreakByTruncatingTail;
        return p;
    }();
    static NSLayoutManager* metrics = [[NSLayoutManager alloc] init];
    const CGFloat h = ceil([metrics defaultLineHeightForFont:font]);
    const NSRect line = NSMakeRect(r.origin.x, r.origin.y + floor((r.size.height - h) / 2), r.size.width, h);
    [s drawWithRect:line
            options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine
         attributes:@{ NSFontAttributeName : font, NSForegroundColorAttributeName : color, NSParagraphStyleAttributeName : para }
            context:nil];
}

CGFloat TextWidth(NSString* s, NSFont* font) {
    if (s.length == 0) return 0;
    return ceil([s sizeWithAttributes:@{ NSFontAttributeName : font }].width);
}

void DrawAvatar(NSRect r, const Author& a) {
    [[NSColor colorWithSRGBRed:((a.color >> 16) & 255) / 255.0 green:((a.color >> 8) & 255) / 255.0 blue:(a.color & 255) / 255.0 alpha:1] setFill];
    [[NSBezierPath bezierPathWithOvalInRect:r] fill];
    NSDictionary* attrs = @{
        NSFontAttributeName : [NSFont systemFontOfSize:round(r.size.height * 0.4) weight:NSFontWeightSemibold],
        NSForegroundColorAttributeName : [NSColor colorWithWhite:1 alpha:0.95],
    };
    NSString* s = NS(a.initials);
    const NSSize sz = [s sizeWithAttributes:attrs];
    [s drawAtPoint:NSMakePoint(NSMidX(r) - sz.width / 2, NSMidY(r) - sz.height / 2) withAttributes:attrs];
}

NSTextField* MakeLabel(NSString* text, NSFont* font, NSColor* color) {
    NSTextField* l = [NSTextField labelWithString:text ? text : @""];
    if (font) l.font = font;
    if (color) l.textColor = color;
    l.lineBreakMode = NSLineBreakByTruncatingTail;
    return l;
}

// ---------------------------------------------------------------- alerts

void RunPrompt(NSWindow* window, const Prompt& p, void (^done)(int button)) {
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = NS(p.title);
    a.informativeText = NS(p.message);
    a.alertStyle = p.danger >= 0 ? NSAlertStyleWarning : NSAlertStyleInformational;
    for (const std::wstring& b : p.buttons) [a addButtonWithTitle:NS(b).capitalizedString];  // "Force push" -> "Force Push"
    if (p.danger >= 0 && p.danger < (int)a.buttons.count) a.buttons[(NSUInteger)p.danger].hasDestructiveAction = YES;
    const int count = (int)p.buttons.size();
    void (^finish)(NSModalResponse) = ^(NSModalResponse r) {
        const int b = (int)(r - NSAlertFirstButtonReturn);
        done(b >= 0 && b < count ? b : count - 1);  // anything else cancels
    };
    if (window) [a beginSheetModalForWindow:window completionHandler:finish];
    else finish([a runModal]);
}

// ---------------------------------------------------------------- main thread

void InstallMainDispatcher() {
    SetMainDispatcher([](std::function<void()> fn) {
        auto* f = new std::function<void()>(std::move(fn));
        dispatch_async_f(dispatch_get_main_queue(), f, [](void* ctx) {
            std::unique_ptr<std::function<void()>> call(static_cast<std::function<void()>*>(ctx));
            (*call)();
        });
    });
}

// ---------------------------------------------------------------- dropping folders

static NSArray<NSString*>* DroppedFolders(id<NSDraggingInfo> info) {
    NSArray<NSURL*>* urls = [info.draggingPasteboard readObjectsForClasses:@[ NSURL.class ]
                                                                   options:@{ NSPasteboardURLReadingFileURLsOnlyKey : @YES }];
    NSMutableArray<NSString*>* folders = [NSMutableArray array];
    for (NSURL* u in urls) {
        NSNumber* dir = nil;
        [u getResourceValue:&dir forKey:NSURLIsDirectoryKey error:nil];
        if (dir.boolValue) [folders addObject:u.path];
    }
    return folders;
}

@implementation MGDropView

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    return self;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info {
    return self.onDrop && DroppedFolders(info).count ? NSDragOperationCopy : NSDragOperationNone;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
    NSArray<NSString*>* folders = DroppedFolders(info);
    if (!folders.count || !self.onDrop) return NO;
    self.onDrop(folders);
    return YES;
}

@end
