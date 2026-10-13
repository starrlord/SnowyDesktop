// Hanging lights: the HUD, a small dark pill at the bottom of the screen that
// says what to do, shows the sag of the span at hand, and has Clear all and
// Done buttons. It is activated so that it takes the keyboard; what it gets
// (its buttons, keys, the wheel, the pointer passing over it) goes to App.
//
// It is a per-pixel-alpha layered window drawn with Direct2D only when
// something changes, so it costs nothing while it sits there.
#pragma once

#include "Common.h"
#include "Settings.h"

#include <d2d1.h>
#include <dwrite.h>
#include <functional>

class LightsHud {
public:
    // What it shows.
    struct State {
        float sag = -1.0f;       // slack of the span being placed or under the pointer (-1: none)
        bool placing = false;    // a strand is being placed
        int strands = 0;         // on the monitors being edited
        BulbType bulb = BulbType::C7;
        bool operator==(const State&) const = default;
    };
    // What it hands on. Called from its window procedure: they must not
    // destroy the HUD (post a message to do that).
    struct Events {
        std::function<void()> clearAll, done;
        std::function<void(UINT vk)> key;           // a key went down (modifiers: GetKeyState)
        std::function<void(float notches)> wheel;   // positive: away from the user
        std::function<void(POINT screen)> pointer;  // the pointer moved over it (virtual-screen px)
    };

    LightsHud(HINSTANCE instance, Events events);
    ~LightsHud();
    LightsHud(const LightsHud&) = delete;
    LightsHud& operator=(const LightsHud&) = delete;

    // Shows it at the bottom of `monitor`, above the taskbar, and activates it.
    bool Create(HMONITOR monitor, const State& state);
    HWND Hwnd() const { return hwnd_; }
    void SetState(const State& state);
    // Moves it to the bottom of `monitor` (when it is not there already).
    void SetMonitor(HMONITOR monitor);
    // The pointer is at `p` (virtual-screen px). While the user works near it
    // (placing, dragging, over a strand) it steps aside; otherwise it stays put
    // so its buttons can be clicked, and goes home once the pointer is away.
    void Avoid(POINT p, bool working);
    // Back above the overlays (put topmost after it).
    void Raise();

    // Draws it in a few states, stacked, over a dark backdrop, to a PNG: for
    // looking at without showing a window (lightsedit_test --hud).
    static HRESULT SaveSnapshot(const wchar_t* path, float scale);

private:
    // The fonts, shared by the window and the snapshot.
    struct Text {
        ComPtr<IDWriteFactory> dwrite;
        ComPtr<IDWriteTextFormat> body, note, button;
    };
    // Where things are, in DIPs from the window's top left (its shadow's margin included).
    struct Layout {
        float width = 0.0f, height = 0.0f;  // the window
        D2D1_RECT_F pill{}, icon{}, line1{}, line2{}, chip{}, clear{}, done{};
    };
    enum Hot : int { kNone, kClear, kDone };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    static HRESULT CreateText(Text& out);
    static Layout MakeLayout(const Text& text);
    static void Draw(ID2D1RenderTarget* rt, const Text& text, const Layout& l, const State& state, D2D1_COLOR_F accent,
                     int hot, int pressed);
    void Render();
    void PlaceAt(HMONITOR monitor);
    POINT HomePos() const;
    RECT PillAt(POINT pos) const;
    void MoveTo(POINT pos, bool glide);
    void Glide();
    int HitButton(POINT client) const;
    bool InPill(POINT client) const;

    HINSTANCE instance_ = nullptr;
    Events events_;
    HWND hwnd_ = nullptr;
    State state_;
    D2D1_COLOR_F accent_{};

    ComPtr<ID2D1Factory> factory_;
    ComPtr<ID2D1DCRenderTarget> rt_;
    Text text_;
    Layout layout_;               // DIPs: it doesn't change with the scale

    HMONITOR monitor_ = nullptr;
    float scale_ = 1.0f;          // px per DIP
    SIZE size_{};                 // px
    POINT pos_{};                 // px: where it is
    POINT target_{};              // ... and where it is gliding to
    float glideX_ = 0.0f, glideY_ = 0.0f;
    bool gliding_ = false;
    double glideLast_ = 0.0;
    bool aside_ = false;          // stepped aside from its home

    int hot_ = kNone, pressed_ = kNone;
    bool tracking_ = false;       // asked for WM_MOUSELEAVE
};
