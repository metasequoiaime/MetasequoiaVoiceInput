#include "wave_overlay.h"

#include <d2d1.h>
#include <d2d1helper.h>
#include <algorithm>
#include <cmath>

namespace
{
constexpr wchar_t kClassName[] = L"MviWaveOverlayWindow";
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kTimerMs = 16;
constexpr int kWidth = 220;
constexpr int kHeight = 64;
constexpr int kBarCount = 30;
constexpr float kDotRadius = 1.6f;
constexpr float kMaxHalfHeight = 27.0f;

template <typename T> void safe_release(T **obj)
{
    if (*obj)
    {
        (*obj)->Release();
        *obj = nullptr;
    }
}
} // namespace

WaveOverlay::WaveOverlay()
{
}

WaveOverlay::~WaveOverlay()
{
    shutdown();
}

bool WaveOverlay::init(HINSTANCE instance)
{
    if (hwnd_)
    {
        return true;
    }

    instance_ = instance;

    const HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory_);
    if (FAILED(hr))
    {
        return false;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = WaveOverlay::wnd_proc;
    wc.hInstance = instance_;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    wc.style = CS_HREDRAW | CS_VREDRAW;

    const ATOM atom = RegisterClassW(&wc);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        shutdown();
        return false;
    }

    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClassName, L"", WS_POPUP, 120, 120, kWidth, kHeight, nullptr, nullptr, instance_, this);

    if (!hwnd_)
    {
        shutdown();
        return false;
    }

    return true;
}

void WaveOverlay::shutdown()
{
    if (hwnd_)
    {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    release_render_target();
    safe_release(&factory_);
}

void WaveOverlay::show()
{
    if (hwnd_)
    {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd_);
    }
}

void WaveOverlay::hide()
{
    if (hwnd_)
    {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void WaveOverlay::set_listening(bool listening)
{
    listening_.store(listening);
}

void WaveOverlay::set_input_level(float level)
{
    const float clamped = std::max(0.0f, std::min(1.0f, level));
    input_level_.store(clamped);
}

LRESULT CALLBACK WaveOverlay::wnd_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    WaveOverlay *self = reinterpret_cast<WaveOverlay *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE)
    {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
        self = reinterpret_cast<WaveOverlay *>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    if (!self)
    {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    return self->handle_message(hwnd, message, wParam, lParam);
}

LRESULT WaveOverlay::handle_message(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        SetTimer(hwnd, kTimerId, kTimerMs, nullptr);
        return 0;
    case WM_TIMER:
        if (wParam == kTimerId)
        {
            update_wave_levels();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_SIZE:
        if (render_target_)
        {
            render_target_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        }
        update_dpi_scale();
        return 0;
    case WM_DPICHANGED:
        update_dpi_scale();
        return 0;
    case WM_PAINT:
    case WM_DISPLAYCHANGE: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        draw();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, kTimerId);
        release_render_target();
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

bool WaveOverlay::ensure_render_target()
{
    if (render_target_)
    {
        return true;
    }

    RECT rc{};
    GetClientRect(hwnd_, &rc);

    D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT>(rc.right - rc.left), static_cast<UINT>(rc.bottom - rc.top));
    const HRESULT hr = factory_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(), D2D1::HwndRenderTargetProperties(hwnd_, size), &render_target_);

    if (FAILED(hr))
    {
        return false;
    }

    if (FAILED(render_target_->CreateSolidColorBrush(D2D1::ColorF(0.07f, 0.08f, 0.10f, 0.92f), &bg_brush_)))
    {
        release_render_target();
        return false;
    }

    if (FAILED(render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &bar_brush_)))
    {
        release_render_target();
        return false;
    }

    update_dpi_scale();
    return true;
}

void WaveOverlay::release_render_target()
{
    safe_release(&bar_brush_);
    safe_release(&bg_brush_);
    safe_release(&render_target_);
}

void WaveOverlay::update_dpi_scale()
{
    if (!hwnd_)
    {
        return;
    }

    dpi_ = GetDpiForWindow(hwnd_);
    scale_x_ = static_cast<float>(dpi_) / 96.0f;
    scale_y_ = static_cast<float>(dpi_) / 96.0f;
}

void WaveOverlay::update_wave_levels()
{
    const bool listening = listening_.load();
    const float level = listening ? input_level_.load() : 0.0f;

    for (int i = 0; i < kBarCount; ++i)
    {
        const float position = std::abs((static_cast<float>(i) - (kBarCount - 1) * 0.5f) / ((kBarCount - 1) * 0.5f));
        const float shape = 1.0f - position;
        const float target = level * (0.45f + 0.55f * shape);

        const float attack = 0.35f;
        const float decay = 0.08f;
        const float smooth = target > levels_[i] ? attack : decay;
        levels_[i] = levels_[i] * (1.0f - smooth) + target * smooth;
    }
}

void WaveOverlay::draw()
{
    if (!ensure_render_target())
    {
        return;
    }

    RECT rc{};
    GetClientRect(hwnd_, &rc);

    const float w = static_cast<float>(rc.right - rc.left) / scale_x_;
    const float h = static_cast<float>(rc.bottom - rc.top) / scale_y_;
    const float center_y = h * 0.5f;
    const float step = w / static_cast<float>(kBarCount);
    const float bar_width = std::max(2.0f, step * 0.45f);

    render_target_->BeginDraw();
    render_target_->SetTransform(D2D1::Matrix3x2F::Identity());

    render_target_->FillRectangle(D2D1::RectF(0.0f, 0.0f, w, h), bg_brush_);

    for (int i = 0; i < kBarCount; ++i)
    {
        const float x = (i + 0.5f) * step;
        const float half = kDotRadius + levels_[i] * (kMaxHalfHeight - kDotRadius);
        if (levels_[i] < 0.06f)
        {
            render_target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, center_y), kDotRadius, kDotRadius), bar_brush_);
        }
        else
        {
            const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(x - bar_width * 0.5f, center_y - half, x + bar_width * 0.5f, center_y + half), bar_width * 0.5f, bar_width * 0.5f);
            render_target_->FillRoundedRectangle(rr, bar_brush_);
        }
    }

    const HRESULT hr = render_target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
    {
        release_render_target();
    }
}
