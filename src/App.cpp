#include "App.h"

#include "Lights.h"
#include "LightsHud.h"
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
constexpr UINT WM_APP_HANG_DONE = WM_APP + 53;  // hanging lights is over (posted: the HUD may be on the stack)

constexpr UINT kHangIdleMs = 10 * 60 * 1000;  // hanging lights ends after this long without input

enum TimerId : UINT_PTR {
    kTimerRebuild = 1,  // debounced display/taskbar change
    kTimerFade,         // overlay fade in/out
    kTimerWatch,        // periodic fullscreen check + z-order fallback
    kTimerRaise,        // delayed z-order re-check
    kTimerSave,         // debounced settings save
    kTimerTray,         // retry adding the tray icon (Explorer not ready yet)
    kTimerHangIdle,     // hanging lights left alone too long
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
    IDM_BEHIND_WINDOWS,
    IDM_TREES,
    IDM_SNOWMEN,
    IDM_LIGHTS,
    IDM_HANG_LIGHTS,
    IDM_LIGHTS_ACROSS_TOP,  // radio pair: keep these two together, in menu order
    IDM_LIGHTS_CUSTOM,
    IDM_FLAKES_RECOMMENDED = 200,  // radio group: keep these contiguous, in menu order
    IDM_FLAKES_ALL,
    IDM_FLAKES_SOFT,
    IDM_FLAKES_CRYSTALS,
    IDM_FLAKES_SIMPLE,
    IDM_FLAKES_CLASSIC,
    IDM_FLAKES_CHOOSE,
    IDM_AMOUNT = 300,  // + preset index
    IDM_SPEED = 320,
    IDM_WIND = 340,
    IDM_DRIFT = 360,
    IDM_COLOR = 380,  // + SnowColor
    IDM_TREE_AMOUNT = 400,  // + preset index
    IDM_SNOWMAN_AMOUNT = 420,
    IDM_BULB = 440,             // + BulbType
    IDM_LIGHT_ANIMATION = 460,  // + LightAnimation
    IDM_LIGHT_COLORS = 480,     // + LightColors
    IDM_LIGHT_SPEED = 500,      // + preset index
};

App* g_app = nullptr;

bool IsTaskbarWindow(HWND hwnd) {
    wchar_t cls[64];
    if (!GetClassNameW(hwnd, cls, 64)) return false;
    return wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

// The top-level window showing the wallpaper and desktop icons: Progman, or on
// older builds the WorkerW it hands the icons to once a wallpaper change has
// been animated.
HWND DesktopWindow() {
    HWND shell = GetShellWindow();
    if (shell && FindWindowExW(shell, nullptr, L"SHELLDLL_DefView", nullptr)) return shell;
    for (HWND w = nullptr; (w = FindWindowExW(nullptr, w, L"WorkerW", nullptr)) != nullptr;)
        if (FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr)) return w;
    return shell;
}

// True when `overlay` is above the desktop with no visible window in between
// (other overlays aside), so that every window covers its snow.
bool RestsOnDesktop(HWND overlay, HWND desktop) {
    for (HWND w = GetWindow(desktop, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV)) {
        if (w == overlay) return true;
        if (!IsWindowVisible(w) || IsIconic(w)) continue;
        wchar_t cls[64];
        if (GetClassNameW(w, cls, 64) && wcscmp(cls, kOverlayClass) == 0) continue;
        BOOL cloaked = FALSE;
        if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) continue;
        return false;
    }
    return false;  // below the desktop, e.g. after Show desktop raised it
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
    if (s.paused) return SNOWYSTARR_APP_NAME L" — paused";
    const wchar_t* amount = s.amount < 20 ? L"Flurries" : s.amount < 38 ? L"Light snow"
                          : s.amount < 58 ? L"Snowing" : s.amount < 85 ? L"Heavy snow" : L"Blizzard";
    return std::wstring(SNOWYSTARR_APP_NAME L" — ") + amount;
}

// The preset nearest to a slider value, so the menu always shows where the
// setting is even after fine-tuning in the settings window.
template <size_t N>
int NearestPreset(const Preset (&presets)[N], int value) {
    int best = 0;
    for (size_t i = 1; i < N; ++i)
        if (std::abs(presets[i].value - value) < std::abs(presets[best].value - value)) best = static_cast<int>(i);
    return best;
}

// GDI names (\\.\DISPLAYn) of the monitors Windows composes in floating point:
// HDR, or Auto Color Management on an SDR display. There it clamps pixels whose
// colour exceeds their alpha, which the trees' lights use to add light.
std::vector<std::wstring> AdvancedColorDisplays() {
    std::vector<std::wstring> out;
    UINT32 paths = 0, modes = 0;
    std::vector<DISPLAYCONFIG_PATH_INFO> path;
    std::vector<DISPLAYCONFIG_MODE_INFO> mode;
    LONG rc = ERROR_INSUFFICIENT_BUFFER;
    // (Displays coming and going between the two calls make the buffers too small: ask again.)
    for (int tries = 0; tries < 4 && rc == ERROR_INSUFFICIENT_BUFFER; ++tries) {
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS) return out;
        path.resize(paths);
        mode.resize(modes);
        rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, path.data(), &modes, mode.data(), nullptr);
    }
    if (rc != ERROR_SUCCESS) return out;
    for (UINT32 i = 0; i < paths; ++i) {
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color{};
        color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        color.header.size = sizeof(color);
        color.header.adapterId = path[i].targetInfo.adapterId;
        color.header.id = path[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&color.header) != ERROR_SUCCESS ||
            !(color.advancedColorEnabled || color.wideColorEnforced))
            continue;
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path[i].sourceInfo.adapterId;
        source.header.id = path[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS) out.emplace_back(source.viewGdiDeviceName);
    }
    return out;
}

// A name as a menu item: an & in it would underline the next letter instead of showing.
std::wstring MenuText(const wchar_t* name) {
    std::wstring out;
    for (const wchar_t* c = name; *c; ++c) {
        if (*c == L'&') out += L'&';
        out += *c;
    }
    return out;
}

// Another app is in front. (No foreground window at all, as during a switch, doesn't count.)
bool OtherAppInFront() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return fg && pid != GetCurrentProcessId();
}

bool SameTargets(const std::vector<MonitorTarget>& a, const std::vector<MonitorTarget>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const MonitorTarget& x = a[i];
        const MonitorTarget& y = b[i];
        if (x.key != y.key || x.monitor != y.monitor || !EqualRect(&x.rect, &y.rect) || x.groundY != y.groundY ||
            x.dpi != y.dpi || x.refreshHz != y.refreshHz || x.leftOpen != y.leftOpen || x.rightOpen != y.rightOpen ||
            x.primaryCadence != y.primaryCadence || x.autoHideGround != y.autoHideGround ||
            x.originScale != y.originScale || x.advancedColor != y.advancedColor)
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
    if (iconLarge_) DestroyIcon(iconLarge_);
    g_app = nullptr;
}

bool App::Init(HINSTANCE instance) {
    instance_ = instance;
    clockOrigin_ = QpcSeconds();
    settings_ = LoadSettings();

    DisablePowerThrottling();
    EnableDarkMenus();
    // A Microsoft Store update closes the running app: have Windows start it again
    // afterwards (not after a reboot, where the startup task decides).
    if (IsPackaged()) RegisterApplicationRestart(nullptr, RESTART_NO_CRASH | RESTART_NO_HANG | RESTART_NO_REBOOT);

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
    LoadIconMetric(instance, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &iconLarge_);
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(hwnd_, taskbarCreatedMsg_, MSGFLT_ALLOW, nullptr);
    AddTrayIcon();

    WTSRegisterSessionNotification(hwnd_, NOTIFY_FOR_THIS_SESSION);
    displayNotify_ = RegisterPowerSettingNotification(hwnd_, &GUID_SESSION_DISPLAY_STATUS, DEVICE_NOTIFY_WINDOW_HANDLE);

    // Z-order changes anywhere (e.g. the taskbar being clicked) and
    // foreground changes (fullscreen apps) are reported to this thread.
    reorderHook_ = SetWinEventHook(EVENT_OBJECT_REORDER, EVENT_OBJECT_REORDER, nullptr, WinEventProc, 0, 0,
                                   WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    foregroundHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    // Windows appearing, disappearing, minimized, restored or moved: the trees
    // and snowmen look for windows over them right away. (Not every location
    // change: that event also fires for every move of the mouse.)
    showHook_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE, nullptr, WinEventProc, 0, 0,
                                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    minimizeHook_ = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, WinEventProc, 0, 0,
                                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    moveSizeHook_ = SetWinEventHook(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND, nullptr, WinEventProc, 0, 0,
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
        // Release a closed settings window only here, where none of its code
        // (e.g. a colour dialog's modal loop) can be on the stack.
        if (settingsWindow_ && !settingsWindow_->IsOpen() && !settingsWindow_->Busy()) settingsWindow_.reset();
        if (settingsWindow_ && settingsWindow_->WantsFrame()) {
            const double start = QpcSeconds();
            const bool drawn = settingsWindow_->Frame();
            // EndDraw normally blocks until vsync. When it clearly didn't
            // (occluded, remote session), wait out the rest of the refresh
            // instead of spinning a core.
            DWM_TIMING_INFO ti{};
            ti.cbSize = sizeof(ti);
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            double period = 1.0 / 60.0;
            if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.qpcRefreshPeriod > 0)
                period = static_cast<double>(ti.qpcRefreshPeriod) / static_cast<double>(f.QuadPart);
            const double took = QpcSeconds() - start;
            if (!drawn) {
                MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            } else if (took < 0.5 * period) {
                const DWORD wait = static_cast<DWORD>((period - took) * 1000.0);
                if (wait > 0) MsgWaitForMultipleObjectsEx(0, nullptr, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
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
    // Hanging lights: the overlays take the pointer for the editor.
    if (g_app && g_app->hanging_) {
        LRESULT result = 0;
        if (g_app->HangInput(hwnd, msg, wp, lp, result)) return result;
    }
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

void CALLBACK App::WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (!g_app || !g_app->hwnd_) return;
    const bool shown = event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_HIDE;
    // Shown or hidden: only whole top-level windows (not every control, caret or cursor).
    if (shown && (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd || GetAncestor(hwnd, GA_ROOT) != hwnd))
        return;
    // Anything that may put a window over the trees and snowmen (or take one away).
    for (Overlay& o : g_app->overlays_)
        if (o.renderer) o.renderer->ProbeCoverSoon();
    if (event == EVENT_SYSTEM_FOREGROUND) PostMessageW(g_app->hwnd_, WM_APP_CHECK_FULLSCREEN, 0, 0);
    if (event != EVENT_OBJECT_REORDER && event != EVENT_SYSTEM_FOREGROUND) return;
    if (!g_app->raisePending_) {
        g_app->raisePending_ = true;
        PostMessageW(g_app->hwnd_, WM_APP_RAISE, 0, 0);
    }
}

LRESULT App::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == taskbarCreatedMsg_ && taskbarCreatedMsg_ != 0) {
        // Explorer restarted: the tray icon is gone and taskbars were recreated.
        trayAdded_ = false;
        trayRetries_ = 0;
        AddTrayIcon();
        SetTimer(hwnd_, kTimerRebuild, 1500, nullptr);
        return 0;
    }

    switch (msg) {
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            // A click on the icon while lights are being hung is done with them.
            if (hanging_) EndHanging();
            else ShowSettingsWindow();
            break;
        case WM_CONTEXTMENU:
            EndHanging();
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
        // Only wakes the loop in Run(), which releases the window object safely.
        return 0;

    case WM_APP_CLEAR_DRIFTS:
        for (Overlay& o : overlays_)
            if (o.renderer) o.renderer->ClearDrift();
        return 0;

    case WM_APP_HANG_LIGHTS:
        BeginHanging();
        return 0;

    case WM_APP_HANG_DONE:
        EndHanging();
        return 0;

    case WM_APP_RENDER_FAILED:
        // Rendering keeps retrying in the background; nothing useful to show
        // beyond not crashing. Rebuild once in case the display set changed.
        SetTimer(hwnd_, kTimerRebuild, 2000, nullptr);
        return 0;

    case WM_APP_RAISE:
        RestackOverlays();
        return 0;

    case WM_APP_CHECK_FULLSCREEN:
        // Another app came to the front while lights were being hung: done. (Asked now,
        // not when the event fired: one from before hanging began may arrive late.)
        if (hanging_ && OtherAppInFront()) EndHanging();
        CheckFullscreen();
        return 0;

    case WM_APP_REBUILD:
        SetTimer(hwnd_, kTimerRebuild, 400, nullptr);
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_DEVICECHANGE:
        // A new resolution or arrangement moves the monitors out from under strands being hung.
        if (msg == WM_DISPLAYCHANGE) EndHanging();
        SetTimer(hwnd_, kTimerRebuild, 600, nullptr);
        return 0;

    case WM_SETTINGCHANGE:
        if (wp == SPI_SETWORKAREA || (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"TraySettings") == 0))
            SetTimer(hwnd_, kTimerRebuild, 400, nullptr);
        if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) FlushDarkMenus();
        return 0;

    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK) {
            EndHanging();
            sessionLocked_ = true;
        } else if (wp == WTS_SESSION_UNLOCK) {
            sessionLocked_ = false;
        }
        UpdateFrameCap();  // remote connects/disconnects change the cap
        UpdateVisibility();
        return 0;

    case WM_POWERBROADCAST:
        if (wp == PBT_POWERSETTINGCHANGE) {
            auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lp);
            if (setting && IsEqualGUID(setting->PowerSetting, GUID_SESSION_DISPLAY_STATUS) &&
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
            EndHanging();
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
            RestackOverlays();
            break;
        case kTimerTray:
            KillTimer(hwnd_, kTimerTray);
            if (!trayAdded_) AddTrayIcon();
            break;
        case kTimerRaise:
            KillTimer(hwnd_, kTimerRaise);
            RestackOverlays();
            break;
        case kTimerSave:
            KillTimer(hwnd_, kTimerSave);
            SaveSettings(settings_);
            break;
        case kTimerHangIdle:
            KillTimer(hwnd_, kTimerHangIdle);
            EndHanging();
            break;
        default:
            break;
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;

    case WM_DESTROY:
        // Strands being hung are kept, and saved with the rest.
        EndHanging();
        KillTimer(hwnd_, kTimerSave);
        SaveSettings(settings_);
        // Close (not delete): we may be inside the settings window's colour
        // dialog loop; ~App releases the object once the stack has unwound.
        if (settingsWindow_) settingsWindow_->Close();
        for (HWINEVENTHOOK* hook : {&reorderHook_, &foregroundHook_, &showHook_, &minimizeHook_, &moveSizeHook_}) {
            if (*hook) UnhookWinEvent(*hook);
            *hook = nullptr;
        }
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
    // NIM_ADD can time out while a busy Explorer still adds the icon; adopt it.
    if (!trayAdded_) trayAdded_ = Shell_NotifyIconW(NIM_MODIFY, &nid) != FALSE;
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
    nid.hBalloonIcon = iconLarge_ ? iconLarge_ : icon_;
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
        {IDM_FLAKES_RECOMMENDED, L"&Recommended", kDefaultFlakeMask},
        {IDM_FLAKES_ALL, L"&Everything", kAllFlakesMask},
        {IDM_FLAKES_SOFT, L"&Soft snow and sparkles", kSoftMask | FlakeBit(FlakeKind::Sparkle)},
        {IDM_FLAKES_CRYSTALS, L"&Crystals", kCrystalMask},
        {IDM_FLAKES_SIMPLE, L"Simple sh&apes", kSimpleMask},
        {IDM_FLAKES_CLASSIC, L"Classic &pixel art", kClassicMask},
    };
    UINT checkedFlakes = 0;
    for (const FlakePreset& p : presets) {
        AppendMenuW(flakes, MF_STRING, p.id, p.name);
        if (s.flakeMask == p.mask) checkedFlakes = p.id;
    }
    if (checkedFlakes)
        CheckMenuRadioItem(flakes, IDM_FLAKES_RECOMMENDED, IDM_FLAKES_CLASSIC, checkedFlakes, MF_BYCOMMAND);
    AppendMenuW(flakes, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(flakes, MF_STRING, IDM_FLAKES_CHOOSE, L"C&hoose flakes...");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(flakes), L"Snow&flakes");

    auto presetMenu = [&](const wchar_t* title, const auto& list, UINT base, int value) {
        HMENU sub = CreatePopupMenu();
        const int n = static_cast<int>(std::size(list));
        for (int i = 0; i < n; ++i) AppendMenuW(sub, MF_STRING, base + i, list[i].name);
        CheckMenuRadioItem(sub, base, base + n - 1, base + NearestPreset(list, value), MF_BYCOMMAND);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), title);
        return sub;
    };
    presetMenu(L"&Amount", kAmountPresets, IDM_AMOUNT, s.amount);
    presetMenu(L"Sp&eed", kSpeedPresets, IDM_SPEED, s.speed);
    HMENU wind = presetMenu(L"&Wind", kWindPresets, IDM_WIND, s.wind);
    AppendMenuW(wind, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(wind, MF_STRING | (s.gusts >= 8 ? MF_CHECKED : 0), IDM_GUSTY, L"&Gusty");
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

    // Trees and snowmen: a switch and how many of each.
    HMENU scenery = CreatePopupMenu();
    auto amounts = [&](const auto& list, UINT base, int value) {
        const int n = static_cast<int>(std::size(list));
        for (int i = 0; i < n; ++i) AppendMenuW(scenery, MF_STRING, base + i, list[i].name);
        CheckMenuRadioItem(scenery, base, base + n - 1, base + NearestPreset(list, value), MF_BYCOMMAND);
    };
    AppendMenuW(scenery, MF_STRING | (s.trees ? MF_CHECKED : 0), IDM_TREES, L"Show &trees");
    amounts(kTreePresets, IDM_TREE_AMOUNT, s.treeAmount);
    AppendMenuW(scenery, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(scenery, MF_STRING | (s.snowmen ? MF_CHECKED : 0), IDM_SNOWMEN, L"Show &snowmen");
    amounts(kSnowmanPresets, IDM_SNOWMAN_AMOUNT, s.snowmanAmount);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(scenery), L"&Trees and snowmen");

    // Christmas lights: a switch, hanging them, the main choices and where they hang.
    HMENU lights = CreatePopupMenu();
    AppendMenuW(lights, MF_STRING | (s.lights ? MF_CHECKED : 0), IDM_LIGHTS, L"&Show lights");
    AppendMenuW(lights, MF_STRING, IDM_HANG_LIGHTS, L"&Hang lights...");
    AppendMenuW(lights, MF_SEPARATOR, 0, nullptr);
    auto choices = [&](const wchar_t* title, UINT base, int count, int current, auto name) {
        HMENU sub = CreatePopupMenu();
        for (int i = 0; i < count; ++i) AppendMenuW(sub, MF_STRING, base + i, name(i).c_str());
        CheckMenuRadioItem(sub, base, base + count - 1, base + current, MF_BYCOMMAND);
        AppendMenuW(lights, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), title);
    };
    choices(L"&Bulb", IDM_BULB, static_cast<int>(BulbType::Count), static_cast<int>(s.bulb),
            [](int i) { return MenuText(BulbTypeName(static_cast<BulbType>(i))); });
    choices(L"&Animation", IDM_LIGHT_ANIMATION, static_cast<int>(LightAnimation::Count),
            static_cast<int>(s.lightAnimation),
            [](int i) { return MenuText(LightAnimationName(static_cast<LightAnimation>(i))); });
    choices(L"&Colors", IDM_LIGHT_COLORS, static_cast<int>(LightColors::Count), static_cast<int>(s.lightColors),
            [](int i) {
                const auto c = static_cast<LightColors>(i);
                return c == LightColors::Custom ? std::wstring(L"Custom...") : MenuText(LightColorsName(c));
            });
    choices(L"S&peed", IDM_LIGHT_SPEED, static_cast<int>(std::size(kLightSpeedPresets)),
            NearestPreset(kLightSpeedPresets, s.lightSpeed), [](int i) { return MenuText(kLightSpeedPresets[i].name); });
    AppendMenuW(lights, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(lights, MF_STRING, IDM_LIGHTS_ACROSS_TOP, L"Across the &top");
    // Nothing to show until some are hung (Hang lights... does that).
    AppendMenuW(lights, MF_STRING | (s.strands.empty() ? MF_GRAYED : 0), IDM_LIGHTS_CUSTOM, L"&My strands");
    CheckMenuRadioItem(lights, IDM_LIGHTS_ACROSS_TOP, IDM_LIGHTS_CUSTOM,
                       s.lightLayout == LightLayout::Custom ? IDM_LIGHTS_CUSTOM : IDM_LIGHTS_ACROSS_TOP, MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(lights), L"Christmas &lights");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (s.mouse ? MF_CHECKED : 0), IDM_MOUSE, L"&Mouse plays with snow");
    AppendMenuW(menu, MF_STRING | (s.shadow ? MF_CHECKED : 0), IDM_SHADOW, L"Flake &outlines on light windows");
    AppendMenuW(menu, MF_STRING | (s.pauseFullscreen ? MF_CHECKED : 0), IDM_FULLSCREEN,
                L"Hide during f&ullscreen apps");
    AppendMenuW(menu, MF_STRING | (s.allMonitors ? MF_CHECKED : 0), IDM_ALL_MONITORS, L"Snow on all mo&nitors");
    AppendMenuW(menu, MF_STRING | (s.behindWindows ? MF_CHECKED : 0), IDM_BEHIND_WINDOWS, L"Snow be&hind windows");
    AppendMenuW(menu, MF_STRING | (IsStartWithWindows() ? MF_CHECKED : 0), IDM_STARTUP, L"Start with W&indows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_ABOUT, L"A&bout " SNOWYSTARR_APP_NAME);
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
    case IDM_BEHIND_WINDOWS: s.behindWindows = !s.behindWindows; break;
    case IDM_TREES: s.trees = !s.trees; break;
    case IDM_SNOWMEN: s.snowmen = !s.snowmen; break;
    case IDM_LIGHTS: s.lights = !s.lights; break;
    case IDM_HANG_LIGHTS: BeginHanging(); return;
    case IDM_LIGHTS_ACROSS_TOP:
        s.lightLayout = LightLayout::AcrossTop;
        s.lights = true;
        break;
    case IDM_LIGHTS_CUSTOM:
        s.lightLayout = LightLayout::Custom;
        s.lights = true;
        break;
    case IDM_STARTUP: SetStartWithWindows(!IsStartWithWindows()); return;
    case IDM_GUSTY: s.gusts = s.gusts >= 8 ? 0 : 45; break;
    case IDM_FLAKES_RECOMMENDED: s.flakeMask = kDefaultFlakeMask; break;
    case IDM_FLAKES_ALL: s.flakeMask = kAllFlakesMask; break;
    case IDM_FLAKES_SOFT: s.flakeMask = kSoftMask | FlakeBit(FlakeKind::Sparkle); break;
    case IDM_FLAKES_CRYSTALS: s.flakeMask = kCrystalMask; break;
    case IDM_FLAKES_SIMPLE: s.flakeMask = kSimpleMask; break;
    case IDM_FLAKES_CLASSIC: s.flakeMask = kClassicMask; break;
    case IDM_FLAKES_CHOOSE:
        ShowSettingsWindow();
        if (settingsWindow_) settingsWindow_->SelectTab(SettingsWindow::Tab::Snow);
        return;
    default:
        if (id >= IDM_AMOUNT && id < IDM_AMOUNT + std::size(kAmountPresets)) {
            s.amount = kAmountPresets[id - IDM_AMOUNT].value;
        } else if (id >= IDM_SPEED && id < IDM_SPEED + std::size(kSpeedPresets)) {
            s.speed = kSpeedPresets[id - IDM_SPEED].value;
        } else if (id >= IDM_WIND && id < IDM_WIND + std::size(kWindPresets)) {
            s.wind = kWindPresets[id - IDM_WIND].value;
        } else if (id >= IDM_DRIFT && id < IDM_DRIFT + std::size(kDriftPresets)) {
            s.drift = kDriftPresets[id - IDM_DRIFT].value;
        } else if (id >= IDM_TREE_AMOUNT && id < IDM_TREE_AMOUNT + std::size(kTreePresets)) {
            s.treeAmount = kTreePresets[id - IDM_TREE_AMOUNT].value;
            s.trees = true;
        } else if (id >= IDM_SNOWMAN_AMOUNT && id < IDM_SNOWMAN_AMOUNT + std::size(kSnowmanPresets)) {
            s.snowmanAmount = kSnowmanPresets[id - IDM_SNOWMAN_AMOUNT].value;
            s.snowmen = true;
        } else if (id >= IDM_BULB && id < IDM_BULB + static_cast<UINT>(BulbType::Count)) {
            s.bulb = static_cast<BulbType>(id - IDM_BULB);
            s.lights = true;
        } else if (id >= IDM_LIGHT_ANIMATION && id < IDM_LIGHT_ANIMATION + static_cast<UINT>(LightAnimation::Count)) {
            s.lightAnimation = static_cast<LightAnimation>(id - IDM_LIGHT_ANIMATION);
            s.lights = true;
        } else if (id >= IDM_LIGHT_SPEED && id < IDM_LIGHT_SPEED + std::size(kLightSpeedPresets)) {
            s.lightSpeed = kLightSpeedPresets[id - IDM_LIGHT_SPEED].value;
            s.lights = true;
        } else if (id >= IDM_LIGHT_COLORS && id < IDM_LIGHT_COLORS + static_cast<UINT>(LightColors::Count)) {
            s.lightColors = static_cast<LightColors>(id - IDM_LIGHT_COLORS);
            s.lights = true;
            if (s.lightColors == LightColors::Custom) {
                // The picker is the settings window's, as for the snow's colour.
                ShowSettingsWindow();
                if (settingsWindow_) settingsWindow_->RequestLightColor(true);
                return;
            }
        } else if (id >= IDM_COLOR && id < IDM_COLOR + static_cast<UINT>(SnowColor::Count)) {
            s.color = static_cast<SnowColor>(id - IDM_COLOR);
            if (s.color == SnowColor::Custom) {
                // The colour dialog belongs to the visible settings window, so
                // it opens in a sensible place and keeps a taskbar button.
                ShowSettingsWindow();
                if (settingsWindow_) settingsWindow_->RequestCustomColor();
                return;
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
    tdc.pszWindowTitle = L"About " SNOWYSTARR_APP_NAME;
    tdc.pszMainIcon = MAKEINTRESOURCEW(IDI_APP);
    tdc.pszMainInstruction = SNOWYSTARR_APP_NAME L" " SNOWYSTARR_VERSION_STR;
    tdc.pszContent =
        L"Snow for your Windows desktop: it drifts across every monitor and settles on the taskbar.\n\n"
        L"Created by Joshua Starr, 2026\n"
        L"<a href=\"https://" SNOWYSTARR_REPO L"\">" SNOWYSTARR_REPO L"</a>";
    tdc.pszFooter = L"Click the tray snowflake for settings; right-click for quick options.";
    tdc.dwCommonButtons = TDCBF_OK_BUTTON;
    // The dialog only reports link clicks; opening them is up to us.
    tdc.pfCallback = [](HWND dialog, UINT msg, WPARAM, LPARAM lp, LONG_PTR) -> HRESULT {
        if (msg == TDN_HYPERLINK_CLICKED)
            ShellExecuteW(dialog, L"open", reinterpret_cast<const wchar_t*>(lp), nullptr, nullptr, SW_SHOWNORMAL);
        return S_OK;
    };
    SetForegroundWindow(hwnd_);
    TaskDialogIndirect(&tdc, nullptr, nullptr, nullptr);
}

void App::ShowSettingsWindow() {
    // It would sit behind the overlays while lights are hung: done with them first.
    EndHanging();
    if (!settingsWindow_) settingsWindow_ = std::make_unique<SettingsWindow>(instance_, hwnd_);
    settingsWindow_->Show(settings_);
}

void App::ApplySettings(const Settings& next, bool save) {
    Settings s = next;
    s.Sanitize();
    const Settings old = settings_;
    settings_ = s;

    if (old.allMonitors != s.allMonitors) RebuildOverlays(true);
    // Overlays kept by a rebuild need the new settings too.
    for (Overlay& o : overlays_) {
        if (o.renderer) o.renderer->ApplySettings(settings_);
        if (old.behindWindows == s.behindWindows) continue;
        if (!s.behindWindows && o.shown) {
            // Coming out from behind the windows, the last frame shows the trees and
            // snowmen as if nothing covered them: hide the snow and fade it back in
            // once the renderer has looked at what covers them now.
            o.alpha = 0;
            SetLayeredWindowAttributes(o.hwnd, 0, 0, LWA_ALPHA);
        }
        PlaceOverlay(o.hwnd);
    }
    if (old.paused != s.paused || old.amount != s.amount) UpdateTrayIcon();
    UpdateVisibility();
    if (settingsWindow_) settingsWindow_->Update(settings_);
    if (save) SetTimer(hwnd_, kTimerSave, 800, nullptr);
}

// ---------------------------------------------------------------------------
// Hanging lights
// ---------------------------------------------------------------------------

// The overlays take the pointer (shown, opaque and on top, whatever would hide
// them otherwise), the HUD takes the keyboard, and the editor every strand: on
// a monitor showing the drape across the top (or with nothing hung anywhere
// yet), that drape, so there is something to start from and opening and
// closing the mode leaves the screens as they were.
void App::BeginHanging() {
    if (hanging_) {
        if (hud_) SetForegroundWindow(hud_->Hwnd());
        return;
    }
    // Not under the settings window's colour dialog, which would be stuck behind the overlays.
    if (overlays_.empty() || (settingsWindow_ && settingsWindow_->Busy())) return;

    std::vector<EditMonitor> monitors;
    std::vector<LightStrand> strands = settings_.strands;
    const bool drape = settings_.strands.empty() || settings_.lightLayout == LightLayout::AcrossTop;
    for (const Overlay& o : overlays_) {
        const RECT& r = o.target.rect;
        const int w = r.right - r.left, h = r.bottom - r.top;
        const float scale = static_cast<float>(o.target.dpi) / 96.0f;
        monitors.push_back({o.target.deviceId, static_cast<float>(r.left), static_cast<float>(r.top),
                            static_cast<float>(w), static_cast<float>(h), scale});
        const bool own = std::any_of(settings_.strands.begin(), settings_.strands.end(),
                                     [&](const LightStrand& st) { return st.monitor == o.target.deviceId; });
        if (drape && !own) {
            std::vector<LightStrand> top = Lights::AcrossTop(o.target.deviceId, w, h, scale);
            strands.insert(strands.end(), std::make_move_iterator(top.begin()), std::make_move_iterator(top.end()));
        }
    }
    editor_.Begin(std::move(monitors), std::move(strands));
    hanging_ = true;
    sentEdits_.assign(overlays_.size(), LightsEdit{});
    leaveTracked_ = nullptr;

    for (Overlay& o : overlays_) {
        SetWindowLongPtrW(o.hwnd, GWL_EXSTYLE, GetWindowLongPtrW(o.hwnd, GWL_EXSTYLE) & ~WS_EX_TRANSPARENT);
        SetWindowPos(o.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        o.wantVisible = true;
        if (!o.shown) {
            o.renderer->SetVisible(true);
            ShowWindow(o.hwnd, SW_SHOWNOACTIVATE);
            o.shown = true;
        }
        o.alpha = 255;
        SetLayeredWindowAttributes(o.hwnd, 0, 255, LWA_ALPHA);
    }

    // The HUD, on the monitor the pointer is on. (Its events come from inside
    // its window procedure, so being done is posted rather than done there.)
    LightsHud::Events events;
    events.clearAll = [this] {
        editor_.ClearAll();
        HangChanged();
    };
    events.done = [this] { PostMessageW(hwnd_, WM_APP_HANG_DONE, 0, 0); };
    events.key = [this](UINT vk) { HangKey(vk); };
    events.wheel = [this](float notches) {
        editor_.Wheel(notches);
        HangChanged();
    };
    events.pointer = [this](POINT p) { HangPointer(p); };
    POINT p{};
    GetCursorPos(&p);
    HMONITOR monitor = overlays_.front().target.monitor;
    for (const Overlay& o : overlays_)
        if (PtInRect(&o.target.rect, p)) monitor = o.target.monitor;
    hud_ = std::make_unique<LightsHud>(instance_, std::move(events));
    LightsHud::State state;
    state.strands = editor_.StrandCount();
    state.bulb = settings_.bulb;
    if (!hud_->Create(monitor, state)) {
        // No keys and no Done without it: leave everything as it was.
        hanging_ = false;
        hud_.reset();
        RestoreOverlays();
        return;
    }
    // Hidden only now that the HUD is active: hiding the active window would
    // hand the activation to another app, and that ends hanging.
    settingsHidden_ = settingsWindow_ && settingsWindow_->IsOpen();
    if (settingsHidden_) settingsWindow_->SetHidden(true);
    if (IsOverlay(WindowFromPoint(p))) editor_.Move(static_cast<float>(p.x), static_cast<float>(p.y));
    HangChanged();
}

// Done: the overlays let the pointer through again, and the strands as hung
// become the settings (shown, on the user's own strands; with every strand
// taken down, the lights go off, as the screens were left).
void App::EndHanging() {
    if (!hanging_) return;
    hanging_ = false;
    KillTimer(hwnd_, kTimerHangIdle);
    Settings s = settings_;
    s.strands = editor_.Finish();
    s.lightLayout = s.strands.empty() ? LightLayout::AcrossTop : LightLayout::Custom;
    s.lights = !s.strands.empty();
    RestoreOverlays();
    // The settings window comes back (and takes the activation) before the HUD goes.
    if (settingsHidden_ && settingsWindow_) settingsWindow_->SetHidden(false);
    settingsHidden_ = false;
    hud_.reset();
    ApplySettings(s);
    // Only now that the renderers have the new strands do they stop drawing the editor's (no blink).
    for (Overlay& o : overlays_)
        if (o.renderer) o.renderer->SetLightsEdit({});
    sentEdits_.clear();
}

// The overlays as they were: click-through, back in their place, and shown or
// fading out as they would be.
void App::RestoreOverlays() {
    if (IsOverlay(GetCapture())) ReleaseCapture();
    leaveTracked_ = nullptr;
    for (Overlay& o : overlays_) {
        SetWindowLongPtrW(o.hwnd, GWL_EXSTYLE, GetWindowLongPtrW(o.hwnd, GWL_EXSTYLE) | WS_EX_TRANSPARENT);
        SetWindowPos(o.hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        PlaceOverlay(o.hwnd);
    }
    UpdateVisibility();
}

int App::OverlayIndex(HWND hwnd) const {
    for (size_t i = 0; i < overlays_.size(); ++i)
        if (overlays_[i].hwnd == hwnd) return static_cast<int>(i);
    return -1;
}

// What an overlay gets while lights are hung. False: not for the editor.
bool App::HangInput(HWND overlay, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    const int index = OverlayIndex(overlay);
    if (index < 0) return false;
    // Client px (from this overlay, even beyond it during a drag) to virtual-screen px.
    const RECT& rect = overlays_[static_cast<size_t>(index)].target.rect;
    const float x = static_cast<float>(rect.left + GET_X_LPARAM(lp));
    const float y = static_cast<float>(rect.top + GET_Y_LPARAM(lp));
    result = 0;
    switch (msg) {
    case WM_NCHITTEST:
        result = HTCLIENT;
        return true;
    case WM_SETCURSOR:
        if (LOWORD(lp) != HTCLIENT) return false;
        SetCursor(HangCursor());
        result = TRUE;
        return true;
    case WM_MOUSEMOVE:
        if (leaveTracked_ != overlay) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, overlay, 0};
            if (TrackMouseEvent(&tme)) leaveTracked_ = overlay;
        }
        editor_.Move(x, y);
        HangChanged();
        if (hud_) hud_->Avoid({static_cast<LONG>(x), static_cast<LONG>(y)}, HangWorking());
        return true;
    case WM_MOUSELEAVE: {
        if (leaveTracked_ == overlay) leaveTracked_ = nullptr;
        // Onto the next monitor's overlay or the HUD (which take over), or held by a drag: still ours.
        POINT p{};
        GetCursorPos(&p);
        const HWND under = WindowFromPoint(p);
        if (editor_.Pressed() || IsOverlay(under) || (hud_ && under == hud_->Hwnd())) return true;
        editor_.Leave();
        HangChanged();
        return true;
    }
    case WM_LBUTTONDOWN:
        // A click doesn't activate the overlays: make sure the keys go to the HUD.
        if (hud_ && GetForegroundWindow() != hud_->Hwnd()) SetForegroundWindow(hud_->Hwnd());
        SetCapture(overlay);
        editor_.Press(x, y);
        HangChanged();
        return true;
    case WM_LBUTTONUP:
        // Let go before the capture: losing it cancels a press.
        editor_.Release(x, y);
        if (GetCapture() == overlay) ReleaseCapture();
        HangChanged();
        return true;
    case WM_CAPTURECHANGED:
        // Something else took the pointer mid-press: what was dragged stays where it got to.
        if (editor_.Pressed()) {
            editor_.CancelPress();
            HangChanged();
        }
        return true;
    case WM_RBUTTONDOWN:
        // Ends the strand being placed (or puts back a hook being dragged).
        if (!editor_.CancelDrag()) editor_.EndStrand();
        HangChanged();
        return true;
    case WM_RBUTTONUP:
        return true;
    case WM_MOUSEWHEEL:
        editor_.Wheel(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / static_cast<float>(WHEEL_DELTA));
        HangChanged();
        return true;
    case WM_KEYDOWN:
        HangKey(static_cast<UINT>(wp));
        return true;
    default:
        return false;
    }
}

// The pointer over the HUD: a strand being placed keeps following it, but
// nothing on the screen below is under it.
void App::HangPointer(POINT p) {
    if (editor_.Placing() || editor_.Pressed()) editor_.Move(static_cast<float>(p.x), static_cast<float>(p.y));
    else editor_.Leave();
    HangChanged();
    if (hud_) hud_->Avoid(p, HangWorking());
}

void App::HangKey(UINT vk) {
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    switch (vk) {
    case VK_ESCAPE:
        // Puts a dragged hook back, else ends the strand being placed, else is done.
        if (!editor_.CancelDrag() && !editor_.EndStrand()) {
            PostMessageW(hwnd_, WM_APP_HANG_DONE, 0, 0);
            return;
        }
        break;
    case VK_RETURN:
        PostMessageW(hwnd_, WM_APP_HANG_DONE, 0, 0);
        return;
    case VK_DELETE:
        editor_.Delete();
        break;
    case VK_BACK:
        editor_.Undo();
        break;
    case 'Z':
        if (!ctrl) return;
        if (shift) editor_.Redo();
        else editor_.Undo();
        break;
    case 'Y':
        if (!ctrl) return;
        editor_.Redo();
        break;
    default:
        return;
    }
    HangChanged();
}

// After any input: each overlay draws its monitor's part, the HUD follows the
// pointer's monitor and shows the sag, the pointer shows what a press would do.
void App::HangChanged() {
    if (!hanging_) return;
    for (size_t i = 0; i < overlays_.size() && i < sentEdits_.size(); ++i) {
        LightsEdit edit = editor_.EditFor(static_cast<int>(i));
        if (edit == sentEdits_[i]) continue;
        if (overlays_[i].renderer) overlays_[i].renderer->SetLightsEdit(edit);
        sentEdits_[i] = std::move(edit);
    }
    if (hud_) {
        const int m = editor_.PointerMonitor();
        if (m >= 0 && m < static_cast<int>(overlays_.size()))
            hud_->SetMonitor(overlays_[static_cast<size_t>(m)].target.monitor);
        LightsHud::State state;
        state.sag = editor_.Sag();
        state.placing = editor_.Placing();
        state.strands = editor_.StrandCount();
        state.bulb = settings_.bulb;
        hud_->SetState(state);
    }
    POINT p{};
    if (GetCursorPos(&p) && (IsOverlay(GetCapture()) || IsOverlay(WindowFromPoint(p)))) SetCursor(HangCursor());
    SetTimer(hwnd_, kTimerHangIdle, kHangIdleMs, nullptr);
}

// The user is at work under the pointer: the HUD keeps out of the way.
bool App::HangWorking() const {
    return editor_.Placing() || editor_.DraggingHook() || editor_.Hover() != LightsEditor::Over::Nothing;
}

// Crosshair to hang a hook, size-all over a hook (it can be dragged), a hand over a wire.
HCURSOR App::HangCursor() const {
    switch (editor_.Hover()) {
    case LightsEditor::Over::Hook: return LoadCursorW(nullptr, IDC_SIZEALL);
    case LightsEditor::Over::Wire: return LoadCursorW(nullptr, IDC_HAND);
    default: return LoadCursorW(nullptr, IDC_CROSS);
    }
}

// ---------------------------------------------------------------------------
// Monitors and overlays
// ---------------------------------------------------------------------------

std::vector<MonitorTarget> App::EnumerateMonitors() const {
    struct Mon {
        MonitorTarget t;
        RECT work;
        std::wstring device;  // GDI name
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
            // Key on the monitor's device interface path (stable across the
            // \\.\DISPLAYn renumbering Windows does) plus its position.
            DISPLAY_DEVICEW dd{};
            dd.cb = sizeof(dd);
            const wchar_t* id = mi.szDevice;
            if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, EDD_GET_DEVICE_INTERFACE_NAME) && dd.DeviceID[0])
                id = dd.DeviceID;
            wchar_t rect[64];
            swprintf_s(rect, L"@%ld,%ld,%ld,%ld", mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right,
                       mi.rcMonitor.bottom);
            mon.t.key = std::wstring(id) + rect;
            mon.t.deviceId = id;
            mon.device = mi.szDevice;
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

    const std::vector<std::wstring> advanced = AdvancedColorDisplays();
    for (Mon& m : mons)
        m.t.advancedColor = std::find(advanced.begin(), advanced.end(), m.device) != advanced.end();

    float primaryHz = 60.0f, primaryScale = 1.0f;
    for (const Mon& m : mons) {
        if (!m.t.primary) continue;
        primaryHz = m.t.refreshHz;
        primaryScale = static_cast<float>(m.t.dpi) / 96.0f;
    }

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
        t.autoHideGround = autoHide;
        t.originScale = primaryScale;
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
    // The overlays are about to change under any strands being hung: done with them first.
    EndHanging();

    // Overlays whose window can stay (same monitor, rectangle and DPI) keep
    // running; smaller changes (ground line, drift ends, refresh rate) are
    // applied in place. Only the rest are torn down and created afresh, so
    // e.g. turning on "Snow on all monitors" doesn't blink the primary.
    std::vector<Overlay> kept;
    std::vector<MonitorTarget> fresh;
    for (MonitorTarget& t : targets) {
        auto it = std::find_if(overlays_.begin(), overlays_.end(), [&](const Overlay& o) {
            return o.hwnd && o.renderer && o.target.key == t.key && o.target.monitor == t.monitor &&
                   EqualRect(&o.target.rect, &t.rect) && o.target.dpi == t.dpi;
        });
        if (it == overlays_.end()) {
            fresh.push_back(t);
            continue;
        }
        t.hwnd = it->hwnd;
        if (!SameTargets({it->target}, {t})) it->renderer->Reconfigure(t);
        it->target = t;
        kept.push_back(std::move(*it));
        overlays_.erase(it);
    }
    DestroyOverlays();
    overlays_ = std::move(kept);
    for (MonitorTarget& t : fresh) {
        const int w = t.rect.right - t.rect.left, h = t.rect.bottom - t.rect.top;
        HWND hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
                                        WS_EX_NOACTIVATE,
                                    kOverlayClass, kAppName, WS_POPUP, t.rect.left, t.rect.top, w, h, nullptr,
                                    nullptr, instance_, nullptr);
        if (!hwnd) continue;
        // A transparent, screen-sized topmost window would otherwise count as
        // a "fullscreen app": the taskbar would drop below other windows and
        // notifications would be suppressed.
        SetPropW(hwnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));
        SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);  // fade in
        // The window may have been created at a different DPI and resized.
        SetWindowPos(hwnd, nullptr, t.rect.left, t.rect.top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        PlaceOverlay(hwnd);

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
        // While lights are hung every overlay shows, paused or not.
        o.wantVisible = hanging_ || (!settings_.paused && !sessionLocked_ && !displayOff_ && !userAway_ &&
                                     !(settings_.pauseFullscreen && fullscreen));
        if (o.wantVisible && !o.shown) {
            o.renderer->SetVisible(true);
            ShowWindow(o.hwnd, SW_SHOWNOACTIVATE);
            PlaceOverlay(o.hwnd);
            o.shown = true;
        }
    }
    // Re-arming a running timer restarts its countdown, which would stall a
    // fade while settings change every frame (slider drags).
    if (!fadeTimer_) {
        SetTimer(hwnd_, kTimerFade, 16, nullptr);
        fadeTimer_ = true;
    }
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
    if (!active) {
        KillTimer(hwnd_, kTimerFade);
        fadeTimer_ = false;
    }
}

bool App::TaskbarAboveOverlay(HWND overlay) const {
    for (HWND w = GetWindow(overlay, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV))
        if (IsTaskbarWindow(w)) return true;
    return false;
}

// Puts an overlay where its snow belongs: above everything, taskbar included,
// or (behind windows) directly above the desktop, so that every window covers it.
// While lights are hung it is on top either way.
void App::PlaceOverlay(HWND overlay) const {
    constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    if (!settings_.behindWindows || hanging_) {
        SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0, flags);
        return;
    }
    const HWND desktop = DesktopWindow();
    const HWND above = desktop ? GetWindow(desktop, GW_HWNDPREV) : nullptr;  // none: no shell running
    if (above == overlay) return;
    // Slot in under the window above the desktop, in its band: Show desktop
    // raises the desktop up to the topmost windows, so the snow then joins
    // those (below the taskbar), as Rainmeter does for "on desktop" skins.
    // HWND_BOTTOM drops topmost status without passing over other windows.
    const bool topmost = above && (GetWindowLongW(above, GWL_EXSTYLE) & WS_EX_TOPMOST);
    if (topmost != ((GetWindowLongW(overlay, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0))
        SetWindowPos(overlay, topmost ? HWND_TOPMOST : HWND_BOTTOM, 0, 0, 0, 0, flags);
    if (above) SetWindowPos(overlay, above, 0, 0, 0, 0, flags);
}

void App::RestackOverlays() {
    raisePending_ = false;
    const double now = QpcSeconds();
    if (now - lastRaise_ < 0.1) {
        // Rate limit so we never ping-pong with another app that wants the same spot.
        SetTimer(hwnd_, kTimerRaise, 120, nullptr);
        return;
    }
    const bool onTop = !settings_.behindWindows || hanging_;
    const HWND desktop = onTop ? nullptr : DesktopWindow();
    bool moved = false;
    for (Overlay& o : overlays_) {
        if (!o.shown) continue;
        // On top, only the taskbar matters: other topmost windows (picture-in-
        // picture players, tool palettes) may stay above the snow. Behind
        // windows, nothing may come between the snow and the desktop.
        const bool misplaced = onTop ? TaskbarAboveOverlay(o.hwnd) : desktop && !RestsOnDesktop(o.hwnd, desktop);
        if (misplaced) {
            PlaceOverlay(o.hwnd);
            moved = true;
        }
    }
    if (moved) {
        lastRaise_ = now;
        if (hud_) hud_->Raise();  // the HUD stays above the overlays
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
    bool all = false, away = false;
    QUERY_USER_NOTIFICATION_STATE state;
    if (SUCCEEDED(SHQueryUserNotificationState(&state))) {
        all = state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE;
        away = state == QUNS_NOT_PRESENT;  // screen saver, locked, or switched-out session
    }
#ifdef SNOWYSTARR_DEV_TOOLS
    wchar_t ignore[4];
    if (GetEnvironmentVariableW(L"SNOWYSTARR_IGNORE_AWAY", ignore, 4) > 0) away = false;  // test while unattended
#endif
    // Remember the last window activated on each monitor: a fullscreen video
    // keeps its monitor snow-free while the user works on another screen,
    // until something else is activated on that monitor.
    HWND fg = GetForegroundWindow();
    DWORD fgPid = 0;
    if (fg) GetWindowThreadProcessId(fg, &fgPid);
    if (fg && fgPid != GetCurrentProcessId() && !IsShellWindow(fg))
        lastActive_[MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST)] = fg;
    std::vector<HMONITOR> monitors;
    for (auto it = lastActive_.begin(); it != lastActive_.end();) {
        if (!IsWindow(it->second)) {
            it = lastActive_.erase(it);
            continue;
        }
        HMONITOR mon = nullptr;
        if (IsFullscreenWindow(it->second, mon) && mon == it->first) monitors.push_back(it->first);
        ++it;
    }
    if (all != fullscreenAll_ || monitors != fullscreenMonitors_ || away != userAway_) {
        fullscreenAll_ = all;
        userAway_ = away;
        fullscreenMonitors_ = std::move(monitors);
        UpdateVisibility();
    }
}
