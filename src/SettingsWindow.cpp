#include "SettingsWindow.h"

#include "resource.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <utility>

namespace {

constexpr wchar_t kClass[] = L"snowystarrSettings";
constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
constexpr float kClientW = 720.0f;  // DIPs
constexpr float kClientH = 840.0f;
constexpr float kMargin = 26.0f;

constexpr float kPreviewTop = 16.0f;
constexpr float kPreviewH = 150.0f;
constexpr float kPreviewTaskbar = 22.0f;  // the little taskbar in the preview
constexpr float kPreviewDesk = 1920.0f;   // DIP: the desktop whose drape of lights the preview shows, squeezed

constexpr float kTabsTop = kPreviewTop + kPreviewH + 12.0f;
constexpr float kTabsH = 32.0f;
constexpr float kTabW = 112.0f;                         // each of Snow and Lights
constexpr float kPageTop = kTabsTop + kTabsH + 14.0f;  // where each tab's controls start

constexpr int kTileCols = 10;
constexpr float kTile = 56.0f;
constexpr float kTileGap = 12.0f;

constexpr float kChipH = 28.0f;
constexpr float kChipGap = 8.0f;
constexpr float kChipPad = 13.0f;     // text to edge
constexpr float kChipSwatch = 28.0f;  // left edge to text, past the colour dot

constexpr float kCardH = 60.0f;        // the strands' two layouts
constexpr float kCardInset = 8.0f;     // card edge to its picture
constexpr float kStrandIconW = 64.0f;
constexpr float kStrandIconH = kCardH - 2.0f * kCardInset;
constexpr float kHangW = 124.0f;       // the Hang lights... button

// The bulb tiles: a scallop of wire across the top, the bulb hanging from its lowest point.
constexpr float kWireEnds = 9.0f;  // DIP from the tile's top
constexpr float kWireLow = 15.0f;
constexpr float kWireW = 2.0f;

enum Id : int {
    kIdTile = 100,  // + FlakeKind
    kIdAmount = 200, kIdSpeed, kIdSize, kIdWind, kIdGusts, kIdDrift, kIdTreeAmount, kIdSnowmanAmount,
    kIdLightSpeed, kIdLightSize, kIdLightGlow,
    kIdColor = 300,  // + SnowColor
    kIdPause = 400, kIdMouse, kIdShadow, kIdFullscreen, kIdMonitors, kIdStartup, kIdBehind, kIdTrees, kIdSnowmen,
    kIdLights,
    kIdClear = 500, kIdDefaults, kIdDone, kIdAllFlakes, kIdDefaultFlakes, kIdCredit, kIdHang, kIdClearStrands,
    kIdTab = 600,         // + Tab
    kIdBulb = 700,        // + BulbType
    kIdLightColor = 800,  // + LightColors
    kIdAnimation = 900,   // + LightAnimation
    kIdWire = 1000,       // + WireColor
    kIdLayout = 1100,     // + LightLayout
};

constexpr int kLightColorCount = static_cast<int>(LightColors::Count);
constexpr int kAnimationCount = static_cast<int>(LightAnimation::Count);
constexpr int kWireCount = static_cast<int>(WireColor::Count);
constexpr int kLayoutCount = static_cast<int>(LightLayout::Count);

// Tiles and chips that make one tab stop, the arrow keys moving between them.
struct Group {
    int first, count, cols;
};
constexpr Group kGroups[] = {
    {kIdTile, kFlakeKindCount, kTileCols},
    {kIdTab, 2, 2},
    {kIdBulb, kBulbTypeCount, kBulbTypeCount},
    {kIdLightColor, kLightColorCount, kLightColorCount},
    {kIdAnimation, kAnimationCount, kAnimationCount},
    {kIdWire, kWireCount, kWireCount},
    {kIdLayout, kLayoutCount, kLayoutCount},
};

const Group* GroupOf(int id) {
    for (const Group& g : kGroups)
        if (id >= g.first && id < g.first + g.count) return &g;
    return nullptr;
}

bool SameGroup(int a, int b) {
    const Group* g = GroupOf(a);
    return g && g == GroupOf(b);
}

// UI scale (px per DIP) on a monitor: its DPI scale, reduced when the window
// would not fit the work area (e.g. 1920x1080 at 150%, 1366x768 at 100%).
float FitScale(HMONITOR mon, UINT dpi) {
    RECT nc{};
    AdjustWindowRectExForDpi(&nc, kStyle, FALSE, 0, dpi);
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi)) return static_cast<float>(dpi) / 96.0f;
    const float w = static_cast<float>(mi.rcWork.right - mi.rcWork.left - (nc.right - nc.left));
    const float h = static_cast<float>(mi.rcWork.bottom - mi.rcWork.top - (nc.bottom - nc.top));
    return std::max(0.5f, std::min({static_cast<float>(dpi) / 96.0f, w / kClientW, h / kClientH}));
}

// Outer window size for a UI scale at a DPI.
SIZE WindowSize(float scale, UINT dpi) {
    RECT rc = {0, 0, std::lround(kClientW * scale), std::lround(kClientH * scale)};
    AdjustWindowRectExForDpi(&rc, kStyle, FALSE, 0, dpi);
    return {rc.right - rc.left, rc.bottom - rc.top};
}

RECT WorkArea(HMONITOR mon) {
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi)) SystemParametersInfoW(SPI_GETWORKAREA, 0, &mi.rcWork, 0);
    return mi.rcWork;
}

// Where a window whose DPI changes to `dpi` is headed: the monitor at that DPI
// overlapping `r` most. (During a drag, WM_GETDPISCALEDSIZE arrives while the
// window still sits mostly on the old monitor.)
HMONITOR MonitorAtDpi(const RECT& r, UINT dpi) {
    struct Search {
        RECT r;
        UINT dpi;
        HMONITOR best = nullptr;
        LONGLONG area = -1;
    } s{r, dpi};
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT mr, LPARAM lp) -> BOOL {
            auto& search = *reinterpret_cast<Search*>(lp);
            UINT dx = 0, dy = 0;
            if (FAILED(GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dx, &dy)) || dx != search.dpi) return TRUE;
            RECT i{};
            IntersectRect(&i, mr, &search.r);
            const LONGLONG area = static_cast<LONGLONG>(i.right - i.left) * (i.bottom - i.top);
            if (area > search.area) {
                search.best = m;
                search.area = area;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&s));
    return s.best ? s.best : MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
}

D2D1_COLOR_F Rgb(uint32_t rgb, float a = 1.0f) { return D2D1::ColorF(rgb, a); }

D2D1_COLOR_F FromColorRef(COLORREF c) {
    return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f);
}

COLORREF ToColorRef(const D2D1_COLOR_F& c) {
    auto byte = [](float v) { return static_cast<BYTE>(std::lround(Saturate(v) * 255.0f)); };
    return RGB(byte(c.r), byte(c.g), byte(c.b));
}

D2D1_COLOR_F Mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    return {Lerp(a.r, b.r, t), Lerp(a.g, b.g, t), Lerp(a.b, b.b, t), Lerp(a.a, b.a, t)};
}

D2D1_RECT_F Rect(float x, float y, float w, float h) { return {x, y, x + w, y + h}; }

D2D1_RECT_F Inflate(const D2D1_RECT_F& r, float d) { return {r.left - d, r.top - d, r.right + d, r.bottom + d}; }

bool Contains(const D2D1_RECT_F& r, D2D1_POINT_2F p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

bool SystemDarkMode() {
    DWORD v = 1, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v == 0;
}

D2D1_COLOR_F SystemAccent(bool dark) {
    DWORD abgr = 0, size = sizeof(abgr);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD,
                     nullptr, &abgr, &size) == ERROR_SUCCESS) {
        D2D1_COLOR_F c{(abgr & 0xFF) / 255.0f, ((abgr >> 8) & 0xFF) / 255.0f, ((abgr >> 16) & 0xFF) / 255.0f, 1.0f};
        // Like Windows, use a lighter tint of the accent on dark backgrounds
        // and a deeper one on light backgrounds.
        return dark ? Mix(c, {1, 1, 1, 1}, 0.45f) : Mix(c, {0, 0, 0, 1}, 0.12f);
    }
    return dark ? Rgb(0x60CDFF) : Rgb(0x005FB8);
}

float Luma(const D2D1_COLOR_F& c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

// Descriptions for slider values.
const wchar_t* AmountText(int v) {
    if (v < 20) return L"Flurries";
    if (v < 38) return L"Light";
    if (v < 58) return L"Moderate";
    if (v < 85) return L"Heavy";
    return L"Blizzard";
}
const wchar_t* SpeedText(int v) {
    if (v < 15) return L"Drifting";
    if (v < 38) return L"Slow";
    if (v < 63) return L"Normal";
    if (v < 85) return L"Fast";
    return L"Very fast";
}
const wchar_t* SizeText(int v) {
    if (v < 15) return L"Tiny";
    if (v < 38) return L"Small";
    if (v < 63) return L"Medium";
    if (v < 85) return L"Large";
    return L"Huge";
}
std::wstring WindText(int v) {
    int a = std::abs(v);
    if (a < 6) return L"Calm";
    const wchar_t* name = a < 40 ? L"Breeze" : (a < 75 ? L"Windy" : L"Gale");
    return std::wstring(v < 0 ? L"← " : L"") + name + (v > 0 ? L" →" : L"");
}
const wchar_t* GustText(int v) {
    if (v < 8) return L"Steady";
    if (v < 40) return L"Gentle";
    if (v < 75) return L"Gusty";
    return L"Stormy";
}
const wchar_t* DriftText(int v) {
    if (v <= 0) return L"Off";
    if (v < 40) return L"Dusting";
    if (v < 75) return L"Drifts";
    if (v < 130) return L"Deep";
    return L"Snowbanks";
}
const wchar_t* TreeText(int v) {
    if (v < 20) return L"A few";
    if (v < 50) return L"Some";
    if (v < 85) return L"Lots";
    return L"A forest";
}
const wchar_t* SnowmanText(int v) {
    if (v < 15) return L"One";
    if (v < 50) return L"Some";
    if (v < 85) return L"Lots";
    return L"A crowd";
}
const wchar_t* LightSpeedText(int v) {
    if (v < 15) return L"Very slow";
    if (v < 38) return L"Slow";
    if (v < 63) return L"Normal";
    if (v < 85) return L"Fast";
    return L"Very fast";
}
const wchar_t* GlowText(int v) {
    if (v <= 0) return L"None";
    if (v < 25) return L"Faint";
    if (v < 50) return L"Soft";
    if (v < 80) return L"Bright";
    return L"Dazzling";
}

// Radius at which a kind is drawn in its tile, so the picker hints at the
// relative size of each flake like a field guide.
float TileRadius(FlakeKind k) {
    switch (k) {
    case FlakeKind::Dot: return 6.0f;
    case FlakeKind::Soft: return 11.0f;
    case FlakeKind::Plus: return 11.0f;
    case FlakeKind::Asterisk: return 13.0f;
    case FlakeKind::Sparkle: return 16.0f;
    default: return 21.0f;
    }
}

bool IsClassic(FlakeKind k) {
    return static_cast<int>(k) >= static_cast<int>(FlakeKind::Classic1) &&
           static_cast<int>(k) <= static_cast<int>(FlakeKind::Classic8);
}

// Length (DIP, socket to tip) a bulb is drawn at in its tile: true to the real
// ones' proportions, the small ones a little enlarged so they still read.
float BulbTileLength(BulbType t) {
    switch (t) {
    case BulbType::Mini: return 17.0f;
    case BulbType::MiniLed: return 16.0f;
    case BulbType::WideLed: return 13.0f;
    case BulbType::C5: return 18.0f;
    case BulbType::C6: return 18.0f;
    case BulbType::C7: return 23.0f;
    case BulbType::C9: return 28.0f;
    case BulbType::Globe: return 23.0f;
    case BulbType::Bubble: return 33.0f;
    default: return 24.0f;  // Candle
    }
}

// The lights' colours, for the swatches and the bulb tiles (the strands
// themselves take theirs from LightsArt.cpp). Bulb `i` of a strand.
D2D1_COLOR_F LightColor(LightColors c, COLORREF custom, int i) {
    static constexpr uint32_t kMulti[] = {0xFF3030, 0x2FD55B, 0x3D7BFF, 0xFFC93A, 0xA855F7, 0xFF7F1F, 0x1FC9D6, 0xFF4FA3};
    switch (c) {
    case LightColors::Multicolor: return Rgb(kMulti[i % 8]);
    case LightColors::WarmWhite: return Rgb(0xFFD08A);
    case LightColors::CoolWhite: return Rgb(0xDCEBFF);
    case LightColors::RedGreen: return Rgb(i % 2 ? 0x2FD55B : 0xFF3030);
    case LightColors::BlueWhite: return Rgb(i % 2 ? 0xEAF2FF : 0x3D7BFF);
    default: return FromColorRef(custom);
    }
}

D2D1_COLOR_F WireRgb(WireColor w) {
    switch (w) {
    case WireColor::White: return Rgb(0xE8E8E4);
    case WireColor::Black: return Rgb(0x161616);
    default: return Rgb(0x1B4D2E);
    }
}

const wchar_t* LightColorLabel(LightColors c) { return c == LightColors::Custom ? L"Custom…" : LightColorsName(c); }

const wchar_t* LayoutLabel(LightLayout l) { return l == LightLayout::AcrossTop ? L"Across the top" : L"My strands"; }

// The strands in the layout cards' pictures (kStrandIconW x kStrandIconH DIP):
// hooks as fractions of the picture, and how far each span sags.
struct IconStrand {
    D2D1_POINT_2F hooks[4];
    int count;
    float sag;  // DIP, at each span's middle
};
constexpr IconStrand kIconStrands[] = {
    {{{0.0f, 0.18f}, {0.34f, 0.18f}, {0.67f, 0.18f}, {1.0f, 0.18f}}, 4, 0.17f * kStrandIconH},  // AcrossTop
    {{{0.12f, 0.2f}, {0.5f, 0.4f}, {0.88f, 0.18f}}, 3, 0.26f * kStrandIconH},                  // Custom
};

D2D1_POINT_2F IconHook(const IconStrand& s, int i) { return {s.hooks[i].x * kStrandIconW, s.hooks[i].y * kStrandIconH}; }

// Each span is a quadratic, its control point twice the sag below the chord's middle.
D2D1_POINT_2F IconControl(const IconStrand& s, int span) {
    const D2D1_POINT_2F a = IconHook(s, span - 1), b = IconHook(s, span);
    return {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f + 2.0f * s.sag};
}

// The point `t` of the way along a span (from hook span-1 to hook span).
D2D1_POINT_2F IconSpanPoint(const IconStrand& s, int span, float t) {
    const D2D1_POINT_2F a = IconHook(s, span - 1), b = IconHook(s, span), c = IconControl(s, span);
    const float u = 1.0f - t;
    return {u * u * a.x + 2.0f * u * t * c.x + t * t * b.x, u * u * a.y + 2.0f * u * t * c.y + t * t * b.y};
}

#ifdef SNOWYSTARR_DEV_TOOLS
HRESULT WritePng(IWICImagingFactory* wic, IWICBitmap* bmp, const wchar_t* path) {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    HRESULT hr;
    if (FAILED(hr = wic->CreateStream(&stream))) return hr;
    if (FAILED(hr = stream->InitializeFromFilename(path, GENERIC_WRITE))) return hr;
    if (FAILED(hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) return hr;
    enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    enc->CreateNewFrame(&frame, nullptr);
    frame->Initialize(nullptr);
    hr = frame->WriteSource(bmp, nullptr);
    frame->Commit();
    enc->Commit();
    return hr;
}
#endif

}  // namespace

SettingsWindow::SettingsWindow(HINSTANCE instance, HWND app) : instance_(instance), app_(app) {
    origin_ = QpcSeconds();
    CreateDeviceIndependent();
}

SettingsWindow::~SettingsWindow() {
    Close();
    ReleaseResources();
}

void SettingsWindow::Close() {
    if (hwnd_) DestroyWindow(hwnd_);
}

HRESULT SettingsWindow::CreateDeviceIndependent() {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf());
    if (FAILED(hr)) return hr;
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
    if (FAILED(hr)) return hr;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
    if (FAILED(hr)) return hr;

    // Windows 11's UI font, falling back to Segoe UI on Windows 10.
    const wchar_t* family = L"Segoe UI";
    ComPtr<IDWriteFontCollection> fonts;
    if (SUCCEEDED(dwrite_->GetSystemFontCollection(&fonts, FALSE))) {
        UINT32 index;
        BOOL exists = FALSE;
        if (SUCCEEDED(fonts->FindFamilyName(L"Segoe UI Variable Text", &index, &exists)) && exists)
            family = L"Segoe UI Variable Text";
    }
    auto make = [&](ComPtr<IDWriteTextFormat>& out, float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align) {
        if (FAILED(dwrite_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &out)))
            return;
        out->SetTextAlignment(align);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(out.Get(), &ellipsis)))
            out->SetTrimming(&trim, ellipsis.Get());
    };
    make(fmtTitle_, 20.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    make(fmtSection_, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    make(fmtBody_, 13.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    make(fmtSmall_, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    make(fmtValue_, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    make(fmtButton_, 13.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    make(fmtChip_, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);

    // The Lights tab sizes its chips to their words.
    for (int i = 0; i < kLightColorCount; ++i)
        lightColorW_[i] = Measure(LightColorLabel(static_cast<LightColors>(i)), fmtChip_.Get());
    for (int i = 0; i < kAnimationCount; ++i)
        animationW_[i] = Measure(LightAnimationName(static_cast<LightAnimation>(i)), fmtChip_.Get());
    for (int i = 0; i < kWireCount; ++i) wireW_[i] = Measure(WireColorName(static_cast<WireColor>(i)), fmtChip_.Get());
    showLightsW_ = Measure(L"Show lights", fmtBody_.Get());
    lightsTitleW_ = Measure(L"Holiday lights", fmtSection_.Get());
    strandsTitleW_ = Measure(L"Strands", fmtSection_.Get());

    previewRect_ = Rect(kMargin - 10.0f, kPreviewTop, kClientW - 2.0f * (kMargin - 10.0f), kPreviewH);
    factory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(previewRect_, 8.0f, 8.0f), &previewClip_);

    // The wires in the layout cards' pictures.
    for (int l = 0; l < kLayoutCount; ++l) {
        const IconStrand& strand = kIconStrands[l];
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(factory_->CreatePathGeometry(&strandWires_[l])) || FAILED(strandWires_[l]->Open(&sink))) continue;
        sink->BeginFigure(IconHook(strand, 0), D2D1_FIGURE_BEGIN_HOLLOW);
        for (int i = 1; i < strand.count; ++i) sink->AddQuadraticBezier({IconControl(strand, i), IconHook(strand, i)});
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        if (FAILED(sink->Close())) strandWires_[l].Reset();
    }
    return S_OK;
}

float SettingsWindow::Measure(const wchar_t* s, IDWriteTextFormat* fmt) const {
    ComPtr<IDWriteTextLayout> layout;
    DWRITE_TEXT_METRICS m{};
    if (!dwrite_ || !fmt || !s ||
        FAILED(dwrite_->CreateTextLayout(s, static_cast<UINT32>(wcslen(s)), fmt, 4096.0f, 64.0f, &layout)) ||
        FAILED(layout->GetMetrics(&m)))
        return 0.0f;
    return std::ceil(m.widthIncludingTrailingWhitespace);
}

SettingsWindow::Theme SettingsWindow::MakeTheme(bool dark) {
    Theme t;
    t.dark = dark;
    if (t.dark) {
        t.bg = Rgb(0x202020);
        t.card = Rgb(0x2D2D2D);
        t.text = Rgb(0xFFFFFF);
        t.subtext = Rgb(0xC5C5C5);
        t.track = Rgb(0x8B8B8B);
        t.border = Rgb(0x3D3D3D);
        t.tile = Rgb(0x15223A);
        t.tileHover = Rgb(0x1F3253);
        t.focus = Rgb(0xFFFFFF);
    } else {
        t.bg = Rgb(0xF3F3F3);
        t.card = Rgb(0xFBFBFB);
        t.text = Rgb(0x1A1A1A);
        t.subtext = Rgb(0x5F5F5F);
        t.track = Rgb(0x8A8A8A);
        t.border = Rgb(0xDCDCDC);
        t.tile = Rgb(0x1B2A46);
        t.tileHover = Rgb(0x263B60);
        t.focus = Rgb(0x000000);
    }
    t.accent = SystemAccent(t.dark);
    t.onAccent = Luma(t.accent) > 0.5f ? Rgb(0x000000) : Rgb(0xFFFFFF);
    return t;
}

void SettingsWindow::ApplyTheme() {
    theme_ = MakeTheme(SystemDarkMode());
    if (hwnd_) {
        BOOL dark = theme_.dark;
        DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    }
}

void SettingsWindow::Show(const Settings& s) {
    settings_ = s;
    startWithWindows_ = IsStartWithWindows();
    if (!hwnd_) {
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc = WndProc;
            wc.hInstance = instance_;
            wc.lpszClassName = kClass;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP));
            LoadIconMetric(instance_, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &wc.hIconSm);
            registered = RegisterClassExW(&wc) != 0;
        }
        ApplyTheme();
        tab_ = Tab::Snow;

        // Open on the monitor the user is pointing at, centred in its work area.
        POINT pt;
        GetCursorPos(&pt);
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
        UINT dpi = 96, dpiY = 96;
        GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi, &dpiY);
        auto place = [&](HMONITOR m, UINT d) {
            scale_ = FitScale(m, d);
            const SIZE size = WindowSize(scale_, d);
            const RECT work = WorkArea(m);
            const LONG x = work.left + std::max(0L, (work.right - work.left - size.cx) / 2);
            const LONG y = work.top + std::max(0L, (work.bottom - work.top - size.cy) / 2);
            return RECT{x, y, x + size.cx, y + size.cy};
        };
        RECT rc = place(mon, dpi);
        hwnd_ = CreateWindowExW(0, kClass, SNOWYSTARR_APP_NAME L" settings", kStyle, rc.left, rc.top, rc.right - rc.left,
                                rc.bottom - rc.top, nullptr, nullptr, instance_, this);
        if (!hwnd_) return;
        dpi_ = GetDpiForWindow(hwnd_);
        if (dpi_ != dpi) {
            rc = place(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), dpi_);
            SetWindowPos(hwnd_, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ApplyTheme();
        ConfigurePreview();
        // Stay invisible until the first frame is drawn, so the window never
        // flashes blank. (Cloaked windows get no idle-loop frames.)
        Cloak(true);
        ShowWindow(hwnd_, SW_SHOW);
        Render();
        if (cloaked_) Cloak(false);  // drawing failed: show the plain background meanwhile
#ifdef SNOWYSTARR_DEV_TOOLS
        wchar_t snap[MAX_PATH];
        if (GetEnvironmentVariableW(L"SNOWYSTARR_SNAPSHOT", snap, MAX_PATH) > 0) snapshotPath_ = snap;
#endif
    } else if (IsIconic(hwnd_) && !hidden_) {
        ShowWindow(hwnd_, SW_RESTORE);
    }
    // With the colour picker up, bring that forward instead of the disabled owner.
    // (Put away while lights are hung, it waits for SetHidden(false).)
    if (hwnd_ && !hidden_) SetForegroundWindow(busy_ ? GetLastActivePopup(hwnd_) : hwnd_);
}

void SettingsWindow::SelectTab(Tab tab) {
    if (tab == tab_) return;
    tab_ = tab;
    // Keyboard focus stays on the tabs or the buttons along the bottom if it
    // was there; anywhere else it starts over (that widget is gone).
    if (SameGroup(focus_, kIdTab)) focus_ = kIdTab + static_cast<int>(tab);
    else if (focus_ != kIdClear && focus_ != kIdCredit && focus_ != kIdDefaults && focus_ != kIdDone) focus_ = 0;
    clearArmed_ = 0.0;
}

void SettingsWindow::RequestCustomColor() {
    if (!hwnd_ || busy_) return;
    SelectTab(Tab::Snow);
    action_ = Action::CustomColor;
}

void SettingsWindow::RequestLightColor(bool turnOn) {
    if (!hwnd_ || busy_) return;
    SelectTab(Tab::Lights);
    action_ = Action::LightColor;
    lightColorTurnsOn_ = turnOn;
}

void SettingsWindow::SetHidden(bool hidden) {
    if (!hwnd_ || hidden == hidden_) return;
    hidden_ = hidden;
    if (hidden) {
        // The colour picker would be left floating on its own: cancel it.
        if (busy_) {
            HWND popup = GetLastActivePopup(hwnd_);
            if (popup && popup != hwnd_) PostMessageW(popup, WM_COMMAND, IDCANCEL, 0);
        }
        if (GetCapture() == hwnd_) ReleaseCapture();  // a press in progress is cancelled (WM_CAPTURECHANGED)
        ShowWindow(hwnd_, SW_HIDE);
        input_.inside = false;
        input_.mouse = {-1000, -1000};
        clearArmed_ = 0.0;
    } else {
        last_ = 0.0;  // the preview picks up where it was rather than catching up on the time away
        if (IsIconic(hwnd_)) {
            ShowWindow(hwnd_, SW_SHOWMINNOACTIVE);
        } else {
            ShowWindow(hwnd_, SW_SHOW);
            SetForegroundWindow(hwnd_);
        }
    }
}

void SettingsWindow::Cloak(bool on) {
    BOOL value = on;
    DwmSetWindowAttribute(hwnd_, DWMWA_CLOAK, &value, sizeof(value));
    cloaked_ = on;
}

void SettingsWindow::Update(const Settings& s) {
    if (s == settings_) return;
    bool shadowChanged = s.shadow != settings_.shadow;
    settings_ = s;
    if (preview_) preview_->SetParams(PreviewParams());
    (void)shadowChanged;  // the atlas is rebuilt lazily in Render()
}

bool SettingsWindow::WantsFrame() const {
    if (!hwnd_ || hidden_ || !IsWindowVisible(hwnd_) || IsIconic(hwnd_)) return false;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(hwnd_, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return !cloaked;
}

bool SettingsWindow::Frame() {
#ifdef SNOWYSTARR_DEV_TOOLS
    if (!snapshotPath_.empty() && QpcSeconds() - origin_ > 2.0) {
        SaveSnapshot(snapshotPath_.c_str());
        snapshotPath_.clear();
    }
#endif
    // An occluded window (screen saver, lock screen, covered) presents without
    // waiting for vsync; skip drawing rather than spin.
    if (hwndRt_ && (hwndRt_->CheckWindowState() & D2D1_WINDOW_STATE_OCCLUDED)) return false;
    Render();
    return rt_ != nullptr;
}

SimParams SettingsWindow::PreviewParams() const {
    SimParams p = MakeSimParams(settings_);
    p.density *= 3.0f;  // the preview is small; show off the selected flakes
    // Its sky is short: deep drifts are squeezed so they don't bury it.
    if (p.driftMaxDip > 40.0f) p.driftMaxDip = 40.0f + 0.3f * (p.driftMaxDip - 40.0f);
    // The lights always hang as the drape across the top (see ConfigurePreview),
    // never the user's own strands: those belong to real monitors.
    p.lightLayout = LightLayout::Custom;
    p.strands = previewStrands_;
    return p;
}

void SettingsWindow::ConfigurePreview() {
    if (!preview_) {
        preview_ = std::make_unique<Scene>();
        preview_->SetDriftBoost(7.0f);    // build drifts quickly so the setting is visible
        preview_->SetScenerySettle(0.0f); // and show new tree and snowman amounts as the slider moves
    }
    SceneGeometry geo;
    geo.width = static_cast<int>(std::lround((previewRect_.right - previewRect_.left) * scale_));
    geo.height = static_cast<int>(std::lround((previewRect_.bottom - previewRect_.top) * scale_));
    geo.dpiScale = scale_;
    geo.groundY = std::round((kPreviewH - kPreviewTaskbar) * scale_);
    geo.leftOpen = geo.rightOpen = true;
    geo.monitor = L"preview";
    // The drape a whole desktop gets, squeezed into the preview's width: its
    // scallops stay shallow over the little sky, the bulbs at their real size.
    previewStrands_ =
        Lights::AcrossTop(geo.monitor, static_cast<int>(std::lround(kPreviewDesk * scale_)), geo.height, scale_);
    preview_->Configure(geo, PreviewParams());
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self = nullptr;
    }
    return self ? self->Handle(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

// `hwnd` equals hwnd_ until WM_DESTROY clears it; default processing must still get the real handle.
LRESULT SettingsWindow::Handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto mouseAt = [&](LPARAM l) {
        input_.mouse = {static_cast<float>(GET_X_LPARAM(l)) / scale_, static_cast<float>(GET_Y_LPARAM(l)) / scale_};
    };
    switch (msg) {
    case WM_MOUSEMOVE:
        mouseAt(lp);
        input_.inside = true;
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        return 0;
    case WM_MOUSELEAVE:
        tracking_ = false;
        input_.inside = false;
        if (!input_.down) input_.mouse = {-1000, -1000};
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        mouseAt(lp);
        SetCapture(hwnd_);
        input_.down = true;
        input_.pressed = true;
        return 0;
    case WM_LBUTTONUP:
        mouseAt(lp);
        // Before ReleaseCapture(): the WM_CAPTURECHANGED it sends must not cancel this release.
        input_.down = false;
        input_.released = true;
        ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        // Capture taken away mid-press (Alt+Tab, another window): a cancel, not a click.
        if (input_.down) {
            input_.down = false;
            input_.pressed = false;
            active_ = 0;
        }
        return 0;
    case WM_MOUSEWHEEL:
        input_.wheel += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            Close();
            return 0;
        }
        input_.keys.push_back(static_cast<UINT>(wp));
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, hot_ ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        EndPaint(hwnd_, &ps);
        Render();
        return 0;
    }
    case WM_ERASEBKGND:
        // Direct2D paints everything. Without a target yet (or after losing
        // it) show the theme background rather than stale pixels.
        if (!hwndRt_) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (HBRUSH brush = CreateSolidBrush(ToColorRef(theme_.bg))) {
                FillRect(reinterpret_cast<HDC>(wp), &rc, brush);
                DeleteObject(brush);
            }
        }
        return 1;
    case WM_SIZE:
        if (hwndRt_) hwndRt_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        return 0;
    case WM_ENTERSIZEMOVE:
    case WM_EXITSIZEMOVE:
        moving_ = msg == WM_ENTERSIZEMOVE;
        break;
    case WM_GETDPISCALEDSIZE: {
        // Propose the size fitted to the monitor being entered, so the
        // rectangle WM_DPICHANGED suggests already matches.
        const UINT dpi = static_cast<UINT>(wp);
        RECT wr;
        GetWindowRect(hwnd, &wr);
        *reinterpret_cast<SIZE*>(lp) = WindowSize(FitScale(MonitorAtDpi(wr, dpi), dpi), dpi);
        return TRUE;
    }
    case WM_DPICHANGED: {
        // Fit the destination monitor (the suggested rectangle's) rather than
        // scaling linearly, which can push the footer under the taskbar.
        const RECT& r = *reinterpret_cast<const RECT*>(lp);
        HMONITOR mon = MonitorAtDpi(r, HIWORD(wp));
        dpi_ = HIWORD(wp);
        scale_ = FitScale(mon, dpi_);
        const SIZE size = WindowSize(scale_, dpi_);
        LONG x = r.left, y = r.top;
        if (!moving_) {
            // E.g. a scaling change: keep it on the work area. (A drag places the window itself.)
            const RECT work = WorkArea(mon);
            x += std::max(work.left - x, std::min(0L, work.right - (x + size.cx)));
            y += std::max(work.top - y, std::min(0L, work.bottom - (y + size.cy)));
        }
        ReleaseResources();
        SetWindowPos(hwnd, nullptr, x, y, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
        ConfigurePreview();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
            ApplyTheme();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        ApplyTheme();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) startWithWindows_ = IsStartWithWindows();
        break;
    case WM_CLOSE:
        Close();
        return 0;
    case WM_DESTROY:
        ReleaseResources();
        hwnd_ = nullptr;
        // Start clean should the object be shown again.
        input_ = Input{};
        active_ = hot_ = 0;
        action_ = Action::None;
        tracking_ = moving_ = cloaked_ = hidden_ = false;
        clearArmed_ = 0.0;
        PostMessageW(app_, WM_APP_SETTINGS_CLOSED, 0, 0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

HRESULT SettingsWindow::CreateResources() {
    if (!factory_) return E_FAIL;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    // Opaque window: ignore alpha so text can use ClearType.
    D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        96.0f * scale_, 96.0f * scale_);
    HRESULT hr = factory_->CreateHwndRenderTarget(
        rtp, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom))),
        &hwndRt_);
    if (FAILED(hr)) return hr;
    if (FAILED(hr = hwndRt_.As(&rt_))) return hr;
    return CreateTargetResources();
}

HRESULT SettingsWindow::CreateTargetResources() {
    HRESULT hr;
    if (FAILED(hr = rt_.As(&dc_))) return hr;
    if (FAILED(hr = rt_->CreateSolidColorBrush(theme_.text, &brush_))) return hr;

    const D2D1_GRADIENT_STOP skyStops[] = {
        {0.0f, Rgb(0x0A1222)}, {0.65f, Rgb(0x17294A)}, {1.0f, Rgb(0x2A3F66)},
    };
    ComPtr<ID2D1GradientStopCollection> stops;
    if (SUCCEEDED(rt_->CreateGradientStopCollection(skyStops, 3, &stops))) {
        rt_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties({0, previewRect_.top}, {0, previewRect_.bottom - kPreviewTaskbar}),
            stops.Get(), &sky_);
    }
    const D2D1_GRADIENT_STOP moonStops[] = {
        {0.0f, Rgb(0xDDE8FF, 0.30f)}, {0.25f, Rgb(0xB8CCF5, 0.10f)}, {1.0f, Rgb(0x9DB4E8, 0.0f)},
    };
    ComPtr<ID2D1GradientStopCollection> moonCol;
    if (SUCCEEDED(rt_->CreateGradientStopCollection(moonStops, 3, &moonCol))) {
        rt_->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties({previewRect_.right - 90.0f, previewRect_.top + 34.0f}, {0, 0}, 120.0f,
                                                120.0f),
            moonCol.Get(), &moon_);
    }

    for (int i = 0; i < 8; ++i) {
        ComPtr<IWICBitmapSource> src;
        if (SUCCEEDED(LoadClassicFlake(wic_.Get(), i, &src)))
            rt_->CreateBitmapFromWicBitmap(src.Get(), nullptr, &classic_[i]);
    }
    BuildTileImages();

    // Without an atlas the preview just has no snow; Render() retries.
    FlakeArtOptions opt;
    opt.shadow = settings_.shadow;
    if (SUCCEEDED(atlas_.Build(dc_.Get(), wic_.Get(), opt))) atlasShadow_ = settings_.shadow;
    else atlasRetry_ = QpcSeconds() + 1.0;
    return S_OK;
}

void SettingsWindow::ReleaseResources() {
    atlas_.Reset();
    if (preview_) preview_->ReleaseDeviceResources();
    for (auto& b : tileImages_) b.Reset();
    for (auto& set : bulbImages_)
        for (auto& b : set) b.Reset();
    bulbLook_ = BulbLook{};
    for (auto& b : classic_) b.Reset();
    moon_.Reset();
    sky_.Reset();
    brush_.Reset();
    dc_.Reset();
    rt_.Reset();
    hwndRt_.Reset();
}

#ifdef SNOWYSTARR_DEV_TOOLS
HRESULT SettingsWindow::SaveSnapshot(const wchar_t* path) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    ReleaseResources();
    ComPtr<IWICBitmap> bmp;
    const HRESULT hr = RenderOffscreen(static_cast<UINT>(rc.right), static_cast<UINT>(rc.bottom), Snapshot{}, &bmp);
    ReleaseResources();  // the next frame recreates the window target
    return SUCCEEDED(hr) ? WritePng(wic_.Get(), bmp.Get(), path) : hr;
}

// Draws into a WIC bitmap instead of the window: the preview first runs for a
// while, then the snapshot's clicks and keys go in, one frame each.
HRESULT SettingsWindow::RenderOffscreen(UINT width, UINT height, const Snapshot& shot, IWICBitmap** out) {
    ComPtr<IWICBitmap> bmp;
    HRESULT hr = wic_->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp);
    if (FAILED(hr)) return hr;
    D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f * scale_, 96.0f * scale_);
    if (FAILED(hr = factory_->CreateWicBitmapRenderTarget(bmp.Get(), rtp, &rt_))) return hr;
    if (FAILED(hr = CreateTargetResources())) return hr;

    // Let the preview fill with snow, build a little drift and light up first.
    constexpr int kSteps = 600;
    for (int i = 0; i < kSteps; ++i) preview_->Update(1.0f / 60.0f, i / 60.0, CursorState{});
    origin_ = QpcSeconds() - kSteps / 60.0;  // and carry on from there
    last_ = 0.0;
    Render();  // a first frame lays the widgets out, so the first Tab has somewhere to go
    for (const D2D1_POINT_2F& p : shot.clicks) {
        input_.mouse = p;
        input_.inside = true;
        input_.down = input_.pressed = true;
        Render();
        input_.down = false;
        input_.released = true;
        Render();
    }
    for (UINT k : shot.keys) {
        input_.keys.push_back(k);
        Render();
    }
    input_.mouse = shot.hover;
    input_.inside = shot.hover.x > -1000.0f;
    Render();
    *out = bmp.Detach();
    return S_OK;
}

HRESULT SettingsWindow::RenderSnapshot(const Settings& s, const Snapshot& shot, const wchar_t* path) {
    SettingsWindow w(nullptr, nullptr);
    if (!w.factory_ || !w.dwrite_ || !w.wic_) return E_FAIL;
    w.settings_ = s;
    w.scale_ = Clamp(shot.scale, 0.5f, 4.0f);
    w.theme_ = MakeTheme(shot.dark);
    w.tab_ = shot.tab;
    w.ConfigurePreview();
    ComPtr<IWICBitmap> bmp;
    const HRESULT hr = w.RenderOffscreen(static_cast<UINT>(std::lround(kClientW * w.scale_)),
                                         static_cast<UINT>(std::lround(kClientH * w.scale_)), shot, &bmp);
    w.ReleaseResources();
    return SUCCEEDED(hr) ? WritePng(w.wic_.Get(), bmp.Get(), path) : hr;
}
#endif

void SettingsWindow::BuildTileImages() {
    FlakeArtOptions opt;
    opt.shadow = false;  // tiles sit on a dark background
    // Pixel size only: the images keep the target's DPI, so drawn at their own
    // size from a whole-pixel corner they map 1:1 even at fractional scales.
    const UINT32 px = static_cast<UINT32>(std::lround(kTile * scale_));
    const D2D1_SIZE_U pixels = D2D1::SizeU(px, px);
    const D2D1_PIXEL_FORMAT format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    for (int k = 0; k < kFlakeKindCount; ++k) {
        ComPtr<ID2D1BitmapRenderTarget> brt;
        if (FAILED(rt_->CreateCompatibleRenderTarget(nullptr, &pixels, &format,
                                                     D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, &brt)))
            continue;
        const D2D1_SIZE_F size = brt->GetSize();
        const FlakeKind kind = static_cast<FlakeKind>(k);
        ID2D1Bitmap* classic = nullptr;
        if (IsClassic(kind)) classic = classic_[k - static_cast<int>(FlakeKind::Classic1)].Get();
        brt->BeginDraw();
        brt->Clear(D2D1::ColorF(0, 0, 0, 0));
        // Classic flakes are 15 px pixel art: show them at a crisp 2x.
        float r = IsClassic(kind) ? 15.0f : TileRadius(kind);
        DrawFlakeDesign(brt.Get(), kind, 0, size.width * 0.5f, size.height * 0.5f, r, opt, classic);
        if (SUCCEEDED(brt->EndDraw())) brt->GetBitmap(&tileImages_[k]);
    }
}

// Each bulb hangs from a little scallop of wire across its tile, in the chosen
// colours on the chosen wire (the tiles in a row read like a strand), drawn
// lit and unlit so the chosen one can shine.
void SettingsWindow::BuildBulbImages() {
    for (auto& set : bulbImages_)
        for (auto& b : set) b.Reset();
    bulbLook_ = {settings_.lightColors, settings_.lightCustomColor, settings_.wire};
    ComPtr<ID2D1PathGeometry> wire;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory_->CreatePathGeometry(&wire)) || FAILED(wire->Open(&sink))) return;
    sink->BeginFigure({-2.0f, kWireEnds}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddQuadraticBezier({{0.5f * kTile, 2.0f * kWireLow - kWireEnds}, {kTile + 2.0f, kWireEnds}});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return;

    const UINT32 px = static_cast<UINT32>(std::lround(kTile * scale_));
    const D2D1_SIZE_U pixels = D2D1::SizeU(px, px);
    const D2D1_PIXEL_FORMAT format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    const D2D1_COLOR_F wireColor = WireRgb(settings_.wire);
    // A faint sheen along the top of the wire, like the strands' own.
    const D2D1_COLOR_F sheen = Mix(wireColor, Rgb(0xFFFFFF), settings_.wire == WireColor::White ? 0.6f : 0.22f);
    for (int lit = 0; lit < 2; ++lit) {
        for (int t = 0; t < kBulbTypeCount; ++t) {
            ComPtr<ID2D1BitmapRenderTarget> brt;
            ComPtr<ID2D1SolidColorBrush> brush;
            if (FAILED(rt_->CreateCompatibleRenderTarget(nullptr, &pixels, &format,
                                                         D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, &brt)) ||
                FAILED(brt->CreateSolidColorBrush(wireColor, &brush)))
                continue;
            const BulbType type = static_cast<BulbType>(t);
            const D2D1_COLOR_F color = LightColor(settings_.lightColors, settings_.lightCustomColor, t);
            const float len = BulbTileLength(type);
            const D2D1_POINT_2F socket = {0.5f * kTile, kWireLow};
            brt->BeginDraw();
            brt->Clear(D2D1::ColorF(0, 0, 0, 0));
            brt->DrawGeometry(wire.Get(), brush.Get(), kWireW);
            brush->SetColor(sheen);
            brush->SetOpacity(0.55f);
            brt->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, -0.45f));
            brt->DrawGeometry(wire.Get(), brush.Get(), 0.6f);
            brt->SetTransform(D2D1::Matrix3x2F::Identity());
            if (lit) {
                // The light it throws on the tile: soft, and well inside its edges.
                const D2D1_POINT_2F glass = {socket.x, socket.y + 0.6f * len};
                const float r = std::min(0.75f * len + 6.0f, std::min(kTile - glass.y, glass.y) - 1.0f);
                const D2D1_GRADIENT_STOP stops[] = {
                    {0.0f, D2D1::ColorF(color.r, color.g, color.b, 0.55f)},
                    {0.4f, D2D1::ColorF(color.r, color.g, color.b, 0.2f)},
                    {1.0f, D2D1::ColorF(color.r, color.g, color.b, 0.0f)},
                };
                ComPtr<ID2D1GradientStopCollection> coll;
                ComPtr<ID2D1RadialGradientBrush> halo;
                if (SUCCEEDED(brt->CreateGradientStopCollection(stops, 3, &coll)) &&
                    SUCCEEDED(brt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(glass, {0, 0}, r, r),
                                                             coll.Get(), &halo)))
                    brt->FillEllipse(D2D1::Ellipse(glass, r, r), halo.Get());
            }
            Lights::DrawIcon(brt.Get(), type, socket, len, color, lit != 0);
            if (SUCCEEDED(brt->EndDraw())) brt->GetBitmap(&bulbImages_[lit][t]);
        }
    }
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

void SettingsWindow::Text(const wchar_t* s, const D2D1_RECT_F& r, IDWriteTextFormat* fmt, const D2D1_COLOR_F& c) {
    if (!fmt || !s) return;
    brush_->SetColor(c);
    rt_->DrawTextW(s, static_cast<UINT32>(wcslen(s)), fmt, r, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void SettingsWindow::Fill(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c) {
    brush_->SetColor(c);
    if (radius > 0.0f) rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.Get());
    else rt_->FillRectangle(r, brush_.Get());
}

void SettingsWindow::Stroke(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c, float width) {
    brush_->SetColor(c);
    D2D1_RECT_F in = Inflate(r, -width * 0.5f);
    if (radius > 0.0f) rt_->DrawRoundedRectangle(D2D1::RoundedRect(in, radius, radius), brush_.Get(), width);
    else rt_->DrawRectangle(in, brush_.Get(), width);
}

void SettingsWindow::Circle(float cx, float cy, float radius, const D2D1_COLOR_F& c) {
    brush_->SetColor(c);
    rt_->FillEllipse(D2D1::Ellipse({cx, cy}, radius, radius), brush_.Get());
}

void SettingsWindow::Swatch(float cx, float cy, float radius, const D2D1_COLOR_F& c) {
    Circle(cx, cy, radius, Rgb(0x9A9A9A));
    Circle(cx, cy, radius - 1.0f, c);
}

void SettingsWindow::FocusRing(int id, const D2D1_RECT_F& r, float radius) {
    if (focusVisible_ && focus_ == id) Stroke(Inflate(r, 3.0f), radius + 3.0f, theme_.focus, 2.0f);
}

// Small line icons for the tabs: a snowflake, and a bulb on its socket.
void SettingsWindow::TabIcon(Tab tab, float cx, float cy, const D2D1_COLOR_F& c) {
    brush_->SetColor(c);
    if (tab == Tab::Snow) {
        for (int i = 0; i < 6; ++i) {
            const float a = static_cast<float>(i) * kTau / 6.0f;
            const float dx = std::sin(a), dy = -std::cos(a);
            rt_->DrawLine({cx, cy}, {cx + 6.5f * dx, cy + 6.5f * dy}, brush_.Get(), 1.3f);
            // A little V on each arm.
            const D2D1_POINT_2F b = {cx + 3.8f * dx, cy + 3.8f * dy};
            for (float side : {-1.0f, 1.0f}) {
                const float s = a + side * 0.75f;
                rt_->DrawLine(b, {b.x + 2.3f * std::sin(s), b.y - 2.3f * std::cos(s)}, brush_.Get(), 1.1f);
            }
        }
        return;
    }
    rt_->FillRoundedRectangle(D2D1::RoundedRect(Rect(cx - 2.2f, cy - 7.0f, 4.4f, 3.6f), 0.8f, 0.8f), brush_.Get());
    rt_->FillEllipse(D2D1::Ellipse({cx, cy + 1.6f}, 3.9f, 5.3f), brush_.Get());
}

// A little picture of each layout, in the chosen colours on the chosen wire:
// scallops along the top, or a strand hung from hooks wherever.
void SettingsWindow::StrandIcon(LightLayout layout, const D2D1_RECT_F& r) {
    Fill(r, 6.0f, theme_.tile);
    const IconStrand& strand = kIconStrands[static_cast<int>(layout)];
    D2D1_MATRIX_3X2_F old;
    rt_->GetTransform(&old);
    rt_->SetTransform(D2D1::Matrix3x2F::Translation(r.left, r.top) * old);
    if (const auto& wire = strandWires_[static_cast<int>(layout)]) {
        brush_->SetColor(WireRgb(settings_.wire));
        rt_->DrawGeometry(wire.Get(), brush_.Get(), 1.2f);
    }
    // Two bulbs to a span, hanging from the wire, with a glint (dark while the lights are off).
    int n = 0;
    for (int i = 1; i < strand.count; ++i)
        for (float t : {0.27f, 0.73f}) {
            D2D1_POINT_2F p = IconSpanPoint(strand, i, t);
            p.y += 2.8f;
            D2D1_COLOR_F color = LightColor(settings_.lightColors, settings_.lightCustomColor, n++);
            if (!settings_.lights) color = Mix(color, theme_.tile, 0.55f);
            brush_->SetColor(color);
            rt_->FillEllipse(D2D1::Ellipse(p, 1.8f, 2.5f), brush_.Get());
            if (settings_.lights) Circle(p.x - 0.5f, p.y - 0.7f, 0.6f, Rgb(0xFFFFFF, 0.8f));
        }
    // The hooks the user put up, as hanging mode shows them.
    if (layout == LightLayout::Custom)
        for (int i = 0; i < strand.count; ++i) {
            const D2D1_POINT_2F p = IconHook(strand, i);
            Circle(p.x, p.y, 2.0f, Rgb(0xFFFFFF, 0.9f));
            Circle(p.x, p.y, 1.0f, theme_.tile);
        }
    rt_->SetTransform(old);
}

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------

void SettingsWindow::Interact(int id, const D2D1_RECT_F& r, bool& hovered, bool& held, bool& clicked) {
    if (!GroupOf(id)) order_.push_back(id);  // a group of tiles or chips is a single tab stop (see GroupStop)
    hovered = Contains(r, input_.mouse) && (active_ == 0 || active_ == id);
    if (hovered) hot_ = id;
    clicked = false;
    if (hovered && input_.pressed) {
        active_ = id;
        focus_ = id;
        focusVisible_ = false;
    }
    held = active_ == id && (input_.down || input_.pressed);
    if (active_ == id && input_.released && hovered) clicked = true;
    if (focus_ == id)
        for (UINT k : input_.keys)
            if (k == VK_SPACE || k == VK_RETURN) clicked = true;
}

void SettingsWindow::GroupStop(int selectedId) { order_.push_back(SameGroup(focus_, selectedId) ? focus_ : selectedId); }

bool SettingsWindow::Slider(int id, float x, float y, float w, const wchar_t* label, int& value, int lo, int hi,
                            bool bipolar, const wchar_t* valueText) {
    const float labelW = 62.0f, valueW = 84.0f;
    const float tx0 = x + labelW + 10.0f, tx1 = x + w - valueW - 8.0f;
    const D2D1_RECT_F hit = {tx0 - 10.0f, y - 15.0f, tx1 + 10.0f, y + 15.0f};
    const float span = static_cast<float>(hi - lo);
    const float thumbX = Lerp(tx0, tx1, Saturate(static_cast<float>(value - lo) / span));  // before this frame's input
    bool hovered, held, clicked;
    Interact(id, hit, hovered, held, clicked);
    const int old = value;
    if (held && active_ == id) {
        // Pressing the thumb grabs it where it is; pressing the bare track jumps there.
        if (input_.pressed) {
            const float offset = input_.mouse.x - thumbX;
            grabOffset_ = std::fabs(offset) <= 10.0f ? offset : 0.0f;
        }
        float t = Saturate((input_.mouse.x - grabOffset_ - tx0) / (tx1 - tx0));
        value = lo + static_cast<int>(std::lround(t * span));
    }
    if (hovered) {
        // Whole steps only, keeping the remainder so a precision touchpad's small deltas add up.
        if (wheelSlider_ != id) {
            wheelSlider_ = id;
            wheelSteps_ = 0.0f;
        }
        wheelSteps_ += input_.wheel * 3.0f;
        const float steps = std::trunc(wheelSteps_);
        wheelSteps_ -= steps;
        value += static_cast<int>(steps);
    }
    if (focus_ == id) {
        for (UINT k : input_.keys) {
            if (k == VK_LEFT || k == VK_DOWN) value -= 1;
            else if (k == VK_RIGHT || k == VK_UP) value += 1;
            else if (k == VK_PRIOR) value += 10;
            else if (k == VK_NEXT) value -= 10;
            else if (k == VK_HOME) value = lo;
            else if (k == VK_END) value = hi;
        }
    }
    value = Clamp(value, lo, hi);

    Text(label, Rect(x, y - 12.0f, labelW, 24.0f), fmtBody_.Get(), theme_.text);
    const float t = static_cast<float>(value - lo) / static_cast<float>(hi - lo);
    const float tx = Lerp(tx0, tx1, t);
    Fill({tx0, y - 2.0f, tx1, y + 2.0f}, 2.0f, Mix(theme_.track, theme_.bg, 0.35f));
    float from = bipolar ? (tx0 + tx1) * 0.5f : tx0;
    if (bipolar) Fill({from - 1.0f, y - 6.0f, from + 1.0f, y + 6.0f}, 1.0f, theme_.track);
    Fill({std::min(from, tx), y - 2.0f, std::max(from, tx), y + 2.0f}, 2.0f, theme_.accent);
    // Thumb: Windows 11 style ring with an accent core that grows on hover.
    Circle(tx, y, 10.0f, theme_.border);
    Circle(tx, y, 9.0f, theme_.card);
    Circle(tx, y, held ? 4.5f : (hovered ? 6.5f : 5.5f), theme_.accent);
    Text(valueText, Rect(tx1 + 8.0f, y - 12.0f, valueW, 24.0f), fmtValue_.Get(), theme_.subtext);
    FocusRing(id, hit, 6.0f);
    return value != old;
}

bool SettingsWindow::Toggle(int id, float x, float y, float w, const wchar_t* label, bool& value) {
    const D2D1_RECT_F hit = Rect(x - 4.0f, y - 14.0f, w, 28.0f);
    bool hovered, held, clicked;
    Interact(id, hit, hovered, held, clicked);
    bool changed = false;
    if (clicked) {
        value = !value;
        changed = true;
    }
    const D2D1_RECT_F sw = Rect(x, y - 10.0f, 40.0f, 20.0f);
    if (value) {
        Fill(sw, 10.0f, hovered ? Mix(theme_.accent, theme_.text, 0.1f) : theme_.accent);
        Circle(x + 30.0f, y, hovered ? 6.5f : 6.0f, theme_.onAccent);
    } else {
        Fill(sw, 10.0f, hovered ? Mix(theme_.card, theme_.text, 0.06f) : theme_.card);
        Stroke(sw, 10.0f, theme_.subtext, 1.0f);
        Circle(x + 10.0f, y, hovered ? 5.5f : 5.0f, theme_.subtext);
    }
    Text(label, Rect(x + 52.0f, y - 12.0f, w - 56.0f, 24.0f), fmtBody_.Get(), theme_.text);
    FocusRing(id, hit, 6.0f);
    return changed;
}

bool SettingsWindow::Button(int id, const D2D1_RECT_F& r, const wchar_t* text, bool primary) {
    bool hovered, held, clicked;
    Interact(id, r, hovered, held, clicked);
    D2D1_COLOR_F fill = primary ? theme_.accent : theme_.card;
    if (held && hovered) fill = Mix(fill, theme_.bg, 0.25f);
    else if (hovered) fill = Mix(fill, theme_.text, 0.07f);
    Fill(r, 5.0f, fill);
    if (!primary) Stroke(r, 5.0f, theme_.border, 1.0f);
    Text(text, r, fmtButton_.Get(), primary ? theme_.onAccent : theme_.text);
    FocusRing(id, r, 6.0f);
    return clicked;
}

bool SettingsWindow::Link(int id, const D2D1_RECT_F& r, const wchar_t* text, IDWriteTextFormat* fmt) {
    bool hovered, held, clicked;
    Interact(id, r, hovered, held, clicked);
    Text(text, r, fmt ? fmt : fmtValue_.Get(), hovered ? Mix(theme_.accent, theme_.text, 0.25f) : theme_.accent);
    FocusRing(id, r, 4.0f);
    return clicked;
}

void SettingsWindow::TileFrame(const D2D1_RECT_F& r, bool hovered, bool selected) {
    if (selected) {
        Stroke(r, 8.0f, theme_.accent, 2.0f);
        // Check badge.
        const float cx = r.right - 10.0f, cy = r.top + 10.0f;
        Circle(cx, cy, 7.0f, theme_.accent);
        brush_->SetColor(theme_.onAccent);
        rt_->DrawLine({cx - 3.2f, cy + 0.2f}, {cx - 0.8f, cy + 2.6f}, brush_.Get(), 1.6f);
        rt_->DrawLine({cx - 0.8f, cy + 2.6f}, {cx + 3.4f, cy - 2.4f}, brush_.Get(), 1.6f);
    } else if (!theme_.dark) {
        Stroke(r, 8.0f, Mix(hovered ? theme_.tileHover : theme_.tile, theme_.bg, 0.5f), 1.0f);
    }
}

bool SettingsWindow::Tile(int id, const D2D1_RECT_F& r, FlakeKind kind, bool selected) {
    bool hovered, held, clicked;
    Interact(id, r, hovered, held, clicked);
    if (hovered) hoveredTile_ = static_cast<int>(kind);
    Fill(r, 8.0f, hovered ? theme_.tileHover : theme_.tile);
    if (const auto& img = tileImages_[static_cast<int>(kind)]) {
        // At its own size from the tile's whole-pixel corner: 1:1 with the device pixels.
        const D2D1_SIZE_F size = img->GetSize();
        rt_->DrawBitmap(img.Get(), Rect(r.left, r.top, size.width, size.height), selected ? 1.0f : 0.35f,
                        IsClassic(kind) ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                        : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    TileFrame(r, false, selected);
    FocusRing(id, r, 10.0f);
    return clicked;
}

bool SettingsWindow::BulbTile(int id, const D2D1_RECT_F& r, BulbType type, bool selected) {
    bool hovered, held, clicked;
    Interact(id, r, hovered, held, clicked);
    // The name shows for the bulb under the pointer, else for the one the keyboard is on.
    const bool focused = focusVisible_ && focus_ == id;
    if (hovered || (focused && hoveredTile_ < 0)) hoveredTile_ = static_cast<int>(type);
    Fill(r, 8.0f, hovered ? theme_.tileHover : theme_.tile);
    // Every bulb shines while the lights are on (dark glass shows too little of
    // it to choose by); the chosen one, or the one under the pointer or the
    // keyboard, at full strength.
    const bool lit = settings_.lights;
    const bool strong = selected || hovered || focused;
    if (const auto& img = bulbImages_[lit ? 1 : 0][static_cast<int>(type)]) {
        const D2D1_SIZE_F size = img->GetSize();
        rt_->DrawBitmap(img.Get(), Rect(r.left, r.top, size.width, size.height), lit && strong ? 1.0f : 0.72f,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    TileFrame(r, hovered, selected);
    FocusRing(id, r, 10.0f);
    return clicked;
}

bool SettingsWindow::ColorChip(int id, const D2D1_RECT_F& r, SnowColor color, const wchar_t* text, bool selected) {
    bool hovered, held, clicked;
    Interact(id, r, hovered, held, clicked);
    Fill(r, 16.0f, hovered ? Mix(theme_.card, theme_.text, 0.06f) : theme_.card);
    Stroke(r, 16.0f, selected ? theme_.accent : theme_.border, selected ? 2.0f : 1.0f);
    const float cx = r.left + 18.0f, cy = (r.top + r.bottom) * 0.5f;
    switch (color) {
    case SnowColor::White:
        Circle(cx, cy, 8.0f, Rgb(0x9A9A9A));
        Circle(cx, cy, 7.0f, Rgb(0xFFFFFF));
        break;
    case SnowColor::Ice:
        Circle(cx, cy, 8.0f, Rgb(0x7FA7C9));
        Circle(cx, cy, 7.0f, D2D1::ColorF(0.80f, 0.92f, 1.0f));
        break;
    case SnowColor::Rainbow:
        Circle(cx - 3.5f, cy - 3.0f, 4.5f, Rgb(0xFFB3C7));
        Circle(cx + 3.5f, cy - 3.0f, 4.5f, Rgb(0xFFE7A3));
        Circle(cx - 3.5f, cy + 3.5f, 4.5f, Rgb(0xA8E6B8));
        Circle(cx + 3.5f, cy + 3.5f, 4.5f, Rgb(0xA9C8FF));
        break;
    default:
        Swatch(cx, cy, 8.0f, FromColorRef(settings_.customColor));
        break;
    }
    Text(text, {r.left + 32.0f, r.top, r.right - 8.0f, r.bottom}, fmtBody_.Get(), theme_.text);
    FocusRing(id, r, 18.0f);
    return clicked;
}

bool SettingsWindow::Chip(int id, const D2D1_RECT_F& r, const wchar_t* text, bool selected, bool swatch) {
    bool hovered, held, clicked;
    Interact(id, r, hovered, held, clicked);
    const float radius = 0.5f * (r.bottom - r.top);
    D2D1_COLOR_F fill = selected ? Mix(theme_.card, theme_.accent, theme_.dark ? 0.16f : 0.1f) : theme_.card;
    if (held && hovered) fill = Mix(fill, theme_.bg, 0.4f);
    else if (hovered) fill = Mix(fill, theme_.text, 0.06f);
    Fill(r, radius, fill);
    Stroke(r, radius, selected ? theme_.accent : theme_.border, selected ? 2.0f : 1.0f);
    Text(text, {r.left + (swatch ? kChipSwatch : kChipPad), r.top, r.right - kChipPad, r.bottom}, fmtChip_.Get(),
         theme_.text);
    FocusRing(id, r, radius + 2.0f);
    return clicked;
}

bool SettingsWindow::StrandCard(int id, const D2D1_RECT_F& r, float hitRight, LightLayout layout, const wchar_t* text,
                                bool selected, bool enabled) {
    bool hovered = false, held = false, clicked = false;
    if (enabled) Interact(id, {r.left, r.top, hitRight, r.bottom}, hovered, held, clicked);
    // Chosen and hovered like the chips.
    D2D1_COLOR_F fill = selected ? Mix(theme_.card, theme_.accent, theme_.dark ? 0.16f : 0.1f) : theme_.card;
    if (held && hovered) fill = Mix(fill, theme_.bg, 0.4f);
    else if (hovered) fill = Mix(fill, theme_.text, 0.06f);
    Fill(r, 8.0f, fill);
    Stroke(r, 8.0f, selected ? theme_.accent : theme_.border, selected ? 2.0f : 1.0f);
    const D2D1_RECT_F icon = Rect(Snap(r.left + kCardInset), Snap(r.top + kCardInset), kStrandIconW, kStrandIconH);
    StrandIcon(layout, icon);
    const float tx = icon.right + 14.0f, mid = (r.top + r.bottom) * 0.5f;
    Text(LayoutLabel(layout), {tx, mid - 20.0f, hitRight - 8.0f, mid}, fmtBody_.Get(),
         enabled ? theme_.text : Mix(theme_.text, theme_.bg, 0.45f));
    Text(text, {tx, mid, hitRight - 8.0f, mid + 18.0f}, fmtSmall_.Get(), theme_.subtext);
    FocusRing(id, r, 10.0f);
    return clicked;
}

void SettingsWindow::DrawPreview(const D2D1_RECT_F& r) {
    rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), previewClip_.Get()), nullptr);

    if (sky_) rt_->FillRectangle(r, sky_.Get());
    if (moon_) rt_->FillRectangle(r, moon_.Get());

    // A miniature taskbar for the snow to settle on.
    const D2D1_RECT_F bar = {r.left, r.bottom - kPreviewTaskbar, r.right, r.bottom};
    Fill(bar, 0.0f, Rgb(0x1C1C1C, 0.96f));
    const D2D1_COLOR_F icons[] = {Rgb(0x3A8EE6), Rgb(0xF2B233), Rgb(0x4CB86B), Rgb(0xE5533C), Rgb(0x9A6BE0)};
    const float iconSize = 12.0f, gap = 8.0f;
    float ix = (r.left + r.right) * 0.5f - (5 * iconSize + 4 * gap) * 0.5f;
    for (const auto& c : icons) {
        Fill(Rect(ix, bar.top + (kPreviewTaskbar - iconSize) * 0.5f, iconSize, iconSize), 3.0f, c);
        ix += iconSize + gap;
    }

    // The snow and the lights, drawn exactly like the desktop overlay (in pixels).
    if (preview_ && atlas_.Ready()) {
        D2D1_MATRIX_3X2_F old;
        rt_->GetTransform(&old);
        rt_->SetTransform(D2D1::Matrix3x2F::Scale(1.0f / scale_, 1.0f / scale_) *
                          D2D1::Matrix3x2F::Translation(Snap(r.left), Snap(r.top)) * old);
        preview_->Draw(dc_.Get(), atlas_);
        rt_->SetTransform(old);
    }

    rt_->PopLayer();
    Stroke(r, 8.0f, theme_.border, 1.0f);
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------

// The Snow | Lights switch under the preview: a sunken track with the chosen
// tab raised out of it (one tab stop; the arrow keys switch tabs). The
// preview's caption sits beside it, clear of the lights draped across its top.
void SettingsWindow::TabBar(float y) {
    const D2D1_RECT_F bar = Rect(kMargin, y, 2.0f * kTabW + 8.0f, kTabsH);
    const D2D1_COLOR_F well = theme_.dark ? Mix(theme_.bg, Rgb(0x000000), 0.3f) : Mix(theme_.bg, Rgb(0x000000), 0.045f);
    Fill(bar, 8.0f, well);
    Stroke(bar, 8.0f, Mix(theme_.border, well, 0.4f), 1.0f);
    const wchar_t* names[] = {L"Snow", L"Lights"};
    for (int i = 0; i < 2; ++i) {
        const Tab tab = static_cast<Tab>(i);
        const bool selected = tab == tab_;
        const D2D1_RECT_F r = Rect(bar.left + 4.0f + static_cast<float>(i) * kTabW, bar.top + 4.0f, kTabW, kTabsH - 8.0f);
        bool hovered, held, clicked;
        Interact(kIdTab + i, r, hovered, held, clicked);
        if (clicked) SelectTab(tab);
        if (selected) {
            Fill(r, 5.0f, theme_.dark ? Mix(theme_.card, Rgb(0xFFFFFF), 0.05f) : Rgb(0xFFFFFF));
            Stroke(r, 5.0f, theme_.border, 1.0f);
            Fill(Rect((r.left + r.right) * 0.5f - 8.0f, r.bottom - 3.0f, 16.0f, 3.0f), 1.5f, theme_.accent);
        } else if (hovered) {
            Fill(r, 5.0f, Mix(well, theme_.text, held ? 0.04f : 0.07f));
        }
        const D2D1_COLOR_F ink = selected ? theme_.text : (hovered ? Mix(theme_.subtext, theme_.text, 0.5f) : theme_.subtext);
        const float cx = (r.left + r.right) * 0.5f, cy = (r.top + r.bottom) * 0.5f - 1.0f;
        TabIcon(tab, cx - 20.0f, cy, selected ? theme_.accent : ink);
        Text(names[i], Rect(cx - 8.0f, r.top, kTabW * 0.5f, r.bottom - r.top - 2.0f), fmtBody_.Get(), ink);
        FocusRing(kIdTab + i, r, 6.0f);
    }
    GroupStop(kIdTab + static_cast<int>(tab_));

    const Settings& s = settings_;
    const wchar_t* caption = L"Preview • move the mouse through the snow";
    if (tab_ == Tab::Snow && s.paused) caption = L"Preview • snow is paused on the desktop";
    else if (tab_ == Tab::Lights) caption = !s.lights ? L"Preview • the lights are off" : L"Preview • brush the lights with the mouse";
    Text(caption, Rect(bar.right + 16.0f, y, kClientW - kMargin - bar.right - 16.0f, kTabsH), fmtValue_.Get(),
         theme_.subtext);
}

void SettingsWindow::SnowPage(Settings& s, float y) {
    // --- Snowflakes -------------------------------------------------------
    // The links sit above the grid, so they come first in the tab order.
    if (Link(kIdAllFlakes, Rect(kClientW - kMargin - 170.0f, y, 70.0f, 24.0f), L"Select all")) {
        s.flakeMask = kAllFlakesMask;
        Changed();
    }
    if (Link(kIdDefaultFlakes, Rect(kClientW - kMargin - 90.0f, y, 90.0f, 24.0f), L"Recommended")) {
        s.flakeMask = kDefaultFlakeMask;
        Changed();
    }
    for (int k = 0; k < kFlakeKindCount; ++k) {
        int col = k % kTileCols, row = k / kTileCols;
        // Whole-pixel corners keep the tile images crisp at fractional scales.
        D2D1_RECT_F r = Rect(Snap(kMargin + static_cast<float>(col) * (kTile + kTileGap)),
                             Snap(y + 30.0f + static_cast<float>(row) * (kTile + kTileGap)), kTile, kTile);
        const uint32_t bit = 1u << k;
        if (Tile(kIdTile + k, r, static_cast<FlakeKind>(k), (s.flakeMask & bit) != 0)) {
            // Keep at least one design selected.
            if ((s.flakeMask & bit) == 0 || (s.flakeMask & ~bit) != 0) {
                s.flakeMask ^= bit;
                Changed();
            }
        }
    }
    // One tab stop for the grid: the focused tile, else the first. Arrow keys move within it.
    GroupStop(kIdTile);
    Text(L"Snowflakes", Rect(kMargin, y, 120.0f, 24.0f), fmtSection_.Get(), theme_.text);
    std::wstring hint = hoveredTile_ >= 0 ? FlakeKindName(static_cast<FlakeKind>(hoveredTile_))
                                          : L"Pick the designs that fall";
    Text(hint.c_str(), Rect(kMargin + 104.0f, y, 300.0f, 24.0f), fmtSmall_.Get(), theme_.subtext);

    // --- Sliders ----------------------------------------------------------
    y += 30.0f + 2.0f * kTile + kTileGap + 14.0f;
    Fill(Rect(kMargin, y, kClientW - 2.0f * kMargin, 1.0f), 0.0f, theme_.border);
    y += 24.0f;
    const float colW = (kClientW - 2.0f * kMargin - 24.0f) * 0.5f;
    const float x2 = kMargin + colW + 24.0f;
    const std::wstring wind = WindText(s.wind);
    if (Slider(kIdAmount, kMargin, y, colW, L"Amount", s.amount, 0, 100, false, AmountText(s.amount))) Changed();
    if (Slider(kIdWind, x2, y, colW, L"Wind", s.wind, -100, 100, true, wind.c_str())) Changed();
    y += 38.0f;
    if (Slider(kIdSpeed, kMargin, y, colW, L"Speed", s.speed, 0, 100, false, SpeedText(s.speed))) Changed();
    if (Slider(kIdGusts, x2, y, colW, L"Gusts", s.gusts, 0, 100, false, GustText(s.gusts))) Changed();
    y += 38.0f;
    if (Slider(kIdSize, kMargin, y, colW, L"Size", s.size, 0, 100, false, SizeText(s.size))) Changed();
    if (Slider(kIdDrift, x2, y, colW, L"Drifts", s.drift, 0, 200, false, DriftText(s.drift))) Changed();
    y += 38.0f;
    if (Slider(kIdTreeAmount, kMargin, y, colW, L"Trees", s.treeAmount, 0, 100, false, TreeText(s.treeAmount))) Changed();
    if (Slider(kIdSnowmanAmount, x2, y, colW, L"Snowmen", s.snowmanAmount, 0, 100, false, SnowmanText(s.snowmanAmount)))
        Changed();

    // --- Colour -------------------------------------------------------------
    y += 30.0f;
    Text(L"Color", Rect(kMargin, y, 62.0f, 32.0f), fmtBody_.Get(), theme_.text);
    const wchar_t* colorNames[] = {L"White", L"Ice blue", L"Rainbow", L"Custom…"};
    for (int i = 0; i < static_cast<int>(SnowColor::Count); ++i) {
        D2D1_RECT_F r = Rect(kMargin + 72.0f + static_cast<float>(i) * 150.0f, y, 140.0f, 32.0f);
        SnowColor c = static_cast<SnowColor>(i);
        if (ColorChip(kIdColor + i, r, c, colorNames[i], s.color == c)) {
            if (c == SnowColor::Custom) {
                action_ = Action::CustomColor;  // modal dialog after EndDraw
            } else if (s.color != c) {
                s.color = c;
                Changed();
            }
        }
    }

    // --- Switches -----------------------------------------------------------
    y += 46.0f;
    Fill(Rect(kMargin, y, kClientW - 2.0f * kMargin, 1.0f), 0.0f, theme_.border);
    y += 22.0f;
    if (Toggle(kIdPause, kMargin, y, colW, L"Pause snow", s.paused)) Changed();
    if (Toggle(kIdMouse, x2, y, colW, L"Mouse plays with snow", s.mouse)) Changed();
    y += 32.0f;
    if (Toggle(kIdShadow, kMargin, y, colW, L"Outline flakes on light windows", s.shadow)) Changed();
    if (Toggle(kIdFullscreen, x2, y, colW, L"Hide during fullscreen apps", s.pauseFullscreen)) Changed();
    y += 32.0f;
    if (Toggle(kIdMonitors, kMargin, y, colW, L"Snow on all monitors", s.allMonitors)) Changed();
    if (Toggle(kIdBehind, x2, y, colW, L"Snow behind windows", s.behindWindows)) Changed();
    y += 32.0f;
    if (Toggle(kIdTrees, kMargin, y, colW, L"Show trees", s.trees)) Changed();
    if (Toggle(kIdSnowmen, x2, y, colW, L"Show snowmen", s.snowmen)) Changed();
    y += 32.0f;
    bool startup = startWithWindows_;
    // (Only a real window changes it: a snapshot has none.)
    if (Toggle(kIdStartup, kMargin, y, colW, L"Start with Windows", startup) && hwnd_) {
        if (SetStartWithWindows(startup)) startWithWindows_ = startup;
    }
}

void SettingsWindow::LightsPage(Settings& s, float y) {
    // --- Bulbs --------------------------------------------------------------
    // The switch for them all sits above the bulbs, so it comes first in the tab order.
    const float showW = 52.0f + showLightsW_ + 8.0f;
    if (Toggle(kIdLights, kClientW - kMargin - showW + 4.0f, y + 12.0f, showW, L"Show lights", s.lights)) Changed();
    Text(L"Holiday lights", Rect(kMargin, y, lightsTitleW_ + 4.0f, 24.0f), fmtSection_.Get(), theme_.text);

    // Turned off, the rest fades back (still adjustable, for the next time they go up).
    const bool dim = !s.lights;
    if (dim)
        rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), 0.5f),
                       nullptr);
    for (int t = 0; t < kBulbTypeCount; ++t) {
        const BulbType type = static_cast<BulbType>(t);
        D2D1_RECT_F r = Rect(Snap(kMargin + static_cast<float>(t) * (kTile + kTileGap)), Snap(y + 30.0f), kTile, kTile);
        if (BulbTile(kIdBulb + t, r, type, s.bulb == type) && s.bulb != type) {
            s.bulb = type;
            Changed();
        }
    }
    GroupStop(kIdBulb + static_cast<int>(s.bulb));
    const wchar_t* hint = hoveredTile_ >= 0 ? BulbTypeName(static_cast<BulbType>(hoveredTile_)) : L"Pick a bulb";
    Text(hint, Rect(kMargin + lightsTitleW_ + 16.0f, y, 240.0f, 24.0f), fmtSmall_.Get(), theme_.subtext);

    // --- Colours and pattern -------------------------------------------------
    // Rows of chips sized to their words, spread to the width of the bulbs above.
    const float rowW = kClientW - 2.0f * kMargin;
    auto row = [&](float top, int count, const float* textW, float lead, auto&& chip) {
        float natural = 0.0f;
        for (int i = 0; i < count; ++i) natural += textW[i] + lead + kChipPad;
        const float extra = std::max(0.0f, (rowW - natural - static_cast<float>(count - 1) * kChipGap) / count);
        float x = kMargin;
        for (int i = 0; i < count; ++i) {
            const float w = textW[i] + lead + kChipPad + extra;
            chip(i, Rect(Snap(x), top, Snap(x + w) - Snap(x), kChipH));
            x += w + kChipGap;
        }
    };
    y += 30.0f + kTile + 14.0f;
    Fill(Rect(kMargin, y, rowW, 1.0f), 0.0f, theme_.border);
    y += 14.0f;
    Text(L"Colors", Rect(kMargin, y, 200.0f, 24.0f), fmtBody_.Get(), theme_.text);
    y += 26.0f;
    row(y, kLightColorCount, lightColorW_, kChipSwatch, [&](int i, const D2D1_RECT_F& r) {
        const LightColors c = static_cast<LightColors>(i);
        if (Chip(kIdLightColor + i, r, LightColorLabel(c), s.lightColors == c, true)) {
            if (c == LightColors::Custom) {
                action_ = Action::LightColor;  // modal dialog after EndDraw
                lightColorTurnsOn_ = false;
            } else if (s.lightColors != c) {
                s.lightColors = c;
                Changed();
            }
        }
        const float cx = r.left + 15.0f, cy = (r.top + r.bottom) * 0.5f;
        const COLORREF custom = s.lightCustomColor;
        if (c == LightColors::Multicolor) {
            Circle(cx - 3.2f, cy - 3.2f, 3.9f, LightColor(c, custom, 0));
            Circle(cx + 3.2f, cy - 3.2f, 3.9f, LightColor(c, custom, 3));
            Circle(cx - 3.2f, cy + 3.2f, 3.9f, LightColor(c, custom, 2));
            Circle(cx + 3.2f, cy + 3.2f, 3.9f, LightColor(c, custom, 1));
        } else if (c == LightColors::RedGreen || c == LightColors::BlueWhite) {
            Swatch(cx - 3.0f, cy, 5.5f, LightColor(c, custom, 0));
            Swatch(cx + 3.0f, cy, 5.5f, LightColor(c, custom, 1));
        } else {
            Swatch(cx, cy, 7.0f, LightColor(c, custom, 0));
        }
    });
    GroupStop(kIdLightColor + static_cast<int>(s.lightColors));
    y += kChipH + 16.0f;
    Text(L"Animation", Rect(kMargin, y, 200.0f, 24.0f), fmtBody_.Get(), theme_.text);
    y += 26.0f;
    row(y, kAnimationCount, animationW_, kChipPad, [&](int i, const D2D1_RECT_F& r) {
        const LightAnimation a = static_cast<LightAnimation>(i);
        if (Chip(kIdAnimation + i, r, LightAnimationName(a), s.lightAnimation == a, false) && s.lightAnimation != a) {
            s.lightAnimation = a;
            Changed();
        }
    });
    GroupStop(kIdAnimation + static_cast<int>(s.lightAnimation));

    // --- Sliders and wire -----------------------------------------------------
    y += kChipH + 18.0f;
    Fill(Rect(kMargin, y, rowW, 1.0f), 0.0f, theme_.border);
    y += 26.0f;
    const float colW = (rowW - 24.0f) * 0.5f;
    const float x2 = kMargin + colW + 24.0f;
    if (Slider(kIdLightSpeed, kMargin, y, colW, L"Speed", s.lightSpeed, 0, 100, false, LightSpeedText(s.lightSpeed)))
        Changed();
    if (Slider(kIdLightSize, x2, y, colW, L"Size", s.lightSize, 0, 100, false, SizeText(s.lightSize))) Changed();
    y += 38.0f;
    if (Slider(kIdLightGlow, kMargin, y, colW, L"Glow", s.lightGlow, 0, 100, false, GlowText(s.lightGlow))) Changed();
    // The wire's colour, in the slider grid's last cell: chips where the track would start.
    Text(L"Wire", Rect(x2, y - 12.0f, 62.0f, 24.0f), fmtBody_.Get(), theme_.text);
    float wx = x2 + 72.0f;
    for (int i = 0; i < kWireCount; ++i) {
        const WireColor w = static_cast<WireColor>(i);
        const float cw = wireW_[i] + kChipSwatch + kChipPad;
        const D2D1_RECT_F r = Rect(Snap(wx), y - 0.5f * kChipH, Snap(wx + cw) - Snap(wx), kChipH);
        if (Chip(kIdWire + i, r, WireColorName(w), s.wire == w, true) && s.wire != w) {
            s.wire = w;
            Changed();
        }
        Swatch(r.left + 15.0f, y, 7.0f, WireRgb(w));
        wx += cw + kChipGap;
    }
    GroupStop(kIdWire + static_cast<int>(s.wire));
    if (dim) rt_->PopLayer();

    // --- Strands --------------------------------------------------------------
    // Two cards rather than chips, so each layout can say what it means; the
    // button that hangs the user's own lives on theirs.
    y += 30.0f;
    Fill(Rect(kMargin, y, rowW, 1.0f), 0.0f, theme_.border);
    y += 18.0f;
    const bool any = !s.strands.empty();
    // Clearing asks for a second click: the strands may have taken a while to hang.
    if (any) {
        const double now = QpcSeconds();
        const bool armed = now < clearArmed_;
        if (Link(kIdClearStrands, Rect(kClientW - kMargin - 160.0f, y, 160.0f, 24.0f),
                 armed ? L"Click again to clear them" : L"Clear my strands")) {
            if (armed) {
                s.strands.clear();
                s.lightLayout = LightLayout::AcrossTop;
                clearArmed_ = 0.0;
                Changed();
            } else {
                clearArmed_ = now + 4.0;
            }
        }
    }
    Text(L"Strands", Rect(kMargin, y, strandsTitleW_ + 4.0f, 24.0f), fmtSection_.Get(), theme_.text);
    Text(L"Where the lights hang", Rect(kMargin + strandsTitleW_ + 16.0f, y, 240.0f, 24.0f), fmtSmall_.Get(),
         theme_.subtext);

    y += 32.0f;
    std::wstring own = L"None yet: hang your own, on hooks wherever you like";
    if (any) {
        // How many, and on how many screens.
        std::vector<const std::wstring*> monitors;
        for (const LightStrand& st : s.strands)
            if (std::none_of(monitors.begin(), monitors.end(), [&](const std::wstring* m) { return *m == st.monitor; }))
                monitors.push_back(&st.monitor);
        own = std::to_wstring(s.strands.size()) + (s.strands.size() == 1 ? L" strand on " : L" strands on ") +
              std::to_wstring(monitors.size()) + (monitors.size() == 1 ? L" screen" : L" screens") +
              L", hung where you put them";
    }
    // The arrow keys can reach a card that can't be chosen yet: keep the keyboard on the other.
    if (!any && focus_ == kIdLayout + static_cast<int>(LightLayout::Custom))
        focus_ = kIdLayout + static_cast<int>(LightLayout::AcrossTop);
    const D2D1_RECT_F topCard = Rect(kMargin, Snap(y), rowW, kCardH);
    const D2D1_RECT_F ownCard = Rect(kMargin, Snap(y + kCardH + 8.0f), rowW, kCardH);
    const D2D1_RECT_F hang = Rect(ownCard.right - 12.0f - kHangW, Snap((ownCard.top + ownCard.bottom) * 0.5f - 16.0f),
                                  kHangW, 32.0f);
    if (StrandCard(kIdLayout + static_cast<int>(LightLayout::AcrossTop), topCard, topCard.right, LightLayout::AcrossTop,
                   L"A drape along the top of every screen", s.lightLayout == LightLayout::AcrossTop, true) &&
        s.lightLayout != LightLayout::AcrossTop) {
        s.lightLayout = LightLayout::AcrossTop;
        Changed();
    }
    // Their own strands can be chosen once there are some to show.
    if (StrandCard(kIdLayout + static_cast<int>(LightLayout::Custom), ownCard, hang.left - 12.0f, LightLayout::Custom,
                   own.c_str(), s.lightLayout == LightLayout::Custom, any) &&
        s.lightLayout != LightLayout::Custom) {
        s.lightLayout = LightLayout::Custom;
        Changed();
    }
    GroupStop(kIdLayout + static_cast<int>(s.lightLayout));
    if (Button(kIdHang, hang, L"Hang lights…", true)) PostMessageW(app_, WM_APP_HANG_LIGHTS, 0, 0);
}

void SettingsWindow::Footer(Settings& s) {
    const float by = kClientH - 16.0f - 32.0f;
    const float defaultsX = kClientW - kMargin - 236.0f;
    if (Button(kIdClear, Rect(kMargin, by, 130.0f, 32.0f), L"Clear drifts", false)) {
        PostMessageW(app_, WM_APP_CLEAR_DRIFTS, 0, 0);
        if (preview_) preview_->ClearDrift(true);
    }
    // Centred in the gap between Clear drifts and Defaults; opens the project page.
    const float creditX = (kMargin + 130.0f + defaultsX) * 0.5f;
    if (Link(kIdCredit, Rect(creditX - 110.0f, by, 220.0f, 32.0f), L"Created by Joshua Starr, 2026", fmtButton_.Get()))
        action_ = Action::OpenRepo;
    if (Button(kIdDefaults, Rect(defaultsX, by, 110.0f, 32.0f), L"Defaults", false)) {
        // Pausing is a state rather than a preference, and the strands the
        // user hung are their own work: both stay (and keep showing).
        Settings d;
        d.paused = s.paused;
        d.strands = s.strands;
        if (!d.strands.empty()) d.lightLayout = s.lightLayout;
        s = d;
        Changed();
    }
    if (Button(kIdDone, Rect(kClientW - kMargin - 110.0f, by, 110.0f, 32.0f), L"Done", true)) action_ = Action::Close;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void SettingsWindow::Render() {
    if (!hwnd_ && !rt_) return;  // closed (an offscreen snapshot has a target but no window)
    if (!rt_ && FAILED(CreateResources())) {
        ReleaseResources();
        return;
    }
    const double now = QpcSeconds();
    // Rebuild the atlas for a changed outline setting; retry a failed build about once a second.
    if ((!atlas_.Ready() || atlasShadow_ != settings_.shadow) && now >= atlasRetry_) {
        FlakeArtOptions opt;
        opt.shadow = settings_.shadow;
        if (SUCCEEDED(atlas_.Build(dc_.Get(), wic_.Get(), opt))) atlasShadow_ = settings_.shadow;
        else atlasRetry_ = now + 1.0;
    }
    // The bulb tiles show the chosen colours and wire: drawn again when they change.
    if (tab_ == Tab::Lights && bulbLook_ != BulbLook{settings_.lightColors, settings_.lightCustomColor, settings_.wire})
        BuildBulbImages();

    // Advance the preview, letting the mouse play with it.
    float dt = last_ > 0.0 ? static_cast<float>(now - last_) : 1.0f / 60.0f;
    last_ = now;
    dt = Clamp(dt, 0.0f, 0.1f);
    CursorState cursor;
    if (input_.inside && Contains(previewRect_, input_.mouse)) {
        if (lastMouseValid_ && dt > 0.0f) {
            float k = ApproachFactor(dt, 0.03f);
            mouseVx_ += ((input_.mouse.x - lastMouse_.x) * scale_ / dt - mouseVx_) * k;
            mouseVy_ += ((input_.mouse.y - lastMouse_.y) * scale_ / dt - mouseVy_) * k;
        }
        lastMouse_ = input_.mouse;
        lastMouseValid_ = true;
        cursor.valid = true;
        cursor.x = (input_.mouse.x - Snap(previewRect_.left)) * scale_;
        cursor.y = (input_.mouse.y - Snap(previewRect_.top)) * scale_;
        cursor.vx = mouseVx_;
        cursor.vy = mouseVy_;
    } else {
        lastMouseValid_ = false;
        mouseVx_ = mouseVy_ = 0.0f;
    }
    if (preview_) preview_->Update(dt, now - origin_, cursor);

    // Keyboard focus traversal uses the previous frame's widget order, in
    // which each group of tiles or chips is one stop (any of its members
    // matches it). Ctrl+Tab switches tabs, as in other tabbed windows.
    const bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
    for (UINT k : input_.keys) {
        if (k == VK_TAB && ctrl) {
            SelectTab(tab_ == Tab::Snow ? Tab::Lights : Tab::Snow);
        } else if (k == VK_TAB && !prevOrder_.empty()) {
            auto it = std::find_if(prevOrder_.begin(), prevOrder_.end(),
                                   [&](int id) { return id == focus_ || SameGroup(id, focus_); });
            size_t n = prevOrder_.size();
            size_t idx = it == prevOrder_.end() ? (shift ? n - 1 : 0)
                                                : (static_cast<size_t>(it - prevOrder_.begin()) + (shift ? n - 1 : 1)) % n;
            focus_ = prevOrder_[idx];
            focusVisible_ = true;
        } else if (const Group* g = GroupOf(focus_); g && (k == VK_LEFT || k == VK_RIGHT || k == VK_UP || k == VK_DOWN)) {
            // In a single row, up and down step along it like left and right.
            const bool oneRow = g->cols >= g->count;
            int i = focus_ - g->first;
            if (k == VK_LEFT || (oneRow && k == VK_UP)) i -= 1;
            else if (k == VK_RIGHT || (oneRow && k == VK_DOWN)) i += 1;
            else if (k == VK_UP) i -= g->cols;
            else i += g->cols;
            if (i >= 0 && i < g->count) {  // nothing beyond the edges
                focus_ = g->first + i;
                if (g->first == kIdTab) SelectTab(static_cast<Tab>(i));  // tabs switch as they are reached
            }
            focusVisible_ = true;
        }
    }

    hot_ = 0;
    hoveredTile_ = -1;
    order_.clear();
    Settings& s = settings_;

    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->Clear(theme_.bg);

    DrawPreview(previewRect_);
    TabBar(kTabsTop);
    if (tab_ == Tab::Snow) SnowPage(s, kPageTop);
    else LightsPage(s, kPageTop);
    Footer(s);

    // End of frame bookkeeping.
    if (input_.released) active_ = 0;
    if (hot_ != wheelSlider_) wheelSlider_ = 0;  // left the slider: its wheel remainder is dropped
    input_.pressed = input_.released = false;
    input_.wheel = 0.0f;
    input_.keys.clear();
    prevOrder_ = order_;

    HRESULT hr = rt_->EndDraw();
    if (SUCCEEDED(hr) && cloaked_ && hwndRt_) Cloak(false);  // the first frame is up: reveal the window
    if (hr == D2DERR_RECREATE_TARGET) ReleaseResources();

    if (changed_) {
        changed_ = false;
        if (preview_) preview_->SetParams(PreviewParams());
        PostMessageW(app_, WM_APP_SETTINGS_CHANGED, 0, 0);
    }
    // Not while a dialog runs: WM_PAINT can render from inside its loop.
    if (action_ != Action::None && !busy_) RunAction();
}

void SettingsWindow::RunAction() {
    Action a = action_;
    action_ = Action::None;
    if (!hwnd_) return;  // an offscreen snapshot: no window to open anything from
    if (a == Action::Close) {
        Close();
    } else if (a == Action::OpenRepo) {
        ShellExecuteW(hwnd_, L"open", L"https://" SNOWYSTARR_REPO, nullptr, nullptr, SW_SHOWNORMAL);
    } else if (a == Action::CustomColor || a == Action::LightColor) {
        // Each picker remembers its own custom colours: pale ones for snow, festive ones for lights.
        static COLORREF snowCustom[16] = {RGB(255, 255, 255), RGB(200, 230, 255), RGB(255, 220, 235), RGB(220, 255, 230),
                                          RGB(255, 245, 200)};
        static COLORREF lightCustom[16] = {RGB(255, 190, 110), RGB(255, 48, 48), RGB(47, 213, 91), RGB(61, 123, 255),
                                           RGB(255, 201, 58), RGB(168, 85, 247)};
        const bool light = a == Action::LightColor;
        const bool turnOn = std::exchange(lightColorTurnsOn_, false);
        CHOOSECOLORW cc{sizeof(cc)};
        cc.hwndOwner = hwnd_;
        cc.rgbResult = light ? settings_.lightCustomColor : settings_.customColor;
        cc.lpCustColors = light ? lightCustom : snowCustom;
        cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
        // The dialog's message loop may close the window (e.g. Exit from the
        // tray); Busy() keeps the app from deleting this object meanwhile.
        busy_ = true;
        const BOOL ok = ChooseColorW(&cc);
        busy_ = false;
        // The dialog swallowed the button release.
        input_.down = false;
        active_ = 0;
        if (!hwnd_) return;
        if (ok) {
            if (light) {
                settings_.lightCustomColor = cc.rgbResult;
                settings_.lightColors = LightColors::Custom;
                if (turnOn) settings_.lights = true;
            } else {
                settings_.customColor = cc.rgbResult;
                settings_.color = SnowColor::Custom;
            }
            if (preview_) preview_->SetParams(PreviewParams());
            PostMessageW(app_, WM_APP_SETTINGS_CHANGED, 0, 0);
        }
    }
}
