// Per-monitor render thread.
//
// Each monitor gets its own thread with its own DirectComposition device and
// composition swap chain bound to that monitor's overlay window; all threads
// share one Direct3D/Direct2D device. Monitors that run at the primary display's refresh
// rate are paced by the swap chain's frame-latency waitable object (DWM's
// clock follows the primary display); monitors at a different rate wait for
// their own vertical blank, so a 60 Hz screen next to a 144 Hz one still gets
// evenly spaced frames. The animation keeps running while the UI thread is
// busy with menus or dialogs.
#pragma once

#include "Common.h"
#include "Scene.h"
#include "Settings.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

struct ID3D11Device;
struct IDXGIDevice;
struct IDXGIFactory2;

// One Direct3D 11 / Direct2D device shared by every monitor's render thread.
// Each GPU device costs the driver dozens of worker threads and tens of MB,
// so monitors share one; Direct2D's multithreaded factory serializes access
// and ID2D1Multithread guards the few direct DXGI calls.
struct GpuDevice {
    ComPtr<ID3D11Device> d3d;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2d;
    ComPtr<ID2D1Multithread> lock;
    std::atomic<bool> lost{false};  // set by whichever thread sees the device removed
    bool software = false;          // WARP fallback: replaced once a GPU is usable again

    // Flake sprites are device resources, so one atlas serves every monitor.
    // Threads hold a shared_ptr for each frame; a rebuild swaps in a new one.
    std::mutex atlasMutex;
    std::shared_ptr<FlakeAtlas> atlas;
    bool atlasShadow = false;
};

class GpuDeviceHub {
public:
    // The shared device, (re)created when missing or lost.
    HRESULT Acquire(std::shared_ptr<GpuDevice>& out);

private:
    std::mutex mutex_;
    std::shared_ptr<GpuDevice> device_;
};

struct MonitorTarget {
    HWND hwnd = nullptr;   // overlay window covering the monitor (owned by the UI thread)
    HMONITOR monitor = nullptr;
    RECT rect{};           // monitor rectangle, virtual-screen px
    int groundY = 0;       // px from rect.top where snow lands (taskbar top or screen bottom)
    UINT dpi = 96;
    float refreshHz = 60.0f;
    bool primary = false;
    bool primaryCadence = true;  // refresh rate matches the primary display
    bool leftOpen = true;        // drift may spill off this end
    bool rightOpen = true;
    bool autoHideGround = false; // ground is the screen edge under an auto-hide taskbar (no plowing)
    float originScale = 1.0f;    // primary display's px per DIP: shared gust timeline across monitors
    std::wstring key;            // stable identity used to keep drifts across rebuilds
};

class MonitorRenderer {
public:
    // Takes ownership of `scene` (pass null to start fresh). `gpu` must outlive
    // the renderer.
    MonitorRenderer(HWND notify, GpuDeviceHub& gpu, const MonitorTarget& target, std::unique_ptr<Scene> scene,
                    const Settings& settings, double clockOrigin);
    ~MonitorRenderer();
    MonitorRenderer(const MonitorRenderer&) = delete;
    MonitorRenderer& operator=(const MonitorRenderer&) = delete;

    void Start();
    // Stops the thread and hands back the scene so its drift can be reused.
    std::unique_ptr<Scene> Stop();

    void ApplySettings(const Settings& settings);
    void SetVisible(bool visible);
    void ClearDrift();
    // Caps the frame rate (0 = the display's refresh rate). Applied as a
    // whole number of refreshes per frame so motion stays evenly paced.
    void SetFrameCap(float fps);
    // Applies a changed ground line, open drift ends or pacing without
    // recreating the window or devices (same window, rectangle and DPI).
    void Reconfigure(const MonitorTarget& target);

    const MonitorTarget& Target() const { return target_; }

    struct Devices;  // per-thread device objects (Renderer.cpp)

private:
    void ThreadMain();
    HRESULT CreateDevices();
    HRESULT AcquireAtlas();
    void ReleaseDevices();
    void ProcessCommands();
    HRESULT RenderFrame();
    SceneGeometry GeometryFor(const MonitorTarget& t) const;
    float FrameDelta(double now);
    CursorState ReadCursor(double now);

    HWND notify_;
    GpuDeviceHub& gpuHub_;
    MonitorTarget target_;
    std::unique_ptr<Scene> scene_;
    double clockOrigin_;

    std::thread thread_;
    HANDLE wake_ = nullptr;  // auto-reset: commands pending

    // Commands from the UI thread.
    std::mutex mutex_;
    bool stop_ = false;
    bool hasSettings_ = false;
    Settings pendingSettings_;
    bool pendingVisible_ = true;
    bool clearDrift_ = false;
    float pendingCap_ = 0.0f;
    bool hasTarget_ = false;
    MonitorTarget pendingTarget_;

    // Render-thread state.
    Settings settings_;
    bool visible_ = true;
    int interval_ = 1;  // refreshes per frame (frame cap)
    std::unique_ptr<Devices> dev_;
    std::shared_ptr<FlakeAtlas> atlas_;  // shared via GpuDevice
    double lastFrame_ = 0.0;
    double period_ = 1.0 / 60.0;  // estimated refresh period (s)
    POINT lastCursor_{};
    double lastCursorTime_ = 0.0;
    float cursorVx_ = 0.0f, cursorVy_ = 0.0f;
    int failures_ = 0;

    // Optional frame statistics (SNOWYSTARR_DEBUG=1 writes %TEMP%\snowystarr.log).
    struct Stats {
        bool enabled = false;
        double windowStart = 0.0, lastPresent = 0.0;
        int frames = 0;
        double sumInterval = 0.0, maxInterval = 0.0, sumWork = 0.0, maxWork = 0.0;
        int late = 0;  // intervals > 1.5 refresh periods
    } stats_;
    void RecordFrame(double start, double end);
};
