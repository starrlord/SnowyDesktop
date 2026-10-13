// Lights internals shared by the engine (Lights.cpp: geometry, motion,
// patterns) and the look (LightsArt.cpp: the sprite page and drawing): each
// bulb type's proportions, and the frame the engine fills for the look to draw.
#pragma once

#include "Lights.h"

namespace lights {

// ---------------------------------------------------------------------------
// Bulb types. Lengths are DIPs in real proportion to each other (a C7 is
// about as long as a taskbar icon is tall); SizeScale sets the overall size.
// ---------------------------------------------------------------------------

struct BulbSpec {
    float length;       // from the socket's top (where it clips onto the wire) to the tip of the glass
    float width;        // widest part of the glass (or the base, for a bubble light)
    float spacing;      // along the wire, bulb to bulb
    float wire;         // wire thickness
    float glassCenter;  // from the socket's top along the axis to where the light comes from (glows centre there)
    float hang;         // 0: sticks out square to the wire .. 1: hangs straight down
    float tauOn;        // s: how quickly the light comes on (a filament warming up; an LED is near instant)
    float tauOff;       // s: ... and goes off
    bool led;           // keeps its colour as it dims (an incandescent dims towards orange)
};

inline const BulbSpec& Spec(BulbType t) {
    static const BulbSpec kSpecs[kBulbTypeCount] = {
        //  length width spacing wire  centre hang  on     off    led
        {12.0f, 4.2f, 24.0f, 1.5f, 7.5f, 0.4f, 0.05f, 0.08f, false},    // Mini
        {11.0f, 4.8f, 24.0f, 1.5f, 7.0f, 0.4f, 0.015f, 0.02f, true},    // MiniLed
        {8.0f, 6.5f, 24.0f, 1.5f, 5.0f, 0.45f, 0.015f, 0.02f, true},    // WideLed
        {13.0f, 7.0f, 30.0f, 1.8f, 8.0f, 0.45f, 0.06f, 0.10f, false},   // C5
        {13.0f, 7.5f, 30.0f, 1.8f, 8.0f, 0.45f, 0.015f, 0.02f, true},   // C6
        {19.0f, 10.0f, 40.0f, 2.0f, 12.0f, 0.5f, 0.08f, 0.14f, false},  // C7
        {25.0f, 13.0f, 52.0f, 2.3f, 16.0f, 0.55f, 0.10f, 0.18f, false}, // C9
        {19.0f, 14.0f, 46.0f, 2.0f, 12.0f, 0.7f, 0.10f, 0.18f, false},  // Globe
        {32.0f, 11.0f, 52.0f, 2.2f, 10.0f, 0.92f, 0.12f, 0.20f, false}, // Bubble
        {20.0f, 8.5f, 44.0f, 2.0f, 13.0f, 0.6f, 0.05f, 0.09f, false},   // Candle
    };
    const int i = Clamp(static_cast<int>(t), 0, kBulbTypeCount - 1);
    return kSpecs[i];
}

// The Size setting as a multiplier on the proportions above. At the middle
// setting strands read as festive across a whole desktop, a little larger than
// life beside the taskbar's icons.
inline float SizeScale(int size) { return 1.3f * std::exp2(static_cast<float>(size - 50) / 50.0f); }

// ---------------------------------------------------------------------------
// The frame: everything the look needs to draw one moment.
// ---------------------------------------------------------------------------

struct BulbDraw {
    float x = 0.0f, y = 0.0f;  // px: where the socket meets the wire
    float angle = 0.0f;        // rad: the bulb's axis, clockwise from hanging straight down
    float scale = 1.0f;        // px per DIP of its BulbSpec (DPI x Size x a little per-bulb variety)
    int color = 0;             // place in the palette (any non-negative number; the look wraps it)
    float level = 0.0f;        // 0..1: light it gives now
    float heat = 0.0f;         // 0..1: filament temperature (incandescents warm as it rises; LEDs: = level)
    float alpha = 1.0f;        // 0..1: shown (a window over it, lights fading in or out)
    float anim = 0.0f;         // candle: the flame's flicker (-1..1); bubble light: s its bubbles have run; else 0
    uint32_t seed = 0;         // stable per bulb (small variations in the art)
};

struct WireDraw {
    std::vector<D2D1_POINT_2F> pts;  // px, along the wire
    std::vector<float> alpha;        // per point, 0..1
    float width = 2.0f;              // px
    int strand = 0;                  // which strand (hanging mode highlights the hovered one)
};

struct HookDraw {
    float x = 0.0f, y = 0.0f;  // px
    int strand = 0, hook = 0;
    bool hovered = false;      // under the pointer
    bool end = false;          // the last hook of the strand being placed
    int snap = 0;              // edges it is snapped to (as LightsEdit::snap)
};

struct Frame {
    std::vector<WireDraw> wires;
    std::vector<BulbDraw> bulbs;  // in drawing order
    // Hanging mode.
    bool editing = false;
    float veil = 0.0f;            // 0..1: how far the screen is dimmed
    std::vector<HookDraw> hooks;
    bool cursorValid = false;     // where the next hook would go
    D2D1_POINT_2F cursor{};
    int cursorSnap = 0;
    bool placing = false;
    int hoverStrand = -1;
};

// ---------------------------------------------------------------------------
// The seams between the halves.
// ---------------------------------------------------------------------------

// Engine (Lights.cpp).
const Frame& FrameOf(const Lights::Engine& engine);

// Look (LightsArt.cpp).
Lights::Look* NewLook();
// The settings and scene size it draws for (the page follows the bulb type,
// Size, wire colour and DPI).
void LookSetParams(Lights::Look& look, const LightParams& params, const LightsLayout& layout);

}  // namespace lights
