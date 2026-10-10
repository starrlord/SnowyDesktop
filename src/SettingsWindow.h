// The settings window: a live preview of the snow, a grid of toggleable flake
// designs, sliders and switches. Drawn entirely with Direct2D as a small
// immediate-mode UI that follows the Windows light/dark theme. Changes apply
// live; the window posts WM_APP_SETTINGS_CHANGED to the app.
#pragma once

#include "Common.h"
#include "FlakeArt.h"
#include "Scene.h"
#include "Settings.h"

#include <d2d1_3.h>
#include <dwrite.h>
#include <memory>

class SettingsWindow {
public:
    SettingsWindow(HINSTANCE instance, HWND app);
    ~SettingsWindow();
    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    void Show(const Settings& s);
    // Destroys the window (if open) but not the object; WM_APP_SETTINGS_CLOSED follows as usual.
    void Close();
    // Settings changed elsewhere (tray menu).
    void Update(const Settings& s);
    // Opens the colour picker after the next frame, like the Custom... chip. Call after Show() so the
    // dialog belongs to the visible window. Ignored while the picker is already open.
    void RequestCustomColor();
    const Settings& Current() const { return settings_; }
    bool IsOpen() const { return hwnd_ != nullptr; }
    // True while a modal dialog (the colour picker) runs inside a frame. The window may be closed
    // meanwhile, but the object must not be deleted until this returns false.
    bool Busy() const { return busy_; }

    // Idle-loop rendering (see App::Run). Frame() returns false when nothing
    // was drawn (e.g. the window is occluded) so the caller can back off.
    bool WantsFrame() const;
    bool Frame();

private:
    struct Theme {
        bool dark = false;
        D2D1_COLOR_F bg, card, text, subtext, track, accent, onAccent, border, tile, tileHover, focus;
    };
    struct Input {
        D2D1_POINT_2F mouse{-1000, -1000};  // DIP
        bool inside = false;
        bool down = false;
        bool pressed = false;
        bool released = false;
        float wheel = 0;
        std::vector<UINT> keys;
    };
    enum class Action { None, Close, CustomColor, OpenRepo };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HRESULT CreateDeviceIndependent();
    HRESULT CreateResources();
    HRESULT CreateTargetResources();
    void ReleaseResources();
#ifdef SNOWYSTARR_DEV_TOOLS
    HRESULT SaveSnapshot(const wchar_t* path);  // renders the window offscreen to a PNG
    std::wstring snapshotPath_;
#endif
    void BuildTileImages();
    void ConfigurePreview();
    SimParams PreviewParams() const;
    void ApplyTheme();
    void Cloak(bool on);
    void Render();
    void RunAction();
    void Changed() { changed_ = true; }
    float Snap(float v) const { return std::round(v * scale_) / scale_; }  // DIP on a whole device pixel

    // Widgets.
    void Interact(int id, const D2D1_RECT_F& r, bool& hovered, bool& held, bool& clicked);
    bool Slider(int id, float x, float y, float w, const wchar_t* label, int& value, int lo, int hi, bool bipolar,
                const wchar_t* valueText);
    bool Toggle(int id, float x, float y, float w, const wchar_t* label, bool& value);
    bool Button(int id, const D2D1_RECT_F& r, const wchar_t* text, bool primary);
    // Right-aligned small text unless `fmt` says otherwise.
    bool Link(int id, const D2D1_RECT_F& r, const wchar_t* text, IDWriteTextFormat* fmt = nullptr);
    bool Tile(int id, const D2D1_RECT_F& r, FlakeKind kind, bool selected);
    bool ColorChip(int id, const D2D1_RECT_F& r, SnowColor color, const wchar_t* text, bool selected);

    void DrawPreview(const D2D1_RECT_F& r);
    void Text(const wchar_t* s, const D2D1_RECT_F& r, IDWriteTextFormat* fmt, const D2D1_COLOR_F& c);
    void FocusRing(int id, const D2D1_RECT_F& r, float radius);
    void Fill(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c);
    void Stroke(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c, float width);
    void Circle(float cx, float cy, float radius, const D2D1_COLOR_F& c);

    HINSTANCE instance_;
    HWND app_;
    HWND hwnd_ = nullptr;
    Settings settings_;
    bool startWithWindows_ = false;
    UINT dpi_ = 96;       // the window's DPI (sizes the system-drawn frame)
    float scale_ = 1.0f;  // px per DIP: the DPI scale, reduced if needed to fit the work area
    Theme theme_;

    // Device-independent resources.
    ComPtr<ID2D1Factory1> factory_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IWICImagingFactory> wic_;
    ComPtr<IDWriteTextFormat> fmtTitle_, fmtSection_, fmtBody_, fmtSmall_, fmtValue_, fmtButton_;
    ComPtr<ID2D1RoundedRectangleGeometry> previewClip_;

    // Device resources. rt_ is the window's target (or an offscreen one for
    // snapshots); hwndRt_ is set when it is the window's.
    ComPtr<ID2D1HwndRenderTarget> hwndRt_;
    ComPtr<ID2D1RenderTarget> rt_;
    ComPtr<ID2D1DeviceContext> dc_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1LinearGradientBrush> sky_;
    ComPtr<ID2D1RadialGradientBrush> moon_;
    ComPtr<ID2D1Bitmap> classic_[8];
    ComPtr<ID2D1Bitmap> tileImages_[kFlakeKindCount];
    FlakeAtlas atlas_;
    bool atlasShadow_ = true;  // outline setting of the last successful build
    double atlasRetry_ = 0.0;  // earliest time to retry a failed build

    // Live preview.
    std::unique_ptr<Scene> preview_;
    D2D1_RECT_F previewRect_{};
    double origin_ = 0.0, last_ = 0.0;
    D2D1_POINT_2F lastMouse_{};
    bool lastMouseValid_ = false;
    float mouseVx_ = 0, mouseVy_ = 0;

    // Immediate-mode UI state.
    Input input_;
    int hot_ = 0, active_ = 0, focus_ = 0;
    bool focusVisible_ = false;
    std::vector<int> order_, prevOrder_;
    int hoveredTile_ = -1;
    float grabOffset_ = 0.0f;  // where the dragged slider's thumb was grabbed (DIP from its centre)
    int wheelSlider_ = 0;      // hovered slider that wheelSteps_ belongs to
    float wheelSteps_ = 0.0f;  // fractional wheel steps not applied yet
    bool changed_ = false;
    bool tracking_ = false;
    bool moving_ = false;   // inside the modal move/size loop
    bool cloaked_ = false;  // kept hidden until the first frame is drawn
    bool busy_ = false;
    Action action_ = Action::None;
};
