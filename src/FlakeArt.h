// Flake artwork: procedural vector designs for every FlakeKind, drawn with
// Direct2D, plus a multi-resolution sprite atlas used for fast batched drawing.
#pragma once

#include "Common.h"
#include "Settings.h"

#include <d2d1_3.h>
#include <wincodec.h>

// Random variations generated per procedural kind (classic kinds have one).
constexpr int kMaxVariants = 4;

struct FlakeArtOptions {
    bool shadow = true;  // faint dark halo so white flakes stay visible on light windows
};

// Static per-kind traits the simulation uses.
struct FlakeTraits {
    float minDip, maxDip;  // diameter range in DIPs at size setting 50
    bool rotates;          // spins while falling
    bool twinkles;         // alpha/scale shimmer (sparkle)
    bool pixelArt;         // classic bitmaps: sample crisp
};
const FlakeTraits& GetFlakeTraits(FlakeKind kind);

int FlakeVariantCount(FlakeKind kind);

// Loads a classic pixel-art flake PNG (IDR_CLASSIC1 + index) from the exe resources
// as a 32bpp premultiplied BGRA WIC bitmap. index is 0..7.
HRESULT LoadClassicFlake(IWICImagingFactory* wic, int index, IWICBitmapSource** out);

// Draws one flake design in white, centred at (cx, cy), fitting a circle of
// radius r. Works on any render target (device context, WIC or DC target).
// `classic` must be the classic bitmap (created on `rt`) for classic kinds and
// may be null otherwise.
void DrawFlakeDesign(ID2D1RenderTarget* rt, FlakeKind kind, int variant, float cx, float cy, float r,
                     const FlakeArtOptions& opt, ID2D1Bitmap* classic);

// All flake sprites, pre-rendered at several resolutions so that drawing any
// on-screen size samples a cell at most 2x larger than the target (no shimmer).
class FlakeAtlas {
public:
    struct Sprite {
        ID2D1Bitmap1* bitmap = nullptr;  // atlas page
        D2D1_RECT_U src{};               // cell rectangle in the page
        float extent = 1.0f;             // cell size / flake diameter (cells include halo padding)
    };

    // (Re)creates the atlas on the device that owns `dc`.
    HRESULT Build(ID2D1DeviceContext* dc, IWICImagingFactory* wic, const FlakeArtOptions& opt);
    void Reset();
    bool Ready() const { return !levels_.empty(); }

    static int SpriteIndex(FlakeKind kind, int variant);
    static int SpriteCount();

    // Best source cell for drawing sprite `index` at `diameterPx` on screen.
    const Sprite& Get(int index, float diameterPx) const;

private:
    struct Level {
        std::vector<Sprite> sprites;  // by sprite index
    };
    std::vector<ComPtr<ID2D1Bitmap1>> pages_;  // every level shares these pages
    std::vector<Level> levels_;                // largest first; diameters shrink by sqrt(2) per level
    std::vector<float> diameter_;              // drawn flake diameter (px) per level: procedural sprites
    std::vector<float> classicDiameter_;       // ... and classic sprites (whole-pixel scales)
};
