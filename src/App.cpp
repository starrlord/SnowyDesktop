#include "App.h"

#include "SettingsWindow.h"
#include "resource.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>
#include <wtsapi32.h>

namespace {

constexpr UINT kTrayId = 1;
constexpr wchar_t kOverlayClass[] = L"snowystarrOverlay";
constexpr wchar_t kRegKey[] = L"Software\\snowystarr";

// Private messages (Common.h owns WM_APP + 1..).
constexpr UINT WM_APP_RAISE = WM_APP + 50;
constexpr UINT WM_APP_CHECK_FULLSCREEN = WM_APP + 51;
constexpr UINT WM_APP_REBUILD = WM_APP + 52;

enum TimerId : UINT_PTR {
    kTimerRebuild = 1,  // debounced display/taskbar change
    kTimerFade,         // overlay fade in/out
    kTimerWatch,        // periodic fullscreen check + z-order fallback
    kTimerRaise,        // delayed z-order re-check
    kTimerSave,         // debounced settings save
    kTimerTray,         // retry adding the tray icon (Explorer not ready yet)
};

enum MenuId : UINT {
    IDM_PAUSE = 100,
    IDM_SETTINGS,
    IDM_ABOUT,
    IDM_EXIT,
    IDM_CLEAR_DRIFTS,
    IDM_MOUSE,
    IDM_FULLSCREEN,
    IDM_ALL_MONITORS,
    IDM_STARTUP,
    IDM_SHADOW,
    IDM_GUSTY,
    IDM_FLAKES_ALL = 200,
    IDM_FLAKES_SOFT,
    IDM_FLAKES_CRYSTALS,
    IDM_FLAKES_CLASSIC,
    IDM_FLAKES_SIMPLE,
    IDM_FLAKES_CHOOSE,
    IDM_AMOUNT = 300,  // + preset index
    IDM_SPEED = 320,
    IDM_WIND = 340,
    IDM_DRIFT = 360,
    IDM_COLOR = 380,  // + SnowColor
};

App* g_app = nullptr;

bool IsShellWindow(HWND hwnd) {
    wchar_t cls[64];
    if (!GetClassNameW(hwnd, cls, 64)) return false;
    return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 || wcscmp(cls, L"Shell_TrayWnd") == 0 ||
           wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

bool IsTaskbarWindow(HWND hwnd) {
    wchar_t cls[64];
    if (!GetClassNameW(hwnd, cls, 64)) return false;
    return wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

// True when `hwnd` covers its whole monitor (borderless fullscreen games,
// F11 browsers, video players). Returns the monitor through `out`.
bool IsFullscreenWindow(HWND hwnd, HMONITOR& out) {
    if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd) || IsShellWindow(hwnd)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return false;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;

    RECT r;
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if (style & (WS_CAPTION | WS_THICKFRAME)) {
        if (!GetClientRect(hwnd, &r)) return false;
        MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
    } else if (!GetWindowRect(hwnd, &r)) {
        return false;
    }
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi)) return false;
    const RECT& m = mi.rcMonitor;
    if (r.left <= m.left && r.top <= m.top && r.right >= m.right && r.bottom >= m.bottom) {
        out = mon;
        return true;
    }
    return false;
}

DWORD OsBuild() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    static const DWORD build = [] {
        OSVERSIONINFOW vi{sizeof(vi)};
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(
                reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))))
            fn(&vi);
        return vi.dwBuildNumber;
    }();
    return build;
}

// Lets popup menus follow the system dark/light app theme (Windows 10 1903+).
// Uses the long-stable, undocumented uxtheme ordinals that Explorer and many
// Win32 apps rely on; skipped silently if unavailable.
void EnableDarkMenus() {
    if (OsBuild() < 18362) return;
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    auto setMode = reinterpret_cast<SetPreferredAppModeFn>(
        reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(135))));
    auto flush = reinterpret_cast<FlushMenuThemesFn>(
        reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(136))));
    if (setMode) setMode(1);  // AllowDark: follow the system setting
    if (flush) flush();
}

void FlushDarkMenus() {
    if (OsBuild() < 18362) return;
    HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
    if (!ux) return;
    using FlushMenuThemesFn = void(WINAPI*)();
    if (auto flush = reinterpret_cast<FlushMenuThemesFn>(
            reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(136)))))
        flush();
}

// Opts the process out of EcoQoS so Windows doesn't park the render threads on
// slow cores while the app has no foreground window (keeps frames on time).
void DisablePowerThrottling() {
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
}

DWORD ReadRegDword(const wchar_t* name, DWORD fallback) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return fallback;
    return value;
}

void WriteRegDword(const wchar_t* name, DWORD value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, name, REG_DWORD, &value, sizeof(value));
}

// Tray tooltip: what the snow is doing right now.
std::wstring TrayTip(const Settings& s) {
    if (s.paused) return L"snowystarr — paused";
    const wchar_t* amount = s.amount < 20 ? L"Flurries" : s.amount < 38 ? L"Light snow"
                          : s.amount < 58 ? L"Snowing" : s.amount < 85 ? L"Heavy snow" : L"Blizzard";
    return std::wstring(L"snowystarr — ") + amount;
}

template <size_t N>
int MatchPreset(const Preset (&presets)[N], int value) {
    for (size_t i = 0; i < N; ++i)
        if (presets[i].value == value) return static_cast<int>(i);
    return -1;
}

bool SameTargets(const std::vector<MonitorTarget>& a, const std::vector<MonitorTarget>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const MonitorTarget& x = a[i];
        const MonitorTarget& y = b[i];
        if (x.key != y.key || x.groundY != y.groundY || x.dpi != y.dpi || x.refreshHz != y.refreshHz ||
            x.leftOpen != y.leftOpen || x.rightOpen != y.rightOpen || x.primaryCadence != y.primaryCadence)
            return false;
    }
    return true;
}

}  // namespace

App::App() { g_app = this; }

App::~App() {
    DestroyOverlays();
    if (icon_) DestroyIcon(icon_);
    if (iconPaused_) DestroyIcon(iconPaused_);
    g_app = nullptr;
}

bool App::Init(HINSTANCE instance) {
    instance_ = instance;
    clockOrigin_ = QpcSeconds();
    settings_ = LoadSettings();

    DisablePowerThrottling();
    EnableDarkMenus();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    if (!RegisterClassExW(&wc)) return false;

    WNDCLASSEXW oc{sizeof(oc)};
    oc.lpfnWndProc = OverlayProc;
    oc.hInstance = instance;
    oc.lpszClassName = kOverlayClass;
    oc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&oc)) return false;

    // Hidden top-level window: receives tray callbacks and the display /
    // settings broadcasts that message-only windows miss.
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                            instance, this);
    if (!hwnd_) return false;

    LoadIconMetric(instance, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &icon_);
    LoadIconMetric(instance, MAKEINTRESOURCEW(IDI_APP_PAUSED), LIM_SMALL, &iconPaused_);
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(hwnd_, taskbarCreatedMsg_, MSGFLT_ALLOW, nullptr);
    AddTrayIcon();

    WTSRegisterSessionNotification(hwnd_, NOTIFY_FOR_THIS_SESSION);
    displayNotify_ = RegisterPowerSettingNotification(hwnd_, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);

    // Z-order changes anywhere (e.g. the taskbar being clicked) and
    // foreground changes (fullscreen apps) are reported to this thread.
    reorderHook_ = SetWinEventHook(EVENT_OBJECT_REORDER, EVENT_OBJECT_REORDER, nullptr, WinEventProc, 0, 0,
                                   WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    foregroundHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    CheckFullscreen();
    UpdateFrameCap();
    RebuildOverlays(true);
    SetTimer(hwnd_, kTimerWatch, 1000, nullptr);
    ShowFirstRunTip();
    return true;
}

int App::Run() {
    // While the settings window is open its live preview renders from the
    // idle loop (paced by vsync in EndDraw). Rendering from WM_PAINT with
    // continuous invalidation would starve WM_TIMER, which drives fades and
    // the z-order watchdog.
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return static_cast<int>(msg.wParam);
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (settingsWindow_ && settingsWindow_->WantsFrame()) {
            const double start = QpcSeconds();
            const bool drawn = settingsWindow_->Frame();
            // EndDraw normally blocks until vsync. When it doesn't (occluded,
            // remote session), cap the rate instead of spinning a core.
            if (!drawn) MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            else if (QpcSeconds() - start < 0.004) MsgWaitForMultipleObjectsEx(0, nullptr, 8, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        } else {
            WaitMessage();
        }
    }
}

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->hwnd_ = hwnd;
    } else if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (app) app->hwnd_ = nullptr;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return app ? app->HandleMessage(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK App::OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_DPICHANGED:
        // Keep covering the monitor exactly; re-read the geometry instead of
        // accepting the suggested (rescaled) rectangle.
        if (g_app) PostMessageW(g_app->hwnd_, WM_APP_REBUILD, 0, 0);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        // Content comes from DirectComposition; just validate. (ValidateRect
        // alone does not clear the update region of these layered windows,
        // which made the system re-send WM_PAINT in a tight loop.)
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void CALLBACK App::WinEventProc(HWINEVENTHOOK, DWORD event, HWND, LONG, LONG, DWORD, DWORD) {
    if (!g_app || !g_app->hwnd_) return;
    if (event == EVENT_SYSTEM_FOREGROUND) PostMessageW(g_app->hwnd_, WM_APP_CHECK_FULLSCREEN, 0, 0);
    if (!g_app->raisePending_) {
        g_app->raisePending_ = true;
        PostMessageW(g_app->hwnd_, WM_APP_RAISE, 0, 0);
    }
}

LRESULT App::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == taskbarCreatedMsg_ && taskbarCreatedMsg_ != 0) {
        // Explorer restarted: the tray icon is gone and taskbars were recreated.
        trayAdded_ = false;
        AddTrayIcon();
        SetTimer(hwnd_, kTimerRebuild, 1500, nullptr);
        return 0;
    }

    switch (msg) {
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            ShowSettingsWindow();
            break;
        case WM_CONTEXTMENU:
            ShowContextMenu({GET_X_LPARAM(wp), GET_Y_LPARAM(wp)});
            break;
        default:
            break;
        }
        return 0;

    case WM_APP_SHOW_SETTINGS:
        ShowSettingsWindow();
        return 0;

    case WM_APP_SETTINGS_CHANGED:
        if (settingsWindow_) ApplySettings(settingsWindow_->Current());
        return 0;

    case WM_APP_SETTINGS_CLOSED:
        // Ignore a stale notice if the window was reopened in the meantime.
        if (settingsWindow_ && !settingsWindow_->IsOpen()) settingsWindow_.reset();
        return 0;

    case WM_APP_CLEAR_DRIFTS:
        for (Overlay& o : overlays_)
            if (o.renderer) o.renderer->ClearDrift();
        return 0;

    case WM_APP_RENDER_FAILED:
        // Rendering keeps retrying in the background; nothing useful to show
        // beyond not crashing. Rebuild once in case the display set changed.
        SetTimer(hwnd_, kTimerRebuild, 2000, nullptr);
        return 0;

    case WM_APP_RAISE:
        RaiseOverlays();
        return 0;

    case WM_APP_CHECK_FULLSCREEN:
        CheckFullscreen();
        return 0;

    case WM_APP_REBUILD:
        SetTimer(hwnd_, kTimerRebuild, 400, nullptr);
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_DEVICECHANGE:
        SetTimer(hwnd_, kTimerRebuild, 600, nullptr);
        return 0;

    case WM_SETTINGCHANGE:
        if (wp == SPI_SETWORKAREA || (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"TraySettings") == 0))
            SetTimer(hwnd_, kTimerRebuild, 400, nullptr);
        if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) FlushDarkMenus();
        return 0;

    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK) sessionLocked_ = true;
        else if (wp == WTS_SESSION_UNLOCK) sessionLocked_ = false;
        UpdateFrameCap();  // remote connects/disconnects change the cap
        UpdateVisibility();
        return 0;

    case WM_POWERBROADCAST:
        if (wp == PBT_POWERSETTINGCHANGE) {
            auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lp);
            if (setting && IsEqualGUID(setting->PowerSetting, GUID_CONSOLE_DISPLAY_STATE) &&
                setting->DataLength >= sizeof(DWORD)) {
                displayOff_ = *reinterpret_cast<const DWORD*>(setting->Data) == 0;
                UpdateVisibility();
            }
        } else if (wp == PBT_APMRESUMEAUTOMATIC) {
            SetTimer(hwnd_, kTimerRebuild, 1500, nullptr);
        } else if (wp == PBT_APMPOWERSTATUSCHANGE) {
            UpdateFrameCap();  // plugged in / unplugged / battery saver
        }
        return TRUE;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        // Signing out, shutting down, or an installer closing us through the
        // Restart Manager: persist and exit cleanly.
        if (wp) {
            KillTimer(hwnd_, kTimerSave);
            SaveSettings(settings_);
            DestroyWindow(hwnd_);
        }
        return 0;

    case WM_TIMER:
        switch (wp) {
        case kTimerRebuild:
            KillTimer(hwnd_, kTimerRebuild);
            RebuildOverlays(false);
            break;
        case kTimerFade:
            StepFades();
            break;
        case kTimerWatch:
            CheckFullscreen();
            UpdateFrameCap();
            RaiseOverlays();
            break;
        case kTimerTray:
            KillTimer(hwnd_, kTimerTray);
            if (!trayAdded_) AddTrayIcon();
            break;
        case kTimerRaise:
            KillTimer(hwnd_, kTimerRaise);
            RaiseOverlays();
            break;
        case kTimerSave:
            KillTimer(hwnd_, kTimerSave);
            SaveSettings(settings_);
            break;
        default:
            break;
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd_, kTimerSave);
        SaveSettings(settings_);
        settingsWindow_.reset();
        if (reorderHook_) UnhookWinEvent(reorderHook_);
        if (foregroundHook_) UnhookWinEvent(foregroundHook_);
        reorderHook_ = foregroundHook_ = nullptr;
        if (displayNotify_) UnregisterPowerSettingNotification(displayNotify_);
        displayNotify_ = nullptr;
        WTSUnRegisterSessionNotification(hwnd_);
        DestroyOverlays();
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

// ---------------------------------------------------------------------------
// Tray icon and menu
// ---------------------------------------------------------------------------

void App::AddTrayIcon() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = settings_.paused ? iconPaused_ : icon_;
    wcscpy_s(nid.szTip, TrayTip(settings_).c_str());
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (!trayAdded_) {
        // At sign-in Explorer may not be ready; TaskbarCreated also re-adds.
        if (++trayRetries_ <= 30) SetTimer(hwnd_, kTimerTray, 2000, nullptr);
        return;
    }
    trayRetries_ = 0;
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void App::RemoveTrayIcon() {
    if (!trayAdded_) return;
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    trayAdded_ = false;
}

void App::UpdateTrayIcon() {
    if (!trayAdded_) return;
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.hIcon = settings_.paused ? iconPaused_ : icon_;
    wcscpy_s(nid.szTip, TrayTip(settings_).c_str());
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void App::ShowFirstRunTip() {
    if (ReadRegDword(L"Welcomed", 0) || !trayAdded_) return;
    WriteRegDword(L"Welcomed", 1);
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    nid.hBalloonIcon = icon_;
    wcscpy_s(nid.szInfoTitle, L"Let it snow!");
    wcscpy_s(nid.szInfo, L"Click the snowflake in the notification area to choose flakes and settings. "
                         L"Right-click it for quick options.");
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void App::ShowContextMenu(POINT pt) {
    const Settings& s = settings_;
    HMENU menu = CreatePopupMenu();

    AppendMenuW(menu, MF_STRING, IDM_PAUSE, s.paused ? L"&Resume snow" : L"&Pause snow");
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"&Settings...");
    SetMenuDefaultItem(menu, IDM_SETTINGS, FALSE);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Snowflakes
    HMENU flakes = CreatePopupMenu();
    struct FlakePreset {
        UINT id;
        const wchar_t* name;
        uint32_t mask;
    };
    const FlakePreset presets[] = {
        {IDM_FLAKES_ALL, L"&Everything", kAllFlakesMask},
        {IDM_FLAKES_SOFT, L"&Soft snow and sparkles", kSoftMask | FlakeBit(FlakeKind::Sparkle)},
        {IDM_FLAKES_CRYSTALS, L"&Crystals", kCrystalMask},
        {IDM_FLAKES_SIMPLE, L"Simple &shapes", kSimpleMask},
        {IDM_FLAKES_CLASSIC, L"Classic &pixel art", kClassicMask},
    };
    UINT checkedFlakes = 0;
    for (const FlakePreset& p : presets) {
        AppendMenuW(flakes, MF_STRING, p.id, p.name);
        if (s.flakeMask == p.mask) checkedFlakes = p.id;
    }
    if (checkedFlakes) CheckMenuRadioItem(flakes, IDM_FLAKES_ALL, IDM_FLAKES_CLASSIC, checkedFlakes, MF_BYCOMMAND);
    AppendMenuW(flakes, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(flakes, MF_STRING, IDM_FLAKES_CHOOSE, L"C&hoose flakes...");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(flakes), L"Snow&flakes");

    auto presetMenu = [&](const wchar_t* title, const auto& list, UINT base, int value) {
        HMENU sub = CreatePopupMenu();
        const int n = static_cast<int>(std::size(list));
        for (int i = 0; i < n; ++i) AppendMenuW(sub, MF_STRING, base + i, list[i].name);
        int match = MatchPreset(list, value);
        if (match >= 0) CheckMenuRadioItem(sub, base, base + n - 1, base + match, MF_BYCOMMAND);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), title);
        return sub;
    };
    presetMenu(L"&Amount", kAmountPresets, IDM_AMOUNT, s.amount);
    presetMenu(L"S&peed", kSpeedPresets, IDM_SPEED, s.speed);
    HMENU wind = presetMenu(L"&Wind", kWindPresets, IDM_WIND, s.wind);
    AppendMenuW(wind, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(wind, MF_STRING | (s.gusts > 0 ? MF_CHECKED : 0), IDM_GUSTY, L"&Gusty");
    HMENU drift = presetMenu(L"Taskbar &drifts", kDriftPresets, IDM_DRIFT, s.drift);
    AppendMenuW(drift, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(drift, MF_STRING, IDM_CLEAR_DRIFTS, L"&Clear drifts now");

    HMENU color = CreatePopupMenu();
    const wchar_t* colorNames[] = {L"&White", L"&Ice blue", L"&Rainbow", L"&Custom..."};
    for (int i = 0; i < static_cast<int>(SnowColor::Count); ++i)
        AppendMenuW(color, MF_STRING, IDM_COLOR + i, colorNames[i]);
    CheckMenuRadioItem(color, IDM_COLOR, IDM_COLOR + static_cast<int>(SnowColor::Count) - 1,
                       IDM_COLOR + static_cast<int>(s.color), MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(color), L"&Color");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (s.mouse ? MF_CHECKED : 0), IDM_MOUSE, L"&Mouse plays with snow");
    AppendMenuW(menu, MF_STRING | (s.shadow ? MF_CHECKED : 0), IDM_SHADOW, L"Flake &outlines on light windows");
    AppendMenuW(menu, MF_STRING | (s.pauseFullscreen ? MF_CHECKED : 0), IDM_FULLSCREEN,
                L"Hide during &fullscreen apps");
    AppendMenuW(menu, MF_STRING | (s.allMonitors ? MF_CHECKED : 0), IDM_ALL_MONITORS, L"Snow on all &monitors");
    AppendMenuW(menu, MF_STRING | (IsStartWithWindows() ? MF_CHECKED : 0), IDM_STARTUP, L"Start with &Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_ABOUT, L"A&bout snowystarr");
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"E&xit");

    // Required so the menu closes when the user clicks elsewhere.
    SetForegroundWindow(hwnd_);
    UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY;
    flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    UINT cmd = static_cast<UINT>(TrackPopupMenuEx(menu, flags, pt.x, pt.y, hwnd_, nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (cmd) OnCommand(cmd);
}

void App::OnCommand(UINT id) {
    Settings s = settings_;
    switch (id) {
    case IDM_PAUSE: s.paused = !s.paused; break;
    case IDM_SETTINGS: ShowSettingsWindow(); return;
    case IDM_ABOUT: ShowAbout(); return;
    case IDM_EXIT: DestroyWindow(hwnd_); return;
    case IDM_CLEAR_DRIFTS:
        for (Overlay& o : overlays_)
            if (o.renderer) o.renderer->ClearDrift();
        return;
    case IDM_MOUSE: s.mouse = !s.mouse; break;
    case IDM_SHADOW: s.shadow = !s.shadow; break;
    case IDM_FULLSCREEN: s.pauseFullscreen = !s.pauseFullscreen; break;
    case IDM_ALL_MONITORS: s.allMonitors = !s.allMonitors; break;
    case IDM_STARTUP: SetStartWithWindows(!IsStartWithWindows()); return;
    case IDM_GUSTY: s.gusts = s.gusts > 0 ? 0 : 45; break;
    case IDM_FLAKES_ALL: s.flakeMask = kAllFlakesMask; break;
    case IDM_FLAKES_SOFT: s.flakeMask = kSoftMask | FlakeBit(FlakeKind::Sparkle); break;
    case IDM_FLAKES_CRYSTALS: s.flakeMask = kCrystalMask; break;
    case IDM_FLAKES_SIMPLE: s.flakeMask = kSimpleMask; break;
    case IDM_FLAKES_CLASSIC: s.flakeMask = kClassicMask; break;
    case IDM_FLAKES_CHOOSE: ShowSettingsWindow(); return;
    default:
        if (id >= IDM_AMOUNT && id < IDM_AMOUNT + std::size(kAmountPresets)) {
            s.amount = kAmountPresets[id - IDM_AMOUNT].value;
        } else if (id >= IDM_SPEED && id < IDM_SPEED + std::size(kSpeedPresets)) {
            s.speed = kSpeedPresets[id - IDM_SPEED].value;
        } else if (id >= IDM_WIND && id < IDM_WIND + std::size(kWindPresets)) {
            s.wind = kWindPresets[id - IDM_WIND].value;
        } else if (id >= IDM_DRIFT && id < IDM_DRIFT + std::size(kDriftPresets)) {
            s.drift = kDriftPresets[id - IDM_DRIFT].value;
        } else if (id >= IDM_COLOR && id < IDM_COLOR + static_cast<UINT>(SnowColor::Count)) {
            s.color = static_cast<SnowColor>(id - IDM_COLOR);
            if (s.color == SnowColor::Custom) {
                static COLORREF custom[16] = {RGB(255, 255, 255), RGB(200, 230, 255), RGB(255, 220, 235),
                                              RGB(220, 255, 230), RGB(255, 245, 200)};
                CHOOSECOLORW cc{sizeof(cc)};
                cc.hwndOwner = hwnd_;
                cc.rgbResult = s.customColor;
                cc.lpCustColors = custom;
                cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
                SetForegroundWindow(hwnd_);
                if (!ChooseColorW(&cc)) return;
                s.customColor = cc.rgbResult;
            }
        } else {
            return;
        }
        break;
    }
    ApplySettings(s);
}

void App::ShowAbout() {
    TASKDIALOGCONFIG tdc{sizeof(tdc)};
    tdc.hwndParent = nullptr;
    tdc.hInstance = instance_;
    tdc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_ENABLE_HYPERLINKS | TDF_POSITION_RELATIVE_TO_WINDOW;
    tdc.pszWindowTitle = L"About snowystarr";
    tdc.pszMainIcon = MAKEINTRESOURCEW(IDI_APP);
    tdc.pszMainInstruction = L"snowystarr " SNOWYSTARR_VERSION_STR;
    tdc.pszContent =
        L"Snow for your Windows desktop: it drifts across every monitor and settles on the taskbar.";
    tdc.pszFooter = L"Click the tray snowflake for settings; right-click for quick options.";
    tdc.dwCommonButtons = TDCBF_OK_BUTTON;
    SetForegroundWindow(hwnd_);
    TaskDialogIndirect(&tdc, nullptr, nullptr, nullptr);
}

void App::ShowSettingsWindow() {
    if (!settingsWindow_) settingsWindow_ = std::make_unique<SettingsWindow>(instance_, hwnd_);
    settingsWindow_->Show(settings_);
}

void App::ApplySettings(const Settings& next, bool save) {
    Settings s = next;
    s.Sanitize();
    const Settings old = settings_;
    settings_ = s;

    if (old.allMonitors != s.allMonitors) {
        RebuildOverlays(true);
    } else {
        for (Overlay& o : overlays_)
            if (o.renderer) o.renderer->ApplySettings(settings_);
    }
    if (old.paused != s.paused || old.amount != s.amount) UpdateTrayIcon();
    UpdateVisibility();
    if (settingsWindow_) settingsWindow_->Update(settings_);
    if (save) SetTimer(hwnd_, kTimerSave, 800, nullptr);
}

// ---------------------------------------------------------------------------
// Monitors and overlays
// ---------------------------------------------------------------------------

std::vector<MonitorTarget> App::EnumerateMonitors() const {
    struct Mon {
        MonitorTarget t;
        RECT work;
    };
    std::vector<Mon> mons;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto& list = *reinterpret_cast<std::vector<Mon>*>(lp);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoW(m, &mi)) return TRUE;
            Mon mon;
            mon.t.monitor = m;
            mon.t.rect = mi.rcMonitor;
            mon.work = mi.rcWork;
            mon.t.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
            UINT dx = 96, dy = 96;
            if (FAILED(GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
            mon.t.dpi = dx;
            DEVMODEW dm{};
            dm.dmSize = sizeof(dm);
            mon.t.refreshHz = (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
                               dm.dmDisplayFrequency > 1)
                                  ? static_cast<float>(dm.dmDisplayFrequency)
                                  : 60.0f;
            wchar_t key[128];
            swprintf_s(key, L"%s@%ld,%ld,%ld,%ld", mi.szDevice, mi.rcMonitor.left, mi.rcMonitor.top,
                       mi.rcMonitor.right, mi.rcMonitor.bottom);
            mon.t.key = key;
            list.push_back(mon);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&mons));

    // Taskbars: the primary one plus one per extra display when the user
    // shows the taskbar on all displays.
    std::vector<std::pair<HMONITOR, RECT>> bars;
    auto addBar = [&](HWND bar) {
        RECT r;
        if (bar && IsWindowVisible(bar) && GetWindowRect(bar, &r))
            bars.emplace_back(MonitorFromWindow(bar, MONITOR_DEFAULTTONEAREST), r);
    };
    addBar(FindWindowW(L"Shell_TrayWnd", nullptr));
    for (HWND w = nullptr; (w = FindWindowExW(nullptr, w, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr;) addBar(w);
    APPBARDATA abd{sizeof(abd)};
    const bool autoHide = (SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) != 0;

    float primaryHz = 60.0f;
    for (const Mon& m : mons)
        if (m.t.primary) primaryHz = m.t.refreshHz;

    std::vector<MonitorTarget> out;
    for (Mon& m : mons) {
        if (!settings_.allMonitors && !m.t.primary) continue;
        MonitorTarget t = m.t;
        const int h = t.rect.bottom - t.rect.top;
        const int w = t.rect.right - t.rect.left;
        int ground = -1;
        if (autoHide) {
            ground = h;  // an auto-hidden taskbar comes and goes: settle on the screen edge
        } else {
            for (const auto& [mon, r] : bars) {
                bool bottomDocked = mon == t.monitor && r.bottom >= t.rect.bottom - 2 && r.top > t.rect.top + h / 2 &&
                                    (r.right - r.left) >= w / 2;
                if (bottomDocked) ground = r.top - t.rect.top;
            }
            if (ground < 0) ground = m.work.bottom - t.rect.top;  // taskbar elsewhere: other appbars or the edge
        }
        t.groundY = Clamp(ground, h / 2, h);
        t.primaryCadence = std::fabs(t.refreshHz - primaryHz) < 0.5f;
        out.push_back(t);
    }

    // Drifts continue across monitors that sit side by side at the same level.
    for (MonitorTarget& a : out) {
        for (const MonitorTarget& b : out) {
            if (&a == &b) continue;
            int ga = a.rect.top + a.groundY, gb = b.rect.top + b.groundY;
            if (std::abs(ga - gb) > 2) continue;
            if (b.rect.left == a.rect.right) a.rightOpen = false;
            if (b.rect.right == a.rect.left) a.leftOpen = false;
        }
    }
    std::sort(out.begin(), out.end(), [](const MonitorTarget& a, const MonitorTarget& b) { return a.key < b.key; });
    return out;
}

void App::RebuildOverlays(bool force) {
    std::vector<MonitorTarget> targets = EnumerateMonitors();
    if (!force) {
        std::vector<MonitorTarget> current;
        for (const Overlay& o : overlays_) current.push_back(o.target);
        if (SameTargets(current, targets)) {
            UpdateVisibility();
            return;
        }
    }

    DestroyOverlays();
    for (MonitorTarget& t : targets) {
        const int w = t.rect.right - t.rect.left, h = t.rect.bottom - t.rect.top;
        HWND hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
                                        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                    kOverlayClass, L"snowystarr", WS_POPUP, t.rect.left, t.rect.top, w, h, nullptr,
                                    nullptr, instance_, nullptr);
        if (!hwnd) continue;
        // A transparent, screen-sized topmost window would otherwise count as
        // a "fullscreen app": the taskbar would drop below other windows and
        // notifications would be suppressed.
        SetPropW(hwnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));
        SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);  // fade in
        // The window may have been created at a different DPI and resized.
        SetWindowPos(hwnd, HWND_TOPMOST, t.rect.left, t.rect.top, w, h, SWP_NOACTIVATE);

        Overlay o;
        o.hwnd = hwnd;
        o.target = t;
        o.target.hwnd = hwnd;
        std::unique_ptr<Scene> scene;
        if (auto it = sceneCache_.find(t.key); it != sceneCache_.end()) {
            scene = std::move(it->second);
            sceneCache_.erase(it);
        }
        o.renderer =
            std::make_unique<MonitorRenderer>(hwnd_, gpuHub_, o.target, std::move(scene), settings_, clockOrigin_);
        o.renderer->SetVisible(false);
        o.renderer->SetFrameCap(frameCap_);
        o.renderer->Start();
        overlays_.push_back(std::move(o));
    }
    // Keep a few scenes from displays that went away (e.g. a monitor that is
    // switched off and on again) so their drifts come back.
    while (sceneCache_.size() > 6) sceneCache_.erase(sceneCache_.begin());
    UpdateVisibility();
}

void App::DestroyOverlays() {
    for (Overlay& o : overlays_) {
        if (o.renderer) {
            if (auto scene = o.renderer->Stop()) sceneCache_[o.target.key] = std::move(scene);
            o.renderer.reset();
        }
        if (o.hwnd) DestroyWindow(o.hwnd);
    }
    overlays_.clear();
}

void App::UpdateVisibility() {
    for (Overlay& o : overlays_) {
        bool fullscreen = fullscreenAll_ || std::find(fullscreenMonitors_.begin(), fullscreenMonitors_.end(),
                                                      o.target.monitor) != fullscreenMonitors_.end();
        o.wantVisible = !settings_.paused && !sessionLocked_ && !displayOff_ && !userAway_ &&
                        !(settings_.pauseFullscreen && fullscreen);
        if (o.wantVisible && !o.shown) {
            o.renderer->SetVisible(true);
            ShowWindow(o.hwnd, SW_SHOWNOACTIVATE);
            SetWindowPos(o.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            o.shown = true;
        }
    }
    SetTimer(hwnd_, kTimerFade, 16, nullptr);
}

void App::StepFades() {
    bool active = false;
    const int step = 255 * 16 / 400;  // ~0.4 s fades
    for (Overlay& o : overlays_) {
        int target = o.wantVisible ? 255 : 0;
        if (o.alpha != target) {
            o.alpha = target > o.alpha ? std::min(target, o.alpha + step) : std::max(target, o.alpha - step);
            SetLayeredWindowAttributes(o.hwnd, 0, static_cast<BYTE>(o.alpha), LWA_ALPHA);
            active = true;
        }
        if (!o.wantVisible && o.alpha == 0 && o.shown) {
            ShowWindow(o.hwnd, SW_HIDE);
            o.renderer->SetVisible(false);
            o.shown = false;
        }
    }
    if (!active) KillTimer(hwnd_, kTimerFade);
}

bool App::TaskbarAboveOverlay(HWND overlay) const {
    for (HWND w = GetWindow(overlay, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV))
        if (IsTaskbarWindow(w)) return true;
    return false;
}

void App::RaiseOverlays() {
    raisePending_ = false;
    const double now = QpcSeconds();
    if (now - lastRaise_ < 0.1) {
        // Rate limit so we never ping-pong with another always-on-top app.
        SetTimer(hwnd_, kTimerRaise, 120, nullptr);
        return;
    }
    bool raised = false;
    for (Overlay& o : overlays_) {
        // Only the taskbar matters: other topmost windows (picture-in-picture
        // players, tool palettes) may stay above the snow.
        if (o.shown && TaskbarAboveOverlay(o.hwnd)) {
            SetWindowPos(o.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            raised = true;
        }
    }
    if (raised) {
        lastRaise_ = now;
        SetTimer(hwnd_, kTimerRaise, 60, nullptr);  // the shell sometimes reorders again right after
    }
}

void App::UpdateFrameCap() {
    // Snow is gentle enough to look smooth at 60 fps; save power when it
    // matters. Remote sessions send every frame over the network.
    float cap = 0.0f;
    SYSTEM_POWER_STATUS sps{};
    if (GetSystemMetrics(SM_REMOTESESSION)) {
        cap = 30.0f;
    } else if (GetSystemPowerStatus(&sps)) {
        if (sps.SystemStatusFlag & 1) cap = 30.0f;  // battery saver / energy saver
        else if (sps.ACLineStatus == 0) cap = 60.0f;  // on battery
    }
    if (cap == frameCap_) return;
    frameCap_ = cap;
    for (Overlay& o : overlays_)
        if (o.renderer) o.renderer->SetFrameCap(cap);
}

void App::CheckFullscreen() {
    bool all = false, away = false, primaryBusy = false;
    QUERY_USER_NOTIFICATION_STATE state;
    if (SUCCEEDED(SHQueryUserNotificationState(&state))) {
        all = state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE;
        away = state == QUNS_NOT_PRESENT;  // screen saver, locked, or switched-out session
        // A fullscreen app on the primary display that isn't in the
        // foreground (e.g. a video while working on another screen). Our
        // overlays are NonRudeHWND, so they never cause this themselves.
        primaryBusy = state == QUNS_BUSY;
    }
#ifdef SNOWYSTARR_DEV_TOOLS
    wchar_t ignore[4];
    if (GetEnvironmentVariableW(L"SNOWYSTARR_IGNORE_AWAY", ignore, 4) > 0) away = false;  // test while unattended
#endif
    std::vector<HMONITOR> monitors;
    HMONITOR mon = nullptr;
    if (IsFullscreenWindow(GetForegroundWindow(), mon)) monitors.push_back(mon);
    if (primaryBusy) {
        HMONITOR primary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        if (std::find(monitors.begin(), monitors.end(), primary) == monitors.end()) monitors.push_back(primary);
    }
    if (all != fullscreenAll_ || monitors != fullscreenMonitors_ || away != userAway_) {
        fullscreenAll_ = all;
        userAway_ = away;
        fullscreenMonitors_ = std::move(monitors);
        UpdateVisibility();
    }
}
