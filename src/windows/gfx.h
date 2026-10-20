#pragma once
#include "common.h"

// Text styles. Sizes are in DIPs and scaled by Gfx::SetScale (everything is drawn in physical pixels).
enum class F : int {
    UI, UISemi, Small, SmallSemi, Tiny, TinySemi, Mono, MonoSemi, MonoSmall,
    Nav, RowTitle, Title, Huge, Repo, PageTitle,
    Icon10, Icon12, Icon14, Icon16, Icon20, Icon28,
    Avatar,
    Body, BodyTitle, Caption,  // wrapping text: 13, 18 and 12 DIP
    Code,  // monospace source text: no trimming, 4-column tab stops
    Count
};

// Direct2D renderer on a DirectComposition swap chain (premultiplied alpha, so the
// Windows 11 Mica backdrop shows through every pixel that is not fully opaque).
class Gfx {
public:
    bool Init(HWND hwnd, UINT w, UINT h);
    void Resize(UINT w, UINT h);
    void SetScale(float s);
    bool Begin();
    void End();

    ID2D1DeviceContext* dc() const { return ctx.Get(); }
    IDWriteFactory1* dwrite() const { return dw.Get(); }
    ID2D1Factory1* factory() const { return d2f.Get(); }
    IDWriteTextFormat* Fmt(F f) const { return fmt[(int)f].Get(); }
    UINT Width() const { return width; }
    UINT Height() const { return height; }
    float scale = 1.f;
    float codeCharW = 7.f;  // advance of one F::Code character, pixels

    ID2D1SolidColorBrush* B(const Color& c) { brush->SetColor(c); return brush.Get(); }
    ID2D1SolidColorBrush* B2(const Color& c) { brush2->SetColor(c); return brush2.Get(); }
    ID2D1StrokeStyle* Dash() const { return dash.Get(); }
    ID2D1StrokeStyle* RoundCap() const { return roundCap.Get(); }

    void Fill(const RectF& r, const Color& c);
    void FillR(const RectF& r, float rad, const Color& c);
    void StrokeR(const RectF& r, float rad, const Color& c, float w = 1.f, ID2D1StrokeStyle* st = nullptr);
    void Line(float x0, float y0, float x1, float y1, const Color& c, float w = 1.f, ID2D1StrokeStyle* st = nullptr);
    void HLine(float x0, float x1, float y, const Color& c);  // crisp 1px
    void VLine(float x, float y0, float y1, const Color& c);  // crisp 1px
    void Circle(float cx, float cy, float rad, const Color& c);
    void CircleS(float cx, float cy, float rad, const Color& c, float w, ID2D1StrokeStyle* st = nullptr);
    void Text(std::wstring_view s, const RectF& r, F f, const Color& c,
              DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING);
    void Glyph(wchar_t g, const RectF& r, F f, const Color& c);
    void GlyphAt(wchar_t g, float cx, float cy, F f, const Color& c);
    float Measure(std::wstring_view s, F f);
    ComPtr<IDWriteTextLayout> Layout(std::wstring_view s, F f, float maxW, float maxH = 100000.f);
    void PushClip(const RectF& r) { ctx->PushAxisAlignedClip(r.D2D(), D2D1_ANTIALIAS_MODE_ALIASED); }
    void PopClip() { ctx->PopAxisAlignedClip(); }
    ComPtr<ID2D1PathGeometry> Path();
    // Horizontal two-stop gradient brush, cached per key (stop colors must be constant for a key).
    ID2D1LinearGradientBrush* HGrad(int key, const Color& c0, const Color& c1, float x0, float x1);
    void ClearGradients() { grads.clear(); }  // after the colors of the keys changed (theme switch)

private:
    bool CreateDevice();
    void CreateTarget();
    void ReleaseDevice();
    void PickFonts();
    void CreateFormats();

    HWND hwnd = nullptr;
    UINT width = 1, height = 1;
    ComPtr<ID3D11Device> d3d;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> dtarget;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<ID2D1Factory1> d2f;
    ComPtr<ID2D1Device> d2dev;
    ComPtr<ID2D1DeviceContext> ctx;
    ComPtr<ID2D1Bitmap1> target;
    ComPtr<IDWriteFactory1> dw;
    ComPtr<ID2D1SolidColorBrush> brush, brush2;
    ComPtr<ID2D1StrokeStyle> dash, roundCap;
    std::unordered_map<int, ComPtr<ID2D1LinearGradientBrush>> grads;
    ComPtr<IDWriteTextFormat> fmt[(int)F::Count];
    std::wstring fontUI, fontDisplay, fontMono, fontIcons;
};
