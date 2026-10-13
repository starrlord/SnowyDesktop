#include "LightsHud.h"

#include "Lights.h"

#include <shellscalingapi.h>
#include <wincodec.h>
#include <windowsx.h>

#include <cstdio>

namespace {

constexpr wchar_t kClass[] = L"snowystarrLightsHud";

constexpr float kPillH = 60.0f;      // DIPs
constexpr float kShadow = 18.0f;     // room around the pill for its shadow
constexpr float kPadLeft = 20.0f;    // pill edge to the bulb
constexpr float kPadRight = 13.0f;   // last button to the pill edge (the buttons are rounded like it)
constexpr float kIconW = 26.0f;
constexpr float kChipH = 30.0f;
constexpr float kButtonH = 34.0f;
constexpr float kBottomGap = 28.0f;  // work area's bottom (the taskbar) to the pill
constexpr float kSideGap = 12.0f;    // the pill keeps this far inside the work area
constexpr float kNearDip = 40.0f;    // the pointer this close while the user works there: step aside
constexpr float kHomeDip = 140.0f;   // ... and once it is this far from home, go back
constexpr float kGlideTau = 0.045f;  // s: how quickly it glides aside
constexpr UINT_PTR kTimerGlide = 1;

constexpr wchar_t kLine1[] = L"Click to place hooks · Right-click ends a strand · Scroll: more or less sag";
constexpr wchar_t kLine2[] = L"Drag a hook to move it · Delete removes · Ctrl+Z undo · Esc when done";
constexpr wchar_t kClearText[] = L"Clear all";
constexpr wchar_t kDoneText[] = L"Done";

// The bulb in the corner, lit warm.
constexpr D2D1_COLOR_F kBulbColor = {1.0f, 0.70f, 0.26f, 1.0f};

D2D1_COLOR_F Rgb(uint32_t rgb, float a = 1.0f) { return D2D1::ColorF(rgb, a); }

D2D1_COLOR_F Mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    return {Lerp(a.r, b.r, t), Lerp(a.g, b.g, t), Lerp(a.b, b.b, t), Lerp(a.a, b.a, t)};
}

D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) {
    c.a = a;
    return c;
}

float Luma(const D2D1_COLOR_F& c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

D2D1_RECT_F Inflate(const D2D1_RECT_F& r, float d) { return {r.left - d, r.top - d, r.right + d, r.bottom + d}; }

// The accent as Windows uses it on dark surfaces (as the settings window does).
D2D1_COLOR_F DarkAccent() {
    DWORD abgr = 0, size = sizeof(abgr);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD,
                     nullptr, &abgr, &size) == ERROR_SUCCESS) {
        const D2D1_COLOR_F c{(abgr & 0xFF) / 255.0f, ((abgr >> 8) & 0xFF) / 255.0f, ((abgr >> 16) & 0xFF) / 255.0f,
                             1.0f};
        return Mix(c, {1, 1, 1, 1}, 0.45f);
    }
    return Rgb(0x60CDFF);
}

float TextWidth(IDWriteFactory* dwrite, IDWriteTextFormat* fmt, const wchar_t* text) {
    ComPtr<IDWriteTextLayout> layout;
    if (!fmt || FAILED(dwrite->CreateTextLayout(text, static_cast<UINT32>(wcslen(text)), fmt, 4000.0f, 100.0f, &layout)))
        return 8.0f * static_cast<float>(wcslen(text));
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return std::ceil(m.widthIncludingTrailingWhitespace);
}

// What the chip says: the sag of the span at hand, else how many strands there are.
std::wstring ChipText(const LightsHud::State& s) {
    wchar_t buf[48];
    if (s.sag >= 0.0f) swprintf_s(buf, L"Sag %ld%%", std::lround(s.sag * 100.0f));
    else if (s.strands == 0) swprintf_s(buf, L"No strands");
    else swprintf_s(buf, s.strands == 1 ? L"%d strand" : L"%d strands", s.strands);
    return buf;
}

}  // namespace

LightsHud::LightsHud(HINSTANCE instance, Events events) : instance_(instance), events_(std::move(events)) {}

LightsHud::~LightsHud() {
    if (hwnd_) DestroyWindow(hwnd_);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

HRESULT LightsHud::CreateText(Text& out) {
    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                     reinterpret_cast<IUnknown**>(out.dwrite.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) return hr;
    // Windows 11's UI font, falling back to Segoe UI on Windows 10.
    const wchar_t* family = L"Segoe UI";
    ComPtr<IDWriteFontCollection> fonts;
    if (SUCCEEDED(out.dwrite->GetSystemFontCollection(&fonts, FALSE))) {
        UINT32 index;
        BOOL exists = FALSE;
        if (SUCCEEDED(fonts->FindFamilyName(L"Segoe UI Variable Text", &index, &exists)) && exists)
            family = L"Segoe UI Variable Text";
    }
    auto make = [&](ComPtr<IDWriteTextFormat>& fmt, float size, DWRITE_FONT_WEIGHT weight,
                    DWRITE_TEXT_ALIGNMENT align) {
        if (FAILED(hr)) return;
        hr = out.dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                          size, L"en-us", &fmt);
        if (FAILED(hr)) return;
        fmt->SetTextAlignment(align);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    };
    make(out.body, 13.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    make(out.note, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    make(out.button, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    return hr;
}

// The pill sized to its text: bulb, two lines of help, the chip, Clear all and Done.
LightsHud::Layout LightsHud::MakeLayout(const Text& text) {
    IDWriteFactory* dw = text.dwrite.Get();
    const float textW = std::max(TextWidth(dw, text.body.Get(), kLine1), TextWidth(dw, text.note.Get(), kLine2)) + 2.0f;
    // The chip is as wide as its widest text, so the pill keeps its size as it changes.
    const float chipW = std::max(TextWidth(dw, text.button.Get(), L"Sag 40%"),
                                 TextWidth(dw, text.button.Get(), L"No strands")) + 26.0f;
    const float clearW = std::max(84.0f, TextWidth(dw, text.button.Get(), kClearText) + 32.0f);
    const float doneW = std::max(80.0f, TextWidth(dw, text.button.Get(), kDoneText) + 44.0f);

    Layout l;
    const float top = kShadow, cy = kShadow + 0.5f * kPillH;
    float x = kShadow + kPadLeft;
    l.icon = {x, top + 8.0f, x + kIconW, top + kPillH - 8.0f};
    x = l.icon.right + 14.0f;
    l.line1 = {x, cy - 18.0f, x + textW, cy};
    l.line2 = {x, cy, x + textW, cy + 16.0f};
    x = l.line1.right + 18.0f;
    l.chip = {x, cy - 0.5f * kChipH, x + chipW, cy + 0.5f * kChipH};
    x = l.chip.right + 14.0f;
    l.clear = {x, cy - 0.5f * kButtonH, x + clearW, cy + 0.5f * kButtonH};
    x = l.clear.right + 8.0f;
    l.done = {x, cy - 0.5f * kButtonH, x + doneW, cy + 0.5f * kButtonH};
    l.pill = {kShadow, top, l.done.right + kPadRight, top + kPillH};
    l.width = l.pill.right + kShadow;
    l.height = l.pill.bottom + kShadow;
    return l;
}

void LightsHud::Draw(ID2D1RenderTarget* rt, const Text& text, const Layout& l, const State& state, D2D1_COLOR_F accent,
                     int hot, int pressed) {
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0, 0.0f), &brush))) return;
    auto fill = [&](const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c) {
        brush->SetColor(c);
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.Get());
    };
    auto stroke = [&](const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c) {
        brush->SetColor(c);
        const D2D1_RECT_F in = Inflate(r, -0.5f);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(in, radius - 0.5f, radius - 0.5f), brush.Get(), 1.0f);
    };
    auto label = [&](const wchar_t* s, const D2D1_RECT_F& r, IDWriteTextFormat* fmt, D2D1_COLOR_F c) {
        if (!fmt) return;
        brush->SetColor(c);
        rt->DrawText(s, static_cast<UINT32>(wcslen(s)), fmt, r, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    const float radius = 0.5f * kPillH;

    // A soft shadow: rings growing outwards, each adding a little, so it fades smoothly.
    constexpr int kRings = 14;
    for (int i = 0; i < kRings; ++i) {
        const float grow = static_cast<float>(kRings - i) * 1.1f - 3.0f;
        D2D1_RECT_F r = Inflate(l.pill, grow);
        r.top += 4.0f;
        r.bottom += 4.0f;
        fill(r, radius + grow, D2D1::ColorF(0, 0.035f));
    }
    // The pill: near black, a faint light rim so it reads on a dark desktop.
    fill(l.pill, radius, Rgb(0x1C1C1E, 0.97f));
    stroke(l.pill, radius, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f));

    // A lit bulb on a bit of wire, glowing.
    const float bx = 0.5f * (l.icon.left + l.icon.right);
    const float bulbLen = (l.icon.bottom - l.icon.top) - 9.0f;
    const D2D1_POINT_2F anchor = {bx, l.icon.top + 5.0f};
    {
        ComPtr<ID2D1GradientStopCollection> stops;
        const D2D1_GRADIENT_STOP s[] = {{0.0f, WithAlpha(kBulbColor, 0.42f)},
                                        {0.45f, WithAlpha(kBulbColor, 0.16f)},
                                        {1.0f, WithAlpha(kBulbColor, 0.0f)}};
        ComPtr<ID2D1RadialGradientBrush> glow;
        if (SUCCEEDED(rt->CreateGradientStopCollection(s, 3, &stops)) &&
            SUCCEEDED(rt->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties({bx, anchor.y + 0.6f * bulbLen}, {0, 0}, 20.0f, 22.0f),
                stops.Get(), &glow)))
            rt->FillEllipse(D2D1::Ellipse({bx, anchor.y + 0.6f * bulbLen}, 20.0f, 22.0f), glow.Get());
    }
    {
        ComPtr<ID2D1PathGeometry> wire;
        ComPtr<ID2D1Factory> factory;
        rt->GetFactory(&factory);
        ComPtr<ID2D1GeometrySink> sink;
        if (factory && SUCCEEDED(factory->CreatePathGeometry(&wire)) && SUCCEEDED(wire->Open(&sink))) {
            sink->BeginFigure({l.icon.left - 5.0f, anchor.y - 2.5f}, D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddQuadraticBezier({{bx, anchor.y + 4.5f}, {l.icon.right + 6.0f, anchor.y - 2.5f}});
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            if (SUCCEEDED(sink->Close())) {
                brush->SetColor(Rgb(0x3F8A5A));
                rt->DrawGeometry(wire.Get(), brush.Get(), 1.6f);
            }
        }
    }
    Lights::DrawIcon(rt, state.bulb, anchor, bulbLen, kBulbColor, true);

    label(kLine1, l.line1, text.body.Get(), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f));
    label(kLine2, l.line2, text.note.Get(), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.62f));

    // The chip: the sag in the accent while there is a span at hand.
    const std::wstring chip = ChipText(state);
    const float chipR = 0.5f * kChipH;
    if (state.sag >= 0.0f) {
        fill(l.chip, chipR, WithAlpha(accent, 0.20f));
        stroke(l.chip, chipR, WithAlpha(accent, 0.45f));
        label(chip.c_str(), l.chip, text.button.Get(), Mix(accent, {1, 1, 1, 1}, 0.35f));
    } else {
        fill(l.chip, chipR, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f));
        label(chip.c_str(), l.chip, text.button.Get(), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.70f));
    }

    // Clear all, quiet; Done, in the accent.
    const float buttonR = 0.5f * kButtonH;
    const float clearA = pressed == kClear ? 0.04f : hot == kClear ? 0.14f : 0.08f;
    fill(l.clear, buttonR, D2D1::ColorF(1.0f, 1.0f, 1.0f, clearA));
    stroke(l.clear, buttonR, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f));
    label(kClearText, l.clear, text.button.Get(), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.92f));
    D2D1_COLOR_F done = accent;
    if (pressed == kDone) done = Mix(done, {0, 0, 0, 1}, 0.18f);
    else if (hot == kDone) done = Mix(done, {1, 1, 1, 1}, 0.12f);
    fill(l.done, buttonR, done);
    label(kDoneText, l.done, text.button.Get(), Luma(accent) > 0.5f ? Rgb(0x000000) : Rgb(0xFFFFFF));
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

bool LightsHud::Create(HMONITOR monitor, const State& state) {
    if (hwnd_) return true;
    state_ = state;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = instance_;
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        registered = true;
    }
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf())) ||
        FAILED(CreateText(text_)))
        return false;
    layout_ = MakeLayout(text_);
    accent_ = DarkAccent();
    // Topmost and layered (its shape is its pixels' alpha); no taskbar button.
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClass, L"Hang lights", WS_POPUP, 0, 0,
                            0, 0, nullptr, nullptr, instance_, this);
    if (!hwnd_) return false;
    PlaceAt(monitor);
    // Activated, so the keys come here.
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    return true;
}

void LightsHud::SetState(const State& state) {
    if (state == state_) return;
    state_ = state;
    Render();
}

void LightsHud::SetMonitor(HMONITOR monitor) {
    if (hwnd_ && monitor && monitor != monitor_) PlaceAt(monitor);
}

void LightsHud::Raise() {
    if (hwnd_) SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// At home on `monitor`, at its scale: its DPI's, smaller if the pill would not fit across the work area.
void LightsHud::PlaceAt(HMONITOR monitor) {
    monitor_ = monitor;
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    const float dpiScale = static_cast<float>(dx) / 96.0f;
    const float room = static_cast<float>(mi.rcWork.right - mi.rcWork.left) - 2.0f * kSideGap * dpiScale;
    scale_ = std::max(0.5f, std::min(dpiScale, room / (layout_.width - 2.0f * kShadow)));
    size_ = {static_cast<LONG>(std::ceil(layout_.width * scale_)), static_cast<LONG>(std::ceil(layout_.height * scale_))};
    gliding_ = false;
    KillTimer(hwnd_, kTimerGlide);
    aside_ = false;
    pos_ = target_ = HomePos();
    Render();
    Raise();
}

// Bottom centre of the work area, the taskbar below it; never up in the top third, where the lights hang.
POINT LightsHud::HomePos() const {
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(monitor_, &mi)) return pos_;
    const RECT& work = mi.rcWork;
    const float s = scale_;
    const float pillBottom = static_cast<float>(work.bottom) - kBottomGap * s;
    float top = pillBottom - (kPillH + kShadow) * s;
    const float lowest = static_cast<float>(mi.rcMonitor.top) +
                         static_cast<float>(mi.rcMonitor.bottom - mi.rcMonitor.top) / 3.0f - kShadow * s;
    top = std::max(top, lowest);
    const float left = 0.5f * static_cast<float>(work.left + work.right) - 0.5f * static_cast<float>(size_.cx);
    return {std::lround(left), std::lround(top)};
}

// The pill's rectangle on screen with the window at `pos`.
RECT LightsHud::PillAt(POINT pos) const {
    const float s = scale_;
    return {pos.x + std::lround(layout_.pill.left * s), pos.y + std::lround(layout_.pill.top * s),
            pos.x + std::lround(layout_.pill.right * s), pos.y + std::lround(layout_.pill.bottom * s)};
}

void LightsHud::Avoid(POINT p, bool working) {
    if (!hwnd_) return;
    const float s = scale_;
    auto within = [&](POINT pos, float dip) {
        RECT r = PillAt(pos);
        InflateRect(&r, std::lround(dip * s), std::lround(dip * s));
        return PtInRect(&r, p) != FALSE;
    };
    const POINT home = HomePos();
    if (aside_ && !within(home, kHomeDip)) {
        // The pointer has gone: back home.
        aside_ = false;
        MoveTo(home, true);
        return;
    }
    // Only in the way while the user works near it: otherwise its buttons are there to be clicked.
    if (!working || !within(target_, kNearDip)) return;

    // The nearest spot clear of the pointer: along the bottom to one side, else up above it
    // (still in the lower part of the screen).
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(monitor_, &mi)) return;
    const RECT& work = mi.rcWork;
    const LONG clearance = std::lround(1.6f * kNearDip * s);
    const LONG inset = std::lround(kSideGap * s);
    const RECT pill = PillAt(home);
    const LONG pillW = pill.right - pill.left, pillH = pill.bottom - pill.top;
    const LONG offX = pill.left - home.x, offY = pill.top - home.y;  // window to pill
    POINT best = target_;
    LONG bestCost = LONG_MAX;
    auto consider = [&](LONG pillLeft, LONG pillTop) {
        pillLeft = Clamp(pillLeft, work.left + inset, std::max(work.left + inset, work.right - inset - pillW));
        const LONG lowest = mi.rcMonitor.top + (mi.rcMonitor.bottom - mi.rcMonitor.top) / 3;
        pillTop = std::max(pillTop, lowest);
        const POINT pos = {pillLeft - offX, pillTop - offY};
        if (within(pos, kNearDip)) return;
        const LONG cost = std::abs(pos.x - home.x) + 2 * std::abs(pos.y - home.y);  // rather sideways than up
        if (cost < bestCost) {
            bestCost = cost;
            best = pos;
        }
    };
    consider(p.x - clearance - pillW, pill.top);  // left of the pointer
    consider(p.x + clearance, pill.top);          // right of it
    consider(pill.left, p.y - clearance - pillH); // above it
    if (bestCost == LONG_MAX) return;
    aside_ = true;
    MoveTo(best, true);
}

void LightsHud::MoveTo(POINT pos, bool glide) {
    target_ = pos;
    if (!glide) {
        gliding_ = false;
        KillTimer(hwnd_, kTimerGlide);
        pos_ = pos;
        SetWindowPos(hwnd_, nullptr, pos.x, pos.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
    if (!gliding_) {
        gliding_ = true;
        glideX_ = static_cast<float>(pos_.x);
        glideY_ = static_cast<float>(pos_.y);
        glideLast_ = QpcSeconds();
        SetTimer(hwnd_, kTimerGlide, 10, nullptr);
    }
}

// One step of the glide: eases out towards the target.
void LightsHud::Glide() {
    const double now = QpcSeconds();
    const float k = ApproachFactor(static_cast<float>(std::min(now - glideLast_, 0.1)), kGlideTau);
    glideLast_ = now;
    glideX_ += (static_cast<float>(target_.x) - glideX_) * k;
    glideY_ += (static_cast<float>(target_.y) - glideY_) * k;
    if (std::fabs(glideX_ - static_cast<float>(target_.x)) < 0.75f &&
        std::fabs(glideY_ - static_cast<float>(target_.y)) < 0.75f) {
        MoveTo(target_, false);
        return;
    }
    pos_ = {std::lround(glideX_), std::lround(glideY_)};
    SetWindowPos(hwnd_, nullptr, pos_.x, pos_.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Draws it into a bitmap and hands that to the window (its pixels' alpha is its
// shape). Only a finished drawing is handed over: a blank one would leave the
// HUD invisible, and clicks passing through it.
void LightsHud::Render() {
    if (!hwnd_ || size_.cx <= 0 || size_.cy <= 0) return;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size_.cx;
    bi.bmiHeader.biHeight = -size_.cy;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib && mem) {
        HGDIOBJ old = SelectObject(mem, dib);
        const RECT rc = {0, 0, size_.cx, size_.cy};
        bool drawn = false;
        // A lost target (the GPU reset) is made again and drawn with at once.
        for (int attempt = 0; attempt < 2 && !drawn; ++attempt) {
            if (!rt_) {
                const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                    D2D1_RENDER_TARGET_TYPE_DEFAULT,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                if (FAILED(factory_->CreateDCRenderTarget(&props, &rt_))) break;
                // Over a transparent bitmap ClearType has nothing to blend with.
                rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            }
            if (FAILED(rt_->BindDC(mem, &rc))) break;
            rt_->SetDpi(96.0f * scale_, 96.0f * scale_);
            rt_->BeginDraw();
            rt_->Clear(D2D1::ColorF(0, 0.0f));
            Draw(rt_.Get(), text_, layout_, state_, accent_, hot_, pressed_);
            const HRESULT hr = rt_->EndDraw();
            if (hr == D2DERR_RECREATE_TARGET) {
                rt_.Reset();
                continue;
            }
            drawn = SUCCEEDED(hr);
        }
        if (drawn) {
            POINT src = {0, 0};
            BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            UpdateLayeredWindow(hwnd_, screen, &pos_, &size_, mem, &src, 0, &blend, ULW_ALPHA);
        } else {
            // The last good picture stays up, still following a move.
            SetWindowPos(hwnd_, nullptr, pos_.x, pos_.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        SelectObject(mem, old);
    }
    if (dib) DeleteObject(dib);
    if (mem) DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

bool LightsHud::InPill(POINT client) const {
    const float x = static_cast<float>(client.x) / scale_, y = static_cast<float>(client.y) / scale_;
    const D2D1_RECT_F& r = layout_.pill;
    if (x < r.left || x >= r.right || y < r.top || y >= r.bottom) return false;
    // The rounded ends.
    const float radius = 0.5f * kPillH, cy = 0.5f * (r.top + r.bottom);
    const float cx = Clamp(x, r.left + radius, r.right - radius);
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= radius * radius;
}

int LightsHud::HitButton(POINT client) const {
    const D2D1_POINT_2F p = {static_cast<float>(client.x) / scale_, static_cast<float>(client.y) / scale_};
    auto in = [&](const D2D1_RECT_F& r) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; };
    return in(layout_.clear) ? kClear : in(layout_.done) ? kDone : kNone;
}

LRESULT CALLBACK LightsHud::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<LightsHud*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<LightsHud*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (self) self->hwnd_ = nullptr;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self ? self->HandleMessage(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT LightsHud::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST: {
        // The shadow around the pill lets the pointer through to the overlay below.
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &p);
        return InPill(p) ? HTCLIENT : HTTRANSPARENT;
    }
    case WM_MOUSEMOVE: {
        const POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        const int hot = HitButton(p);
        if (hot != hot_) {
            hot_ = hot;
            Render();
        }
        if (events_.pointer) {
            POINT screen = p;
            ClientToScreen(hwnd_, &screen);
            events_.pointer(screen);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (hot_ != kNone) {
            hot_ = kNone;
            Render();
        }
        return 0;
    case WM_LBUTTONDOWN:
        pressed_ = HitButton({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (pressed_ != kNone) {
            SetCapture(hwnd_);
            Render();
        }
        return 0;
    case WM_LBUTTONUP: {
        const int was = pressed_;
        if (was == kNone) return 0;
        pressed_ = kNone;
        ReleaseCapture();
        Render();
        // A button counts when let go over it.
        if (HitButton({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}) != was) return 0;
        if (was == kClear && events_.clearAll) events_.clearAll();
        else if (was == kDone && events_.done) events_.done();
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (pressed_ != kNone) {
            pressed_ = kNone;
            Render();
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (events_.wheel)
            events_.wheel(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / static_cast<float>(WHEEL_DELTA));
        return 0;
    case WM_KEYDOWN:
        if (events_.key) events_.key(static_cast<UINT>(wp));
        return 0;
    case WM_CLOSE:
        // Alt+F4: done, as with the button (App takes the window down).
        if (events_.done) events_.done();
        return 0;
    case WM_TIMER:
        if (wp == kTimerGlide) Glide();
        return 0;
    case WM_DPICHANGED:
        // Placed and drawn for its monitor's DPI already: keep it as it is.
        return 0;
    default:
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

// ---------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------

HRESULT LightsHud::SaveSnapshot(const wchar_t* path, float scale) {
    scale = Clamp(std::isfinite(scale) ? scale : 1.0f, 0.5f, 4.0f);
    Text text;
    HRESULT hr = CreateText(text);
    if (FAILED(hr)) return hr;
    const Layout l = MakeLayout(text);
    const D2D1_COLOR_F accent = DarkAccent();

    // The states to show, one row each.
    struct Row {
        State state;
        int hot, pressed;
    };
    const Row rows[] = {
        {{-1.0f, false, 3, BulbType::C7}, kNone, kNone},     // idle
        {{0.07f, true, 3, BulbType::C9}, kDone, kNone},      // placing, the pointer on Done
        {{0.12f, false, 1, BulbType::Globe}, kClear, kClear},  // a span under the pointer, Clear all held
        {{-1.0f, false, 0, BulbType::MiniLed}, kNone, kNone},  // nothing hung
    };
    constexpr float kMargin = 24.0f;
    const float wDip = l.width + 2.0f * kMargin;
    const float hDip = static_cast<float>(std::size(rows)) * l.height + 2.0f * kMargin;
    const UINT w = static_cast<UINT>(std::ceil(wDip * scale)), h = static_cast<UINT>(std::ceil(hDip * scale));

    ComPtr<IWICImagingFactory> wic;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (FAILED(hr)) return hr;
    ComPtr<IWICBitmap> bitmap;
    hr = wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap);
    if (FAILED(hr)) return hr;
    ComPtr<ID2D1Factory> factory;
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf());
    if (FAILED(hr)) return hr;
    ComPtr<ID2D1RenderTarget> rt;
    hr = factory->CreateWicBitmapRenderTarget(
        bitmap.Get(),
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                     D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                     96.0f * scale, 96.0f * scale),
        &rt);
    if (FAILED(hr)) return hr;
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    rt->BeginDraw();
    // A night-blue desktop under the hanging-mode veil.
    {
        ComPtr<ID2D1GradientStopCollection> stops;
        const D2D1_GRADIENT_STOP s[] = {{0.0f, Rgb(0x16233F)}, {1.0f, Rgb(0x0B1020)}};
        ComPtr<ID2D1LinearGradientBrush> sky;
        if (SUCCEEDED(rt->CreateGradientStopCollection(s, 2, &stops)) &&
            SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {0, hDip}),
                                                    stops.Get(), &sky)))
            rt->FillRectangle(D2D1::RectF(0, 0, wDip, hDip), sky.Get());
        ComPtr<ID2D1SolidColorBrush> veil;
        if (SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(0, 0.28f), &veil)))
            rt->FillRectangle(D2D1::RectF(0, 0, wDip, hDip), veil.Get());
    }
    for (size_t i = 0; i < std::size(rows); ++i) {
        rt->SetTransform(D2D1::Matrix3x2F::Translation(kMargin, kMargin + static_cast<float>(i) * l.height));
        Draw(rt.Get(), text, l, rows[i].state, accent, rows[i].hot, rows[i].pressed);
    }
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    hr = rt->EndDraw();
    if (FAILED(hr)) return hr;

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(hr = wic->CreateStream(&stream))) return hr;
    if (FAILED(hr = stream->InitializeFromFilename(path, GENERIC_WRITE))) return hr;
    if (FAILED(hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) return hr;
    if (FAILED(hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return hr;
    if (FAILED(hr = encoder->CreateNewFrame(&frame, nullptr))) return hr;
    if (FAILED(hr = frame->Initialize(nullptr))) return hr;
    if (FAILED(hr = frame->SetSize(w, h))) return hr;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    if (FAILED(hr = frame->SetPixelFormat(&format))) return hr;
    if (FAILED(hr = frame->WriteSource(bitmap.Get(), nullptr))) return hr;
    if (FAILED(hr = frame->Commit())) return hr;
    return encoder->Commit();
}
