#include "Scene.h"

#include "Noise.h"

#include <atomic>

namespace {

constexpr float kRefArea = 1920.0f * 1080.0f;  // DIP^2 that `density` refers to
constexpr float kBackgroundDepth = 0.2f;        // flakes farther than this pass behind the taskbar
constexpr size_t kMaxParticles = 900;
constexpr float kPlowDwell = 0.035f;            // s a sweep must last before it moves snow
constexpr float kPropFront = 0.78f;             // flakes nearer than this pass in front of trees and snowmen

// Noise inputs repeat every 256 units; wrapping in double keeps float precision
// for an app that may run for weeks.
float Wrap256(double v) { return static_cast<float>(std::fmod(v, 256.0)); }

D2D1_COLOR_F HsvToRgb(float h, float s, float v) {
    h = (h - std::floor(h)) * 6.0f;
    int i = static_cast<int>(h);
    float f = h - static_cast<float>(i);
    float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (i % 6) {
    case 0: return {v, t, p, 1};
    case 1: return {q, v, p, 1};
    case 2: return {p, v, t, 1};
    case 3: return {p, q, v, 1};
    case 4: return {t, p, v, 1};
    default: return {v, p, q, 1};
    }
}

// Sprite batch colours are straight alpha like every other Direct2D colour
// (measured: (1,1,1,0.5) over black gives 50% white); D2D premultiplies them.
D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) { return {c.r, c.g, c.b, a}; }

// Rotation in the sprite's plane, then a horizontal squash (a plate tumbling
// about the vertical axis), then the position.
D2D1_MATRIX_3X2_F SpriteTransform(float angle, float squash, float x, float y) {
    float c = std::cos(angle), s = std::sin(angle);
    return {c * squash, s, -s * squash, c, x, y};
}

bool IsCrystal(FlakeKind k) {
    return static_cast<int>(k) >= static_cast<int>(FlakeKind::Dendrite) &&
           static_cast<int>(k) <= static_cast<int>(FlakeKind::Twelve);
}

// How each kind moves, relative to a generic flake.
struct Motion {
    float fall;   // terminal speed factor
    float sway;   // flutter amplitude factor
    float alpha;  // opacity factor
};
Motion MotionFor(FlakeKind k) {
    switch (k) {
    case FlakeKind::Dot: return {0.78f, 0.55f, 0.95f};
    case FlakeKind::Soft: return {0.9f, 0.8f, 0.85f};
    case FlakeKind::Sparkle: return {0.85f, 0.7f, 1.0f};
    case FlakeKind::Needle: return {1.1f, 0.8f, 1.0f};
    case FlakeKind::HexPlate:
    case FlakeKind::Sectored:
    case FlakeKind::StellarPlate: return {1.0f, 1.25f, 1.0f};
    default: return {1.0f, 1.0f, 1.0f};
    }
}

}  // namespace

// ---------------------------------------------------------------------------

struct Scene::Flake {
    // Position and velocity in px, px/s. vx/vy exclude the sway oscillation.
    float x = 0, y = 0, vx = 0, vy = 0;
    float z = 0.5f;  // depth: 0 far .. 1 near
    // Random "genes", fixed for the flake's life; Derive() turns them into
    // sizes and speeds so settings changes apply instantly.
    float uSize = 0.5f, uFall = 0.5f, uSway = 0.5f, hue = 0.0f;
    uint32_t id = 0;  // tells it apart where it might settle on the props
    int variant = 0;
    FlakeKind kind = FlakeKind::Soft;
    // Derived.
    float diameter = 8.0f, fall = 60.0f, swayAmp = 0.0f, alpha = 1.0f;
    int sprite = 0;
    // Motion state.
    float tau = 1.0f;  // drag time constant: how quickly it follows the air
    float swayPhase = 0, swayFreq = 1;
    float angle = 0, spin = 0, twinkle = 0;
    float tumble = 0, tumbleSpeed = 0;  // crystals slowly turn edge-on and back
    float born = 1.0f;                // fade-in 0..1
    float life = 0.0f, lifeMax = 1.0f;  // while landed: remaining fade time
    bool landed = false;
    Perch perch;      // while landed on a bough or a hat
    bool background = false;
    bool retire = false;  // remove instead of respawning
    bool dead = false;
};

struct Scene::Particle {
    float x, y, vx, vy, d, life, maxLife;
    bool deposit;          // returns to the drift (or a bough) when it lands
    uint32_t id = 0;       // (as a flake's)
    float noCatch = 0.0f;  // s before it can settle on a bough (snow just thrown off one)
    bool puff = false;     // a soft burst of powder that drifts, swells and fades
};

struct Scene::Glint {
    float x, life, maxLife, size;
};

SimParams MakeSimParams(const Settings& s) {
    SimParams p;
    // Flurries ~100, Light ~170, Moderate ~270, Heavy ~570, Blizzard ~1400.
    p.density = 70.0f * std::pow(20.0f, static_cast<float>(s.amount) / 100.0f);
    p.speed = std::pow(2.5f, static_cast<float>(s.speed - 50) / 50.0f);
    p.wind = static_cast<float>(s.wind) * 1.6f;
    p.gusts = static_cast<float>(s.gusts) / 100.0f;
    p.size = std::pow(2.0f, static_cast<float>(s.size - 50) / 50.0f);
    // Up to 56 DIP at 100; the range above it reaches twice that.
    const float drift = static_cast<float>(s.drift);
    p.driftMaxDip = s.drift <= 0 ? 0.0f : drift <= 100.0f ? 6.0f + 0.5f * drift : 56.0f + 0.56f * (drift - 100.0f);
    p.color = s.color;
    p.customColor = s.customColor;
    p.flakeMask = s.flakeMask;
    p.mouse = s.mouse;
    p.trees = s.trees;
    p.treeAmount = s.treeAmount;
    p.snowmen = s.snowmen;
    p.snowmanAmount = s.snowmanAmount;
    return p;
}

float WindAt(const SimParams& p, double time) {
    // Slow swings over ~30 s, a faster flutter, and occasional gust bursts
    // that blow with the prevailing wind.
    float slow = noise::Fbm1(Wrap256(time * 0.031), 3);
    float fast = noise::Fbm1(Wrap256(time * 0.19 + 91.7), 2);
    float b = noise::Noise1(Wrap256(time * 0.067 + 13.1));
    float burst = SmoothStep(0.15f, 0.75f, b);
    // Bursts blow with the prevailing wind; in calm air their direction
    // wanders slowly (smoothly, so a burst never flips mid-gust).
    float dir = p.wind > 2.0f    ? 1.0f
                : p.wind < -2.0f ? -1.0f
                                 : std::tanh(6.0f * noise::Noise1(Wrap256(time * 0.011 + 57.3)));
    float g = p.gusts;
    return p.wind * (1.0f + 0.4f * g * slow) + g * (34.0f * slow + 10.0f * fast) + dir * g * 110.0f * burst;
}

Scene::Scene() {
    // Full-resolution ticks mixed with an instance count, and a PCG stream of
    // its own, so scenes created together (one per monitor) never share a snowfall.
    static std::atomic<uint64_t> instances{0};
    const uint64_t n = instances.fetch_add(1, std::memory_order_relaxed);
    LARGE_INTEGER ticks;
    QueryPerformanceCounter(&ticks);
    rng_.Seed(static_cast<uint64_t>(ticks.QuadPart) ^ (n * 0xD1B54A32D192ED03ull), 0x9E3779B97F4A7C15ull + 2 * n);
}
Scene::~Scene() = default;

size_t Scene::FlakeCount() const { return flakes_.size(); }

void Scene::Configure(const SceneGeometry& geo, const SimParams& params) {
    bool resized = geo.width != geo_.width || geo.height != geo_.height || geo.dpiScale != geo_.dpiScale ||
                   geo.groundY != geo_.groundY;
    geo_ = geo;
    params_ = params;
    DriftParams dp;
    dp.maxHeight = params.driftMaxDip * geo.dpiScale;
    dp.dpiScale = geo.dpiScale;
    dp.leftOpen = geo.leftOpen;
    dp.rightOpen = geo.rightOpen;
    drift_.Configure(geo.width, dp);
    scenery_.Configure({geo.width, geo.height, geo.dpiScale, geo.groundY, geo.originX});
    scenery_.SetAdditiveGlow(geo.additiveGlow);
    if (resized) {
        flakes_.clear();
        particles_.clear();
        glints_.clear();
        driftBitmap_.Reset();
    }
    plowDwell_ = 0.0f;
    plowValid_ = false;
    driftUploadAll_ = true;
    LayoutField();
    SetParams(params);
}

// ---------------------------------------------------------------------------
// Air flow
// ---------------------------------------------------------------------------

void Scene::LayoutField() {
    const float s = geo_.dpiScale;
    const float margin = 64.0f * s;
    fieldCell_ = 110.0f * s;
    fieldX0_ = -margin;
    fieldY0_ = -margin;
    fieldCols_ = static_cast<int>(std::ceil((static_cast<float>(geo_.width) + 2.0f * margin) / fieldCell_)) + 1;
    fieldRows_ = static_cast<int>(std::ceil((static_cast<float>(geo_.height) + margin) / fieldCell_)) + 1;
    fieldCols_ = std::max(fieldCols_, 2);
    fieldRows_ = std::max(fieldRows_, 2);
    psi_.assign(static_cast<size_t>(fieldCols_ + 2) * (fieldRows_ + 2), 0.0f);
    fieldU_.assign(static_cast<size_t>(fieldCols_) * fieldRows_, 0.0f);
    fieldV_.assign(fieldU_.size(), 0.0f);
    fieldWind_.assign(static_cast<size_t>(fieldCols_), 0.0f);
    fieldTime_ = -1.0;
    if (fieldSeed_ == 0.0f) fieldSeed_ = rng_.Range(0.0f, 200.0f);
}

void Scene::UpdateField() {
    // The field changes over seconds; 60 updates per second is plenty even
    // on 144-240 Hz displays (flakes interpolate it bilinearly in space).
    if (fieldTime_ >= 0.0 && time_ - fieldTime_ < 1.0 / 60.0 && time_ >= fieldTime_) return;
    fieldTime_ = time_;
    const float s = geo_.dpiScale;

    // Potential: two octaves of simplex noise, features ~420 and ~180 DIP,
    // evolving over ~10 s. Velocity is its curl, so it is divergence-free.
    const float f1 = 1.0f / (420.0f * s), f2 = f1 * 2.3f;
    // Simplex3 is not periodic in time, so wrap rarely: one seam every ~15 h
    // (and flake drag smooths it); float precision is still ample at 4096.
    const float tz = static_cast<float>(std::fmod(time_ * 0.075, 4096.0));
    const int pc = fieldCols_ + 2, pr = fieldRows_ + 2;
    for (int j = 0; j < pr; ++j) {
        const float y = fieldY0_ + static_cast<float>(j - 1) * fieldCell_;
        for (int i = 0; i < pc; ++i) {
            const float x = fieldX0_ + static_cast<float>(i - 1) * fieldCell_;
            psi_[static_cast<size_t>(j) * pc + i] =
                noise::Simplex3(x * f1 + fieldSeed_, y * f1 - fieldSeed_, tz) +
                0.45f * noise::Simplex3(x * f2 + 17.3f, y * f2 - 9.1f, tz * 1.7f + 3.3f);
        }
    }
    double sum2 = 0.0;
    for (int j = 0; j < fieldRows_; ++j) {
        for (int i = 0; i < fieldCols_; ++i) {
            const size_t c = static_cast<size_t>(j + 1) * pc + (i + 1);
            const float u = psi_[c + pc] - psi_[c - pc];  // d(psi)/dy
            const float v = psi_[c - 1] - psi_[c + 1];    // -d(psi)/dx
            fieldU_[static_cast<size_t>(j) * fieldCols_ + i] = u;
            fieldV_[static_cast<size_t>(j) * fieldCols_ + i] = v;
            sum2 += static_cast<double>(u) * u + static_cast<double>(v) * v;
        }
    }
    // Normalize to the target strength with a slow running RMS, so there are
    // still lulls and surges from moment to moment.
    const float rms = static_cast<float>(std::sqrt(sum2 / (2.0 * fieldCols_ * fieldRows_)));
    fieldRms_ = fieldRms_ <= 0.0f ? rms : fieldRms_ + (rms - fieldRms_) * 0.003f;
    const float target = (5.0f + 16.0f * params_.gusts) * s;  // px/s per component
    fieldScale_ = fieldRms_ > 1e-6f ? target / fieldRms_ : 0.0f;

    // Wind gusts travel downwind across the whole desktop: a column feels the
    // gust that the column upwind of it felt a moment ago. Its place on that
    // timeline uses one desktop-wide scale, so monitors of mixed DPI line up.
    const float speed = std::max(std::fabs(params_.wind), 40.0f) * (params_.wind < 0.0f ? -1.0f : 1.0f);
    const float originScale = geo_.originScale > 0.0f ? geo_.originScale : s;
    for (int i = 0; i < fieldCols_; ++i) {
        const float xDip = (geo_.originX + fieldX0_ + static_cast<float>(i) * fieldCell_) / originScale;
        fieldWind_[static_cast<size_t>(i)] = WindAt(params_, time_ - static_cast<double>(xDip / speed)) * s;
    }
}

void Scene::SampleField(float x, float y, float& u, float& v, float& wind) const {
    const float fx = (x - fieldX0_) / fieldCell_, fy = (y - fieldY0_) / fieldCell_;
    const int i = Clamp(static_cast<int>(std::floor(fx)), 0, fieldCols_ - 2);
    const int j = Clamp(static_cast<int>(std::floor(fy)), 0, fieldRows_ - 2);
    const float tx = Saturate(fx - static_cast<float>(i)), ty = Saturate(fy - static_cast<float>(j));
    const size_t a = static_cast<size_t>(j) * fieldCols_ + i, b = a + fieldCols_;
    auto bilerp = [&](const std::vector<float>& g) {
        return Lerp(Lerp(g[a], g[a + 1], tx), Lerp(g[b], g[b + 1], tx), ty);
    };
    u = bilerp(fieldU_) * fieldScale_;
    v = bilerp(fieldV_) * fieldScale_;
    wind = Lerp(fieldWind_[static_cast<size_t>(i)], fieldWind_[static_cast<size_t>(i) + 1], tx);
}

void Scene::SetParams(const SimParams& params) {
    params_ = params;

    kinds_.clear();
    for (int k = 0; k < kFlakeKindCount; ++k)
        if (params.flakeMask & (1u << k)) kinds_.push_back(static_cast<FlakeKind>(k));
    if (kinds_.empty()) kinds_.push_back(FlakeKind::Soft);

    DriftParams dp = drift_.Params();
    dp.maxHeight = params.driftMaxDip * geo_.dpiScale;
    drift_.SetParams(dp);
    scenery_.SetTrees(params.trees, params.treeAmount);
    scenery_.SetSnowmen(params.snowmen, params.snowmanAmount);
    scenery_.SetSnowDepth(SnowDepth());

    for (Flake& f : flakes_) {
        if (!(params.flakeMask & (1u << static_cast<int>(f.kind)))) {
            // Swap the look in place and fade the new one in.
            ChooseLook(f);
            f.born = 0.0f;
        }
        Derive(f);
    }
}

// How deep snow lies on boughs and hats follows the drift setting (none when drifts are off).
float Scene::SnowDepth() const {
    return params_.driftMaxDip > 0.0f ? Clamp(params_.driftMaxDip / 33.5f, 0.4f, 1.4f) : 0.0f;
}

int Scene::TargetCount() const {
    float s = geo_.dpiScale;
    float area = (static_cast<float>(geo_.width) / s) * (static_cast<float>(geo_.height) / s);
    return static_cast<int>(params_.density * area / kRefArea + 0.5f);
}

void Scene::ResizePopulation() {
    int target = TargetCount();
    int live = 0;
    for (Flake& f : flakes_) {
        if (f.retire) continue;
        if (live >= target) f.retire = true;  // leaves when it next lands or exits
        else ++live;
    }
    // Reinstate retiring flakes before creating new ones.
    for (Flake& f : flakes_) {
        if (live >= target) break;
        if (f.retire) {
            f.retire = false;
            ++live;
        }
    }
    while (live < target) {
        flakes_.emplace_back();
        SpawnFlake(flakes_.back(), true);
        ++live;
    }
}

void Scene::ChooseLook(Flake& f) {
    f.kind = kinds_[static_cast<size_t>(rng_.Int(0, static_cast<int>(kinds_.size()) - 1))];
    f.variant = rng_.Int(0, FlakeVariantCount(f.kind) - 1);
    f.sprite = FlakeAtlas::SpriteIndex(f.kind, f.variant);
    const FlakeTraits& t = GetFlakeTraits(f.kind);
    f.spin = t.rotates ? rng_.Range(-0.9f, 0.9f) : 0.0f;
    f.angle = t.rotates ? rng_.Range(0.0f, kTau) : 0.0f;
    f.twinkle = rng_.Range(0.0f, kTau);
    f.tumble = rng_.Range(0.0f, kTau);
    f.tumbleSpeed = IsCrystal(f.kind) ? rng_.Range(0.3f, 1.0f) * (rng_.Chance(0.5f) ? 1.0f : -1.0f) : 0.0f;
}

void Scene::Derive(Flake& f) const {
    const float s = geo_.dpiScale;
    const FlakeTraits& t = GetFlakeTraits(f.kind);
    const Motion m = MotionFor(f.kind);
    float parallax = 0.5f + 0.9f * f.z;  // near flakes are bigger and faster
    float dip = Lerp(t.minDip, t.maxDip, f.uSize * f.uSize);
    f.diameter = std::max(1.6f, dip * params_.size * parallax * s);
    f.fall = 58.0f * s * parallax * params_.speed * m.fall * Lerp(0.82f, 1.18f, f.uFall);
    f.swayAmp = Lerp(3.0f, 15.0f, f.uSway) * s * parallax * m.sway;
    f.alpha = Lerp(0.38f, 1.0f, f.z) * m.alpha;
}

void Scene::SpawnFlake(Flake& f, bool initial) {
    f = Flake{};
    f.z = std::pow(rng_.Float(), 1.35f);  // more distant flakes than near ones
    f.uSize = rng_.Float();
    f.uFall = rng_.Float();
    f.uSway = rng_.Float();
    f.hue = rng_.Float();
    f.id = rng_.Next();
    ChooseLook(f);
    Derive(f);
    f.tau = rng_.Range(0.45f, 1.25f);
    f.swayFreq = kTau * rng_.Range(0.16f, 0.5f);
    f.swayPhase = rng_.Range(0.0f, kTau);
    f.background = f.z < kBackgroundDepth;

    const float s = geo_.dpiScale;
    f.x = rng_.Range(0.0f, static_cast<float>(geo_.width));
    if (initial) {
        f.y = rng_.Range(-0.05f * static_cast<float>(geo_.height), geo_.groundY - 6.0f * s);
        f.born = 0.0f;
    } else {
        f.y = -f.diameter * 1.5f - rng_.Range(0.0f, 60.0f * s);
        f.born = 1.0f;  // enters from off screen
    }
    f.vx = windNow_ * (0.5f + 0.9f * f.z);
    f.vy = f.fall;
}

void Scene::Land(Flake& f) {
    f.landed = true;
    f.lifeMax = f.life = rng_.Range(0.9f, 2.4f) * (f.kind == FlakeKind::Dot ? 0.6f : 1.0f);
    // A flake straddling a screen edge feeds the end column.
    drift_.Deposit(Clamp(f.x, 0.0f, static_cast<float>(geo_.width) - 1.0f), f.diameter, driftBoost_);

    // An occasional soft puff of powder when a bigger flake lands.
    const float s = geo_.dpiScale;
    if (f.diameter > 7.0f * s && rng_.Chance(0.3f) && particles_.size() < kMaxParticles) {
        int n = rng_.Int(1, 3);
        for (int i = 0; i < n; ++i) {
            Particle p;
            p.x = f.x + rng_.Range(-0.3f, 0.3f) * f.diameter;
            p.y = f.y;
            p.vx = rng_.Range(-35.0f, 35.0f) * s + f.vx * 0.3f;
            p.vy = -rng_.Range(25.0f, 70.0f) * s;
            p.d = rng_.Range(1.4f, 2.6f) * s;
            p.maxLife = p.life = rng_.Range(0.3f, 0.6f);
            p.deposit = false;
            particles_.push_back(p);
        }
    }
}

// A flake settling on a bough or a hat melts into the snow there quickly; a
// big or soft one at once, so none sits there like a cotton ball (its snow
// still counts).
void Scene::LandOnProp(Flake& f) {
    f.landed = true;
    const float s = geo_.dpiScale;
    const bool melts = f.diameter > 5.0f * s || f.kind == FlakeKind::Soft;
    f.lifeMax = f.life = melts ? 0.15f : rng_.Range(0.35f, 0.7f) * (f.kind == FlakeKind::Dot ? 0.6f : 1.0f);
}

// Snow knocked or slid off the scenery falls as powder onto the drift, some of
// it bursting into a soft puff.
void Scene::SpawnClumps() {
    const float s = geo_.dpiScale;
    for (const SnowClump& c : clumps_) {
        if (particles_.size() >= kMaxParticles) break;
        Particle p;
        p.x = c.x;
        p.y = c.y;
        p.vx = c.vx;
        p.vy = c.vy;
        p.d = c.d;
        p.maxLife = p.life = 4.0f;
        p.deposit = true;
        p.id = rng_.Next();
        p.noCatch = 0.15f;
        particles_.push_back(p);
        if (rng_.Chance(0.3f) && particles_.size() < kMaxParticles) {
            Particle puff = p;
            puff.vx = 0.25f * c.vx;
            puff.vy = 12.0f * s;
            puff.d = std::max(4.5f * s, 2.6f * c.d);
            puff.maxLife = puff.life = rng_.Range(0.35f, 0.55f);
            puff.deposit = false;
            puff.puff = true;
            particles_.push_back(puff);
        }
    }
    clumps_.clear();
}

void Scene::Update(float dt, double time, const CursorState& cursor) {
    dt = Clamp(dt, 0.0f, 0.1f);
    time_ = time;
    UpdateField();
    windNow_ = fieldWind_.empty() ? WindAt(params_, time) * geo_.dpiScale : fieldWind_[fieldWind_.size() / 2];
    ResizePopulation();
    scenery_.Update(dt, time, windNow_, drift_, clumps_);
    UpdateFlakes(dt, cursor);
    // (A pointer resting on a bare screen edge to bring up an auto-hide taskbar
    // leaves the props alone, as it leaves the drift.)
    if (params_.mouse && cursor.valid && (geo_.plowable || cursor.y < geo_.groundY - 2.0f * geo_.dpiScale))
        scenery_.Touch(dt, cursor.x, cursor.y, cursor.vx, cursor.vy, clumps_);
    SpawnClumps();
    UpdatePlow(dt, cursor);
    UpdateParticles(dt);
    drift_.Update(dt, windNow_);
    UpdateGlints(dt);
}

void Scene::UpdateFlakes(float dt, const CursorState& cursor) {
    const float s = geo_.dpiScale;
    const float W = static_cast<float>(geo_.width);
    const float margin = 48.0f * s;
    const bool driftOn = drift_.Enabled();
    const bool propsOn = scenery_.Active();

    // Cursor influence: moving air around the pointer carries flakes along,
    // and a gentle push keeps them from sitting on it.
    const bool useCursor = params_.mouse && cursor.valid;
    const float radius = 85.0f * s;
    const float radius2 = radius * radius;
    float cvx = cursor.vx, cvy = cursor.vy;
    float cspeed = std::sqrt(cvx * cvx + cvy * cvy);
    const float maxCarry = 900.0f * s;
    if (cspeed > maxCarry) {
        cvx *= maxCarry / cspeed;
        cvy *= maxCarry / cspeed;
    }

    for (Flake& f : flakes_) {
        if (f.dead) continue;
        if (f.born < 1.0f) f.born = std::min(1.0f, f.born + dt / 1.2f);

        if (f.landed) {
            f.life -= dt;
            if (f.perch.kind >= 0) {
                float px, py;
                if (!scenery_.PerchPoint(f.perch, px, py)) {
                    // The snow under it is gone (knocked off, melted): it falls again.
                    f.landed = false;
                    f.perch = Perch{};
                    f.vx = 0.0f;
                    f.vy = 0.3f * f.fall;
                    continue;
                }
                f.x = px;
                f.y = py - f.diameter * 0.12f;
            } else {
                f.y = geo_.groundY - drift_.HeightAt(f.x) - f.diameter * 0.12f;
            }
            if (f.life <= 0.0f) {
                if (f.retire) f.dead = true;
                else SpawnFlake(f, false);
            }
            continue;
        }

        // Nearer flakes cross the screen faster (parallax), for wind and
        // turbulence alike; the air itself is shared by every depth.
        const float parallax = 0.5f + 0.9f * f.z;
        float tu, tv, wind;
        SampleField(f.x, f.y, tu, tv, wind);
        float targetVx = (wind + tu) * parallax;
        float targetVy = f.fall + tv * parallax;

        float k = ApproachFactor(dt, f.tau);
        f.vx += (targetVx - f.vx) * k;
        f.vy += (targetVy - f.vy) * k;

        if (useCursor) {
            float dx = f.x - cursor.x, dy = f.y - cursor.y;
            float d2 = dx * dx + dy * dy;
            if (d2 < radius2) {
                float d = std::sqrt(d2) + 0.001f;
                float w = 1.0f - d / radius;
                w *= w;
                float carry = ApproachFactor(dt, 1.0f / 9.0f) * w;
                f.vx += (cvx * 0.85f - f.vx) * carry;
                f.vy += (cvy * 0.85f - f.vy) * carry;
                float push = 300.0f * s * w * dt;
                f.vx += dx / d * push;
                f.vy += dy / d * push;
            }
        }

        const float y0 = f.y;
        f.swayPhase += f.swayFreq * dt;
        if (f.swayPhase > kTau) f.swayPhase -= kTau;
        float swayV = std::cos(f.swayPhase) * f.swayAmp * f.swayFreq;
        // Plates fall a touch faster when they rock edge-on.
        float flutter = 1.0f + 0.1f * std::sin(2.0f * f.swayPhase);
        f.x += (f.vx + swayV) * dt;
        f.y += f.vy * flutter * dt;
        f.angle += f.spin * dt;
        f.twinkle += dt * 5.0f;
        f.tumble += f.tumbleSpeed * dt;
        if (f.tumble > kTau) f.tumble -= kTau;
        else if (f.tumble < 0.0f) f.tumble += kTau;

        if (f.x < -margin) f.x += W + 2.0f * margin;
        else if (f.x > W + margin) f.x -= W + 2.0f * margin;

        float r = f.diameter * 0.5f;
        // Flakes at the props' depth settle on the boughs and hats they come down on.
        if (propsOn && !f.background && f.z < kPropFront && f.vy > 0.0f &&
            f.y + r >= scenery_.Top() && scenery_.Catch(f.x, y0, f.y, r, driftBoost_, f.id, &f.perch)) {
            LandOnProp(f);
            continue;
        }
        if (driftOn && !f.background && f.x > -r && f.x < W + r) {
            float surface = geo_.groundY - drift_.HeightAt(Clamp(f.x, 0.0f, W - 1.0f));
            if (f.y + r * 0.5f >= surface && f.vy > 0.0f) {
                Land(f);
                continue;
            }
        }
        // Gone below the ground line (behind the taskbar) or pushed far off the top.
        if (f.y - r * 2.0f > geo_.groundY || f.y < -static_cast<float>(geo_.height)) {
            if (f.retire) f.dead = true;
            else SpawnFlake(f, false);
        }
    }

    flakes_.erase(std::remove_if(flakes_.begin(), flakes_.end(), [](const Flake& f) { return f.dead; }),
                  flakes_.end());
}

void Scene::UpdatePlow(float dt, const CursorState& cursor) {
    const float s = geo_.dpiScale;
    // Only a deliberate sideways sweep through the snow moves it: within ~27
    // degrees of horizontal and held for a moment, so trips down to the
    // taskbar don't cut notches. A pointer resting on a bare screen edge
    // (auto-hide taskbar) never plows.
    const float ax = std::fabs(cursor.vx);
    float ceiling = 0.0f;  // cursor height above the ground line
    bool sweep = params_.mouse && cursor.valid && geo_.plowable && drift_.Enabled() && ax >= 40.0f * s &&
                 std::fabs(cursor.vy) <= 0.5f * ax && cursor.x >= 0.0f && cursor.x < static_cast<float>(geo_.width);
    if (sweep) {
        ceiling = geo_.groundY - cursor.y;
        sweep = ceiling >= -2.0f * s && ceiling < drift_.HeightAt(cursor.x);
    }
    if (!sweep) {
        plowDwell_ = 0.0f;
        plowValid_ = false;
        return;
    }
    plowDwell_ += dt;
    if (!plowValid_) {
        // The trench starts here if the sweep lasts.
        plowX_ = cursor.x;
        plowCeiling_ = ceiling;
        plowValid_ = true;
        return;
    }
    if (plowDwell_ < kPlowDwell) return;

    // Cut the whole span swept since the last cut in steps no wider than the
    // scoop, so the trench is clean at any pointer speed and frame rate.
    const float r = 8.0f * s;
    const float x0 = plowX_, c0 = plowCeiling_, span = cursor.x - x0;
    const int n = std::max(1, static_cast<int>(std::ceil(std::fabs(span) / r)));
    float removed = 0.0f;
    for (int k = 0; k <= n; ++k) {
        const float t = static_cast<float>(k) / static_cast<float>(n);
        const float x = x0 + span * t;
        removed += drift_.Plow(x - r, x + r, std::max(0.0f, Lerp(c0, ceiling, t)));
    }
    plowX_ = cursor.x;
    plowCeiling_ = ceiling;

    sprayBudget_ += removed;
    const float perParticle = 10.0f * s * s;
    const float vx = Clamp(cursor.vx, -900.0f * s, 900.0f * s);
    const float dir = vx > 0.0f ? 1.0f : -1.0f;
    while (sprayBudget_ > perParticle && particles_.size() < kMaxParticles) {
        sprayBudget_ -= perParticle;
        const float t = rng_.Float();  // spread along the swept span
        Particle p;
        p.x = x0 + span * t + rng_.Range(-r, r);
        p.y = geo_.groundY - std::max(0.0f, Lerp(c0, ceiling, t)) - rng_.Range(0.0f, 4.0f * s);
        p.vx = vx * rng_.Range(0.25f, 0.6f) + dir * rng_.Range(10.0f, 60.0f) * s;
        p.vy = -rng_.Range(60.0f, 220.0f) * s;
        p.d = rng_.Range(1.6f, 3.4f) * s;
        p.maxLife = p.life = 4.0f;
        p.deposit = true;
        p.id = rng_.Next();
        particles_.push_back(p);
    }
    if (particles_.size() >= kMaxParticles) sprayBudget_ = 0.0f;
}

void Scene::UpdateParticles(float dt) {
    const float s = geo_.dpiScale;
    const float g = 420.0f * s;
    const float drag = std::exp(-dt * 1.6f);
    const float W = static_cast<float>(geo_.width);
    const bool propsOn = scenery_.Active();
    for (Particle& p : particles_) {
        if (p.puff) {
            // Drifts, slows and fades; never settles.
            const float damp = std::exp(-6.0f * dt);
            p.vx *= damp;
            p.vy *= damp;
            p.x += p.vx * dt;
            p.y += p.vy * dt;
            p.life -= dt;
            continue;
        }
        const float y0 = p.y;
        p.vy += g * dt;
        p.vx = p.vx * drag + windNow_ * (1.0f - drag) * 0.5f;
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.life -= dt;
        // Snow coming down on a bough stays there.
        if (p.noCatch > 0.0f) {
            p.noCatch -= dt;
        } else if (propsOn && p.deposit && p.vy > 0.0f && p.y >= scenery_.Top() &&
                   scenery_.Catch(p.x, y0, p.y, 0.5f * p.d, 1.0f, p.id, nullptr)) {
            p.life = 0.0f;
            continue;
        }
        // Powder stops where it comes down on the snow (spray from the drift
        // returns to it); past the ends it vanishes behind the taskbar.
        if (p.x >= 0.0f && p.x < W) {
            if (p.vy > 0.0f && p.y >= geo_.groundY - drift_.HeightAt(p.x)) {
                if (p.deposit) drift_.Deposit(p.x, p.d, 1.0f);
                p.life = 0.0f;
            }
        } else if (p.y > geo_.groundY) {
            p.life = 0.0f;
        }
    }
    particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
                                    [](const Particle& p) { return p.life <= 0.0f; }),
                     particles_.end());
}

void Scene::UpdateGlints(float dt) {
    for (Glint& g : glints_) g.life -= dt;
    glints_.erase(std::remove_if(glints_.begin(), glints_.end(), [](const Glint& g) { return g.life <= 0.0f; }),
                  glints_.end());
    if (drift_.Empty()) return;

    // A few twinkles per second along the drift, proportional to its width.
    glintTimer_ -= dt;
    if (glintTimer_ > 0.0f) return;
    float perSecond = 1.6f * static_cast<float>(geo_.width) / (1000.0f * geo_.dpiScale);
    glintTimer_ = -std::log(std::max(rng_.Float(), 1e-4f)) / std::max(perSecond, 0.1f);
    float x, h;
    if (glints_.size() < 24 && drift_.RandomSurfacePoint(rng_, x, h) && h > 2.0f * geo_.dpiScale) {
        Glint g;
        g.x = x;
        g.maxLife = g.life = rng_.Range(0.5f, 1.3f);
        g.size = rng_.Range(5.0f, 10.0f) * geo_.dpiScale;
        glints_.push_back(g);
    }
}

void Scene::ClearDrift(bool animated) {
    if (animated) drift_.MeltAway(2.0f);
    else drift_.Clear();
    scenery_.ClearSnow(animated);
    for (Flake& f : flakes_)
        if (f.landed) f.life = std::min(f.life, 0.4f);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

D2D1_COLOR_F Scene::Tint(const Flake& f) const {
    switch (params_.color) {
    case SnowColor::Ice: return {0.80f, 0.92f, 1.0f, 1.0f};
    case SnowColor::Rainbow: return HsvToRgb(f.hue, 0.42f, 1.0f);
    case SnowColor::Custom:
        return {GetRValue(params_.customColor) / 255.0f, GetGValue(params_.customColor) / 255.0f,
                GetBValue(params_.customColor) / 255.0f, 1.0f};
    default: return {1.0f, 1.0f, 1.0f, 1.0f};
    }
}

void Scene::ReleaseDeviceResources() {
    driftBitmap_.Reset();
    scenery_.ReleaseDeviceResources();
    batch_.Reset();
    dc3_.Reset();
    dcOwner_ = nullptr;
    driftUploadAll_ = true;
    for (Bucket& b : buckets_) b.page = nullptr;
}

Scene::Bucket& Scene::BucketFor(ID2D1Bitmap1* page) {
    for (Bucket& b : buckets_)
        if (b.page == page) return b;
    for (Bucket& b : buckets_)
        if (b.page == nullptr || b.dst.empty()) {
            b.page = page;
            return b;
        }
    Bucket& b = buckets_.emplace_back();
    b.page = page;
    return b;
}

void Scene::AddSprite(const FlakeAtlas& atlas, int sprite, float x, float y, float diameter, float angle,
                      D2D1_COLOR_F color, float squash) {
    if (color.a <= 0.004f || diameter <= 0.5f) return;
    const FlakeAtlas::Sprite& sp = atlas.Get(sprite, diameter);
    if (!sp.bitmap) return;
    Bucket& bucket = BucketFor(sp.bitmap);
    float half = diameter * sp.extent * 0.5f;
    bucket.dst.push_back({-half, -half, half, half});
    bucket.src.push_back(sp.src);
    bucket.col.push_back(color);
    bucket.xf.push_back(SpriteTransform(angle, squash, x, y));
}

// Queues scenerySprites_ (all on the scenery's page, so they keep their order).
void Scene::AddScenerySprites() {
    if (scenerySprites_.empty()) return;
    Bucket& bucket = BucketFor(scenery_.Page());
    for (const ScenerySprite& t : scenerySprites_) {
        if (t.color.a <= 0.004f) continue;
        bucket.dst.push_back(t.dst);
        bucket.src.push_back(t.src);
        bucket.col.push_back(t.color);
        bucket.xf.push_back(t.xf);
    }
    scenerySprites_.clear();
}

void Scene::FlushSprites(ID2D1DeviceContext* dc) {
    if (dc3_ && batch_) {
        batch_->Clear();
        dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        UINT32 start = 0;
        for (Bucket& b : buckets_) {
            UINT32 n = static_cast<UINT32>(b.dst.size());
            if (n == 0) continue;
            if (SUCCEEDED(batch_->AddSprites(n, b.dst.data(), b.src.data(), b.col.data(), b.xf.data()))) {
                dc3_->DrawSpriteBatch(batch_.Get(), start, n, b.page, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                                      D2D1_SPRITE_OPTIONS_NONE);
                start += n;
            }
        }
        dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    } else {
        // Fallback for systems without sprite batches: one DrawBitmap per flake
        // (no colour tint, opacity only).
        D2D1_MATRIX_3X2_F old;
        dc->GetTransform(&old);
        for (Bucket& b : buckets_) {
            for (size_t i = 0; i < b.dst.size(); ++i) {
                dc->SetTransform(b.xf[i]);
                const D2D1_RECT_U& u = b.src[i];
                D2D1_RECT_F src = {static_cast<float>(u.left), static_cast<float>(u.top),
                                   static_cast<float>(u.right), static_cast<float>(u.bottom)};
                dc->DrawBitmap(b.page, b.dst[i], b.col[i].a, D2D1_INTERPOLATION_MODE_LINEAR, &src);
            }
        }
        dc->SetTransform(old);
    }
    for (Bucket& b : buckets_) {
        b.dst.clear();
        b.src.clear();
        b.col.clear();
        b.xf.clear();
    }
}

void Scene::DrawDrift(ID2D1DeviceContext* dc) {
    int w = drift_.TexWidth(), h = drift_.TexHeight();
    if (w <= 0 || h <= 0) return;
    // Columns re-shaded this frame, as a few disjoint spans: upload only those.
    constexpr int kMaxSpans = 8;
    int x0[kMaxSpans], x1[kMaxSpans];
    int spans = drift_.Shade(x0, x1, kMaxSpans);
    if (driftBitmap_) {
        D2D1_SIZE_U size = driftBitmap_->GetPixelSize();
        if (size.width != static_cast<UINT32>(w) || size.height != static_cast<UINT32>(h)) driftBitmap_.Reset();
    }
    if (!driftBitmap_) {
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(h)), nullptr, 0, props,
                                    &driftBitmap_)))
            return;
        driftUploadAll_ = true;
    }
    if (driftUploadAll_) {
        x0[0] = 0;
        x1[0] = w;
        spans = 1;
        driftUploadAll_ = false;
    }
    for (int i = 0; i < spans; ++i) {
        if (x1[i] <= x0[i]) continue;
        D2D1_RECT_U r = {static_cast<UINT32>(x0[i]), 0, static_cast<UINT32>(x1[i]), static_cast<UINT32>(h)};
        driftBitmap_->CopyFromMemory(&r, drift_.Pixels() + x0[i], static_cast<UINT32>(w) * 4u);
    }
    if (drift_.Empty() && spans == 0) return;
    float top = geo_.groundY - static_cast<float>(drift_.GroundRow());
    dc->DrawBitmap(driftBitmap_.Get(), D2D1::RectF(0, top, static_cast<float>(w), top + static_cast<float>(h)), 1.0f,
                   D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
}

void Scene::Draw(ID2D1DeviceContext* dc, const FlakeAtlas& atlas) {
    if (dc != dcOwner_) {
        ReleaseDeviceResources();
        dcOwner_ = dc;
        if (SUCCEEDED(dc->QueryInterface(IID_PPV_ARGS(&dc3_)))) {
            if (FAILED(dc3_->CreateSpriteBatch(&batch_))) dc3_.Reset();
        }
    }
    if (!atlas.Ready()) return;

    const bool driftOn = drift_.Enabled();
    const float W = static_cast<float>(geo_.width);
    const bool props = scenery_.Prepare(dc);
    // A tumbling crystal narrows as it turns edge-on (never below 60%).
    auto squash = [](const Flake& f) {
        return f.tumbleSpeed != 0.0f ? 0.6f + 0.4f * std::fabs(std::cos(f.tumble)) : 1.0f;
    };
    auto falling = [&](const Flake& f) {
        float a = f.alpha * f.born;
        if (GetFlakeTraits(f.kind).twinkles) a *= 0.6f + 0.4f * std::sin(f.twinkle);
        AddSprite(atlas, f.sprite, f.x, f.y, f.diameter, f.angle, WithAlpha(Tint(f), a), squash(f));
    };
    // Falling flakes nearer than the distant ones pass in front of the props,
    // and with accumulation land on the drift: they come later.
    const bool nearLater = driftOn || props;

    // 1. Distant flakes (and, with nothing to pass in front of, every flake)
    //    pass behind the ground line: clip them to the area above it.
    for (const Flake& f : flakes_)
        if (!f.landed && (f.background || !nearLater)) falling(f);
    dc->PushAxisAlignedClip(D2D1::RectF(0, 0, W, geo_.groundY), D2D1_ANTIALIAS_MODE_ALIASED);
    FlushSprites(dc);
    dc->PopAxisAlignedClip();

    // 2. Trees and snowmen, in front of the distant flakes; the drift buries their feet.
    if (props) {
        scenery_.EmitProps(scenerySprites_);
        AddScenerySprites();
        dc->PushAxisAlignedClip(D2D1::RectF(0, 0, W, geo_.groundY), D2D1_ANTIALIAS_MODE_ALIASED);
        FlushSprites(dc);
        dc->PopAxisAlignedClip();
    }

    // 3. The drift, and the snow heaped round the props' feet.
    DrawDrift(dc);
    if (props) {
        scenery_.EmitGround(scenerySprites_);
        AddScenerySprites();
        dc->PushAxisAlignedClip(D2D1::RectF(0, 0, W, geo_.groundY), D2D1_ANTIALIAS_MODE_ALIASED);
        FlushSprites(dc);
        dc->PopAxisAlignedClip();
    }

    // Without accumulation the nearer flakes pass in front of the props, and
    // still behind the ground line.
    if (props && !driftOn) {
        for (const Flake& f : flakes_)
            if (!f.landed && !f.background) falling(f);
        dc->PushAxisAlignedClip(D2D1::RectF(0, 0, W, geo_.groundY), D2D1_ANTIALIAS_MODE_ALIASED);
        FlushSprites(dc);
        dc->PopAxisAlignedClip();
    }

    // 4. Near flakes, landed flakes melting into the drift or on the boughs, and powder.
    if (driftOn || !particles_.empty() || props) {
        for (const Flake& f : flakes_) {
            if (!f.landed && (!driftOn || f.background)) continue;
            float a = f.alpha * f.born;
            float d = f.diameter;
            if (f.landed) {
                float t = Saturate(f.life / f.lifeMax);
                a *= t * t * (3.0f - 2.0f * t);
                d *= (0.8f + 0.2f * t) * (f.perch.kind >= 0 ? 0.6f : 1.0f);  // on a bough it sinks into the snow
            } else if (GetFlakeTraits(f.kind).twinkles) {
                a *= 0.6f + 0.4f * std::sin(f.twinkle);
            }
            // Landed crystals keep their (frozen) tumble rather than popping to full width.
            AddSprite(atlas, f.sprite, f.x, f.y, d, f.angle, WithAlpha(Tint(f), a), squash(f));
        }
        const int dot = FlakeAtlas::SpriteIndex(FlakeKind::Dot, 0);
        const int soft = FlakeAtlas::SpriteIndex(FlakeKind::Soft, 0);
        for (const Particle& p : particles_) {
            float t = Saturate(p.life / p.maxLife);
            if (p.puff) {
                AddSprite(atlas, soft, p.x, p.y, p.d * (1.9f - 0.9f * t), 0.0f, WithAlpha({1, 1, 1, 1}, 0.6f * t * t));
                continue;
            }
            float a = p.deposit ? 0.95f : 0.8f * t;
            AddSprite(atlas, dot, p.x, p.y, p.d, 0.0f, WithAlpha({1, 1, 1, 1}, a));
        }
    }

    // 5. The trees' lights glow over everything near them: needles, snow, flakes.
    if (props) {
        FlushSprites(dc);
        scenery_.EmitGlow(scenerySprites_);
        AddScenerySprites();
        dc->PushAxisAlignedClip(D2D1::RectF(0, 0, W, geo_.groundY), D2D1_ANTIALIAS_MODE_ALIASED);
        FlushSprites(dc);
        dc->PopAxisAlignedClip();
    }

    // 6. Glints twinkling on the drift surface.
    const int sparkle = FlakeAtlas::SpriteIndex(FlakeKind::Sparkle, 0);
    for (const Glint& g : glints_) {
        float t = 1.0f - g.life / g.maxLife;
        float env = std::sin(kPi * t);
        env *= env;
        float y = geo_.groundY - drift_.HeightAt(g.x) + 1.5f * geo_.dpiScale;
        AddSprite(atlas, sparkle, g.x, y, g.size * (0.6f + 0.4f * env), t * 0.6f, WithAlpha({1, 1, 1, 1}, env));
    }
    FlushSprites(dc);
}
