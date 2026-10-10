#include "SettingsWindow.h"

#include "resource.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

namespace {

constexpr wchar_t kClass[] = L"snowystarrSettings";
constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
constexpr float kClientW = 720.0f;  // DIPs
constexpr float kClientH = 692.0f;
constexpr float kMargin = 26.0f;

constexpr float kPreviewTop = 16.0f;
constexpr float kPreviewH = 150.0f;
constexpr float kPreviewTaskbar = 22.0f;  // the little taskbar in the preview

constexpr int kTileCols = 10;
constexpr float kTile = 56.0f;
constexpr float kTileGap = 12.0f;

enum Id : int {
    kIdTile = 100,  // + FlakeKind
    kIdAmount = 200, kIdSpeed, kIdSize, kIdWind, kIdGusts, kIdDrift,
    kIdColor = 300,  // + SnowColor
    kIdPause = 400, kIdMouse, kIdShadow, kIdFullscreen, kIdMonitors, kIdStartup,
    kIdClear = 500, kIdDefaults, kIdDone, kIdAllFlakes, kIdDefaultFlakes,
};

bool IsTileId(int id) { return id >= kIdTile && id < kIdTile + kFlakeKindCount; }

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
    return L"Deep";
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

    previewRect_ = Rect(kMargin - 10.0f, kPreviewTop, kClientW - 2.0f * (kMargin - 10.0f), kPreviewH);
    factory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(previewRect_, 8.0f, 8.0f), &previewClip_);
    return S_OK;
}

void SettingsWindow::ApplyTheme() {
    Theme t;
    t.dark = SystemDarkMode();
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
    theme_ = t;
    if (hwnd_) {
        BOOL dark = t.dark;
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
        hwnd_ = CreateWindowExW(0, kClass, L"snowystarr settings", kStyle, rc.left, rc.top, rc.right - rc.left,
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
    } else if (IsIconic(hwnd_)) {
        ShowWindow(hwnd_, SW_RESTORE);
    }
    // With the colour picker up, bring that forward instead of the disabled owner.
    if (hwnd_) SetForegroundWindow(busy_ ? GetLastActivePopup(hwnd_) : hwnd_);
}

void SettingsWindow::RequestCustomColor() {
    if (hwnd_ && !busy_) action_ = Action::CustomColor;
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
    if (!hwnd_ || !IsWindowVisible(hwnd_) || IsIconic(hwnd_)) return false;
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
    return p;
}

void SettingsWindow::ConfigurePreview() {
    if (!preview_) {
        preview_ = std::make_unique<Scene>();
        preview_->SetDriftBoost(7.0f);  // build drifts quickly so the setting is visible
    }
    SceneGeometry geo;
    geo.width = static_cast<int>(std::lround((previewRect_.right - previewRect_.left) * scale_));
    geo.height = static_cast<int>(std::lround((previewRect_.bottom - previewRect_.top) * scale_));
    geo.dpiScale = scale_;
    geo.groundY = std::round((kPreviewH - kPreviewTaskbar) * scale_);
    geo.leftOpen = geo.rightOpen = true;
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
        tracking_ = moving_ = cloaked_ = false;
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
    ComPtr<IWICBitmap> bmp;
    HRESULT hr = wic_->CreateBitmap(static_cast<UINT>(rc.right), static_cast<UINT>(rc.bottom),
                                    GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp);
    if (FAILED(hr)) return hr;
    ReleaseResources();
    D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f * scale_, 96.0f * scale_);
    if (SUCCEEDED(hr = factory_->CreateWicBitmapRenderTarget(bmp.Get(), rtp, &rt_)) &&
        SUCCEEDED(hr = CreateTargetResources())) {
        // Let the preview fill with snow and build a little drift first.
        for (int i = 0; i < 600; ++i) preview_->Update(1.0f / 60.0f, i / 60.0, CursorState{});
        last_ = 0.0;
        Render();
    }
    ReleaseResources();  // the next frame recreates the window target
    if (FAILED(hr)) return hr;

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(hr = wic_->CreateStream(&stream))) return hr;
    if (FAILED(hr = stream->InitializeFromFilename(path, GENERIC_WRITE))) return hr;
    if (FAILED(hr = wic_->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) return hr;
    enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    enc->CreateNewFrame(&frame, nullptr);
    frame->Initialize(nullptr);
    hr = frame->WriteSource(bmp.Get(), nullptr);
    frame->Commit();
    enc->Commit();
    return hr;
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

void SettingsWindow::FocusRing(int id, const D2D1_RECT_F& r, float radius) {
    if (focusVisible_ && focus_ == id) Stroke(Inflate(r, 3.0f), radius + 3.0f, theme_.focus, 2.0f);
}

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------

void SettingsWindow::Interact(int id, const D2D1_RECT_F& r, bool& hovered, bool& held, bool& clicked) {
    if (!IsTileId(id)) order_.push_back(id);  // the tile grid is a single tab stop (see Render)
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

bool SettingsWindow::Link(int id, const D2D1_RECT_F& r, const wchar_t* text) {
    bool hovered, held, clicked;
    Interact(id, r, hovered, held, clicked);
    Text(text, r, fmtValue_.Get(), hovered ? Mix(theme_.accent, theme_.text, 0.25f) : theme_.accent);
    FocusRing(id, r, 4.0f);
    return clicked;
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
    if (selected) {
        Stroke(r, 8.0f, theme_.accent, 2.0f);
        // Check badge.
        const float cx = r.right - 10.0f, cy = r.top + 10.0f;
        Circle(cx, cy, 7.0f, theme_.accent);
        brush_->SetColor(theme_.onAccent);
        rt_->DrawLine({cx - 3.2f, cy + 0.2f}, {cx - 0.8f, cy + 2.6f}, brush_.Get(), 1.6f);
        rt_->DrawLine({cx - 0.8f, cy + 2.6f}, {cx + 3.4f, cy - 2.4f}, brush_.Get(), 1.6f);
    } else if (!theme_.dark) {
        Stroke(r, 8.0f, Mix(theme_.tile, theme_.bg, 0.5f), 1.0f);
    }
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
        Circle(cx, cy, 8.0f, Rgb(0x9A9A9A));
        Circle(cx, cy, 7.0f,
               D2D1::ColorF(GetRValue(settings_.customColor) / 255.0f, GetGValue(settings_.customColor) / 255.0f,
                            GetBValue(settings_.customColor) / 255.0f));
        break;
    }
    Text(text, {r.left + 32.0f, r.top, r.right - 8.0f, r.bottom}, fmtBody_.Get(), theme_.text);
    FocusRing(id, r, 18.0f);
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

    // The snow itself, drawn exactly like the desktop overlay (in pixels).
    if (preview_ && atlas_.Ready()) {
        D2D1_MATRIX_3X2_F old;
        rt_->GetTransform(&old);
        rt_->SetTransform(D2D1::Matrix3x2F::Scale(1.0f / scale_, 1.0f / scale_) *
                          D2D1::Matrix3x2F::Translation(Snap(r.left), Snap(r.top)) * old);
        preview_->Draw(dc_.Get(), atlas_);
        rt_->SetTransform(old);
    }

    Text(settings_.paused ? L"Preview • snow is paused on the desktop" : L"Preview • move the mouse through the snow",
         Rect(r.left + 12.0f, r.top + 6.0f, 360.0f, 20.0f), fmtSmall_.Get(), Rgb(0xFFFFFF, 0.55f));
    rt_->PopLayer();
    Stroke(r, 8.0f, theme_.border, 1.0f);
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void SettingsWindow::Render() {
    if (!hwnd_) return;
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
    // which the tile grid is one stop (any tile matches it).
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    for (UINT k : input_.keys) {
        if (k == VK_TAB && !prevOrder_.empty()) {
            auto it = std::find_if(prevOrder_.begin(), prevOrder_.end(),
                                   [&](int id) { return id == focus_ || (IsTileId(id) && IsTileId(focus_)); });
            size_t n = prevOrder_.size();
            size_t idx = it == prevOrder_.end() ? (shift ? n - 1 : 0)
                                                : (static_cast<size_t>(it - prevOrder_.begin()) + (shift ? n - 1 : 1)) % n;
            focus_ = prevOrder_[idx];
            focusVisible_ = true;
        } else if (IsTileId(focus_) && (k == VK_LEFT || k == VK_RIGHT || k == VK_UP || k == VK_DOWN)) {
            int t = focus_ - kIdTile;
            if (k == VK_LEFT) t -= 1;
            else if (k == VK_RIGHT) t += 1;
            else if (k == VK_UP) t -= kTileCols;
            else t += kTileCols;
            if (t >= 0 && t < kFlakeKindCount) focus_ = kIdTile + t;  // nothing beyond the edges
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

    // --- Snowflakes -------------------------------------------------------
    // The links sit above the grid, so they come first in the tab order.
    float y = kPreviewTop + kPreviewH + 12.0f;
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
    order_.push_back(IsTileId(focus_) ? focus_ : kIdTile);
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
    if (Slider(kIdDrift, x2, y, colW, L"Drifts", s.drift, 0, 100, false, DriftText(s.drift))) Changed();

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
    bool startup = startWithWindows_;
    if (Toggle(kIdStartup, x2, y, colW, L"Start with Windows", startup)) {
        if (SetStartWithWindows(startup)) startWithWindows_ = startup;
    }

    // --- Footer -------------------------------------------------------------
    y += 30.0f;
    const float by = kClientH - 16.0f - 32.0f;
    (void)y;
    if (Button(kIdClear, Rect(kMargin, by, 130.0f, 32.0f), L"Clear drifts", false)) {
        PostMessageW(app_, WM_APP_CLEAR_DRIFTS, 0, 0);
        if (preview_) preview_->ClearDrift(true);
    }
    if (Button(kIdDefaults, Rect(kClientW - kMargin - 236.0f, by, 110.0f, 32.0f), L"Defaults", false)) {
        Settings d;
        d.paused = s.paused;
        s = d;
        Changed();
    }
    if (Button(kIdDone, Rect(kClientW - kMargin - 110.0f, by, 110.0f, 32.0f), L"Done", true)) action_ = Action::Close;

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
    if (a == Action::Close) {
        Close();
    } else if (a == Action::CustomColor && hwnd_) {
        static COLORREF custom[16] = {RGB(255, 255, 255), RGB(200, 230, 255), RGB(255, 220, 235), RGB(220, 255, 230),
                                      RGB(255, 245, 200)};
        CHOOSECOLORW cc{sizeof(cc)};
        cc.hwndOwner = hwnd_;
        cc.rgbResult = settings_.customColor;
        cc.lpCustColors = custom;
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
            settings_.customColor = cc.rgbResult;
            settings_.color = SnowColor::Custom;
            if (preview_) preview_->SetParams(PreviewParams());
            PostMessageW(app_, WM_APP_SETTINGS_CHANGED, 0, 0);
        }
    }
}
