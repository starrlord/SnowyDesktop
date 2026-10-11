// Scenery internals shared by Scenery.cpp (simulation, drawing) and
// SceneryArt.cpp (the sprite page): tunables, palette, shapes and the props.
#pragma once

#include "Scenery.h"

#include <initializer_list>

namespace scenery {

// ---------------------------------------------------------------------------
// Tunables. Lengths are DIPs (scaled by dpiScale where used) or fractions of a
// prop's height H, so the scenery looks the same at every display scale.
// ---------------------------------------------------------------------------

// Layout.
constexpr float kSpotSpacing = 820.0f;     // one group of trees per this much screen width (usual amount)
constexpr float kSnowmanSpacing = 1600.0f; // one snowman per this much (usual amount)
constexpr float kAmountDouble = 30.0f;     // slider steps that double the count
constexpr int kMaxGroups = 12, kMaxTrees = 26, kMaxSnowmen = 10;
constexpr float kMaxTree = 96.0f;          // tallest tree, base to tip (the star sits above)
constexpr float kMaxSnowman = 70.0f;       // tallest snowman, base to the top of the hat
constexpr float kSkyShare = 0.5f;          // props stay under this share of the height above the ground
constexpr float kMinTree = 30.0f;          // rather no props than smaller ones

// Tree shape. Foliage starts above a short stretch of trunk; tiers are skirts,
// steeper under their point and flatter at the rim, where the snow lies.
constexpr float kTrunkShow = 0.075f;  // of H: trunk between the base and the bottom tier's tips
constexpr float kWidth = 0.74f;       // foliage width / foliage height (varies by kWidthVary)
constexpr float kWidthVary = 0.06f;
constexpr float kTierSpread = 0.92f;  // tier bottoms sit at ((i+1)/n)^this down the foliage
constexpr float kTierPoint = 0.52f;   // a tier's point sits this far down the tier above
constexpr float kStepOut = 1.02f;     // tiers above the bottom stick out past the cone: the steps
constexpr float kTopWide = 1.2f;      // the top tier is wider and shorter than the series gives
constexpr float kTopShort = 0.85f;
constexpr float kEdgeLine = 0.30f;    // flank: this much straight line, the rest a flaring curve
constexpr float kEdgeExp = 1.6f;
constexpr float kArch = 0.07f;        // the middle bough tips hang this much (of the tier) higher
constexpr float kHemArc = 0.10f;      // the hem arches up this much (of the tier) between tips
constexpr float kTipHang = 0.03f;     // the outermost tips hang a little lower, heavy at the end
constexpr float kTipThick = 1.8f;     // DIP: the thinnest a bough tip gets

// Snow on boughs and hats.
constexpr float kCellDip = 3.0f;        // spacing of the snow cells
constexpr float kSnowBase = 1.0f;       // deepest snow on a bough, DIP: kSnowBase + kSnowPerH * H(DIP)
constexpr float kSnowPerH = 0.045f;
constexpr float kTreeVolume = 1.1f;     // DIP^2 of snow per landed flake of 1 DIP (the drift's is 1)
constexpr float kVolumeExp = 1.3f;      // same law as the drift
constexpr float kMaxCatchDip = 16.0f;   // bigger flakes pass in front of the props
constexpr float kFlankCatch = 0.6f;     // share of flakes crossing a tree's outline that settle on it
constexpr float kSkirtCatch = 0.45f;    // ... crossing the bough tips along a hem (the rest pass in front)
constexpr float kHatCatch = 0.8f;       // ... coming down on a snowman's hat
constexpr float kMeltTau = 1200.0f;     // s
constexpr float kSublimation = 0.0005f; // DIP/s
constexpr float kDiffuse = 0.8f;        // 1/s: neighbouring cells even out
constexpr float kSkirtDiffuse = 0.16f;  // 1/s: ... much less on the bough tips, so pillows form
constexpr float kSlideTau = 0.5f;       // s: snow above a cell's cap slides on
constexpr float kCapKnee = 0.7f;        // fill level where flakes start to bounce off
constexpr float kDroop = 0.2f;          // tip droop under a full load (of the tier height)
constexpr float kClumpArea = 2.5f;      // DIP^2 of snow per falling clump
constexpr float kSloughArea = 14.0f;    // DIP^2 gathered at a tip before it lets go
constexpr float kSlideRest = 30.0f;     // s at least between a tree's own snow slides (plus up to 60)
constexpr float kShade = 6.0f;          // DIP under the tier above in which snow lies in its shade

// Motion: boughs ring at a few Hz, a whole tree at about one, a snowman rocks
// slowly. Upper tiers are smaller and stiffer; small trees likewise.
constexpr float kSwayHz = 2.0f, kSwayHzTop = 3.0f, kSwayZeta = 0.16f;
constexpr float kBounceHz = 2.4f, kBounceHzTop = 3.2f, kBounceZeta = 0.17f;
constexpr float kLeanHz = 1.0f, kLeanZeta = 0.14f;
constexpr float kLeanMax = 0.035f;     // of H at the tip, in a strong gust
constexpr float kGustWind = 220.0f;    // DIP/s counted as a strong gust
constexpr float kShakeOff = 0.22f;     // bough speed (half widths/s) above which it sheds snow
constexpr float kReach = 9.0f;         // DIP: how far from the pointer props feel it
constexpr float kCouple = 0.27f;       // share of a brushed tier's swing passed to its neighbours
constexpr float kLeanCouple = 0.03f;   // ... and to the whole tree
constexpr float kRockHz = 1.4f, kRockZeta = 0.12f, kRockMax = 0.12f;  // snowman wobble (rad)
constexpr float kHatHz = 2.6f, kHatZeta = 0.14f, kHatMax = 0.35f;     // its hat on its head

// Light. Glows add light (premultiplied colour above alpha); kGlowAlpha is how
// much of the desktop they veil.
constexpr float kGlowAlpha = 0.35f;
constexpr float kMistAlpha = 0.6f;     // the same where they can't add light: soft coloured mist
constexpr float kPowerOn = 1.5f;       // s for a string to come on, bulb by bulb

// Art.
constexpr int kPad = 3;          // per side of every page cell: 1 px antialiasing + 2 px clear border
constexpr int kClearBorder = 2;

struct Rgb {
    float r, g, b;
};
constexpr Rgb Hex(uint32_t c) {
    return {static_cast<float>((c >> 16) & 0xFF) / 255.0f, static_cast<float>((c >> 8) & 0xFF) / 255.0f,
            static_cast<float>(c & 0xFF) / 255.0f};
}
// Needles lean blue to suit a snowy night; lit from the upper left like the drift.
constexpr Rgb kNeedleShadow = Hex(0x0F3A2F);
constexpr Rgb kNeedleDark = Hex(0x1D6447);
constexpr Rgb kNeedleMid = Hex(0x237047);
constexpr Rgb kNeedleLit = Hex(0x3D9457);
constexpr Rgb kRim = Hex(0x9EDFAE);
constexpr Rgb kBarkShadow = Hex(0x47301F);
constexpr Rgb kBarkMid = Hex(0x6F4B33);
constexpr Rgb kBarkLit = Hex(0x9C7250);
constexpr Rgb kStarCore = Hex(0xFFF6C8);
constexpr Rgb kStarBody = Hex(0xFFD447);
constexpr Rgb kStarEdge = Hex(0xE9A21F);
constexpr Rgb kStarGlow = Hex(0xFFC94A);
constexpr Rgb kSparkle = Hex(0xFFE08A);
constexpr Rgb kSnowShade = Hex(0xBDD4F5);  // the drift's shaded snow
constexpr Rgb kSnowUnder = Hex(0xB3C9ED);  // ... and the underside of its lip
constexpr Rgb kShadow = Hex(0x050A17);     // ... and its cast shadow
constexpr Rgb kMound = Hex(0xEEF3FC);      // snow heaped round a prop's foot
constexpr Rgb kShadeBlue = Hex(0x2F5FC8);  // a prop's shade on the snow, in the drift's cool blue
constexpr Rgb kWire = Hex(0x0B2A20);
constexpr Rgb kCoal = Hex(0x1D1F27);
constexpr Rgb kWhite = {1.0f, 1.0f, 1.0f};

// The strings. Multicolour is a classic set (the green a light lime, the only
// green that doesn't vanish into the needles); a few trees get red and gold.
// Halos shift red towards rose and orange towards peach, so the light they
// throw on the needles doesn't brown them.
struct BulbColor {
    Rgb glass, halo;
};
constexpr BulbColor kMulticolour[] = {
    {Hex(0xFF3426), Hex(0xFF4F7A)}, {Hex(0xFFC83A), Hex(0xFFC83A)}, {Hex(0x3D8BFF), Hex(0x3D8BFF)},
    {Hex(0x8CFF5A), Hex(0x8CFF5A)}, {Hex(0xE04DE0), Hex(0xE04DE0)}, {Hex(0xFF7A1A), Hex(0xFFB04D)},
};
constexpr BulbColor kRedGold[] = {{Hex(0xFF3426), Hex(0xFF4F7A)}, {Hex(0xFFC83A), Hex(0xFFC83A)}};

// Snowmen's knitwear: scarf/hat colour and its stripes.
struct Knit {
    Rgb main, stripe;
};
constexpr Knit kKnits[] = {
    {Hex(0xD23A3A), Hex(0xF7F1E6)},  // red and cream
    {Hex(0x2F8A5B), Hex(0xF2E3B3)},  // green and gold
    {Hex(0x3A68C8), Hex(0xF4F7FF)},  // blue and white
    {Hex(0xE07B2A), Hex(0x5A2E1A)},  // orange and brown
};
constexpr int kKnitCount = static_cast<int>(std::size(kKnits));

inline D2D1_COLOR_F Color(Rgb c, float a) { return {c.r, c.g, c.b, a}; }
inline Rgb Mix(Rgb a, Rgb b, float t) { return {Lerp(a.r, b.r, t), Lerp(a.g, b.g, t), Lerp(a.b, b.b, t)}; }

// ---------------------------------------------------------------------------
// Tree tier geometry, tier-local px: the apex is the origin, y points down.
// ---------------------------------------------------------------------------

inline float EdgeY(float h, float w, float ax) {
    const float t = Saturate(ax / w);
    return h * (kEdgeLine * t + (1.0f - kEdgeLine) * (1.0f - std::pow(1.0f - t, kEdgeExp)));
}
inline float EdgeSlope(float h, float w, float ax) {
    const float t = Saturate(ax / w);
    return h / w * (kEdgeLine + (1.0f - kEdgeLine) * kEdgeExp * std::pow(1.0f - t, kEdgeExp - 1.0f));
}
// Half width at depth y below the apex.
inline float EdgeX(float h, float w, float y) {
    float lo = 0.0f, hi = w;
    for (int i = 0; i < 24; ++i) {
        const float mid = 0.5f * (lo + hi);
        (EdgeY(h, w, mid) < y ? lo : hi) = mid;
    }
    return 0.5f * (lo + hi);
}
// The line the bough tips hang to: the middle ones a little higher, the
// outermost a little lower.
inline float HemLow(float h, float w, float arch, float x) {
    const float u = Saturate(std::fabs(x) / w);
    return h * (1.0f - arch * (1.0f - u * u) + kTipHang * u * u * u * u);
}
// The hem: bough tips with shallow arches between them, never rising to within
// `thick` px of the flank above (so every tip keeps some body). `tipness`
// (optional) is 1 at a tip, 0 mid-arch.
inline float HemY(float h, float w, float arch, const std::vector<float>& boughs, float x, float thick,
                  float* tipness = nullptr) {
    const float u = Saturate(std::fabs(x) / w);
    const int m = static_cast<int>(boughs.size());
    const float span = 2.0f * w / static_cast<float>(m);
    const float from = Clamp(w - x, 0.0f, 2.0f * w);  // arches run from the right tip
    const int j = std::min(m - 1, static_cast<int>(from / span));
    float a = std::max(0.0f, std::sin(kPi * (from / span - static_cast<float>(j))));
    if (tipness) *tipness = 1.0f - a;
    a *= 1.0f - 0.75f * SmoothStep(0.55f, 0.95f, u);  // the outermost arches stay shallow
    const float y = HemLow(h, w, arch, x) - kHemArc * h * boughs[static_cast<size_t>(j)] * a;
    const float body = EdgeY(h, w, std::fabs(x)) + Lerp(0.16f * h, thick, SmoothStep(0.6f, 1.0f, u));
    return std::max(y, body);
}

}  // namespace scenery

// ---------------------------------------------------------------------------
// The props
// ---------------------------------------------------------------------------

struct Scenery::Cell {
    float lx = 0.0f;           // part-local x of the centre (px from the axis)
    float ey = 0.0f;           // the shelf the snow lies on, there (px below the part's anchor)
    float nx = 0.0f, ny = -1;  // outward (upward) unit normal of the shelf
    float capF = 1.0f;         // share of the cap this spot holds
    float load = 0.0f;         // snow thickness (px)
    float wobble = 0.0f;       // fixed variation for drawing
    float shade = 0.0f;        // 0..1: in the shadow of what is above
    bool exposed = true;       // not hidden under what is above
    int seg = 0;               // hats: which part of the hat (snow only evens out within one)
};

struct Scenery::Bulb {
    float lx = 0.0f, ly = 0.0f;  // tier-local px
    float size = 0.0f;           // px
    int color = 0;               // into the tree's set
    float phase = 0.0f;          // [0, 1)
    int order = 0;               // along the tree's string, bottom to top
};

// Snow lies on two shelves of a tier: along its flanks (its outline against
// the sky) and on the bough tips just above its hem (its flat rim, seen a
// little from above).
struct Scenery::Tier {
    float h = 0.0f, w = 0.0f;   // apex-to-tips height, half width (px)
    float apexY = 0.0f;         // apex height above the tree's base (px)
    float arch = 0.0f;
    float tip = 0.0f;           // px: thinnest a tip gets (see HemY)
    std::vector<float> boughs;  // how far the hem arches up between each pair of tips (x kHemArc)
    int half = 0;               // snow cells per side
    float cellW = 0.0f;         // px
    std::vector<Cell> cells;    // flanks: left tip .. axis .. right tip
    std::vector<Cell> skirt;    // bough tips: left .. right
    std::vector<Bulb> bulbs;
    float shed = 0.0f;          // px^2 of loosened snow not yet thrown as clumps
    float loose = 0.0f;         // px^2 too much for the bough tips, about to drop
    float overflow[2] = {};     // px^2 slid down to each flank's tip, about to drop
    // Art: the apex is the anchor; strips are page x boundaries, left to right,
    // one of them on the axis.
    Sprite art;
    std::vector<UINT32> strips;
    // Motion.
    float sway = 0.0f, swayV = 0.0f;      // px: sideways offset of the tips from the apex
    float droop[2] = {}, droopV[2] = {};  // px: left / right tip droop
    float swayW = 0.0f, bounceW = 0.0f;   // rad/s
    float gust = 0.0f;                    // noise offset for flutter
};

// A group of trees standing together, as laid out (DIP).
struct Scenery::Group {
    struct Member {
        float dx, h;           // from the group's left edge; height
    };
    std::vector<Member> trees;
    float w = 0.0f;
    int mode = 0, lights = 0;  // one light pattern and string for the group
    float phase = 0.0f;
};

struct Scenery::Tree {
    int group = 0, member = 0; // where it stands: groups_[group].trees[member]
    float x = 0.0f;            // base centre (px, whole)
    float H = 0.0f;            // base to the top tier's apex (px)
    float halfW = 0.0f;        // widest half width, plus the pointer's reach (px)
    float trunkW = 0.0f, trunkTop = 0.0f;  // px; trunkTop: height of the bottom hem above the base
    float starR = 0.0f;
    float lift = 0.0f, liftGoal = 0.0f;    // drift height it stands on (px); where it is gliding to
    float fall = 0.0f;         // px/s: dropping onto snow that went from under it
    bool placed = false;       // stood on the drift yet (until then it goes straight there)
    float snow = 0.0f;         // the drift's surface at the trunk (px above the ground)
    float snowL = 0.0f, snowR = 0.0f;      // ... under the bottom tier's tips
    float lean = 0.0f, leanV = 0.0f;       // px at the tip
    float slideWait = 0.0f;    // s before its snow may slide off on its own again
    float vis = 1.0f;          // 0..1: stepping aside for a window
    bool covered = false;
    bool probed = false;       // covered has been looked up (until then vis follows it at once)
    int mode = 0;              // light pattern
    float phase = 0.0f;
    int lights = 0;            // 0 multicolour, 1 red and gold
    int bulbs = 0;
    Sprite trunk, star;
    std::vector<Tier> tiers;   // bottom to top
};

// A snowman: three balls of snow, coal face and buttons, carrot nose, twig
// arms, a knitted scarf and a hat. Body-local px have the base as origin and y
// pointing down, as on screen (it rocks about the base); hat-local px likewise
// from the hat's foot.
struct Scenery::Snowman {
    float x = 0.0f;            // base centre (px, whole)
    float H = 0.0f;            // base to the top of the hat (px)
    float r[3] = {}, c[3] = {};  // ball radii and centre heights, bottom to top (px)
    int dir = 1;               // which way the face turns (+1: right)
    int hat = 0;               // 0 top hat, 1 bobble hat
    int knit = 0;              // scarf (and bobble hat) colours
    int arms = 0;              // pose: 0 down, 1 one waving, 2 both up
    float hatY = 0.0f;         // hat foot above the base (px)
    float hatTilt0 = 0.0f;     // the jaunty angle it sits at (rad)
    float knotX = 0.0f, knotY = 0.0f;  // where the scarf's tail hangs from (px from the base)
    float halfW = 0.0f;        // half width incl. arms, plus the pointer's reach (px)
    float lift = 0.0f, liftGoal = 0.0f, fall = 0.0f, snow = 0.0f, snowL = 0.0f, snowR = 0.0f;  // as for a tree
    bool placed = false;
    float rock = 0.0f, rockV = 0.0f;   // rad: wobble about the base
    float hatTilt = 0.0f, hatTiltV = 0.0f;
    float rockW = 0.0f;        // rad/s
    float flutter = 0.0f;      // noise offset for the scarf
    float vis = 1.0f;
    bool covered = false;
    bool probed = false;
    std::vector<Cell> hatSnow; // on the crown (and brim) of the hat
    float shed = 0.0f;
    Sprite body, hatArt, tail;
};
