// Small helpers shared by the views: text, colors, fonts, drawing, the Canvas widget, menus, questions, and what
// the system does for the app (clipboard, links, the file manager, a terminal).

#include "LinuxUI.h"

#include <unordered_set>

namespace {

graphene_rect_t R(float x, float y, float w, float h) {
    graphene_rect_t r;
    graphene_rect_init(&r, x, y, w, h);
    return r;
}

graphene_point_t P(float x, float y) {
    graphene_point_t p;
    graphene_point_init(&p, x, y);
    return p;
}

}  // namespace

// ---------------------------------------------------------------- text

std::string TitleCase(const std::wstring& s) {
    static const wchar_t* const kSmall[] = { L"a", L"an", L"and", L"as", L"at", L"by", L"for", L"in", L"of", L"on", L"or", L"the", L"to" };
    std::wstring out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(L' ', i);
        if (j == std::wstring::npos) j = s.size();
        std::wstring word = s.substr(i, j - i);
        bool small = false;
        if (i > 0)
            for (const wchar_t* w : kSmall) small |= word == w;
        if (!word.empty() && !small) word[0] = ToUpper(word.substr(0, 1))[0];
        out += word;
        if (j < s.size()) out += L' ';
        i = j + 1;
    }
    return U8(out);
}

std::string Esc(std::string_view s) {
    char* e = g_markup_escape_text(s.data(), (gssize)s.size());
    std::string r = e ? e : "";
    g_free(e);
    return r;
}

// ---------------------------------------------------------------- colors

namespace {
Palette g_pal;
bool g_palReady = false;
}  // namespace

GdkRGBA Rgb(uint32_t c, float alpha) {
    GdkRGBA r;
    r.red = ((c >> 16) & 255) / 255.0f;
    r.green = ((c >> 8) & 255) / 255.0f;
    r.blue = (c & 255) / 255.0f;
    r.alpha = alpha;
    return r;
}

GdkRGBA WithAlpha(GdkRGBA c, float alpha) {
    c.alpha *= alpha;
    return c;
}

GdkRGBA Palette::Fg(float alpha) const { return WithAlpha(fg, alpha); }

const Palette& Pal() {
    if (!g_palReady) UpdatePalette();
    return g_pal;
}

// libadwaita's named colors as the style defines them (Ubuntu's accent, for one); its defaults where the style
// doesn't answer.
void UpdatePalette() {
    Palette p;
    p.dark = adw_style_manager_get_dark(adw_style_manager_get_default());
    p.fg = p.dark ? Rgb(0xffffff) : Rgb(0x000000, 0.8f);
    p.viewBg = p.dark ? Rgb(0x1e1e1e) : Rgb(0xffffff);
    p.accent = Rgb(0x3584e4);
    p.accentText = p.dark ? Rgb(0x78aeed) : Rgb(0x1c71d8);
    p.success = p.dark ? Rgb(0x8ff0a4) : Rgb(0x1b8553);
    p.error = p.dark ? Rgb(0xff7b63) : Rgb(0xc01c28);
    p.warning = p.dark ? Rgb(0xf8e45c) : Rgb(0x9c6e03);
    static GtkWidget* probe = GTK_WIDGET(g_object_ref_sink(gtk_label_new("")));
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GtkStyleContext* ctx = gtk_widget_get_style_context(probe);
    const struct {
        const char* name;
        GdkRGBA* out;
    } named[] = {
        { "window_fg_color", &p.fg }, { "view_bg_color", &p.viewBg }, { "accent_bg_color", &p.accent }, { "accent_color", &p.accentText },
        { "success_color", &p.success }, { "error_color", &p.error }, { "warning_color", &p.warning },
    };
    for (const auto& n : named) {
        GdkRGBA c;
        if (gtk_style_context_lookup_color(ctx, n.name, &c)) *n.out = c;
    }
    G_GNUC_END_IGNORE_DEPRECATIONS
#if ADW_CHECK_VERSION(1, 6, 0)
    GdkRGBA* accent = adw_style_manager_get_accent_color_rgba(adw_style_manager_get_default());
    if (accent) {
        p.accent = *accent;
        gdk_rgba_free(accent);
    }
#endif
    g_pal = p;
    g_palReady = true;
}

GdkRGBA LaneColor(int lane) {
    // GNOME's palette: purple, orange, green, pink, teal, blue, brown, yellow, red; lighter in the dark style
    static const uint32_t light[] = { 0x9141ac, 0xe66100, 0x26a269, 0xd56199, 0x2190a4, 0x1a5fb4, 0x986a44, 0xc88800, 0xc01c28 };
    static const uint32_t dark[] = { 0xc061cb, 0xffa348, 0x57e389, 0xf28ac2, 0x5bc8d8, 0x99c1f1, 0xcdab8f, 0xf6d32d, 0xf66151 };
    if (lane <= 0) return Pal().dark ? WithAlpha(Pal().accentText, 1) : Pal().accent;
    const size_t i = (size_t)(lane - 1) % (sizeof light / sizeof light[0]);
    return Rgb(Pal().dark ? dark[i] : light[i]);
}

GdkRGBA StatusColor(wchar_t st) {
    const bool dk = Pal().dark;
    switch (st) {
    case L'A': case L'?': return Pal().success;
    case L'D': return Pal().error;
    case L'R': case L'C': return Pal().accentText;
    case L'U': return Rgb(dk ? 0xf28ac2 : 0xc0367a);
    default: return Rgb(dk ? 0xffa348 : 0xc64600);
    }
}

GdkRGBA AddedColor() { return Pal().success; }
GdkRGBA DeletedColor() { return Pal().error; }

// ---------------------------------------------------------------- fonts

namespace {
Fonts g_fonts;
bool g_fontsReady = false;
std::unordered_map<const PangoFontDescription*, float> g_lineHeights;
}  // namespace

void ResetFonts() {
    for (PangoFontDescription** f : { &g_fonts.ui, &g_fonts.medium, &g_fonts.bold, &g_fonts.small, &g_fonts.smallBold, &g_fonts.mono,
                                      &g_fonts.monoSmall, &g_fonts.monoSmallBold }) {
        if (*f) pango_font_description_free(*f);
        *f = nullptr;
    }
    g_fontsReady = false;
    g_lineHeights.clear();
}

const Fonts& Font() {
    if (g_fontsReady) return g_fonts;
    char* name = nullptr;
    g_object_get(gtk_settings_get_default(), "gtk-font-name", &name, nullptr);
    PangoFontDescription* base = pango_font_description_from_string(name && *name ? name : "Sans 10");
    g_free(name);
    if (pango_font_description_get_size(base) <= 0) pango_font_description_set_size(base, 10 * PANGO_SCALE);
    const int size = pango_font_description_get_size(base);
    const bool absolute = pango_font_description_get_size_is_absolute(base);
    auto make = [&](double scale, PangoWeight weight, const char* family) {
        PangoFontDescription* f = pango_font_description_copy(base);
        if (family) pango_font_description_set_family(f, family);
        const int sz = (int)lround(size * scale);
        if (absolute) pango_font_description_set_absolute_size(f, sz);
        else pango_font_description_set_size(f, sz);
        pango_font_description_set_weight(f, weight);
        return f;
    };
    g_fonts.ui = make(1, PANGO_WEIGHT_NORMAL, nullptr);
    g_fonts.medium = make(1, PANGO_WEIGHT_SEMIBOLD, nullptr);
    g_fonts.bold = make(1, PANGO_WEIGHT_BOLD, nullptr);
    g_fonts.small = make(0.85, PANGO_WEIGHT_NORMAL, nullptr);
    g_fonts.smallBold = make(0.85, PANGO_WEIGHT_BOLD, nullptr);
    g_fonts.mono = make(0.95, PANGO_WEIGHT_NORMAL, "Monospace");
    g_fonts.monoSmall = make(0.85, PANGO_WEIGHT_NORMAL, "Monospace");
    g_fonts.monoSmallBold = make(0.85, PANGO_WEIGHT_BOLD, "Monospace");
    pango_font_description_free(base);
    g_fontsReady = true;
    return g_fonts;
}

// ---------------------------------------------------------------- drawing

namespace draw {

void Rect(GtkSnapshot* s, float x, float y, float w, float h, const GdkRGBA& c) {
    if (w <= 0 || h <= 0 || c.alpha <= 0) return;
    const graphene_rect_t r = R(x, y, w, h);
    gtk_snapshot_append_color(s, &c, &r);
}

void Round(GtkSnapshot* s, float x, float y, float w, float h, float radius, const GdkRGBA& c) {
    if (w <= 0 || h <= 0 || c.alpha <= 0) return;
    const graphene_rect_t r = R(x, y, w, h);
    GskRoundedRect rr;
    gsk_rounded_rect_init_from_rect(&rr, &r, std::min(radius, std::min(w, h) / 2));
    gtk_snapshot_push_rounded_clip(s, &rr);
    gtk_snapshot_append_color(s, &c, &r);
    gtk_snapshot_pop(s);
}

void RoundBorder(GtkSnapshot* s, float x, float y, float w, float h, float radius, float width, const GdkRGBA& c) {
    if (w <= 0 || h <= 0 || c.alpha <= 0) return;
    const graphene_rect_t r = R(x, y, w, h);
    GskRoundedRect rr;
    gsk_rounded_rect_init_from_rect(&rr, &r, std::min(radius, std::min(w, h) / 2));
    const float widths[4] = { width, width, width, width };
    const GdkRGBA colors[4] = { c, c, c, c };
    gtk_snapshot_append_border(s, &rr, widths, colors);
}

void Circle(GtkSnapshot* s, float cx, float cy, float r, const GdkRGBA& c) {
    GskPathBuilder* b = gsk_path_builder_new();
    const graphene_point_t p = P(cx, cy);
    gsk_path_builder_add_circle(b, &p, r);
    GskPath* path = gsk_path_builder_free_to_path(b);
    gtk_snapshot_append_fill(s, path, GSK_FILL_RULE_WINDING, &c);
    gsk_path_unref(path);
}

void Ring(GtkSnapshot* s, float cx, float cy, float r, float width, const GdkRGBA& c, float dash) {
    GskPathBuilder* b = gsk_path_builder_new();
    const graphene_point_t p = P(cx, cy);
    gsk_path_builder_add_circle(b, &p, r);
    GskPath* path = gsk_path_builder_free_to_path(b);
    GskStroke* st = gsk_stroke_new(width);
    if (dash > 0) {
        const float d[2] = { dash, dash * 0.85f };
        gsk_stroke_set_dash(st, d, 2);
    }
    gtk_snapshot_append_stroke(s, path, st, &c);
    gsk_stroke_free(st);
    gsk_path_unref(path);
}

static PangoLayout* Layout(GtkWidget* w, std::string_view text, const PangoFontDescription* font) {
    PangoLayout* l = gtk_widget_create_pango_layout(w, nullptr);
    pango_layout_set_font_description(l, font);
    pango_layout_set_single_paragraph_mode(l, TRUE);
    pango_layout_set_text(l, text.data(), (int)text.size());
    return l;
}

float Text(GtkSnapshot* s, GtkWidget* w, std::string_view text, const PangoFontDescription* font, const GdkRGBA& c, float x, float y,
           float width, float height, PangoEllipsizeMode mode, bool right) {
    if (text.empty() || width < 2) return 0;
    PangoLayout* l = Layout(w, text, font);
    pango_layout_set_width(l, (int)(width * PANGO_SCALE));
    pango_layout_set_ellipsize(l, mode);
    int lw = 0, lh = 0;
    pango_layout_get_pixel_size(l, &lw, &lh);
    const float dx = right ? std::max(0.0f, width - (float)lw) : 0;
    gtk_snapshot_save(s);
    const graphene_point_t p = P(roundf(x + dx), roundf(y + (height - (float)lh) / 2));
    gtk_snapshot_translate(s, &p);
    gtk_snapshot_append_layout(s, l, &c);
    gtk_snapshot_restore(s);
    g_object_unref(l);
    return (float)lw;
}

float TextWidth(GtkWidget* w, std::string_view text, const PangoFontDescription* font) {
    if (text.empty()) return 0;
    PangoLayout* l = Layout(w, text, font);
    int lw = 0, lh = 0;
    pango_layout_get_pixel_size(l, &lw, &lh);
    g_object_unref(l);
    return (float)lw;
}

float LineHeight(GtkWidget* w, const PangoFontDescription* font) {
    if (auto it = g_lineHeights.find(font); it != g_lineHeights.end()) return it->second;
    PangoLayout* l = Layout(w, "Ag", font);
    int lw = 0, lh = 0;
    pango_layout_get_pixel_size(l, &lw, &lh);
    g_object_unref(l);
    g_lineHeights[font] = (float)lh;
    return (float)lh;
}

void Icon(GtkSnapshot* s, GtkWidget* w, const char* name, float x, float y, float size, const GdkRGBA& c) {
    static std::unordered_map<std::string, GtkIconPaintable*> cache;
    const int scale = gtk_widget_get_scale_factor(w);
    const std::string key = std::string(name) + "|" + std::to_string((int)size) + "|" + std::to_string(scale);
    GtkIconPaintable* icon = nullptr;
    if (auto it = cache.find(key); it != cache.end()) icon = it->second;
    else {
        GtkIconTheme* theme = gtk_icon_theme_get_for_display(gtk_widget_get_display(w));
        icon = gtk_icon_theme_lookup_icon(theme, name, nullptr, (int)size, scale, GTK_TEXT_DIR_NONE, (GtkIconLookupFlags)0);
        cache[key] = icon;
    }
    if (!icon) return;
    const GdkRGBA colors[4] = { c, Pal().error, Pal().warning, Pal().success };
    gtk_snapshot_save(s);
    const graphene_point_t p = P(roundf(x), roundf(y));
    gtk_snapshot_translate(s, &p);
    gtk_symbolic_paintable_snapshot_symbolic(GTK_SYMBOLIC_PAINTABLE(icon), s, size, size, colors, 4);
    gtk_snapshot_restore(s);
}

void Avatar(GtkSnapshot* s, GtkWidget* w, float x, float y, float size, const std::wstring& initials, uint32_t color) {
    Circle(s, x + size / 2, y + size / 2, size / 2, Rgb(color));
    PangoFontDescription* f = pango_font_description_copy(Font().bold);
    pango_font_description_set_absolute_size(f, size * 0.38 * PANGO_SCALE);
    const std::string text = U8(initials);
    const float tw = TextWidth(w, text, f);
    Text(s, w, text, f, Rgb(0xffffff, 0.95f), x + (size - tw) / 2, y, tw + 2, size);
    pango_font_description_free(f);
}

float ButtonWidth(GtkWidget* w, std::string_view label) { return TextWidth(w, label, Font().small) + 20; }

void Button(GtkSnapshot* s, GtkWidget* w, const graphene_rect_t& r, std::string_view label, bool hover, bool enabled) {
    Round(s, r.origin.x, r.origin.y, r.size.width, r.size.height, 6, Pal().Fg(hover && enabled ? 0.15f : 0.08f));
    Text(s, w, label, Font().small, Pal().Fg(enabled ? 0.9f : 0.4f), r.origin.x + 10, r.origin.y, r.size.width - 19, r.size.height);
}

}  // namespace draw

// ---------------------------------------------------------------- the Canvas widget

namespace {

std::unordered_set<Canvas*>& Canvases() {
    static std::unordered_set<Canvas*> all;
    return all;
}

}  // namespace

struct _MgCanvas {
    GtkWidget parent_instance;
    Canvas* canvas;
    GtkAdjustment* hadj;
    GtkAdjustment* vadj;
    GtkScrollablePolicy hpolicy, vpolicy;
};

G_DECLARE_FINAL_TYPE(MgCanvas, mg_canvas, MG, CANVAS, GtkWidget)
G_DEFINE_FINAL_TYPE_WITH_CODE(MgCanvas, mg_canvas, GTK_TYPE_WIDGET, G_IMPLEMENT_INTERFACE(GTK_TYPE_SCROLLABLE, nullptr))

enum { PROP_0, PROP_HADJUSTMENT, PROP_VADJUSTMENT, PROP_HSCROLL_POLICY, PROP_VSCROLL_POLICY };

static void Configure(MgCanvas* c) {
    GtkWidget* w = GTK_WIDGET(c);
    const double width = gtk_widget_get_width(w), height = gtk_widget_get_height(w);
    if (c->vadj) {
        const double content = std::max(height, c->canvas ? c->canvas->ContentHeight() : 0.0);
        const double value = std::clamp(gtk_adjustment_get_value(c->vadj), 0.0, std::max(0.0, content - height));
        gtk_adjustment_configure(c->vadj, value, 0, content, std::max(1.0, height * 0.1), std::max(1.0, height * 0.9), height);
    }
    if (c->hadj) {
        double content = c->canvas ? c->canvas->ContentWidth() : 0.0;
        content = std::max(width, content);
        const double value = std::clamp(gtk_adjustment_get_value(c->hadj), 0.0, std::max(0.0, content - width));
        gtk_adjustment_configure(c->hadj, value, 0, content, std::max(1.0, width * 0.1), std::max(1.0, width * 0.9), width);
    }
}

static void OnAdjustmentValue(GtkAdjustment*, gpointer data) {
    MgCanvas* c = MG_CANVAS(data);
    if (c->canvas) c->canvas->Scrolled();
    gtk_widget_queue_draw(GTK_WIDGET(c));
}

static void SetAdjustment(MgCanvas* c, GtkAdjustment** slot, GtkAdjustment* a) {
    if (a && *slot == a) return;
    if (*slot) {
        g_signal_handlers_disconnect_by_data(*slot, c);
        g_object_unref(*slot);
    }
    if (!a) a = gtk_adjustment_new(0, 0, 0, 0, 0, 0);
    *slot = GTK_ADJUSTMENT(g_object_ref_sink(a));
    g_signal_connect(a, "value-changed", G_CALLBACK(OnAdjustmentValue), c);
    Configure(c);
}

static void mg_canvas_set_property(GObject* o, guint id, const GValue* v, GParamSpec* ps) {
    MgCanvas* c = MG_CANVAS(o);
    switch (id) {
    case PROP_HADJUSTMENT: SetAdjustment(c, &c->hadj, GTK_ADJUSTMENT(g_value_get_object(v))); break;
    case PROP_VADJUSTMENT: SetAdjustment(c, &c->vadj, GTK_ADJUSTMENT(g_value_get_object(v))); break;
    case PROP_HSCROLL_POLICY: c->hpolicy = (GtkScrollablePolicy)g_value_get_enum(v); break;
    case PROP_VSCROLL_POLICY: c->vpolicy = (GtkScrollablePolicy)g_value_get_enum(v); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(o, id, ps);
    }
}

static void mg_canvas_get_property(GObject* o, guint id, GValue* v, GParamSpec* ps) {
    MgCanvas* c = MG_CANVAS(o);
    switch (id) {
    case PROP_HADJUSTMENT: g_value_set_object(v, c->hadj); break;
    case PROP_VADJUSTMENT: g_value_set_object(v, c->vadj); break;
    case PROP_HSCROLL_POLICY: g_value_set_enum(v, c->hpolicy); break;
    case PROP_VSCROLL_POLICY: g_value_set_enum(v, c->vpolicy); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(o, id, ps);
    }
}

static void mg_canvas_dispose(GObject* o) {
    MgCanvas* c = MG_CANVAS(o);
    if (Canvas* cv = c->canvas) {
        c->canvas = nullptr;
        cv->widget = nullptr;
        delete cv;
    }
    for (GtkAdjustment** a : { &c->hadj, &c->vadj }) {
        if (!*a) continue;
        g_signal_handlers_disconnect_by_data(*a, c);
        g_clear_object(a);
    }
    G_OBJECT_CLASS(mg_canvas_parent_class)->dispose(o);
}

static void mg_canvas_snapshot(GtkWidget* w, GtkSnapshot* s) {
    MgCanvas* c = MG_CANVAS(w);
    if (!c->canvas) return;
    const float width = (float)gtk_widget_get_width(w), height = (float)gtk_widget_get_height(w);
    const graphene_rect_t clip = R(0, 0, width, height);
    gtk_snapshot_push_clip(s, &clip);
    c->canvas->Draw(s, width, height);
    gtk_snapshot_pop(s);
}

static void mg_canvas_size_allocate(GtkWidget* w, int, int, int) {
    MgCanvas* c = MG_CANVAS(w);
    if (c->canvas) c->canvas->Resized();
    Configure(c);
}

static void mg_canvas_measure(GtkWidget*, GtkOrientation, int, int* minimum, int* natural, int* minBaseline, int* natBaseline) {
    *minimum = 0;
    *natural = 0;
    *minBaseline = -1;
    *natBaseline = -1;
}

static void mg_canvas_class_init(MgCanvasClass* klass) {
    GObjectClass* oc = G_OBJECT_CLASS(klass);
    GtkWidgetClass* wc = GTK_WIDGET_CLASS(klass);
    oc->set_property = mg_canvas_set_property;
    oc->get_property = mg_canvas_get_property;
    oc->dispose = mg_canvas_dispose;
    wc->snapshot = mg_canvas_snapshot;
    wc->size_allocate = mg_canvas_size_allocate;
    wc->measure = mg_canvas_measure;
    g_object_class_override_property(oc, PROP_HADJUSTMENT, "hadjustment");
    g_object_class_override_property(oc, PROP_VADJUSTMENT, "vadjustment");
    g_object_class_override_property(oc, PROP_HSCROLL_POLICY, "hscroll-policy");
    g_object_class_override_property(oc, PROP_VSCROLL_POLICY, "vscroll-policy");
    gtk_widget_class_set_css_name(wc, "mgcanvas");
}

static void mg_canvas_init(MgCanvas* c) {
    c->canvas = nullptr;
    c->hadj = nullptr;
    c->vadj = nullptr;
    c->hpolicy = GTK_SCROLL_MINIMUM;
    c->vpolicy = GTK_SCROLL_MINIMUM;
    gtk_widget_set_overflow(GTK_WIDGET(c), GTK_OVERFLOW_HIDDEN);
}

Canvas::~Canvas() { Canvases().erase(this); }

float Canvas::Width() const { return widget ? (float)gtk_widget_get_width(widget) : 0; }
float Canvas::Height() const { return widget ? (float)gtk_widget_get_height(widget) : 0; }

double Canvas::ScrollY() const {
    MgCanvas* c = widget ? MG_CANVAS(widget) : nullptr;
    return c && c->vadj ? gtk_adjustment_get_value(c->vadj) : 0;
}

double Canvas::ScrollX() const {
    MgCanvas* c = widget ? MG_CANVAS(widget) : nullptr;
    return c && c->hadj ? gtk_adjustment_get_value(c->hadj) : 0;
}

void Canvas::SetScrollY(double y) {
    MgCanvas* c = widget ? MG_CANVAS(widget) : nullptr;
    if (!c || !c->vadj) return;
    const double max = gtk_adjustment_get_upper(c->vadj) - gtk_adjustment_get_page_size(c->vadj);
    gtk_adjustment_set_value(c->vadj, std::clamp(y, 0.0, std::max(0.0, max)));
}

void Canvas::SetScrollX(double x) {
    MgCanvas* c = widget ? MG_CANVAS(widget) : nullptr;
    if (!c || !c->hadj) return;
    const double max = gtk_adjustment_get_upper(c->hadj) - gtk_adjustment_get_page_size(c->hadj);
    gtk_adjustment_set_value(c->hadj, std::clamp(x, 0.0, std::max(0.0, max)));
}

void Canvas::Refresh() {
    if (!widget) return;
    Configure(MG_CANVAS(widget));
    gtk_widget_queue_draw(widget);
}

void Canvas::Redraw() {
    if (widget) gtk_widget_queue_draw(widget);
}

GtkWidget* MakeCanvas(Canvas* c, Scroll scroll) {
    MgCanvas* w = MG_CANVAS(g_object_new(mg_canvas_get_type(), nullptr));
    w->canvas = c;
    c->widget = GTK_WIDGET(w);
    Canvases().insert(c);
    if (scroll == Scroll::None) return GTK_WIDGET(w);
    GtkWidget* sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), scroll == Scroll::Both ? GTK_POLICY_AUTOMATIC : GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), GTK_WIDGET(w));
    c->scroller = sw;
    return sw;
}

void RedrawAllCanvases() {
    for (Canvas* c : Canvases()) c->Refresh();
}

// ---------------------------------------------------------------- menus

namespace {

struct MenuItemRef {
    std::vector<std::function<void()>>* fns;
    size_t index;
};

std::string MenuLabel(const std::string& s) {  // "_" would underline the next letter
    std::string r;
    for (char ch : s) {
        r += ch;
        if (ch == '_') r += '_';
    }
    return r;
}

}  // namespace

Menu::Menu() {
    root = g_menu_new();
    section = g_menu_new();
    g_menu_append_section(root, nullptr, G_MENU_MODEL(section));
    group = g_simple_action_group_new();
    fns = new std::vector<std::function<void()>>();
    g_object_set_data_full(G_OBJECT(group), "mg-fns", fns, [](gpointer p) { delete static_cast<std::vector<std::function<void()>>*>(p); });
}

Menu::~Menu() {
    g_object_unref(section);
    g_object_unref(root);
    g_object_unref(group);
}

void Menu::Add(const std::string& label, std::function<void()> fn, bool enabled) {
    const std::string name = "a" + std::to_string(count++);
    fns->push_back(std::move(fn));
    GSimpleAction* a = g_simple_action_new(name.c_str(), nullptr);
    g_simple_action_set_enabled(a, enabled);
    g_signal_connect_data(
        a, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer data) {
            const MenuItemRef* ref = static_cast<MenuItemRef*>(data);
            const std::function<void()> f = (*ref->fns)[ref->index];  // a copy: the call may close the menu
            if (f) f();
        }),
        new MenuItemRef{ fns, fns->size() - 1 }, [](gpointer data, GClosure*) { delete static_cast<MenuItemRef*>(data); }, (GConnectFlags)0);
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(a));
    g_object_unref(a);
    GMenuItem* item = g_menu_item_new(MenuLabel(label).c_str(), ("ctx." + name).c_str());
    g_menu_append_item(section, item);
    g_object_unref(item);
}

void Menu::Section() {
    if (g_menu_model_get_n_items(G_MENU_MODEL(section)) == 0) return;
    g_object_unref(section);
    section = g_menu_new();
    g_menu_append_section(root, nullptr, G_MENU_MODEL(section));
}

static GtkWidget* MenuPopover(GMenu* root, GSimpleActionGroup* group, GtkWidget* parent) {
    GtkWidget* pop = gtk_popover_menu_new_from_model(G_MENU_MODEL(root));
    gtk_widget_insert_action_group(pop, "ctx", G_ACTION_GROUP(group));
    gtk_widget_set_parent(pop, parent);
    g_signal_connect(pop, "closed", G_CALLBACK(+[](GtkPopover* p, gpointer) {
        g_object_ref(p);
        g_idle_add_once(
            [](gpointer data) {
                GtkWidget* w = GTK_WIDGET(data);
                if (gtk_widget_get_parent(w)) gtk_widget_unparent(w);
                g_object_unref(w);
            },
            p);
    }), nullptr);
    return pop;
}

void Menu::PopupAt(GtkWidget* parent, double x, double y) {
    if (count == 0 || !parent) return;
    GtkWidget* pop = MenuPopover(root, group, parent);
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    gtk_widget_set_halign(pop, GTK_ALIGN_START);
    const GdkRectangle r = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &r);
    gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_BOTTOM);
    gtk_popover_popup(GTK_POPOVER(pop));
}

void Menu::PopupUnder(GtkWidget* button) {
    if (count == 0 || !button) return;
    GtkWidget* pop = MenuPopover(root, group, button);
    gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_BOTTOM);
    gtk_popover_popup(GTK_POPOVER(pop));
}

// ---------------------------------------------------------------- questions

void RunPrompt(GtkWidget* parent, const Prompt& p, std::function<void(int)> done) {
    AdwDialog* d = adw_alert_dialog_new(U8(p.title).c_str(), p.message.empty() ? nullptr : U8(p.message).c_str());
    AdwAlertDialog* a = ADW_ALERT_DIALOG(d);
    const int n = (int)p.buttons.size();
    // GNOME's order: the button that cancels first, the default one last; Enter never loses data
    for (int i = n - 1; i >= 0; --i) {
        const std::string id = "r" + std::to_string(i);
        adw_alert_dialog_add_response(a, id.c_str(), TitleCase(p.buttons[(size_t)i]).c_str());
        if (i == p.danger) adw_alert_dialog_set_response_appearance(a, id.c_str(), ADW_RESPONSE_DESTRUCTIVE);
        else if (i == p.defaultButton && i != n - 1) adw_alert_dialog_set_response_appearance(a, id.c_str(), ADW_RESPONSE_SUGGESTED);
    }
    const int def = p.defaultButton == p.danger ? n - 1 : p.defaultButton;
    adw_alert_dialog_set_default_response(a, ("r" + std::to_string(def)).c_str());
    adw_alert_dialog_set_close_response(a, ("r" + std::to_string(n - 1)).c_str());
    struct Done {
        std::function<void(int)> fn;
        int count;
    };
    g_signal_connect_data(
        d, "response", G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) {
            const Done* dn = static_cast<Done*>(data);
            int b = response && response[0] == 'r' ? atoi(response + 1) : -1;
            if (b < 0 || b >= dn->count) b = dn->count - 1;  // anything else cancels
            if (dn->fn) dn->fn(b);
        }),
        new Done{ std::move(done), n }, [](gpointer data, GClosure*) { delete static_cast<Done*>(data); }, (GConnectFlags)0);
    adw_dialog_present(d, parent);
}

// ---------------------------------------------------------------- the system

void InstallMainDispatcher() {
    SetMainDispatcher([](std::function<void()> fn) {
        auto* f = new std::function<void()>(std::move(fn));
        g_idle_add_full(
            G_PRIORITY_DEFAULT,
            [](gpointer data) -> gboolean {
                std::unique_ptr<std::function<void()>> call(static_cast<std::function<void()>*>(data));
                (*call)();
                return G_SOURCE_REMOVE;
            },
            f, nullptr);
    });
}

void Later(std::function<void()> fn) {
    g_idle_add_full(
        G_PRIORITY_DEFAULT_IDLE,
        [](gpointer data) -> gboolean {
            (*static_cast<std::function<void()>*>(data))();
            return G_SOURCE_REMOVE;
        },
        new std::function<void()>(std::move(fn)), [](gpointer data) { delete static_cast<std::function<void()>*>(data); });
}

guint After(unsigned ms, std::function<void()> fn) {
    return g_timeout_add_full(
        G_PRIORITY_DEFAULT, ms,
        [](gpointer data) -> gboolean {
            (*static_cast<std::function<void()>*>(data))();
            return G_SOURCE_REMOVE;
        },
        new std::function<void()>(std::move(fn)), [](gpointer data) { delete static_cast<std::function<void()>*>(data); });
}

void CopyToClipboard(GtkWidget* w, const std::string& text) {
    if (w) gdk_clipboard_set_text(gtk_widget_get_clipboard(w), text.c_str());
}

static GtkWindow* WindowOf(GtkWidget* w) {
    GtkRoot* root = w ? gtk_widget_get_root(w) : nullptr;
    return root && GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : nullptr;
}

void OpenUri(GtkWidget* w, const std::string& uri) {
    GtkUriLauncher* l = gtk_uri_launcher_new(uri.c_str());
    gtk_uri_launcher_launch(l, WindowOf(w), nullptr, nullptr, nullptr);
    g_object_unref(l);
}

void OpenFolder(GtkWidget* w, const std::wstring& path) {
    GFile* f = g_file_new_for_path(U8(path).c_str());
    GtkFileLauncher* l = gtk_file_launcher_new(f);
    gtk_file_launcher_launch(l, WindowOf(w), nullptr, nullptr, nullptr);
    g_object_unref(l);
    g_object_unref(f);
}

void RevealFile(GtkWidget* w, const std::wstring& path) {
    GFile* f = g_file_new_for_path(U8(path).c_str());
    GtkFileLauncher* l = gtk_file_launcher_new(f);
    gtk_file_launcher_open_containing_folder(l, WindowOf(w), nullptr, nullptr, nullptr);
    g_object_unref(l);
    g_object_unref(f);
}

// The user's terminal, in the folder: the first one installed, from the standard launcher to the classics.
bool OpenTerminal(const std::wstring& dir) {
    const std::string d = U8(dir);
    const std::vector<std::vector<std::string>> terminals = {
        { "xdg-terminal-exec" },
        { "ptyxis", "--new-window", "--working-directory=" + d },
        { "kgx", "--working-directory=" + d },
        { "gnome-terminal", "--working-directory=" + d },
        { "konsole", "--workdir", d },
        { "xfce4-terminal", "--working-directory=" + d },
        { "tilix", "--working-directory=" + d },
        { "kitty", "--directory", d },
        { "alacritty", "--working-directory", d },
        { "wezterm", "start", "--cwd", d },
        { "foot", "--working-directory=" + d },
        { "x-terminal-emulator" },
        { "xterm" },
    };
    for (const std::vector<std::string>& t : terminals) {
        char* exe = g_find_program_in_path(t[0].c_str());
        if (!exe) continue;
        std::vector<char*> argv;
        argv.push_back(exe);
        for (size_t i = 1; i < t.size(); ++i) argv.push_back(const_cast<char*>(t[i].c_str()));
        argv.push_back(nullptr);
        const gboolean ok = g_spawn_async(d.c_str(), argv.data(), nullptr, G_SPAWN_DEFAULT, nullptr, nullptr, nullptr, nullptr);
        g_free(exe);
        if (ok) return true;
    }
    return false;
}

std::string FileUri(const std::wstring& path) {
    char* uri = g_filename_to_uri(U8(path).c_str(), nullptr, nullptr);
    std::string r = uri ? uri : "";
    g_free(uri);
    return r;
}

GtkWidget* MakeLabel(const std::string& text, const char* cssClass) {
    GtkWidget* l = gtk_label_new(text.c_str());
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    if (cssClass) AddClass(l, cssClass);
    return l;
}

void AddClass(GtkWidget* w, const char* cssClass) {  // one or more, separated by spaces
    gchar** names = g_strsplit(cssClass, " ", -1);
    for (gchar** n = names; *n; ++n)
        if (**n) gtk_widget_add_css_class(w, *n);
    g_strfreev(names);
}

void SetClass(GtkWidget* w, const char* cssClass, bool on) {
    if (on) gtk_widget_add_css_class(w, cssClass);
    else gtk_widget_remove_css_class(w, cssClass);
}

void ApplyColorScheme() {
    const std::wstring theme = AppSettings().Get(L"theme");
    adw_style_manager_set_color_scheme(adw_style_manager_get_default(), theme == L"light"  ? ADW_COLOR_SCHEME_FORCE_LIGHT
                                                                        : theme == L"dark" ? ADW_COLOR_SCHEME_FORCE_DARK
                                                                                           : ADW_COLOR_SCHEME_DEFAULT);
}
