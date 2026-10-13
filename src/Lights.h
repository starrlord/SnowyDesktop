// Strings of Christmas lights hung across the screen: draped across the top on
// their own, or on hooks the user placed. Each span between two hooks hangs
// under gravity with the slack it was given, sways in the wind and when the
// pointer brushes it; the bulbs dangle from the wire, a pattern runs along the
// strand, and their glow adds light to the desktop.
//
// The scene owns one Lights. Two halves sit behind this class: the engine
// (Lights.cpp: geometry, motion, patterns) fills a frame each update, and the
// look (LightsArt.cpp: the sprite page and drawing) turns it into sprites for
// the scene's batch. LightsParts.h is the contract between them.
#pragma once

#include "Common.h"
#include "Scenery.h"  // ScenerySprite
#include "Settings.h"

#include <d2d1_3.h>
#include <functional>
#include <memory>

struct LightsLayout {
    int width = 0, height = 0;  // scene px
    float dpiScale = 1.0f;      // px per DIP

    bool operator==(const LightsLayout&) const = default;
};

struct LightParams {
    bool show = false;
    BulbType bulb = BulbType::C7;
    LightColors colors = LightColors::Multicolor;
    COLORREF customColor = RGB(255, 190, 110);
    LightAnimation animation = LightAnimation::Twinkle;
    int speed = 50, size = 50, glow = 60;  // 0..100
    WireColor wire = WireColor::Green;
    bool acrossTop = true;               // the automatic drape, else `strands`
    std::vector<LightStrand> strands;    // this scene's own (hooks as fractions of the scene)
    bool mouse = true;                   // the pointer may brush the strands

    bool operator==(const LightParams&) const = default;
};

// Hanging mode as one monitor draws it: the editor's strands for this monitor
// (they replace the params' while active), the strand being placed and where
// its next hook would go, and what the pointer is over.
struct LightsEdit {
    bool active = false;
    std::vector<LightStrand> strands;
    int placing = -1;                    // strand whose last hook has a live span to `cursor` (-1: none)
    float placingSlack = kDefaultSlack;
    bool cursorValid = false;            // the pointer is on this monitor
    LightHook cursor;                    // where the next hook would go (snapped), fractions
    int hoverStrand = -1, hoverHook = -1;  // under the pointer (hoverHook -1: the wire, or nothing)
    int snap = 0;                        // edges the cursor snapped to: 1 left, 2 top, 4 right, 8 bottom

    bool operator==(const LightsEdit&) const = default;
};

// The pointer, in scene px and px/s.
struct LightsPointer {
    bool valid = false;
    float x = 0.0f, y = 0.0f, vx = 0.0f, vy = 0.0f;
};

class Lights {
public:
    Lights();
    ~Lights();
    Lights(const Lights&) = delete;
    Lights& operator=(const Lights&) = delete;

    // ---- Engine (Lights.cpp) ---------------------------------------------
    void Configure(const LightsLayout& layout);
    void SetParams(const LightParams& params);
    void SetEdit(const LightsEdit& edit);
    // Anything to update or draw.
    bool Active() const;
    bool Editing() const;
    // Motion, patterns and fades. `windPx` is the wind now (px/s, positive
    // blows right); `pointer` brushes the strands.
    void Update(float dt, double time, float windPx, const LightsPointer& pointer);
    // Bulbs step aside where a window covers them, like the trees: same
    // contract as Scenery::ProbeCover / CoverStale / RecheckCover.
    void ProbeCover(const std::function<bool(float, float)>& covered);
    bool CoverStale() const;
    void RecheckCover();

    // The automatic drape across the top of a `width` x `height` px monitor at
    // `dpiScale`, as strands (hanging mode starts from it).
    static std::vector<LightStrand> AcrossTop(const std::wstring& monitor, int width, int height, float dpiScale);
    // The rest shape of one span from `a` to `b` (px) with `slack`, as the
    // engine hangs it: points from a to b inclusive, appended to `out`. For
    // hit-testing a strand in hanging mode. `screenWidth` (px) tells which way
    // is into the screen for a vertical run (0: a guess from the left edge).
    static void SpanShape(D2D1_POINT_2F a, D2D1_POINT_2F b, float slack, float dpiScale,
                          std::vector<D2D1_POINT_2F>& out, float screenWidth = 0.0f);

    // ---- Look (LightsArt.cpp) ----------------------------------------------
    // Whether glows may add light (premultiplied colour above alpha); where
    // Windows composes in floating point they are drawn as soft colour instead.
    void SetAdditiveGlow(bool on);
    // Builds the sprite page on `dc`'s device if needed. False: nothing to draw.
    bool Prepare(ID2D1DeviceContext* dc);
    ID2D1Bitmap1* Page() const;
    // Hanging mode: the veil dimming the screen (drawn first).
    void EmitVeil(std::vector<ScenerySprite>& out) const;
    // The wire, sockets and glass (drawn with the snow).
    void EmitBody(std::vector<ScenerySprite>& out) const;
    // The light they give, over everything.
    void EmitGlow(std::vector<ScenerySprite>& out) const;
    // Hanging mode: hooks and markers (drawn last).
    void EmitEdit(std::vector<ScenerySprite>& out) const;
    void ReleaseDeviceResources();

    // One bulb for the settings window's tiles, drawn with vector art on `rt`:
    // its socket at `anchor`, `length` px from the socket to the tip, hanging
    // straight down, in `color` (straight alpha), lit or not.
    static void DrawIcon(ID2D1RenderTarget* rt, BulbType type, D2D1_POINT_2F anchor, float length,
                         D2D1_COLOR_F color, bool lit);

    struct Engine;
    struct Look;
    struct LookDeleter {
        void operator()(Look* look) const;  // LightsArt.cpp
    };

private:
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<Look, LookDeleter> look_;
};
