// The settings window: a live preview of the snow and the lights, then two
// tabs. Snow has a grid of toggleable flake designs, sliders and switches;
// Lights has the bulbs, their colours and pattern, and where the strands hang.
// Drawn entirely with Direct2D as a small immediate-mode UI that follows the
// Windows light/dark theme. Changes apply live; the window posts
// WM_APP_SETTINGS_CHANGED to the app.
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
    enum class Tab { Snow, Lights };

    SettingsWindow(HINSTANCE instance, HWND app);
    ~SettingsWindow();
    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    // Opens on the Snow tab (or brings the open window forward, on whichever tab it shows).
    void Show(const Settings& s);
    // Switches tabs, e.g. to Snow for the tray's "Choose flakes...".
    void SelectTab(Tab tab);
    // Destroys the window (if open) but not the object; WM_APP_SETTINGS_CLOSED follows as usual.
    void Close();
    // Settings changed elsewhere (tray menu).
    void Update(const Settings& s);
    // Opens the colour picker after the next frame, like the Custom... chip. Call after Show() so the
    // dialog belongs to the visible window. Ignored while the picker is already open.
    void RequestCustomColor();
    // ... and for the lights' custom colour (`turnOn`: choosing one also turns
    // the lights on, as the tray's other light choices do).
    void RequestLightColor(bool turnOn = false);
    // Hides the window while lights are hung, and shows it again afterwards,
    // keeping its state (tab, focus). No-op when it is not open.
    void SetHidden(bool hidden);
    const Settings& Current() const { return settings_; }
    bool IsOpen() const { return hwnd_ != nullptr; }
    // True while a modal dialog (the colour picker) runs inside a frame. The window may be closed
    // meanwhile, but the object must not be deleted until this returns false.
    bool Busy() const { return busy_; }

    // Idle-loop rendering (see App::Run). Frame() returns false when nothing
    // was drawn (e.g. the window is occluded) so the caller can back off.
    bool WantsFrame() const;
    bool Frame();

#ifdef SNOWYSTARR_DEV_TOOLS
    // Developer builds: the window drawn offscreen into a PNG with no window at
    // all (tools/settingslab.cpp), after a few seconds of preview and any
    // clicks and keys given, one per frame.
    struct Snapshot {
        float scale = 1.0f;                 // px per DIP
        bool dark = false;
        Tab tab = Tab::Snow;
        std::vector<D2D1_POINT_2F> clicks;  // DIP
        std::vector<UINT> keys;             // virtual keys, after the clicks
        D2D1_POINT_2F hover{-1000, -1000};  // where the pointer rests for the picture (DIP)
    };
    static HRESULT RenderSnapshot(const Settings& s, const Snapshot& shot, const wchar_t* path);
#endif

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
    enum class Action { None, Close, CustomColor, LightColor, OpenRepo };
    // What the bulb tiles were drawn for: they show the chosen colours on the chosen wire.
    struct BulbLook {
        LightColors colors = LightColors::Count;
        COLORREF custom = 0;
        WireColor wire = WireColor::Count;
        bool operator==(const BulbLook&) const = default;
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HRESULT CreateDeviceIndependent();
    HRESULT CreateResources();
    HRESULT CreateTargetResources();
    void ReleaseResources();
#ifdef SNOWYSTARR_DEV_TOOLS
    HRESULT SaveSnapshot(const wchar_t* path);  // renders the window offscreen to a PNG
    HRESULT RenderOffscreen(UINT width, UINT height, const Snapshot& shot, IWICBitmap** out);
    std::wstring snapshotPath_;
#endif
    void BuildTileImages();
    void BuildBulbImages();
    void ConfigurePreview();
    SimParams PreviewParams() const;
    static Theme MakeTheme(bool dark);
    void ApplyTheme();
    void Cloak(bool on);
    void Render();
    void RunAction();
    void Changed() { changed_ = true; }
    float Snap(float v) const { return std::round(v * scale_) / scale_; }  // DIP on a whole device pixel
    float Measure(const wchar_t* s, IDWriteTextFormat* fmt) const;    // natural width in DIP

    // Pages.
    void TabBar(float y);
    void SnowPage(Settings& s, float y);
    void LightsPage(Settings& s, float y);
    void Footer(Settings& s);

    // Widgets.
    void Interact(int id, const D2D1_RECT_F& r, bool& hovered, bool& held, bool& clicked);
    bool Slider(int id, float x, float y, float w, const wchar_t* label, int& value, int lo, int hi, bool bipolar,
                const wchar_t* valueText);
    bool Toggle(int id, float x, float y, float w, const wchar_t* label, bool& value);
    bool Button(int id, const D2D1_RECT_F& r, const wchar_t* text, bool primary);
    // Right-aligned small text unless `fmt` says otherwise.
    bool Link(int id, const D2D1_RECT_F& r, const wchar_t* text, IDWriteTextFormat* fmt = nullptr);
    bool Tile(int id, const D2D1_RECT_F& r, FlakeKind kind, bool selected);
    bool BulbTile(int id, const D2D1_RECT_F& r, BulbType type, bool selected);
    bool ColorChip(int id, const D2D1_RECT_F& r, SnowColor color, const wchar_t* text, bool selected);
    // A small pill choosing one of a few options; `swatch` leaves room on the left for a colour dot.
    bool Chip(int id, const D2D1_RECT_F& r, const wchar_t* text, bool selected, bool swatch);
    // A wide option for where the strands hang: a little picture, a title and a line about it. Only the
    // part left of `hitRight` chooses it, leaving room for a button of its own.
    bool StrandCard(int id, const D2D1_RECT_F& r, float hitRight, LightLayout layout, const wchar_t* text,
                    bool selected, bool enabled);
    // The shared look of the tile widgets: background, selection ring and check badge.
    void TileFrame(const D2D1_RECT_F& r, bool hovered, bool selected);
    // One tab stop for a group of tiles or chips: the focused one, else the selected one.
    void GroupStop(int selectedId);

    void DrawPreview(const D2D1_RECT_F& r);
    void Text(const wchar_t* s, const D2D1_RECT_F& r, IDWriteTextFormat* fmt, const D2D1_COLOR_F& c);
    void FocusRing(int id, const D2D1_RECT_F& r, float radius);
    void Fill(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c);
    void Stroke(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c, float width);
    void Circle(float cx, float cy, float radius, const D2D1_COLOR_F& c);
    void Swatch(float cx, float cy, float radius, const D2D1_COLOR_F& c);  // a colour dot with a thin rim
    void TabIcon(Tab tab, float cx, float cy, const D2D1_COLOR_F& c);
    void StrandIcon(LightLayout layout, const D2D1_RECT_F& r);  // a little picture of where they hang

    HINSTANCE instance_;
    HWND app_;
    HWND hwnd_ = nullptr;
    Settings settings_;
    bool startWithWindows_ = false;
    UINT dpi_ = 96;       // the window's DPI (sizes the system-drawn frame)
    float scale_ = 1.0f;  // px per DIP: the DPI scale, reduced if needed to fit the work area
    Theme theme_;
    Tab tab_ = Tab::Snow;

    // Device-independent resources.
    ComPtr<ID2D1Factory1> factory_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IWICImagingFactory> wic_;
    ComPtr<IDWriteTextFormat> fmtTitle_, fmtSection_, fmtBody_, fmtSmall_, fmtValue_, fmtButton_, fmtChip_;
    ComPtr<ID2D1RoundedRectangleGeometry> previewClip_;
    ComPtr<ID2D1PathGeometry> strandWires_[static_cast<int>(LightLayout::Count)];  // in StrandIcon's pictures
    // Natural widths (DIP) of the Lights tab's chip labels, switch label and titles, measured once.
    float lightColorW_[static_cast<int>(LightColors::Count)] = {};
    float animationW_[static_cast<int>(LightAnimation::Count)] = {};
    float wireW_[static_cast<int>(WireColor::Count)] = {};
    float showLightsW_ = 0.0f, lightsTitleW_ = 0.0f, strandsTitleW_ = 0.0f;

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
    ComPtr<ID2D1Bitmap> bulbImages_[2][kBulbTypeCount];  // [lit][type]
    BulbLook bulbLook_;                                   // what bulbImages_ show
    FlakeAtlas atlas_;
    bool atlasShadow_ = true;  // outline setting of the last successful build
    double atlasRetry_ = 0.0;  // earliest time to retry a failed build

    // Live preview.
    std::unique_ptr<Scene> preview_;
    D2D1_RECT_F previewRect_{};
    std::vector<LightStrand> previewStrands_;  // the drape across its top
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
    double clearArmed_ = 0.0;  // "Clear my strands" waits for a second click until then
    bool changed_ = false;
    bool tracking_ = false;
    bool moving_ = false;   // inside the modal move/size loop
    bool cloaked_ = false;  // kept hidden until the first frame is drawn
    bool hidden_ = false;   // put away while lights are hung (SetHidden)
    bool busy_ = false;
    Action action_ = Action::None;
    bool lightColorTurnsOn_ = false;  // RequestLightColor(true): the picker's OK turns the lights on
};
