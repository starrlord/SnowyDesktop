#include "SceneryParts.h"

#include "Noise.h"

using namespace scenery;

namespace {

// A sprite colour that adds `c` at strength `a` while veiling only `veil` of
// what is behind (Direct2D premultiplies it to (c * a, veil * a)).
D2D1_COLOR_F Glow(Rgb c, float a, float veil) {
    const float k = 1.0f / veil;
    return {c.r * k, c.g * k, c.b * k, veil * a};
}

// Noise inputs repeat every 256 units; wrapping in double keeps float precision.
float Wrap256(double v) { return static_cast<float>(std::fmod(v, 256.0)); }
float Frac(double v) { return static_cast<float>(v - std::floor(v)); }

// One step of a damped spring (semi-implicit Euler; callers keep w * dt small).
void Spring(float& x, float& v, float rest, float w, float zeta, float dt) {
    v += (-(w * w) * (x - rest) - 2.0f * zeta * w * v) * dt;
    x += v * dt;
}

D2D1_MATRIX_3X2_F Placed(float angle, float x, float y) {
    if (angle == 0.0f) return {1, 0, 0, 1, x, y};
    const float c = std::cos(angle), s = std::sin(angle);
    return {c, s, -s, c, x, y};
}

// A page cell anchored at its sprite's anchor, scaled by (kx, ky), turned and placed.
ScenerySprite Quad(const Scenery::Sprite& sp, float kx, float ky, float angle, float x, float y, D2D1_COLOR_F col) {
    return {{(static_cast<float>(sp.src.left) - sp.cx) * kx, (static_cast<float>(sp.src.top) - sp.cy) * ky,
             (static_cast<float>(sp.src.right) - sp.cx) * kx, (static_cast<float>(sp.src.bottom) - sp.cy) * ky},
            sp.src,
            col,
            Placed(angle, x, y)};
}

// Smallest level at least `d` px across (levels halve from the first).
int PickLevel(const float* px, int count, float d) {
    int l = 0;
    while (l + 1 < count && px[l + 1] >= d) ++l;
    return l;
}

int CellIndex(int half, float cellW, float lx) {
    const int i = std::min(half - 1, static_cast<int>(std::fabs(lx) / cellW));
    return lx < 0.0f ? half - 1 - i : half + i;
}
int SkirtIndex(int half, float cellW, float w, float lx) {
    return Clamp(static_cast<int>((lx + w) / cellW), 0, 2 * half - 1);
}

const BulbColor& Bulbs(int lights, int color) {
    return lights == 1 ? kRedGold[color % 2] : kMulticolour[color % static_cast<int>(std::size(kMulticolour))];
}

float Ease(float t) { return t * t * (3.0f - 2.0f * t); }

constexpr float kMinGap = 14.0f;   // DIP at least between tree groups, and between them and snowmen
constexpr float kManShare = 0.46f; // a snowman's half width (arms) / H
constexpr float kDrop = 1500.0f;   // DIP/s^2: a prop dropping onto snow that went from under it

}  // namespace

Scenery::Scenery() {
    LARGE_INTEGER ticks;
    QueryPerformanceCounter(&ticks);
    rng_.Seed(static_cast<uint64_t>(ticks.QuadPart) ^ reinterpret_cast<uintptr_t>(this), 0x7EE5ull);
}
Scenery::~Scenery() = default;

bool Scenery::Active() const {
    return (!trees_.empty() && fadeTrees_ > 0.0f) || (!snowmen_.empty() && fadeSnowmen_ > 0.0f);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void Scenery::Configure(const SceneryLayout& layout) {
    if (configured_ && layout == layout_) return;
    layout_ = layout;
    configured_ = true;
    Relayout(true);
}

void Scenery::SetTrees(bool show, int amount) {
    showTrees_ = show;
    amount = Clamp(amount, 0, 100);
    if (amount == treeAmount_) return;
    treeAmount_ = amount;
    AmountsChanged();
}

void Scenery::SetSnowmen(bool show, int amount) {
    showSnowmen_ = show;
    amount = Clamp(amount, 0, 100);
    if (amount == snowmanAmount_) return;
    snowmanAmount_ = amount;
    AmountsChanged();
}

// New amounts lay the props out again once they have rested, and only if they
// change the counts (back to what is standing: nothing to do).
void Scenery::AmountsChanged() {
    if (!configured_) return;
    if (CountsFor(treeAmount_, snowmanAmount_) == built_) {
        relayoutAt_ = -1.0;
        return;
    }
    relayoutAt_ = time_ + settle_;
    if (settle_ <= 0.0f) Relayout(false);
}

void Scenery::RecheckCover() {
    for (Tree& t : trees_) t.probed = false;
    for (Snowman& m : snowmen_) m.probed = false;
    coverStale_ = true;
}

void Scenery::SetAdditiveGlow(bool on) { glowAlpha_ = on ? kGlowAlpha : 1.0f; }

void Scenery::SetSnowDepth(float scale) {
    if (!(scale > 0.0f)) scale = 0.0f;
    if (scale == 0.0f && snowDepth_ > 0.0f) ClearSnow(true);  // accumulation switched off: melt
    snowDepth_ = scale;
}

float Scenery::CapPx(const Tree& tree) const {
    const float s = layout_.dpiScale;
    return (kSnowBase + kSnowPerH * tree.H / s) * s * snowDepth_;
}

float Scenery::CapPx(const Snowman& man) const {
    const float s = layout_.dpiScale;
    return (0.8f * kSnowBase + 0.035f * man.H / s) * s * snowDepth_;
}

// How many tree groups and snowmen the amounts give on this screen: at the
// usual amount about one group per kSpotSpacing of width and one snowman per
// kSnowmanSpacing, doubling every kAmountDouble steps. None where the screen is
// too small for props.
Scenery::Counts Scenery::CountsFor(int treeAmount, int snowmanAmount) const {
    const float s = layout_.dpiScale;
    if (layout_.width <= 0 || !(s > 0.0f)) return {};
    const float wDip = static_cast<float>(layout_.width) / s;
    const float hMax = std::min(kMaxTree, kSkyShare * layout_.groundY / s);
    if (wDip < 240.0f || hMax < kMinTree) return {};
    const float treeMul = std::exp2(static_cast<float>(treeAmount - kUsualAmount) / kAmountDouble);
    const float manMul = std::exp2(static_cast<float>(snowmanAmount - kUsualAmount) / kAmountDouble);
    return {Clamp(static_cast<int>(std::lround(wDip / kSpotSpacing * treeMul)), 1, kMaxGroups),
            Clamp(static_cast<int>(std::lround(wDip / kSnowmanSpacing * manMul)), 1, kMaxSnowmen)};
}

// The same monitor always gets the same scene.
uint32_t Scenery::Seed() const {
    return Hash32(static_cast<uint32_t>(layout_.width)) ^ Hash32(static_cast<uint32_t>(layout_.height) * 7919u + 1u) ^
           Hash32(static_cast<uint32_t>(std::lround(layout_.dpiScale * 100.0f)) + 77u) ^
           Hash32(static_cast<uint32_t>(std::lround(layout_.originX)) + 0x51ED27u);
}

// Lays the props out for the current amounts: everything (`all`), or what the
// counts changed. A new number of snowmen keeps the trees as they are, snow,
// lights and all (they only move if the snowmen need the room).
void Scenery::Relayout(bool all) {
    relayoutAt_ = -1.0;
    const Counts want = CountsFor(treeAmount_, snowmanAmount_);
    const bool trees = all || want.groups != built_.groups;
    if (!trees && want.men == built_.men) return;
    if (trees) LayoutTrees(want.groups);
    LayoutSnowmen(want.men);
    built_ = want;
    ++layoutGen_;
    coverStale_ = true;
}

// Room kept clear at the screen's ends (DIP).
float Scenery::Margin() const {
    return std::max(70.0f, 0.05f * static_cast<float>(layout_.width) / layout_.dpiScale);
}

// Groups of one to three trees, as many as the screen has room for. Each tree
// draws its own numbers, so with the same count a monitor always gets the same
// trees and nothing else on the screen changes how they look. Where they stand
// is up to LayoutSnowmen.
void Scenery::LayoutTrees(int groups) {
    trees_.clear();
    groups_.clear();
    ++genTrees_;
    powerOn_ = 0.0f;
    sparkleTree_ = -1;
    top_ = layout_.groundY;
    if (groups <= 0) return;
    const float s = layout_.dpiScale;
    const float wDip = static_cast<float>(layout_.width) / s;
    const float hMax = std::min(kMaxTree, kSkyShare * layout_.groundY / s);
    const uint32_t seed = Seed();
    Rng rng(seed, 0x7EE5ull);
    const float halfShare = 0.5f * kWidth * (1.0f - kTrunkShow);  // a tree's half width / H
    int treeCount = 0;
    for (int g = 0; g < groups; ++g) {
        Group it;
        const float pick = rng.Float();
        const bool narrow = wDip < 900.0f;
        int n = pick < 0.42f ? 1 : (pick < 0.86f || narrow ? 2 : 3);
        n = std::min(n, kMaxTrees - treeCount);
        if (n <= 0) break;
        const float lead = hMax * rng.Range(0.82f, 1.0f);
        // Companions differ from each other, and a trio leans one way as often as not.
        float hs[3] = {lead, lead * rng.Range(0.62f, 0.8f), 0.0f};
        hs[2] = hs[1] - lead * rng.Range(0.12f, 0.2f);
        const bool oneSide = rng.Chance(0.5f);
        float side = rng.Chance(0.5f) ? 1.0f : -1.0f;
        std::vector<float> xs = {0.0f};
        float reachL = 0.0f, reachR = 0.0f;  // centres of the outermost trees, from the lead
        for (int k = 1; k < n; ++k) {
            const float prev = k == 1 || !oneSide ? lead : hs[k - 1];
            const float gap = halfShare * (prev + 0.85f * hs[k]) * rng.Range(0.82f, 1.02f);
            float x = side > 0.0f ? reachR + gap : reachL - gap;
            if (k == 1 || !oneSide) x = side * gap;
            xs.push_back(x);
            reachL = std::min(reachL, x);
            reachR = std::max(reachR, x);
            if (!oneSide) side = -side;
        }
        float left = 1.0e9f, right = -1.0e9f;
        for (int k = 0; k < n; ++k) {
            const float half = 1.05f * halfShare * hs[k];
            left = std::min(left, xs[static_cast<size_t>(k)] - half);
            right = std::max(right, xs[static_cast<size_t>(k)] + half);
        }
        for (int k = 0; k < n; ++k) it.trees.push_back({xs[static_cast<size_t>(k)] - left, hs[k]});
        it.w = right - left;
        // One light pattern per group, each tree a little behind the last, so a
        // group reads as one display. Cross-fades and waves keep a tree's glow
        // even; breathing and twinkling are rarer.
        const float m = rng.Float();
        it.mode = m < 0.5f ? 0 : (m < 0.8f ? 2 : (m < 0.9f ? 3 : 1));
        it.phase = rng.Float();
        it.lights = rng.Chance(0.2f) ? 1 : 0;
        treeCount += n;
        groups_.push_back(std::move(it));
    }
    const float usable = wDip - 2.0f * Margin();
    float need = -kMinGap;
    size_t fit = 0;
    for (; fit < groups_.size(); ++fit) {
        need += groups_[fit].w + kMinGap;
        if (need > usable && fit > 0) break;
    }
    groups_.resize(fit);

    // Big trees first: smaller ones stand in front.
    struct Who {
        int group, member;
        float h;
    };
    std::vector<Who> order;
    for (size_t g = 0; g < groups_.size(); ++g)
        for (size_t k = 0; k < groups_[g].trees.size(); ++k)
            order.push_back({static_cast<int>(g), static_cast<int>(k), groups_[g].trees[k].h});
    std::stable_sort(order.begin(), order.end(), [](const Who& a, const Who& b) { return a.h > b.h; });
    for (const Who& w : order) {
        const Group& gr = groups_[static_cast<size_t>(w.group)];
        Rng own(seed ^ Hash32(static_cast<uint32_t>(w.group * 8 + w.member) + 0x3C6EF372u), 0x7EE5ull);
        Tree& t = trees_.emplace_back();
        BuildTree(t, 0.0f, w.h * s, gr.mode, gr.phase + 0.15f * static_cast<float>(w.member), gr.lights, own);
        t.group = w.group;
        t.member = w.member;
    }
}

// Where everything stands, and the snowmen. The tree groups spread out over
// the screen, sharing the room left over unevenly; the snowmen (now and then a
// big one with a little one) go in the open stretches between them, the middle
// ones first, then the screen's ends (where the drift thins out), and turn to
// face the nearest trees. So the trees stay put whatever the snowmen, unless
// the stretches can't hold them all: then the groups close up to make room,
// keeping their looks, snow and lights.
void Scenery::LayoutSnowmen(int men) {
    snowmen_.clear();
    spans_.clear();
    ++genMen_;
    const float s = layout_.dpiScale;
    if (layout_.width <= 0 || !(s > 0.0f)) return;
    const float wDip = static_cast<float>(layout_.width) / s;
    const float hMax = std::min(kMaxTree, kSkyShare * layout_.groundY / s);
    const uint32_t seed = Seed();
    Rng rng(seed ^ 0x5A0Bu, 0x5A0Bull);
    Rng spread(seed ^ 0x6A9Du, 0x6A9Dull);  // the spacing

    struct Plan {
        float dx, h;  // DIP from the item's left edge; height
    };
    struct Item {
        std::vector<Plan> plans;
        float w = 0.0f;  // DIP
    };
    std::vector<Item> items;
    const float manMax = std::min(kMaxSnowman, 0.8f * hMax);
    for (int k = 0; k < men;) {
        Item it;
        const float h = manMax * rng.Range(0.85f, 1.0f);
        if (men - k >= 2 && rng.Chance(0.3f)) {
            const float child = h * rng.Range(0.6f, 0.72f);
            const float gap = kManShare * (h + child) * 0.75f;
            it.plans = {{kManShare * h, h}, {kManShare * h + gap, child}};
            it.w = kManShare * h + gap + kManShare * child;
            k += 2;
        } else {
            it.plans = {{kManShare * h, h}};
            it.w = 2.0f * kManShare * h;
            ++k;
        }
        items.push_back(std::move(it));
    }

    // A row of things `widths` wide along the ground, sharing the room left
    // over out between them unevenly (half as much at the ends): left edges.
    const float margin = Margin(), usable = wDip - 2.0f * margin;
    auto row = [&](const std::vector<float>& widths) {
        float need = -kMinGap;
        for (float w : widths) need += w + kMinGap;
        const float room = std::max(0.0f, usable - need);
        std::vector<float> weights(widths.size() + 1);
        float total = 0.0f;
        for (size_t i = 0; i < weights.size(); ++i)
            total += (weights[i] = spread.Range(0.6f, 1.4f) * (i == 0 || i == widths.size() ? 0.5f : 1.0f));
        std::vector<float> lefts(widths.size());
        float x = margin;
        for (size_t i = 0; i < widths.size(); ++i) {
            x += room * weights[i] / total;
            lefts[i] = x;
            x += widths[i] + kMinGap;
        }
        return lefts;
    };
    const int G = static_cast<int>(groups_.size());
    std::vector<float> widths;
    for (const Group& gr : groups_) widths.push_back(gr.w);
    std::vector<float> groupLeft = row(widths);

    // The open stretches.
    struct Gap {
        float a, b;  // DIP
        std::vector<size_t> items;
        float used = 0.0f;  // DIP taken by its items
    };
    std::vector<Gap> gaps;
    for (int i = 0; i + 1 < G; ++i)
        gaps.push_back({groupLeft[static_cast<size_t>(i)] + groups_[static_cast<size_t>(i)].w, groupLeft[static_cast<size_t>(i) + 1]});
    std::stable_sort(gaps.begin(), gaps.end(), [wDip](const Gap& p, const Gap& q) {
        return std::fabs(p.a + p.b - wDip) < std::fabs(q.a + q.b - wDip);
    });
    const bool leftFirst = rng.Chance(0.5f);
    if (G == 0) {
        gaps.push_back({margin, wDip - margin});
    } else {
        const Gap ends[2] = {{margin, groupLeft.front()}, {groupLeft.back() + groups_.back().w, wDip - margin}};
        gaps.push_back(ends[leftFirst ? 0 : 1]);
        gaps.push_back(ends[leftFirst ? 1 : 0]);
    }
    // Room left in a gap for one more item, clear of its neighbours.
    auto room = [](const Gap& g) {
        return g.b - g.a - g.used - kMinGap * static_cast<float>(g.items.size() + 2);
    };
    // Each in turn to the next gap with room for it, so they spread out.
    size_t next = 0, fitted = 0;
    for (size_t k = 0; k < items.size(); ++k)
        for (size_t tries = 0; tries < gaps.size(); ++tries) {
            Gap& g = gaps[(next + tries) % gaps.size()];
            if (room(g) < items[k].w) continue;
            g.items.push_back(k);
            g.used += items[k].w;
            next = (next + tries + 1) % gaps.size();
            ++fitted;
            break;
        }

    std::vector<std::pair<float, size_t>> at;  // left edge (DIP) of each snowman item placed
    if (fitted == items.size()) {
        // In each gap, share the room left over out between its items.
        for (const Gap& g : gaps) {
            if (g.items.empty()) continue;
            const float spare = std::max(0.0f, room(g) + kMinGap);
            std::vector<float> weights(g.items.size() + 1);
            float total = 0.0f;
            for (float& w : weights) total += (w = rng.Range(0.6f, 1.4f));
            float x = g.a + kMinGap;
            for (size_t i = 0; i < g.items.size(); ++i) {
                x += spare * weights[i] / total;
                at.push_back({x, g.items[i]});
                x += items[g.items[i]].w + kMinGap;
            }
        }
    } else {
        // The groups close up: snowmen between them (from the middle out),
        // then at the ends, as many as fit. In the line, group i is i and
        // snowman item k is -1 - k.
        std::vector<int> prefs;  // before group i (0 and G: the ends)
        for (int i = 1; i < G; ++i) prefs.push_back(i);
        std::stable_sort(prefs.begin(), prefs.end(), [G](int a, int b) { return std::abs(2 * a - G) < std::abs(2 * b - G); });
        prefs.push_back(leftFirst ? 0 : G);
        prefs.push_back(leftFirst ? G : 0);
        std::vector<std::vector<int>> into(static_cast<size_t>(G) + 1);
        for (size_t k = 0; k < items.size(); ++k)
            into[static_cast<size_t>(prefs[k % prefs.size()])].push_back(-1 - static_cast<int>(k));
        std::vector<int> line;
        for (int i = 0; i <= G; ++i) {
            line.insert(line.end(), into[static_cast<size_t>(i)].begin(), into[static_cast<size_t>(i)].end());
            if (i < G) line.push_back(i);
        }
        auto width = [&](int e) {
            return e >= 0 ? groups_[static_cast<size_t>(e)].w : items[static_cast<size_t>(-1 - e)].w;
        };
        auto need = [&] {
            float w = -kMinGap;
            for (int e : line) w += width(e) + kMinGap;
            return w;
        };
        while (need() > usable) {
            auto it = std::find_if(line.rbegin(), line.rend(), [](int e) { return e < 0; });
            if (it == line.rend()) break;
            line.erase(std::next(it).base());
        }
        std::vector<float> lw;
        for (int e : line) lw.push_back(width(e));
        const std::vector<float> lefts = row(lw);
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] >= 0) groupLeft[static_cast<size_t>(line[i])] = lefts[i];
            else at.push_back({lefts[i], static_cast<size_t>(-1 - line[i])});
        }
    }

    // The trees to their places (any that move go straight onto the drift there).
    for (int i = 0; i < G; ++i)
        spans_.push_back({groupLeft[static_cast<size_t>(i)], groupLeft[static_cast<size_t>(i)] + groups_[static_cast<size_t>(i)].w});
    for (Tree& t : trees_) {
        const float x = std::round((groupLeft[static_cast<size_t>(t.group)] +
                                    groups_[static_cast<size_t>(t.group)].trees[static_cast<size_t>(t.member)].dx) * s);
        if (x == t.x) continue;
        t.x = x;
        t.placed = t.probed = false;
    }
    // The snowmen, each facing the nearest trees; each draws its own numbers.
    for (const auto& [left, k] : at) {
        const Item& it = items[k];
        for (size_t j = 0; j < it.plans.size(); ++j) {
            const float cx = left + it.plans[j].dx;
            float target = 0.5f * wDip, best = 1.0e9f;
            for (const Span& sp : spans_) {
                const float c = 0.5f * (sp.left + sp.right);
                if (std::fabs(c - cx) < best) {
                    best = std::fabs(c - cx);
                    target = c;
                }
            }
            Rng own(seed ^ Hash32(static_cast<uint32_t>(k * 2 + j) + 0x5A0B5A0Bu), 0x5A0Bull);
            BuildSnowman(snowmen_.emplace_back(), std::round(cx * s), it.plans[j].h * s, target >= cx ? 1 : -1, own);
        }
    }
}

void Scenery::BuildTree(Tree& t, float x, float H, int mode, float phase, int lights, Rng& rng) const {
    const float s = layout_.dpiScale;
    const float hDip = H / s;
    t.x = x;
    t.H = H;
    t.mode = mode;
    t.phase = Frac(phase);
    t.lights = lights;
    const int n = hDip < 66.0f ? 3 : 4;
    const float foliage = (1.0f - kTrunkShow) * H;
    const float halfW = 0.5f * (kWidth + rng.Range(-kWidthVary, kWidthVary)) * foliage;  // slim fir .. plump spruce
    const float stiff = std::pow(80.0f / std::max(hDip, 20.0f), 0.35f);

    // Tiers from the top: bottoms spread down the foliage, each point hidden part
    // way down the tier above; every tier but the bottom one sticks out a little.
    t.tiers.resize(static_cast<size_t>(n));
    float apex = H;
    for (int i = 0; i < n; ++i) {
        Tier& tier = t.tiers[static_cast<size_t>(n - 1 - i)];
        const float frac = std::pow(static_cast<float>(i + 1) / static_cast<float>(n), kTierSpread);
        float bottom = H - foliage * frac;
        if (i == 0) bottom = H - kTopShort * (H - bottom);
        tier.apexY = apex;
        tier.h = apex - bottom;
        tier.w = halfW * std::pow(frac, 0.97f) * (i + 1 < n ? kStepOut : 1.0f) * (i == 0 ? kTopWide : 1.0f);
        tier.tip = kTipThick * s;
        apex -= kTierPoint * tier.h;
        tier.arch = kArch * rng.Range(0.8f, 1.2f);
        const int arcs = tier.h < 14.0f * s ? 2 : (tier.w < 22.0f * s ? 3 : 4);
        for (int j = 0; j < arcs; ++j) tier.boughs.push_back(rng.Range(0.75f, 1.25f));
        const float u = 1.0f - static_cast<float>(i) / static_cast<float>(n - 1);  // 0 bottom .. 1 top
        tier.swayW = kTau * Lerp(kSwayHz, kSwayHzTop, u) * stiff * rng.Range(0.92f, 1.08f);
        tier.bounceW = kTau * Lerp(kBounceHz, kBounceHzTop, u) * stiff * rng.Range(0.92f, 1.08f);
        tier.gust = rng.Range(0.0f, 100.0f);
    }
    t.halfW = 0.0f;
    for (const Tier& tier : t.tiers) t.halfW = std::max(t.halfW, tier.w);
    t.halfW += kReach * s;
    const Tier& bottom = t.tiers.front();
    t.trunkW = std::max(0.13f * 2.0f * halfW, 4.0f * s);
    t.trunkTop = bottom.apexY - bottom.h * (1.0f - bottom.arch) + 2.0f * s;
    t.starR = std::max(0.065f * H, 3.5f * s);
    t.slideWait = rng.Range(0.0f, kSlideRest);

    const int colors = lights == 1 ? 2 : static_cast<int>(std::size(kMulticolour));
    const int colorBase = rng.Int(0, colors - 1);
    const float spacing = std::max(6.5f, 0.085f * hDip) * s;
    const float size = 2.0f * std::max(1.45f, 0.021f * hDip) * s;
    int order = 0;
    for (int k = 0; k < n; ++k) {
        Tier& tier = t.tiers[static_cast<size_t>(k)];
        const Tier* above = k + 1 < n ? &t.tiers[static_cast<size_t>(k) + 1] : nullptr;
        const float drop = above ? above->apexY - tier.apexY : 0.0f;  // the upper apex is this much higher
        // The tier above's hem in this tier's frame (its lowest reach at ax).
        auto hemAbove = [&](float ax) {
            if (!above || ax >= above->w) return -1.0e9f;
            return HemLow(above->h, above->w, above->arch, ax) - drop;
        };
        tier.half = std::max(2, static_cast<int>(std::ceil(tier.w / (kCellDip * s))));
        tier.cellW = tier.w / static_cast<float>(tier.half);

        // Snow cells along each flank: the deepest snow on the flatter rim, none
        // where the flank is steeper than about 60 degrees or out on the tip
        // (the bough tips have their own shelf).
        tier.cells.resize(static_cast<size_t>(2 * tier.half));
        for (int i = 0; i < tier.half; ++i) {
            const float ax = (static_cast<float>(i) + 0.5f) * tier.cellW;
            const float tt = ax / tier.w;
            const float ey = EdgeY(tier.h, tier.w, ax);
            const float slope = EdgeSlope(tier.h, tier.w, ax);
            const float inv = 1.0f / std::sqrt(1.0f + slope * slope);
            const float deg = std::atan(slope) * (180.0f / kPi);
            const float flat = std::pow(Saturate((62.0f - deg) / 30.0f), 1.2f);
            const float tip = 1.0f - Saturate((tt - 0.7f) / 0.14f);
            const float under = ey - hemAbove(ax);  // px below the hem of the tier above
            for (int side = 0; side < 2; ++side) {
                Cell& c = tier.cells[static_cast<size_t>(side ? tier.half + i : tier.half - 1 - i)];
                c.lx = side ? ax : -ax;
                c.ey = ey;
                c.nx = (side ? slope : -slope) * inv;
                c.ny = -inv;
                c.capF = std::max(0.05f, flat * tip) * rng.Range(0.85f, 1.15f);
                c.wobble = rng.Float();
                c.exposed = under > 0.4f * tier.cellW;
                c.shade = above && ax < above->w ? 1.0f - Saturate(under / (kShade * s)) : 0.0f;
            }
        }

        // Snow cells on the bough tips, seated on the hem: pillows on the tips,
        // thin bridges across the arches between them.
        tier.skirt.resize(static_cast<size_t>(2 * tier.half));
        auto shelf = [&](float lx) { return HemY(tier.h, tier.w, tier.arch, tier.boughs, lx, tier.tip) - 0.1f * s; };
        for (int i = 0; i < 2 * tier.half; ++i) {
            Cell& c = tier.skirt[static_cast<size_t>(i)];
            c.lx = -tier.w + (static_cast<float>(i) + 0.5f) * tier.cellW;
            c.ey = shelf(c.lx);
            const float e = 0.3f * tier.cellW;
            const float slope = (shelf(c.lx + e) - shelf(c.lx - e)) / (2.0f * e);
            const float inv = 1.0f / std::sqrt(1.0f + slope * slope);
            c.nx = slope * inv;
            c.ny = -inv;
            float tipness = 0.0f;
            HemY(tier.h, tier.w, tier.arch, tier.boughs, c.lx, tier.tip, &tipness);
            c.capF = (0.15f + 0.85f * tipness * tipness) * rng.Range(0.85f, 1.15f);
            c.wobble = rng.Float();
        }

        // One loop of the string per tier: in on one side, out lower on the other
        // as it wraps round the tree, sagging between; every bulb on the face.
        const float reach = 0.66f * tier.w;
        const int count = std::max(3, static_cast<int>(std::lround(2.0f * reach / spacing)) + 1);
        const bool rtl = k % 2 != 0;  // the string zigzags up: odd tiers run right to left
        const float yIn = 0.55f * tier.h, yOut = 0.86f * tier.h;
        const float sag = 0.14f * tier.h;
        for (int i = 0; i < count; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(count - 1);  // along the string
            Bulb b;
            b.lx = (rtl ? 1.0f - 2.0f * u : 2.0f * u - 1.0f) * reach;
            // Clear of the flank above it, of the tier above and of the snow on the
            // bough tips below it; where the face is too thin there, step inwards.
            float lo = 0.0f, hi = 0.0f;
            for (int tries = 0; tries < 12; ++tries) {
                const float ax = std::fabs(b.lx);
                lo = std::max(EdgeY(tier.h, tier.w, ax) + 0.75f * size, hemAbove(ax) + 0.6f * size);
                hi = shelf(b.lx) - 0.6f * size - 0.6f * kSnowBase * s;
                if (lo <= hi) break;
                b.lx *= 0.93f;
            }
            b.ly = Clamp(Lerp(yIn, yOut, u) + sag * 4.0f * u * (1.0f - u), lo, std::max(lo, hi));
            b.size = size * rng.Range(0.92f, 1.08f);
            b.color = (colorBase + order) % colors;
            b.phase = rng.Float();
            b.order = order++;
            tier.bulbs.push_back(b);
        }
    }
    t.bulbs = order;
}

void Scenery::BuildSnowman(Snowman& m, float x, float H, int dir, Rng& rng) const {
    const float s = layout_.dpiScale;
    m.x = x;
    m.H = H;
    m.dir = dir;
    m.hat = rng.Chance(0.55f) ? 0 : 1;
    m.knit = rng.Int(0, kKnitCount - 1);
    const float pose = rng.Float();
    m.arms = pose < 0.5f ? 0 : (pose < 0.82f ? 1 : 2);
    m.flutter = rng.Range(0.0f, 100.0f);
    // Three balls, each sunk a little into the one below; the hat adds about 0.55 r1.
    const float r1 = H / (m.hat == 0 ? 4.3f : 4.15f);
    m.r[0] = r1 * rng.Range(0.97f, 1.03f);
    m.r[1] = r1 * rng.Range(0.72f, 0.77f);
    m.r[2] = r1 * rng.Range(0.5f, 0.55f);
    m.c[0] = 0.9f * m.r[0];
    m.c[1] = m.c[0] + 0.8f * m.r[0] + 0.65f * m.r[1];
    m.c[2] = m.c[1] + 0.78f * m.r[1] + 0.7f * m.r[2];
    m.hatY = m.c[2] + 0.72f * m.r[2];
    m.hatTilt0 = static_cast<float>(-dir) * rng.Range(0.05f, 0.16f);
    // The scarf ties at the neck, on the side the face turns to.
    m.knotX = static_cast<float>(dir) * 0.42f * m.r[2];
    m.knotY = m.c[1] + 0.7f * m.r[1];
    m.halfW = 2.2f * m.r[1] + kReach * s;
    m.rockW = kTau * kRockHz * std::pow(70.0f / std::max(H / s, 20.0f), 0.3f);

    // Snow on the hat: the crown's top (and a top hat's brim, either side).
    const float r3 = m.r[2];
    int seg = 0;
    auto add = [&](float x0, float x1, float y, float capF) {
        const int n = std::max(2, static_cast<int>(std::ceil((x1 - x0) / (kCellDip * s))));
        for (int i = 0; i < n; ++i) {
            Cell c;
            c.lx = Lerp(x0, x1, (static_cast<float>(i) + 0.5f) / static_cast<float>(n));
            c.ey = y;
            c.capF = capF * rng.Range(0.85f, 1.15f) * (1.0f - 0.4f * std::pow(std::fabs(2.0f * (static_cast<float>(i) + 0.5f) / static_cast<float>(n) - 1.0f), 3.0f));
            c.wobble = rng.Float();
            c.seg = seg;
            m.hatSnow.push_back(c);
        }
        ++seg;
    };
    if (m.hat == 0) {
        add(-0.58f * r3, 0.58f * r3, -1.08f * r3, 1.0f);  // crown top
        add(-0.92f * r3, -0.62f * r3, -0.12f * r3, 0.45f);  // brim, either side
        add(0.62f * r3, 0.92f * r3, -0.12f * r3, 0.45f);
    } else {
        add(-0.72f * r3, 0.72f * r3, -0.8f * r3, 0.7f);  // the dome, round the bobble
    }
    // Hat cells' shelf heights follow its shape (filled in where drawn: see HatShelf).
    for (Cell& c : m.hatSnow) {
        if (m.hat == 1) {
            const float u = c.lx / (0.98f * r3);
            c.ey = -0.95f * r3 * std::sqrt(std::max(0.0f, 1.0f - u * u)) + 0.02f * r3;
            const float slope = 0.95f * r3 * u / std::max(1e-3f, std::sqrt(std::max(1e-4f, 1.0f - u * u))) / (0.98f * r3);
            const float inv = 1.0f / std::sqrt(1.0f + slope * slope);
            c.nx = slope * inv;
            c.ny = -inv;
        }
    }
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------

float Scenery::Lean(const Tree& tree, float heightPx) const {
    const float u = Saturate(heightPx / tree.H);
    return tree.lean * u * u;
}

D2D1_POINT_2F Scenery::Apex(const Tree& tree, const Tier& tier) const {
    return {tree.x + Lean(tree, tier.apexY), layout_.groundY - tree.lift - tier.apexY};
}

// Horizontal shear of a tier (px sideways per px down): its own sway plus the
// tree's lean between its apex and its tips.
float Scenery::Shear(const Tree& tree, const Tier& tier) const {
    return (tier.sway + Lean(tree, tier.apexY - tier.h) - Lean(tree, tier.apexY)) / tier.h;
}

// Tier-local point to scene px: columns shift down with the droop of their
// side, rows shift sideways with the shear.
D2D1_POINT_2F Scenery::Map(const Tree& tree, const Tier& tier, float lx, float ly) const {
    const D2D1_POINT_2F apex = Apex(tree, tier);
    const float u = lx / tier.w;
    return {apex.x + lx + Shear(tree, tier) * ly, apex.y + ly + tier.droop[lx >= 0.0f ? 1 : 0] * u * u};
}

// Body-local point (from the base, y down) to scene px, rocking about the base.
D2D1_POINT_2F Scenery::BodyPoint(const Snowman& man, float bx, float by) const {
    const float c = std::cos(man.rock), sn = std::sin(man.rock);
    return {man.x + bx * c - by * sn, layout_.groundY - man.lift + bx * sn + by * c};
}

// Hat-local point (from the hat's foot, y down) to scene px.
D2D1_POINT_2F Scenery::HatPoint(const Snowman& man, float hx, float hy) const {
    const D2D1_POINT_2F foot = BodyPoint(man, 0.0f, -man.hatY);
    const float a = man.rock + man.hatTilt0 + man.hatTilt;
    const float c = std::cos(a), sn = std::sin(a);
    return {foot.x + hx * c - hy * sn, foot.y + hx * sn + hy * c};
}

// How full one side of a tier is (both shelves), 0..1: its weight bends the boughs.
float Scenery::LoadFraction(const Tree& tree, const Tier& tier, int side) const {
    const float capPx = CapPx(tree);
    if (capPx <= 0.0f) return 0.0f;
    float load = 0.0f, cap = 0.0f;
    for (const std::vector<Cell>* shelf : {&tier.cells, &tier.skirt})
        for (const Cell& c : *shelf) {
            if (!c.exposed || (c.lx >= 0.0f) != (side == 1)) continue;
            load += c.load;
            cap += c.capF;
        }
    return cap > 0.0f ? Saturate(load / (cap * capPx)) : 0.0f;
}

// Snow landing at lx on a shelf: mostly on that cell, some on its neighbours.
// Near the cap less of it sticks; on a flank the rest slides on outwards, off
// the bough tips it drops.
void Scenery::Deposit(Tier& tier, float capPx, bool skirt, float lx, float area) {
    if (capPx <= 0.0f || !(area > 0.0f)) return;
    std::vector<Cell>& cells = skirt ? tier.skirt : tier.cells;
    const int c = skirt ? SkirtIndex(tier.half, tier.cellW, tier.w, lx) : CellIndex(tier.half, tier.cellW, lx);
    const int side = c >= tier.half ? 1 : 0;
    const int lo = skirt || !side ? 0 : tier.half;
    const int hi = skirt || side ? 2 * tier.half - 1 : tier.half - 1;
    const int outward = side ? 1 : -1;
    static constexpr float kShare[3] = {0.25f, 0.5f, 0.25f};
    for (int d = -1; d <= 1; ++d) {
        const int i = c + d;
        if (i < lo || i > hi) continue;
        Cell& cell = cells[static_cast<size_t>(i)];
        if (!cell.exposed) continue;
        const float add = kShare[d + 1] * area / tier.cellW;
        const float cap = std::max(cell.capF * capPx, 1e-3f);
        const float keep = 1.0f - SmoothStep(kCapKnee, 1.0f, cell.load / cap);
        // However big the flake, a spot never fills past its cap at once.
        const float stick = std::min(add * keep, std::max(0.0f, cap - cell.load));
        cell.load += stick;
        const float spill = add - stick;
        if (spill <= 0.0f) continue;
        const int next = i + outward;
        if (skirt) {
            tier.loose += spill * tier.cellW;
        } else if (next >= lo && next <= hi) {
            // Half of it slides on to the next spot out (as much as fits there), past it off the tip.
            Cell& on = cells[static_cast<size_t>(next)];
            const float take = std::min(0.5f * spill, std::max(0.0f, on.capF * capPx - on.load));
            on.load += take;
            tier.overflow[side] += (0.5f * spill - take) * tier.cellW;
        } else {
            tier.overflow[side] += spill * tier.cellW;
        }
    }
}

// Loosened snow (px^2) from a tree's cell, thrown as clumps of powder.
void Scenery::Shed(const Tree& tree, Tier& tier, const Cell& c, float area, float vx, float vy,
                   std::vector<SnowClump>& clumps) {
    const float s = layout_.dpiScale;
    tier.shed += area;
    const float per = kClumpArea * s * s;
    for (int n = 0; tier.shed >= per && n < 24; ++n) {
        tier.shed -= per;
        const D2D1_POINT_2F p = Map(tree, tier, c.lx + rng_.Range(-0.5f, 0.5f) * tier.cellW, c.ey - 0.5f * c.load);
        SnowClump k;
        k.x = p.x;
        k.y = p.y;
        k.vx = vx + rng_.Range(-25.0f, 25.0f) * s;
        k.vy = std::max(vy + rng_.Range(-30.0f, 10.0f) * s, -60.0f * s);
        k.d = rng_.Range(1.1f, 2.4f) * s;
        clumps.push_back(k);
    }
    tier.shed = std::min(tier.shed, 4.0f * per);  // a big shake doesn't queue up a blizzard
}

void Scenery::ShedHat(Snowman& m, const Cell& c, float area, float vx, float vy, std::vector<SnowClump>& clumps) {
    const float s = layout_.dpiScale;
    m.shed += area;
    const float per = kClumpArea * s * s;
    for (int n = 0; m.shed >= per && n < 16; ++n) {
        m.shed -= per;
        const D2D1_POINT_2F p = HatPoint(m, c.lx + rng_.Range(-1.5f, 1.5f) * s, c.ey - 0.5f * c.load);
        SnowClump k;
        k.x = p.x;
        k.y = p.y;
        k.vx = vx + rng_.Range(-25.0f, 25.0f) * s;
        k.vy = std::max(vy + rng_.Range(-30.0f, 10.0f) * s, -60.0f * s);
        k.d = rng_.Range(1.1f, 2.2f) * s;
        clumps.push_back(k);
    }
    m.shed = std::min(m.shed, 4.0f * per);
}

void Scenery::UpdateTreeSnow(Tree& tree, Tier& tier, float dt, std::vector<SnowClump>& clumps) {
    const float s = layout_.dpiScale;
    const float capPx = CapPx(tree);
    // Melt, and the animated clear.
    const float keep = std::exp(-dt / kMeltTau);
    const float sub = kSublimation * s * dt + (clearLeft_ > 0.0f ? clearRate_ * dt : 0.0f);
    for (std::vector<Cell>* shelf : {&tier.cells, &tier.skirt})
        for (Cell& c : *shelf)
            if (c.load > 0.0f) c.load = std::max(0.0f, c.load * keep - sub);

    const float k = std::min(0.25f, kDiffuse * dt);
    const float slide = ApproachFactor(dt, kSlideTau);
    for (int side = 0; side < 2; ++side) {
        // Walk each flank from the axis out to its tip.
        const int step = side ? 1 : -1;
        const int first = side ? tier.half : tier.half - 1;
        int prev = -1;
        for (int j = 0; j < tier.half; ++j) {
            const int i = first + step * j;
            Cell& c = tier.cells[static_cast<size_t>(i)];
            if (!c.exposed) continue;
            if (prev >= 0) {
                // Neighbours even out (mass conserving).
                Cell& p = tier.cells[static_cast<size_t>(prev)];
                const float f = k * (p.load - c.load);
                p.load -= f;
                c.load += f;
            }
            prev = i;
        }
        // Snow piled above a spot's cap slides on outwards, and off the tip.
        for (int j = 0; j < tier.half; ++j) {
            const int i = first + step * j;
            Cell& c = tier.cells[static_cast<size_t>(i)];
            if (!c.exposed) continue;
            const float excess = c.load - c.capF * capPx;
            if (excess <= 0.0f) continue;
            const float m = excess * slide;
            c.load -= m;
            if (j + 1 < tier.half) tier.cells[static_cast<size_t>(i + step)].load += m;
            else tier.overflow[side] += m * tier.cellW;
        }
    }

    // The bough tips: neighbours even out a little; what is too much for them comes loose.
    const float ks = std::min(0.25f, kSkirtDiffuse * dt);
    for (size_t i = 1; i < tier.skirt.size(); ++i) {
        const float f = ks * (tier.skirt[i - 1].load - tier.skirt[i].load);
        tier.skirt[i - 1].load -= f;
        tier.skirt[i].load += f;
    }
    for (Cell& c : tier.skirt) {
        const float excess = c.load - c.capF * capPx;
        if (excess <= 0.0f) continue;
        const float m = excess * slide;
        c.load -= m;
        tier.loose += m * tier.cellW;
    }

    // Now and then a laden bough lets its snow go in a soft clump, and springs up.
    if (tree.slideWait <= 0.0f && rng_.Chance(std::min(1.0f, 2.0f * dt))) {
        const float least = kSloughArea * s * s;
        int side = tier.overflow[1] > tier.overflow[0] ? 1 : 0;
        if (tier.overflow[side] > least) {
            const Cell& tip = tier.cells[static_cast<size_t>(side ? 2 * tier.half - 1 : 0)];
            const float out = side ? 1.0f : -1.0f;
            Shed(tree, tier, tip, tier.overflow[side], out * rng_.Range(10.0f, 35.0f) * s, rng_.Range(0.0f, 20.0f) * s,
                 clumps);
            tier.overflow[side] = 0.0f;
            tier.droopV[side] -= 0.6f * tier.bounceW * kDroop * tier.h;
            tree.slideWait = kSlideRest + rng_.Range(0.0f, 60.0f);
        } else if (tier.loose > least) {
            size_t most = 0;
            for (size_t i = 1; i < tier.skirt.size(); ++i)
                if (tier.skirt[i].load > tier.skirt[most].load) most = i;
            const Cell& c = tier.skirt[most];
            side = c.lx >= 0.0f ? 1 : 0;
            Shed(tree, tier, c, tier.loose, rng_.Range(-15.0f, 15.0f) * s, rng_.Range(0.0f, 20.0f) * s, clumps);
            tier.loose = 0.0f;
            tier.droopV[side] -= 0.4f * tier.bounceW * kDroop * tier.h;
            tree.slideWait = kSlideRest + rng_.Range(0.0f, 60.0f);
        }
    }

    // A bough swinging hard shakes its snow off. Motion: how fast the tips move,
    // in tier half-widths per second.
    const float motion = (std::fabs(tier.swayV) + std::max(std::fabs(tier.droopV[0]), std::fabs(tier.droopV[1]))) /
                         std::max(tier.w, 1.0f);
    const float shake = 3.0f * Saturate(motion - kShakeOff) * dt;
    if (shake > 0.0f) {
        for (std::vector<Cell>* shelf : {&tier.cells, &tier.skirt})
            for (Cell& c : *shelf) {
                if (!c.exposed || c.load <= 0.0f) continue;
                const float take = c.load * std::min(shake, 1.0f);
                c.load -= take;
                const float u = std::fabs(c.lx) / tier.w;
                Shed(tree, tier, c, take * tier.cellW, tier.swayV * Saturate(c.ey / tier.h),
                     tier.droopV[c.lx >= 0.0f ? 1 : 0] * u * u - rng_.Range(10.0f, 40.0f) * s, clumps);
            }
        // Shaken loose, it doesn't wait to slide.
        for (float& o : tier.overflow) o *= 1.0f - std::min(shake, 1.0f);
        tier.loose *= 1.0f - std::min(shake, 1.0f);
    }
}

void Scenery::UpdateHatSnow(Snowman& m, float dt, std::vector<SnowClump>& clumps) {
    const float s = layout_.dpiScale;
    const float capPx = CapPx(m);
    const float keep = std::exp(-dt / kMeltTau);
    const float sub = kSublimation * s * dt + (clearLeft_ > 0.0f ? clearRate_ * dt : 0.0f);
    for (Cell& c : m.hatSnow) {
        if (c.load > 0.0f) c.load = std::max(0.0f, c.load * keep - sub);
        // Too much for the hat slides off its edge.
        const float excess = c.load - c.capF * capPx;
        if (excess > 0.0f) c.load -= excess * ApproachFactor(dt, kSlideTau);
    }
    const float k = std::min(0.25f, kDiffuse * dt);
    for (size_t i = 1; i < m.hatSnow.size(); ++i) {
        if (m.hatSnow[i].seg != m.hatSnow[i - 1].seg) continue;  // the crown and each brim keep their own
        const float f = k * (m.hatSnow[i - 1].load - m.hatSnow[i].load);
        m.hatSnow[i - 1].load -= f;
        m.hatSnow[i].load += f;
    }
    // Rocked hard, the hat sheds it.
    const float motion = std::fabs(m.rockV) + 0.5f * std::fabs(m.hatTiltV);
    const float shake = 3.0f * Saturate(motion - 0.5f) * dt;
    if (shake > 0.0f) {
        for (Cell& c : m.hatSnow) {
            if (c.load <= 0.0f) continue;
            const float take = c.load * std::min(shake, 1.0f);
            c.load -= take;
            ShedHat(m, c, take * kCellDip * s, m.rockV * m.hatY * 0.6f, -rng_.Range(10.0f, 40.0f) * s, clumps);
        }
    }
}

void Scenery::Update(float dt, double time, float windPx, const Drift& drift, std::vector<SnowClump>& clumps) {
    time_ = time;
    if (relayoutAt_ >= 0.0 && time >= relayoutAt_) Relayout(false);
    if (trees_.empty() && snowmen_.empty()) return;
    if (!(dt > 0.0f)) dt = 0.0f;
    // Fade each kind in and out; a kind fully hidden drops its snow (and the flakes on it).
    auto fade = [&](float& f, bool show, bool trees) {
        if (f == (show ? 1.0f : 0.0f)) return;
        if (f == 0.0f) {
            // Appearing: the lights come on afresh, and windows over it are looked for at once.
            if (trees) {
                powerOn_ = 0.0f;
                for (Tree& t : trees_) t.probed = false;
            } else {
                for (Snowman& m : snowmen_) m.probed = false;
            }
            coverStale_ = true;
        }
        f = show ? std::min(1.0f, f + dt / 0.7f) : std::max(0.0f, f - dt / 0.5f);
        if (f > 0.0f) return;
        if (trees) {
            for (Tree& t : trees_)
                for (Tier& tier : t.tiers) {
                    for (std::vector<Cell>* shelf : {&tier.cells, &tier.skirt})
                        for (Cell& c : *shelf) c.load = 0.0f;
                    tier.overflow[0] = tier.overflow[1] = tier.shed = tier.loose = 0.0f;
                    tier.sway = tier.swayV = 0.0f;
                    tier.droop[0] = tier.droop[1] = tier.droopV[0] = tier.droopV[1] = 0.0f;
                }
            ++genTrees_;
        } else {
            for (Snowman& m : snowmen_) {
                for (Cell& c : m.hatSnow) c.load = 0.0f;
                m.rock = m.rockV = m.hatTilt = m.hatTiltV = 0.0f;
            }
            ++genMen_;
        }
    };
    fade(fadeTrees_, showTrees_, true);
    fade(fadeSnowmen_, showSnowmen_, false);
    if (!Active()) {
        // Nothing follows the drift while everything is hidden: whatever comes
        // back goes straight onto it.
        for (Tree& t : trees_) t.placed = false;
        for (Snowman& m : snowmen_) m.placed = false;
        return;
    }
    if (clearLeft_ > 0.0f) clearLeft_ -= dt;
    powerOn_ = std::min(powerOn_ + dt, 60.0f);
    // People who turn off Windows' animation effects get steady lights.
    if ((calmCheck_ -= dt) <= 0.0f) {
        calmCheck_ = 2.0f;
        BOOL animate = TRUE;
        calm_ = SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0) && !animate;
    }
    // One star sparkles now and then, never two close together on a screen.
    if (nextSparkle_ <= 0.0) nextSparkle_ = time + rng_.Range(6.0f, 15.0f);
    if (time >= nextSparkle_ && !trees_.empty()) {
        nextSparkle_ = time + rng_.Range(14.0f, 30.0f);
        const int pick = rng_.Int(0, static_cast<int>(trees_.size()) - 1);
        if (!calm_ && trees_[static_cast<size_t>(pick)].vis > 0.99f) {
            sparkleTree_ = pick;
            sparkleAt_ = time;
        }
    }

    const float s = layout_.dpiScale;
    const float gust = Clamp(windPx / (kGustWind * s), -1.0f, 1.0f);
    gust_ = gust;
    const int sub = std::max(1, static_cast<int>(std::ceil(dt * 240.0f)));
    const float h = dt / static_cast<float>(sub);
    float top = layout_.groundY;
    // Stand on the drift, a little sunk in. Rising snow lifts a prop gently to
    // whole pixels, only once it has really built up, so it never twitches as
    // the drift settles; snow going from under it (melting, cleared, plowed)
    // drops it straight down onto what is left.
    auto stand = [&](float& lift, float& goal, float& fall, bool& placed, float& snow, float& snowL, float& snowR,
                     float x, float footW, float spanW) {
        snow = snowL = snowR = 0.0f;
        if (drift.Width() > 0) {
            snow = std::min({drift.HeightAt(x - 0.5f * footW), drift.HeightAt(x), drift.HeightAt(x + 0.5f * footW)});
            snowL = drift.HeightAt(std::max(0.0f, x - spanW));
            snowR = drift.HeightAt(std::min(static_cast<float>(drift.Width() - 1), x + spanW));
        }
        const float want = std::max(0.0f, snow - std::min(0.3f * snow, 5.0f * s));
        if (!placed) {
            goal = lift = std::round(want);
            fall = 0.0f;
            placed = true;
            return;
        }
        if (std::fabs(want - goal) > 1.5f * s) goal = std::round(want);
        if (goal < lift) {
            fall += kDrop * s * dt;
            lift = std::max(goal, lift - fall * dt);
        } else {
            lift += (goal - lift) * ApproachFactor(dt, 1.2f);
            if (goal - lift < 0.05f) lift = goal;
        }
        if (lift == goal) fall = 0.0f;
    };
    auto step = [&](float& vis, bool covered) {
        vis = covered ? std::max(0.0f, vis - dt / 0.25f) : std::min(1.0f, vis + dt / 0.7f);
    };

    for (Tree& t : trees_) {
        stand(t.lift, t.liftGoal, t.fall, t.placed, t.snow, t.snowL, t.snowR, t.x, t.trunkW, t.tiers.front().w);
        step(t.vis, t.covered);
        if (t.slideWait > 0.0f) t.slideWait -= dt;
        if (fadeTrees_ <= 0.0f) continue;
        // Leaning with the wind, with a flutter in gusts.
        const float flutter = noise::Noise1(Wrap256(time * 1.3 + t.phase * 50.0)) * std::fabs(gust);
        const float leanRest = (kLeanMax * gust + 0.008f * flutter) * t.H;
        for (int i = 0; i < sub; ++i) Spring(t.lean, t.leanV, leanRest, kTau * kLeanHz, kLeanZeta, h);
        for (Tier& tier : t.tiers) {
            const float swayRest = 0.03f * tier.w * std::fabs(gust) * noise::Noise1(Wrap256(time * 1.7 + tier.gust));
            const float droopRest[2] = {LoadFraction(t, tier, 0) * kDroop * tier.h,
                                        LoadFraction(t, tier, 1) * kDroop * tier.h};
            for (int i = 0; i < sub; ++i) {
                Spring(tier.sway, tier.swayV, swayRest, tier.swayW, kSwayZeta, h);
                for (int side = 0; side < 2; ++side)
                    Spring(tier.droop[side], tier.droopV[side], droopRest[side], tier.bounceW, kBounceZeta, h);
            }
            // However hard it is brushed, a bough stays on the tree.
            tier.sway = Clamp(tier.sway, -0.35f * tier.w, 0.35f * tier.w);
            for (float& d : tier.droop) d = Clamp(d, -0.15f * tier.h, 0.3f * tier.h);
            UpdateTreeSnow(t, tier, dt, clumps);
        }
        top = std::min(top, layout_.groundY - t.lift - t.H - 2.0f * t.starR);
    }
    for (Snowman& m : snowmen_) {
        stand(m.lift, m.liftGoal, m.fall, m.placed, m.snow, m.snowL, m.snowR, m.x, m.r[0], 1.3f * m.r[0]);
        step(m.vis, m.covered);
        if (fadeSnowmen_ <= 0.0f) continue;
        // Heavy: the wind only rocks it a hair; the hat settles back on its own.
        const float rest = 0.006f * gust;
        for (int i = 0; i < sub; ++i) {
            Spring(m.rock, m.rockV, rest, m.rockW, kRockZeta, h);
            Spring(m.hatTilt, m.hatTiltV, 0.0f, kTau * kHatHz, kHatZeta, h);
        }
        m.rock = Clamp(m.rock, -kRockMax, kRockMax);
        m.hatTilt = Clamp(m.hatTilt, -kHatMax, kHatMax);
        UpdateHatSnow(m, dt, clumps);
        top = std::min(top, layout_.groundY - m.lift - m.H - 2.0f * s);
    }
    top_ = top - 4.0f * s;
}

void Scenery::Touch(float dt, float x, float y, float vx, float vy, std::vector<SnowClump>& clumps) {
    if (!Active() || y < top_ || !(dt > 0.0f)) return;
    const float s = layout_.dpiScale;
    const float speed = std::sqrt(vx * vx + vy * vy);
    if (speed < 25.0f * s) return;  // a resting pointer leaves the scenery alone
    const float reach = kReach * s;
    const float cvx = Clamp(vx, -1200.0f * s, 1200.0f * s), cvy = Clamp(vy, -1200.0f * s, 1200.0f * s);
    const float push = ApproachFactor(dt, 0.08f);
    const float knock = Saturate(speed / (280.0f * s)) * ApproachFactor(dt, 0.04f);
    for (Tree& t : trees_) {
        if (Alpha(false, t.vis) < 0.999f) continue;
        const float base = layout_.groundY - t.lift;
        if (std::fabs(x - t.x) > t.halfW || y > base || y < base - t.H - reach) continue;
        for (size_t k = 0; k < t.tiers.size(); ++k) {
            Tier& tier = t.tiers[k];
            // The pointer in tier-local coordinates (sway and droop are small).
            const D2D1_POINT_2F apex = Apex(t, tier);
            float ly = y - apex.y;
            const float lx = x - apex.x - Shear(t, tier) * ly;
            const float ax = std::fabs(lx);
            if (ax > tier.w + reach) continue;
            const int side = lx >= 0.0f ? 1 : 0;
            const float u = std::min(ax / tier.w, 1.0f);
            ly -= tier.droop[side] * u * u;
            const float snowTop =
                EdgeY(tier.h, tier.w, ax) - tier.cells[static_cast<size_t>(CellIndex(tier.half, tier.cellW, lx))].load;
            const float hem = HemLow(tier.h, tier.w, tier.arch, ax);
            if (ly < snowTop - reach || ly > hem + 0.5f * reach) continue;
            // Brushed aside, and pushed down by a pointer moving down through it;
            // the touched side leads, and the tiers around it (and the tree) feel it too.
            const float edge = 1.0f - SmoothStep(tier.w, tier.w + reach, ax);
            const float before = tier.swayV;
            tier.swayV += (0.22f * cvx - tier.swayV) * push * edge;
            const float kick = tier.swayV - before;
            if (k > 0) t.tiers[k - 1].swayV += kCouple * kick;
            if (k + 1 < t.tiers.size()) t.tiers[k + 1].swayV += kCouple * kick;
            t.leanV += kLeanCouple * kick;
            tier.droopV[side] += (0.22f * std::max(cvy, 0.0f) + 0.15f * std::fabs(cvx)) * push * edge * u;
            // And the snow around the pointer comes off.
            const float zone = reach + 0.5f * tier.cellW;
            for (std::vector<Cell>* shelf : {&tier.cells, &tier.skirt})
                for (Cell& c : *shelf) {
                    if (!c.exposed || c.load <= 0.0f) continue;
                    const float dx = std::fabs(c.lx - lx), dy = std::fabs(c.ey - 0.5f * c.load - ly);
                    if (dx > zone || dy > zone + c.load) continue;
                    const float take = c.load * knock * (1.0f - dx / zone) * (1.0f - dy / (zone + c.load + 1.0f));
                    c.load -= take;
                    Shed(t, tier, c, take * tier.cellW, cvx * rng_.Range(0.2f, 0.55f),
                         std::min(0.25f * cvy, 0.0f) - rng_.Range(30.0f, 100.0f) * s, clumps);
                }
        }
    }
    for (Snowman& m : snowmen_) {
        if (Alpha(true, m.vis) < 0.999f) continue;
        const float base = layout_.groundY - m.lift;
        if (std::fabs(x - m.x) > m.halfW || y > base + reach || y < base - m.H - reach) continue;
        // The pointer in body coordinates (undoing the rock).
        const float c = std::cos(m.rock), sn = std::sin(m.rock);
        const float px = x - m.x, py = y - base;
        const float bx = px * c + py * sn, by = -px * sn + py * c;
        bool hit = false;
        for (int i = 0; i < 3; ++i) {
            const float dx = bx, dy = by + m.c[i];
            if (dx * dx + dy * dy < (m.r[i] + reach) * (m.r[i] + reach)) hit = true;
        }
        const float hatTop = m.hatY + (m.hat == 0 ? 1.15f : 1.05f) * m.r[2];
        const bool atHat = std::fabs(bx) < 1.0f * m.r[2] + reach && -by > m.hatY - reach && -by < hatTop + reach;
        if (!hit && !atHat) continue;
        // Bumped: it rocks (more for a bump up high) and its hat wobbles on its head.
        const float lever = Saturate(-by / std::max(m.H, 1.0f));
        m.rockV += (0.1f * cvx / std::max(m.H, 1.0f) * (0.3f + 0.7f * lever) - m.rockV) * push;
        m.hatTiltV += (atHat ? 0.3f : 0.12f) * cvx / std::max(m.r[2], 1.0f) * push;
        if (!atHat) continue;
        for (Cell& cell : m.hatSnow) {
            if (cell.load <= 0.0f) continue;
            const float dx = std::fabs(cell.lx - bx);
            if (dx > reach + 2.0f * s) continue;
            const float take = cell.load * knock * (1.0f - dx / (reach + 2.0f * s));
            cell.load -= take;
            ShedHat(m, cell, take * kCellDip * s, cvx * rng_.Range(0.2f, 0.55f),
                    std::min(0.25f * cvy, 0.0f) - rng_.Range(30.0f, 90.0f) * s, clumps);
        }
    }
}

bool Scenery::Catch(float x, float y0, float y1, float r, float amount, uint32_t id, Perch* perch) {
    if (snowDepth_ <= 0.0f || !Active() || y1 + r < top_) return false;
    const float s = layout_.dpiScale;
    if (2.0f * r > kMaxCatchDip * s) return false;
    const float dDip = Clamp(2.0f * r / s, 0.5f, kMaxCatchDip);
    const float area = kTreeVolume * std::pow(dDip, kVolumeExp) * s * s * std::min(amount, 64.0f) * rng_.Range(0.7f, 1.3f);
    // Came down past `surface` this frame (not already below it: then it is in front).
    auto crossed = [&](float surface) { return y1 + 0.5f * r >= surface && y0 + 0.5f * r < surface + 1.5f * s; };
    // Whether it settles on a surface or passes in front: the same answer
    // every frame it spends crossing, so the odds don't grow with the frame rate.
    auto settles = [&](float chance, int kind, int prop, int part, int shelf) {
        const uint32_t where = static_cast<uint32_t>(((kind * 64 + prop) * 8 + part) * 4 + shelf);
        return HashFloat(id * 0x9E3779B1u ^ Hash32(where + 0x2545F491u)) < chance;
    };
    auto settle = [&](int kind, int prop, int part, int shelf, float lx, float load) {
        if (perch) *perch = {kind, prop, part, shelf, lx, load, kind == 0 ? genTrees_ : genMen_};
        return true;
    };
    for (size_t ti = 0; ti < trees_.size(); ++ti) {
        Tree& t = trees_[ti];
        if (std::fabs(x - t.x) > t.halfW || Alpha(false, t.vis) < 0.999f) continue;
        const float capPx = CapPx(t);
        // The tree's outline: the flank of the top-most tier over x. Flakes at
        // about the trunk's depth settle there (where it isn't too steep); the
        // rest pass on in front.
        for (int k = static_cast<int>(t.tiers.size()) - 1; k >= 0; --k) {
            Tier& tier = t.tiers[static_cast<size_t>(k)];
            const D2D1_POINT_2F apex = Apex(t, tier);
            float lx = x - apex.x;
            if (std::fabs(lx) > tier.w) continue;
            lx -= Shear(t, tier) * EdgeY(tier.h, tier.w, std::fabs(lx));
            const float ax = std::fabs(lx);
            if (ax > tier.w) break;
            const float u = ax / tier.w;
            const Cell& c = tier.cells[static_cast<size_t>(CellIndex(tier.half, tier.cellW, lx))];
            const float surface = apex.y + EdgeY(tier.h, tier.w, ax) + tier.droop[lx >= 0.0f ? 1 : 0] * u * u - 0.9f * c.load;
            if (c.exposed && c.capF >= 0.25f && crossed(surface) && settles(kFlankCatch, 0, static_cast<int>(ti), k, 0)) {
                Deposit(tier, capPx, false, lx, area);
                return settle(0, static_cast<int>(ti), k, 0, lx, c.load);
            }
            break;
        }
        // Further down, in front of the trunk: the bough tips along each hem.
        for (int k = static_cast<int>(t.tiers.size()) - 1; k >= 0; --k) {
            Tier& tier = t.tiers[static_cast<size_t>(k)];
            const D2D1_POINT_2F apex = Apex(t, tier);
            const float lx = x - apex.x - Shear(t, tier) * tier.h;
            if (std::fabs(lx) > tier.w) continue;
            const float u = lx / tier.w;
            const Cell& c = tier.skirt[static_cast<size_t>(SkirtIndex(tier.half, tier.cellW, tier.w, lx))];
            const float surface = apex.y + c.ey + tier.droop[lx >= 0.0f ? 1 : 0] * u * u - 0.55f * c.load;
            if (crossed(surface) && settles(kSkirtCatch, 0, static_cast<int>(ti), k, 1)) {
                Deposit(tier, capPx, true, lx, area);
                return settle(0, static_cast<int>(ti), k, 1, lx, c.load);
            }
        }
    }
    // Snowmen's hats.
    for (size_t mi = 0; mi < snowmen_.size(); ++mi) {
        Snowman& m = snowmen_[mi];
        if (std::fabs(x - m.x) > m.halfW || Alpha(true, m.vis) < 0.999f) continue;
        const float capPx = CapPx(m);
        if (capPx <= 0.0f) continue;
        for (Cell& c : m.hatSnow) {
            const D2D1_POINT_2F p = HatPoint(m, c.lx, c.ey - 0.9f * c.load);
            if (std::fabs(p.x - x) > 0.5f * kCellDip * s + r * 0.5f || !crossed(p.y) ||
                !settles(kHatCatch, 1, static_cast<int>(mi), 0, c.seg))
                continue;
            const float add = area / (kCellDip * s);
            const float cap = std::max(c.capF * capPx, 1e-3f);
            const float keep = 1.0f - SmoothStep(kCapKnee, 1.0f, c.load / cap);
            c.load += std::min(add * keep, std::max(0.0f, cap - c.load));  // never past the cap at once
            return settle(1, static_cast<int>(mi), 0, 0, c.lx, c.load);
        }
    }
    return false;
}

bool Scenery::PerchPoint(const Perch& perch, float& x, float& y) const {
    if (perch.gen != (perch.kind == 0 ? genTrees_ : genMen_) || perch.prop < 0) return false;
    const float s = layout_.dpiScale;
    if (perch.kind == 0) {
        if (perch.prop >= static_cast<int>(trees_.size())) return false;
        const Tree& t = trees_[static_cast<size_t>(perch.prop)];
        if (perch.part < 0 || perch.part >= static_cast<int>(t.tiers.size())) return false;
        const Tier& tier = t.tiers[static_cast<size_t>(perch.part)];
        const bool skirt = perch.shelf == 1;
        const Cell& c = skirt ? tier.skirt[static_cast<size_t>(SkirtIndex(tier.half, tier.cellW, tier.w, perch.lx))]
                              : tier.cells[static_cast<size_t>(CellIndex(tier.half, tier.cellW, perch.lx))];
        if (c.load < 0.35f * perch.load - 0.3f * s) return false;  // knocked off or melted
        const float ly = skirt ? c.ey - 0.55f * c.load : EdgeY(tier.h, tier.w, std::fabs(perch.lx)) - 0.8f * c.load;
        const D2D1_POINT_2F p = Map(t, tier, perch.lx, ly);
        x = p.x;
        y = p.y;
        return true;
    }
    if (perch.kind == 1 && perch.prop < static_cast<int>(snowmen_.size())) {
        const Snowman& m = snowmen_[static_cast<size_t>(perch.prop)];
        for (const Cell& c : m.hatSnow) {
            if (c.lx != perch.lx) continue;
            if (c.load < 0.35f * perch.load - 0.3f * s) return false;
            const D2D1_POINT_2F p = HatPoint(m, c.lx, c.ey - 0.8f * c.load);
            x = p.x;
            y = p.y;
            return true;
        }
    }
    return false;
}

void Scenery::ProbeCover(const std::function<bool(float, float)>& covered) {
    coverStale_ = false;
    // A hidden kind isn't looked at (it is again when it reappears). A prop
    // looked at for the first time takes the answer at once, so it never shows
    // over a window, even for a moment.
    // Points spread over each prop, its outermost parts included (bough tips,
    // the star, arms and hat brim), so a window over any part of it counts.
    if (fadeTrees_ > 0.0f) {
        for (Tree& t : trees_) {
            const float base = layout_.groundY - t.lift;
            const float w = t.tiers.front().w, top = base - t.H - t.starR;
            auto tips = [&](const Tier& tier) {
                const float y = base - tier.apexY + 0.95f * tier.h;
                return covered(t.x - 0.9f * tier.w, y) || covered(t.x + 0.9f * tier.w, y);
            };
            t.covered = covered(t.x, top + 0.15f * (base - top)) || covered(t.x - 0.5f * w, base - 0.3f * t.H) ||
                        covered(t.x + 0.5f * w, base - 0.3f * t.H) || covered(t.x, base - 0.1f * t.H) ||
                        tips(t.tiers.front()) || tips(t.tiers[1]) || tips(t.tiers.back()) ||
                        covered(t.x, base - t.H - 0.9f * t.starR);
            if (!t.probed) t.vis = t.covered ? 0.0f : 1.0f;
            t.probed = true;
        }
    }
    if (fadeSnowmen_ > 0.0f) {
        for (Snowman& m : snowmen_) {
            const float base = layout_.groundY - m.lift;
            const float arms = base - m.c[1], brim = base - m.hatY - 0.1f * m.r[2];
            m.covered = covered(m.x, base - m.H + 0.1f * m.r[2]) || covered(m.x, base - m.c[1]) ||
                        covered(m.x - 0.7f * m.r[0], base - m.c[0]) || covered(m.x + 0.7f * m.r[0], base - m.c[0]) ||
                        covered(m.x - 1.6f * m.r[1], arms) || covered(m.x + 1.6f * m.r[1], arms) ||
                        covered(m.x - 0.85f * m.r[2], brim) || covered(m.x + 0.85f * m.r[2], brim);
            if (!m.probed) m.vis = m.covered ? 0.0f : 1.0f;
            m.probed = true;
        }
    }
}

void Scenery::ClearSnow(bool animated) {
    float most = 0.0f;
    auto clear = [&](Cell& c) {
        most = std::max(most, c.load);
        if (!animated) c.load = 0.0f;
    };
    for (Tree& t : trees_)
        for (Tier& tier : t.tiers) {
            for (std::vector<Cell>* shelf : {&tier.cells, &tier.skirt})
                for (Cell& c : *shelf) clear(c);
            tier.overflow[0] = tier.overflow[1] = 0.0f;
            tier.shed = tier.loose = 0.0f;
        }
    for (Snowman& m : snowmen_) {
        for (Cell& c : m.hatSnow) clear(c);
        m.shed = 0.0f;
    }
    if (animated && most > 0.0f) {
        clearLeft_ = 2.0f;
        clearRate_ = most / clearLeft_;
    } else {
        clearLeft_ = clearRate_ = 0.0f;
    }
}

// ---------------------------------------------------------------------------
// Lights
// ---------------------------------------------------------------------------

// Brightness of every bulb on a tree (by its place on the string), 0..1. Like a
// real two-wire string, alternate bulbs form two sets. Each group of trees runs
// one of a few slow patterns (well under one change a second) and nothing ever
// flashes; with animation effects off the lights burn steadily.
void Scenery::LightLevels(const Tree& tree, std::vector<float>& out) const {
    out.resize(static_cast<size_t>(tree.bulbs));
    const double t = time_;
    const float n = static_cast<float>(std::max(tree.bulbs, 1));
    for (const Tier& tier : tree.tiers) {
        for (const Bulb& b : tier.bulbs) {
            float v = 0.85f;
            if (!calm_) {
                switch (tree.mode) {
                case 0:  // slow fade: the two sets cross-fade, so the tree's glow stays even
                    v = 0.775f + 0.225f * std::cos(kTau * Frac(t / 7.5 + 0.5 * (b.order & 1) + tree.phase));
                    break;
                case 1: {  // slo-glo: the whole string breathes, gently
                    const float e = std::exp(std::sin(kTau * Frac(t / 8.0 + tree.phase)));
                    v = 0.7f + 0.3f * (e - 0.367879f) / (2.718282f - 0.367879f);
                    break;
                }
                case 2:  // waves rolling gently up the string
                    v = 0.75f + 0.25f * std::cos(kTau * Frac(t / 5.0 - b.order / n + tree.phase));
                    break;
                default: {  // soft twinkle: now and then a bulb dims a little and comes back
                    const double period = 14.0 + 10.0 * b.phase;
                    const double at = t / period + b.phase;
                    const double cycle = std::floor(at);
                    const float start = HashFloat(static_cast<uint32_t>(b.order) * 977u + static_cast<uint32_t>(cycle)) *
                                        static_cast<float>(period - 2.3);
                    const float u = static_cast<float>((at - cycle) * period) - start;
                    v = 0.95f;
                    if (u > 0.0f && u < 0.7f) v -= 0.35f * Ease(u / 0.7f);
                    else if (u >= 0.7f && u < 2.1f) v -= 0.35f * (1.0f - Ease((u - 0.7f) / 1.4f));
                    break;
                }
                }
            }
            // Switched on one by one.
            const float on = 0.25f + kPowerOn * static_cast<float>(b.order) / n;
            out[static_cast<size_t>(b.order)] = Saturate(v) * SmoothStep(on, on + 0.18f, powerOn_);
        }
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void Scenery::ReleaseDeviceResources() {
    page_.Reset();
    pageGen_ = 0;
}

bool Scenery::Prepare(ID2D1DeviceContext* dc) {
    if (!Active() || !dc) return false;
    if (page_ && pageGen_ == layoutGen_) return true;
    const double now = QpcSeconds();
    if (now < pageRetry_) return false;
    page_.Reset();
    if (FAILED(BuildPage(dc))) {
        page_.Reset();
        pageRetry_ = now + 1.0;
        return false;
    }
    pageGen_ = layoutGen_;
    return true;
}

// A prop's opacity: its kind's fade and its own stepping aside, both eased.
float Scenery::Alpha(bool snowman, float vis) const {
    const float f = snowman ? fadeSnowmen_ : fadeTrees_;
    return Ease(Saturate(f)) * Ease(Saturate(vis));
}

// The drift's surface under a tree, dx px from its trunk (y in scene px).
float Scenery::SurfaceY(const Tree& tree, float dx) const {
    const float w = std::max(tree.tiers.front().w, 1.0f);
    const float u = Saturate(std::fabs(dx) / w);
    return layout_.groundY - Lerp(tree.snow, dx < 0.0f ? tree.snowL : tree.snowR, u);
}

void Scenery::EmitProps(std::vector<ScenerySprite>& out) const {
    if (!Active() || !page_ || pageGen_ != layoutGen_) return;
    const float s = layout_.dpiScale;
    auto put = [&](const Sprite& sp, float k, float x, float y, float angle, D2D1_COLOR_F col) {
        out.push_back(Quad(sp, k, k, angle, x, y, col));
    };
    // An oval `w` x `h` px of a levelled sprite, turned by `angle`.
    auto oval = [&](const Sprite* sp, const float* px, float x, float y, float w, float h, float angle, D2D1_COLOR_F col) {
        const int l = PickLevel(px, kLevels, std::max(w, h));
        out.push_back(Quad(sp[l], w / px[l], h / px[l], angle, x, y, col));
    };
    // Snow lying along a shelf: ovals laid along it make one soft band standing a
    // little proud of the edge, white on top (cooler away from the light), blue
    // below, with a few lumps and drips where it is deep.
    auto snowBand = [&](const std::vector<Cell>& cells, float cellW, float halfW, float alpha, auto&& place) {
        for (int pass = 0; pass < 4; ++pass) {
            for (const Cell& c : cells) {
                if (!c.exposed || c.load < 0.15f * s) continue;
                if (pass == 2 && (c.wobble < 0.55f || c.load < 1.2f * s)) continue;  // lumps
                if (pass == 3 && (c.wobble < 0.88f || c.load < 1.8f * s)) continue;  // drips
                const float thick = c.load + 0.6f * s;
                const float a = alpha * Saturate(c.load / (0.6f * s));
                D2D1_POINT_2F p;
                float angle;
                place(c, p, angle);
                float len = (1.8f + 0.3f * c.wobble) * cellW + 0.5f * thick, across = thick;
                float off = 0.08f * thick;  // along the outward normal
                Rgb col = Mix(Mix(kWhite, kSnowShade, 0.3f * Saturate(c.lx / halfW)), kSnowUnder, 0.55f * c.shade);
                if (pass == 0) {
                    across = thick + s;
                    off = -0.3f * thick - 0.35f * s;
                    col = Mix(kSnowShade, kSnowUnder, c.shade);
                } else if (pass == 2) {
                    len = across = 1.05f * thick;
                    off = 0.35f * thick;
                } else if (pass == 3) {
                    len = across = 0.32f * thick + 0.5f * s;
                    off = -0.62f * thick;
                    col = Mix(kWhite, kSnowShade, 0.5f);
                }
                const float nx = std::sin(angle), ny = -std::cos(angle);  // the band's outward normal
                oval(blob_, blobPx_, p.x + nx * off, p.y + ny * off, len, across, angle, Color(col, a));
            }
        }
    };

    for (const Tree& t : trees_) {
        const float alpha = Alpha(false, t.vis);
        if (alpha <= 0.004f) continue;
        const D2D1_COLOR_F plain = {1.0f, 1.0f, 1.0f, alpha};
        LightLevels(t, levels_);
        put(t.trunk, 1.0f, t.x, layout_.groundY - t.lift, 0.0f, plain);
        for (const Tier& tier : t.tiers) {
            const D2D1_POINT_2F apex = Apex(t, tier);
            const float shear = Shear(t, tier);
            // The boughs, in strips that follow the droop of their side.
            const float top = static_cast<float>(tier.art.src.top) - tier.art.cy;
            const float bottom = static_cast<float>(tier.art.src.bottom) - tier.art.cy;
            for (size_t j = 0; j + 1 < tier.strips.size(); ++j) {
                const UINT32 sx0 = tier.strips[j], sx1 = tier.strips[j + 1];
                const float lx0 = static_cast<float>(sx0) - tier.art.cx, lx1 = static_cast<float>(sx1) - tier.art.cx;
                const float d = tier.droop[lx0 + lx1 >= 0.0f ? 1 : 0];
                const float u0 = lx0 / tier.w, u1 = lx1 / tier.w;
                const float dy0 = d * u0 * u0;
                const float slope = (d * u1 * u1 - dy0) / (lx1 - lx0);
                // Neighbouring strips share their edge exactly, and aliased sprites
                // cover each pixel once: no seam, and no column drawn twice.
                out.push_back({{0.0f, top, static_cast<float>(sx1 - sx0), bottom},
                               {sx0, tier.art.src.top, sx1, tier.art.src.bottom},
                               plain,
                               {1.0f, slope, shear, 1.0f, apex.x + lx0, apex.y + dy0}});
            }
            snowBand(tier.cells, tier.cellW, tier.w, alpha, [&](const Cell& c, D2D1_POINT_2F& p, float& angle) {
                p = Map(t, tier, c.lx, c.ey);
                angle = std::atan2(c.nx, -c.ny);
            });
            snowBand(tier.skirt, tier.cellW, tier.w, alpha, [&](const Cell& c, D2D1_POINT_2F& p, float& angle) {
                p = Map(t, tier, c.lx, c.ey);
                angle = std::atan2(c.nx, -c.ny);
            });
            // Bulbs in front of the bough-tip snow: coloured glass, dim but never dark
            // (their glow comes later, over everything).
            for (const Bulb& b : tier.bulbs) {
                const D2D1_POINT_2F p = Map(t, tier, b.lx, b.ly);
                const float v = levels_[static_cast<size_t>(b.order)];
                const float k = 0.7f + 0.3f * v;
                const Rgb c = Bulbs(t.lights, b.color).glass;
                const int l = PickLevel(bulbPx_, kLevels, b.size);
                put(bulb_[l], b.size / bulbPx_[l], p.x, p.y, 0.0f, {c.r * k, c.g * k, c.b * k, alpha});
            }
        }
    }

    for (const Snowman& m : snowmen_) {
        const float alpha = Alpha(true, m.vis);
        if (alpha <= 0.004f) continue;
        const D2D1_COLOR_F plain = {1.0f, 1.0f, 1.0f, alpha};
        put(m.body, 1.0f, m.x, layout_.groundY - m.lift, m.rock, plain);
        // The scarf's tail hangs from the knot and streams a little downwind
        // (a positive turn swings its end to the left), lagging the rocking.
        const float stream = -0.35f * gust_;
        const double t = time_;
        const float sway = calm_ ? 0.0f
                                 : 0.06f * std::sin(kTau * Frac(t / 2.7 + 0.01 * m.flutter)) +
                                       0.08f * noise::Noise1(Wrap256(t * 1.9 + m.flutter));
        const D2D1_POINT_2F knot = BodyPoint(m, m.knotX, -m.knotY);
        put(m.tail, 1.0f, knot.x, knot.y, m.rock + stream + sway + 0.3f * m.rockV / std::max(m.rockW, 1.0f), plain);
        // The hat, and the snow on it.
        const D2D1_POINT_2F foot = BodyPoint(m, 0.0f, -m.hatY);
        const float hatAngle = m.rock + m.hatTilt0 + m.hatTilt;
        put(m.hatArt, 1.0f, foot.x, foot.y, hatAngle, plain);
        snowBand(m.hatSnow, kCellDip * s, m.r[2], alpha, [&](const Cell& c, D2D1_POINT_2F& p, float& angle) {
            p = HatPoint(m, c.lx, c.ey);
            angle = hatAngle + std::atan2(c.nx, -c.ny);
        });
    }
}

void Scenery::EmitGround(std::vector<ScenerySprite>& out) const {
    if (!Active() || !page_ || pageGen_ != layoutGen_) return;
    const float s = layout_.dpiScale;
    auto blob = [&](float x, float y, float w, float h, float angle, D2D1_COLOR_F col) {
        if (col.a <= 0.004f) return;
        const int l = PickLevel(blobPx_, kLevels, std::max(w, h));
        out.push_back(Quad(blob_[l], w / blobPx_[l], h / blobPx_[l], angle, x, y, col));
    };
    // Snow heaped round a foot, on the drift's surface and tilted with it, with
    // the prop's shade on it in the drift's cool blue, a little to the right
    // (the light comes from the upper left).
    auto mound = [&](float x, float snow, float snowL, float snowR, float W, float m, float alpha) {
        const float baseY = layout_.groundY - snow;
        const float slope = (snowR - snowL) / std::max(W, 1.0f);  // height gained per px to the right
        auto surf = [&](float dx) { return baseY - slope * dx; };
        const float angle = std::atan2(-slope, 1.0f);
        blob(x, surf(0.0f), 0.62f * W, 2.0f * m, angle, Color(kMound, alpha));
        blob(x - 0.28f * W, surf(-0.28f * W) + 0.15f * m, 0.42f * W, 1.4f * m, angle, Color(kMound, alpha));
        blob(x + 0.30f * W, surf(0.30f * W) + 0.2f * m, 0.40f * W, 1.3f * m, angle, Color(kMound, alpha));
        const float sw = W, sh = std::max(3.5f * s, 0.16f * W);
        blob(x + 0.1f * W, surf(0.1f * W) - 0.55f * m + 0.5f * sh, sw, sh, angle, Color(kShadeBlue, 0.22f * alpha));
        blob(x + 0.04f * W, surf(0.04f * W) - 0.7f * m + 0.225f * sh, 0.36f * W, 0.45f * sh, angle,
             Color(kShadeBlue, 0.2f * alpha));
    };
    for (const Tree& t : trees_) {
        const float alpha = Alpha(false, t.vis);
        if (alpha <= 0.004f) continue;
        const float W = 2.0f * t.tiers.front().w;
        // Smaller where there is no drift: just a little heap on the taskbar's edge.
        const float m = std::max(3.0f * s, 0.06f * t.H) * (t.snow > 1.5f * s ? 1.0f : 0.75f);
        mound(t.x, t.snow, t.snowL, t.snowR, W, m, alpha);
        // Coloured light pooling on the snow under the lowest bulbs.
        LightLevels(t, levels_);
        const float slope = (t.snowR - t.snowL) / std::max(W, 1.0f);
        for (const Tier& tier : t.tiers) {
            for (const Bulb& b : tier.bulbs) {
                const D2D1_POINT_2F p = Map(t, tier, b.lx, b.ly);
                const float sy = SurfaceY(t, p.x - t.x);
                const float gap = sy - p.y;  // px the bulb hangs above the snow
                if (gap < -2.0f * s || gap > 12.0f * s) continue;
                const float k = 1.0f - Saturate(gap / (12.0f * s));
                const float v = levels_[static_cast<size_t>(b.order)];
                blob(p.x, sy + 0.9f * b.size, 5.0f * b.size, 1.8f * b.size, std::atan2(-slope, 1.0f),
                     Color(Bulbs(t.lights, b.color).glass, 0.2f * k * v * alpha));
            }
        }
    }
    for (const Snowman& m : snowmen_) {
        const float alpha = Alpha(true, m.vis);
        if (alpha <= 0.004f) continue;
        const float W = 2.6f * m.r[0];
        mound(m.x, m.snow, m.snowL, m.snowR, W, std::max(3.0f * s, 0.25f * m.r[0]) * (m.snow > 1.5f * s ? 1.0f : 0.75f),
              alpha);
    }
}

void Scenery::EmitGlow(std::vector<ScenerySprite>& out) const {
    if (!Active() || !page_ || pageGen_ != layoutGen_) return;
    const bool mist = glowAlpha_ >= 1.0f;
    auto glow = [&](float x, float y, float d, Rgb c, float a) {
        if (a <= 0.004f) return;
        const int l = PickLevel(glowPx_, kLevels, d);
        const float k = d / glowPx_[l];
        out.push_back(Quad(glow_[l], k, k, 0.0f, x, y, Glow(c, a, glowAlpha_)));
    };
    const double t = time_;
    for (size_t ti = 0; ti < trees_.size(); ++ti) {
        const Tree& tree = trees_[ti];
        const float alpha = Alpha(false, tree.vis);
        if (alpha <= 0.004f) continue;
        LightLevels(tree, levels_);
        for (const Tier& tier : tree.tiers) {
            for (const Bulb& b : tier.bulbs) {
                const D2D1_POINT_2F p = Map(tree, tier, b.lx, b.ly);
                // A bulb sunk in the drift glows only through what shows of it.
                const float clear = SurfaceY(tree, p.x - tree.x) - p.y;
                const float vis = Saturate((clear + 0.5f * b.size) / b.size);
                const float v = levels_[static_cast<size_t>(b.order)] * alpha * vis;
                const BulbColor& c = Bulbs(tree.lights, b.color);
                if (mist) {
                    // Ordinary blending can't brighten: a firmer core, a faint mist.
                    glow(p.x, p.y, 3.0f * b.size, Mix(c.halo, kWhite, 0.15f), kMistAlpha * 0.45f * v);
                    glow(p.x, p.y, 1.5f * b.size, Mix(c.glass, kWhite, 0.45f), std::min(1.0f, 0.85f * v));
                    continue;
                }
                glow(p.x, p.y, 5.5f * b.size, c.halo, 0.10f * v);              // wide bloom
                glow(p.x, p.y, 3.0f * b.size, c.halo, 0.40f * v);              // halo
                glow(p.x, p.y, 1.5f * b.size, Mix(c.glass, kWhite, 0.25f), 0.7f * v);  // hot core
            }
        }
        // The star: a slowly breathing halo behind it (so its own shading stays),
        // a hint of light at its heart, and now and then a sparkle.
        const Tier& top = tree.tiers.back();
        const D2D1_POINT_2F p = Map(tree, top, 0.0f, -0.25f * tree.starR);
        const float power = SmoothStep(0.1f, 0.6f, powerOn_);
        const float breath = calm_ ? 0.95f : 0.9f + 0.1f * (0.5f + 0.5f * std::sin(kTau * Frac(t / 8.0 + tree.phase + 0.37)));
        glow(p.x, p.y, 4.4f * tree.starR, kStarGlow, (mist ? 0.35f : 0.45f) * breath * power * alpha);
        out.push_back(Quad(tree.star, 1.0f, 1.0f, -std::atan(Shear(tree, top)), p.x, p.y, {1.0f, 1.0f, 1.0f, alpha}));
        glow(p.x, p.y, 1.0f * tree.starR, kStarCore, 0.15f * power * alpha);
        if (static_cast<int>(ti) == sparkleTree_ && !calm_) {
            const float u = static_cast<float>(t - sparkleAt_);
            if (u >= 0.0f && u < 1.2f) {
                const float e = std::sin(kPi * u / 1.2f);
                const float d = 2.5f * tree.starR * (0.75f + 0.25f * e);
                out.push_back(Quad(rays_, d / raysPx_, d / raysPx_, 0.0f, p.x, p.y, Glow(kSparkle, 0.7f * e * e * alpha, glowAlpha_)));
            }
        }
    }
}
