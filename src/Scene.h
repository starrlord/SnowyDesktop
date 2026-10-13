// One monitor's worth of snow: falling flakes, landing, cursor interaction, the
// taskbar drift and the trees and snowmen standing in it. Pure simulation plus a
// Draw() that issues Direct2D calls.
#pragma once

#include "Common.h"
#include "Drift.h"
#include "FlakeArt.h"
#include "Lights.h"
#include "Settings.h"
#include "Scenery.h"

#include <d2d1_3.h>
#include <functional>

// Simulation parameters derived from Settings (resolution independent).
struct SimParams {
    float density = 260.0f;     // flakes per 1920x1080 DIP of screen area
    float speed = 1.0f;         // fall speed multiplier
    float wind = 0.0f;          // steady wind, DIP/s (positive blows right)
    float gusts = 0.5f;         // 0..1 wind variability
    float size = 1.0f;          // flake size multiplier
    float driftMaxDip = 30.0f;  // drift height cap in DIPs, 0 = no accumulation
    SnowColor color = SnowColor::White;
    COLORREF customColor = RGB(255, 255, 255);
    uint32_t flakeMask = kDefaultFlakeMask;
    bool mouse = true;
    bool trees = true;          // holiday trees in the drift
    int treeAmount = Scenery::kUsualAmount;  // 0..100: how many
    bool snowmen = true;        // snowmen too
    int snowmanAmount = Scenery::kUsualAmount;
    bool lights = false;        // strings of holiday lights
    BulbType bulb = BulbType::C7;
    LightColors lightColors = LightColors::Multicolor;
    COLORREF lightCustomColor = RGB(255, 190, 110);
    LightAnimation lightAnimation = LightAnimation::Twinkle;
    int lightSpeed = 50, lightSize = 50, lightGlow = 60;
    WireColor wire = WireColor::Green;
    LightLayout lightLayout = LightLayout::AcrossTop;
    std::vector<LightStrand> strands;  // every monitor's: the scene keeps its own (SceneGeometry::monitor)
};
SimParams MakeSimParams(const Settings& s);

struct SceneGeometry {
    int width = 0, height = 0;  // px
    float dpiScale = 1.0f;      // px per DIP
    float groundY = 0.0f;       // px from the top: line where snow lands (taskbar top)
    float originX = 0.0f;       // left edge in virtual-screen px (gusts sweep across monitors)
    float originScale = 1.0f;   // one global px per DIP (the primary monitor's) that maps virtual-screen px onto
                                // the shared gust timeline, so gusts are continuous across monitors of mixed DPI
    bool leftOpen = true;       // drift may spill off the left end
    bool rightOpen = true;
    bool plowable = true;       // false when the ground is the bare screen edge under an auto-hide taskbar: the
                                // mouse rests there, so it must not plow
    bool additiveGlow = true;   // lights may add light to the desktop (false where Windows composes in floating
                                // point and would clamp it away: they glow like coloured mist instead)
    std::wstring monitor;       // the monitor's stable id (MonitorTarget::deviceId): which strands hang here

    bool operator==(const SceneGeometry&) const = default;
};

struct CursorState {
    bool valid = false;
    float x = 0, y = 0;    // scene-local px
    float vx = 0, vy = 0;  // px/s
};

// Wind shared by every monitor: a deterministic function of time so that
// adjacent screens agree. Returns DIP/s.
float WindAt(const SimParams& p, double time);

class Scene {
public:
    Scene();
    ~Scene();
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    void Configure(const SceneGeometry& geo, const SimParams& params);
    void SetParams(const SimParams& params);
    const SceneGeometry& Geometry() const { return geo_; }

    // Advances the simulation. time is the shared clock (seconds).
    void Update(float dt, double time, const CursorState& cursor);

    // Draws flakes and drift into the current target of `dc` (cleared by the
    // caller). The target's DPI must be 96 so that DIPs equal pixels.
    void Draw(ID2D1DeviceContext* dc, const FlakeAtlas& atlas);

    // GPU resources (sprite batch, drift bitmap) belong to one device.
    void ReleaseDeviceResources();

    void ClearDrift(bool animated);

    // Speeds up accumulation (used by the small settings preview).
    void SetDriftBoost(float boost) { driftBoost_ = boost; }

    // Lets trees and snowmen step aside where a window covers them:
    // `covered(x, y)` says whether one covers that scene point. Call every so
    // often, and before drawing whenever CoverStale() (props new or shown again
    // take the answer at once). RecheckCover: probe them all afresh.
    void ProbeCover(const std::function<bool(float, float)>& covered) {
        scenery_.ProbeCover(covered);
        lights_.ProbeCover(covered);
    }
    bool CoverStale() const { return scenery_.CoverStale() || lights_.CoverStale(); }
    void RecheckCover() {
        scenery_.RecheckCover();
        lights_.RecheckCover();
    }

    // How long new tree and snowman amounts rest before the props are laid out
    // again (a dragged slider passes through many); 0: at once.
    void SetScenerySettle(float seconds) { scenery_.SetSettle(seconds); }

    // Hanging mode: the editor's strands and pointer for this monitor.
    void SetLightsEdit(const LightsEdit& edit) { lights_.SetEdit(edit); }

    size_t FlakeCount() const;

private:
    struct Flake;
    struct Particle;
    struct Glint;
    struct Bucket {
        ID2D1Bitmap1* page = nullptr;
        std::vector<D2D1_RECT_F> dst;
        std::vector<D2D1_RECT_U> src;
        std::vector<D2D1_COLOR_F> col;
        std::vector<D2D1_MATRIX_3X2_F> xf;
    };

    void SpawnFlake(Flake& f, bool initial);
    void ChooseLook(Flake& f);
    void Derive(Flake& f) const;
    void Land(Flake& f);
    void LandOnProp(Flake& f);
    void SpawnClumps();
    float SnowDepth() const;
    void LayoutField();
    void UpdateField();
    void SampleField(float x, float y, float& u, float& v, float& wind) const;
    void UpdateFlakes(float dt, const CursorState& cursor);
    void UpdateParticles(float dt);
    void UpdatePlow(float dt, const CursorState& cursor);
    void UpdateGlints(float dt);
    void ResizePopulation();
    int TargetCount() const;
    D2D1_COLOR_F Tint(const Flake& f) const;

    Bucket& BucketFor(ID2D1Bitmap1* page);
    void AddSprite(const FlakeAtlas& atlas, int sprite, float x, float y, float diameter, float angle,
                   D2D1_COLOR_F color, float squash = 1.0f);
    void AddScenerySprites(ID2D1Bitmap1* page);
    LightParams MakeLightParams() const;
    void FlushSprites(ID2D1DeviceContext* dc);
    void DrawDrift(ID2D1DeviceContext* dc);

    SceneGeometry geo_;
    SimParams params_;
    std::vector<Flake> flakes_;
    std::vector<Particle> particles_;
    std::vector<Glint> glints_;
    std::vector<FlakeKind> kinds_;  // enabled kinds
    Drift drift_;
    Scenery scenery_;
    Lights lights_;
    std::vector<SnowClump> clumps_;      // snow off the trees and snowmen this frame, becoming powder
    std::vector<ScenerySprite> scenerySprites_;  // scenery's or lights' sprites on their way to the batch
    Rng rng_;
    double time_ = 0.0;
    float windNow_ = 0.0f;  // px/s
    float glintTimer_ = 0.0f;
    float sprayBudget_ = 0.0f;
    float driftBoost_ = 1.0f;

    // Plowing: how long the current sweep has qualified (s), and where its
    // trench ends so far (px; ceiling is px above the ground line).
    float plowDwell_ = 0.0f;
    float plowX_ = 0.0f, plowCeiling_ = 0.0f;
    bool plowValid_ = false;

    // Air flow on a coarse grid: divergence-free turbulence (the curl of a
    // noise potential, so flakes never bunch into lanes) plus the wind, whose
    // gusts travel across the desktop downwind.
    int fieldCols_ = 0, fieldRows_ = 0;
    float fieldCell_ = 1.0f, fieldX0_ = 0.0f, fieldY0_ = 0.0f;
    std::vector<float> psi_, fieldU_, fieldV_, fieldWind_;
    double fieldTime_ = -1.0;
    float fieldRms_ = 0.0f;    // running RMS of the raw curl (normalizes amplitude)
    float fieldScale_ = 0.0f;  // raw curl -> px/s
    float fieldSeed_ = 0.0f;

    // Device resources.
    ComPtr<ID2D1Bitmap1> driftBitmap_;
    ComPtr<ID2D1SpriteBatch> batch_;
    ComPtr<ID2D1DeviceContext3> dc3_;
    ID2D1DeviceContext* dcOwner_ = nullptr;
    bool driftUploadAll_ = true;

    std::vector<Bucket> buckets_;
};
