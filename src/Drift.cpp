#include "Drift.h"

#include <bit>

#include "Noise.h"

namespace {

// ---------------------------------------------------------------------------
// Tunables. Lengths and heights are DIPs and are scaled by dpiScale where
// used, so the drift behaves and looks the same at every display scale.
// ---------------------------------------------------------------------------

// Deposition: a landed flake adds volume ~ d^1.3 (sub-linear in its area, so a
// few big flakes don't dominate), mostly as a wide, low swell plus a small
// clump that keeps a little texture in the surface.
constexpr float kFlakeVolume = 1.0f;                   // DIP^2 from a 1-DIP flake
constexpr float kFlakeVolumeExp = 1.3f;
constexpr float kSpreadMin = 1.0f, kSpreadMax = 2.0f;  // swell half-width / flake diameter
constexpr float kClumpShare = 0.25f;                   // share of the snow left as a clump
constexpr float kClumpMin = 0.5f, kClumpMax = 0.9f;    // clump half-width / flake diameter
constexpr float kCapKnee = 0.6f;                       // fill level where sticking starts to fade
constexpr float kLeeReach = 36.0f;                     // a crest shelters spots this far downwind
constexpr float kLeeBonus = 0.6f;                      // extra catch in a crest's lee in a stiff breeze
constexpr float kShelterTan = 0.3f;                    // slope of the sheltered zone behind a crest

// Where snow gathers: broad swells plus sparse spots that build real drifts.
// The pattern drifts slowly. Simplex noise is not periodic, so rather than
// wrapping time (the pattern would jump) the sample point travels around a
// circle in noise space: seamless however long the app runs (a lap is ~4 days).
constexpr float kSpotCatch = 3.0f;
constexpr double kFieldSpeed = 0.0012;  // noise units per second
constexpr float kFieldOrbit = 64.0f;    // radius of that circle (noise units)
constexpr float kEndTaper = 140.0f;     // the pile thins out over this distance to an open end

// Cohesion: snow is sticky, so it doesn't pile into cones like sand. Crests
// sharper than a minimum radius slump and round off, sharp hollows fill in,
// and a faint creep softens everything. Flux-form diffusion whose rate rises
// with the local curvature: mass conserving, and stable with substeps.
constexpr float kCreepBase = 0.6f;   // DIP^2/s everywhere
constexpr float kCreepCrest = 8.0f;  // DIP^2/s more on crests sharper than the limit
constexpr float kCreepFoot = 3.0f;   // DIP^2/s more in hollows sharper than the limit
constexpr float kCrestCurv = 0.03f;  // 1/DIP: tightest crest curvature (varies along x)
constexpr float kFootCurv = 0.15f;   // 1/DIP
constexpr float kCreepCfl = 0.45f;   // stability bound of one explicit substep
constexpr float kDust = 1.0e-4f;     // px; thinner snow is dropped

// Avalanching: fixed-rate relaxation sweeps; each moves half of the excess
// slope between neighbours (thermal-erosion style, mass conserving).
constexpr float kSweepRate = 240.0f;  // sweeps per second
constexpr int kMaxSweeps = 32;
constexpr float kRelax = 0.5f;
constexpr float kReposeMinDeg = 38.0f, kReposeMaxDeg = 55.0f;

// Wind transport (saltation): the flux relaxes toward a capacity that is
// higher on windward slopes and crests and zero in a crest's lee.
constexpr float kWindTick = 1.0f / 30.0f;
constexpr int kMaxWindTicks = 4;
constexpr float kWindThreshold = 35.0f;  // DIP/s before snow starts to move
constexpr float kFluxScale = 0.006f;     // DIP^2/s per (DIP/s)^1.5 of excess wind
constexpr float kSaturationLen = 10.0f;  // how quickly the flux adapts to the surface
constexpr float kStossGain = 2.5f;
constexpr float kCrestGain = 1.0f;

// Melt / settling: exponential decay, faster on tall exposed peaks, plus a
// little sublimation so stray dust eventually disappears.
constexpr float kMeltTau = 220.0f;      // s
constexpr float kPeakMelt = 0.6f;
constexpr float kTallMelt = 0.8f;
constexpr float kSublimation = 0.003f;  // DIP/s
constexpr float kMeltPeriod = 0.5f;     // every melt block is visited this often (s)
constexpr float kOverCapTau = 3.0f;     // snow above its local cap settles over this time (s)
constexpr float kCapEaseTau = 0.8f;     // a lowered cap eases down with this time constant

// Rendering.
constexpr float kVisibleMin = 0.05f;  // px; thinner snow is not drawn
constexpr float kRedrawEps = 0.1f;    // px of change that triggers a re-shade
constexpr float kLipMax = 3.2f;       // deepest droop over the ground line
constexpr float kShadowLen = 5.0f;    // the cast shadow fades out over this
constexpr float kShadowAlpha = 0.28f;
constexpr float kAoRadius = 8.0f;     // neighbourhood for concavity / ambient occlusion
constexpr float kLipRadius = 3.0f;
constexpr float kPeakRadius = 12.0f;
constexpr int kTopMarginPx = 2;
constexpr float kLayoutQuantum = 8.0f;  // the strip's height above the ground grows in these steps
constexpr float kRound = 2.2f;        // how far below the surface its slope still shows
constexpr float kFrontLight = 0.92f;  // the drift's face, relative to its flat top

// Shade work per call, in blocks of Drift::kShadeBlock columns: clusters of
// dirty blocks (bridging clean gaps of up to kMaxGap blocks), round robin,
// until the budget is used. While the whole strip moves (melting away, a
// lowered cap easing down) it is swept left to right in as few equal passes
// of at most kAnimBlocks as possible, so at most one seam shows.
constexpr int kShadeBudget = 40;
constexpr int kMaxGap = 3;
constexpr int kAnimBlocks = 80;

struct Rgb {
    float r, g, b;
};
constexpr Rgb kWhite = {1.0f, 1.0f, 1.0f};
constexpr Rgb kShadeCol = {0.74f, 0.83f, 0.96f};  // fully shaded snow: cool blue
constexpr Rgb kUnder = {0.70f, 0.79f, 0.93f};     // underside of the overhanging lip
constexpr Rgb kShadow = {0.02f, 0.04f, 0.09f};

inline Rgb Mix(Rgb a, Rgb b, float t) { return {Lerp(a.r, b.r, t), Lerp(a.g, b.g, t), Lerp(a.b, b.b, t)}; }

// Illumination (1 = sunlit flat top) to snow colour: white fading to cool blue.
inline Rgb SnowColor(float light) { return Mix(kShadeCol, kWhite, Saturate((light - 0.6f) * 2.5f)); }

// Bit tests, so they keep working under /fp:fast (which may fold NaN compares).
inline bool IsNan(float v) { return (std::bit_cast<uint32_t>(v) & 0x7FFFFFFFu) > 0x7F800000u; }
inline bool IsFinite(float v) { return (std::bit_cast<uint32_t>(v) & 0x7F800000u) != 0x7F800000u; }

// Coverage of a pixel row by a surface crossing it linearly from height a to b
// (both relative to the row's bottom): integral over [0,1] of clamp(h, 0, 1).
inline float Ramp(float u) { return u <= 0.0f ? 0.0f : (u >= 1.0f ? u - 0.5f : 0.5f * u * u); }
inline float EdgeCoverage(float a, float b) {
    float d = b - a;
    if (std::fabs(d) < 1e-3f) return Saturate(0.5f * (a + b));
    return (Ramp(b) - Ramp(a)) / d;
}

inline uint32_t PackPremul(Rgb c, float a) {
    a = Saturate(a);
    float s = 255.0f * a;
    auto ch = [s](float v) { return static_cast<uint32_t>(Saturate(v) * s + 0.5f); };
    return (static_cast<uint32_t>(s + 0.5f) << 24) | (ch(c.r) << 16) | (ch(c.g) << 8) | ch(c.b);
}

int RoundPx(float v) { return std::max(1, static_cast<int>(v + 0.5f)); }

}  // namespace

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void Drift::Configure(int widthPx, const DriftParams& params) {
    if (!seeded_) {
        // Each drift gets its own pattern so that monitors don't match.
        uint64_t t = static_cast<uint64_t>(QpcSeconds() * 1.0e7);
        rng_.Seed(t ^ static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this)));
        seed_ = rng_.Range(0.0f, 200.0f);
        seeded_ = true;
    }
    widthPx = std::max(widthPx, 0);
    if (widthPx != width_) Resize(widthPx);
    SetParams(params);
}

void Drift::Resize(int widthPx) {
    const int oldW = width_;
    std::vector<float> old;
    old.swap(height_);
    width_ = widthPx;
    const size_t n = static_cast<size_t>(widthPx);
    height_.assign(n, 0.0f);
    if (oldW > 0 && widthPx > 0 && old.size() == static_cast<size_t>(oldW)) {
        // Keep the snow, stretched to the new width.
        float k = static_cast<float>(oldW) / static_cast<float>(widthPx);
        for (int i = 0; i < widthPx; ++i) {
            float src = Clamp((static_cast<float>(i) + 0.5f) * k - 0.5f, 0.0f, static_cast<float>(oldW - 1));
            int j = static_cast<int>(src);
            int j1 = std::min(j + 1, oldW - 1);
            height_[static_cast<size_t>(i)] = Lerp(old[static_cast<size_t>(j)], old[static_cast<size_t>(j1)],
                                                   src - static_cast<float>(j));
        }
    }
    smooth_.assign(n, 0.0f);
    scratch_.assign(n + 1, 0.0f);
    work_.assign(n + 1, 0.0f);
    talus_.assign(n, 1.0f);
    crestInv_.assign(n, 1.0f);
    capF_.assign(n, 1.0f);
    catch_.assign(n, 1.0f);
    rawCatch_.assign(n, 0.0f);
    rawSpot_.assign(n, 0.0f);
    rawCap_.assign(n, 0.0f);
    lipF_.assign(n, 0.0f);
    prefix_.assign(n + 1, 0.0);
    shadedS_.assign(n, -1.0f);
    colS_.assign(n + 2, 0.0f);
    colLip_.assign(n + 2, 0.0f);
    colWide_.assign(n + 2, 0.0f);
    blockDirty_.assign(static_cast<size_t>((widthPx + kShadeBlock - 1) / kShadeBlock), 0);
    meltAge_.assign(static_cast<size_t>((widthPx + kMeltBlock - 1) / kMeltBlock), 0.0f);
    spans_.clear();
    spans_.reserve(blockDirty_.size());
    dirtyBlocks_ = 0;
    shadeCursor_ = meltCursor_ = fieldCursor_ = 0;
    fieldsValid_ = false;
    chg0_ = 0;
    chg1_ = widthPx;
    av0_ = 0;
    av1_ = widthPx;
    maxH_ = 0.0f;
    for (float h : height_) maxH_ = std::max(maxH_, h);
    // Force a fresh layout (the pixel buffer must match the new width).
    texHeight_ = groundRow_ = 0;
    pixels_.clear();
    MarkAllDirty();
}

void Drift::SetParams(const DriftParams& in) {
    DriftParams p = in;
    if (IsNan(p.maxHeight) || p.maxHeight <= 0.0f) p.maxHeight = 0.0f;
    p.maxHeight = std::min(p.maxHeight, 4096.0f);
    if (IsNan(p.dpiScale) || p.dpiScale <= 0.25f) p.dpiScale = 1.0f;
    p.dpiScale = std::min(p.dpiScale, 8.0f);

    const bool fieldsChanged = !fieldsValid_ || p.dpiScale != params_.dpiScale ||
                               p.leftOpen != params_.leftOpen || p.rightOpen != params_.rightOpen;
    const float oldMax = params_.maxHeight;
    params_ = p;

    if (fieldsChanged && width_ > 0) {
        ComputeStaticFields();
        RefreshFields(0, width_);
        FieldStatistics();
        MapFields(0, width_);
        NormalizeCatch();
        fieldsValid_ = true;
        av0_ = 0;  // repose angles changed: re-check every slope
        av1_ = width_;
        chg0_ = 0;
        chg1_ = width_;
        MarkAllDirty();
    }
    if (p.maxHeight != oldMax || capHeight_ <= 0.0f) {
        if (p.maxHeight <= 0.0f) {
            // Accumulation switched off: let what is there melt away gently.
            if (!Empty() && meltLeft_ <= 0.0f) MeltAway(3.0f);
        } else if (p.maxHeight >= capHeight_) {
            capHeight_ = p.maxHeight;  // a lower cap eases down in Update
        }
    }
    UpdateLayout();
}

// Static per-column fields: repose angle, crest roundness and the shape of
// the overhanging lip.
void Drift::ComputeStaticFields() {
    const float s = params_.dpiScale;
    const float inv = 1.0f / s;
    const float wDip = static_cast<float>(width_) * inv;
    for (int i = 0; i < width_; ++i) {
        const size_t u = static_cast<size_t>(i);
        float xd = (static_cast<float>(i) + 0.5f) * inv;
        float taper = EndTaper(xd, wDip);
        // Some stretches are clumpy, holding steep faces and tighter crests;
        // others slump into soft, round shapes.
        float n = 0.6f * noise::Noise1(xd * (1.0f / 55.0f) + seed_ * 0.71f + 91.1f) +
                  0.4f * noise::Noise1(xd * (1.0f / 19.0f) + seed_ * 0.53f + 7.7f);
        float clump = SmoothStep(-0.4f, 0.4f, n);
        float deg = Lerp(kReposeMinDeg, kReposeMaxDeg, clump);
        deg -= 12.0f * (1.0f - taper);  // looser near an open end
        talus_[u] = std::tan(deg * (kPi / 180.0f));
        crestInv_[u] = s / (kCrestCurv * Lerp(0.6f, 1.5f, clump));
        // Rounded, irregular scallops hanging over the edge: drips of mixed
        // size, bunched in some stretches and sparse in others.
        float a = noise::Noise1(xd * (1.0f / 9.0f) + seed_ * 0.37f + 51.3f);
        float b = noise::Noise1(xd * (1.0f / 3.7f) + seed_ * 0.29f + 13.9f);
        float c = noise::Noise1(xd * (1.0f / 31.0f) + seed_ * 0.61f + 77.7f);
        float v = 0.5f + 0.9f * (0.6f * a + 0.25f * b) + 0.45f * c;
        lipF_[u] = 0.1f + 0.9f * SmoothStep(0.15f, 0.95f, v);
    }
}

float Drift::EndTaper(float xDip, float widthDip) const {
    float t = 1.0f;
    if (params_.leftOpen) t = std::min(t, SmoothStep(0.0f, kEndTaper, xDip));
    if (params_.rightOpen) t = std::min(t, SmoothStep(0.0f, kEndTaper, widthDip - xDip));
    return t;
}

// Slowly drifting fields: how much snow each stretch catches, and how high it
// may pile there. Raw noise is evaluated in DIP space, then mapped through the
// strip's own statistics so that every screen (wide or narrow, whatever the
// noise happens to do there) gets a similar mix of thin and deep stretches.
void Drift::RefreshFields(int x0, int x1) {
    const float inv = 1.0f / params_.dpiScale;
    // Where on its circle the sample point is now (see kFieldOrbit). A field
    // that changes m times as fast rides a circle m times as large.
    const double ang = time_ * (kFieldSpeed / static_cast<double>(kFieldOrbit));
    const float oc = kFieldOrbit * static_cast<float>(std::cos(ang));
    const float os = kFieldOrbit * static_cast<float>(std::sin(ang));
    auto field = [oc, os](float x, float m, float z) { return noise::Simplex3(x, m * oc, z + m * os); };
    x0 = std::max(x0, 0);
    x1 = std::min(x1, width_);
    for (int i = x0; i < x1; ++i) {
        const size_t u = static_cast<size_t>(i);
        float xd = (static_cast<float>(i) + 0.5f) * inv;
        float broad = field(xd * (1.0f / 560.0f) + seed_, 1.0f, 3.7f);
        float mid = field(xd * (1.0f / 150.0f) + seed_ * 1.37f, 1.3f, 9.1f);
        rawCatch_[u] = 0.6f * broad + 0.4f * mid;
        rawSpot_[u] = field(xd * (1.0f / 70.0f) + seed_ * 0.77f, 1.7f, 15.3f);
        rawCap_[u] = field(xd * (1.0f / 330.0f) + seed_ + 11.3f, 0.7f, 8.1f) +
                     0.4f * field(xd * (1.0f / 110.0f) + seed_ * 0.59f + 3.3f, 0.9f, 21.7f);
    }
    MapFields(x0, x1);
}

void Drift::MapFields(int x0, int x1) {
    const float inv = 1.0f / params_.dpiScale;
    const float wDip = static_cast<float>(width_) * inv;
    for (int i = x0; i < x1; ++i) {
        const size_t u = static_cast<size_t>(i);
        float taper = EndTaper((static_cast<float>(i) + 0.5f) * inv, wDip);
        float c = SmoothStep(-1.2f, 1.5f, (rawCatch_[u] - fieldStats_[0]) / fieldStats_[1]);
        float spot = SmoothStep(0.8f, 1.8f, (rawSpot_[u] - fieldStats_[2]) / fieldStats_[3]);
        catch_[u] = (0.1f + 1.6f * c * c + kSpotCatch * spot * (0.3f + 0.7f * c)) * (0.35f + 0.65f * taper);
        // The cap follows the noise linearly (no saturation), so even a pile
        // filled to it has rounded, uneven tops rather than flat mesas.
        float m = Saturate(0.55f + 0.3f * (rawCap_[u] - fieldStats_[4]) / fieldStats_[5]);
        capF_[u] = (0.45f + 0.55f * m) * (0.15f + 0.85f * taper);
    }
}

// Mean and spread of each raw field over the strip.
void Drift::FieldStatistics() {
    if (width_ <= 0) return;
    const std::vector<float>* fields[3] = {&rawCatch_, &rawSpot_, &rawCap_};
    const double n = static_cast<double>(width_);
    for (int f = 0; f < 3; ++f) {
        double sum = 0, sq = 0;
        for (float v : *fields[f]) {
            sum += v;
            sq += static_cast<double>(v) * v;
        }
        double mean = sum / n;
        // A floor on the spread keeps narrow strips from amplifying tiny wiggles.
        fieldStats_[2 * f] = static_cast<float>(mean);
        fieldStats_[2 * f + 1] = std::max(static_cast<float>(std::sqrt(std::max(sq / n - mean * mean, 0.0))), 0.12f);
    }
}

// The factor that makes the catch average 1, so the total snowfall doesn't
// depend on the pattern.
void Drift::NormalizeCatch() {
    double sum = 0;
    for (float c : catch_) sum += c;
    catchScale_ = sum > 0.0 ? static_cast<float>(static_cast<double>(width_) / sum) : 1.0f;
}

void Drift::UpdateLayout() {
    int ground = 0, tex = 0;
    if (width_ > 0 && (Enabled() || !Empty())) {
        const float s = params_.dpiScale;
        // The rows above the ground come in quanta, so dragging the height
        // slider only now and then reallocates the strip (and with it the
        // renderer's bitmap): grow to the next quantum, and shrink only once
        // more than a quantum beyond that is spare.
        const int q = RoundPx(kLayoutQuantum * s);
        const int need = static_cast<int>(std::ceil(std::max(params_.maxHeight, capHeight_))) + kTopMarginPx;
        const int spare = groundRow_ - need;
        ground = spare >= 0 && spare < 2 * q ? groundRow_ : (need + q - 1) / q * q;
        tex = ground + static_cast<int>(std::ceil((kLipMax + kShadowLen) * s)) + 2;
    }
    if (ground == groundRow_ && tex == texHeight_) return;
    // While a lowered cap eases down or the pile melts away, keep the current
    // strip as long as everything still fits in it, so it is not reallocated
    // every frame along the way. Growth is never deferred.
    const bool fits = ground <= groundRow_ && tex - ground <= texHeight_ - groundRow_;
    if (fits && (meltLeft_ > 0.0f || (Enabled() && capHeight_ > params_.maxHeight))) return;
    groundRow_ = ground;
    texHeight_ = tex;
    pixels_.assign(static_cast<size_t>(width_) * static_cast<size_t>(tex), 0u);
    MarkAllDirty();
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

float Drift::HeightAt(float x) const {
    if (width_ <= 0) return 0.0f;
    float f = x - 0.5f;  // column i is centred on x = i + 0.5
    if (IsNan(f) || f <= 0.0f) return smooth_[0];
    if (f >= static_cast<float>(width_ - 1)) return smooth_[static_cast<size_t>(width_ - 1)];
    int i = static_cast<int>(f);
    float t = f - static_cast<float>(i);
    float a = smooth_[static_cast<size_t>(i)], b = smooth_[static_cast<size_t>(i) + 1];
    return a + (b - a) * t;
}

bool Drift::Empty() const { return maxH_ < kVisibleMin; }

bool Drift::RandomSurfacePoint(Rng& rng, float& x, float& y) const {
    if (width_ <= 0 || Empty()) return false;
    const float minH = 0.6f * params_.dpiScale;
    for (int tries = 0; tries < 24; ++tries) {
        int i = rng.Int(0, width_ - 1);
        if (smooth_[static_cast<size_t>(i)] > minH) {
            x = static_cast<float>(i) + rng.Float();
            y = HeightAt(x);
            return true;
        }
    }
    return false;
}

float Drift::WideAverage(int i, int radius) const {
    int a = std::max(i - radius, 0);
    int b = std::min(i + radius + 1, width_);
    if (b <= a) return 0.0f;
    return static_cast<float>((prefix_[static_cast<size_t>(b)] - prefix_[static_cast<size_t>(a)]) /
                              static_cast<double>(b - a));
}

// ---------------------------------------------------------------------------
// Changes
// ---------------------------------------------------------------------------

void Drift::Touch(int x0, int x1, bool steep) {
    x0 = std::max(x0, 0);
    x1 = std::min(x1, width_);
    if (x1 <= x0) return;
    chg0_ = std::min(chg0_, x0);
    chg1_ = std::max(chg1_, x1);
    if (steep) {
        av0_ = std::min(av0_, x0);
        av1_ = std::max(av1_, x1);
    }
}

void Drift::MarkDirty(int x0, int x1) {
    x0 = std::max(x0, 0);
    x1 = std::min(x1, width_);
    if (x1 <= x0) return;
    for (int b = x0 / kShadeBlock; b <= (x1 - 1) / kShadeBlock; ++b) {
        uint8_t& d = blockDirty_[static_cast<size_t>(b)];
        if (!d) {
            d = 1;
            ++dirtyBlocks_;
        }
    }
}

void Drift::MarkAllDirty() { fullDirty_ = true; }

void Drift::AddBump(float cx, float r, float volume) {
    if (!(volume > 0.0f) || width_ <= 0) return;
    r = std::max(r, 0.75f);
    int i0 = std::max(0, static_cast<int>(std::floor(cx - r - 0.5f)));
    int i1 = std::min(width_ - 1, static_cast<int>(std::ceil(cx + r - 0.5f)));
    if (i0 > i1) return;
    const float inv = 1.0f / r;
    float sum = 0.0f;
    for (int i = i0; i <= i1; ++i) {
        float u = (static_cast<float>(i) + 0.5f - cx) * inv;
        float w = 1.0f - u * u;
        w = w > 0.0f ? w * w : 0.0f;  // biweight: smooth, compact
        scratch_[static_cast<size_t>(i - i0)] = w;
        sum += w;
    }
    if (sum < 1e-4f) {
        int i = Clamp(static_cast<int>(cx), 0, width_ - 1);
        float& h = height_[static_cast<size_t>(i)];
        h = std::min(h + volume, capHeight_);
        maxH_ = std::max(maxH_, h);
        Touch(i, i + 1);
        return;
    }
    const float k = volume / sum;
    float top = 0.0f;
    for (int i = i0; i <= i1; ++i) {
        float& h = height_[static_cast<size_t>(i)];
        h = std::min(h + scratch_[static_cast<size_t>(i - i0)] * k, capHeight_);
        top = std::max(top, h);
    }
    maxH_ = std::max(maxH_, top);
    Touch(i0, i1 + 1);
}

// Extra catch in the lee of a crest: how far the surface upwind rises above a
// shallow line drawn back from this spot.
float Drift::LeeFactor(int i) const {
    float w = Saturate((std::fabs(windDip_) - 8.0f) / 70.0f);
    if (w <= 0.0f) return 1.0f;
    const float s = params_.dpiScale;
    const int dir = windDip_ > 0.0f ? -1 : 1;  // look upwind
    const int reach = static_cast<int>(kLeeReach * s);
    const float h0 = smooth_[static_cast<size_t>(i)];
    float best = 0.0f;
    for (int k = 1; k <= reach; ++k) {
        int j = i + dir * k;
        if (j < 0 || j >= width_) break;
        best = std::max(best, smooth_[static_cast<size_t>(j)] - kShelterTan * static_cast<float>(k) - h0);
    }
    return 1.0f + kLeeBonus * w * SmoothStep(0.0f, 3.0f * s, best);
}

void Drift::Deposit(float x, float diameterPx, float amount) {
    if (!Enabled() || width_ <= 0 || IsNan(x) || IsNan(diameterPx) || IsNan(amount)) return;
    if (x < 0.0f || x >= static_cast<float>(width_) || diameterPx <= 0.0f || amount <= 0.0f) return;
    const float s = params_.dpiScale;
    const float dDip = Clamp(diameterPx / s, 0.5f, 64.0f);
    amount = std::min(amount, 64.0f);
    const int i = Clamp(static_cast<int>(x), 0, width_ - 1);
    const size_t u = static_cast<size_t>(i);

    // Stretches that gather more snow get wider as well as taller swells, so
    // the catch shapes broad drifts rather than spikes.
    const float c = catch_[u] * catchScale_ * LeeFactor(i);
    const float vol = kFlakeVolume * std::pow(dDip, kFlakeVolumeExp) * amount * s * s * c * rng_.Range(0.7f, 1.3f);
    const float d = dDip * s;
    const float rWide = d * rng_.Range(kSpreadMin, kSpreadMax) * Clamp(std::sqrt(c), 0.7f, 1.4f) + 1.5f * s;
    const float rClump = std::max(s, d * rng_.Range(kClumpMin, kClumpMax));
    const float cx = x + rng_.Range(-0.15f, 0.15f) * d;

    // Soft cap: near the local cap less of the snow sticks; the rest is loose
    // and slides or blows on to the roomiest spot nearby.
    const float fill = height_[u] / std::max(LocalCap(i), 0.5f);
    const float keep = 1.0f - SmoothStep(kCapKnee, 1.0f, fill);
    AddBump(cx, rWide, vol * keep * (1.0f - kClumpShare));
    AddBump(cx + rng_.Range(-0.3f, 0.3f) * d, rClump, vol * keep * kClumpShare);
    const float loose = vol * (1.0f - keep);
    if (loose < 1e-3f) return;
    float dir = windDip_ > 8.0f ? 1.0f : (windDip_ < -8.0f ? -1.0f : (rng_.Chance(0.5f) ? 1.0f : -1.0f));
    float bestX = -1.0f, bestRoom = 0.0f;
    for (int k = 0; k < 4; ++k) {
        float side = k < 3 ? dir : -dir;  // mostly downwind
        float xx = cx + side * rng_.Range(1.0f, 4.0f) * rWide;
        if (!(xx >= 0.0f && xx < static_cast<float>(width_))) continue;
        int j = static_cast<int>(xx);
        float room = 0.9f * LocalCap(j) - height_[static_cast<size_t>(j)];
        if (room > bestRoom) {
            bestRoom = room;
            bestX = xx;
        }
    }
    if (bestX >= 0.0f) AddBump(bestX, rWide * 1.2f, loose * SmoothStep(0.0f, 2.0f * s, bestRoom));
}

float Drift::Plow(float x0, float x1, float ceiling) {
    if (width_ <= 0 || !IsFinite(x0) || !IsFinite(x1) || IsNan(ceiling) || x1 <= x0) return 0.0f;
    ceiling = std::max(ceiling, 0.0f);
    const float cx = 0.5f * x0 + 0.5f * x1;  // halves first: no overflow
    const float r = 0.5f * x1 - 0.5f * x0;
    // Clamp in float before converting, so huge inputs stay defined.
    const float w = static_cast<float>(width_);
    const int i0 = static_cast<int>(std::floor(Clamp(x0, 0.0f, w)));
    const int i1 = static_cast<int>(std::ceil(Clamp(x1, 0.0f, w)));
    float removed = 0.0f;
    for (int i = i0; i < i1; ++i) {
        // A rounded scoop: the cut is deepest under the middle of the range.
        float dx = std::fabs(static_cast<float>(i) + 0.5f - cx) / std::max(r, 0.5f);
        float lift = dx < 1.0f ? r * (1.0f - std::sqrt(1.0f - dx * dx)) * 0.5f : r;
        float c = ceiling + lift;
        float& h = height_[static_cast<size_t>(i)];
        if (h > c) {
            removed += h - c;
            h = c;
        }
    }
    if (removed > 0.0f) {
        Touch(i0, i1);
        Derive();  // so HeightAt sees the cut right away
    }
    return removed;
}

void Drift::Clear() {
    std::fill(height_.begin(), height_.end(), 0.0f);
    std::fill(smooth_.begin(), smooth_.end(), 0.0f);
    std::fill(prefix_.begin(), prefix_.end(), 0.0);
    maxH_ = 0.0f;
    meltRate_ = meltLeft_ = 0.0f;
    chg0_ = av0_ = width_;
    chg1_ = av1_ = 0;
    MarkAllDirty();
    UpdateLayout();
}

void Drift::MeltAway(float seconds) {
    if (width_ <= 0 || Empty()) return;
    if (IsNan(seconds) || seconds < 0.05f) seconds = 0.05f;
    seconds = std::min(seconds, 600.0f);
    float top = 0.0f;
    for (float h : height_) top = std::max(top, h);
    meltRate_ = top / seconds;
    meltLeft_ = seconds;
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------

void Drift::Update(float dt, float wind) {
    if (width_ <= 0 || IsNan(dt) || dt <= 0.0f) return;
    dt = std::min(dt, 0.1f);
    if (IsNan(wind) || std::fabs(wind) > 1.0e5f) wind = 0.0f;
    const float s = params_.dpiScale;
    time_ += dt;
    windDip_ = wind / s;
    if (unshadedUpdates_ < 1000) ++unshadedUpdates_;

    if (meltLeft_ > 0.0f) {
        // Animated melt: everything sinks at the same pace and is gone on time.
        float m = meltRate_ * std::min(dt, meltLeft_);
        meltLeft_ -= dt;
        const bool done = meltLeft_ <= 0.0f;
        for (float& h : height_) h = done ? 0.0f : std::max(0.0f, h - m);
        if (done) meltRate_ = meltLeft_ = 0.0f;
        Touch(0, width_, false);
    } else if (Enabled() && capHeight_ > params_.maxHeight) {
        // The cap was lowered: ease it down and squeeze the snow above a knee
        // in proportion, so drifts keep their rounded shapes while they sink.
        const float oldCap = capHeight_;
        capHeight_ = Lerp(capHeight_, params_.maxHeight, ApproachFactor(dt, kCapEaseTau));
        if (capHeight_ - params_.maxHeight < 0.05f) capHeight_ = params_.maxHeight;
        const float knee = 0.6f * params_.maxHeight;
        const float r = (capHeight_ - knee) / std::max(oldCap - knee, 1e-3f);
        int lo = width_, hi = -1;
        for (int i = 0; i < width_; ++i) {
            float& h = height_[static_cast<size_t>(i)];
            if (h > knee) {
                h = std::min(knee + (h - knee) * r, capHeight_);
                lo = std::min(lo, i);
                hi = i;
            }
        }
        Touch(lo, hi + 1, false);
    }

    // Refresh a slice of the slowly drifting fields every frame.
    if (fieldsValid_) {
        int chunk = std::max(64, width_ / 64);
        RefreshFields(fieldCursor_, fieldCursor_ + chunk);
        fieldCursor_ += chunk;
        if (fieldCursor_ >= width_) {
            fieldCursor_ = 0;
            FieldStatistics();
            NormalizeCatch();
        }
    }

    windDebt_ += dt;
    for (int n = 0; windDebt_ >= kWindTick; ++n) {
        if (n == kMaxWindTicks) {
            windDebt_ = 0.0f;
            break;
        }
        windDebt_ -= kWindTick;
        WindStep(kWindTick, wind);
    }

    if (meltLeft_ <= 0.0f) MeltBlocks(dt);

    sweepDebt_ += dt * kSweepRate;
    int sweeps = static_cast<int>(sweepDebt_);
    sweepDebt_ -= static_cast<float>(sweeps);
    Relax(std::min(sweeps, kMaxSweeps));

    Creep(dt);

    Derive();
    UpdateLayout();
}

// Angle of repose: alternate-direction Gauss-Seidel sweeps over the columns
// that may be unstable. Each over-steep pair trades half of its excess
// (thermal-erosion style), so the total is conserved; open ends shed snow.
void Drift::Relax(int sweeps) {
    const int W = width_;
    if (W < 2) return;
    float* h = height_.data();
    const float* T = talus_.data();
    int touched0 = W, touched1 = -1;
    for (int n = 0; n < sweeps && av1_ > av0_; ++n) {
        const int a = std::max(av0_ - 1, 0), b = std::min(av1_ + 1, W);
        int lo = W, hi = -1;
        auto pair = [&](int i) {
            float d = h[i] - h[i + 1];
            float t = 0.5f * (T[i] + T[i + 1]);
            float m;
            if (d > t) {
                m = kRelax * 0.5f * (d - t);
                h[i] -= m;
                h[i + 1] += m;
            } else if (-d > t) {
                m = kRelax * 0.5f * (-d - t);
                h[i] += m;
                h[i + 1] -= m;
            } else {
                return false;
            }
            if (m > 1e-5f) {
                lo = std::min(lo, i);
                hi = std::max(hi, i);
                return true;
            }
            return false;
        };
        if ((sweepParity_++ & 1u) == 0) {
            int last = -2;
            for (int i = a; i < W - 1; ++i) {
                if (i >= b - 1 && last != i - 1) break;  // past the range: only follow a running cascade
                if (pair(i)) last = i;
            }
        } else {
            int last = W + 2;
            for (int i = b - 2; i >= 0; --i) {
                if (i < a && last != i + 1) break;
                if (pair(i)) last = i;
            }
        }
        // Open ends: snow steeper than repose against the drop slides off.
        if (params_.leftOpen && a == 0 && h[0] > T[0]) {
            h[0] -= kRelax * (h[0] - T[0]);
            lo = std::min(lo, 0);
            hi = std::max(hi, 0);
        }
        if (params_.rightOpen && b == W && h[W - 1] > T[W - 1]) {
            h[W - 1] -= kRelax * (h[W - 1] - T[W - 1]);
            lo = std::min(lo, W - 2);
            hi = std::max(hi, W - 2);
        }
        if (hi < lo) {
            av0_ = W;
            av1_ = 0;
        } else {
            av0_ = lo - 1;
            av1_ = hi + 3;
            touched0 = std::min(touched0, lo);
            touched1 = std::max(touched1, hi + 2);
        }
    }
    if (touched1 > touched0) {
        chg0_ = std::min(chg0_, std::max(touched0, 0));
        chg1_ = std::max(chg1_, std::min(touched1, W));
    }
}

// Cohesive creep over the snowy span. The flux between neighbours is
// D * (h[i-1] - h[i]); D rises where either column's curvature is sharper than
// the crest (convex) or foot (concave) limit, so peaks round off to a minimum
// radius and kinks soften while broad shapes and straight faces stay put.
// Explicit substeps keep D * dt <= kCreepCfl px^2, which is monotone: no new
// extrema, no negative heights, no overshoot of the cap.
void Drift::Creep(float dt) {
    const int W = width_;
    if (W < 3 || maxH_ <= 0.0f) return;
    float* z = height_.data();
    int first = 0, last = W - 1;
    while (first < W && z[first] <= 0.0f) ++first;
    if (first >= W) return;
    while (z[last] <= 0.0f) --last;

    const float s = params_.dpiScale;
    const float s2 = s * s;
    const float dMax = (kCreepBase + kCreepCrest + kCreepFoot) * s2;  // px^2/s
    const int steps = std::max(1, static_cast<int>(std::ceil(dt * dMax / kCreepCfl)));
    const float step = dt / static_cast<float>(steps);
    const float gBase = kCreepBase * s2 * step, gCrest = kCreepCrest * s2 * step, gFoot = kCreepFoot * s2 * step;
    const float invFoot = s / kFootCurv;
    // Each substep spreads snow by at most one column.
    const int x0 = std::max(0, first - steps - 1), x1 = std::min(W, last + steps + 2);
    const bool openL = params_.leftOpen, openR = params_.rightOpen;
    float* K = work_.data();
    float* F = scratch_.data();  // F[k]: flow from column k-1 into column k
    const float* ic = crestInv_.data();
    for (int n = 0; n < steps; ++n) {
        // Beyond an open end the ground drops away; a closed end mirrors.
        const float gl = openL ? 0.0f : z[0];
        const float gr = openR ? 0.0f : z[W - 1];
        for (int i = x0; i < x1; ++i) {
            const float a = i > 0 ? z[i - 1] : gl;
            const float b = i + 1 < W ? z[i + 1] : gr;
            K[i] = a + b - 2.0f * z[i];
        }
        for (int k = x0; k <= x1; ++k) {
            const float diff = (k > 0 ? z[k - 1] : gl) - (k < W ? z[k] : gr);
            if (diff == 0.0f) {
                F[k] = 0.0f;
                continue;
            }
            const int il = std::max(k - 1, x0), ir = std::min(k, x1 - 1);
            const float sharp = std::max(-K[il] * ic[il], -K[ir] * ic[ir]);
            const float hollow = std::max(K[il], K[ir]) * invFoot;
            F[k] = (gBase + gCrest * Saturate(2.0f * (sharp - 1.0f)) + gFoot * Saturate(hollow - 1.0f)) * diff;
        }
        for (int i = x0; i < x1; ++i) {
            const float v = z[i] + F[i] - F[i + 1];
            z[i] = v > kDust ? v : 0.0f;
        }
    }
    Touch(x0, x1, false);  // diffusion never steepens beyond repose
}

// Saltation: walking downwind, the moving flux relaxes toward the surface's
// carrying capacity; where it rises the surface is scoured, where it falls
// (in the lee of crests) snow drops out. Drifts grow gentle windward ramps,
// steep lee faces, and creep downwind.
void Drift::WindStep(float dt, float windPx) {
    const float s = params_.dpiScale;
    const float u = std::fabs(windPx) / s;
    if (u <= kWindThreshold || Empty()) return;
    const int W = width_;
    const float Q = kFluxScale * std::pow(u - kWindThreshold, 1.5f) * s * s;  // px^2/s
    const float alpha = 1.0f - std::exp(-1.0f / (kSaturationLen * s));
    const float availH = 1.5f * s;
    const float crestRef = 0.3f * std::max(capHeight_, 4.0f * s);
    const int crestR = RoundPx(kPeakRadius * s);
    const int dir = windPx > 0.0f ? 1 : -1;
    const bool openUpwind = dir > 0 ? params_.leftOpen : params_.rightOpen;
    int i = dir > 0 ? 0 : W - 1;
    const int end = dir > 0 ? W : -1;

    float q = 0.0f;
    float shelter = -1.0e9f;
    float prev = height_[static_cast<size_t>(i)];
    bool first = true;
    int lo = W, hi = -1;
    for (; i != end; i += dir) {
        float& hr = height_[static_cast<size_t>(i)];
        const float h = hr;
        shelter -= kShelterTan;
        float qs = 0.0f;
        if (shelter <= h + 0.25f * s) {
            float rise = Clamp(h - prev, 0.0f, 1.0f);
            float crest = Saturate((smooth_[static_cast<size_t>(i)] - WideAverage(i, crestR)) / crestRef);
            qs = Q * (1.0f + kStossGain * rise + kCrestGain * crest) * SmoothStep(0.0f, availH, h);
        }
        shelter = std::max(shelter, h);
        if (first) {
            // A closed end receives what the neighbouring screen sends: assume equilibrium.
            if (!openUpwind) q = qs;
            first = false;
        }
        float qn = q + (qs - q) * alpha;
        float dh = (q - qn) * dt;
        if (h + dh < 0.0f) {
            dh = -h;
            qn = q + h / dt;
        }
        if (dh != 0.0f) {
            hr = std::min(h + dh, capHeight_);
            lo = std::min(lo, i);
            hi = std::max(hi, i);
        }
        q = qn;
        prev = h;
    }
    Touch(lo, hi + 1);
}

// Slow melt and settling, applied one block per call in rotation so that the
// changes (and the re-shading they cause) stay local. Each block melts by the
// time that has passed since its last visit, so the rate is frame-rate free.
void Drift::MeltBlocks(float dt) {
    const int nb = static_cast<int>(meltAge_.size());
    if (nb == 0) return;
    for (float& a : meltAge_) a += dt;
    meltDebt_ += dt * static_cast<float>(nb) / kMeltPeriod;
    int n = std::min(static_cast<int>(meltDebt_), nb);
    meltDebt_ = std::min(meltDebt_ - static_cast<float>(n), 1.0f);
    const float s = params_.dpiScale;
    const int peakR = RoundPx(kPeakRadius * s);
    const float sub = kSublimation * s;
    for (; n > 0; --n) {
        const int b = meltCursor_;
        meltCursor_ = (meltCursor_ + 1) % nb;
        const float age = meltAge_[static_cast<size_t>(b)];
        meltAge_[static_cast<size_t>(b)] = 0.0f;
        const int x0 = b * kMeltBlock, x1 = std::min(width_, x0 + kMeltBlock);
        bool any = false;
        for (int i = x0; i < x1; ++i) {
            float& h = height_[static_cast<size_t>(i)];
            if (h <= 0.0f) continue;
            const float cap = std::max(LocalCap(i), 1.0f);
            float expo = std::max(0.0f, smooth_[static_cast<size_t>(i)] - WideAverage(i, peakR)) / (0.25f * cap + s);
            float fill = h / cap;
            float rate = (1.0f + kPeakMelt * std::min(expo, 2.0f) + kTallMelt * fill * fill) / kMeltTau;
            float v = h * std::exp(-rate * age) - sub * age;
            if (v > cap) v = cap + (v - cap) * std::exp(-age / kOverCapTau);
            h = std::max(v, 0.0f);
            any = true;
        }
        if (any) Touch(x0, x1, false);
    }
}

// Light blur of the changed range into smooth_, neighbourhood sums, and the
// shading dirty set (columns whose visible height moved noticeably).
void Drift::Derive() {
    if (chg1_ <= chg0_ || width_ <= 0) return;
    const int W = width_;
    const float s = params_.dpiScale;
    const int R = RoundPx(1.0f * s);
    const float norm = 1.0f / static_cast<float>((R + 1) * (R + 1));
    const int a = std::max(0, chg0_ - R), b = std::min(W, chg1_ + R);
    const bool wasEmpty = Empty();
    for (int i = a; i < b; ++i) {
        float sum = 0.0f;
        for (int k = -R; k <= R; ++k) {
            int j = Clamp(i + k, 0, W - 1);
            sum += static_cast<float>(R + 1 - std::abs(k)) * height_[static_cast<size_t>(j)];
        }
        smooth_[static_cast<size_t>(i)] = sum * norm;
    }
    for (int i = a; i < W; ++i)
        prefix_[static_cast<size_t>(i) + 1] = prefix_[static_cast<size_t>(i)] + smooth_[static_cast<size_t>(i)];

    // Shading reads neighbours up to the AO radius away.
    const int dep = RoundPx(kAoRadius * s) + 3;
    int run0 = -1, run1 = -1;
    for (int i = a; i < b; ++i) {
        const size_t u = static_cast<size_t>(i);
        float now = smooth_[u], was = shadedS_[u];
        bool moved = std::fabs(now - was) > kRedrawEps || ((now >= kVisibleMin) != (was >= kVisibleMin));
        if (moved) {
            if (run0 < 0) run0 = i;
            run1 = i;
        } else if (run0 >= 0 && i - run1 > kShadeBlock) {
            MarkDirty(run0 - dep, run1 + 1 + dep);
            run0 = -1;
        }
    }
    if (run0 >= 0) MarkDirty(run0 - dep, run1 + 1 + dep);

    float top = 0.0f;
    for (float h : height_) top = std::max(top, h);
    maxH_ = top;
    if (!wasEmpty && Empty()) MarkAllDirty();  // clear the strip in one go
    chg0_ = W;
    chg1_ = 0;
}

// ---------------------------------------------------------------------------
// Shading
// ---------------------------------------------------------------------------

int Drift::Shade(int* x0, int* x1, int maxSpans) {
    if (!x0 || !x1 || maxSpans < 1 || width_ <= 0 || texHeight_ <= 0 || pixels_.empty()) return 0;
    if (fullDirty_) {
        ShadeRange(0, width_);
        std::fill(blockDirty_.begin(), blockDirty_.end(), static_cast<uint8_t>(0));
        dirtyBlocks_ = 0;
        fullDirty_ = false;
        unshadedUpdates_ = 0;
        x0[0] = 0;
        x1[0] = width_;
        return 1;
    }
    if (dirtyBlocks_ <= 0) {
        unshadedUpdates_ = 0;
        return 0;
    }
    const int nb = static_cast<int>(blockDirty_.size());
    int budget = kShadeBudget;
    if (unshadedUpdates_ > 2) {
        // Not drawn for a while (hidden or paused): catch up in one go so
        // nothing stale shows when drawing resumes.
        budget = nb;
    } else if (meltLeft_ > 0.0f || (Enabled() && capHeight_ > params_.maxHeight)) {
        // The whole strip is moving: sweep it in as few equal passes as
        // possible, left to right, so at most one seam shows.
        const int passes = (nb + kAnimBlocks - 1) / kAnimBlocks;
        budget = std::max(budget, (nb + passes - 1) / passes);
    }
    unshadedUpdates_ = 0;

    // Round robin from where the last call stopped: shade one cluster of dirty
    // blocks after another until the budget is used. Distant changes become
    // separate spans, so the clean columns between them are not re-uploaded.
    spans_.clear();
    int c0 = shadeCursor_ < nb ? shadeCursor_ : 0;
    while (budget > 0 && dirtyBlocks_ > 0) {
        for (int k = 0; k < nb && !blockDirty_[static_cast<size_t>(c0)]; ++k) c0 = c0 + 1 < nb ? c0 + 1 : 0;
        if (!blockDirty_[static_cast<size_t>(c0)]) {
            dirtyBlocks_ = 0;  // the count was off; nothing is dirty
            break;
        }
        int c1 = c0;
        for (int k = c0 + 1; k < nb && k - c0 < budget; ++k) {
            if (blockDirty_[static_cast<size_t>(k)]) c1 = k;
            else if (k - c1 > kMaxGap) break;
        }
        ShadeRange(c0 * kShadeBlock, std::min(width_, (c1 + 1) * kShadeBlock));
        for (int k = c0; k <= c1; ++k) {
            uint8_t& d = blockDirty_[static_cast<size_t>(k)];
            if (d) {
                d = 0;
                --dirtyBlocks_;
            }
        }
        budget -= c1 - c0 + 1;
        spans_.emplace_back(c0, c1 + 1);
        c0 = c1 + 1 < nb ? c1 + 1 : 0;
    }
    shadeCursor_ = c0;
    if (spans_.empty()) return 0;

    // Ascending and disjoint, touching spans joined; then, while there are
    // more than the caller takes, merge the two closest neighbours.
    std::sort(spans_.begin(), spans_.end());
    size_t n = 0;
    for (size_t i = 1; i < spans_.size(); ++i) {
        if (spans_[i].first <= spans_[n].second) spans_[n].second = std::max(spans_[n].second, spans_[i].second);
        else spans_[++n] = spans_[i];
    }
    spans_.resize(n + 1);
    while (spans_.size() > static_cast<size_t>(maxSpans)) {
        size_t best = 0;
        for (size_t i = 1; i + 1 < spans_.size(); ++i)
            if (spans_[i + 1].first - spans_[i].second < spans_[best + 1].first - spans_[best].second) best = i;
        spans_[best].second = spans_[best + 1].second;
        spans_.erase(spans_.begin() + static_cast<std::ptrdiff_t>(best) + 1);
    }
    for (size_t i = 0; i < spans_.size(); ++i) {
        x0[i] = spans_[i].first * kShadeBlock;
        x1[i] = std::min(width_, spans_[i].second * kShadeBlock);
    }
    return static_cast<int>(spans_.size());
}

void Drift::ShadeRange(int x0, int x1) {
    const int W = width_;
    const float s = params_.dpiScale;
    const float invS = 1.0f / s;
    const int aoR = RoundPx(kAoRadius * s);
    const int lipR = RoundPx(kLipRadius * s);
    const int slopeK = RoundPx(s);
    const float lipMax = kLipMax * s;
    const float shadowLen = kShadowLen * s;

    // Per-column inputs with a one-column apron on each side.
    for (int k = 0; k < x1 - x0 + 2; ++k) {
        const int i = Clamp(x0 - 1 + k, 0, W - 1);
        const size_t u = static_cast<size_t>(k);
        float h = smooth_[static_cast<size_t>(i)];
        colS_[u] = h >= kVisibleMin ? h : 0.0f;
        colWide_[u] = WideAverage(i, aoR);
        float pres = SmoothStep(1.5f * s, 12.0f * s, WideAverage(i, lipR));
        colLip_[u] = lipMax * pres * lipF_[static_cast<size_t>(i)];
    }

    // Light from the upper left (unit vector in the profile plane).
    constexpr float kLx = -0.42f, kLy = 0.907f;
    const int ground = groundRow_;
    const int rows = texHeight_;

    for (int x = x0; x < x1; ++x) {
        const size_t k = static_cast<size_t>(x - x0 + 1);
        uint32_t* px = pixels_.data() + x;
        const float S = colS_[k];
        const float SL = 0.5f * (colS_[k - 1] + S), SR = 0.5f * (S + colS_[k + 1]);
        const float hi = std::max(S, std::max(SL, SR));
        const float lo = std::min(S, std::min(SL, SR));
        const float wide = colWide_[k];
        const float lipC = colLip_[k];
        const float lipL = 0.5f * (colLip_[k - 1] + lipC), lipR2 = 0.5f * (lipC + colLip_[k + 1]);
        const float shadowA = kShadowAlpha * SmoothStep(0.3f * s, 8.0f * s, wide);

        if (hi <= 0.0f && shadowA <= 0.002f && lipC <= 0.0f) {
            for (int r = 0; r < rows; ++r) px[static_cast<size_t>(r) * static_cast<size_t>(W)] = 0u;
            shadedS_[static_cast<size_t>(x)] = smooth_[static_cast<size_t>(x)];
            continue;
        }

        // Surface slope from a slightly wider stencil. Near the top the snow
        // is lit by its own slope (light from the upper left); deeper down it
        // turns into the face of the drift. Depth and concave dips add soft,
        // cool ambient occlusion.
        const float hl = smooth_[static_cast<size_t>(std::max(x - slopeK, 0))];
        const float hr = smooth_[static_cast<size_t>(std::min(x + slopeK, W - 1))];
        const float sx = (hr - hl) / static_cast<float>(2 * slopeK);
        const float slopeLight = Clamp((kLy - sx * kLx) / (kLy * std::sqrt(1.0f + sx * sx)), 0.55f, 1.12f);
        const float thick = SmoothStep(1.0f * s, 10.0f * s, S);  // thin snow stays white
        const float dip = Saturate((wide - S) / (2.0f * s + 0.3f * wide));

        // Rows above the surface are clear.
        int first = ground - 1 - static_cast<int>(std::floor(hi));
        first = Clamp(first, 0, ground);
        for (int r = 0; r < first; ++r) px[static_cast<size_t>(r) * static_cast<size_t>(W)] = 0u;

        // The pile.
        for (int r = first; r < ground; ++r) {
            const float yb = static_cast<float>(ground - 1 - r);  // row bottom, px above ground
            float cov;
            if (yb + 1.0f <= lo) cov = 1.0f;
            else cov = 0.5f * (EdgeCoverage(SL - yb, S - yb) + EdgeCoverage(S - yb, SR - yb));
            if (cov <= 0.0f) {
                px[static_cast<size_t>(r) * static_cast<size_t>(W)] = 0u;
                continue;
            }
            const float d = std::max(S - (yb + 0.5f), 0.0f) * invS;  // DIP below the surface
            const float q = d * (1.0f / kRound);
            const float g = 1.0f / (1.0f + q * q);
            const float depthT = d / (d + 8.0f);
            const float ao = thick * Saturate(0.6f * depthT + dip * (0.3f + 0.6f * depthT));
            float light = Lerp(kFrontLight, slopeLight, g) * (1.0f - 0.32f * ao);
            const float rim = Saturate(1.0f - d);
            light += 0.08f * rim * rim;  // crisp bright edge
            // Grain that moves with the surface: keyed by depth below it.
            const uint32_t di = static_cast<uint32_t>(std::max(S - yb, 0.0f));
            const uint32_t hsh = Hash32(static_cast<uint32_t>(x) * 0x9E3779B1u ^ (di * 0x85EBCA77u + 0x2545F491u));
            light *= 1.0f + 0.025f * (static_cast<float>(hsh & 0xFFu) * (1.0f / 255.0f) - 0.5f);
            Rgb c = SnowColor(light);
            if (((hsh >> 8) & 0x3FFu) < 4u) c = Mix(c, kWhite, 0.8f);  // the odd sparkle
            px[static_cast<size_t>(r) * static_cast<size_t>(W)] = PackPremul(c, cov);
        }

        // Below the ground line: the lip drooping over the edge, and its shadow.
        for (int r = ground; r < rows; ++r) {
            const float yd = static_cast<float>(r - ground);  // row top, px below ground
            float lipCov = 0.0f;
            if (lipC > 0.0f || lipL > 0.0f || lipR2 > 0.0f)
                lipCov = 0.5f * (EdgeCoverage(lipL - yd, lipC - yd) + EdgeCoverage(lipC - yd, lipR2 - yd));
            const float yc = yd + 0.5f;
            float t = (yc - 0.5f * lipC) / (shadowLen + 0.5f * lipC);
            float sh = t <= 0.0f ? shadowA : (t >= 1.0f ? 0.0f : shadowA * (1.0f - t) * (1.0f - t));
            if (lipCov <= 0.0f) {
                px[static_cast<size_t>(r) * static_cast<size_t>(W)] = sh > 0.0f ? PackPremul(kShadow, sh) : 0u;
                continue;
            }
            const float d = (S + yc) * invS;
            const float depthT = d / (d + 8.0f);
            const float ao = thick * Saturate(0.6f * depthT + dip * (0.3f + 0.6f * depthT));
            Rgb c = SnowColor(kFrontLight * (1.0f - 0.32f * ao));
            c = Mix(c, kUnder, SmoothStep(0.15f, 1.0f, yc / std::max(lipC, 0.5f)));
            // Lip over shadow, premultiplied.
            const float aOut = lipCov + sh * (1.0f - lipCov);
            const float inv = 1.0f / aOut;
            Rgb o = {(c.r * lipCov + kShadow.r * sh * (1.0f - lipCov)) * inv,
                     (c.g * lipCov + kShadow.g * sh * (1.0f - lipCov)) * inv,
                     (c.b * lipCov + kShadow.b * sh * (1.0f - lipCov)) * inv};
            px[static_cast<size_t>(r) * static_cast<size_t>(W)] = PackPremul(o, aOut);
        }
        shadedS_[static_cast<size_t>(x)] = smooth_[static_cast<size_t>(x)];
    }
}
