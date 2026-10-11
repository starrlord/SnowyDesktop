#include "Settings.h"

#include <appmodel.h>
#include <shellapi.h>
#include <chrono>
#include <system_error>
#include <thread>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>

namespace {

constexpr wchar_t kKey[] = L"Software\\snowystarr";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
// Our value under kRunKey. It keeps the original name (not the display name):
// the installer writes and removes the same one, and existing entries use it.
constexpr wchar_t kRunValue[] = L"snowystarr";
// The Microsoft Store (MSIX) build starts with Windows through this startup
// task (installer/msix/AppxManifest.xml) instead: a packaged app's registry
// writes are virtualized, so Windows would never see a Run value.
constexpr wchar_t kStartupTaskId[] = L"SnowyDesktopStartup";
// Where Task Manager / Settings > Apps > Startup record "disabled" (the Run
// value itself stays). First byte odd (03, 07) = disabled.
constexpr wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

bool ReadDword(const wchar_t* name, DWORD& out) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return false;
    out = value;
    return true;
}

void WriteDword(HKEY key, const wchar_t* name, DWORD value) {
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

template <typename T>
void Read(const wchar_t* name, T& field) {
    DWORD v;
    if (ReadDword(name, v)) field = static_cast<T>(static_cast<int32_t>(v));
}

void ReadBool(const wchar_t* name, bool& field) {
    DWORD v;
    if (ReadDword(name, v)) field = v != 0;
}

// Runs `f` (returning bool) on a fresh MTA thread and waits for it: the
// startup task calls block, which C++/WinRT forbids on the STA UI thread.
template <typename F>
bool OnMtaThread(F f) {
    bool result = false;
    try {
        std::thread([&] {
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
            } catch (...) {
                return;
            }
            try {
                result = f();
            } catch (...) {
                result = false;  // e.g. the task is missing from the manifest
            }
            winrt::uninit_apartment();
        }).join();
    } catch (const std::system_error&) {
        return false;  // no thread
    }
    return result;
}

// Waits for a startup task call, but not forever: the UI thread is waiting too.
template <typename Op, typename T>
bool Await(const Op& op, T& result) {
    if (op.wait_for(std::chrono::seconds(2)) != winrt::Windows::Foundation::AsyncStatus::Completed) {
        op.Cancel();
        return false;
    }
    result = op.GetResults();
    return true;
}

bool IsEnabled(winrt::Windows::ApplicationModel::StartupTaskState state) {
    using winrt::Windows::ApplicationModel::StartupTaskState;
    return state == StartupTaskState::Enabled || state == StartupTaskState::EnabledByPolicy;
}

std::wstring ExePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return {};
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

}  // namespace

const wchar_t* FlakeKindName(FlakeKind k) {
    switch (k) {
    case FlakeKind::Dot: return L"Powder";
    case FlakeKind::Soft: return L"Soft";
    case FlakeKind::Plus: return L"Cross";
    case FlakeKind::Asterisk: return L"Asterisk";
    case FlakeKind::Sparkle: return L"Sparkle";
    case FlakeKind::Dendrite: return L"Dendrite";
    case FlakeKind::Sectored: return L"Sectored plate";
    case FlakeKind::Fern: return L"Fern";
    case FlakeKind::HexPlate: return L"Hex plate";
    case FlakeKind::StellarPlate: return L"Stellar plate";
    case FlakeKind::Needle: return L"Needle star";
    case FlakeKind::Twelve: return L"Twelve-branch";
    case FlakeKind::Classic1: return L"Classic 1";
    case FlakeKind::Classic2: return L"Classic 2";
    case FlakeKind::Classic3: return L"Classic 3";
    case FlakeKind::Classic4: return L"Classic 4";
    case FlakeKind::Classic5: return L"Classic 5";
    case FlakeKind::Classic6: return L"Classic 6";
    case FlakeKind::Classic7: return L"Classic 7";
    case FlakeKind::Classic8: return L"Classic 8";
    default: return L"";
    }
}

void Settings::Sanitize() {
    flakeMask &= kAllFlakesMask;
    if (flakeMask == 0) flakeMask = kDefaultFlakeMask;
    amount = Clamp(amount, 0, 100);
    speed = Clamp(speed, 0, 100);
    wind = Clamp(wind, -100, 100);
    gusts = Clamp(gusts, 0, 100);
    size = Clamp(size, 0, 100);
    drift = Clamp(drift, 0, 200);
    treeAmount = Clamp(treeAmount, 0, 100);
    snowmanAmount = Clamp(snowmanAmount, 0, 100);
    if (static_cast<int>(color) < 0 || color >= SnowColor::Count) color = SnowColor::White;
    customColor &= 0x00FFFFFF;
}

Settings LoadSettings() {
    Settings s;
    Read(L"FlakeMask", s.flakeMask);
    Read(L"Amount", s.amount);
    Read(L"Speed", s.speed);
    Read(L"Wind", s.wind);
    Read(L"Gusts", s.gusts);
    Read(L"Size", s.size);
    Read(L"Drift", s.drift);
    Read(L"Color", s.color);
    Read(L"CustomColor", s.customColor);
    ReadBool(L"Shadow", s.shadow);
    ReadBool(L"Mouse", s.mouse);
    ReadBool(L"PauseFullscreen", s.pauseFullscreen);
    ReadBool(L"AllMonitors", s.allMonitors);
    ReadBool(L"BehindWindows", s.behindWindows);
    ReadBool(L"Trees", s.trees);
    Read(L"TreeAmount", s.treeAmount);
    ReadBool(L"Snowmen", s.snowmen);
    Read(L"SnowmanAmount", s.snowmanAmount);
    ReadBool(L"Paused", s.paused);
    s.Sanitize();
    return s;
}

void SaveSettings(const Settings& s) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    WriteDword(key, L"FlakeMask", s.flakeMask);
    WriteDword(key, L"Amount", static_cast<DWORD>(s.amount));
    WriteDword(key, L"Speed", static_cast<DWORD>(s.speed));
    WriteDword(key, L"Wind", static_cast<DWORD>(s.wind));
    WriteDword(key, L"Gusts", static_cast<DWORD>(s.gusts));
    WriteDword(key, L"Size", static_cast<DWORD>(s.size));
    WriteDword(key, L"Drift", static_cast<DWORD>(s.drift));
    WriteDword(key, L"Color", static_cast<DWORD>(s.color));
    WriteDword(key, L"CustomColor", s.customColor);
    WriteDword(key, L"Shadow", s.shadow);
    WriteDword(key, L"Mouse", s.mouse);
    WriteDword(key, L"PauseFullscreen", s.pauseFullscreen);
    WriteDword(key, L"AllMonitors", s.allMonitors);
    WriteDword(key, L"BehindWindows", s.behindWindows);
    WriteDword(key, L"Trees", s.trees);
    WriteDword(key, L"TreeAmount", static_cast<DWORD>(s.treeAmount));
    WriteDword(key, L"Snowmen", s.snowmen);
    WriteDword(key, L"SnowmanAmount", static_cast<DWORD>(s.snowmanAmount));
    WriteDword(key, L"Paused", s.paused);
    RegCloseKey(key);
}

bool IsPackaged() {
    static const bool packaged = [] {
        UINT32 length = 0;
        const LONG rc = GetCurrentPackageFullName(&length, nullptr);
        return rc == ERROR_INSUFFICIENT_BUFFER || rc == ERROR_SUCCESS;
    }();
    return packaged;
}

bool IsStartWithWindows() {
    if (IsPackaged()) {
        return OnMtaThread([] {
            using winrt::Windows::ApplicationModel::StartupTask;
            StartupTask task{nullptr};
            return Await(StartupTask::GetAsync(kStartupTaskId), task) && IsEnabled(task.State());
        });
    }
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return false;
    // Only report "on" when the entry points at this executable...
    std::wstring want = L"\"" + ExePath() + L"\"";
    if (_wcsicmp(buf, want.c_str()) != 0) return false;
    // ...and Windows hasn't been told to skip it.
    BYTE approved[16];
    DWORD approvedSize = sizeof(approved);
    if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kRunValue, RRF_RT_REG_BINARY, nullptr, approved,
                     &approvedSize) == ERROR_SUCCESS &&
        approvedSize >= 1 && (approved[0] & 1))
        return false;
    return true;
}

bool SetStartWithWindows(bool enable) {
    if (IsPackaged()) {
        bool disabledByUser = false;
        const bool ok = OnMtaThread([enable, &disabledByUser] {
            using winrt::Windows::ApplicationModel::StartupTask;
            using winrt::Windows::ApplicationModel::StartupTaskState;
            StartupTask task{nullptr};
            if (!Await(StartupTask::GetAsync(kStartupTaskId), task)) return false;
            if (!enable) {
                if (task.State() == StartupTaskState::EnabledByPolicy) return false;  // an administrator's call
                task.Disable();
                return true;
            }
            StartupTaskState state{};
            if (!Await(task.RequestEnableAsync(), state)) return false;
            disabledByUser = state == StartupTaskState::DisabledByUser;
            return IsEnabled(state);
        });
        // Turned off in Settings > Apps > Startup or Task Manager: only the user
        // can turn it back on, there, so take them to that page.
        if (disabledByUser) ShellExecuteW(nullptr, L"open", L"ms-settings:startupapps", nullptr, nullptr, SW_SHOWNORMAL);
        return ok;
    }
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return false;
    LSTATUS st;
    if (enable) {
        std::wstring cmd = L"\"" + ExePath() + L"\"";
        st = RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        st = RegDeleteValueW(key, kRunValue);
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    // Clear any "disabled" mark so turning it on here really turns it on.
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kApprovedKey, kRunValue);
    return st == ERROR_SUCCESS;
}

const Preset kAmountPresets[5] = {
    {L"Flurries", 12}, {L"Light", 30}, {L"Moderate", 45}, {L"Heavy", 70}, {L"Blizzard", 100},
};
const Preset kSpeedPresets[3] = {{L"Slow", 20}, {L"Normal", 50}, {L"Fast", 80}};
const Preset kWindPresets[5] = {
    {L"Windy ←", -70}, {L"Breeze ←", -15}, {L"Calm", 0}, {L"Breeze →", 15}, {L"Windy →", 70},
};
const Preset kDriftPresets[5] = {{L"Off", 0}, {L"Dusting", 25}, {L"Drifts", 55}, {L"Deep", 90}, {L"Snowbanks", 200}};
const Preset kTreePresets[4] = {{L"A few trees", 10}, {L"Some trees", 35}, {L"Lots of trees", 65}, {L"A forest", 100}};
const Preset kSnowmanPresets[4] = {
    {L"One snowman", 0}, {L"Some snowmen", 35}, {L"Lots of snowmen", 65}, {L"A crowd of snowmen", 100},
};
