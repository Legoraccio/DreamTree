#pragma once

// Windows interface: system headers and the small types shared by its files. The shared core (src\core)
// never includes this file.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite_1.h>
#include <dcomp.h>
#include <wrl/client.h>

#include "base.h"

using Microsoft::WRL::ComPtr;
using Color = D2D1_COLOR_F;

struct RectF {
    float x = 0, y = 0, w = 0, h = 0;
    float r() const { return x + w; }
    float b() const { return y + h; }
    float cx() const { return x + w * 0.5f; }
    float cy() const { return y + h * 0.5f; }
    bool Contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    D2D1_RECT_F D2D() const { return D2D1::RectF(x, y, x + w, y + h); }
    RectF Inset(float dx, float dy) const { return { x + dx, y + dy, w - 2 * dx, h - 2 * dy }; }
    bool Empty() const { return w <= 0 || h <= 0; }
};

// Messages posted from worker threads to the UI thread.
constexpr UINT WM_APP_RUN = WM_APP + 1;  // lParam = std::function<void()>*

void CopyToClipboard(HWND h, const std::wstring& s);
