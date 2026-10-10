// Snow drift that accumulates on a ledge (the top edge of the taskbar).
//
// The pile is a 1D heightfield with one column per physical pixel. Landed
// flakes deposit volume, steep slopes avalanche (angle of repose), wind
// scours exposed crests and drops snow in the lee, and a slow melt keeps it
// from filling up evenly. The surface is shaded on the CPU into a
// premultiplied BGRA strip that the renderer draws over the taskbar edge.
#pragma once

#include "Common.h"

struct DriftParams {
    float maxHeight = 0.0f;  // px; 0 disables accumulation
    float dpiScale = 1.0f;   // px per DIP
    bool leftOpen = true;    // snow can slide off the left end
    bool rightOpen = true;   // snow can slide off the right end
};

class Drift {
public:
    // Sizes the drift. Keeps existing snow when only params change.
    void Configure(int widthPx, const DriftParams& params);
    void SetParams(const DriftParams& params);
    const DriftParams& Params() const { return params_; }

    int Width() const { return width_; }
    bool Enabled() const { return params_.maxHeight > 0.0f; }

    // Smoothed surface height above the ground line at x (px), x clamped.
    float HeightAt(float x) const;

    // A flake of the given on-screen diameter landed at x. `amount` scales
    // the deposited volume (1 = one flake's worth).
    void Deposit(float x, float diameterPx, float amount = 1.0f);

    // Cuts snow above `ceiling` (px above ground) in columns [x0, x1).
    // Returns the removed area in px^2 so the caller can spawn spray.
    float Plow(float x0, float x1, float ceiling);

    // Advances settling, avalanching, wind transport and melt.
    // wind is the current wind speed in px/s (positive = blowing right).
    void Update(float dt, float wind);

    void Clear();                 // remove everything now
    void MeltAway(float seconds); // animated melt to zero
    bool Empty() const;

    // ---- Rendering --------------------------------------------------------
    // The strip covers every column; row GroundRow() is the first row at or
    // below the ground line. Rows above it hold the pile, rows below it hold
    // the overhanging lip and its soft shadow on the taskbar.
    int TexWidth() const { return width_; }
    int TexHeight() const { return texHeight_; }
    int GroundRow() const { return groundRow_; }

    // Re-shades changed columns. Returns false when nothing changed; when it
    // returns true, [x0, x1) is the column range that must be re-uploaded.
    bool Shade(int& x0, int& x1);
    const uint32_t* Pixels() const { return pixels_.data(); }

    // A random point on the visible surface for sparkle glints:
    // returns false if there is no snow. y is height above ground (px).
    bool RandomSurfacePoint(Rng& rng, float& x, float& y) const;

private:
    // Implementation detail; see Drift.cpp.
    friend struct DriftTestAccess;  // offline test harness only

    static constexpr int kShadeBlock = 32;  // dirty-tracking granularity for Shade (px)
    static constexpr int kMeltBlock = 64;   // melt is applied one block at a time (px)

    void Resize(int widthPx);
    void MarkDirty(int x0, int x1);
    void MarkAllDirty();
    // Heights changed in [x0, x1). `steep`: the change may leave slopes past repose.
    void Touch(int x0, int x1, bool steep = true);
    void ComputeStaticFields();
    void RefreshFields(int x0, int x1);
    void MapFields(int x0, int x1);
    void FieldStatistics();
    void NormalizeCatch();
    float EndTaper(float xDip, float widthDip) const;
    void UpdateLayout();
    float LocalCap(int i) const { return capHeight_ * capF_[static_cast<size_t>(i)]; }
    void AddBump(float cx, float halfWidth, float volume);
    float LeeFactor(int i) const;
    void Relax(int sweeps);
    void Creep(float dt);
    void WindStep(float dt, float windPx);
    void MeltBlocks(float dt);
    void Derive();
    float WideAverage(int i, int radius) const;
    void ShadeRange(int x0, int x1);

    DriftParams params_;
    int width_ = 0;
    int texHeight_ = 0;
    int groundRow_ = 0;
    std::vector<float> height_;   // physical heights (px)
    std::vector<float> smooth_;   // smoothed heights used for collision and shading
    std::vector<float> scratch_;  // temporaries: bump weights, creep fluxes
    std::vector<float> work_;     // temporaries: creep curvature
    std::vector<uint32_t> pixels_;

    // Per-column fields from smooth noise (DIP space, so the look is resolution independent).
    std::vector<float> talus_;     // max stable slope (tan of the angle of repose)
    std::vector<float> crestInv_;  // 1 / sharpest crest curvature that creep leaves alone (px)
    std::vector<float> capF_;      // local cap as a fraction of the cap height (incl. end taper)
    std::vector<float> catch_;     // how much snow a stretch gathers
    std::vector<float> lipF_;      // lip droop shape, 0..1
    std::vector<float> rawCatch_, rawSpot_, rawCap_;  // raw noise behind catch_ and capF_
    float fieldStats_[6] = {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f};  // mean, spread of each raw field
    float catchScale_ = 1.0f;      // makes catch_ average 1 over the strip
    std::vector<double> prefix_;   // prefix sums of smooth_ for O(1) neighbourhood averages
    std::vector<float> shadedS_;   // smooth_ as of the last shade of each column
    std::vector<float> colS_, colLip_, colWide_;  // per-column shading inputs (with a 1-px apron)
    std::vector<uint8_t> blockDirty_;
    std::vector<float> meltAge_;   // seconds since each melt block was last melted

    int dirtyBlocks_ = 0;
    bool fullDirty_ = true;
    int shadeCursor_ = 0;          // next block Shade looks at (round robin)
    int unshadedUpdates_ = 0;      // Update calls since the last Shade
    int chg0_ = 0, chg1_ = 0;      // heights changed in [chg0_, chg1_) since the last Derive
    int av0_ = 0, av1_ = 0;        // columns that may be steeper than repose
    int meltCursor_ = 0;
    float meltDebt_ = 0.0f;
    float sweepDebt_ = 0.0f;
    float windDebt_ = 0.0f;
    unsigned sweepParity_ = 0;
    int fieldCursor_ = 0;
    float capHeight_ = 0.0f;       // effective cap (px); eases down when maxHeight shrinks
    float maxH_ = 0.0f;            // tallest column (px)
    float windDip_ = 0.0f;         // last wind, DIP/s
    float meltRate_ = 0.0f;        // px/s while melting away
    float meltLeft_ = 0.0f;        // seconds of melt-away remaining
    float seed_ = 0.0f;            // noise-field offset, differs per drift
    bool seeded_ = false;
    bool fieldsValid_ = false;
    double time_ = 0.0;
    Rng rng_{0xD21F7u};
};
