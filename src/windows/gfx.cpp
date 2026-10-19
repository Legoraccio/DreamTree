#include "gfx.h"

bool Gfx::Init(HWND h, UINT w, UINT hgt) {
    hwnd = h;
    width = std::max(1u, w);
    height = std::max(1u, hgt);
    D2D1_FACTORY_OPTIONS fo{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, (void**)d2f.GetAddressOf())))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory1), (IUnknown**)dw.GetAddressOf())))
        return false;
    PickFonts();
    CreateFormats();
    return CreateDevice();
}

bool Gfx::CreateDevice() {
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                         D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, d3d.ReleaseAndGetAddressOf(), nullptr, nullptr);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                               d3d.ReleaseAndGetAddressOf(), nullptr, nullptr);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIDevice1> dxgi;
    d3d.As(&dxgi);
    dxgi->SetMaximumFrameLatency(1);
    ComPtr<IDXGIAdapter> adapter;
    dxgi->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width;
    sd.Height = height;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    sd.Scaling = DXGI_SCALING_STRETCH;
    if (FAILED(factory->CreateSwapChainForComposition(d3d.Get(), &sd, nullptr, swap.ReleaseAndGetAddressOf()))) return false;

    if (FAILED(d2f->CreateDevice(dxgi.Get(), d2dev.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(d2dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, ctx.ReleaseAndGetAddressOf()))) return false;
    ctx->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);  // ClearType needs opaque pixels
    CreateTarget();

    if (FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(dcomp.ReleaseAndGetAddressOf())))) return false;
    dcomp->CreateTargetForHwnd(hwnd, TRUE, dtarget.ReleaseAndGetAddressOf());
    dcomp->CreateVisual(visual.ReleaseAndGetAddressOf());
    visual->SetContent(swap.Get());
    dtarget->SetRoot(visual.Get());
    dcomp->Commit();

    ctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), brush.ReleaseAndGetAddressOf());
    ctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), brush2.ReleaseAndGetAddressOf());

    D2D1_STROKE_STYLE_PROPERTIES dp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                                  D2D1_LINE_JOIN_ROUND, 10.f, D2D1_DASH_STYLE_CUSTOM, 0.f);
    const float dashes[] = { 1.6f, 1.6f };
    d2f->CreateStrokeStyle(dp, dashes, 2, dash.ReleaseAndGetAddressOf());
    D2D1_STROKE_STYLE_PROPERTIES rp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                  D2D1_LINE_JOIN_ROUND);
    d2f->CreateStrokeStyle(rp, nullptr, 0, roundCap.ReleaseAndGetAddressOf());
    return true;
}

void Gfx::CreateTarget() {
    ComPtr<IDXGISurface> surface;
    if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&surface)))) return;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    ctx->CreateBitmapFromDxgiSurface(surface.Get(), &bp, target.ReleaseAndGetAddressOf());
    ctx->SetTarget(target.Get());
}

void Gfx::ReleaseDevice() {
    if (ctx) ctx->SetTarget(nullptr);
    target.Reset();
    brush.Reset();
    brush2.Reset();
    grads.clear();
    visual.Reset();
    dtarget.Reset();
    dcomp.Reset();
    ctx.Reset();
    d2dev.Reset();
    swap.Reset();
    d3d.Reset();
}

void Gfx::Resize(UINT w, UINT h) {
    w = std::max(1u, w);
    h = std::max(1u, h);
    if (w == width && h == height) return;
    width = w;
    height = h;
    if (!swap) return;
    ctx->SetTarget(nullptr);
    target.Reset();
    if (FAILED(swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) {
        ReleaseDevice();
        CreateDevice();
        return;
    }
    CreateTarget();
}

bool Gfx::Begin() {
    if (!d2f || !dw) return false;  // not initialized yet (messages sent during window creation)
    if (!ctx && !CreateDevice()) return false;
    if (!target) CreateTarget();
    ctx->BeginDraw();
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
    return true;
}

void Gfx::End() {
    HRESULT hr = ctx->EndDraw();
    bool lost = hr == D2DERR_RECREATE_TARGET;
    if (!lost) {
        hr = swap->Present(1, 0);
        lost = hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET;
    }
    if (lost) {
        ReleaseDevice();
        CreateDevice();
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

// ---------------------------------------------------------------- fonts

void Gfx::PickFonts() {
    ComPtr<IDWriteFontCollection> sys;
    dw->GetSystemFontCollection(&sys);
    auto has = [&](const wchar_t* name) {
        UINT32 i = 0;
        BOOL exists = FALSE;
        return sys && SUCCEEDED(sys->FindFamilyName(name, &i, &exists)) && exists;
    };
    fontUI = has(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    fontDisplay = has(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : fontUI;
    fontMono = has(L"Cascadia Mono") ? L"Cascadia Mono" : has(L"Consolas") ? L"Consolas" : L"Courier New";
    fontIcons = has(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
}

void Gfx::SetScale(float s) {
    if (std::fabs(s - scale) < 0.001f && fmt[0]) return;
    scale = s;
    CreateFormats();
}

void Gfx::CreateFormats() {
    struct Def {
        F f;
        const std::wstring* family;
        float size;
        DWRITE_FONT_WEIGHT weight;
        int kind;  // 0 single line, 1 icon, 2 wrapping
    };
    const Def defs[] = {
        { F::UI, &fontUI, 13.f, DWRITE_FONT_WEIGHT_NORMAL, 0 },
        { F::UISemi, &fontUI, 13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, 0 },
        { F::Small, &fontUI, 12.f, DWRITE_FONT_WEIGHT_NORMAL, 0 },
        { F::SmallSemi, &fontUI, 12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, 0 },
        { F::Tiny, &fontUI, 11.f, DWRITE_FONT_WEIGHT_NORMAL, 0 },
        { F::TinySemi, &fontUI, 11.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, 0 },
        { F::Mono, &fontMono, 12.f, DWRITE_FONT_WEIGHT_NORMAL, 0 },
        { F::MonoSemi, &fontMono, 12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, 0 },
        { F::MonoSmall, &fontMono, 11.f, DWRITE_FONT_WEIGHT_NORMAL, 0 },
        { F::Nav, &fontUI, 14.f, DWRITE_FONT_WEIGHT_NORMAL, 0 },
        { F::RowTitle, &fontUI, 14.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, 0 },
        { F::Title, &fontDisplay, 16.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, 0 },
        { F::Huge, &fontDisplay, 34.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, 0 },
        { F::Repo, &fontDisplay, 14.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, 0 },
        { F::PageTitle, &fontDisplay, 28.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, 0 },
        { F::Icon10, &fontIcons, 10.f, DWRITE_FONT_WEIGHT_NORMAL, 1 },
        { F::Icon12, &fontIcons, 12.f, DWRITE_FONT_WEIGHT_NORMAL, 1 },
        { F::Icon14, &fontIcons, 14.f, DWRITE_FONT_WEIGHT_NORMAL, 1 },
        { F::Icon16, &fontIcons, 16.f, DWRITE_FONT_WEIGHT_NORMAL, 1 },
        { F::Icon20, &fontIcons, 20.f, DWRITE_FONT_WEIGHT_NORMAL, 1 },
        { F::Icon28, &fontIcons, 28.f, DWRITE_FONT_WEIGHT_NORMAL, 1 },
        { F::Avatar, &fontUI, 12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1 },
        { F::Body, &fontUI, 13.f, DWRITE_FONT_WEIGHT_NORMAL, 2 },
        { F::BodyTitle, &fontDisplay, 18.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2 },
        { F::Code, &fontMono, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, 3 },
    };
    for (const Def& d : defs) {
        ComPtr<IDWriteTextFormat> f;
        if (FAILED(dw->CreateTextFormat(d.family->c_str(), nullptr, d.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        d.size * scale, L"", &f)))
            continue;
        if (d.kind == 2) {
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        } else if (d.kind == 3) {
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            ComPtr<IDWriteTextLayout> probe;  // monospace advance, for tab stops and horizontal scrolling
            if (SUCCEEDED(dw->CreateTextLayout(L"0000000000", 10, f.Get(), 10000.f, 100.f, &probe))) {
                DWRITE_TEXT_METRICS m{};
                probe->GetMetrics(&m);
                codeCharW = std::max(1.f, m.widthIncludingTrailingWhitespace / 10.f);
                f->SetIncrementalTabStop(codeCharW * 4.f);
            }
        } else {
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            if (d.kind == 0) {
                DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
                ComPtr<IDWriteInlineObject> ellipsis;
                dw->CreateEllipsisTrimmingSign(f.Get(), &ellipsis);
                f->SetTrimming(&trim, ellipsis.Get());
            } else {
                f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }
        fmt[(int)d.f] = f;
    }
}

// ---------------------------------------------------------------- primitives

void Gfx::Fill(const RectF& r, const Color& c) {
    if (r.w <= 0 || r.h <= 0 || c.a <= 0) return;
    ctx->FillRectangle(r.D2D(), B(c));
}

void Gfx::FillR(const RectF& r, float rad, const Color& c) {
    if (r.w <= 0 || r.h <= 0 || c.a <= 0) return;
    ctx->FillRoundedRectangle(D2D1::RoundedRect(r.D2D(), rad, rad), B(c));
}

void Gfx::StrokeR(const RectF& r, float rad, const Color& c, float w, ID2D1StrokeStyle* st) {
    if (r.w <= 0 || r.h <= 0) return;
    RectF q = r.Inset(w * 0.5f, w * 0.5f);
    ctx->DrawRoundedRectangle(D2D1::RoundedRect(q.D2D(), rad, rad), B(c), w, st);
}

void Gfx::Line(float x0, float y0, float x1, float y1, const Color& c, float w, ID2D1StrokeStyle* st) {
    ctx->DrawLine({ x0, y0 }, { x1, y1 }, B(c), w, st);
}

void Gfx::HLine(float x0, float x1, float y, const Color& c) {
    Fill({ x0, std::floor(y), x1 - x0, 1.f }, c);
}

void Gfx::VLine(float x, float y0, float y1, const Color& c) {
    Fill({ std::floor(x), y0, 1.f, y1 - y0 }, c);
}

void Gfx::Circle(float cx, float cy, float rad, const Color& c) {
    ctx->FillEllipse(D2D1::Ellipse({ cx, cy }, rad, rad), B(c));
}

void Gfx::CircleS(float cx, float cy, float rad, const Color& c, float w, ID2D1StrokeStyle* st) {
    ctx->DrawEllipse(D2D1::Ellipse({ cx, cy }, rad, rad), B(c), w, st);
}

void Gfx::Text(std::wstring_view s, const RectF& r, F f, const Color& c, DWRITE_TEXT_ALIGNMENT al) {
    if (s.empty() || r.w <= 1) return;
    IDWriteTextFormat* fm = fmt[(int)f].Get();
    if (!fm) return;
    fm->SetTextAlignment(al);
    ctx->DrawText(s.data(), (UINT32)s.size(), fm, r.D2D(), B(c), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

void Gfx::Glyph(wchar_t g, const RectF& r, F f, const Color& c) {
    IDWriteTextFormat* fm = fmt[(int)f].Get();
    if (!fm) return;
    fm->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    ctx->DrawText(&g, 1, fm, r.D2D(), B(c), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void Gfx::GlyphAt(wchar_t g, float cx, float cy, F f, const Color& c) {
    float s = 40.f * scale;
    Glyph(g, { cx - s, cy - s, 2 * s, 2 * s }, f, c);
}

float Gfx::Measure(std::wstring_view s, F f) {
    if (s.empty()) return 0;
    ComPtr<IDWriteTextLayout> l;
    if (FAILED(dw->CreateTextLayout(s.data(), (UINT32)s.size(), fmt[(int)f].Get(), 100000.f, 1000.f, &l))) return 0;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

ComPtr<IDWriteTextLayout> Gfx::Layout(std::wstring_view s, F f, float maxW, float maxH) {
    ComPtr<IDWriteTextLayout> l;
    IDWriteTextFormat* fm = fmt[(int)f].Get();
    fm->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    dw->CreateTextLayout(s.data(), (UINT32)s.size(), fm, std::max(1.f, maxW), std::max(1.f, maxH), &l);
    return l;
}

ID2D1LinearGradientBrush* Gfx::HGrad(int key, const Color& c0, const Color& c1, float x0, float x1) {
    ComPtr<ID2D1LinearGradientBrush>& b = grads[key];
    if (!b) {
        const D2D1_GRADIENT_STOP stops[2] = { { 0.f, c0 }, { 1.f, c1 } };
        ComPtr<ID2D1GradientStopCollection> coll;
        ctx->CreateGradientStopCollection(stops, 2, &coll);
        ctx->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ x0, 0 }, { x1, 0 }), coll.Get(), &b);
    }
    b->SetStartPoint({ x0, 0 });
    b->SetEndPoint({ x1, 0 });
    b->SetOpacity(1.f);
    return b.Get();
}

ComPtr<ID2D1PathGeometry> Gfx::Path() {
    ComPtr<ID2D1PathGeometry> p;
    d2f->CreatePathGeometry(&p);
    return p;
}
