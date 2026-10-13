// The lights' engine: the strands laid out on their hooks, the bulbs along
// them, how they move and how much light each gives.
//
// Each span between two hooks hangs as a catenary of its length (on a steep
// run, a bow), sampled into a polyline, and the bulbs sit at even steps along
// the whole strand. A span swings like a pendulum: its belly moves sideways and
// rises on an arc while its hooks stay put, leaning with the wind, pushed about
// by gusts and by the pointer. Each bulb hangs from the wire on a springy
// socket and swings when the wire moves under it or the pointer brushes it. A
// pattern says how bright each bulb should be; a filament follows it with a
// little lag (an LED at once).
#include "LightsParts.h"

#include "Noise.h"

using namespace lights;

namespace {

// The automatic drape across the top.
constexpr float kTopInset = 6.0f;    // DIP: its hooks sit this far below the top edge
constexpr float kScallop = 520.0f;   // DIP: about this wide between its hooks
constexpr float kTopSag = 0.14f;     // its sag, of the distance between hooks
constexpr float kTopDepth = 0.15f;   // on a short screen, more scallops so they hang no deeper than this of it ...
constexpr float kMinScallop = 200.0f;  // DIP: ... though none narrower than this

// How a span hangs.
constexpr float kWireStep = 3.5f;       // px between points along a span: reads smooth at any scale
constexpr int kMaxSpanPoints = 4096;    // however long the span
constexpr int kFine = 2;                // a span is measured this many times finer than its points
constexpr float kSteepFrom = 0.45f;     // a span dropping this much of its length starts to hang as a bow ...
constexpr float kSteepTo = 0.85f;       // ... and from here it is all bow (no "J" on a side run)
constexpr float kInteriorLean = 0.2f;   // a bow within about 11 degrees of vertical bulges into the screen
constexpr float kFlipBand = 0.05f;      // ... flattening through straight over about 3 degrees where it changes sides
constexpr float kLeftSide = 600.0f;     // DIP: a vertical run left of this is taken to be on the screen's left

// Laying the strands out.
constexpr int kMaxBulbs = 1500;         // per scene
constexpr int kMaxWirePoints = 30000;   // per scene: more wire than this is sampled more coarsely
constexpr float kHookRange = 4.0f;      // hooks further off the screen than this (of its size) are held there
constexpr float kSnapEdge = 6.0f;       // DIP: a hook this close to an edge is shown as snapped to it

// Sway: each span is a pendulum as long as its belly hangs below its chord.
constexpr float kSwayG = 1050.0f;       // DIP/s^2: a 60 DIP belly swings back and forth in 1.5 s
constexpr float kSwayPeriodMin = 0.7f;  // s: a short, nearly taut span
constexpr float kSwayPeriodMax = 2.2f;  // s: a long, deep one
constexpr float kSwayZeta = 0.08f;      // lightly damped: a brushed strand swings a few times
constexpr float kSwayCouple = 0.12f;    // how much a span tugs its neighbours through their shared hook
constexpr float kMaxSwing = 0.5f;       // rad either way
constexpr float kStrongWind = 200.0f;   // DIP/s counted as a strong wind
constexpr float kLeanMax = 0.08f;       // rad: the lean in a strong steady wind (a 60 DIP belly moves ~5 DIP)
constexpr float kGustMax = 0.14f;       // rad: a strong gust's push on top of it
constexpr float kBuffet = 0.12f;        // rad: how far a strong wind buffets a span about (its own way)
constexpr float kBuffetRate = 0.6f;     // /s: how quickly the buffeting changes
constexpr float kLeanTau = 3.0f;        // s: the prevailing wind, out of the wind now
constexpr float kGustTau = 0.35f;       // s: gusts smoothed, so the strands never jitter
constexpr float kSubstepHz = 240.0f;    // the same motion at 30 Hz as at 240

// The pointer.
constexpr float kReach = 10.0f;         // DIP: it brushes what it passes this close to
constexpr float kBrushMin = 25.0f;      // DIP/s: slower than this it is just resting there
constexpr float kBrushDrag = 0.35f;     // of the pointer's speed a brushed span takes up
constexpr float kBrushMax = 140.0f;     // DIP/s: the most it takes up, however fast the brush
constexpr float kBrushTau = 0.08f;      // s: how quickly it takes it up
constexpr float kKick = 0.7f;           // of the pointer's speed a brushed bulb's tip takes up

// Dangle: each bulb hangs from its socket like a stiff little pendulum.
constexpr float kDangleGain = 1.4f;     // a touch livelier than the real thing, so it reads on screen
constexpr float kMaxDangle = 1.047f;    // rad (60 degrees) either way from where it rests
constexpr float kMaxDangleV = 9.0f;     // rad/s
constexpr float kMaxTilt = 1.75f;       // rad from hanging straight down: never pointing up

// Patterns.
constexpr float kMaxFlashHz = 2.5f;     // /s: no pattern repeats faster (no bulb flashes over 3 times in a second)
constexpr float kTwinkleSlot = 0.6f;    // s
constexpr float kTwinkleOdds = 0.16f;   // that a bulb drops out during a slot
constexpr float kPulsePeriod = 3.2f;    // s
constexpr float kPulseFloor = 0.12f;    // an ember at the bottom of a breath
constexpr float kChaseRate = 4.0f;      // steps/s
constexpr float kChaseFade = 0.3f;      // of a step: neighbouring sets cross-fade
constexpr float kAlternatePeriod = 1.6f;  // s
constexpr float kAlternateFade = 0.06f;   // of a period
constexpr float kWavePeriod = 2.4f;     // s
constexpr float kWaveLength = 10.0f;    // bulbs
constexpr float kWaveFloor = 0.2f;      // the dimmest a wave leaves a bulb
constexpr float kProgram = 12.0f;       // s: each pattern of the combination
constexpr float kProgramFade = 1.5f;    // s: cross-fading into the next
constexpr float kSwitchFade = 0.6f;     // s: a new animation cross-fades from the old

// Power and fades.
constexpr float kPowerDelay = 0.15f;    // s before the first bulb comes on
constexpr float kPowerSpread = 1.3f;    // s from the first bulb on a strand to the last
constexpr float kQuickSpread = 0.6f;    // ... when only the bulbs changed
constexpr float kPowerRamp = 0.12f;     // s: each bulb switching on
constexpr float kShowFade = 0.3f;       // s: the strands appearing
constexpr float kHideFade = 0.4f;       // s: and going
constexpr float kNewBulbFade = 0.15f;   // s: a bulb new on a strand (hanging mode)
constexpr float kVeil = 0.28f;          // hanging mode dims the screen this much
constexpr float kVeilFade = 0.2f;       // s
constexpr double kEditGrace = 2.0;      // s after hanging mode its strands stay up until the settings with them arrive

// Windows over the bulbs.
constexpr int kProbeBudget = 48;        // looks per call
constexpr float kCoverOut = 0.25f;      // s: stepping aside
constexpr float kCoverIn = 0.35f;       // s: and back

// Per-type extras.
constexpr float kCandleDip = 0.12f;     // a candle's flame pulls its light down this much as it flickers
constexpr float kBubbleWarm = 2.5f;     // s: a bubble light's liquid warming up ...
constexpr float kBubbleCool = 1.8f;     // ... and cooling
constexpr double kBubbleWrap = 3600.0;  // s: its bubbles' clock starts over (keeps float precision)

// Noise inputs repeat every 256 units; wrapping in double keeps float precision.
float Wrap256(double v) { return static_cast<float>(std::fmod(v, 256.0)); }
float Frac(double v) { return static_cast<float>(v - std::floor(v)); }
float Ease(float t) { return t * t * (3.0f - 2.0f * t); }

bool Finite(D2D1_POINT_2F p) { return std::isfinite(p.x) && std::isfinite(p.y); }

// x > 0 with sinh(x) / x = r: a catenary r times as long as it is wide is
// 2x of its parameter wide. Newton's method on the logarithm, kept inside a
// bracket so it can't wander off.
double CatenaryHalfWidth(double r) {
    if (!(r > 1.0 + 1e-12)) return 0.0;
    const double goal = std::log(r);
    double lo = 0.0, hi = 2.0 * std::log(2.0 * r) + 4.0;
    double x = r < 2.0 ? std::sqrt(6.0 * (r - 1.0)) : std::log(2.0 * r) + std::log(std::log(2.0 * r) + 1.0);
    x = Clamp(x, 1e-9, hi);
    for (int i = 0; i < 80; ++i) {
        const double f = std::log(std::sinh(x) / x) - goal;
        if (f > 0.0) hi = x;
        else lo = x;
        const double d = x < 1e-3 ? x / 3.0 : 1.0 / std::tanh(x) - 1.0 / x;
        double next = x - f / d;
        if (!(next > lo && next < hi)) next = 0.5 * (lo + hi);
        if (std::fabs(next - x) <= 1e-12 * x) return next;
        x = next;
    }
    return x;
}

// One span's rest shape from hook a to hook b, at u (0..1) along its chord: a
// catenary of its length (worked out with y up, from the left hook to the
// right), blended on a steep span into a bow square to the chord.
struct Hang {
    double ax = 0.0, ay = 0.0, dx = 0.0, dy = 0.0;
    double w = 0.0;                                      // 0: catenary .. 1: bow
    double A = 0.0, x0 = 0.0, x1 = 0.0, h = 1.0, v = 0.0;  // the catenary: parameter, lowest x, left hook, width, rise
    double c = 0.0, xb = 0.0, nx = 0.0, ny = 0.0;        // the bow: chord, half-width parameter, which way it bulges

    Hang(D2D1_POINT_2F a, D2D1_POINT_2F b, double slack, double midLine) {
        ax = a.x;
        ay = a.y;
        dx = static_cast<double>(b.x) - ax;
        dy = static_cast<double>(b.y) - ay;
        c = std::hypot(dx, dy);
        if (!(c > 1e-3) || !(slack > 1e-6)) return;  // straight
        const double L = c * (1.0 + slack);
        w = SmoothStep(kSteepFrom, kSteepTo, static_cast<float>(std::fabs(dy) / c));
        if (w < 1.0) {
            const bool aLeft = dx >= 0.0;
            x1 = aLeft ? ax : static_cast<double>(b.x);
            h = std::fabs(dx);
            v = aLeft ? -dy : dy;  // the right hook's height over the left's
            const double xi = CatenaryHalfWidth(std::sqrt(std::max(L * L - v * v, h * h)) / h);
            if (xi > 0.0) {
                A = h / (2.0 * xi);
                x0 = x1 + 0.5 * h - A * std::atanh(Clamp(v / L, -0.999999, 0.999999));
            }
        }
        if (w > 0.0) {
            // A catenary of the same length on the chord itself, bulging the
            // way gravity pulls it; near vertical, into the screen (on a side
            // run gravity can't say). Where the two disagree it flattens through
            // straight, so a span swung past vertical never jumps sides.
            xb = CatenaryHalfWidth(1.0 + slack);
            nx = -dy / c;
            ny = dx / c;
            const double inward = 0.5 * (ax + static_cast<double>(b.x)) < midLine ? 1.0 : -1.0;
            const double side = ny + kInteriorLean * inward * nx;
            const double depth = SmoothStep(0.0f, kFlipBand, static_cast<float>(std::fabs(side)));
            nx *= side < 0.0 ? -depth : depth;
            ny *= side < 0.0 ? -depth : depth;
        }
    }

    D2D1_POINT_2F At(double u) const {
        double x = ax + u * dx, y = ay + u * dy;
        if (A > 0.0) {
            // How far the catenary hangs below the chord here.
            const double rise = 2.0 * A * std::sinh((x + x1 - 2.0 * x0) / (2.0 * A)) * std::sinh((x - x1) / (2.0 * A));
            y += (1.0 - w) * (v * (x - x1) / h - rise);
        }
        if (xb > 0.0) {
            const double depth = c / xb * std::sinh(xb * u) * std::sinh(xb * (1.0 - u));
            x += w * depth * nx;
            y += w * depth * ny;
        }
        return {static_cast<float>(x), static_cast<float>(y)};
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// The engine
// ---------------------------------------------------------------------------

struct Lights::Engine {
    struct Span {
        int first = 0, last = 0;  // its points (the hook it starts at is the last span's end); equal: no length
        float len = 0.0f;         // px: pendulum length, how deep its belly hangs below the chord
        float omega = 0.0f;       // rad/s
        float rise = 1.0f;        // 1: its belly rises as it swings out (a level span) .. 0: a side run
        float bellyX = 0.0f, bellyY = 1.0f;  // which way it hangs off its chord (its bulbs lean out that way)
        float phase = 0.0f;       // its own flutter in the wind
        float rest = 0.0f;        // rad: where the wind holds it now
        float theta = 0.0f, thetaV = 0.0f;  // the swing (rad, rad/s)
        float ax = 0.0f, ay = 0.0f;         // px/s^2: its belly's acceleration this substep
        float dx = 0.0f, dy = 0.0f;         // px: its belly's displacement now
    };
    struct Strand {
        int source = 0;           // which of the source strands (the editor's, while hanging)
        int firstPoint = 0, points = 0;
        int firstSpan = 0, spans = 0;
        int firstBulb = 0, bulbs = 0;
        D2D1_RECT_F bounds{};     // px: its rest points
    };
    struct Bulb {
        // Where it hangs.
        int strand = 0, index = 0;   // its place along its strand
        int span = 0;
        float w = 0.0f;              // its share of its span's swing (sin(pi u): none at the hooks)
        float restX = 0.0f, restY = 0.0f;
        float rest = 0.0f;           // rad: its angle hanging still
        float cr = 1.0f, sr = 0.0f;  // cos, sin of it
        float tiltX = 0.0f, tiltY = 0.0f;  // rad per px of its span's swing: the wire turning under it
        float scale = 1.0f;          // px per DIP
        float arm = 1.0f;            // px from the socket to where its weight hangs
        float omega = 0.0f, zeta = 0.0f;
        float power = 0.0f;          // s after power-on that it comes on
        float bright = 1.0f;         // an incandescent's own brightness
        float phase = 0.0f;          // offsets its twinkle slots
        float waveC = 1.0f, waveS = 0.0f;  // its place in a wave
        int color = 0;
        uint32_t seed = 0;
        // State, kept when the strands are laid out again.
        float heat = 0.0f, level = 0.0f;
        float swing = 0.0f, swingV = 0.0f;  // rad, rad/s from rest
        float appear = 1.0f;         // a bulb new on a strand fades in
        float vis = 1.0f;            // stepping aside for a window
        bool covered = false, probed = false;
        float flame = 0.0f;          // a candle's flicker, -1..1
        float liquid = 0.0f;         // a bubble light's liquid temperature, 0..1
        double bubble = 0.0;         // s its bubbles have run
        // Now.
        float x = 0.0f, y = 0.0f, angle = 0.0f;
    };
    struct Clocks {
        double twinkle = 0.0, pulse = 0.0, chase = 0.0, alternate = 0.0, waves = 0.0, program = 0.0;
    };

    LightsLayout layout;
    LightParams params;
    LightsEdit edit;
    Frame frame;
    std::vector<LightStrand> top;    // the drape across the top, for this layout

    bool dirty = true;               // the strands need laying out again
    bool fresh = true;               // ... and their bulbs come on afresh (else they keep their state)
    float spread = kPowerSpread;     // s: power-on from the first bulb to the last
    double now = 0.0;
    double editEnded = -1.0e9;       // when hanging mode last ended
    std::vector<LightStrand> held;   // ... and the strands it ended with, shown until the settings catch up
    bool holding = false;
    float fade = 0.0f;               // 0 hidden .. 1 shown
    float veil = 0.0f;
    float powerClock = 0.0f;         // s since power-on
    bool calm = false;
    float calmCheck = 0.0f;
    bool windInit = false;
    bool settle = true;              // spans laid out afresh take the wind's lean at once
    float windSlow = 0.0f, gust = 0.0f;  // px/s
    LightAnimation anim = LightAnimation::Count, from = LightAnimation::Steady;
    float blend = 1.0f;              // from `from` to `anim`
    Clocks clocks;
    float pulse = 0.0f, waveCos = 1.0f, waveSin = 0.0f;  // this frame's shared pattern values
    int unprobed = 0;
    size_t probeNext = 0;

    std::vector<Strand> strands;
    std::vector<Span> spans;
    std::vector<Bulb> bulbs;
    std::vector<D2D1_POINT_2F> rest;  // every strand's wire at rest, px
    std::vector<int> pointSpan;       // which span each point belongs to (a hook: the span it ends)
    std::vector<float> pointW;        // its share of the span's swing
    std::vector<int> pointBulb;       // the bulb before it along the wire (-1: none) ...
    std::vector<float> pointT;        // ... and how far on to the next (its alpha follows theirs)
    std::vector<int> order;           // bulbs in drawing order
    // Scratch, kept to save allocating.
    std::vector<Strand> oldStrands;
    std::vector<Span> oldSpans;
    std::vector<Bulb> oldBulbs;
    std::vector<int> oldBySource;
    std::vector<D2D1_POINT_2F> shape, hooks;
    std::vector<float> slacks, arc, coverAlpha, touch, touchW;

    bool Shown() const { return (params.show || edit.active) && layout.width > 0 && layout.height > 0; }
    const BulbSpec& TheSpec() const { return Spec(params.bulb); }
    float Size() const { return SizeScale(Clamp(params.size, 0, 100)); }

    void Update(float dt, double time, float windPx, const LightsPointer& pointer);
    void Layout();
    void LayStrand(int source, const LightStrand& ls, bool live, int stride, bool carry);
    void Wind(float dt, float windPx);
    void Brush(float dt, const LightsPointer& p);
    void Sway(float dt);
    void Levels(float dt);
    float Target(LightAnimation a, const Bulb& b) const;
    void Cover(float dt);
    void Fill();
    void Probe(const std::function<bool(float, float)>& covered);
    void Recheck();
};

const Frame& lights::FrameOf(const Lights::Engine& engine) { return engine.frame; }

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

// Lays every strand out from its hooks. Bulbs keep their state by their place
// along their strand (strand and number), unless they come on afresh: so while
// hanging lights a strand grows and changes under the pointer without its
// bulbs flickering.
void Lights::Engine::Layout() {
    dirty = false;
    const bool carry = !fresh;
    fresh = false;
    oldStrands.swap(strands);
    oldSpans.swap(spans);
    oldBulbs.swap(bulbs);
    strands.clear();
    spans.clear();
    bulbs.clear();
    rest.clear();
    pointSpan.clear();
    pointW.clear();
    pointBulb.clear();
    pointT.clear();
    order.clear();
    oldBySource.clear();
    if (carry) {
        for (size_t i = 0; i < oldStrands.size(); ++i) {
            const size_t src = static_cast<size_t>(oldStrands[i].source);
            if (src >= oldBySource.size()) oldBySource.resize(src + 1, -1);
            oldBySource[src] = static_cast<int>(i);
        }
    } else {
        powerClock = 0.0f;
        settle = true;
    }
    unprobed = 0;
    probeNext = 0;
    if (layout.width <= 0 || layout.height <= 0 || !(layout.dpiScale > 0.0f && layout.dpiScale < 64.0f)) return;

    const std::vector<LightStrand>& src = edit.active ? edit.strands
                                          : holding       ? held
                                          : params.acrossTop ? top
                                                             : params.strands;
    const int placing = edit.active && edit.cursorValid ? edit.placing : -1;
    // A lot of wire is sampled more coarsely, so the work stays bounded.
    const float W = static_cast<float>(layout.width), H = static_cast<float>(layout.height);
    auto along = [&](LightHook p, LightHook q) {
        return std::hypot(static_cast<double>(q.u - p.u) * W, static_cast<double>(q.v - p.v) * H) * (1.0 + kMaxSlack);
    };
    double length = 0.0;
    for (size_t i = 0; i < src.size(); ++i) {
        const std::vector<LightHook>& hk = src[i].hooks;
        for (size_t k = 0; k + 1 < hk.size(); ++k) length += along(hk[k], hk[k + 1]);
        if (static_cast<int>(i) == placing && !hk.empty()) length += along(hk.back(), edit.cursor);
    }
    double want = length / kWireStep;
    if (!std::isfinite(want)) want = 1.0e12;
    const int stride = std::max(1, static_cast<int>(std::min(std::ceil(want / kMaxWirePoints), 1.0e6)));

    for (size_t i = 0; i < src.size(); ++i)
        LayStrand(static_cast<int>(i), src[i], static_cast<int>(i) == placing, stride, carry);

    // Upper bulbs first, so lower ones hang in front.
    order.resize(bulbs.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::sort(order.begin(), order.end(), [this](int a, int b) {
        const float ya = bulbs[static_cast<size_t>(a)].restY, yb = bulbs[static_cast<size_t>(b)].restY;
        return ya < yb || (ya == yb && a < b);
    });
    for (const Bulb& b : bulbs)
        if (!b.probed) ++unprobed;
    // Room for the pointer's touches, so brushing allocates nothing.
    touch.assign(spans.size(), 0.0f);
    touchW.assign(spans.size(), 0.0f);
}

void Lights::Engine::LayStrand(int source, const LightStrand& ls, bool live, int stride, bool carry) {
    const float s = layout.dpiScale;
    const float W = static_cast<float>(layout.width), H = static_cast<float>(layout.height);
    // The hooks in px (with the live span to the pointer while placing), and the slack of each span.
    hooks.clear();
    slacks.clear();
    auto add = [&](LightHook h, float slack) {
        if (!std::isfinite(h.u) || !std::isfinite(h.v)) return;
        hooks.push_back({Clamp(h.u, -kHookRange, kHookRange) * W, Clamp(h.v, -kHookRange, kHookRange) * H});
        slacks.push_back(std::isfinite(slack) ? Clamp(slack, 0.0f, kMaxSlack) : kDefaultSlack);
    };
    for (size_t k = 0; k < ls.hooks.size(); ++k) {
        float slack = k < ls.slack.size() ? ls.slack[k] : kDefaultSlack;
        if (live && k + 1 == ls.hooks.size()) slack = edit.placingSlack;
        add(ls.hooks[k], slack);
    }
    if (live && !ls.hooks.empty()) add(edit.cursor, 0.0f);
    if (hooks.size() < 2) return;

    Strand st;
    st.source = source;
    st.firstPoint = static_cast<int>(rest.size());
    st.firstSpan = static_cast<int>(spans.size());
    st.firstBulb = static_cast<int>(bulbs.size());
    const int strandIndex = static_cast<int>(strands.size());
    const Strand* old = nullptr;
    if (carry && static_cast<size_t>(source) < oldBySource.size() && oldBySource[static_cast<size_t>(source)] >= 0)
        old = &oldStrands[static_cast<size_t>(oldBySource[static_cast<size_t>(source)])];

    // The wire, span by span.
    for (size_t k = 0; k + 1 < hooks.size(); ++k) {
        const D2D1_POINT_2F a = hooks[k], b = hooks[k + 1];
        const int spanIndex = static_cast<int>(spans.size());
        Span sp;
        if (old && static_cast<int>(k) < old->spans) {
            const Span& o = oldSpans[static_cast<size_t>(old->firstSpan) + k];
            sp.theta = o.theta;
            sp.thetaV = o.thetaV;
        }
        const float chord = std::hypot(b.x - a.x, b.y - a.y);
        if (static_cast<int>(rest.size()) == st.firstPoint) {
            rest.push_back(a);
            pointSpan.push_back(spanIndex);
            pointW.push_back(0.0f);
        }
        sp.first = sp.last = static_cast<int>(rest.size()) - 1;
        if (chord < 0.5f) {
            // No length: the wire passes straight on.
            sp.theta = sp.thetaV = 0.0f;
            spans.push_back(sp);
            continue;
        }
        shape.clear();
        Lights::SpanShape(a, b, slacks[k], s, shape, W);
        const int n = static_cast<int>(shape.size());
        if (n < 2) {
            spans.push_back(sp);
            continue;
        }
        // Points after the first (the hook it starts at is already there), every
        // stride-th and always the last; how far each is from the chord.
        const float cx = (b.x - a.x) / chord, cy = (b.y - a.y) / chord;
        float deepest = 0.0f, deepOff = 0.0f;
        for (int j = 1; j < n; ++j) {
            const D2D1_POINT_2F p = shape[static_cast<size_t>(j)];
            const float off = (p.x - a.x) * cy - (p.y - a.y) * cx;  // off the chord, along (cy, -cx)
            if (std::fabs(off) > deepest) {
                deepest = std::fabs(off);
                deepOff = off;
            }
            if (j % stride != 0 && j + 1 < n) continue;
            const float u = static_cast<float>(j) / static_cast<float>(n - 1);
            rest.push_back(p);
            pointSpan.push_back(spanIndex);
            pointW.push_back(j + 1 < n ? std::sin(kPi * u) : 0.0f);
        }
        sp.last = static_cast<int>(rest.size()) - 1;
        // A pendulum as long as the belly hangs (a taut span still trembles a little).
        sp.len = std::max(deepest, 2.0f * s);
        const float period = Clamp(kTau * std::sqrt(sp.len / s / kSwayG), kSwayPeriodMin, kSwayPeriodMax);
        const uint32_t h =
            Hash32(static_cast<uint32_t>(source) * 0x9E3779B1u ^ Hash32(static_cast<uint32_t>(k) + 0x51EDu));
        sp.omega = kTau / period * (0.95f + 0.1f * HashFloat(h));
        sp.phase = 100.0f * HashFloat(h + 1u);
        sp.rise = 1.0f - SmoothStep(kSteepFrom, kSteepTo, std::fabs(b.y - a.y) / chord);
        // The side its belly hangs off the chord, where its bulbs lean out. A
        // taut span: its downward side, or into the screen down a side.
        if (deepest > 0.25f * s) {
            sp.bellyX = deepOff > 0.0f ? cy : -cy;
            sp.bellyY = deepOff > 0.0f ? -cx : cx;
        } else if (std::fabs(cx) > kInteriorLean) {
            sp.bellyX = cx > 0.0f ? -cy : cy;
            sp.bellyY = std::fabs(cx);
        } else {
            sp.bellyX = 0.5f * (a.x + b.x) < 0.5f * W ? 1.0f : -1.0f;
            sp.bellyY = 0.0f;
        }
        spans.push_back(sp);
    }
    st.points = static_cast<int>(rest.size()) - st.firstPoint;
    st.spans = static_cast<int>(spans.size()) - st.firstSpan;
    if (st.points < 2) {
        rest.resize(static_cast<size_t>(st.firstPoint));
        pointSpan.resize(rest.size());
        pointW.resize(rest.size());
        spans.resize(static_cast<size_t>(st.firstSpan));
        return;
    }
    st.bounds = {rest[static_cast<size_t>(st.firstPoint)].x, rest[static_cast<size_t>(st.firstPoint)].y,
                 rest[static_cast<size_t>(st.firstPoint)].x, rest[static_cast<size_t>(st.firstPoint)].y};
    arc.resize(static_cast<size_t>(st.points));
    arc[0] = 0.0f;
    for (int j = 1; j < st.points; ++j) {
        const D2D1_POINT_2F p = rest[static_cast<size_t>(st.firstPoint + j)];
        const D2D1_POINT_2F q = rest[static_cast<size_t>(st.firstPoint + j - 1)];
        arc[static_cast<size_t>(j)] = arc[static_cast<size_t>(j) - 1] + std::hypot(p.x - q.x, p.y - q.y);
        st.bounds.left = std::min(st.bounds.left, p.x);
        st.bounds.top = std::min(st.bounds.top, p.y);
        st.bounds.right = std::max(st.bounds.right, p.x);
        st.bounds.bottom = std::max(st.bounds.bottom, p.y);
    }

    // The bulbs, at even steps along the whole strand.
    const BulbSpec& spec = TheSpec();
    const float scale = s * Size();
    const float step = spec.spacing * scale;
    const float total = arc.back();
    const bool mini = spec.length <= 12.0f;
    const float jitter = (mini ? 10.0f : 6.0f) * (kPi / 180.0f);
    const float hz = Lerp(3.0f, 2.0f, Saturate((spec.length - 11.0f) / 14.0f));  // small bulbs swing quicker
    size_t j = 0;
    for (int i = 0; static_cast<int>(bulbs.size()) < kMaxBulbs; ++i) {
        const float at = (static_cast<float>(i) + 0.5f) * step;
        if (at > total) break;
        while (j + 2 < arc.size() && arc[j + 1] < at) ++j;
        const float segLen = arc[j + 1] - arc[j];
        const float t = segLen > 1e-4f ? Saturate((at - arc[j]) / segLen) : 0.0f;
        const size_t p0 = static_cast<size_t>(st.firstPoint) + j, p1 = p0 + 1;
        Bulb b;
        b.strand = strandIndex;
        b.index = i;
        b.span = pointSpan[p1];
        const Span& sp = spans[static_cast<size_t>(b.span)];
        const D2D1_POINT_2F a = rest[p0], c = rest[p1];
        b.restX = Lerp(a.x, c.x, t);
        b.restY = Lerp(a.y, c.y, t);
        // Its share of the swing is the wire's there (a hook's is none), so it
        // stays on the wire however the span swings.
        b.w = Lerp(pointW[p0], pointW[p1], t);
        // Hanging still: between square to the wire (on the side its span hangs
        // off its chord: below it, or into the screen down a side) and
        // straight down, each clipped on a little askew.
        float tx = c.x - a.x, ty = c.y - a.y;
        const float tl = std::hypot(tx, ty);
        if (tl > 1e-5f) {
            tx /= tl;
            ty /= tl;
        } else {
            tx = 1.0f;
            ty = 0.0f;
        }
        float nx = -ty, ny = tx;
        if (nx * sp.bellyX + ny * sp.bellyY < 0.0f) {
            nx = -nx;
            ny = -ny;
        }
        b.seed = Hash32(static_cast<uint32_t>(source) * 0x9E3779B1u ^ Hash32(static_cast<uint32_t>(i) + 0x6A09E667u));
        const float lean = 1.0f - spec.hang;
        b.rest = lean * std::atan2(-nx, ny) + jitter * (2.0f * HashFloat(b.seed + 1u) - 1.0f);
        b.cr = std::cos(b.rest);
        b.sr = std::sin(b.rest);
        // As the span swings the wire turns under the bulb, by how quickly its
        // share of the swing changes along the wire.
        const float dw = segLen > 1e-4f ? (pointW[p1] - pointW[p0]) / segLen : 0.0f;
        b.tiltX = -lean * dw * ty;
        b.tiltY = lean * dw * tx;
        b.scale = scale * (1.0f + 0.04f * (2.0f * HashFloat(b.seed + 2u) - 1.0f));
        b.arm = std::max(spec.glassCenter * b.scale, 1.0f);
        b.omega = kTau * hz * (0.94f + 0.12f * HashFloat(b.seed + 3u));
        b.zeta = mini ? 0.14f : 0.12f;
        b.bright = spec.led ? 1.0f : 0.93f + 0.07f * HashFloat(b.seed + 4u);
        b.phase = HashFloat(b.seed + 5u);
        b.waveC = std::cos(kTau * static_cast<float>(i) / kWaveLength);
        b.waveS = std::sin(kTau * static_cast<float>(i) / kWaveLength);
        b.color = i + 3 * source;
        b.x = b.restX;
        b.y = b.restY;
        b.angle = b.rest;
        if (old && i < old->bulbs) {
            const Bulb& o = oldBulbs[static_cast<size_t>(old->firstBulb + i)];
            b.heat = o.heat;
            b.level = o.level;
            b.swing = o.swing;
            b.swingV = o.swingV;
            b.appear = o.appear;
            b.vis = o.vis;
            b.covered = o.covered;
            b.probed = o.probed;
            b.liquid = o.liquid;
            b.bubble = o.bubble;
        } else if (carry) {
            b.appear = 0.0f;  // new on the strand: fades in
        }
        bulbs.push_back(b);
    }
    st.bulbs = static_cast<int>(bulbs.size()) - st.firstBulb;
    // They come on one by one along the strand.
    for (int i = 0; i < st.bulbs; ++i)
        bulbs[static_cast<size_t>(st.firstBulb + i)].power =
            kPowerDelay + spread * static_cast<float>(i) / static_cast<float>(std::max(st.bulbs - 1, 1));
    // Each point of the wire takes its alpha from the bulbs either side.
    for (int k = 0; k < st.points; ++k) {
        int at = -1;
        float t = 0.0f;
        if (st.bulbs > 0) {
            const float f = arc[static_cast<size_t>(k)] / step - 0.5f;
            const int i = static_cast<int>(std::floor(f));
            if (i < 0) at = 0;
            else if (i >= st.bulbs - 1) at = st.bulbs - 1;
            else {
                at = i;
                t = f - static_cast<float>(i);
            }
            at += st.firstBulb;
        }
        pointBulb.push_back(at);
        pointT.push_back(t);
    }
    strands.push_back(st);
}

// ---------------------------------------------------------------------------
// Motion
// ---------------------------------------------------------------------------

// The prevailing wind leans every span; gusts push them about on top of it,
// each span a little differently.
void Lights::Engine::Wind(float dt, float windPx) {
    if (!std::isfinite(windPx)) windPx = 0.0f;
    if (!windInit) {
        windSlow = windPx;
        gust = 0.0f;
        windInit = true;
    }
    windSlow += (windPx - windSlow) * ApproachFactor(dt, kLeanTau);
    gust += (windPx - windSlow - gust) * ApproachFactor(dt, kGustTau);
    const float strong = kStrongWind * layout.dpiScale;
    const float lean = kLeanMax * std::tanh(windSlow / strong);
    const float push = kGustMax * std::tanh(gust / strong);
    // Moving air buffets the strands, the more the harder and gustier it blows.
    const float buffet = kBuffet * Saturate((std::fabs(windSlow) + 2.0f * std::fabs(gust)) / strong);
    for (Span& sp : spans) {
        if (calm) {
            sp.rest = 0.0f;
        } else {
            const float own = 0.75f + 0.25f * noise::Noise1(Wrap256(now * 0.4 + sp.phase));
            sp.rest = lean + push * own + buffet * noise::Noise1(Wrap256(now * kBuffetRate + 7.0 * sp.phase));
        }
        // Strands that have just appeared start out where the wind holds them.
        if (settle) {
            sp.theta = sp.rest;
            sp.thetaV = 0.0f;
        }
    }
    settle = false;
}

// The pointer passing through a strand drags the spans it touches along with
// it and flicks the bulbs it catches.
void Lights::Engine::Brush(float dt, const LightsPointer& p) {
    if (!p.valid || !params.mouse || edit.active || calm || !(dt > 0.0f)) return;
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.vx) || !std::isfinite(p.vy)) return;
    const float s = layout.dpiScale;
    const float speed = std::hypot(p.vx, p.vy);
    if (speed < kBrushMin * s) return;
    const float reach = kReach * s;
    const float push = ApproachFactor(dt, kBrushTau);
    const float cap = kBrushMax * s;
    const float vx = Clamp(p.vx, -2000.0f * s, 2000.0f * s), vy = Clamp(p.vy, -2000.0f * s, 2000.0f * s);
    const BulbSpec& spec = TheSpec();
    touch.assign(spans.size(), 0.0f);
    touchW.assign(spans.size(), 0.0f);
    for (const Strand& st : strands) {
        const float pad =
            reach + 2.0f * spec.length * Size() * s + kMaxSwing * 0.5f * (st.bounds.bottom - st.bounds.top);
        if (p.x < st.bounds.left - pad || p.x > st.bounds.right + pad || p.y < st.bounds.top - pad ||
            p.y > st.bounds.bottom + pad)
            continue;
        // The wire.
        for (int k = 0; k < st.points; ++k) {
            const size_t i = static_cast<size_t>(st.firstPoint + k);
            const Span& sp = spans[static_cast<size_t>(pointSpan[i])];
            const float x = rest[i].x + pointW[i] * sp.dx, y = rest[i].y + pointW[i] * sp.dy;
            const float d2 = (p.x - x) * (p.x - x) + (p.y - y) * (p.y - y);
            if (d2 > reach * reach) continue;
            const float close = 1.0f - SmoothStep(0.4f * reach, reach, std::sqrt(d2));
            const size_t sk = static_cast<size_t>(pointSpan[i]);
            if (close > touch[sk]) {
                touch[sk] = close;
                touchW[sk] = pointW[i];
            }
        }
        // The bulbs: caught along their length, their tips take up the pointer's motion.
        for (int k = 0; k < st.bulbs; ++k) {
            Bulb& b = bulbs[static_cast<size_t>(st.firstBulb + k)];
            const float len = spec.length * b.scale;
            const float qx = p.x - b.x, qy = p.y - b.y;
            if (qx * qx + qy * qy > (len + reach) * (len + reach)) continue;
            const float c = std::cos(b.angle), sn = std::sin(b.angle);
            const float ex = -sn, ey = c;  // along the bulb, socket to tip
            const float along = Clamp(qx * ex + qy * ey, 0.0f, len);
            const float d = std::hypot(qx - along * ex, qy - along * ey);
            const float radius = reach + 0.5f * spec.width * b.scale;
            if (d > radius) continue;
            const float close = 1.0f - SmoothStep(0.5f * radius, radius, d);
            const float lever = std::max(along, 0.4f * len);
            const float want = Clamp(-kKick * (vx * c + vy * sn) / lever, -kMaxDangleV, kMaxDangleV);
            b.swingV += (want - b.swingV) * push * close;
            const size_t sk = static_cast<size_t>(b.span);
            if (close > touch[sk]) {
                touch[sk] = close;
                touchW[sk] = b.w;
            }
        }
    }
    // A span touched where it swings little is pushed harder there, and the
    // pointer brushing it sideways drags it along; across, it only stops it.
    for (size_t k = 0; k < spans.size(); ++k) {
        if (touch[k] <= 0.0f) continue;
        Span& sp = spans[k];
        if (sp.first == sp.last) continue;
        const float belly = Clamp(kBrushDrag * vx / std::max(touchW[k], 0.35f), -cap, cap);
        sp.thetaV += (belly / sp.len - sp.thetaV) * push * touch[k];
    }
}

// The spans' swing and the bulbs' dangle, in small steps.
void Lights::Engine::Sway(float dt) {
    if (!(dt > 0.0f)) return;
    const int sub = std::max(1, static_cast<int>(std::ceil(dt * kSubstepHz)));
    const float h = dt / static_cast<float>(sub);
    const float zeta = calm ? 1.0f : kSwayZeta;
    const float gain = calm ? 0.0f : kDangleGain;
    for (int n = 0; n < sub; ++n) {
        for (const Strand& st : strands) {
            for (int k = 0; k < st.spans; ++k) {
                Span& sp = spans[static_cast<size_t>(st.firstSpan + k)];
                if (sp.first == sp.last) continue;
                // Neighbours tug through the hook they share.
                float couple = 0.0f;
                if (k > 0) {
                    const Span& o = spans[static_cast<size_t>(st.firstSpan + k - 1)];
                    if (o.first != o.last) couple += o.theta - sp.theta;
                }
                if (k + 1 < st.spans) {
                    const Span& o = spans[static_cast<size_t>(st.firstSpan + k + 1)];
                    if (o.first != o.last) couple += o.theta - sp.theta;
                }
                const float w2 = sp.omega * sp.omega;
                const float acc =
                    -w2 * (sp.theta - sp.rest - kSwayCouple * couple) - 2.0f * zeta * sp.omega * sp.thetaV;
                sp.thetaV += acc * h;
                sp.theta += sp.thetaV * h;
                if (std::fabs(sp.theta) > kMaxSwing) {
                    sp.theta = Clamp(sp.theta, -kMaxSwing, kMaxSwing);
                    if (sp.theta * sp.thetaV > 0.0f) sp.thetaV = 0.0f;
                }
                // The belly on its arc: sideways, rising as it goes out.
                const float c = std::cos(sp.theta), sn = std::sin(sp.theta), v2 = sp.thetaV * sp.thetaV;
                sp.ax = sp.len * (c * acc - sn * v2);
                sp.ay = -sp.rise * sp.len * (sn * acc + c * v2);
            }
        }
        // Each bulb is pushed about by the wire moving under its socket.
        for (Bulb& b : bulbs) {
            const Span& sp = spans[static_cast<size_t>(b.span)];
            const float drive = gain * b.w * (sp.ax * b.cr + sp.ay * b.sr) / b.arm;
            const float z = calm ? 1.0f : b.zeta;
            const float acc = -b.omega * b.omega * b.swing - 2.0f * z * b.omega * b.swingV + drive;
            b.swingV = Clamp(b.swingV + acc * h, -kMaxDangleV, kMaxDangleV);
            b.swing += b.swingV * h;
            if (std::fabs(b.swing) > kMaxDangle) {
                b.swing = Clamp(b.swing, -kMaxDangle, kMaxDangle);
                if (b.swing * b.swingV > 0.0f) b.swingV = 0.0f;
            }
        }
    }
    for (Span& sp : spans) {
        sp.dx = sp.len * std::sin(sp.theta);
        sp.dy = -sp.rise * sp.len * (1.0f - std::cos(sp.theta));
    }
}

// ---------------------------------------------------------------------------
// Light
// ---------------------------------------------------------------------------

// How bright a bulb should be now under animation `a`, 0..1. Every pattern is
// smooth (cross-fades, eased edges), so even an LED never pops.
float Lights::Engine::Target(LightAnimation a, const Bulb& b) const {
    switch (a) {
    case LightAnimation::Twinkle: {
        // Each bulb's own slots, offset so they never line up; now and then a
        // bulb drops out for part of one, well inside it.
        const double x = clocks.twinkle + static_cast<double>(b.phase);
        const double slot = std::floor(x);
        const float f = static_cast<float>(x - slot);
        const uint32_t h = Hash32(b.seed ^ Hash32(static_cast<uint32_t>(static_cast<int64_t>(slot)) * 0x2C1B3C6Du));
        if (HashFloat(h) >= kTwinkleOdds) return 1.0f;
        const float start = 0.06f + 0.34f * HashFloat(h + 11u);
        const float end = start + 0.26f + 0.29f * HashFloat(h + 12u);
        const float depth = 0.85f + 0.15f * HashFloat(h + 13u);
        return 1.0f - depth * SmoothStep(start, start + 0.1f, f) * (1.0f - SmoothStep(end - 0.16f, end, f));
    }
    case LightAnimation::Pulse:
        return pulse;
    case LightAnimation::Chase: {
        // Sets of three: each lit for one step in three, handing over to the next.
        const double q = clocks.chase - static_cast<double>(b.index % 3);
        float f = static_cast<float>(q - 3.0 * std::floor(q / 3.0));
        if (f > 2.5f) f -= 3.0f;
        const float e = 0.5f * kChaseFade;
        return SmoothStep(-e, e, f) * (1.0f - SmoothStep(1.0f - e, 1.0f + e, f));
    }
    case LightAnimation::Alternate: {
        float f = Frac(clocks.alternate + 0.5 * static_cast<double>(b.index % 2));
        if (f > 0.75f) f -= 1.0f;
        const float e = 0.5f * kAlternateFade;
        return SmoothStep(-e, e, f) * (1.0f - SmoothStep(0.5f - e, 0.5f + e, f));
    }
    case LightAnimation::Waves: {
        const float e = 0.5f + 0.5f * (waveCos * b.waveC + waveSin * b.waveS);
        return kWaveFloor + (1.0f - kWaveFloor) * e * std::sqrt(std::max(e, 0.0f));
    }
    case LightAnimation::Combination: {
        // Like an eight-function controller: each pattern in turn, cross-fading.
        static constexpr LightAnimation kRun[] = {LightAnimation::Twinkle, LightAnimation::Waves,
                                                  LightAnimation::Chase,   LightAnimation::Pulse,
                                                  LightAnimation::Alternate, LightAnimation::Steady};
        constexpr int n = static_cast<int>(std::size(kRun));
        const double at = clocks.program / kProgram;
        const int i = static_cast<int>(static_cast<int64_t>(std::floor(at)) % n);
        const float into = Frac(at) * kProgram;
        const float v = Target(kRun[i], b);
        if (into < kProgram - kProgramFade) return v;
        return Lerp(v, Target(kRun[(i + 1) % n], b), Ease((into - (kProgram - kProgramFade)) / kProgramFade));
    }
    default:
        return 1.0f;
    }
}

void Lights::Engine::Levels(float dt) {
    // The pattern clocks run at the Speed setting, each capped so nothing flashes.
    const float m = std::exp2(static_cast<float>(Clamp(params.speed, 0, 100) - 50) / 40.0f);
    clocks.twinkle += dt * std::min(m / kTwinkleSlot, kMaxFlashHz);
    clocks.pulse += dt * std::min(m / kPulsePeriod, kMaxFlashHz);
    clocks.chase += dt * std::min(kChaseRate * m, 3.0f * kMaxFlashHz);
    clocks.alternate += dt * std::min(m / kAlternatePeriod, kMaxFlashHz);
    clocks.waves += dt * std::min(m / kWavePeriod, kMaxFlashHz);
    clocks.program += dt;
    // Breathing: a slow rise, a fuller peak, a long ember.
    const float e = std::exp(std::sin(kTau * Frac(clocks.pulse)));
    pulse = kPulseFloor + (1.0f - kPulseFloor) * (e - 0.367879f) / (2.718282f - 0.367879f);
    waveCos = std::cos(kTau * Frac(clocks.waves));
    waveSin = std::sin(kTau * Frac(clocks.waves));

    // A new animation (or animation effects turned off) takes over gently.
    const LightAnimation next = calm ? LightAnimation::Steady : params.animation;
    if (anim == LightAnimation::Count) anim = next;
    if (next != anim) {
        from = blend < 0.5f ? from : anim;
        anim = next;
        blend = 0.0f;
    }
    blend = std::min(1.0f, blend + dt / kSwitchFade);

    const BulbSpec& spec = TheSpec();
    const bool shown = Shown();
    const float on = ApproachFactor(dt, spec.tauOn), off = ApproachFactor(dt, spec.tauOff);
    const float warm = ApproachFactor(dt, kBubbleWarm), cool = ApproachFactor(dt, kBubbleCool);
    const bool candle = params.bulb == BulbType::Candle, bubbly = params.bulb == BulbType::Bubble;
    for (Bulb& b : bulbs) {
        float want = 0.0f;
        if (shown) {
            want = Target(anim, b);
            if (blend < 1.0f) want = Lerp(Target(from, b), want, Ease(blend));
            want *= b.bright;
            // Switched on one by one (all at once without animation effects;
            // while hanging they are simply on).
            const float at = calm ? kPowerDelay : b.power;
            if (!edit.active) want *= SmoothStep(at, at + kPowerRamp, powerClock);
        }
        b.heat += (want - b.heat) * (want > b.heat ? on : off);
        b.heat = Saturate(b.heat);
        // A filament dims faster than it cools.
        b.level = spec.led ? b.heat : b.heat * std::sqrt(b.heat);
        b.flame = 0.0f;
        if (candle && !calm) {
            // A flame flickering, now and then guttering.
            const double t = now + 37.0 * static_cast<double>(HashFloat(b.seed + 6u));
            float f = 0.6f * noise::Noise1(Wrap256(t * 6.7)) + 0.4f * noise::Noise1(Wrap256(t * 10.3 + 91.0));
            const float gutter = SmoothStep(0.55f, 0.85f, noise::Noise1(Wrap256(t * 0.23 + 17.0)));
            b.flame = Clamp(1.3f * f * (1.0f + gutter) - 0.7f * gutter, -1.0f, 1.0f);
            b.level *= 1.0f - kCandleDip + kCandleDip * b.flame;
        }
        if (bubbly) {
            // The liquid warms over seconds; it bubbles once it is hot and slows as it cools.
            b.liquid += (b.level - b.liquid) * (b.level > b.liquid ? warm : cool);
            if (!calm) b.bubble += static_cast<double>(SmoothStep(0.35f, 0.8f, b.liquid) * dt);
            // (A second late, so the look never takes the start over for bubbling just beginning.)
            if (b.bubble >= kBubbleWrap + 1.0) b.bubble -= kBubbleWrap;
        }
    }
}

// ---------------------------------------------------------------------------
// Windows over the bulbs
// ---------------------------------------------------------------------------

void Lights::Engine::Cover(float dt) {
    const float out = dt / kCoverOut, in = dt / kCoverIn, appear = dt / kNewBulbFade;
    for (Bulb& b : bulbs) {
        // While hanging lights nothing hides them.
        const bool hide = b.covered && !edit.active;
        b.vis = hide ? std::max(0.0f, b.vis - out) : std::min(1.0f, b.vis + in);
        b.appear = std::min(1.0f, b.appear + appear);
    }
}

// Bulbs not looked at yet first (they take the answer at once), then the rest
// in turn, a few each call.
void Lights::Engine::Probe(const std::function<bool(float, float)>& covered) {
    if (edit.active || bulbs.empty() || (!Shown() && fade <= 0.0f)) return;
    const float gc = TheSpec().glassCenter;
    auto look = [&](Bulb& b) {
        const float r = gc * b.scale;
        b.covered = covered(b.x - std::sin(b.angle) * r, b.y + std::cos(b.angle) * r);
    };
    int budget = kProbeBudget;
    const size_t n = bulbs.size();
    if (unprobed > 0) {
        for (size_t i = 0; i < n && budget > 0; ++i) {
            Bulb& b = bulbs[(probeNext + i) % n];
            if (b.probed) continue;
            look(b);
            b.vis = b.covered ? 0.0f : 1.0f;
            b.probed = true;
            --unprobed;
            --budget;
        }
        return;
    }
    // Each bulb at most once a call.
    for (size_t i = 0; i < std::min(static_cast<size_t>(budget), n); ++i) {
        probeNext %= n;
        look(bulbs[probeNext++]);
    }
}

void Lights::Engine::Recheck() {
    for (Bulb& b : bulbs) b.probed = false;
    unprobed = static_cast<int>(bulbs.size());
}

// ---------------------------------------------------------------------------
// The frame
// ---------------------------------------------------------------------------

void Lights::Engine::Fill() {
    const BulbSpec& spec = TheSpec();
    const float s = layout.dpiScale;
    const float shown = Ease(Saturate(fade));
    coverAlpha.resize(bulbs.size());
    for (size_t i = 0; i < bulbs.size(); ++i) {
        Bulb& b = bulbs[i];
        const Span& sp = spans[static_cast<size_t>(b.span)];
        b.x = b.restX + b.w * sp.dx;
        b.y = b.restY + b.w * sp.dy;
        b.angle = Clamp(b.rest + b.tiltX * sp.dx + b.tiltY * sp.dy + b.swing, -kMaxTilt, kMaxTilt);
        coverAlpha[i] = Ease(b.vis);
    }
    frame.wires.resize(strands.size());
    for (size_t k = 0; k < strands.size(); ++k) {
        const Strand& st = strands[k];
        WireDraw& w = frame.wires[k];
        w.pts.resize(static_cast<size_t>(st.points));
        w.alpha.resize(static_cast<size_t>(st.points));
        w.width = std::max(1.0f, spec.wire * s * std::sqrt(Size()));
        w.strand = st.source;
        for (int j = 0; j < st.points; ++j) {
            const size_t i = static_cast<size_t>(st.firstPoint + j);
            const Span& sp = spans[static_cast<size_t>(pointSpan[i])];
            w.pts[static_cast<size_t>(j)] = {rest[i].x + pointW[i] * sp.dx, rest[i].y + pointW[i] * sp.dy};
            float a = 1.0f;
            if (const int b = pointBulb[i]; b >= 0) {
                const size_t b0 = static_cast<size_t>(b);
                const size_t b1 = std::min(b0 + 1, static_cast<size_t>(st.firstBulb + st.bulbs - 1));
                a = Lerp(coverAlpha[b0], coverAlpha[b1], pointT[i]);
            }
            w.alpha[static_cast<size_t>(j)] = shown * a;
        }
    }
    frame.bulbs.resize(bulbs.size());
    for (size_t k = 0; k < order.size(); ++k) {
        const Bulb& b = bulbs[static_cast<size_t>(order[k])];
        BulbDraw& d = frame.bulbs[k];
        d.x = b.x;
        d.y = b.y;
        d.angle = b.angle;
        d.scale = b.scale;
        d.color = b.color;
        d.level = b.level;
        d.heat = spec.led ? b.level : b.heat;
        d.alpha = shown * coverAlpha[static_cast<size_t>(order[k])] * Ease(b.appear);
        d.anim = params.bulb == BulbType::Candle   ? b.flame
                 : params.bulb == BulbType::Bubble ? static_cast<float>(b.bubble)
                                                   : 0.0f;
        d.seed = b.seed;
    }

    // Hanging mode.
    frame.editing = edit.active;
    frame.veil = veil;
    frame.hooks.clear();
    frame.cursorValid = false;
    frame.placing = false;
    frame.hoverStrand = -1;
    frame.cursorSnap = 0;
    if (!edit.active) return;
    const float W = static_cast<float>(layout.width), H = static_cast<float>(layout.height);
    const float e = kSnapEdge * s;
    auto snapOf = [&](float x, float y) {
        return (x <= e ? 1 : 0) | (y <= e ? 2 : 0) | (x >= W - e ? 4 : 0) | (y >= H - e ? 8 : 0);
    };
    for (size_t si = 0; si < edit.strands.size(); ++si) {
        const std::vector<LightHook>& hk = edit.strands[si].hooks;
        for (size_t hi = 0; hi < hk.size(); ++hi) {
            if (!std::isfinite(hk[hi].u) || !std::isfinite(hk[hi].v)) continue;
            HookDraw h;
            h.x = hk[hi].u * W;
            h.y = hk[hi].v * H;
            h.strand = static_cast<int>(si);
            h.hook = static_cast<int>(hi);
            h.hovered = edit.hoverStrand == h.strand && edit.hoverHook == h.hook;
            h.end = edit.placing == h.strand && hi + 1 == hk.size();
            h.snap = snapOf(h.x, h.y);
            frame.hooks.push_back(h);
        }
    }
    frame.placing = edit.placing >= 0 && edit.placing < static_cast<int>(edit.strands.size());
    frame.hoverStrand = edit.hoverStrand;
    frame.cursorValid = edit.cursorValid && std::isfinite(edit.cursor.u) && std::isfinite(edit.cursor.v);
    if (frame.cursorValid) frame.cursor = {edit.cursor.u * W, edit.cursor.v * H};
    frame.cursorSnap = edit.snap;
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

void Lights::Engine::Update(float dt, double time, float windPx, const LightsPointer& pointer) {
    if (!(dt > 0.0f)) dt = 0.0f;
    dt = std::min(dt, 0.1f);
    if (std::isfinite(time)) now = time;
    // People who turn off Windows' animation effects get steady lights that don't sway.
    if ((calmCheck -= dt) <= 0.0f) {
        calmCheck = 3.0f;
        BOOL animate = TRUE;
        calm = SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0) && !animate;
#ifdef SNOWYSTARR_DEV_TOOLS
        static const bool calmLab = [] {
            wchar_t v[4] = {};
            return GetEnvironmentVariableW(L"SNOWYSTARR_LIGHTS_CALM", v, 4) == 1 && v[0] == L'1';
        }();
        calm = calm || calmLab;
#endif
    }
    const bool shown = Shown();
    veil = edit.active ? std::min(kVeil, veil + dt * kVeil / kVeilFade) : std::max(0.0f, veil - dt * kVeil / kVeilFade);
    if (!shown && fade <= 0.0f) {
        // Hidden: nothing to do, and they come on afresh when shown.
        frame.wires.clear();
        frame.bulbs.clear();
        frame.hooks.clear();
        frame.editing = false;
        frame.veil = veil;
        frame.cursorValid = frame.placing = false;
        if (!fresh) {
            fresh = dirty = true;
            spread = kPowerSpread;
        }
        windInit = false;
        return;
    }
    if (fade <= 0.0f) {
        // Appearing: they come on afresh, and windows over them are looked for at once.
        fresh = dirty = true;
        spread = kPowerSpread;
    }
    if (holding && now - editEnded > kEditGrace) {
        holding = false;
        dirty = true;
    }
    if (dirty) Layout();
    fade = shown ? std::min(1.0f, fade + dt / kShowFade) : std::max(0.0f, fade - dt / kHideFade);
    powerClock = std::min(powerClock + dt, 60.0f);
    Wind(dt, windPx);
    Brush(dt, pointer);
    Sway(dt);
    Levels(dt);
    Cover(dt);
    Fill();
}

// ---------------------------------------------------------------------------
// Lights (engine side)
// ---------------------------------------------------------------------------

Lights::Lights() : engine_(std::make_unique<Engine>()), look_(lights::NewLook()) {}
Lights::~Lights() = default;

void Lights::Configure(const LightsLayout& layout) {
    Engine& e = *engine_;
    if (layout == e.layout) return;
    e.layout = layout;
    e.top = AcrossTop(L"", layout.width, layout.height, layout.dpiScale);
    e.dirty = true;
    LookSetParams(*look_, e.params, layout);
}

void Lights::SetParams(const LightParams& params) {
    Engine& e = *engine_;
    if (params == e.params) return;
    const bool bulb = params.bulb != e.params.bulb;
    const bool strands =
        params.acrossTop != e.params.acrossTop || (!params.acrossTop && params.strands != e.params.strands);
    if (bulb || strands || params.size != e.params.size) e.dirty = true;
    // New bulbs come on quickly; new strands (not the ones just hung) come on one by one.
    if (bulb && !e.fresh) {
        e.fresh = true;
        e.spread = kQuickSpread;
    }
    if (strands && !e.edit.active && e.now - e.editEnded > kEditGrace) {
        e.fresh = true;
        e.spread = kPowerSpread;
    }
    if (strands) e.holding = false;
    e.params = params;
    LookSetParams(*look_, params, e.layout);
}

void Lights::SetEdit(const LightsEdit& edit) {
    Engine& e = *engine_;
    if (edit == e.edit) return;
    const LightsEdit& o = e.edit;
    // Only what changes the strands lays them out again (not what the pointer is over).
    const bool live = edit.cursorValid && edit.placing >= 0;
    if (edit.active != o.active || edit.strands != o.strands || edit.placing != o.placing ||
        edit.cursorValid != o.cursorValid || (live && (edit.cursor != o.cursor || edit.placingSlack != o.placingSlack)))
        e.dirty = true;
    if (o.active && !edit.active) {
        // Done hanging: what was hung stays up until the settings with it
        // arrive (they may come a frame or two later), and the windows over
        // the bulbs are looked at afresh.
        e.editEnded = e.now;
        e.held = o.strands;
        e.holding = true;
        e.Recheck();
    }
    if (edit.active) e.holding = false;
    e.edit = edit;
}

bool Lights::Active() const {
    const Engine& e = *engine_;
    if (e.layout.width <= 0 || e.layout.height <= 0) return false;
    if (e.edit.active || e.veil > 0.0f) return true;
    if (!e.params.show && e.fade <= 0.0f) return false;
    return e.dirty || !e.strands.empty();
}

bool Lights::Editing() const { return engine_->edit.active || engine_->veil > 0.0f; }

void Lights::Update(float dt, double time, float windPx, const LightsPointer& pointer) {
    engine_->Update(dt, time, windPx, pointer);
}

void Lights::ProbeCover(const std::function<bool(float, float)>& covered) { engine_->Probe(covered); }

bool Lights::CoverStale() const {
    const Engine& e = *engine_;
    return e.unprobed > 0 && !e.edit.active && (e.Shown() || e.fade > 0.0f);
}

void Lights::RecheckCover() { engine_->Recheck(); }

// ---------------------------------------------------------------------------
// Shapes
// ---------------------------------------------------------------------------

// A span hangs as a catenary of its length through both hooks (lowest nearer
// the lower hook); a steep one as a bow square to its chord, bulging the way
// gravity pulls it (into the screen, on a vertical run), so a side run never
// hangs in a "J". Both are taken at the same steps along the chord, so between
// the two the shape blends smoothly.
void Lights::SpanShape(D2D1_POINT_2F a, D2D1_POINT_2F b, float slack, float dpiScale,
                       std::vector<D2D1_POINT_2F>& out, float screenWidth) {
    if (!Finite(a) || !Finite(b)) return;
    const double s = std::isfinite(dpiScale) ? Clamp(static_cast<double>(dpiScale), 0.25, 16.0) : 1.0;
    const double k = std::isfinite(slack) ? Clamp(static_cast<double>(slack), 0.0, static_cast<double>(kMaxSlack))
                                          : static_cast<double>(kDefaultSlack);
    out.push_back(a);
    const double chord = std::hypot(static_cast<double>(b.x) - a.x, static_cast<double>(b.y) - a.y);
    if (!std::isfinite(chord) || chord < kWireStep) {
        out.push_back(b);
        return;
    }
    // Measured finely first, so the points can go at even steps along the
    // wire (where it hangs steeply as closely as where it is level).
    static thread_local std::vector<D2D1_POINT_2F> fine;  // kept, to save allocating
    // Into the screen is towards its middle (without its width, a vertical run
    // near the left edge is taken to be on the left).
    const double midLine = std::isfinite(screenWidth) && screenWidth > 0.0f ? 0.5 * screenWidth : kLeftSide * s;
    const Hang hang(a, b, k, midLine);
    const int steps = kFine * Clamp(static_cast<int>(std::ceil(chord * (1.0 + k) / kWireStep)), 1, kMaxSpanPoints);
    auto dist = [](D2D1_POINT_2F p, D2D1_POINT_2F q) {
        return std::hypot(static_cast<double>(q.x) - p.x, static_cast<double>(q.y) - p.y);
    };
    fine.resize(static_cast<size_t>(steps) + 1);
    fine[0] = a;
    double length = 0.0;
    for (int i = 1; i <= steps; ++i) {
        fine[static_cast<size_t>(i)] = i < steps ? hang.At(static_cast<double>(i) / steps) : b;
        length += dist(fine[static_cast<size_t>(i) - 1], fine[static_cast<size_t>(i)]);
    }
    const int n = Clamp(static_cast<int>(std::ceil(length / kWireStep)), 1, kMaxSpanPoints - 1);
    const double each = length / n;
    out.reserve(out.size() + static_cast<size_t>(n));
    double walked = 0.0, next = each;
    int placed = 0;
    for (int i = 1; i <= steps && placed < n - 1; ++i) {
        const D2D1_POINT_2F p = fine[static_cast<size_t>(i) - 1], q = fine[static_cast<size_t>(i)];
        const double d = dist(p, q);
        while (d > 0.0 && walked + d >= next && placed < n - 1) {
            const float t = static_cast<float>((next - walked) / d);
            out.push_back({Lerp(p.x, q.x, t), Lerp(p.y, q.y, t)});
            next += each;
            ++placed;
        }
        walked += d;
    }
    out.push_back(b);
}

std::vector<LightStrand> Lights::AcrossTop(const std::wstring& monitor, int width, int height, float dpiScale) {
    std::vector<LightStrand> out;
    if (width <= 0 || height <= 0 || !(dpiScale > 0.0f) || !std::isfinite(dpiScale)) return out;
    const float wDip = static_cast<float>(width) / dpiScale, hDip = static_cast<float>(height) / dpiScale;
    const float wide = std::min(wDip / kScallop, 1000.0f);
    const float deep = std::min(wDip * kTopSag / (kTopDepth * hDip), 1000.0f);
    const float most = std::min(wDip / kMinScallop, 1000.0f);
    const int n = Clamp(std::min(std::max(static_cast<int>(std::lround(wide)), static_cast<int>(std::ceil(deep))),
                                 static_cast<int>(most)),
                        1, kMaxHooks - 1);
    LightStrand s;
    s.monitor = monitor;
    // A little irregular, as a strand hung by hand: hooks a touch off even, each
    // scallop with its own slack. The same monitor size always gives the same drape.
    const uint32_t seed = Hash32(static_cast<uint32_t>(width) * 31u + static_cast<uint32_t>(height));
    for (int i = 0; i <= n; ++i) {
        float u = static_cast<float>(i) / static_cast<float>(n);
        if (i > 0 && i < n) u += (HashFloat(seed + static_cast<uint32_t>(i)) - 0.5f) * 0.06f / static_cast<float>(n);
        const float drop = (i > 0 && i < n) ? 2.5f * HashFloat(seed ^ (0x9E37u * static_cast<uint32_t>(i + 1))) : 0.0f;
        s.hooks.push_back({Saturate(u), (kTopInset + drop) / hDip});
    }
    for (int i = 0; i < n; ++i) {
        const float sag = kTopSag * (1.0f + 0.12f * (HashFloat(seed * 3u + static_cast<uint32_t>(i)) - 0.5f));
        s.slack.push_back(8.0f / 3.0f * sag * sag);  // a shallow catenary: L = c (1 + 8/3 (sag/c)^2)
    }
    out.push_back(std::move(s));
    return out;
}
