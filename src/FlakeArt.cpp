#include "FlakeArt.h"

#include "resource.h"

#include <array>
#include <deque>
#include <initializer_list>
#include <utility>

namespace {

// ---------------------------------------------------------------------------
// Per-kind tables
// ---------------------------------------------------------------------------

constexpr FlakeTraits kTraits[kFlakeKindCount] = {
    // minDip maxDip rotates twinkles pixelArt
    {2.0f, 4.5f, false, false, false},   // Dot
    {4.0f, 11.0f, false, false, false},  // Soft
    {5.0f, 9.0f, true, false, false},    // Plus
    {6.0f, 11.0f, true, false, false},   // Asterisk
    {6.0f, 13.0f, true, true, false},    // Sparkle
    {10.0f, 24.0f, true, false, false},  // Dendrite
    {9.0f, 20.0f, true, false, false},   // Sectored
    {11.0f, 25.0f, true, false, false},  // Fern
    {8.0f, 17.0f, true, false, false},   // HexPlate
    {10.0f, 22.0f, true, false, false},  // StellarPlate
    {9.0f, 20.0f, true, false, false},   // Needle
    {12.0f, 26.0f, true, false, false},  // Twelve
    {15.0f, 15.0f, false, false, true},  // Classic1..8
    {15.0f, 15.0f, false, false, true},
    {15.0f, 15.0f, false, false, true},
    {15.0f, 15.0f, false, false, true},
    {15.0f, 15.0f, false, false, true},
    {15.0f, 15.0f, false, false, true},
    {15.0f, 15.0f, false, false, true},
    {15.0f, 15.0f, false, false, true},
};

constexpr int kVariantCounts[kFlakeKindCount] = {
    1, 2, 1, 2, 2,           // dot, soft, plus, asterisk, sparkle
    4, 4, 4, 4, 4, 4, 4,     // crystals
    1, 1, 1, 1, 1, 1, 1, 1,  // classics
};

constexpr bool VariantsFit() {
    for (int n : kVariantCounts)
        if (n < 1 || n > kMaxVariants) return false;
    return true;
}
static_assert(VariantsFit(), "every kind needs 1..kMaxVariants variants");

constexpr std::array<int, kFlakeKindCount + 1> MakeSpriteOffsets() {
    std::array<int, kFlakeKindCount + 1> o{};
    for (int k = 0; k < kFlakeKindCount; ++k) o[k + 1] = o[k] + kVariantCounts[k];
    return o;
}
constexpr std::array<int, kFlakeKindCount + 1> kSpriteOffset = MakeSpriteOffsets();
constexpr int kSpriteTotal = kSpriteOffset[kFlakeKindCount];
constexpr int kFirstClassicSprite = kSpriteOffset[static_cast<int>(FlakeKind::Classic1)];

constexpr bool IsClassic(FlakeKind k) { return k >= FlakeKind::Classic1 && k <= FlakeKind::Classic8; }

constexpr int kClassicCount = static_cast<int>(FlakeKind::Classic8) - static_cast<int>(FlakeKind::Classic1) + 1;
static_assert(kClassicCount == 8 && IDR_CLASSIC8 - IDR_CLASSIC_FIRST == 7, "classic resources out of sync");
static_assert(kFirstClassicSprite + kClassicCount == kSpriteTotal, "classic sprites must come last");

// ---------------------------------------------------------------------------
// Halo and atlas tuning
// ---------------------------------------------------------------------------

constexpr float kHaloAlpha = 0.4f;  // peak opacity of the dark halo next to the white

// How far the halo reaches (device px) around a flake of radius rpx.
float HaloPx(float rpx) { return Clamp(0.085f * rpx + 0.85f, 1.25f, 5.5f); }

// Atlas levels: the procedural flake diameter shrinks by sqrt(2) per level, so a
// sprite is never minified by more than that. Classic flakes use whole-pixel scales.
constexpr int kLevelCount = 13;          // 128 px down to 2 px
constexpr float kTopDiameter = 128.0f;
constexpr int kCellPad = 3;              // per side: 1 px antialiasing + 2 px fully transparent border
constexpr int kClearBorder = 2;
constexpr UINT32 kPageWidth = 1024;
constexpr UINT32 kMaxPageHeight = 4096;

float LevelDiameter(int level) { return kTopDiameter * std::exp2(-0.5f * static_cast<float>(level)); }

// Device px per source pixel of a classic flake at a level: whole numbers when enlarging.
float ClassicScale(int level) {
    float k = LevelDiameter(level) / 15.0f;
    return k >= 1.0f ? std::floor(k) : k;
}

// ---------------------------------------------------------------------------
// Geometry helpers. Designs are built in a unit-radius space with arm 0
// pointing up; the sketch scales everything by the requested radius.
// ---------------------------------------------------------------------------

struct Pt {
    float x, y;
};
constexpr Pt operator+(Pt a, Pt b) { return {a.x + b.x, a.y + b.y}; }
constexpr Pt operator-(Pt a, Pt b) { return {a.x - b.x, a.y - b.y}; }
constexpr Pt operator*(Pt a, float s) { return {a.x * s, a.y * s}; }
inline Pt Polar(float radius, float angle) { return {radius * std::cos(angle), radius * std::sin(angle)}; }

constexpr float kSin60 = 0.8660254f;
constexpr float kCos30 = kSin60;
constexpr float kUp = -kPi / 2.0f;    // arm 0 points up
constexpr float kSixth = kPi / 3.0f;  // 60 degrees between arms

inline float ArmAngle(int k, float offset = 0.0f) { return kUp + offset + static_cast<float>(k) * kSixth; }

// Frame along one arm: t runs outwards from the centre, u across the arm.
class Arm {
public:
    explicit Arm(float angle) : c_(std::cos(angle)), s_(std::sin(angle)) {}
    Pt At(float t, float u = 0.0f) const { return {t * c_ - u * s_, t * s_ + u * c_}; }
    // End of a side branch that leaves the arm at t and grows outwards at 60 degrees to it
    // (side +1 or -1), parallel to the neighbouring arm as in real crystals.
    Pt Branch(float t, float len, float side) const { return At(t + 0.5f * len, side * kSin60 * len); }
    // A "V" of two side branches.
    std::vector<Pt> Vee(float t, float len) const { return {Branch(t, len, 1.0f), At(t), Branch(t, len, -1.0f)}; }

private:
    float c_, s_;
};

std::vector<Pt> Hexagon(float radius, float angle0 = kUp) {
    std::vector<Pt> pts;
    pts.reserve(6);
    for (int k = 0; k < 6; ++k) pts.push_back(Polar(radius, angle0 + static_cast<float>(k) * kSixth));
    return pts;
}

std::vector<Pt> Circle(Pt c, float radius, int n) {
    std::vector<Pt> pts;
    pts.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) pts.push_back(c + Polar(radius, static_cast<float>(i) * kTau / static_cast<float>(n)));
    return pts;
}

// Longest side branch at t (<= len) whose tip stays inside `limit` and clear of the
// neighbouring arm's branches (a branch as long as t reaches the 30 degree bisector).
float FitBranch(float t, float len, float limit) {
    len = std::min(len, 0.85f * t);
    // |tip|^2 = t^2 + t*len + len^2
    float maxLen = 0.5f * (-t + std::sqrt(std::max(0.0f, 4.0f * limit * limit - 3.0f * t * t)));
    return std::max(0.0f, std::min(len, maxLen));
}

// Detail level from the drawn radius: 0 = a few bold strokes ... 3 = everything.
int DetailLevel(float rpx, float l1, float l2, float l3) { return rpx < l1 ? 0 : rpx < l2 ? 1 : rpx < l3 ? 2 : 3; }

// ---------------------------------------------------------------------------
// Sketch: collects white strokes, fills and soft glows, then renders them over
// an optional dark halo.
// ---------------------------------------------------------------------------

enum class Ink { Stroke, Fill, Glow };

struct Figure {
    std::vector<Pt> pts;
    bool closed = false;
};

struct Layer {
    Ink ink = Ink::Stroke;
    float width = 0.0f;     // Stroke: line width
    float alpha = 1.0f;     // opacity of the white (graded Fill: at the unit circle)
    float alphaIn = -1.0f;  // Fill: >= 0 grades the opacity radially from this value at the centre
    float halo = 1.0f;      // halo strength, 0 = none
    bool haloFill = false;  // Fill: darken under the whole shape, not only around its outline
    std::vector<Figure> figures;
    // Glow: elliptical radial gradient of white.
    Pt center{};
    float rx = 0.0f, ry = 0.0f;
    float haloCore = 1.0f;  // share of the halo kept under the middle of the glow (0 = a ring)
    std::vector<std::pair<float, float>> stops;  // (position, alpha)
};

class Sketch {
public:
    Sketch(float r, float pxScale) : r_(r), px_(pxScale) {}

    float Rpx() const { return r_ * px_; }
    // Width `rel` (fraction of the radius), but never thinner than minPx device pixels.
    float W(float rel, float minPx) const { return std::max(rel, minPx / Rpx()); }

    Layer& Stroke(float width, float alpha = 1.0f, float halo = 1.0f) {
        Layer& l = layers_.emplace_back();
        l.width = width;
        l.alpha = alpha;
        l.halo = halo;
        return l;
    }
    Layer& Fill(float alpha, float halo = 1.0f, bool haloFill = false) {
        Layer& l = layers_.emplace_back();
        l.ink = Ink::Fill;
        l.alpha = alpha;
        l.halo = halo;
        l.haloFill = haloFill;
        return l;
    }
    // Translucent body whose opacity rises from alphaIn at the centre to alpha at the rim.
    Layer& GradedFill(float alphaIn, float alpha, float halo = 1.0f) {
        Layer& l = Fill(alpha, halo);
        l.alphaIn = alphaIn;
        return l;
    }
    void Glow(Pt c, float rx, float ry, std::initializer_list<std::pair<float, float>> stops, float halo = 0.0f,
              float haloCore = 1.0f) {
        Layer& l = layers_.emplace_back();
        l.ink = Ink::Glow;
        l.center = c;
        l.rx = rx;
        l.ry = ry;
        l.stops.assign(stops.begin(), stops.end());
        l.halo = halo;
        l.haloCore = haloCore;
    }

    static void Line(Layer& l, Pt a, Pt b) { l.figures.push_back({{a, b}, false}); }
    static void Path(Layer& l, std::vector<Pt> pts, bool closed = false) {
        l.figures.push_back({std::move(pts), closed});
    }

    // maxBlend (optional) is `rt` as a device context over a transparent backdrop;
    // it merges the halo passes so overlapping arms do not darken each other.
    void Render(ID2D1RenderTarget* rt, ID2D1DeviceContext* maxBlend, bool shadow) const;

private:
    ComPtr<ID2D1PathGeometry> MakeGeometry(ID2D1Factory* factory, const Layer& l) const;
    void FillGlow(ID2D1RenderTarget* rt, const Layer& l, bool halo, float grow) const;
    ComPtr<ID2D1Brush> FillBrush(ID2D1RenderTarget* rt, const Layer& l) const;

    float r_, px_;
    std::deque<Layer> layers_;  // deque: references stay valid while designs add layers
};

ComPtr<ID2D1PathGeometry> Sketch::MakeGeometry(ID2D1Factory* factory, const Layer& l) const {
    ComPtr<ID2D1PathGeometry> geo;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) return nullptr;
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    std::vector<D2D1_POINT_2F> pts;
    for (const Figure& f : l.figures) {
        if (f.pts.size() < 2) continue;
        pts.clear();
        for (Pt p : f.pts) pts.push_back({p.x * r_, p.y * r_});
        sink->BeginFigure(pts[0], l.ink == Ink::Fill ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLines(pts.data() + 1, static_cast<UINT32>(pts.size() - 1));
        sink->EndFigure(f.closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    }
    if (FAILED(sink->Close())) return nullptr;
    return geo;
}

ComPtr<ID2D1Brush> Sketch::FillBrush(ID2D1RenderTarget* rt, const Layer& l) const {
    ComPtr<ID2D1Brush> brush;
    if (l.alphaIn >= 0.0f) {
        D2D1_GRADIENT_STOP stops[] = {{0.0f, D2D1::ColorF(1, 1, 1, l.alphaIn)}, {1.0f, D2D1::ColorF(1, 1, 1, l.alpha)}};
        ComPtr<ID2D1GradientStopCollection> coll;
        ComPtr<ID2D1RadialGradientBrush> radial;
        if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 2, &coll)) &&
            SUCCEEDED(rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties({0, 0}, {0, 0}, r_, r_),
                                                    coll.Get(), &radial)))
            brush = radial;
    } else {
        ComPtr<ID2D1SolidColorBrush> solid;
        if (SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, l.alpha), &solid))) brush = solid;
    }
    return brush;
}

void Sketch::FillGlow(ID2D1RenderTarget* rt, const Layer& l, bool halo, float grow) const {
    D2D1_GRADIENT_STOP stops[8];
    UINT32 n = 0;
    const float rx = l.rx + grow, ry = l.ry + grow;
    if (halo) {
        // Dark under the glow's edge, fading out past it; haloCore keeps some under the middle.
        const float a = kHaloAlpha * l.halo;
        const float edge = l.rx / rx;  // where the white ends
        stops[n++] = {0.0f, D2D1::ColorF(0, 0, 0, a * l.haloCore)};
        stops[n++] = {0.5f * edge, D2D1::ColorF(0, 0, 0, a * l.haloCore)};
        stops[n++] = {0.85f * edge, D2D1::ColorF(0, 0, 0, a)};
        stops[n++] = {Lerp(edge, 1.0f, 0.4f), D2D1::ColorF(0, 0, 0, a * 0.4f)};
        stops[n++] = {1.0f, D2D1::ColorF(0, 0, 0, 0)};
    } else {
        for (const auto& s : l.stops) {
            if (n == 8) break;
            stops[n++] = {s.first, D2D1::ColorF(1, 1, 1, s.second)};
        }
    }
    ComPtr<ID2D1GradientStopCollection> coll;
    ComPtr<ID2D1RadialGradientBrush> brush;
    const D2D1_POINT_2F c = {l.center.x * r_, l.center.y * r_};
    if (FAILED(rt->CreateGradientStopCollection(stops, n, &coll)) ||
        FAILED(rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(c, {0, 0}, rx * r_, ry * r_),
                                             coll.Get(), &brush)))
        return;
    rt->FillEllipse(D2D1::Ellipse(c, rx * r_, ry * r_), brush.Get());
}

void Sketch::Render(ID2D1RenderTarget* rt, ID2D1DeviceContext* maxBlend, bool shadow) const {
    ComPtr<ID2D1Factory> factory;
    rt->GetFactory(&factory);
    ComPtr<ID2D1StrokeStyle> round;
    if (!factory || FAILED(factory->CreateStrokeStyle(
                        D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                    D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                        nullptr, 0, &round)))
        return;
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush))) return;

    std::vector<ComPtr<ID2D1PathGeometry>> geo(layers_.size());
    for (size_t i = 0; i < layers_.size(); ++i)
        if (layers_[i].ink != Ink::Glow) geo[i] = MakeGeometry(factory.Get(), layers_[i]);

    if (shadow) {
        // Several widened dark passes approximate a blurred silhouette.
        const float hPx = HaloPx(Rpx());
        const float h = hPx / Rpx();
        const int passes = Clamp(static_cast<int>(std::lround(hPx / 0.9f)), 2, 6);
        D2D1_PRIMITIVE_BLEND oldBlend = D2D1_PRIMITIVE_BLEND_SOURCE_OVER;
        if (maxBlend) {
            oldBlend = maxBlend->GetPrimitiveBlend();
            maxBlend->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_MAX);
        }
        for (int j = passes; j >= 1; --j) {
            const float d = h * static_cast<float>(j) / static_cast<float>(passes);
            float a = kHaloAlpha * std::pow(1.0f - static_cast<float>(j - 1) / static_cast<float>(passes), 1.6f);
            if (!maxBlend) a *= 0.5f;  // plain blending accumulates the passes
            for (size_t i = 0; i < layers_.size(); ++i) {
                const Layer& l = layers_[i];
                if (l.halo <= 0.0f) continue;
                if (l.ink == Ink::Glow) {
                    if (j == passes) FillGlow(rt, l, true, h);
                    continue;
                }
                if (!geo[i]) continue;
                brush->SetColor(D2D1::ColorF(0, 0, 0, a * l.halo));
                if (l.ink == Ink::Stroke) {
                    rt->DrawGeometry(geo[i].Get(), brush.Get(), (l.width + 2.0f * d) * r_, round.Get());
                } else {
                    rt->DrawGeometry(geo[i].Get(), brush.Get(), 2.0f * d * r_, round.Get());
                    if (l.haloFill) rt->FillGeometry(geo[i].Get(), brush.Get());
                }
            }
        }
        if (maxBlend) maxBlend->SetPrimitiveBlend(oldBlend);
    }

    for (size_t i = 0; i < layers_.size(); ++i) {
        const Layer& l = layers_[i];
        if (l.ink == Ink::Glow) {
            FillGlow(rt, l, false, 0.0f);
        } else if (geo[i] && l.ink == Ink::Stroke) {
            brush->SetColor(D2D1::ColorF(1, 1, 1, l.alpha));
            rt->DrawGeometry(geo[i].Get(), brush.Get(), l.width * r_, round.Get());
        } else if (geo[i]) {
            if (ComPtr<ID2D1Brush> fill = FillBrush(rt, l)) rt->FillGeometry(geo[i].Get(), fill.Get());
        }
    }
}

// ---------------------------------------------------------------------------
// Simple shapes
// ---------------------------------------------------------------------------

void DesignDot(Sketch& s) {
    // Tiny grains need a firmer core to register at all.
    if (s.Rpx() < 2.5f)
        s.Glow({0, 0}, 1.0f, 1.0f, {{0.0f, 1.0f}, {0.5f, 1.0f}, {0.8f, 0.6f}, {1.0f, 0.0f}}, 1.0f);
    else
        s.Glow({0, 0}, 1.0f, 1.0f, {{0.0f, 1.0f}, {0.4f, 0.97f}, {0.7f, 0.62f}, {1.0f, 0.0f}}, 1.0f);
}

void DesignSoft(Sketch& s, int variant, Rng& rng) {
    // The halo is a faint ring: under the translucent middle it would only turn the flake grey.
    if (variant == 0) {
        // Out-of-focus flake: a soft body with a few overlapping lobes so it is not a perfect disc.
        s.Glow({0, 0}, 0.97f, 0.97f, {{0.0f, 0.62f}, {0.45f, 0.5f}, {0.8f, 0.2f}, {1.0f, 0.0f}}, 0.6f, 0.0f);
        float a0 = rng.Range(0.0f, kTau);
        for (int i = 0; i < 3; ++i) {
            Pt c = Polar(rng.Range(0.16f, 0.26f), a0 + static_cast<float>(i) * kTau / 3.0f + rng.Range(-0.4f, 0.4f));
            float rr = rng.Range(0.5f, 0.62f);
            s.Glow(c, rr, rr * rng.Range(0.85f, 1.0f), {{0.0f, 0.3f}, {0.5f, 0.2f}, {1.0f, 0.0f}});
        }
    } else {
        // Bokeh disc: flat body, slightly brighter rim, soft edge.
        s.Glow({0, 0}, 1.0f, 0.94f, {{0.0f, 0.42f}, {0.55f, 0.46f}, {0.8f, 0.56f}, {0.9f, 0.4f}, {1.0f, 0.0f}},
               0.6f, 0.0f);
    }
}

void DesignPlus(Sketch& s) {
    float w = s.W(0.27f, 1.3f);
    float e = 1.0f - 0.5f * w;
    Layer& l = s.Stroke(w);
    Sketch::Line(l, {-e, 0}, {e, 0});
    Sketch::Line(l, {0, -e}, {0, e});
}

void DesignAsterisk(Sketch& s, int variant) {
    float w = s.W(variant == 0 ? 0.15f : 0.11f, 1.1f);
    float e = 1.0f - 0.5f * w;
    Layer& l = s.Stroke(w);
    for (int k = 0; k < 3; ++k) {
        Arm a(ArmAngle(k));
        Sketch::Line(l, a.At(-e), a.At(e));
    }
    if (variant == 1 && s.Rpx() >= 5.0f) {
        // Small beads near the tips.
        Layer& beads = s.Fill(1.0f, 1.0f, true);
        for (int k = 0; k < 6; ++k) Sketch::Path(beads, Circle(Arm(ArmAngle(k)).At(0.78f), 0.13f, 12), true);
    }
}

void DesignSparkle(Sketch& s, int variant) {
    const float rpx = s.Rpx();
    // Variant 0 doubles as the glint on the drift, where a dark rim would look like soot:
    // it keeps only a whisper of halo. Variant 1 carries the normal one.
    const float halo = variant == 0 ? 0.15f : 0.5f;
    s.Glow({0, 0}, 0.62f, 0.62f, {{0.0f, 0.42f}, {0.35f, 0.18f}, {1.0f, 0.0f}});
    // Long rays: a concave star whose waist keeps the rays at least ~1.4 px wide.
    auto star = [&](float len, float angle0, float alpha) {
        float waist = std::max(0.075f, 1.0f / rpx);
        std::vector<Pt> pts;
        for (int k = 0; k < 4; ++k) {
            float a = angle0 + static_cast<float>(k) * kPi * 0.5f;
            pts.push_back(Polar(len, a));
            pts.push_back(Polar(waist, a + kPi * 0.25f));
        }
        Sketch::Path(s.Fill(alpha, halo, true), pts, true);
    };
    star(1.0f, kUp, 1.0f);
    if (variant == 1) star(0.5f, kUp + kPi * 0.25f, 0.8f);
    s.Glow({0, 0}, 0.24f, 0.24f, {{0.0f, 1.0f}, {0.5f, 0.85f}, {1.0f, 0.0f}});
}

// ---------------------------------------------------------------------------
// Crystals
// ---------------------------------------------------------------------------

// Small hexagonal centre plate: translucent body and an optional bright rim.
void Hub(Sketch& s, float radius, float body, float rimRel) {
    std::vector<Pt> hex = Hexagon(radius);
    Sketch::Path(s.Fill(body, 0.0f), hex, true);
    if (rimRel > 0.0f) Sketch::Path(s.Stroke(s.W(rimRel, 0.8f), 0.95f), hex, true);
}

void DesignDendrite(Sketch& s, int variant, Rng& rng) {
    struct Branch {
        float t, len;
    };
    std::vector<Branch> br;
    float armRel = 0.062f, hub = 0.12f, tipT = 0.87f, tipLen = 0.08f, twig = 0.34f;
    if (variant == 0) {
        br = {{0.32f, 0.3f}, {0.53f, 0.25f}, {0.72f, 0.16f}};
    } else {
        int n = rng.Int(2, 4);
        float t0 = rng.Range(0.26f, 0.35f), t1 = rng.Range(0.64f, 0.77f);
        float lmax = rng.Range(0.22f, 0.32f), fall = rng.Range(0.3f, 0.65f);
        for (int i = 0; i < n; ++i) {
            float u = static_cast<float>(i) / static_cast<float>(n - 1);
            br.push_back({Lerp(t0, t1, u), lmax * (1.0f - fall * u) * rng.Range(0.9f, 1.08f)});
        }
        armRel = rng.Range(0.052f, 0.072f);
        hub = rng.Range(0.08f, 0.15f);
        tipT = rng.Range(0.84f, 0.9f);
        tipLen = rng.Chance(0.65f) ? rng.Range(0.05f, 0.09f) : 0.0f;
        twig = rng.Chance(0.6f) ? rng.Range(0.25f, 0.4f) : 0.0f;
    }

    // Small flakes stay a crisp six-armed star; one pair of branches mid-arm comes next.
    const int lod = DetailLevel(s.Rpx(), 5.5f, 9.0f, 17.0f);
    const float wa = s.W(armRel, lod == 0 ? 1.1f : 1.0f);
    const float wb = s.W(0.042f, 0.85f);
    const float wt = s.W(0.028f, 0.8f);

    Layer& arms = s.Stroke(wa);
    for (int k = 0; k < 6; ++k) Sketch::Line(arms, {0, 0}, Arm(ArmAngle(k)).At(1.0f - 0.5f * wa));
    if (lod == 0) return;
    if (lod == 1) {
        auto mid = std::min_element(br.begin(), br.end(), [](const Branch& a, const Branch& b) {
            return std::fabs(a.t - 0.5f) < std::fabs(b.t - 0.5f);
        });
        br = {*mid};
    }

    Layer& branches = s.Stroke(wb);
    Layer& twigs = s.Stroke(wt);
    const size_t nb = br.size();
    const float limit = 1.0f - 0.5f * wb;
    for (int k = 0; k < 6; ++k) {
        Arm a(ArmAngle(k));
        for (size_t i = 0; i < nb; ++i) {
            float len = FitBranch(br[i].t, br[i].len, limit);
            Sketch::Path(branches, a.Vee(br[i].t, len));
            if (lod >= 3 && twig > 0.0f && i < 2) {
                // Twigs on the inner side branches, parallel to the main arm.
                for (float side : {1.0f, -1.0f}) {
                    Pt base = a.Branch(br[i].t, 0.5f * len, side);
                    Sketch::Line(twigs, base, base + (a.At(1.0f) - a.At(0.0f)) * (twig * len));
                }
            }
        }
        if (lod >= 2 && tipLen > 0.0f) Sketch::Path(branches, a.Vee(tipT, FitBranch(tipT, tipLen, limit)));
    }
    if (lod >= 2) Hub(s, hub, 0.5f, lod >= 3 ? 0.028f : 0.0f);
}

// Fernlike stellar dendrite: dense side branches that shorten towards the tip, so every arm is
// a slender feather and the crystal as a whole a six-pointed star.
void DesignFern(Sketch& s, int variant, Rng& rng) {
    int n = 12;
    float t0 = 0.17f, lmax = 0.3f, taper = 1.0f, spineRel = 0.04f, hub = 0.12f;
    if (variant != 0) {
        n = rng.Int(10, 14);
        t0 = rng.Range(0.14f, 0.21f);
        lmax = rng.Range(0.25f, 0.34f);
        taper = rng.Range(0.75f, 1.35f);
        spineRel = rng.Range(0.034f, 0.048f);
        hub = rng.Range(0.09f, 0.15f);
    }
    const float t1 = 0.93f;
    const int lod = DetailLevel(s.Rpx(), 5.0f, 10.0f, 20.0f);
    const float ws = s.W(lod == 0 ? 0.1f : spineRel, lod == 0 ? 1.1f : 1.0f);
    const float wb = s.W(0.024f, 0.8f);
    const float limit = 1.0f - 0.5f * wb;
    const int count = lod == 0 ? 6 : lod == 1 ? 4 : lod == 2 ? 7 : n;
    auto lengthAt = [&](float u, float t) {
        float e = std::pow(1.0f - u, taper) * std::min(1.0f, 0.35f + 4.0f * u);  // short ramp at the base
        return FitBranch(t, lmax * e, limit);
    };

    // A faint veil between the branches: solid enough to read as a star when small.
    const float veil = lod == 0 ? 0.6f : lod == 1 ? 0.42f : lod == 2 ? 0.16f : 0.1f;
    Layer& leaf = s.Fill(veil, lod <= 1 ? 1.0f : 0.0f);
    Layer& spine = s.Stroke(ws);
    Layer& branches = s.Stroke(wb);
    for (int k = 0; k < 6; ++k) {
        Arm a(ArmAngle(k));
        Sketch::Line(spine, {0, 0}, a.At(1.0f - 0.5f * ws));
        std::vector<Pt> left, right;
        left.push_back(a.At(0.7f * t0));
        for (int i = 0; i < count; ++i) {
            float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(count);
            float t = Lerp(t0, t1, u);
            float len = lengthAt(u, t);
            left.push_back(a.Branch(t, len, 1));
            right.push_back(a.Branch(t, len, -1));
            if (lod >= 1) Sketch::Path(branches, a.Vee(t, len));
        }
        left.push_back(a.At(1.0f - 0.5f * ws));
        for (auto it = right.rbegin(); it != right.rend(); ++it) left.push_back(*it);
        Sketch::Path(leaf, left, true);
    }
    if (lod >= 2) Hub(s, hub, 0.55f, lod >= 3 ? 0.026f : 0.0f);
}

// Sectored plate: ridges from the centre to each corner divide a translucent plate into sectors.
void DesignSectored(Sketch& s, int variant, Rng& rng) {
    float notch = 0.08f, bodyIn = 0.1f, bodyOut = 0.32f, spike = 0.0f;
    if (variant != 0) {
        notch = rng.Range(0.04f, 0.12f);
        bodyIn = rng.Range(0.06f, 0.14f);
        bodyOut = rng.Range(0.26f, 0.36f);
    }
    if (variant == 2) spike = rng.Range(0.1f, 0.15f);
    if (variant == 3) notch = rng.Range(0.12f, 0.17f);

    const int lod = DetailLevel(s.Rpx(), 4.5f, 9.0f, 18.0f);
    const float wr = s.W(0.03f, 0.85f);
    const float wd = s.W(lod == 0 ? 0.085f : 0.046f, 1.0f);
    const float R = 1.0f - 0.5f * std::max(wr, wd);
    const float plate = R * (1.0f - spike);              // corner radius of the plate proper
    const float face = plate * kCos30 * (1.0f - notch);  // the faces dip in where sectors meet

    std::vector<Pt> outline;
    for (int k = 0; k < 6; ++k) {
        float a = ArmAngle(k);
        if (spike > 0.0f) {
            outline.push_back(Polar(plate, a - 0.075f));
            outline.push_back(Polar(R, a));
            outline.push_back(Polar(plate, a + 0.075f));
        } else {
            outline.push_back(Polar(R, a));
        }
        outline.push_back(Polar(face, a + kSixth * 0.5f));
    }
    if (lod == 0) {
        Sketch::Path(s.Fill(0.5f), outline, true);
    } else {
        Sketch::Path(s.GradedFill(bodyIn, bodyOut), outline, true);
        Sketch::Path(s.Stroke(wr, 0.85f), outline, true);
    }

    Layer& ridges = s.Stroke(wd);
    for (int k = 0; k < (lod == 0 ? 3 : 6); ++k) {
        Arm a(ArmAngle(k));
        if (lod == 0) {
            Sketch::Line(ridges, a.At(-R), a.At(R));
        } else if (variant == 3 && lod >= 2) {
            // Doubled ridge: a shallow channel along each sector's spine.
            float off = 0.04f;
            Sketch::Line(ridges, a.At(0.12f, off), a.At(plate * 0.94f, 0.3f * off));
            Sketch::Line(ridges, a.At(0.12f, -off), a.At(plate * 0.94f, -0.3f * off));
        } else {
            Sketch::Line(ridges, {0, 0}, a.At(R));
        }
    }
    if (lod < 2) return;

    Layer& ribs = s.Stroke(s.W(0.022f, 0.8f), 0.62f, 0.4f);
    for (int k = 0; k < 6; ++k) {
        const float ak = ArmAngle(k);
        Arm a(ak);
        if (variant == 1) {
            // Fishbone: ribs leave the ridge parallel to the neighbouring ridges.
            for (float t : {0.34f, 0.52f, 0.7f}) {
                if (lod == 2 && t > 0.6f) continue;
                float len = std::min(0.55f * t, plate * (1.0f - notch) - t - 0.08f);
                if (len > 0.04f) Sketch::Path(ribs, a.Vee(t * plate, len * plate));
            }
        } else {
            // Growth bands: chevrons parallel to the two outer edges, broken where sectors meet.
            const float rings[] = {0.38f, 0.62f, 0.82f};
            for (int i = (lod == 2 ? 1 : 0); i < 3; ++i) {
                if (variant == 2 && i == 0) continue;
                const float t = rings[i] * plate;
                const Pt apex = Polar(t, ak);
                const Pt l = Polar(t * kCos30, ak - kSixth * 0.5f), r = Polar(t * kCos30, ak + kSixth * 0.5f);
                Sketch::Path(ribs, {apex + (l - apex) * 0.78f, apex, apex + (r - apex) * 0.78f});
            }
        }
    }
    if (variant == 3) Sketch::Path(s.Stroke(s.W(0.026f, 0.8f), 0.7f, 0.4f), Hexagon(0.24f * plate), true);
    if (lod >= 3) Hub(s, 0.08f, 0.75f, 0.0f);
}

// Simple hexagonal plate: glassy body, bright rim, inner hexagonal ridges (no spokes).
void DesignHexPlate(Sketch& s, int variant, Rng& rng) {
    float bodyIn = 0.12f, bodyOut = 0.34f, inner = 0.56f;
    if (variant != 0) {
        bodyIn = rng.Range(0.08f, 0.16f);
        bodyOut = rng.Range(0.28f, 0.38f);
        inner = rng.Range(0.46f, 0.62f);
    }
    const int lod = DetailLevel(s.Rpx(), 4.5f, 9.0f, 18.0f);
    const float wr = s.W(lod == 0 ? 0.1f : 0.045f, 1.0f);
    const float wi = s.W(0.024f, 0.8f);
    const float R = 1.0f - 0.5f * wr;
    const std::vector<Pt> hex = Hexagon(R);
    if (lod == 0) {
        Sketch::Path(s.Fill(0.45f), hex, true);
        Sketch::Path(s.Stroke(wr, 0.95f), hex, true);
        return;
    }
    Sketch::Path(s.GradedFill(bodyIn, bodyOut), hex, true);
    Sketch::Path(s.Stroke(wr, 0.95f), hex, true);
    if (lod == 1) {
        Hub(s, 0.17f, 0.5f, 0.0f);
        return;
    }
    if (lod >= 3) Sketch::Path(s.Stroke(wi, 0.3f, 0.0f), Hexagon(R - 0.1f), true);  // bevel just inside the rim
    Layer& lines = s.Stroke(wi, 0.6f, 0.45f);
    switch (variant) {
    case 1:
        // Inner hexagon turned 30 degrees around a small centre plate.
        Sketch::Path(lines, Hexagon(R * inner * 0.9f, kUp + kSixth * 0.5f), true);
        Hub(s, 0.12f, 0.45f, 0.022f);
        break;
    case 2:
        // Growth rings.
        Sketch::Path(lines, Hexagon(R * inner * 0.55f), true);
        Sketch::Path(lines, Hexagon(R * Lerp(inner, 1.0f, 0.4f)), true);
        Hub(s, 0.08f, 0.6f, 0.0f);
        break;
    case 3:
        // Thicker centre: a raised inner plate.
        Sketch::Path(s.Fill(0.16f, 0.0f), Hexagon(R * inner), true);
        Sketch::Path(lines, Hexagon(R * inner), true);
        Hub(s, 0.1f, 0.5f, 0.0f);
        break;
    default:
        Sketch::Path(lines, Hexagon(R * inner), true);
        Hub(s, 0.1f, 0.55f, 0.0f);
        break;
    }
}

void DesignStellarPlate(Sketch& s, int variant, Rng& rng) {
    float hub = 0.17f, ts = 0.48f, half = 0.15f, stemRel = 0.062f;
    if (variant != 0) {
        hub = rng.Range(0.13f, 0.2f);
        ts = rng.Range(0.42f, 0.55f);
        half = rng.Range(0.13f, 0.18f);
        stemRel = rng.Range(0.052f, 0.072f);
    }
    const int lod = DetailLevel(s.Rpx(), 4.5f, 9.0f, 18.0f);
    const float wst = s.W(lod == 0 ? 0.11f : stemRel, 1.1f);
    const float wr = s.W(0.03f, 0.9f);
    const float end = 1.0f - 0.5f * wr;

    // Tip plate outline on the +u side, from where the plate starts to its tip.
    std::vector<Pt> side;
    switch (variant) {
    case 1:  // leaf
        for (int i = 0; i <= 8; ++i) {
            float u = static_cast<float>(i) / 8.0f;
            side.push_back({Lerp(ts, end, u), half * std::pow(std::max(0.0f, std::sin(kPi * u)), 0.8f)});
        }
        break;
    case 2:  // fan: widens towards a blunt end
        side = {{ts, 0.0f}, {ts + 0.05f, 0.05f}, {0.86f, half * 1.35f}, {end, half * 0.55f}, {end, 0.0f}};
        break;
    default:  // hexagonal spade
        side = {{ts, 0.0f}, {ts + 0.1f, half}, {end - 0.12f, half}, {end, 0.0f}};
        break;
    }

    Layer& stems = s.Stroke(wst);
    Layer& plates = lod == 0 ? s.Fill(0.9f) : s.GradedFill(0.2f, 0.5f);
    Layer& rims = s.Stroke(wr, 0.95f, 0.0f);
    Layer& ribs = s.Stroke(s.W(0.024f, 0.8f), 0.8f, 0.0f);
    Layer& leaves = s.Stroke(s.W(0.045f, 0.9f));
    for (int k = 0; k < 6; ++k) {
        Arm a(ArmAngle(k));
        Sketch::Line(stems, {0, 0}, a.At(lod == 0 ? 0.62f : ts + 0.05f));
        std::vector<Pt> outline;
        for (const Pt& p : side) outline.push_back(a.At(p.x, p.y));
        for (auto it = side.rbegin(); it != side.rend(); ++it)
            if (it->y > 1e-4f) outline.push_back(a.At(it->x, -it->y));
        Sketch::Path(plates, outline, true);
        if (lod >= 1) Sketch::Path(rims, outline, true);
        if (lod >= 2) Sketch::Line(ribs, a.At(ts + 0.04f), a.At(end - 0.05f));
        if (variant == 3 && lod >= 2) {
            // A pair of short side branches on the stem.
            float t = Lerp(hub, ts, 0.55f);
            Sketch::Path(leaves, a.Vee(t, FitBranch(t, 0.16f, 1.0f)));
        }
    }
    if (lod >= 1) Hub(s, hub, 0.4f, lod >= 2 ? 0.028f : 0.0f);
}

void DesignNeedle(Sketch& s, int variant, Rng& rng) {
    std::vector<float> ticks = {0.42f, 0.6f, 0.76f};
    float tickLen = 0.07f, armRel = 0.03f;
    int tipStyle = 0;  // 0 diamond, 1 V, 2 bead
    if (variant != 0) {
        int n = rng.Int(2, 4);
        float t0 = rng.Range(0.3f, 0.45f), t1 = rng.Range(0.7f, 0.8f);
        ticks.clear();
        for (int i = 0; i < n; ++i) ticks.push_back(Lerp(t0, t1, static_cast<float>(i) / static_cast<float>(n - 1)));
        tickLen = rng.Range(0.05f, 0.09f);
        armRel = rng.Range(0.025f, 0.036f);
        tipStyle = variant - 1;
    }
    const int lod = DetailLevel(s.Rpx(), 5.0f, 11.0f, 20.0f);
    const float wa = s.W(lod == 0 ? 0.06f : armRel, 0.95f);
    const float wt = s.W(0.022f, 0.8f);
    const float tip = 1.0f - 0.5f * wa;
    Layer& arms = s.Stroke(wa);
    Layer& marks = s.Stroke(wt);
    Layer& tips = s.Fill(1.0f, 1.0f, true);
    for (int k = 0; k < 6; ++k) {
        Arm a(ArmAngle(k));
        Sketch::Line(arms, {0, 0}, a.At(tip));
        if (lod == 0) continue;
        switch (tipStyle) {
        case 1: Sketch::Path(marks, a.Vee(0.86f, 0.1f)); break;
        case 2: Sketch::Path(tips, Circle(a.At(0.9f), 0.06f, 10), true); break;
        default: Sketch::Path(tips, {a.At(0.8f), a.At(0.88f, 0.045f), a.At(tip), a.At(0.88f, -0.045f)}, true); break;
        }
        if (lod < 2) continue;
        for (float t : ticks) Sketch::Path(marks, a.Vee(t, tickLen));
    }
    if (lod >= 2) Sketch::Path(s.Stroke(wt, 0.9f), Hexagon(0.085f), true);
}

void DesignTwelve(Sketch& s, int variant, Rng& rng) {
    float shortLen = 0.78f, b1 = 0.55f, l1 = 0.13f, b2 = 0.76f, l2 = 0.09f, bs = 0.6f, ls = 0.08f;
    if (variant != 0) {
        shortLen = rng.Range(0.68f, 0.84f);
        b1 = rng.Range(0.45f, 0.6f);
        l1 = rng.Range(0.1f, 0.15f);
        b2 = rng.Range(0.7f, 0.8f);
        l2 = rng.Range(0.06f, 0.1f);
        bs = rng.Range(0.5f, 0.62f) * shortLen / 0.78f;
        ls = rng.Range(0.05f, 0.09f);
    }
    const int lod = DetailLevel(s.Rpx(), 5.0f, 11.0f, 20.0f);
    const float wa = s.W(lod == 0 ? 0.08f : 0.048f, 1.0f);
    const float wb = s.W(lod == 0 ? 0.07f : 0.04f, 0.9f);
    const float wt = s.W(0.028f, 0.8f);
    Layer& longArms = s.Stroke(wa);
    Layer& shortArms = s.Stroke(wb);
    Layer& branches = s.Stroke(wt);
    for (int k = 0; k < 6; ++k) {
        Arm a(ArmAngle(k)), b(ArmAngle(k, kSixth * 0.5f));
        Sketch::Line(longArms, {0, 0}, a.At(1.0f - 0.5f * wa));
        Sketch::Line(shortArms, {0, 0}, b.At(shortLen));
        if (lod == 0) continue;
        Sketch::Path(branches, a.Vee(b1, l1));
        if (lod < 2) continue;
        Sketch::Path(branches, a.Vee(b2, l2));
        Sketch::Path(branches, b.Vee(bs, ls));
    }
    if (lod >= 2) Sketch::Path(s.Fill(0.9f, 0.0f), Circle({0, 0}, 0.085f, 12), true);
}

// Draws a procedural design centred on the origin of the current transform.
// pxScale is device px per unit of r.
void DrawProcedural(ID2D1RenderTarget* rt, ID2D1DeviceContext* maxBlend, FlakeKind kind, int variant, float r,
                    float pxScale, bool shadow) {
    variant = Clamp(variant, 0, FlakeVariantCount(kind) - 1);
    Rng rng(0xF1A4E5EEDull + static_cast<uint64_t>(kind) * 0x9E3779B97F4A7C15ull +
            static_cast<uint64_t>(variant) * 7919u);
    Sketch s(r, pxScale);
    switch (kind) {
    case FlakeKind::Dot: DesignDot(s); break;
    case FlakeKind::Soft: DesignSoft(s, variant, rng); break;
    case FlakeKind::Plus: DesignPlus(s); break;
    case FlakeKind::Asterisk: DesignAsterisk(s, variant); break;
    case FlakeKind::Sparkle: DesignSparkle(s, variant); break;
    case FlakeKind::Dendrite: DesignDendrite(s, variant, rng); break;
    case FlakeKind::Sectored: DesignSectored(s, variant, rng); break;
    case FlakeKind::Fern: DesignFern(s, variant, rng); break;
    case FlakeKind::HexPlate: DesignHexPlate(s, variant, rng); break;
    case FlakeKind::StellarPlate: DesignStellarPlate(s, variant, rng); break;
    case FlakeKind::Needle: DesignNeedle(s, variant, rng); break;
    case FlakeKind::Twelve: DesignTwelve(s, variant, rng); break;
    default: return;
    }
    s.Render(rt, maxBlend, shadow);
}

// ---------------------------------------------------------------------------
// Classic pixel-art flakes
// ---------------------------------------------------------------------------

// Halo for a classic flake at its source resolution: the sprite's alpha grown by one pixel,
// in black, and cleared again under the art itself so its translucent pixels are not greyed.
// Scaled up with the art it stays pixel-crisp.
ComPtr<ID2D1Bitmap> MakeClassicHalo(ID2D1RenderTarget* rt, ID2D1Bitmap* art) {
    const D2D1_SIZE_U size = art->GetPixelSize();
    const D2D1_SIZE_U haloSize = {size.width + 2, size.height + 2};
    const D2D1_SIZE_F dips = {static_cast<float>(haloSize.width), static_cast<float>(haloSize.height)};
    const D2D1_PIXEL_FORMAT format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    ComPtr<ID2D1BitmapRenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> ink;
    if (FAILED(rt->CreateCompatibleRenderTarget(&dips, &haloSize, &format, D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE,
                                                &target)) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &ink)))
        return nullptr;
    ComPtr<ID2D1DeviceContext> maxBlend;
    target.As(&maxBlend);

    static constexpr struct {
        float dx, dy, weight;
    } kTaps[] = {{0, 0, 1.0f},    {-1, 0, 0.85f},  {1, 0, 0.85f},  {0, -1, 0.85f}, {0, 1, 0.85f},
                 {-1, -1, 0.55f}, {1, -1, 0.55f}, {-1, 1, 0.55f}, {1, 1, 0.55f}};
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0, 0, 0));
    target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);  // required by FillOpacityMask
    if (maxBlend) maxBlend->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_MAX);
    for (const auto& tap : kTaps) {
        // Whole-pixel offsets at 1:1 copy the alpha exactly.
        ink->SetColor(D2D1::ColorF(0, 0, 0, kHaloAlpha * tap.weight * (maxBlend ? 1.0f : 0.45f)));
        D2D1_RECT_F rc = {1.0f + tap.dx, 1.0f + tap.dy, 1.0f + tap.dx + static_cast<float>(size.width),
                          1.0f + tap.dy + static_cast<float>(size.height)};
        target->FillOpacityMask(art, ink.Get(), D2D1_OPACITY_MASK_CONTENT_GRAPHICS, &rc, nullptr);
    }
    if (maxBlend) {
        maxBlend->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        // DrawImage sizes bitmaps by their DPI: scale so that one art pixel covers one halo pixel.
        float dpiX = 96.0f, dpiY = 96.0f;
        art->GetDpi(&dpiX, &dpiY);
        maxBlend->SetTransform(D2D1::Matrix3x2F::Scale(dpiX / 96.0f, dpiY / 96.0f) *
                               D2D1::Matrix3x2F::Translation(1.0f, 1.0f));
        const D2D1_POINT_2F origin = {0.0f, 0.0f};
        maxBlend->DrawImage(art, &origin, nullptr, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                            D2D1_COMPOSITE_MODE_DESTINATION_OUT);
        maxBlend->SetTransform(D2D1::Matrix3x2F::Identity());
    }
    ComPtr<ID2D1Bitmap> bitmap;
    if (FAILED(target->EndDraw()) || FAILED(target->GetBitmap(&bitmap))) return nullptr;
    return bitmap;
}

// Moves `rc` so that its top-left corner sits on a device pixel corner (when the
// transform has no rotation), so that whole-pixel scaling stays crisp.
void SnapToPixels(ID2D1RenderTarget* rt, D2D1_RECT_F& rc) {
    D2D1_MATRIX_3X2_F m;
    rt->GetTransform(&m);
    if (m._12 != 0.0f || m._21 != 0.0f || m._11 == 0.0f || m._22 == 0.0f) return;
    float dpiX = 96.0f, dpiY = 96.0f;
    rt->GetDpi(&dpiX, &dpiY);
    const float sx = m._11 * dpiX / 96.0f, sy = m._22 * dpiY / 96.0f;
    const float x = (m._11 * rc.left + m._31) * dpiX / 96.0f;
    const float y = (m._22 * rc.top + m._32) * dpiY / 96.0f;
    const float dx = (std::round(x) - x) / sx, dy = (std::round(y) - y) / sy;
    rc = {rc.left + dx, rc.top + dy, rc.right + dx, rc.bottom + dy};
}

// Draws a classic flake (and its halo, if given) centred on the origin, fitting radius r.
// Enlargements use whole-pixel nearest-neighbour scaling.
void DrawClassic(ID2D1RenderTarget* rt, ID2D1Bitmap* art, ID2D1Bitmap* halo, float r, float pxScale) {
    const D2D1_SIZE_U size = art->GetPixelSize();
    if (size.width == 0 || size.height == 0) return;
    const float srcPx = static_cast<float>(std::max(size.width, size.height));
    float k = 2.0f * r * pxScale / srcPx;  // device px per source pixel
    const bool crisp = k >= 1.0f;
    if (crisp) k = std::floor(k + 1e-3f);
    const float px = k / pxScale;  // one source pixel in local units
    D2D1_RECT_F dst = {-0.5f * px * static_cast<float>(size.width), -0.5f * px * static_cast<float>(size.height),
                       0.5f * px * static_cast<float>(size.width), 0.5f * px * static_cast<float>(size.height)};
    if (crisp) SnapToPixels(rt, dst);

    ComPtr<ID2D1DeviceContext> dc;
    if (!crisp) rt->QueryInterface(IID_PPV_ARGS(&dc));
    auto draw = [&](ID2D1Bitmap* bmp, const D2D1_RECT_F& rc) {
        if (crisp) rt->DrawBitmap(bmp, rc, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        else if (dc) dc->DrawBitmap(bmp, &rc, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
        else rt->DrawBitmap(bmp, rc, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    };
    if (halo) {
        const D2D1_SIZE_U hs = halo->GetPixelSize();
        const float gx = 0.5f * px * static_cast<float>(hs.width - size.width);
        const float gy = 0.5f * px * static_cast<float>(hs.height - size.height);
        draw(halo, {dst.left - gx, dst.top - gy, dst.right + gx, dst.bottom + gy});
    }
    draw(art, dst);
}

// Restores a device context's target and drawing state when the atlas has to borrow it.
class ContextStateGuard {
public:
    explicit ContextStateGuard(ID2D1DeviceContext* dc) : dc_(dc) {
        if (!dc_) return;
        dc_->GetTarget(&target_);
        dc_->GetTransform(&transform_);
        dc_->GetDpi(&dpiX_, &dpiY_);
        unit_ = dc_->GetUnitMode();
        antialias_ = dc_->GetAntialiasMode();
        blend_ = dc_->GetPrimitiveBlend();
    }
    ~ContextStateGuard() {
        if (!dc_) return;
        dc_->SetTarget(target_.Get());
        dc_->SetTransform(transform_);
        dc_->SetDpi(dpiX_, dpiY_);
        dc_->SetUnitMode(unit_);
        dc_->SetAntialiasMode(antialias_);
        dc_->SetPrimitiveBlend(blend_);
    }
    ContextStateGuard(const ContextStateGuard&) = delete;
    ContextStateGuard& operator=(const ContextStateGuard&) = delete;

private:
    ID2D1DeviceContext* dc_;
    ComPtr<ID2D1Image> target_;
    D2D1_MATRIX_3X2_F transform_{};
    float dpiX_ = 96.0f, dpiY_ = 96.0f;
    D2D1_UNIT_MODE unit_ = D2D1_UNIT_MODE_DIPS;
    D2D1_ANTIALIAS_MODE antialias_ = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
    D2D1_PRIMITIVE_BLEND blend_ = D2D1_PRIMITIVE_BLEND_SOURCE_OVER;
};

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

const FlakeTraits& GetFlakeTraits(FlakeKind kind) {
    int k = static_cast<int>(kind);
    return kTraits[(k >= 0 && k < kFlakeKindCount) ? k : 0];
}

int FlakeVariantCount(FlakeKind kind) {
    int k = static_cast<int>(kind);
    return (k >= 0 && k < kFlakeKindCount) ? kVariantCounts[k] : 1;
}

HRESULT LoadClassicFlake(IWICImagingFactory* wic, int index, IWICBitmapSource** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!wic || index < 0 || index >= kClassicCount) return E_INVALIDARG;

    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(IDR_CLASSIC_FIRST + index), RT_RCDATA);
    if (!res) return HRESULT_FROM_WIN32(GetLastError());
    HGLOBAL handle = LoadResource(module, res);
    DWORD size = SizeofResource(module, res);
    void* data = handle ? LockResource(handle) : nullptr;
    if (!data || size == 0) return E_FAIL;

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    ComPtr<IWICBitmap> bitmap;
    HRESULT hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory(static_cast<BYTE*>(data), size);
    if (SUCCEEDED(hr)) hr = wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr))
        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeMedianCut);
    // Decode now so the result does not depend on the stream.
    if (SUCCEEDED(hr)) hr = wic->CreateBitmapFromSource(converter.Get(), WICBitmapCacheOnLoad, &bitmap);
    if (SUCCEEDED(hr)) *out = bitmap.Detach();
    return hr;
}

void DrawFlakeDesign(ID2D1RenderTarget* rt, FlakeKind kind, int variant, float cx, float cy, float r,
                     const FlakeArtOptions& opt, ID2D1Bitmap* classic) {
    if (!rt || !(r > 0.0f)) return;
    D2D1_MATRIX_3X2_F old;
    rt->GetTransform(&old);
    float dpiX = 96.0f, dpiY = 96.0f;
    rt->GetDpi(&dpiX, &dpiY);
    const float scale = std::sqrt(std::fabs(old._11 * old._22 - old._12 * old._21));
    const float pxScale = std::max(scale * dpiX / 96.0f, 1e-3f);
    rt->SetTransform(D2D1::Matrix3x2F::Translation(cx, cy) * old);

    if (IsClassic(kind)) {
        if (classic) {
            ComPtr<ID2D1Bitmap> halo;
            if (opt.shadow) halo = MakeClassicHalo(rt, classic);
            DrawClassic(rt, classic, halo.Get(), r, pxScale);
        }
    } else {
        ComPtr<ID2D1DeviceContext> dc;
        if (opt.shadow && SUCCEEDED(rt->QueryInterface(IID_PPV_ARGS(&dc)))) {
            // The halo passes merge with MAX blending, which needs a transparent
            // backdrop: draw the flake into a layer and composite that.
            const float reach = 1.1f * r + (HaloPx(r * pxScale) + 2.0f) / pxScale;
            dc->PushLayer(D2D1::LayerParameters1(D2D1::RectF(-reach, -reach, reach, reach)), nullptr);
            DrawProcedural(rt, dc.Get(), kind, variant, r, pxScale, true);
            dc->PopLayer();
        } else {
            DrawProcedural(rt, nullptr, kind, variant, r, pxScale, opt.shadow);
        }
    }
    rt->SetTransform(old);
}

// ---------------------------------------------------------------------------
// FlakeAtlas
// ---------------------------------------------------------------------------

int FlakeAtlas::SpriteIndex(FlakeKind kind, int variant) {
    int k = static_cast<int>(kind);
    if (k < 0 || k >= kFlakeKindCount) return 0;
    return kSpriteOffset[k] + Clamp(variant, 0, kVariantCounts[k] - 1);
}

int FlakeAtlas::SpriteCount() { return kSpriteTotal; }

void FlakeAtlas::Reset() {
    levels_.clear();
    pages_.clear();
    diameter_.clear();
    classicDiameter_.clear();
    device_.Reset();
}

const FlakeAtlas::Sprite& FlakeAtlas::Get(int index, float diameterPx) const {
    static const Sprite kNone{};
    if (levels_.empty() || index < 0 || index >= kSpriteTotal) return kNone;
    const std::vector<float>& diam = index >= kFirstClassicSprite ? classicDiameter_ : diameter_;
    const int last = static_cast<int>(levels_.size()) - 1;
    const float d = diameterPx > 1.0f ? diameterPx : 1.0f;  // also catches NaN
    // Procedural level l is drawn at kTopDiameter / sqrt(2)^l: estimate the level, then settle on
    // the smallest one that is still at least as large as the target.
    int l = std::min(last, static_cast<int>(2.0f * std::log2(std::max(kTopDiameter / d, 1.0f))));
    while (l > 0 && diam[static_cast<size_t>(l)] < d) --l;
    while (l < last && diam[static_cast<size_t>(l) + 1] >= d) ++l;
    return levels_[static_cast<size_t>(l)].sprites[static_cast<size_t>(index)];
}

HRESULT FlakeAtlas::Build(ID2D1DeviceContext* dc, IWICImagingFactory* wic, const FlakeArtOptions& opt) {
    if (!dc) return E_INVALIDARG;
    // All or nothing: a failed rebuild keeps the previous atlas, unless that
    // one belongs to another device (then it is useless anyway).
    ComPtr<ID2D1Device> device;
    dc->GetDevice(&device);
    if (device != device_) Reset();

    // Draw with a private context on the same device so the caller's target and state stay
    // untouched. Should that fail, borrow the caller's context and restore it afterwards.
    ComPtr<ID2D1DeviceContext> rdc;
    if (device) device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &rdc);
    const bool borrowed = !rdc;
    if (borrowed) rdc = dc;
    ContextStateGuard guard(borrowed ? dc : nullptr);
    rdc->SetDpi(96.0f, 96.0f);
    rdc->SetUnitMode(D2D1_UNIT_MODE_DIPS);
    rdc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
    rdc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    HRESULT hr = S_OK;

    ComPtr<ID2D1Bitmap1> classic[kClassicCount];
    ComPtr<ID2D1Bitmap> classicHalo[kClassicCount];
    if (wic) {
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        for (int i = 0; i < kClassicCount; ++i) {
            ComPtr<IWICBitmapSource> src;
            if (FAILED(LoadClassicFlake(wic, i, &src)) ||
                FAILED(rdc->CreateBitmapFromWicBitmap(src.Get(), &props, &classic[i])))
                continue;
            if (opt.shadow) classicHalo[i] = MakeClassicHalo(rdc.Get(), classic[i].Get());
        }
    }

    // Plan one cell per sprite and level. Classic levels that repeat a whole-pixel scale share a cell.
    struct Cell {
        int level, sprite;
        float d;  // drawn flake diameter (px)
        int size;
        size_t page;
        D2D1_RECT_U rc;
    };
    std::vector<Cell> cells;
    std::vector<int> cellOf(static_cast<size_t>(kLevelCount * kSpriteTotal), -1);
    for (int l = 0; l < kLevelCount; ++l) {
        const float dProc = LevelDiameter(l);
        const float k = ClassicScale(l);
        const bool reuseClassic = l > 0 && k >= 1.0f && k == ClassicScale(l - 1);
        for (int i = 0; i < kSpriteTotal; ++i) {
            const bool isClassic = i >= kFirstClassicSprite;
            if (isClassic && reuseClassic) {
                cellOf[static_cast<size_t>(l * kSpriteTotal + i)] = cellOf[static_cast<size_t>((l - 1) * kSpriteTotal + i)];
                continue;
            }
            const float d = isClassic ? 15.0f * k : dProc;
            const float halo = !opt.shadow ? 0.0f : isClassic ? k : HaloPx(0.5f * d);
            int size = static_cast<int>(std::ceil(d + 2.0f * (halo + static_cast<float>(kCellPad))));
            // Whole-pixel classics: same parity as the art so it lands on the pixel grid.
            if (isClassic && k >= 1.0f && (size & 1) != (static_cast<int>(std::lround(d)) & 1)) ++size;
            cellOf[static_cast<size_t>(l * kSpriteTotal + i)] = static_cast<int>(cells.size());
            cells.push_back({l, i, d, size, 0, {}});
        }
    }

    // Shelf-pack every level onto as few pages as the device allows.
    const UINT32 maxHeight = std::min(kMaxPageHeight, rdc->GetMaximumBitmapSize());
    std::vector<UINT32> pageHeights(1, 0);
    UINT32 x = 0, y = 0, rowHeight = 0;
    for (Cell& c : cells) {
        const UINT32 size = static_cast<UINT32>(c.size);
        if (x + size > kPageWidth) {
            x = 0;
            y += rowHeight;
            rowHeight = 0;
        }
        if (y + size > maxHeight) {
            pageHeights.push_back(0);
            x = y = rowHeight = 0;
        }
        c.page = pageHeights.size() - 1;
        c.rc = {x, y, x + size, y + size};
        x += size;
        rowHeight = std::max(rowHeight, size);
        pageHeights.back() = std::max(pageHeights.back(), y + size);
    }

    std::vector<ComPtr<ID2D1Bitmap1>> pages(pageHeights.size());
    const D2D1_BITMAP_PROPERTIES1 pageProps = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    for (size_t p = 0; p < pages.size(); ++p) {
        hr = rdc->CreateBitmap(D2D1::SizeU(kPageWidth, pageHeights[p]), nullptr, 0, pageProps, &pages[p]);
        if (FAILED(hr)) return hr;
        rdc->SetTarget(pages[p].Get());
        rdc->BeginDraw();
        rdc->Clear(D2D1::ColorF(0, 0, 0, 0));
        for (const Cell& c : cells) {
            if (c.page != p) continue;
            const float left = static_cast<float>(c.rc.left), top = static_cast<float>(c.rc.top);
            const float size = static_cast<float>(c.size);
            rdc->SetTransform(D2D1::Matrix3x2F::Identity());
            // A transparent border keeps bilinear sampling and the aliased quad edges clean.
            const float b = static_cast<float>(kClearBorder);
            rdc->PushAxisAlignedClip(D2D1::RectF(left + b, top + b, left + size - b, top + size - b),
                                     D2D1_ANTIALIAS_MODE_ALIASED);
            rdc->SetTransform(D2D1::Matrix3x2F::Translation(left + 0.5f * size, top + 0.5f * size));
            if (c.sprite >= kFirstClassicSprite) {
                const size_t ci = static_cast<size_t>(c.sprite - kFirstClassicSprite);
                if (classic[ci]) DrawClassic(rdc.Get(), classic[ci].Get(), classicHalo[ci].Get(), 0.5f * c.d, 1.0f);
            } else {
                FlakeKind kind = FlakeKind::Dot;
                int variant = 0;
                for (int k = 0; k < kFlakeKindCount; ++k)
                    if (c.sprite < kSpriteOffset[k + 1]) {
                        kind = static_cast<FlakeKind>(k);
                        variant = c.sprite - kSpriteOffset[k];
                        break;
                    }
                // The page starts transparent, so the halo passes can merge with MAX directly.
                DrawProcedural(rdc.Get(), rdc.Get(), kind, variant, 0.5f * c.d, 1.0f, opt.shadow);
            }
            rdc->SetTransform(D2D1::Matrix3x2F::Identity());
            rdc->PopAxisAlignedClip();
        }
        hr = rdc->EndDraw();
        rdc->SetTarget(nullptr);
        if (FAILED(hr)) return hr;
    }

    std::vector<Level> levels(kLevelCount);
    std::vector<float> diameter(kLevelCount), classicDiameter(kLevelCount);
    for (int l = 0; l < kLevelCount; ++l) {
        Level& level = levels[static_cast<size_t>(l)];
        level.sprites.resize(kSpriteTotal);
        for (int i = 0; i < kSpriteTotal; ++i) {
            const Cell& c = cells[static_cast<size_t>(cellOf[static_cast<size_t>(l * kSpriteTotal + i)])];
            Sprite& sp = level.sprites[static_cast<size_t>(i)];
            sp.bitmap = pages[c.page].Get();
            sp.src = c.rc;
            sp.extent = static_cast<float>(c.size) / c.d;
        }
        diameter[static_cast<size_t>(l)] = LevelDiameter(l);
        classicDiameter[static_cast<size_t>(l)] = 15.0f * ClassicScale(l);
    }
    pages_ = std::move(pages);
    levels_ = std::move(levels);
    diameter_ = std::move(diameter);
    classicDiameter_ = std::move(classicDiameter);
    device_ = std::move(device);
    return S_OK;
}
