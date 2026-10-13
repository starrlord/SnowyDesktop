// The lights' look: the sprite page (each bulb type's socket, glass, filament
// and highlights, the glows, the wire and the hanging-mode markers) and the
// sprites that draw a frame with it. The bulbs are vector art drawn once into
// the page at about the size they are shown; a frame only places, tints and
// fades the cells.
#include "LightsParts.h"

#include <initializer_list>

using namespace lights;

namespace {

using Pts = std::vector<D2D1_POINT_2F>;

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

// Page.
constexpr int kPad = 3;              // per side of every page cell: 1 px antialiasing + 2 px clear border
constexpr int kClearBorder = 2;
constexpr float kSizeSteps = 4.0f;   // pages per doubling of Size (sprites scale to the exact size between)
constexpr int kGlowLevels = 2;
constexpr int kGlowStops = 40;       // the glow's falloff, sampled finely: no kinks, no rings

// Light. Glows add light (premultiplied colour above alpha), veiling only
// kGlowVeil of the desktop, as the trees' do.
constexpr float kGlowVeil = 0.35f;
constexpr float kMistAlpha = 0.6f;   // where they can't add light: a soft coloured mist instead
constexpr float kGlowTuned = 60.0f;  // the Glow setting the strengths below are tuned at

// Wire.
constexpr float kTwistFrom = 2.5f, kTwistFull = 5.0f;  // px of wire where its twist starts to show, and shows fully

// Hanging mode (DIP).
constexpr float kRing = 12.0f;       // a hook's ring
constexpr float kRingEnd = 16.0f;    // ... the last one of the strand being placed
constexpr float kSnapBar = 26.0f, kSnapThick = 3.0f;  // a mark on the edge a hook snapped to
constexpr float kOutline = 1.5f;     // the hovered strand's outline, each side of its wire

// ---------------------------------------------------------------------------
// Colour
// ---------------------------------------------------------------------------

struct Rgb {
    float r, g, b;
};
constexpr Rgb Hex(uint32_t c) {
    return {static_cast<float>((c >> 16) & 0xFF) / 255.0f, static_cast<float>((c >> 8) & 0xFF) / 255.0f,
            static_cast<float>(c & 0xFF) / 255.0f};
}
constexpr Rgb kWhite = {1.0f, 1.0f, 1.0f};
constexpr Rgb Grey(float v) { return {v, v, v}; }
Rgb Mix(Rgb a, Rgb b, float t) { return {Lerp(a.r, b.r, t), Lerp(a.g, b.g, t), Lerp(a.b, b.b, t)}; }
Rgb Mul(Rgb a, Rgb b) { return {a.r * b.r, a.g * b.g, a.b * b.b}; }
Rgb Scale(Rgb a, float k) { return {a.r * k, a.g * k, a.b * k}; }
D2D1_COLOR_F Color(Rgb c, float a) { return {c.r, c.g, c.b, a}; }

// A sprite colour that adds `c` at strength `a` while veiling only `veil` of
// what is behind (Direct2D premultiplies it to (c * a, veil * a)).
D2D1_COLOR_F Glow(Rgb c, float a, float veil) {
    const float k = 1.0f / veil;
    return {c.r * k, c.g * k, c.b * k, veil * a};
}

// A bulb's colours: its glass, and the light it gives (a touch richer).
struct BulbColor {
    Rgb glass, light;
};

// The classic multicolour set, tuned to sing on a dark desktop.
constexpr BulbColor kMulti[] = {
    {Hex(0xFF3030), Hex(0xFF3A2C)},  // red
    {Hex(0x2FD55B), Hex(0x2EE06A)},  // green
    {Hex(0x3D7BFF), Hex(0x4A7DFF)},  // blue
    {Hex(0xFFC93A), Hex(0xFFB52E)},  // yellow
    {Hex(0xA855F7), Hex(0xAD5CFF)},  // purple
    {Hex(0xFF7F1F), Hex(0xFF7A1A)},  // orange
    {Hex(0x1FC9D6), Hex(0x22D4E6)},  // teal
    {Hex(0xFF4FA3), Hex(0xFF4FAA)},  // pink
};
// Whites: incandescents are golden; LEDs white with a hint of warmth or chill.
constexpr BulbColor kWarmGlow = {Hex(0xFFE2B0), Hex(0xFFC468)};
constexpr BulbColor kWarmLed = {Hex(0xFFF0DA), Hex(0xFFDDA6)};
constexpr BulbColor kCoolGlow = {Hex(0xF4F2EA), Hex(0xFFF0D8)};
constexpr BulbColor kCoolLed = {Hex(0xEEF4FF), Hex(0xD6E4FF)};

BulbColor PaletteColor(const LightParams& p, int i, bool led) {
    const int n = std::max(i, 0);
    switch (p.colors) {
    case LightColors::Multicolor: return kMulti[n % 8];
    case LightColors::WarmWhite: return led ? kWarmLed : kWarmGlow;
    case LightColors::CoolWhite: return led ? kCoolLed : kCoolGlow;
    case LightColors::RedGreen: return kMulti[(n & 1) ? 1 : 0];
    case LightColors::BlueWhite: return (n & 1) ? (led ? kCoolLed : kCoolGlow) : kMulti[2];
    default: {
        // The user's colour for the glass; its light at full strength (black
        // glass has no hue to give: it lights white, never a dark smudge).
        const Rgb c = {GetRValue(p.customColor) / 255.0f, GetGValue(p.customColor) / 255.0f,
                       GetBValue(p.customColor) / 255.0f};
        const float top = std::max({c.r, c.g, c.b});
        return {c, top > 0.0f ? Scale(c, 1.0f / top) : (led ? kCoolLed.light : kWarmGlow.light)};
    }
    }
}

// An incandescent's light as its filament cools: from its colour towards a
// deep orange (a blue bulb dims to violet, a white one to amber).
Rgb Warm(Rgb c, float heat) {
    const float h = Saturate(heat);
    return {c.r, c.g * Lerp(0.42f, 1.0f, std::sqrt(h)), c.b * Lerp(0.12f, 1.0f, h * h)};
}

// A filament: white hot, then orange, a dull red, and cold a grey wire.
Rgb FilamentColor(float heat) {
    const float h = Saturate(heat);
    const Rgb hot = h > 0.5f ? Mix(Hex(0xFF9A3C), Hex(0xFFF4DC), (h - 0.5f) / 0.5f)
                             : Mix(Hex(0x8A2A10), Hex(0xFF9A3C), h / 0.5f);
    return Mix(Grey(0.42f), hot, SmoothStep(0.03f, 0.25f, h));
}

// The wire and the sockets, in each wire colour.
struct WireLook {
    Rgb edge, body, lobe, groove;  // wire: its rounded edges, middle, twisted strands and the grooves between
    Rgb sheen;                     // the light along its top
    Rgb sockDark, sockMid, sockLit;  // sockets, moulded in the same plastic
};
const WireLook& WireStyle(WireColor c) {
    static const WireLook kLooks[] = {
        {Hex(0x0C2817), Hex(0x1B4D2E), Hex(0x255E3B), Hex(0x061509), Hex(0x8FD3A6),
         Hex(0x0A2416), Hex(0x154127), Hex(0x24603B)},
        {Hex(0xA9ACA6), Hex(0xE6E7E2), Hex(0xFAFAF7), Hex(0x8C8E89), Hex(0xFFFFFF),
         Hex(0x9EA19B), Hex(0xD9DBD5), Hex(0xF7F8F4)},
        {Hex(0x060607), Hex(0x1C1C1F), Hex(0x323338), Hex(0x000000), Hex(0x8A8C94),
         Hex(0x0A0A0C), Hex(0x222226), Hex(0x4A4B52)},
    };
    return kLooks[Clamp(static_cast<int>(c), 0, 2)];
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

ComPtr<ID2D1PathGeometry> MakePath(ID2D1Factory* factory, const Pts& pts, bool closed) {
    ComPtr<ID2D1PathGeometry> geo;
    ComPtr<ID2D1GeometrySink> sink;
    if (pts.size() < 2 || FAILED(factory->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) return nullptr;
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    sink->BeginFigure(pts[0], closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLines(pts.data() + 1, static_cast<UINT32>(pts.size() - 1));
    sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return nullptr;
    return geo;
}

// Gradient stop collections, one per distinct list of stops (a new collection
// costs far more to draw with than a new brush on one already used).
class Stops {
public:
    explicit Stops(ID2D1RenderTarget* rt) : rt_(rt) {}
    ID2D1RenderTarget* Target() const { return rt_; }
    ID2D1GradientStopCollection* Get(const D2D1_GRADIENT_STOP* list, size_t n) {
        for (const Entry& e : entries_)
            if (std::equal(e.stops.begin(), e.stops.end(), list, list + n, Same)) return e.coll.Get();
        Entry& e = entries_.emplace_back();
        e.stops.assign(list, list + n);
        rt_->CreateGradientStopCollection(list, static_cast<UINT32>(n), &e.coll);
        return e.coll.Get();
    }
    ID2D1GradientStopCollection* Get(std::initializer_list<D2D1_GRADIENT_STOP> list) {
        return Get(list.begin(), list.size());
    }

private:
    static bool Same(const D2D1_GRADIENT_STOP& a, const D2D1_GRADIENT_STOP& b) {
        return a.position == b.position && a.color.r == b.color.r && a.color.g == b.color.g &&
               a.color.b == b.color.b && a.color.a == b.color.a;
    }
    struct Entry {
        std::vector<D2D1_GRADIENT_STOP> stops;
        ComPtr<ID2D1GradientStopCollection> coll;
    };
    ID2D1RenderTarget* rt_;
    std::vector<Entry> entries_;
};

ComPtr<ID2D1Brush> Linear(Stops& stops, D2D1_POINT_2F a, D2D1_POINT_2F b, std::initializer_list<D2D1_GRADIENT_STOP> list) {
    ComPtr<ID2D1LinearGradientBrush> brush;
    if (ID2D1GradientStopCollection* coll = stops.Get(list))
        stops.Target()->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), coll, &brush);
    return brush;
}

ComPtr<ID2D1Brush> Radial(Stops& stops, D2D1_POINT_2F c, float rx, float ry, std::initializer_list<D2D1_GRADIENT_STOP> list) {
    ComPtr<ID2D1RadialGradientBrush> brush;
    if (ID2D1GradientStopCollection* coll = stops.Get(list))
        stops.Target()->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(c, {0, 0}, rx, ry), coll, &brush);
    return brush;
}

// The glow's falloff from its middle (r = 0) to its rim (r = 1), where it and
// its slope reach zero: a soft bell with a long tail.
float GlowFalloff(float r) {
    const float q = 1.0f - r * r;
    return std::pow(q, 2.5f) * (0.8f + 0.2f * std::exp(-4.0f * r * r));
}

// ---------------------------------------------------------------------------
// Bulb shapes. A bulb is drawn hanging straight down from (0, 0), where its
// socket clips onto the wire, in px.
// ---------------------------------------------------------------------------

// Each type's socket and glass along its axis (DIP, as its BulbSpec).
struct Form {
    float sockLen, sockTop, sockFoot;  // the socket: length, width at the wire and at its foot
    float brass;                       // brass neck showing below the socket (0: none)
    float glassTop, glassEnd;          // the glass (the bubble light's base) from here to here
    float glow;                        // how wide the light it gives seems, of its BulbSpec width
    float clear;                       // 0 coloured glass .. 1 clear
    float lit, core;                   // lit, how strongly the glass shows its colour, and its hot heart
};
constexpr Form kForms[kBulbTypeCount] = {
    // sock  top   foot  brass glass  end    glow   clear lit   core
    {4.5f, 5.4f, 5.0f, 0.0f, 4.1f, 12.0f, 1.0f, 0.0f, 1.0f, 0.6f},    // Mini
    {4.3f, 5.8f, 5.4f, 0.0f, 3.9f, 11.0f, 1.0f, 0.0f, 1.0f, 0.45f},   // MiniLed
    {3.6f, 7.0f, 6.8f, 0.0f, 3.3f, 8.0f, 0.85f, 0.0f, 1.0f, 0.45f},   // WideLed
    {4.6f, 5.4f, 4.5f, 0.0f, 4.3f, 13.0f, 1.0f, 0.0f, 1.0f, 0.6f},    // C5
    {4.4f, 5.8f, 4.8f, 0.0f, 4.1f, 13.0f, 1.0f, 0.0f, 1.0f, 0.25f},   // C6 (its facets sparkle)
    {5.4f, 7.0f, 5.6f, 1.0f, 5.9f, 19.0f, 1.0f, 0.0f, 1.0f, 0.6f},    // C7
    {6.8f, 8.8f, 7.0f, 1.6f, 8.0f, 25.0f, 1.0f, 0.0f, 1.0f, 0.6f},    // C9
    {4.0f, 6.0f, 5.2f, 1.3f, 5.0f, 19.0f, 0.8f, 0.0f, 1.0f, 0.6f},    // Globe
    {3.2f, 5.4f, 4.8f, 0.0f, 2.9f, 13.5f, 0.6f, 0.0f, 0.82f, 0.0f},   // Bubble (painted; its tube runs on to the end)
    {5.2f, 6.4f, 5.4f, 1.6f, 6.4f, 20.0f, 0.85f, 0.6f, 0.4f, 0.25f},  // Candle (clear: the flame shows)
};

// From a neck out to the widest point at `at`.
float Swell(float u, float neck, float at) { return Lerp(neck, 1.0f, std::sin(0.5f * kPi * Saturate(u / at))); }
// From the widest point at `at` in to the tip: p shapes the shoulder, q the
// tip (below 1 rounded, above 1 drawn out to a point).
float Taper(float u, float at, float p, float q) {
    const float v = Saturate((u - at) / (1.0f - at));
    return std::pow(std::max(0.0f, 1.0f - std::pow(v, p)), q);
}

// The glass's half width (of the bulb's half width) at `u`: 0 where it leaves
// the socket, 1 at the tip.
float Profile(BulbType t, float u) {
    switch (t) {
    case BulbType::Mini: {
        // A slim capsule, sealed with a little nipple at the tip.
        const float e = u / 0.92f;  // (the body ends short of the tip)
        const float body = e < 0.16f ? Swell(e, 0.8f, 0.16f) : Taper(e, 0.62f, 2.6f, 0.45f);
        const float nip = u > 0.84f ? 0.24f * std::pow(Saturate((1.0f - u) / 0.16f), 0.6f) : 0.0f;
        return std::max(body, nip);
    }
    case BulbType::MiniLed: {
        // A cylinder of a lens narrowing to a rounded cone.
        const float body = u < 0.08f ? Swell(u, 0.9f, 0.08f) : 0.25f + 0.75f * Taper(u, 0.5f, 1.15f, 0.8f);
        return body * (u > 0.92f ? std::sqrt(Saturate((1.0f - u) / 0.08f)) : 1.0f);
    }
    case BulbType::WideLed: return std::pow(std::max(0.0f, 1.0f - std::pow(u, 7.0f)), 0.22f);
    case BulbType::C5: return u < 0.38f ? Swell(u, 0.6f, 0.38f) : Taper(u, 0.38f, 2.1f, 1.6f);
    case BulbType::C6: return u < 0.28f ? Swell(u, 0.78f, 0.28f) : Taper(u, 0.28f, 1.5f, 0.62f);
    case BulbType::C7: return u < 0.36f ? Swell(u, 0.5f, 0.36f) : Taper(u, 0.36f, 1.3f, 0.92f);
    case BulbType::C9: return u < 0.4f ? Swell(u, 0.5f, 0.4f) : Taper(u, 0.4f, 1.45f, 0.72f);
    case BulbType::Globe: {
        // A ball on a narrow neck.
        const Form& f = kForms[static_cast<int>(t)];
        const float r = 0.5f * Spec(t).width;
        const float y = Lerp(f.glassTop, f.glassEnd, u) - (f.glassEnd - r);
        return std::max(std::sqrt(std::max(0.0f, r * r - y * y)) / r, u < 0.12f ? 0.26f : 0.0f);
    }
    case BulbType::Bubble: {
        // A dome swelling from the socket, its rim curling in to the tube's collar.
        if (u < 0.62f) return Lerp(0.42f, 1.0f, std::pow(std::sin(0.5f * kPi * u / 0.62f), 0.7f));
        const float v = (u - 0.62f) / 0.38f;
        return 0.36f + 0.64f * std::pow(std::max(0.0f, 1.0f - std::pow(v, 2.5f)), 0.5f);
    }
    case BulbType::Candle: return u < 0.3f ? Swell(u, 0.55f, 0.3f) : Taper(u, 0.3f, 1.6f, 1.45f);
    default: return 1.0f;
    }
}

// How far the glass's axis bends aside at `u` (of the bulb's width): the
// drawn-out tips of the flame shapes curl a little.
float Bend(BulbType t, float u) {
    if (t == BulbType::C5) return 0.12f * std::pow(Saturate((u - 0.38f) / 0.62f), 2.5f);
    if (t == BulbType::Candle) return 0.07f * std::pow(Saturate((u - 0.3f) / 0.7f), 3.0f);
    return 0.0f;
}

struct Shape {
    BulbType type = BulbType::C7;
    float s = 1.0f;                // px per DIP
    float len = 0.0f, w = 0.0f;    // the whole bulb, px
    float sockLen = 0.0f, sockTop = 0.0f, sockFoot = 0.0f;
    float brass = 0.0f;
    float top = 0.0f, end = 0.0f;  // the glass
    float cy = 0.0f;               // where the light comes from
    float tubeW = 0.0f;            // bubble light: its tube, from `end` down to `len`
    float bubbleR = 0.0f;          // ... and its biggest bubbles
    float flameTop = 0.0f, flameLen = 0.0f, flameW = 0.0f;  // candle: the flame inside, from its root
    Pts glass;                     // the glass's outline

    float Glass() const { return end - top; }
    float HalfAt(float u) const { return 0.5f * w * Profile(type, u); }
    D2D1_POINT_2F At(float u, float side) const {
        return {(Bend(type, u) * w) + side * HalfAt(u), Lerp(top, end, u)};
    }
};

Shape MakeShape(BulbType type, float s) {
    const BulbSpec& spec = Spec(type);
    const Form& f = kForms[static_cast<int>(type)];
    Shape sh;
    sh.type = type;
    sh.s = s;
    sh.len = spec.length * s;
    sh.w = spec.width * s;
    sh.sockLen = f.sockLen * s;
    sh.sockTop = f.sockTop * s;
    sh.sockFoot = f.sockFoot * s;
    sh.brass = f.brass * s;
    sh.top = f.glassTop * s;
    sh.end = f.glassEnd * s;
    sh.cy = spec.glassCenter * s;
    if (type == BulbType::Bubble) {
        sh.tubeW = 0.33f * sh.w;
        sh.bubbleR = 0.3f * sh.tubeW;
    }
    if (type == BulbType::Candle) {
        sh.flameTop = sh.top + 0.1f * sh.Glass();
        sh.flameLen = 0.62f * sh.Glass();
        sh.flameW = 0.44f * sh.w;
    }
    // Down one side and back up the other, finer towards the tip.
    const int n = 56;
    for (int i = 0; i <= n; ++i) {
        const float u = 1.0f - std::pow(1.0f - static_cast<float>(i) / static_cast<float>(n), 1.6f);
        sh.glass.push_back(sh.At(u, 1.0f));
    }
    for (int i = n - 1; i >= 0; --i) {
        const float u = 1.0f - std::pow(1.0f - static_cast<float>(i) / static_cast<float>(n), 1.6f);
        sh.glass.push_back(sh.At(u, -1.0f));
    }
    return sh;
}

// ---------------------------------------------------------------------------
// Bulb art. Each part is drawn on its own (a page cell, or a layer of an icon)
// so that lit and unlit glass can cross-fade, filaments warm and cool, and
// highlights stay put. Parts the sprites tint are drawn white and grey.
// ---------------------------------------------------------------------------

enum Part { kSocket, kGlassDark, kGlassLit, kCore, kFilament, kGleam, kTube, kLiquid, kFlame, kBubble, kPartCount };

// Where the art draws: the page (white, the sprites tint it) or an icon's
// target (in the bulb's colours).
struct Canvas {
    ID2D1RenderTarget* rt = nullptr;
    ID2D1Factory* factory = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    Stops* stops = nullptr;
    Rgb tint = kWhite;      // every colour drawn is multiplied by it
    float opacity = 1.0f;   // ... and its alpha by this

    D2D1_COLOR_F Col(Rgb c, float a) const { return Color(Mul(c, tint), a * opacity); }
    D2D1_GRADIENT_STOP Stop(float at, Rgb c, float a) const { return {at, Col(c, a)}; }
    ID2D1SolidColorBrush* Solid(Rgb c, float a) const {
        brush->SetColor(Col(c, a));
        return brush;
    }
    void Clip(ID2D1Geometry* g) const { rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), g), nullptr); }
};

void DrawSocket(const Canvas& c, const Shape& sh, const WireLook& wl) {
    // A moulded plastic cap, a little narrower at its foot, round in section.
    const float t = 0.5f * sh.sockTop, b = 0.5f * sh.sockFoot, L = sh.sockLen;
    const float r = std::min(0.22f * b, 0.2f * L);
    Pts pts = {{-t, 0.0f}, {t, 0.0f}};
    for (int i = 0; i <= 4; ++i) {
        const float a = 0.5f * kPi * static_cast<float>(i) / 4.0f;
        pts.push_back({b - r + r * std::cos(a), L - r + r * std::sin(a)});
    }
    for (int i = 4; i >= 0; --i) {
        const float a = 0.5f * kPi * static_cast<float>(i) / 4.0f;
        pts.push_back({-(b - r + r * std::cos(a)), L - r + r * std::sin(a)});
    }
    // The brass neck first: the socket's foot covers its top.
    if (sh.brass > 0.0f) {
        const float nw = std::max(sh.HalfAt(0.0f) * 1.1f, 0.42f * b);
        const float y0 = L - 0.5f * sh.s, y1 = L + sh.brass;
        if (auto g = Linear(*c.stops, {-nw, 0}, {nw, 0},
                            {c.Stop(0.0f, Hex(0x6A4E1C), 1), c.Stop(0.3f, Hex(0xE8C877), 1), c.Stop(0.55f, Hex(0xB48A3A), 1),
                             c.Stop(1.0f, Hex(0x5A4116), 1)}))
            c.rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(-nw, y0, nw, y1), 0.3f * sh.s, 0.3f * sh.s), g.Get());
        // Its thread.
        const float pitch = std::max(1.6f, 0.75f * sh.s);
        for (float y = L + 0.4f * pitch; y < y1 - 0.2f * pitch; y += pitch)
            c.rt->DrawLine({-nw, y + 0.25f * pitch}, {nw, y - 0.25f * pitch}, c.Solid(Hex(0x3E2C0C), 0.45f),
                           std::max(0.7f, 0.3f * sh.s));
    }
    auto geo = MakePath(c.factory, pts, true);
    if (!geo) return;
    if (auto g = Linear(*c.stops, {-t, 0}, {t, 0},
                        {c.Stop(0.0f, wl.sockDark, 1), c.Stop(0.22f, wl.sockLit, 1), c.Stop(0.5f, wl.sockMid, 1),
                         c.Stop(1.0f, wl.sockDark, 1)}))
        c.rt->FillGeometry(geo.Get(), g.Get());
    c.Clip(geo.Get());
    // The cap's top catches the light; a lip at its foot holds the glass.
    if (auto g = Linear(*c.stops, {0, 0}, {0, 0.3f * L},
                        {c.Stop(0.0f, wl.sockLit, 0.55f), c.Stop(1.0f, wl.sockLit, 0.0f)}))
        c.rt->FillRectangle(D2D1::RectF(-t, 0.0f, t, 0.3f * L), g.Get());
    c.rt->FillRectangle(D2D1::RectF(-t, L - 0.2f * L, t, L - 0.12f * L), c.Solid(wl.sockDark, 0.6f));
    c.rt->PopLayer();
    c.rt->DrawGeometry(geo.Get(), c.Solid(wl.sockDark, 0.5f), std::max(0.6f, 0.25f * sh.s));
}

// The ribs moulded into C7 and C9 glass, following its curve.
void DrawFlutes(const Canvas& c, const Shape& sh, Rgb col, float alpha) {
    const float width = std::max(0.8f, 0.05f * sh.w);
    for (float f : {-0.62f, -0.2f, 0.2f, 0.62f}) {
        Pts pts;
        for (int i = 2; i <= 20; ++i) {
            const float u = 0.92f * static_cast<float>(i) / 20.0f;
            pts.push_back({f * sh.HalfAt(u), Lerp(sh.top, sh.end, u)});
        }
        if (auto geo = MakePath(c.factory, pts, false))
            c.rt->DrawGeometry(geo.Get(), c.Solid(col, alpha * (1.0f - 0.5f * std::fabs(f))), width);
    }
}

// The C6's strawberry facets: a diamond lattice wrapped round its cone,
// lighter where they face the light (up and left).
void DrawFacets(const Canvas& c, const Shape& sh, bool lit) {
    const int rows = 6, around = 10;
    const float hair = std::max(0.5f, 0.04f * sh.w);
    for (int r = 0; r < rows; ++r) {
        for (int k = 0; k < around; ++k) {
            const float th = (static_cast<float>(k) + 0.5f * static_cast<float>(r & 1)) / static_cast<float>(around) * kTau - kPi;
            const float dth = kTau / static_cast<float>(around);
            if (std::fabs(th) > 0.5f * kPi + 0.5f * dth) continue;  // round the back
            const float u0 = 0.06f + 0.86f * static_cast<float>(r) / static_cast<float>(rows);
            const float du = 0.86f / static_cast<float>(rows);
            auto at = [&](float a, float u) {
                return D2D1_POINT_2F{sh.HalfAt(u) * std::sin(Clamp(a, -0.5f * kPi, 0.5f * kPi)), Lerp(sh.top, sh.end, u)};
            };
            const Pts d = {at(th, u0), at(th + 0.5f * dth, u0 + 0.5f * du), at(th, u0 + du), at(th - 0.5f * dth, u0 + 0.5f * du)};
            auto geo = MakePath(c.factory, d, true);
            if (!geo) continue;
            const float face = 0.5f - 0.5f * std::sin(th) - 0.25f * static_cast<float>(r) / static_cast<float>(rows);
            const float shade = (lit ? 0.3f : 0.22f) * std::fabs(face - 0.5f) * 2.0f;
            c.rt->FillGeometry(geo.Get(), c.Solid(face > 0.5f ? kWhite : Grey(0.0f), shade));
            c.rt->DrawGeometry(geo.Get(), c.Solid(lit ? Grey(0.45f) : Grey(0.1f), lit ? 0.45f : 0.35f), hair);
        }
    }
}

// What is moulded into a type's glass: ribs, facets, an LED's seam, a dished face.
void DrawMoulding(const Canvas& c, const Shape& sh, bool lit) {
    const float hw = 0.5f * sh.w, gl = sh.Glass();
    switch (sh.type) {
    case BulbType::C7:
    case BulbType::C9: DrawFlutes(c, sh, lit ? kWhite : Grey(0.3f), lit ? 0.2f : 0.3f); break;
    case BulbType::C6: DrawFacets(c, sh, lit); break;
    case BulbType::MiniLed:
        // The lead frame inside the lens, seen edge on.
        c.rt->DrawLine({0.22f * hw, sh.top + 0.08f * gl}, {0.22f * hw, sh.top + 0.7f * gl}, c.Solid(Grey(lit ? 0.3f : 0.1f), 0.5f),
                       std::max(0.8f, 0.14f * hw));
        break;
    case BulbType::Bubble:
        // Ridges round the moulded base.
        for (float u : {0.5f, 0.74f}) {
            const float y = Lerp(sh.top, sh.end, u), x = sh.HalfAt(u), t = std::max(0.8f, 0.04f * sh.w);
            c.rt->DrawLine({-x, y}, {x, y}, c.Solid(kWhite, lit ? 0.3f : 0.22f), t);
            c.rt->DrawLine({-x, y + 1.5f * t}, {x, y + 1.5f * t}, c.Solid(Grey(0.1f), 0.3f), t);
        }
        break;
    case BulbType::WideLed: {
        // The face, dished, seen a little from below.
        const D2D1_POINT_2F f = {0.0f, sh.end - 0.17f * sh.w};
        c.rt->FillEllipse(D2D1::Ellipse(f, 0.9f * hw, 0.3f * hw), c.Solid(lit ? Grey(0.8f) : Grey(0.55f), 0.8f));
        c.rt->DrawEllipse(D2D1::Ellipse(f, 0.9f * hw, 0.3f * hw), c.Solid(kWhite, lit ? 0.95f : 0.7f), std::max(0.8f, 0.14f * hw));
        c.rt->FillEllipse(D2D1::Ellipse({f.x, f.y + 0.04f * hw}, 0.55f * hw, 0.17f * hw),
                          c.Solid(lit ? kWhite : Grey(0.4f), lit ? 0.9f : 0.7f));
        break;

    }
    default: break;
    }
}

// Glass, unlit: tinted glass with the light behind the viewer, darker at its rim.
void DrawGlassDark(const Canvas& c, const Shape& sh, ID2D1Geometry* glass) {
    const float gl = sh.Glass(), hw = 0.5f * sh.w;
    if (auto g = Radial(*c.stops, {-0.35f * hw, sh.top + 0.38f * gl}, 1.25f * hw, 0.65f * gl,
                        {c.Stop(0.0f, Grey(0.95f), 0.9f), c.Stop(0.55f, Grey(0.66f), 0.92f), c.Stop(1.0f, Grey(0.4f), 0.96f)}))
        c.rt->FillGeometry(glass, g.Get());
    c.Clip(glass);
    DrawMoulding(c, sh, false);
    c.rt->DrawGeometry(glass, c.Solid(Grey(0.2f), 0.5f), 0.22f * hw);
    c.rt->PopLayer();
}

// Glass, lit: bright inside, the colour deepest at the rim.
void DrawGlassLit(const Canvas& c, const Shape& sh, ID2D1Geometry* glass) {
    const float gl = sh.Glass(), hw = 0.5f * sh.w;
    if (auto g = Radial(*c.stops, {-0.1f * hw, sh.cy}, 1.3f * hw, 0.75f * gl,
                        {c.Stop(0.0f, kWhite, 1), c.Stop(0.55f, Grey(0.94f), 1), c.Stop(1.0f, Grey(0.72f), 1)}))
        c.rt->FillGeometry(glass, g.Get());
    c.Clip(glass);
    DrawMoulding(c, sh, true);
    c.rt->DrawGeometry(glass, c.Solid(Grey(0.5f), 0.55f), 0.2f * hw);
    // The far side, in its own shade.
    if (auto g = Linear(*c.stops, {-hw, 0}, {hw, 0}, {c.Stop(0.45f, Grey(0.0f), 0.0f), c.Stop(1.0f, Grey(0.0f), 0.2f)}))
        c.rt->FillRectangle(D2D1::RectF(-hw - 2.0f, sh.top, hw + 2.0f, sh.end + 1.0f), g.Get());
    c.rt->PopLayer();
}

// The light's heart, glowing through the glass (drawn pale and tinted).
void DrawCore(const Canvas& c, const Shape& sh, ID2D1Geometry* glass) {
    const float gl = sh.Glass(), hw = 0.5f * sh.w;
    if (auto g = Radial(*c.stops, {0.0f, sh.cy}, 0.8f * hw, 0.42f * gl,
                        {c.Stop(0.0f, kWhite, 1), c.Stop(0.35f, kWhite, 0.75f), c.Stop(0.7f, kWhite, 0.25f),
                         c.Stop(1.0f, kWhite, 0.0f)}))
        c.rt->FillGeometry(glass, g.Get());
}

// An LED's die: a white-hot point with a little bloom.
void DrawDie(const Canvas& c, D2D1_POINT_2F p, float r) {
    if (auto g = Radial(*c.stops, p, r, r,
                        {c.Stop(0.0f, kWhite, 1.0f), c.Stop(0.3f, kWhite, 0.85f), c.Stop(0.6f, kWhite, 0.25f),
                         c.Stop(1.0f, kWhite, 0.0f)}))
        c.rt->FillEllipse(D2D1::Ellipse(p, r, r), g.Get());
}

// Filament: the zigzag "M" of the big bulbs, a short bar in the small ones,
// the die of an LED.
void DrawFilament(const Canvas& c, const Shape& sh, ID2D1StrokeStyle* round) {
    const float hw = 0.5f * sh.w, gl = sh.Glass(), hair = std::max(0.9f, 0.55f * sh.s);
    switch (sh.type) {
    case BulbType::MiniLed: DrawDie(c, {0.0f, sh.top + 0.3f * gl}, std::max(1.5f, 0.42f * hw)); return;
    case BulbType::C6: DrawDie(c, {0.0f, sh.top + 0.26f * gl}, std::max(1.5f, 0.36f * hw)); return;
    case BulbType::WideLed: DrawDie(c, {0.0f, sh.end - 0.16f * sh.w}, std::max(1.5f, 0.34f * hw)); return;
    case BulbType::Mini:
    case BulbType::C5: {
        // A short coil across the middle, on two leads from the neck.
        const float y = sh.cy, a = 0.3f * hw;
        c.rt->DrawLine({-a, y}, {-0.35f * a, sh.top}, c.Solid(kWhite, 0.3f), 0.6f * hair, round);
        c.rt->DrawLine({a, y}, {0.35f * a, sh.top}, c.Solid(kWhite, 0.3f), 0.6f * hair, round);
        c.rt->DrawLine({-a, y}, {a, y}, c.Solid(kWhite, 1.0f), std::max(1.0f, 0.75f * sh.s), round);
        return;
    }
    default: {
        const float y = sh.cy - 0.1f * gl;
        const float a = 0.36f * hw, h = 0.15f * gl * (sh.type == BulbType::Globe ? 0.8f : 1.0f);
        const Pts m = {{-a, y + h}, {-0.55f * a, y - 0.4f * h}, {0.0f, y + 0.5f * h}, {0.55f * a, y - 0.4f * h}, {a, y + h}};
        // Its supports run up to the neck, faint.
        c.rt->DrawLine(m.front(), {-0.3f * a, sh.top}, c.Solid(kWhite, 0.3f), 0.7f * hair, round);
        c.rt->DrawLine(m.back(), {0.3f * a, sh.top}, c.Solid(kWhite, 0.3f), 0.7f * hair, round);
        if (auto geo = MakePath(c.factory, m, false)) c.rt->DrawGeometry(geo.Get(), c.Solid(kWhite, 1.0f), hair, round);
        return;
    }
    }
}

// The sheen of the glass: a soft streak down its lit side and a glint.
void DrawGleam(const Canvas& c, const Shape& sh, ID2D1Geometry* glass) {
    const float gl = sh.Glass(), hw = 0.5f * sh.w;
    c.Clip(glass);
    const D2D1_POINT_2F p = {-0.5f * hw, sh.top + 0.42f * gl};
    D2D1::Matrix3x2F was;
    c.rt->GetTransform(&was);
    c.rt->SetTransform(D2D1::Matrix3x2F::Rotation(8.0f, p) * was);
    if (auto g = Radial(*c.stops, p, 0.2f * hw, 0.3f * gl,
                        {c.Stop(0.0f, kWhite, 0.8f), c.Stop(0.5f, kWhite, 0.35f), c.Stop(1.0f, kWhite, 0.0f)}))
        c.rt->FillEllipse(D2D1::Ellipse(p, 0.2f * hw, 0.3f * gl), g.Get());
    c.rt->SetTransform(was);
    const D2D1_POINT_2F q = {-0.38f * hw, sh.top + 0.2f * gl};
    const float r = std::max(1.0f, 0.13f * hw);
    if (auto g = Radial(*c.stops, q, r, r, {c.Stop(0.0f, kWhite, 0.95f), c.Stop(1.0f, kWhite, 0.0f)}))
        c.rt->FillEllipse(D2D1::Ellipse(q, r, r), g.Get());
    c.rt->PopLayer();
}

// The bubble light's tube: straight walls, sealed to a little point at its end.
Pts TubeOutline(const Shape& sh, float inset) {
    const float r = 0.5f * sh.tubeW - inset, y0 = sh.end - 0.5f * sh.s, y1 = sh.len - 0.9f * sh.tubeW;
    const float seal = 0.9f * sh.tubeW - 0.75f * r;  // the point beyond the round end
    Pts pts = {{-r, y0}};
    for (int i = 0; i <= 16; ++i) {
        const float a = kPi * static_cast<float>(i) / 16.0f;
        const float sa = std::sin(a);
        pts.push_back({-r * std::cos(a), y1 + 0.75f * r * sa + 0.65f * seal * std::pow(sa, 6.0f)});
    }
    pts.push_back({r, y0});
    return pts;
}

void DrawTube(const Canvas& c, const Shape& sh) {
    auto tube = MakePath(c.factory, TubeOutline(sh, 0.0f), true);
    if (!tube) return;
    const float r = 0.5f * sh.tubeW, hair = std::max(0.7f, 0.35f * sh.s);
    c.rt->FillGeometry(tube.Get(), c.Solid(kWhite, 0.08f));
    c.Clip(tube.Get());
    // Its walls, lit on the left, and a streak of light down them.
    if (auto g = Linear(*c.stops, {-r, 0}, {r, 0},
                        {c.Stop(0.0f, kWhite, 0.55f), c.Stop(0.18f, kWhite, 0.0f), c.Stop(0.82f, kWhite, 0.0f),
                         c.Stop(1.0f, Grey(0.7f), 0.35f)}))
        c.rt->FillRectangle(D2D1::RectF(-r, sh.end - sh.s, r, sh.len), g.Get());
    c.rt->DrawLine({-0.45f * r, sh.end + 0.6f * sh.tubeW}, {-0.45f * r, sh.len - 1.6f * sh.tubeW}, c.Solid(kWhite, 0.6f),
                   std::max(0.8f, 0.2f * r));
    c.rt->PopLayer();
    c.rt->DrawGeometry(tube.Get(), c.Solid(kWhite, 0.35f), hair);
}

// The liquid in the tube (drawn pale and tinted), a little air above it.
void DrawLiquid(const Canvas& c, const Shape& sh) {
    const float r = 0.5f * sh.tubeW, wall = std::max(0.6f, 0.16f * sh.tubeW);
    auto inner = MakePath(c.factory, TubeOutline(sh, wall), true);
    if (!inner) return;
    const float surface = sh.end + 0.7f * sh.tubeW;
    c.Clip(inner.Get());
    if (auto g = Linear(*c.stops, {-r, 0}, {r, 0},
                        {c.Stop(0.0f, Grey(0.7f), 0.9f), c.Stop(0.4f, kWhite, 0.92f), c.Stop(1.0f, Grey(0.62f), 0.9f)}))
        c.rt->FillRectangle(D2D1::RectF(-r, surface, r, sh.len), g.Get());
    // The meniscus.
    c.rt->FillEllipse(D2D1::Ellipse({0.0f, surface}, r, 0.22f * sh.tubeW), c.Solid(kWhite, 0.9f));
    c.rt->PopLayer();
}

// A bubble: a bright rim, a clear middle and a glint.
void DrawBubble(const Canvas& c, const Shape& sh) {
    const float r = sh.bubbleR;
    if (auto g = Radial(*c.stops, {0, 0}, r, r,
                        {c.Stop(0.0f, kWhite, 0.15f), c.Stop(0.65f, kWhite, 0.3f), c.Stop(0.88f, kWhite, 0.95f),
                         c.Stop(1.0f, kWhite, 0.0f)}))

        c.rt->FillEllipse(D2D1::Ellipse({0, 0}, r, r), g.Get());
    c.rt->FillEllipse(D2D1::Ellipse({-0.35f * r, -0.35f * r}, 0.22f * r, 0.22f * r), c.Solid(kWhite, 0.9f));
}

// The candle lamp's flame: a glowing plate shaped like one, its round root by
// the socket and its point towards the tip (drawn in its own colours).
void DrawFlame(const Canvas& c, const Shape& sh) {
    const float L = sh.flameLen, hw = 0.5f * sh.flameW;
    auto outline = [&](float k) {
        Pts pts;
        const int n = 24;
        for (int side = 1; side >= -1; side -= 2)
            for (int i = 0; i <= n; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(n);
                const float v = side > 0 ? t : 1.0f - t;
                const float h = v < 0.3f ? std::pow(std::sin(0.5f * kPi * v / 0.3f), 0.6f)
                                         : std::pow(std::max(0.0f, 1.0f - std::pow((v - 0.3f) / 0.7f, 1.3f)), 1.5f);
                pts.push_back({static_cast<float>(side) * k * hw * h, (1.0f - k) * 0.1f * L + k * L * v});
            }
        return pts;
    };
    auto outer = MakePath(c.factory, outline(1.0f), true);
    auto inner = MakePath(c.factory, outline(0.55f), true);
    if (!outer || !inner) return;
    if (auto g = Radial(*c.stops, {0.0f, 0.3f * L}, 1.1f * hw, 0.8f * L,
                        {c.Stop(0.0f, Hex(0xFFE9A8), 1), c.Stop(0.35f, Hex(0xFFB43C), 1), c.Stop(0.75f, Hex(0xFF7418), 1),
                         c.Stop(1.0f, Hex(0xC2410C), 1)}))
        c.rt->FillGeometry(outer.Get(), g.Get());
    if (auto g = Radial(*c.stops, {0.0f, 0.25f * L}, 0.7f * hw, 0.45f * L,
                        {c.Stop(0.0f, Hex(0xFFFDF2), 1), c.Stop(0.6f, Hex(0xFFF0B8), 0.9f), c.Stop(1.0f, Hex(0xFFD36A), 0.0f)}))
        c.rt->FillGeometry(inner.Get(), g.Get());
    c.rt->DrawGeometry(outer.Get(), c.Solid(Hex(0x7A2A08), 0.35f), std::max(0.6f, 0.3f * sh.s));
}

// One part of a bulb (shape px, its anchor at the origin).
void DrawPart(const Canvas& c, const Shape& sh, int part, const WireLook& wl, ID2D1StrokeStyle* round) {
    auto glass = MakePath(c.factory, sh.glass, true);
    if (!glass) return;
    switch (part) {
    case kSocket: DrawSocket(c, sh, wl); break;
    case kGlassDark: DrawGlassDark(c, sh, glass.Get()); break;
    case kGlassLit: DrawGlassLit(c, sh, glass.Get()); break;
    case kCore: DrawCore(c, sh, glass.Get()); break;
    case kFilament: DrawFilament(c, sh, round); break;
    case kGleam: DrawGleam(c, sh, glass.Get()); break;
    case kTube: DrawTube(c, sh); break;
    case kLiquid: DrawLiquid(c, sh); break;
    case kBubble: DrawBubble(c, sh); break;
    case kFlame: DrawFlame(c, sh); break;
    default: break;
    }
}

// Which parts a type has.
bool HasPart(BulbType t, int part) {
    switch (part) {
    case kTube:
    case kLiquid:
    case kBubble: return t == BulbType::Bubble;
    case kFlame: return t == BulbType::Candle;
    case kFilament: return t != BulbType::Bubble && t != BulbType::Candle;
    default: return true;
    }
}

// One part of a bulb as drawn in a frame: its tint and opacity, and (for the
// parts that move on their own) where its anchor sits, its scale and turn.
struct Layer {
    int part = kSocket;
    Rgb tint = kWhite;
    float alpha = 1.0f;
    float dx = 0.0f, dy = 0.0f;  // shape px
    float kx = 1.0f, ky = 1.0f, turn = 0.0f;
};
constexpr int kMaxLayers = 16;
constexpr int kBubbles = 3;          // in a tube at once
constexpr float kBubbleRate = 0.8f;  // tube lengths a second each bubble rises

// A bulb's layers, back to front.
int BulbLayers(const Shape& sh, const BulbColor& col, bool led, float level, float heat, float anim, uint32_t seed,
               Layer* out) {
    int n = 0;
    auto add = [&](int part, Rgb tint, float alpha) -> Layer* {
        if (n >= kMaxLayers || !HasPart(sh.type, part) || alpha <= 0.0f) return nullptr;
        out[n] = {part, tint, alpha};
        return &out[n++];
    };
    const BulbType t = sh.type;
    const float v = Saturate(level);
    const Rgb lit = led ? col.glass : Warm(col.glass, heat);
    const Form& f = kForms[static_cast<int>(t)];
    add(kGlassDark, Scale(Mix(col.glass, kWhite, f.clear), 0.62f), 1.0f);
    add(kGlassLit, Mix(lit, kWhite, 0.6f * f.clear), f.lit * v);
    add(kCore, Mix(lit, kWhite, 0.5f), f.core * v);
    if (led) add(kFilament, Mix(kWhite, col.light, 0.2f), 0.15f + 0.85f * v);
    else add(kFilament, FilamentColor(heat), 0.35f + 0.65f * SmoothStep(0.0f, 0.3f, heat));
    if (t == BulbType::Bubble) {
        add(kTube, kWhite, 1.0f);
        add(kLiquid, Scale(Mix(lit, kWhite, 0.3f), 0.5f + 0.5f * v), 0.92f);
        // Bubbles stream up the tube once it has warmed (anim: the time they
        // have run), each wobbling a little, fading in and bursting at the top.
        const float run = std::max(anim, 0.0f);
        const float on = SmoothStep(0.0f, 0.6f, run) * SmoothStep(0.15f, 0.5f, heat);
        const float from = sh.len - 1.3f * sh.tubeW, to = sh.end + 0.85f * sh.tubeW;
        for (int i = 0; i < kBubbles && on > 0.0f; ++i) {
            const float jitter = 0.16f * (HashFloat(seed + static_cast<uint32_t>(i) * 977u) - 0.5f);
            float ph = run * kBubbleRate + (static_cast<float>(i) + jitter) / static_cast<float>(kBubbles);
            ph -= std::floor(ph);
            const float show = SmoothStep(0.0f, 0.12f, ph) * SmoothStep(1.0f, 0.86f, ph);
            if (Layer* L = add(kBubble, kWhite, on * show)) {
                L->dx = 0.07f * sh.tubeW * std::sin(7.0f * run + 2.1f * static_cast<float>(i));
                L->dy = Lerp(from, to, ph);
                L->kx = L->ky = (0.55f + 0.45f * ph) * (0.8f + 0.4f * HashFloat(seed * 7u + static_cast<uint32_t>(i)));
            }
        }
    }
    if (t == BulbType::Candle) {
        // The flame flickers: it stretches, shrinks and leans as it dances.
        const float flicker = Clamp(anim, -1.0f, 1.0f);
        // Cold, it is a dull amber plate; warm, it glows nearly its own colours.
        const Rgb warmth = Mix(Grey(0.3f), Mix(kWhite, col.light, 0.12f), SmoothStep(0.0f, 0.5f, heat));
        if (Layer* L = add(kFlame, warmth, 1.0f)) {
            L->dy = sh.flameTop;
            L->kx = 1.0f - 0.06f * flicker;
            L->ky = 1.0f + 0.12f * flicker;
            L->turn = 0.07f * flicker;
        }
    }
    add(kGleam, kWhite, 0.55f - 0.25f * v);
    add(kSocket, kWhite, 1.0f);
    return n;
}

// ---------------------------------------------------------------------------
// Sprites
// ---------------------------------------------------------------------------

// A page cell: where it is, and the anchor its art was drawn around (page px).
struct Cell {
    D2D1_RECT_U src{};
    float cx = 0.0f, cy = 0.0f;
};

// Turned by `angle` (clockwise), mirrored across its x axis first if `flip`, and placed.
D2D1_MATRIX_3X2_F Placed(float angle, float x, float y, bool flip = false) {
    const float c = std::cos(angle), s = std::sin(angle);
    return flip ? D2D1_MATRIX_3X2_F{c, s, s, -c, x, y} : D2D1_MATRIX_3X2_F{c, s, -s, c, x, y};
}

// A cell around its anchor, scaled by (kx, ky), then transformed.
ScenerySprite Quad(const Cell& cell, float kx, float ky, const D2D1_MATRIX_3X2_F& xf, D2D1_COLOR_F col) {
    return {{(static_cast<float>(cell.src.left) - cell.cx) * kx, (static_cast<float>(cell.src.top) - cell.cy) * ky,
             (static_cast<float>(cell.src.right) - cell.cx) * kx, (static_cast<float>(cell.src.bottom) - cell.cy) * ky},
            cell.src,
            col,
            xf};
}

// Smallest level at least `d` px across (levels shrink from the first).
int PickLevel(const float* px, int count, float d) {
    int l = 0;
    while (l + 1 < count && px[l + 1] >= d) ++l;
    return l;
}

// Px per DIP a page draws bulbs at: the DPI, with Size to the nearest step.
float PageScale(const LightParams& p, const LightsLayout& l) {
    const float steps = std::round(kSizeSteps * std::log2(SizeScale(p.size)));
    return std::max(l.dpiScale, 0.25f) * std::exp2(steps / kSizeSteps);
}

// The system's accent, lightened for a dark screen as Windows does.
Rgb Accent() {
    DWORD abgr = 0, size = sizeof(abgr);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD,
                     nullptr, &abgr, &size) == ERROR_SUCCESS)
        return Mix({(abgr & 0xFF) / 255.0f, ((abgr >> 8) & 0xFF) / 255.0f, ((abgr >> 16) & 0xFF) / 255.0f}, kWhite, 0.45f);
    return Hex(0x60CDFF);
}

}  // namespace

struct Lights::Look {
    LightParams params;
    LightsLayout layout;
    bool additive = true;
    // The page, and what it was drawn for.
    ComPtr<ID2D1Bitmap1> page;
    bool stale = true;
    double retry = 0.0;
    float q = 1.0f;                  // px per DIP the bulb was drawn at
    Shape shape;                     // ... and its shape at that scale
    Cell part[kPartCount];
    Cell glow[kGlowLevels];
    float glowPx[kGlowLevels] = {};  // each glow's disc, px across
    Cell wire, sheen;                // three twists of wire (sprites use the middle one) and the light along it
    float wirePx = 1.0f;             // the wire's thickness in its cell
    int twist = 4;                   // px: one twist
    Cell joint;                      // the wire seen end on: closes the outside of a sharp bend
    Cell bar, solid, ring, ringEnd, disc;
    float barPx = 1.0f;              // the bar's thickness
    float discPx = 1.0f;             // the disc's diameter
    Rgb accent = Hex(0x60CDFF);
    std::vector<float> arc;          // scratch: arc length along a wire
};

void Lights::LookDeleter::operator()(Look* look) const { delete look; }

Lights::Look* lights::NewLook() { return new Lights::Look(); }

void lights::LookSetParams(Lights::Look& look, const LightParams& params, const LightsLayout& layout) {
    // Size redraws the page only as it moves a whole step.
    if (params.bulb != look.params.bulb || params.wire != look.params.wire || layout.dpiScale != look.layout.dpiScale ||
        PageScale(params, layout) != PageScale(look.params, look.layout))
        look.stale = true;
    look.params = params;
    look.layout = layout;
}

namespace {

// The wire's cell: three twists of a round, twisted pair (just a round wire
// when thin), in its true colours.
void DrawWireCell(const Canvas& c, const Lights::Look& k, const WireLook& wl) {
    const float h = 0.5f * k.wirePx, P = static_cast<float>(k.twist), L = 3.0f * P;
    ComPtr<ID2D1RectangleGeometry> band;
    c.factory->CreateRectangleGeometry(D2D1::RectF(0.0f, -h, L, h), &band);
    if (!band) return;
    if (auto g = Linear(*c.stops, {0, -h}, {0, h},
                        {c.Stop(0.0f, wl.edge, 1), c.Stop(0.3f, wl.body, 1), c.Stop(0.55f, wl.body, 1), c.Stop(1.0f, wl.edge, 1)}))
        c.rt->FillGeometry(band.Get(), g.Get());
    const float twist = SmoothStep(kTwistFrom, kTwistFull, k.wirePx);
    if (twist <= 0.0f) return;
    // The two strands wind round each other: seams slant across the wire
    // every half twist, each strand bulging (and catching the light) between.
    c.Clip(band.Get());
    D2D1::Matrix3x2F was;
    c.rt->GetTransform(&was);
    const float slant = 50.0f;  // degrees the seams lean from square across the wire
    for (int i = -2; i <= 7; ++i) {
        const D2D1_POINT_2F p = {(static_cast<float>(i) + 0.5f) * 0.5f * P, 0.0f};
        c.rt->SetTransform(D2D1::Matrix3x2F::Rotation(-slant, p) * was);
        if (auto g = Radial(*c.stops, {p.x, p.y - 0.35f * h}, 0.4f * h, 0.3f * P,
                            {c.Stop(0.0f, wl.lobe, 0.55f * twist), c.Stop(0.6f, wl.lobe, 0.2f * twist), c.Stop(1.0f, wl.lobe, 0.0f)}))
            c.rt->FillEllipse(D2D1::Ellipse({p.x, p.y - 0.35f * h}, 0.4f * h, 0.3f * P), g.Get());
        const D2D1_POINT_2F q = {p.x + 0.25f * P, 0.0f};
        c.rt->SetTransform(D2D1::Matrix3x2F::Rotation(-slant, q) * was);
        c.rt->DrawLine({q.x, -2.0f * h}, {q.x, 2.0f * h}, c.Solid(wl.groove, 0.32f * twist), std::max(0.6f, 0.07f * P));
    }
    c.rt->SetTransform(was);
    c.rt->PopLayer();

}

HRESULT BuildPage(Lights::Look& k, ID2D1DeviceContext* dc) {
    ComPtr<ID2D1Device> device;
    dc->GetDevice(&device);
    if (!device) return E_FAIL;
    // A private context on the same device: the caller is mid-frame.
    ComPtr<ID2D1DeviceContext> rdc;
    HRESULT hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &rdc);
    if (FAILED(hr)) return hr;
    ComPtr<ID2D1Factory> factory;
    rdc->GetFactory(&factory);
    if (!factory) return E_FAIL;
    rdc->SetDpi(96.0f, 96.0f);
    rdc->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    rdc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    const BulbType type = k.params.bulb;
    const BulbSpec& spec = Spec(type);
    const float s = std::max(k.layout.dpiScale, 0.25f);
    k.q = PageScale(k.params, k.layout);
    k.shape = MakeShape(type, k.q);
    const Shape& sh = k.shape;
    k.accent = Accent();

    // ---- Plan the cells ------------------------------------------------------
    struct Box {
        Cell* cell;
        int w, h;      // incl. padding
        float ax, ay;  // anchor inside the box
        int kind, index;
    };
    enum { kBody, kGlowDisc, kWireCell, kSheenCell, kJointCell, kBarCell, kSolidCell, kRingCell, kDiscCell };
    std::vector<Box> boxes;
    auto box = [&](Cell* cell, int kind, int index, float left, float top, float right, float bottom) {
        // Extents around the anchor (px), padded; the anchor lands on a whole pixel.
        const int l = static_cast<int>(std::ceil(left)) + kPad, t = static_cast<int>(std::ceil(top)) + kPad;
        const int r = static_cast<int>(std::ceil(right)) + kPad, b = static_cast<int>(std::ceil(bottom)) + kPad;
        boxes.push_back({cell, l + r, t + b, static_cast<float>(l), static_cast<float>(t), kind, index});
    };
    // The bulb's parts, each around the socket's top.
    const float half = 0.5f * std::max(sh.w, sh.sockTop) + 1.5f;
    for (int p = 0; p < kPartCount; ++p) {
        k.part[p] = {};
        if (!HasPart(type, p)) continue;
        const float fw = 0.6f * sh.flameW + 1.5f, br = sh.bubbleR + 1.5f;
        if (p == kFlame) box(&k.part[p], kBody, p, fw, 1.5f, fw, sh.flameLen + 1.5f);
        else if (p == kBubble) box(&k.part[p], kBody, p, br, br, br, br);
        else box(&k.part[p], kBody, p, half, 1.5f, half, sh.len + 1.5f);
    }
    // Glows: as big as a bloom gets, and a quarter of that for the hot cores;
    // clear all round, the falloff ending well inside the cell.
    k.glowPx[0] = std::ceil(Clamp(10.0f * spec.width * k.q, 64.0f, 512.0f));
    k.glowPx[1] = std::ceil(0.25f * k.glowPx[0]);
    for (int l = 0; l < kGlowLevels; ++l) {
        const float r = 0.5f * k.glowPx[l] + 2.0f;
        box(&k.glow[l], kGlowDisc, l, r, r, r, r);
    }
    k.wirePx = std::max(1.0f, spec.wire * k.q);
    k.twist = std::max(4, static_cast<int>(std::lround(2.6f * k.wirePx)));
    const float wh = 0.5f * k.wirePx + 1.0f;
    box(&k.wire, kWireCell, 0, 0.0f, wh, 3.0f * static_cast<float>(k.twist), wh);
    box(&k.sheen, kSheenCell, 0, 0.0f, wh, 3.0f * static_cast<float>(k.twist), wh);
    box(&k.joint, kJointCell, 0, wh, wh, wh, wh);
    k.barPx = std::ceil(k.wirePx + 2.0f * kOutline * s);
    box(&k.bar, kBarCell, 0, 0.0f, 0.5f * k.barPx + 1.0f, 8.0f, 0.5f * k.barPx + 1.0f);
    box(&k.solid, kSolidCell, 0, 0.0f, 0.0f, 8.0f, 8.0f);
    const float re = 0.5f * kRingEnd * s + 2.0f, rn = 0.5f * kRing * s + 2.0f;
    box(&k.ring, kRingCell, 0, rn, rn, rn, rn);
    box(&k.ringEnd, kRingCell, 1, re, re, re, re);
    k.discPx = (kRingEnd - 3.2f) * s;  // fills a ring to the middle of its stroke
    box(&k.disc, kDiscCell, 0, re, re, re, re);

    // Shelf-pack, tallest first.
    std::vector<size_t> order(boxes.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return boxes[a].h > boxes[b].h; });
    int widest = 0;
    for (const Box& b : boxes) widest = std::max(widest, b.w);
    const UINT32 pageW = widest > 1024 ? 2048u : (widest > 512 ? 1024u : 512u);
    UINT32 x = 0, y = 0, row = 0;
    for (size_t i : order) {
        Box& b = boxes[i];
        if (x + static_cast<UINT32>(b.w) > pageW) {
            x = 0;
            y += row;
            row = 0;
        }
        b.cell->src = {x, y, x + static_cast<UINT32>(b.w), y + static_cast<UINT32>(b.h)};
        b.cell->cx = static_cast<float>(x) + b.ax;
        b.cell->cy = static_cast<float>(y) + b.ay;
        x += static_cast<UINT32>(b.w);
        row = std::max(row, static_cast<UINT32>(b.h));
    }
    const UINT32 pageH = y + row;
    if (pageW > rdc->GetMaximumBitmapSize() || pageH > rdc->GetMaximumBitmapSize() || pageH == 0) return E_OUTOFMEMORY;

    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f,
        96.0f);
    ComPtr<ID2D1Bitmap1> page;
    if (FAILED(hr = rdc->CreateBitmap(D2D1::SizeU(pageW, pageH), nullptr, 0, props, &page))) return hr;
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(hr = rdc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush))) return hr;
    ComPtr<ID2D1StrokeStyle> round;
    factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                           D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                               nullptr, 0, &round);
    Stops stops(rdc.Get());
    const Canvas c{rdc.Get(), factory.Get(), brush.Get(), &stops};
    const WireLook& wl = WireStyle(k.params.wire);

    // ---- Draw ------------------------------------------------------------------
    rdc->SetTarget(page.Get());
    rdc->BeginDraw();
    rdc->Clear(D2D1::ColorF(0, 0, 0, 0));
    for (const Box& b : boxes) {
        const D2D1_RECT_U& rc = b.cell->src;
        rdc->SetTransform(D2D1::Matrix3x2F::Identity());
        const float cb = static_cast<float>(kClearBorder);
        rdc->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(rc.left) + cb, static_cast<float>(rc.top) + cb,
                                             static_cast<float>(rc.right) - cb, static_cast<float>(rc.bottom) - cb),
                                 D2D1_ANTIALIAS_MODE_ALIASED);
        rdc->SetTransform(D2D1::Matrix3x2F::Translation(b.cell->cx, b.cell->cy));
        switch (b.kind) {
        case kBody: DrawPart(c, sh, b.index, wl, round.Get()); break;
        case kGlowDisc: {
            // A soft disc that tints to add light.
            const float r = 0.5f * k.glowPx[b.index];
            D2D1_GRADIENT_STOP list[kGlowStops];
            for (int i = 0; i < kGlowStops; ++i) {
                const float u = static_cast<float>(i) / static_cast<float>(kGlowStops - 1);
                list[i] = {u, D2D1::ColorF(1, 1, 1, GlowFalloff(u))};
            }
            ComPtr<ID2D1RadialGradientBrush> g;
            if (ID2D1GradientStopCollection* coll = stops.Get(list, kGlowStops))
                rdc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties({0, 0}, {0, 0}, r, r), coll, &g);
            if (g) rdc->FillEllipse(D2D1::Ellipse({0, 0}, r, r), g.Get());
            break;
        }
        case kWireCell: DrawWireCell(c, k, wl); break;
        case kSheenCell: {
            // A fine line of light along the top of the wire.
            const float h = 0.5f * k.wirePx, L = 3.0f * static_cast<float>(k.twist);
            if (auto g = Linear(stops, {0, -h}, {0, 0},
                                {c.Stop(0.0f, kWhite, 0.0f), c.Stop(0.25f, kWhite, 0.9f), c.Stop(0.55f, kWhite, 0.25f),
                                 c.Stop(1.0f, kWhite, 0.0f)}))
                rdc->FillRectangle(D2D1::RectF(0.0f, -h, L, 0.0f), g.Get());
            break;
        }
        case kJointCell: {
            // The wire end on, round.
            const float r = 0.5f * k.wirePx;
            if (auto g = Radial(stops, {0, 0}, r, r,
                                {c.Stop(0.0f, wl.body, 1), c.Stop(0.5f, wl.body, 1), c.Stop(1.0f, wl.edge, 1)}))
                rdc->FillEllipse(D2D1::Ellipse({0, 0}, r, r), g.Get());
            break;
        }
        case kBarCell:
            rdc->FillRectangle(D2D1::RectF(-1.0f, -0.5f * k.barPx, 9.0f, 0.5f * k.barPx), c.Solid(kWhite, 1));
            break;
        case kSolidCell: rdc->FillRectangle(D2D1::RectF(-1.0f, -1.0f, 9.0f, 9.0f), c.Solid(kWhite, 1)); break;
        case kRingCell: {
            // A white ring with a dark outline (it reads on any wallpaper) and a dot on the hook itself.
            const float r = 0.5f * (b.index ? kRingEnd : kRing) * s - 1.6f * s, sw = 2.0f * s;
            rdc->DrawEllipse(D2D1::Ellipse({0, 0}, r, r), c.Solid(Grey(0.0f), 0.5f), sw + 2.0f * s);
            rdc->DrawEllipse(D2D1::Ellipse({0, 0}, r, r), c.Solid(kWhite, 1.0f), sw);
            rdc->FillEllipse(D2D1::Ellipse({0, 0}, 1.6f * s, 1.6f * s), c.Solid(Grey(0.0f), 0.45f));
            rdc->FillEllipse(D2D1::Ellipse({0, 0}, 1.0f * s, 1.0f * s), c.Solid(kWhite, 1.0f));
            break;
        }
        case kDiscCell:
            rdc->FillEllipse(D2D1::Ellipse({0, 0}, 0.5f * k.discPx, 0.5f * k.discPx), c.Solid(kWhite, 1.0f));
            break;
        default: break;
        }
        rdc->SetTransform(D2D1::Matrix3x2F::Identity());
        rdc->PopAxisAlignedClip();
    }
    if (FAILED(hr = rdc->EndDraw())) return hr;
    rdc->SetTarget(nullptr);

    // The cells drawn edge to edge are sampled in their middle only: the wire
    // from one twist in (its anchor) to the next, so twists join seamlessly.
    auto middle = [](Cell& cell, int from, int to) {
        cell.src.left = static_cast<UINT32>(cell.cx) + static_cast<UINT32>(from);
        cell.src.right = static_cast<UINT32>(cell.cx) + static_cast<UINT32>(to);
        cell.cx += static_cast<float>(from);
    };

    middle(k.wire, k.twist, 2 * k.twist);
    middle(k.sheen, k.twist, 2 * k.twist);
    middle(k.bar, 3, 5);
    middle(k.solid, 3, 5);
    k.solid.src.top = static_cast<UINT32>(k.solid.cy) + 3u;
    k.solid.src.bottom = k.solid.src.top + 2u;
    k.solid.cy = static_cast<float>(k.solid.src.top);
    k.page = page;
    return S_OK;
}

}  // namespace

void Lights::SetAdditiveGlow(bool on) { look_->additive = on; }

ID2D1Bitmap1* Lights::Page() const { return look_->page.Get(); }

void Lights::ReleaseDeviceResources() {
    look_->page.Reset();
    look_->stale = true;
}

bool Lights::Prepare(ID2D1DeviceContext* dc) {
    Look& k = *look_;
    if (!Active() || !dc) return false;
    if (k.page && !k.stale) return true;
    const double now = QpcSeconds();
    if (now < k.retry) return false;
    k.page.Reset();
    if (FAILED(BuildPage(k, dc))) {
        k.page.Reset();
        k.retry = now + 1.0;
        return false;
    }
    k.stale = false;
    return true;
}

namespace {

// Walks a wire in pieces of about `piece` px, calling `emit(a, b, alpha)` for
// each. Each run between sharp bends (hooks) takes a whole number of pieces, so
// a pattern runs on unbroken along it and the corners stay where they are.
template <typename Emit>
void WalkWire(std::vector<float>& arc, const WireDraw& w, float piece, Emit emit) {
    const size_t n = std::min(w.pts.size(), w.alpha.size());
    if (n < 2 || piece <= 0.0f) return;
    arc.resize(n);
    arc[0] = 0.0f;
    for (size_t i = 1; i < n; ++i)
        arc[i] = arc[i - 1] + std::hypot(w.pts[i].x - w.pts[i - 1].x, w.pts[i].y - w.pts[i - 1].y);
    if (!(arc[n - 1] > 0.5f)) return;
    auto sharp = [&](size_t i) {
        const float ax = w.pts[i].x - w.pts[i - 1].x, ay = w.pts[i].y - w.pts[i - 1].y;
        const float bx = w.pts[i + 1].x - w.pts[i].x, by = w.pts[i + 1].y - w.pts[i].y;
        return std::fabs(std::atan2(ax * by - ay * bx, ax * bx + ay * by)) > 0.2f;
    };
    size_t i = 0;
    auto at = [&](float d, float& alpha) {
        while (i + 2 < n && arc[i + 1] < d) ++i;
        const float t = Saturate((d - arc[i]) / std::max(arc[i + 1] - arc[i], 1e-4f));
        alpha = Lerp(w.alpha[i], w.alpha[i + 1], t);
        return D2D1_POINT_2F{Lerp(w.pts[i].x, w.pts[i + 1].x, t), Lerp(w.pts[i].y, w.pts[i + 1].y, t)};
    };
    size_t from = 0;
    for (size_t to = 1; to < n; ++to) {
        if (to + 1 < n && !sharp(to)) continue;
        const float start = arc[from], len = arc[to] - start;
        if (len > 1e-3f) {
            const int pieces = std::max(1, static_cast<int>(std::lround(len / piece)));
            float a0 = 0.0f;
            i = from;
            D2D1_POINT_2F p0 = at(start, a0);
            for (int j = 1; j <= pieces; ++j) {
                float a1 = 0.0f;
                const D2D1_POINT_2F p1 = at(start + len * static_cast<float>(j) / static_cast<float>(pieces), a1);
                emit(p0, p1, 0.5f * (a0 + a1));
                p0 = p1;
                a0 = a1;
            }
        }
        from = to;
    }
}

}  // namespace

void Lights::EmitVeil(std::vector<ScenerySprite>& out) const {
    const Look& k = *look_;
    const Frame& f = FrameOf(*engine_);
    if (!k.page || f.veil <= 0.0f) return;
    const float w = static_cast<float>(k.layout.width) / 2.0f, h = static_cast<float>(k.layout.height) / 2.0f;
    out.push_back(Quad(k.solid, w, h, Placed(0.0f, 0.0f, 0.0f), {0.0f, 0.01f, 0.03f, f.veil}));
}

void Lights::EmitBody(std::vector<ScenerySprite>& out) const {
    Look& k = *look_;
    const Frame& f = FrameOf(*engine_);
    if (!k.page) return;
    const WireLook& wl = WireStyle(k.params.wire);
    const float P = static_cast<float>(k.twist);
    for (const WireDraw& w : f.wires) {
        const float ky = w.width / k.wirePx;
        // Hanging mode: the strand under the pointer is outlined in the accent,
        // a wider band under its wire.
        const bool hovered = f.editing && w.strand == f.hoverStrand;
        const float band = w.width + 2.0f * kOutline * k.layout.dpiScale;
        for (int pass = hovered ? 0 : 1; pass < 2; ++pass) {
            // The wire, a twist per sprite laid end to end, and the light along
            // whichever edge faces up. Where it curves, pieces butted end to
            // end would leave slivers outside the curve: each reaches a hair
            // past its ends (more would show as beads where the wire is faint).
            // A sharp bend (at a hook) leaves a notch: a round joint fills it.
            const float over = 0.1f * (pass == 0 ? band : w.width);
            float before = 0.0f;
            bool first = true;
            WalkWire(k.arc, w, P * ky, [&](D2D1_POINT_2F a, D2D1_POINT_2F b, float alpha) {
                const float dx = b.x - a.x, dy = b.y - a.y, d = std::hypot(dx, dy);
                if (d < 1e-3f) return;
                const float angle = std::atan2(dy, dx), ux = dx / d, uy = dy / d;
                const bool sharp = !first && std::fabs(std::remainder(angle - before, kTau)) > 0.2f;
                first = false;
                before = angle;
                if (alpha <= 0.004f) return;
                const D2D1_MATRIX_3X2_F xf = Placed(angle, a.x - over * ux, a.y - over * uy);
                if (pass == 0) {
                    const D2D1_COLOR_F col = Color(k.accent, alpha);
                    if (sharp) out.push_back(Quad(k.disc, band / k.discPx, band / k.discPx, Placed(0.0f, a.x, a.y), col));
                    out.push_back(Quad(k.bar, (d + 2.0f * over) / 2.0f, band / k.barPx, xf, col));
                    return;
                }
                const float kx = (d + 2.0f * over) / P;
                if (sharp) out.push_back(Quad(k.joint, ky, ky, Placed(0.0f, a.x, a.y), {1.0f, 1.0f, 1.0f, alpha}));
                out.push_back(Quad(k.wire, kx, ky, xf, {1.0f, 1.0f, 1.0f, alpha}));
                const float up = std::fabs(dx) / d;
                if (up > 0.05f)
                    out.push_back(Quad(k.sheen, kx, ky, Placed(angle, a.x - over * ux, a.y - over * uy, dx < 0.0f),
                                       Color(wl.sheen, 0.3f * up * up * alpha)));
            });

        }
    }
    const bool led = Spec(k.params.bulb).led;
    Layer layers[kMaxLayers];
    for (const BulbDraw& b : f.bulbs) {
        if (b.alpha <= 0.004f) continue;
        const float kk = b.scale / k.q;
        const float c = std::cos(b.angle), s = std::sin(b.angle);
        const int n = BulbLayers(k.shape, PaletteColor(k.params, b.color, led), led, b.level, b.heat, b.anim, b.seed, layers);
        for (int i = 0; i < n; ++i) {
            const Layer& L = layers[i];
            const float a = L.alpha * b.alpha;
            const Cell& cell = k.part[L.part];
            if (a <= 0.004f || cell.src.right <= cell.src.left) continue;
            const float lx = L.dx * kk, ly = L.dy * kk;
            out.push_back(Quad(cell, kk * L.kx, kk * L.ky, Placed(b.angle + L.turn, b.x + c * lx - s * ly, b.y + s * lx + c * ly),
                               Color(L.tint, a)));
        }
    }
}

void Lights::EmitGlow(std::vector<ScenerySprite>& out) const {
    const Look& k = *look_;
    const Frame& f = FrameOf(*engine_);
    if (!k.page || k.params.glow <= 0) return;
    const BulbSpec& spec = Spec(k.params.bulb);
    const bool mist = !k.additive;
    const float veil = mist ? 1.0f : kGlowVeil;
    // Strength rises with the setting, easing off at the top so it never
    // floods to white; the halos spread a little too.
    const float g = static_cast<float>(k.params.glow) / kGlowTuned;
    const float strength = g < 1.0f ? g : 1.0f + 0.6f * (1.0f - std::exp(-(g - 1.0f) * 1.6f));
    const float spread = 0.85f + 0.15f * g;
    auto glow = [&](float x, float y, float across, float along, float angle, Rgb col, float a) {
        if (a <= 0.004f || across <= 0.5f) return;
        const int l = PickLevel(k.glowPx, kGlowLevels, std::max(across, along));
        out.push_back(Quad(k.glow[l], across / k.glowPx[l], along / k.glowPx[l], Placed(angle, x, y), Glow(col, a, veil)));
    };
    for (const BulbDraw& b : f.bulbs) {
        const float v = b.level * b.alpha;
        if (v <= 0.004f) continue;
        const float c = std::cos(b.angle), s = std::sin(b.angle);
        // The light comes from the glass; the socket shades what would go up,
        // so the halo and bloom sit a little lower.
        const float x = b.x - s * spec.glassCenter * b.scale, y = b.y + c * spec.glassCenter * b.scale;
        const float low = 0.2f * spec.length * b.scale, lx = x - s * low, ly = y + c * low;
        const BulbColor col = PaletteColor(k.params, b.color, spec.led);
        const Rgb light = spec.led ? col.light : Warm(col.light, b.heat);
        const float w = spec.width * kForms[static_cast<int>(k.params.bulb)].glow * b.scale * spread;
        const float stretch = 1.0f + 0.22f * std::max(0.0f, spec.length / spec.width - 1.2f);
        if (mist) {
            // Ordinary blending can't brighten: a soft coloured mist, a paler heart.
            glow(lx, ly, 8.0f * w, 8.0f * w, b.angle, light, kMistAlpha * 0.2f * strength * v);
            glow(lx, ly, 4.4f * w, 4.4f * w * stretch, b.angle, Mix(light, kWhite, 0.1f), kMistAlpha * 0.5f * strength * v);
            glow(x, y, 1.6f * w, 1.6f * w * stretch, b.angle, Mix(light, kWhite, 0.5f), 0.2f * strength * v);
            continue;
        }
        glow(lx, ly, 10.0f * w, 10.0f * w, b.angle, light, 0.2f * strength * v);                      // wide bloom
        glow(lx, ly, 5.6f * w, 5.6f * w * stretch, b.angle, light, 0.5f * strength * v);              // halo
        glow(x, y, 1.8f * w, 1.8f * w * stretch, b.angle, Mix(light, kWhite, 0.15f), 0.22f * strength * v);  // hot core
    }
}

void Lights::EmitEdit(std::vector<ScenerySprite>& out) const {
    const Look& k = *look_;
    const Frame& f = FrameOf(*engine_);
    if (!k.page || !f.editing) return;
    const float s = k.layout.dpiScale;
    const float W = static_cast<float>(k.layout.width), H = static_cast<float>(k.layout.height);
    // A bar on each screen edge a point snapped to.
    auto snaps = [&](float x, float y, int snap, float alpha) {
        const float len = kSnapBar * s, t = kSnapThick * s / k.barPx, in = 0.5f * kSnapThick * s;
        const D2D1_COLOR_F col = Color(k.accent, alpha);
        if (snap & 1) out.push_back(Quad(k.bar, len / 2.0f, t, Placed(0.5f * kPi, in, y - 0.5f * len), col));
        if (snap & 4) out.push_back(Quad(k.bar, len / 2.0f, t, Placed(0.5f * kPi, W - in, y - 0.5f * len), col));
        if (snap & 2) out.push_back(Quad(k.bar, len / 2.0f, t, Placed(0.0f, x - 0.5f * len, in), col));
        if (snap & 8) out.push_back(Quad(k.bar, len / 2.0f, t, Placed(0.0f, x - 0.5f * len, H - in), col));
    };
    for (const HookDraw& h : f.hooks) {
        snaps(h.x, h.y, h.snap, 0.9f);
        const Cell& ring = h.end ? k.ringEnd : k.ring;
        const float grow = h.hovered ? 1.15f : 1.0f;
        if (h.hovered || h.end) {
            const float d = (h.end ? kRingEnd - 3.2f : kRing - 3.2f) * s / k.discPx * grow;
            out.push_back(Quad(k.disc, d, d, Placed(0.0f, h.x, h.y), Color(k.accent, h.hovered ? 0.95f : 0.6f)));
        }
        out.push_back(Quad(ring, grow, grow, Placed(0.0f, h.x, h.y), {1.0f, 1.0f, 1.0f, 1.0f}));
    }
    if (f.cursorValid) {
        // Where the next hook would go: a ghost of one.
        snaps(f.cursor.x, f.cursor.y, f.cursorSnap, 0.6f);
        out.push_back(Quad(k.ring, 1.0f, 1.0f, Placed(0.0f, f.cursor.x, f.cursor.y), {1.0f, 1.0f, 1.0f, 0.55f}));
    }
}

void Lights::DrawIcon(ID2D1RenderTarget* rt, BulbType type, D2D1_POINT_2F anchor, float length, D2D1_COLOR_F color,
                      bool lit) {
    if (!rt || !(length > 1.0f)) return;
    const BulbSpec& spec = Spec(type);
    const Shape sh = MakeShape(type, length / spec.length);
    ComPtr<ID2D1Factory> factory;
    rt->GetFactory(&factory);
    ComPtr<ID2D1SolidColorBrush> brush;
    if (!factory || FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush))) return;
    ComPtr<ID2D1StrokeStyle> round;
    factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                           D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                               nullptr, 0, &round);
    Stops stops(rt);
    Canvas c{rt, factory.Get(), brush.Get(), &stops};
    D2D1::Matrix3x2F was;
    rt->GetTransform(&was);
    const Rgb glass = {color.r, color.g, color.b};
    const float top = std::max({glass.r, glass.g, glass.b});
    const BulbColor col = {glass, top > 0.0f ? Scale(glass, 1.0f / top) : kWarmGlow.light};
    if (lit) {
        // A soft halo behind it (on the tile, ordinary blending).
        const D2D1_POINT_2F p = {anchor.x, anchor.y + sh.cy};
        const float rx = 1.3f * sh.w, ry = 1.3f * sh.w + 0.3f * (sh.len - sh.w);
        rt->SetTransform(was);
        if (auto g = Radial(stops, p, rx, ry,
                            {c.Stop(0.0f, col.light, 0.45f * color.a), c.Stop(0.4f, col.light, 0.2f * color.a),
                             c.Stop(1.0f, col.light, 0.0f)}))
            rt->FillEllipse(D2D1::Ellipse(p, rx, ry), g.Get());
    }
    Layer layers[kMaxLayers];
    const float level = lit ? 1.0f : 0.0f;
    const int n = BulbLayers(sh, col, spec.led, level, level, lit ? 0.6f : 0.0f, 7u, layers);
    for (int i = 0; i < n; ++i) {
        const Layer& L = layers[i];
        c.tint = L.tint;
        c.opacity = L.alpha * color.a;
        if (c.opacity <= 0.004f) continue;
        rt->SetTransform(D2D1::Matrix3x2F::Scale(L.kx, L.ky) * D2D1::Matrix3x2F::Rotation(L.turn * 180.0f / kPi) *
                         D2D1::Matrix3x2F::Translation(anchor.x + L.dx, anchor.y + L.dy) * was);
        DrawPart(c, sh, L.part, WireStyle(WireColor::Green), round.Get());
    }
    rt->SetTransform(was);
}
