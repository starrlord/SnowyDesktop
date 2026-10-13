// settingslab: renders the settings window offscreen to PNGs, with no window
// on the screen, for working on its layout (developer builds only).
//
//   settingslab --all <dir>        both tabs, light and dark, at 100% and 150%
//   settingslab --out x.png [--tab snow|lights] [--dark] [--scale 1.5]
//               [--hover x,y] [--click x,y]... [--keys tab,tab,right,space]
//               [--bulb c9] [--colors warm] [--anim chase] [--wire white]
//               [--off] [--strands 3] [--layout custom] [--paused]
//
// Points are DIPs from the window's top left; --click may repeat and the
// clicks go in first, then the keys, one per frame.
#include "SettingsWindow.h"

#include <objbase.h>

#include <cstdio>
#include <cwchar>

namespace {

[[noreturn]] void Die(const wchar_t* what) {
    fwprintf(stderr, L"settingslab: %s\n", what);
    exit(1);
}

template <typename E>
E ParseEnum(const std::wstring& v, std::initializer_list<const wchar_t*> names) {
    int i = 0;
    for (const wchar_t* n : names) {
        if (_wcsicmp(v.c_str(), n) == 0) return static_cast<E>(i);
        ++i;
    }
    fwprintf(stderr, L"settingslab: unknown value '%s'\n", v.c_str());
    exit(1);
}

D2D1_POINT_2F Point(const std::wstring& v) {
    float x = 0, y = 0;
    if (swscanf_s(v.c_str(), L"%f,%f", &x, &y) != 2) Die(L"points look like x,y");
    return {x, y};
}

UINT Key(const std::wstring& name) {
    static const struct {
        const wchar_t* name;
        UINT key;
    } kKeys[] = {{L"tab", VK_TAB},   {L"space", VK_SPACE}, {L"enter", VK_RETURN}, {L"left", VK_LEFT},
                 {L"right", VK_RIGHT}, {L"up", VK_UP},      {L"down", VK_DOWN},    {L"home", VK_HOME},
                 {L"end", VK_END}};
    for (const auto& k : kKeys)
        if (_wcsicmp(name.c_str(), k.name) == 0) return k.key;
    Die((L"unknown key " + name).c_str());
}

std::vector<UINT> Keys(const std::wstring& list) {
    std::vector<UINT> out;
    size_t at = 0;
    while (at <= list.size()) {
        const size_t comma = std::min(list.find(L',', at), list.size());
        if (comma > at) out.push_back(Key(list.substr(at, comma - at)));
        at = comma + 1;
    }
    return out;
}

// Some strands for the "My strands" states: a drape on each of `n` made-up screens.
std::vector<LightStrand> FakeStrands(int n) {
    std::vector<LightStrand> out;
    for (int i = 0; i < n; ++i) {
        LightStrand s;
        s.monitor = L"lab" + std::to_wstring(i % 2);
        s.hooks = {{0.1f, 0.05f}, {0.5f, 0.08f}, {0.9f, 0.05f}};
        s.slack = {0.05f, 0.05f};
        out.push_back(s);
    }
    return out;
}

void Render(const Settings& s, const SettingsWindow::Snapshot& shot, const std::wstring& path) {
    const HRESULT hr = SettingsWindow::RenderSnapshot(s, shot, path.c_str());
    if (FAILED(hr)) {
        fwprintf(stderr, L"settingslab: %s failed (0x%08X)\n", path.c_str(), static_cast<unsigned>(hr));
        exit(1);
    }
    fwprintf(stdout, L"settingslab: wrote %s\n", path.c_str());
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) Die(L"CoInitializeEx failed");

    Settings s;
    SettingsWindow::Snapshot shot;
    std::wstring out, all;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&]() -> std::wstring {
            if (i + 1 >= argc) Die((L"missing value after " + a).c_str());
            return argv[++i];
        };
        if (a == L"--out") out = next();
        else if (a == L"--all") all = next();
        else if (a == L"--tab") shot.tab = ParseEnum<SettingsWindow::Tab>(next(), {L"snow", L"lights"});
        else if (a == L"--dark") shot.dark = true;
        else if (a == L"--scale") shot.scale = static_cast<float>(_wtof(next().c_str()));
        else if (a == L"--hover") shot.hover = Point(next());
        else if (a == L"--click") shot.clicks.push_back(Point(next()));
        else if (a == L"--keys") shot.keys = Keys(next());
        else if (a == L"--bulb")
            s.bulb = ParseEnum<BulbType>(next(), {L"mini", L"miniled", L"wideled", L"c5", L"c6", L"c7", L"c9", L"globe",
                                                 L"bubble", L"candle"});
        else if (a == L"--colors")
            s.lightColors = ParseEnum<LightColors>(next(), {L"multi", L"warm", L"cool", L"redgreen", L"bluewhite", L"custom"});
        else if (a == L"--anim")
            s.lightAnimation = ParseEnum<LightAnimation>(
                next(), {L"steady", L"twinkle", L"pulse", L"chase", L"alternate", L"waves", L"combination"});
        else if (a == L"--wire") s.wire = ParseEnum<WireColor>(next(), {L"green", L"white", L"black"});
        else if (a == L"--layout") s.lightLayout = ParseEnum<LightLayout>(next(), {L"top", L"custom"});
        else if (a == L"--off") s.lights = false;
        else if (a == L"--paused") s.paused = true;
        else if (a == L"--strands") s.strands = FakeStrands(Clamp(_wtoi(next().c_str()), 0, kMaxStrands));
        else Die((L"unknown option " + a).c_str());
    }
    s.Sanitize();

    if (!all.empty()) {
        for (int tab = 0; tab < 2; ++tab)
            for (int dark = 0; dark < 2; ++dark)
                for (float scale : {1.0f, 1.5f}) {
                    SettingsWindow::Snapshot one = shot;
                    one.tab = static_cast<SettingsWindow::Tab>(tab);
                    one.dark = dark != 0;
                    one.scale = scale;
                    wchar_t name[96];
                    swprintf_s(name, L"\\settings-%s-%s-%d.png", tab ? L"lights" : L"snow", dark ? L"dark" : L"light",
                               static_cast<int>(scale * 100.0f));
                    Render(s, one, all + name);
                }
    } else {
        if (out.empty()) Die(L"--out or --all is required");
        Render(s, shot, out);
    }
    CoUninitialize();
    return 0;
}
