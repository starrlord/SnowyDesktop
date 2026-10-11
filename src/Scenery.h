// The scenery standing in the taskbar drift: small Christmas trees, with
// strings of glowing, pulsing lights and a star on top, and snowmen.
//
// A scene places its props along the ground line (the same spots for the same
// monitor and amounts): groups of trees, and snowmen in the open stretches
// between them, facing the trees. A tree's tiers are spring-mounted: the
// pointer brushes them aside and knocks their snow off, snow weighs their tips
// down, and they spring back when relieved. Snowmen wobble when bumped, their
// scarves flutter in the wind and snow settles on their hats. Everything
// stands on the drift and rises and settles with it, steps aside where a
// window covers it, and is never moved or removed by the pointer.
//
// Drawing uses one sprite page per scene, built on the device when the props
// are laid out: tree tiers (drawn as vertical strips so the boughs can bend),
// snowmen, snow blobs, bulbs and glows. The scene feeds the sprites through
// its sprite batch. Glow sprites are tinted so that the premultiplied result
// has more colour than alpha: they add light to the desktop rather than
// cover it.
#pragma once

#include "Common.h"
#include "Drift.h"

#include <d2d1_3.h>
#include <functional>

struct SceneryLayout {
    int width = 0, height = 0;  // scene px
    float dpiScale = 1.0f;      // px per DIP
    float groundY = 0.0f;       // px from the top: the line the props stand on
    float originX = 0.0f;       // scene's left edge in virtual-screen px: seeds the layout

    bool operator==(const SceneryLayout&) const = default;
};

// One sprite in the scene's batch: a cell of Page() drawn through `xf`.
struct ScenerySprite {
    D2D1_RECT_F dst;
    D2D1_RECT_U src;
    D2D1_COLOR_F color;  // straight alpha tint
    D2D1_MATRIX_3X2_F xf;
};

// Snow knocked or sliding off a prop, for the scene to turn into powder.
struct SnowClump {
    float x, y, vx, vy, d;  // px, px/s, diameter px
};

// Where a flake came to rest on a prop.
struct Perch {
    int kind = -1;        // 0 tree, 1 snowman; -1 none
    int prop = 0, part = 0, shelf = 0;
    float lx = 0.0f;      // along the shelf, px from the part's axis
    float load = 0.0f;    // snow thickness there when it landed (px)
    unsigned gen = 0;     // that kind's generation
};

class Scenery {
public:
    // The amount (0..100) that gives the usual few trees or snowmen.
    static constexpr int kUsualAmount = 35;

    Scenery();
    ~Scenery();
    Scenery(const Scenery&) = delete;
    Scenery& operator=(const Scenery&) = delete;

    // Lays the props out for a scene (keeps them when nothing relevant changed).
    void Configure(const SceneryLayout& layout);
    // Shown or hidden (they fade), and how many (0..100, see kUsualAmount). A
    // new amount only rebuilds the props when it changes how many there are,
    // and waits until the amounts have rested for `settle` seconds (a dragged
    // slider passes through many); trees keep their looks, snow and lights
    // when only the snowmen change.
    void SetTrees(bool show, int amount);
    void SetSnowmen(bool show, int amount);
    void SetSettle(float seconds) { settle_ = seconds; }
    // Whether glows may add light (premultiplied colour above alpha). Where
    // Windows composes in floating point it clamps such pixels; there they are
    // drawn as ordinary soft colour instead.
    void SetAdditiveGlow(bool on);
    // How deep snow may lie on boughs and hats, relative to the usual (0 =
    // none: what is there melts away).
    void SetSnowDepth(float scale);
    void ClearSnow(bool animated);

    // Anything to simulate or draw.
    bool Active() const;
    // Highest point of any prop (px from the top), for quick rejects.
    float Top() const { return top_; }

    // Springs, wind, snow settling and melt; props ride on the drift.
    void Update(float dt, double time, float windPx, const Drift& drift, std::vector<SnowClump>& clumps);
    // The pointer at (x, y) moving at (vx, vy) px/s sways the boughs it brushes,
    // bumps snowmen and knocks snow off.
    void Touch(float dt, float x, float y, float vx, float vy, std::vector<SnowClump>& clumps);
    // Something falling at x moved from y0 to y1 (its centre; radius r): if it
    // came down on a snow-catching surface it lands there, adding snow worth
    // `amount` flakes of its size. Fills `perch` (optional) on landing. `id`
    // tells falling things apart: whether one settles on a given surface or
    // passes in front of it is decided once, however often it is asked.
    bool Catch(float x, float y0, float y1, float r, float amount, uint32_t id, Perch* perch);
    // Where a perched flake sits now. False once the snow under it is gone
    // (knocked off or melted) or the props changed.
    bool PerchPoint(const Perch& perch, float& x, float& y) const;
    // Props step aside where a window covers them: `covered(x, y)` says whether
    // one covers that scene point. Call now and then (it is not free), and
    // before drawing whenever CoverStale(): props that have not been probed yet
    // take its answer at once rather than fading.
    void ProbeCover(const std::function<bool(float, float)>& covered);
    bool CoverStale() const { return coverStale_; }
    // What covers them changed all at once (the overlay moved behind or in
    // front of windows): probe everything afresh.
    void RecheckCover();

    // ---- Drawing ----------------------------------------------------------
    // Builds the sprite page on `dc`'s device if needed. False: nothing to draw.
    bool Prepare(ID2D1DeviceContext* dc);
    ID2D1Bitmap1* Page() const { return page_.Get(); }
    // Trees and snowmen, back to front (drawn before the drift, which buries their feet).
    void EmitProps(std::vector<ScenerySprite>& out) const;
    // Over the drift: snow heaped round their feet, their shade, and light
    // from the lowest bulbs pooling on the snow.
    void EmitGround(std::vector<ScenerySprite>& out) const;
    // The lights' glow and the stars, drawn over everything else.
    void EmitGlow(std::vector<ScenerySprite>& out) const;
    void ReleaseDeviceResources();

    struct Cell;
    struct Bulb;
    struct Tier;
    struct Group;
    struct Tree;
    struct Snowman;
    struct Sprite {
        D2D1_RECT_U src{};
        float cx = 0.0f, cy = 0.0f;  // page px where the art's anchor sits
    };

private:
    struct Counts {
        int groups = 0, men = 0;  // tree groups and snowmen (none when the screen is too small for props)
        bool operator==(const Counts&) const = default;
    };
    struct Span {
        float left, right;  // DIP: a tree group's extent along the ground
    };
    Counts CountsFor(int treeAmount, int snowmanAmount) const;
    uint32_t Seed() const;
    void AmountsChanged();
    void Relayout(bool all);
    void LayoutTrees(int groups);
    void LayoutSnowmen(int men);
    float Margin() const;
    void BuildTree(Tree& t, float x, float heightPx, int mode, float phase, int lights, Rng& rng) const;
    void BuildSnowman(Snowman& m, float x, float heightPx, int dir, Rng& rng) const;
    float CapPx(const Tree& tree) const;
    float CapPx(const Snowman& man) const;
    float LoadFraction(const Tree& tree, const Tier& tier, int side) const;
    void Deposit(Tier& tier, float capPx, bool skirt, float lx, float area);
    void Shed(const Tree& tree, Tier& tier, const Cell& cell, float area, float vx, float vy,
              std::vector<SnowClump>& clumps);
    void ShedHat(Snowman& m, const Cell& cell, float area, float vx, float vy, std::vector<SnowClump>& clumps);
    void UpdateTreeSnow(Tree& tree, Tier& tier, float dt, std::vector<SnowClump>& clumps);
    void UpdateHatSnow(Snowman& man, float dt, std::vector<SnowClump>& clumps);
    D2D1_POINT_2F Map(const Tree& tree, const Tier& tier, float lx, float ly) const;
    D2D1_POINT_2F HatPoint(const Snowman& man, float hx, float hy) const;
    D2D1_POINT_2F BodyPoint(const Snowman& man, float bx, float by) const;
    float Shear(const Tree& tree, const Tier& tier) const;
    D2D1_POINT_2F Apex(const Tree& tree, const Tier& tier) const;
    float Lean(const Tree& tree, float heightPx) const;
    float Alpha(bool snowman, float vis) const;
    float SurfaceY(const Tree& tree, float dx) const;
    void LightLevels(const Tree& tree, std::vector<float>& out) const;
    HRESULT BuildPage(ID2D1DeviceContext* dc);

    SceneryLayout layout_;
    bool configured_ = false;
    int treeAmount_ = kUsualAmount, snowmanAmount_ = kUsualAmount;
    Counts built_;              // what the props were laid out for
    float settle_ = 0.25f;      // SetSettle
    double relayoutAt_ = -1.0;  // when to lay out for new amounts (-1: nothing waiting)
    std::vector<Group> groups_;
    std::vector<Tree> trees_;
    std::vector<Snowman> snowmen_;
    std::vector<Span> spans_;   // where the groups stand, left to right
    unsigned genTrees_ = 1, genMen_ = 1;  // bumped whenever a kind is rebuilt or cleared (perches lapse)
    unsigned layoutGen_ = 1;    // bumped whenever the props are rebuilt (the page must follow)
    bool coverStale_ = true;    // some props have not been probed for cover yet
    bool showTrees_ = true, showSnowmen_ = true;
    float fadeTrees_ = 0.0f, fadeSnowmen_ = 0.0f;  // 0 hidden .. 1 shown
    float snowDepth_ = 0.0f;    // SetSnowDepth
    float glowAlpha_ = 1.0f;    // share of the desktop a glow veils (below 1: it adds light)
    float clearRate_ = 0.0f;    // px/s while snow melts away
    float clearLeft_ = 0.0f;    // s
    float top_ = 0.0f;
    float gust_ = 0.0f;         // the wind now, -1..1 of a strong gust
    double time_ = 0.0;
    float powerOn_ = 0.0f;      // s since the trees appeared: the lights come on one by one
    bool calm_ = false;         // Windows animation effects are off: steady lights
    float calmCheck_ = 0.0f;    // s until calm_ is looked up again
    double nextSparkle_ = 0.0;  // when a star may sparkle next (one at a time per screen)
    int sparkleTree_ = -1;
    double sparkleAt_ = -100.0;
    Rng rng_;

    // Device resources.
    ComPtr<ID2D1Bitmap1> page_;
    unsigned pageGen_ = 0;      // layoutGen_ the page was built for
    double pageRetry_ = 0.0;    // earliest time to retry a failed build
    // Shared cells: snow blobs, bulbs and glows at a few resolutions (largest
    // first, halving), and the star's sparkle.
    static constexpr int kLevels = 4;
    Sprite blob_[kLevels], bulb_[kLevels], glow_[kLevels], rays_;
    float blobPx_[kLevels] = {}, bulbPx_[kLevels] = {}, glowPx_[kLevels] = {}, raysPx_ = 0.0f;
    mutable std::vector<float> levels_;  // scratch: light levels while emitting
};
