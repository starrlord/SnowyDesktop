// snowystarr: snow falling over the Windows desktop, settling on the taskbar.
#include "App.h"

#include <commctrl.h>
#include <objbase.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // The manifest declares per-monitor v2 awareness; this is a fallback for
    // builds that lose the manifest.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // One instance per session. Launching again opens the settings window of
    // the running copy instead.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\snowystarr.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(App::kWindowClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(existing, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(existing, WM_APP_SHOW_SETTINGS, 0, 0);
        }
        CloseHandle(mutex);
        return 0;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    int rc = 1;
    {
        App app;
        if (app.Init(instance)) rc = app.Run();
    }

    if (SUCCEEDED(hr)) CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return rc;
}
