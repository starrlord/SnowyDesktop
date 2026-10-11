// The scenery's sprite page: tree tiers, trunks and stars, snowmen and their
// hats and scarves, and the shared snow blobs, bulbs, glows and sparkle.
#include "SceneryParts.h"

using namespace scenery;

namespace {

using Pts = std::vector<D2D1_POINT_2F>;

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

// Gradient stop collections, one per distinct list of stops: a page draws
// hundreds of gradients from a few dozen lists, and a new collection costs far
// more to draw with than a new brush on one already used.
class Stops {
public:
    explicit Stops(ID2D1RenderTarget* rt) : rt_(rt) {}
    ID2D1RenderTarget* Target() const { return rt_; }
    ID2D1GradientStopCollection* Get(std::initializer_list<D2D1_GRADIENT_STOP> list) {
        for (const Entry& e : entries_)
            if (std::equal(e.stops.begin(), e.stops.end(), list.begin(), list.end(), Same)) return e.coll.Get();
        Entry& e = entries_.emplace_back();
        e.stops.assign(list.begin(), list.end());
        rt_->CreateGradientStopCollection(list.begin(), static_cast<UINT32>(list.size()), &e.coll);
        return e.coll.Get();
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

ComPtr<ID2D1Brush> Radial(Stops& stops, D2D1_POINT_2F c, D2D1_POINT_2F offset, float rx, float ry,
                          std::initializer_list<D2D1_GRADIENT_STOP> list) {
    ComPtr<ID2D1RadialGradientBrush> brush;
    if (ID2D1GradientStopCollection* coll = stops.Get(list))
        stops.Target()->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(c, offset, rx, ry), coll, &brush);
    return brush;
}

// A flank from the apex out to its tip at (w, h); sign -1 mirrors it to the left.
void Flank(Pts& pts, float h, float w, float sign, bool outward) {
    const int n = 20;
    Pts f;
    for (int i = 0; i <= n; ++i) {
        // Denser near the apex, where the flank bends most.
        const float u = 1.0f - std::pow(1.0f - static_cast<float>(i) / static_cast<float>(n), 1.5f);
        f.push_back({sign * u * w, EdgeY(h, w, u * w)});
    }
    if (outward) pts.insert(pts.end(), f.begin(), f.end());
    else pts.insert(pts.end(), f.rbegin(), f.rend());
}

// A tier's outline (tier-local px), shifted by (dx, dy): flank, a rounded tip,
// the hem's bough tips and arches, a rounded tip, the other flank.
Pts TierOutline(const Scenery::Tier& tr, float dx, float dy) {
    Pts pts;
    Flank(pts, tr.h, tr.w, 1.0f, true);
    auto cap = [&](float sign) {
        // From the flank's end down to where the hem starts, bulging outwards.
        const float y0 = tr.h, y1 = HemY(tr.h, tr.w, tr.arch, tr.boughs, sign * tr.w, tr.tip);
        const float r = 0.5f * (y1 - y0);
        for (int i = 1; i < 4; ++i) {
            const float a = kPi * static_cast<float>(i) / 4.0f;
            pts.push_back({sign * (tr.w + 0.6f * r * std::sin(a)), y0 + r * (1.0f - std::cos(a))});
        }
    };
    cap(1.0f);
    const int m = static_cast<int>(tr.boughs.size());
    const int per = 12;
    for (int i = 0; i <= m * per; ++i) {
        const float px = tr.w - 2.0f * tr.w * static_cast<float>(i) / static_cast<float>(m * per);
        pts.push_back({px, HemY(tr.h, tr.w, tr.arch, tr.boughs, px, tr.tip)});
    }
    // (cap(-1) runs bottom to top on the left: mirror the points of a right-hand cap.)
    {
        const float y0 = tr.h, y1 = HemY(tr.h, tr.w, tr.arch, tr.boughs, -tr.w, tr.tip);
        const float r = 0.5f * (y1 - y0);
        for (int i = 3; i >= 1; --i) {
            const float a = kPi * static_cast<float>(i) / 4.0f;
            pts.push_back({-(tr.w + 0.6f * r * std::sin(a)), y0 + r * (1.0f - std::cos(a))});
        }
    }
    Flank(pts, tr.h, tr.w, -1.0f, false);
    pts.pop_back();  // the apex again
    for (auto& p : pts) p = {p.x + dx, p.y + dy};
    return pts;
}

// A slightly lumpy, slightly squat ball of packed snow (centre c, radius r).
Pts Snowball(D2D1_POINT_2F c, float r, Rng& rng) {
    Pts pts;
    const int n = 44;
    const float p1 = rng.Range(0.0f, kTau), p2 = rng.Range(0.0f, kTau), p3 = rng.Range(0.0f, kTau);
    for (int i = 0; i < n; ++i) {
        const float a = kTau * static_cast<float>(i) / static_cast<float>(n);
        const float k = 1.0f + 0.018f * std::sin(3.0f * a + p1) + 0.012f * std::sin(5.0f * a + p2) +
                        0.008f * std::sin(9.0f * a + p3);
        pts.push_back({c.x + 1.04f * r * k * std::cos(a), c.y + 0.97f * r * k * std::sin(a)});
    }
    return pts;
}

}  // namespace

HRESULT Scenery::BuildPage(ID2D1DeviceContext* dc) {
    const float s = layout_.dpiScale;
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

    // ---- Plan the cells ------------------------------------------------------
    struct Box {
        Sprite* sprite;
        int w, h;      // incl. padding
        float ax, ay;  // anchor inside the box
        int kind, prop, part, level;
    };
    enum { kBlob, kBulb, kGlow, kRays, kTrunk, kStar, kTier, kBody, kHat, kTail };
    std::vector<Box> boxes;
    auto box = [&](Sprite* sp, int kind, int prop, int part, int level, float left, float top, float right, float bottom) {
        // Extents around the anchor (px), padded; the anchor lands on a whole pixel.
        const int l = static_cast<int>(std::ceil(left)) + kPad, t = static_cast<int>(std::ceil(top)) + kPad;
        const int r = static_cast<int>(std::ceil(right)) + kPad, b = static_cast<int>(std::ceil(bottom)) + kPad;
        boxes.push_back({sp, l + r, t + b, static_cast<float>(l), static_cast<float>(t), kind, prop, part, level});
    };
    float maxStar = 0.0f;
    for (const Tree& t : trees_) maxStar = std::max(maxStar, t.starR);
    for (int l = 0; l < kLevels; ++l) {
        const float k = std::exp2(-static_cast<float>(l));
        blobPx_[l] = 48.0f * k;
        bulbPx_[l] = 32.0f * k;
        glowPx_[l] = 128.0f * k;
        box(&blob_[l], kBlob, 0, 0, l, 0.5f * blobPx_[l], 0.5f * blobPx_[l], 0.5f * blobPx_[l], 0.5f * blobPx_[l]);
        box(&bulb_[l], kBulb, 0, 0, l, 0.5f * bulbPx_[l], 0.5f * bulbPx_[l], 0.5f * bulbPx_[l], 0.5f * bulbPx_[l]);
        box(&glow_[l], kGlow, 0, 0, l, 0.5f * glowPx_[l], 0.5f * glowPx_[l], 0.5f * glowPx_[l], 0.5f * glowPx_[l]);
    }
    raysPx_ = std::max(16.0f, std::ceil(3.0f * maxStar));
    box(&rays_, kRays, 0, 0, 0, 0.5f * raysPx_, 0.5f * raysPx_, 0.5f * raysPx_, 0.5f * raysPx_);
    const float back = std::ceil(1.6f * s) + 1.0f;  // room for the soft shadow behind a tier
    for (size_t ti = 0; ti < trees_.size(); ++ti) {
        Tree& t = trees_[ti];
        const int prop = static_cast<int>(ti);
        // Trunk: anchored on the base (even width: the base stays on a pixel); it runs
        // a little below it (into the snow) and up under the bottom tier.
        const float half = std::ceil(0.5f * t.trunkW) + 1.0f;
        box(&t.trunk, kTrunk, prop, 0, 0, half, t.trunkTop, half, 6.0f * s);
        box(&t.star, kStar, prop, 0, 0, 1.15f * t.starR, 1.15f * t.starR, 1.15f * t.starR, 1.15f * t.starR);
        for (size_t k = 0; k < t.tiers.size(); ++k) {
            Tier& tier = t.tiers[k];
            const float hem = HemY(tier.h, tier.w, tier.arch, tier.boughs, tier.w, tier.tip);
            // (The rounded tips and their edge reach a little past w on both sides.)
            box(&tier.art, kTier, prop, static_cast<int>(k), 0, tier.w + 2.0f + back, 2.0f, tier.w + 2.0f + back,
                std::max(hem, tier.h * (1.0f + kTipHang)) + 2.0f + back);
        }
    }
    for (size_t mi = 0; mi < snowmen_.size(); ++mi) {
        Snowman& m = snowmen_[mi];
        const int prop = static_cast<int>(mi);
        const float r1 = m.r[0], r2 = m.r[1], r3 = m.r[2];
        const float span = std::max(1.1f * r1, 2.35f * r2) + 2.0f + back;  // (the twig fingers reach a little further)
        box(&m.body, kBody, prop, 0, 0, span, m.c[2] + r3 + 2.0f, span, 0.25f * r1 + back);
        box(&m.hatArt, kHat, prop, 0, 0, 1.05f * r3 + 2.0f, 1.45f * r3 + 2.0f, 1.05f * r3 + 2.0f, 0.3f * r3 + 2.0f);
        box(&m.tail, kTail, prop, 0, 0, 0.5f * r3 + 2.0f, 0.2f * r3 + 2.0f, 0.5f * r3 + 2.0f, 1.9f * r3 + 2.0f);
    }

    // Shelf-pack, tallest first.
    std::vector<size_t> order(boxes.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return boxes[a].h > boxes[b].h; });
    int widest = 0;
    for (const Box& b : boxes) widest = std::max(widest, b.w);
    const UINT32 pageW = widest > 1024 ? 2048u : 1024u;
    UINT32 x = 0, y = 0, row = 0;
    for (size_t i : order) {
        Box& b = boxes[i];
        if (x + static_cast<UINT32>(b.w) > pageW) {
            x = 0;
            y += row;
            row = 0;
        }
        b.sprite->src = {x, y, x + static_cast<UINT32>(b.w), y + static_cast<UINT32>(b.h)};
        b.sprite->cx = static_cast<float>(x) + b.ax;
        b.sprite->cy = static_cast<float>(y) + b.ay;
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
    auto fill = [&](ID2D1Geometry* g, Rgb c, float a) {
        brush->SetColor(Color(c, a));
        rdc->FillGeometry(g, brush.Get());
    };
    auto stroke = [&](ID2D1Geometry* g, Rgb c, float a, float width) {
        brush->SetColor(Color(c, a));
        rdc->DrawGeometry(g, brush.Get(), width, round.Get());
    };
    auto line = [&](D2D1_POINT_2F a, D2D1_POINT_2F b, Rgb c, float alpha, float width) {
        brush->SetColor(Color(c, alpha));
        rdc->DrawLine(a, b, brush.Get(), width, round.Get());
    };
    auto dot = [&](D2D1_POINT_2F c, float r, Rgb col, float a) {
        brush->SetColor(Color(col, a));
        rdc->FillEllipse(D2D1::Ellipse(c, r, r), brush.Get());
    };
    // A shape filled shifted by (dx, dy): the soft shadows reuse the shapes that cast them.
    auto fillShifted = [&](ID2D1Geometry* g, float dx, float dy, Rgb c, float a) {
        D2D1::Matrix3x2F was;
        rdc->GetTransform(&was);
        rdc->SetTransform(D2D1::Matrix3x2F::Translation(dx, dy) * was);
        fill(g, c, a);
        rdc->SetTransform(was);
    };
    auto inside = [&](ID2D1Geometry* g) { rdc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), g), nullptr); };
    const float hair = std::max(1.0f, 0.8f * s);  // finest line that still reads (at least a pixel)
    Stops stops(rdc.Get());
    // Every tier's outline (tier-local), made once: its cell uses it, and the
    // tier below for the shadow it casts.
    std::vector<std::vector<ComPtr<ID2D1PathGeometry>>> outlines(trees_.size());
    for (size_t ti = 0; ti < trees_.size(); ++ti)
        for (const Tier& tier : trees_[ti].tiers) outlines[ti].push_back(MakePath(factory.Get(), TierOutline(tier, 0.0f, 0.0f), true));

    // ---- Draw ------------------------------------------------------------------
    rdc->SetTarget(page.Get());
    rdc->BeginDraw();
    rdc->Clear(D2D1::ColorF(0, 0, 0, 0));
    for (const Box& b : boxes) {
        const D2D1_RECT_U& rc = b.sprite->src;
        rdc->SetTransform(D2D1::Matrix3x2F::Identity());
        const float cb = static_cast<float>(kClearBorder);
        rdc->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(rc.left) + cb, static_cast<float>(rc.top) + cb,
                                             static_cast<float>(rc.right) - cb, static_cast<float>(rc.bottom) - cb),
                                 D2D1_ANTIALIAS_MODE_ALIASED);
        rdc->SetTransform(D2D1::Matrix3x2F::Translation(b.sprite->cx, b.sprite->cy));
        switch (b.kind) {
        case kBlob: {
            // Snow: solid most of the way out, then soft, so neighbouring ovals merge.
            const float r = 0.5f * blobPx_[b.level];
            if (auto g = Radial(stops, {0, 0}, {0, 0}, r, r,
                                {{0.0f, D2D1::ColorF(1, 1, 1, 1)}, {0.7f, D2D1::ColorF(1, 1, 1, 1)},
                                 {0.88f, D2D1::ColorF(1, 1, 1, 0.6f)}, {1.0f, D2D1::ColorF(1, 1, 1, 0)}}))
                rdc->FillEllipse(D2D1::Ellipse({0, 0}, r, r), g.Get());
            break;
        }
        case kGlow: {
            // A soft round glow, (1 - r^2)^3; tints make it add light.
            const float r = 0.5f * glowPx_[b.level];
            if (auto g = Radial(stops, {0, 0}, {0, 0}, r, r,
                                {{0.0f, D2D1::ColorF(1, 1, 1, 1)}, {0.2f, D2D1::ColorF(1, 1, 1, 0.885f)},
                                 {0.4f, D2D1::ColorF(1, 1, 1, 0.593f)}, {0.6f, D2D1::ColorF(1, 1, 1, 0.262f)},
                                 {0.8f, D2D1::ColorF(1, 1, 1, 0.047f)}, {1.0f, D2D1::ColorF(1, 1, 1, 0)}}))
                rdc->FillEllipse(D2D1::Ellipse({0, 0}, r, r), g.Get());
            break;
        }
        case kBulb: {
            // A little glass bulb (a C7/C9 shape, a touch taller than wide) on its
            // socket: white so tints colour it, with a hot spot up left; the socket stays dark.
            const float px = bulbPx_[b.level];
            if (auto g = Radial(stops, {-0.12f * px, -0.04f * px}, {0, 0}, 0.48f * px, 0.52f * px,
                                {{0.0f, D2D1::ColorF(1, 1, 1, 1)}, {0.45f, D2D1::ColorF(0.95f, 0.95f, 0.95f, 1)},
                                 {1.0f, D2D1::ColorF(0.68f, 0.68f, 0.68f, 1)}}))
                rdc->FillEllipse(D2D1::Ellipse({0.0f, 0.08f * px}, 0.37f * px, 0.425f * px), g.Get());
            brush->SetColor(Color(kWire, 0.95f));
            rdc->FillRectangle(D2D1::RectF(-0.16f * px, -0.48f * px, 0.16f * px, -0.28f * px), brush.Get());
            break;
        }
        case kRays: {
            // The star's sparkle: four rays, fading out (drawn to add light).
            const float r = 0.5f * raysPx_;
            Pts pts;
            for (int i = 0; i < 4; ++i) {
                const float a = -0.5f * kPi + static_cast<float>(i) * 0.5f * kPi;
                const float waist = std::max(0.12f * r, 1.5f);
                pts.push_back({r * std::cos(a), r * std::sin(a)});
                pts.push_back({waist * std::cos(a + 0.25f * kPi), waist * std::sin(a + 0.25f * kPi)});
            }
            if (auto geo = MakePath(factory.Get(), pts, true))
                if (auto g = Radial(stops, {0, 0}, {0, 0}, r, r,
                                    {{0.0f, D2D1::ColorF(1, 1, 1, 1)}, {0.4f, D2D1::ColorF(1, 1, 1, 0.7f)},
                                     {1.0f, D2D1::ColorF(1, 1, 1, 0)}}))
                    rdc->FillGeometry(geo.Get(), g.Get());
            break;
        }
        case kTrunk: {
            const Tree& t = trees_[static_cast<size_t>(b.prop)];
            const float hw = 0.5f * t.trunkW;
            const float top = -t.trunkTop, bottom = 6.0f * s;
            const D2D1_RECT_F r = {-hw, top, hw, bottom};
            if (auto g = Linear(stops, {-hw, 0}, {hw, 0},
                                {{0.0f, Color(kBarkLit, 1)}, {0.45f, Color(kBarkMid, 1)}, {1.0f, Color(kBarkShadow, 1)}}))
                rdc->FillRectangle(r, g.Get());
            // In the shade of the boughs at the top.
            if (auto g = Linear(stops, {0, top}, {0, top + 0.45f * t.trunkTop},
                                {{0.0f, Color(kShadow, 0.5f)}, {1.0f, Color(kShadow, 0)}}))
                rdc->FillRectangle(r, g.Get());
            break;
        }
        case kStar: {
            // Five plump points: a soft bright core, golden body, deeper edge to the lower right.
            const Tree& t = trees_[static_cast<size_t>(b.prop)];
            const float R = t.starR;
            Pts pts;
            for (int i = 0; i < 10; ++i) {
                const float a = -0.5f * kPi + static_cast<float>(i) * 0.2f * kPi;
                const float r = i % 2 ? 0.48f * R : R;
                pts.push_back({r * std::cos(a), r * std::sin(a)});
            }
            if (auto geo = MakePath(factory.Get(), pts, true)) {
                if (auto g = Radial(stops, {-0.12f * R, -0.18f * R}, {0, 0}, 1.25f * R, 1.25f * R,
                                    {{0.0f, Color(kStarCore, 1)}, {0.3f, Color(kStarBody, 1)}, {1.0f, Color(kStarEdge, 1)}}))
                    rdc->FillGeometry(geo.Get(), g.Get());
                stroke(geo.Get(), Mix(kStarEdge, kShadow, 0.45f), 0.8f, std::max(0.6f * hair, 0.06f * R));
            }
            break;
        }
        case kTier: {
            const Tree& t = trees_[static_cast<size_t>(b.prop)];
            const Tier& tier = t.tiers[static_cast<size_t>(b.part)];
            const Tier* above = b.part + 1 < static_cast<int>(t.tiers.size()) ? &t.tiers[static_cast<size_t>(b.part) + 1] : nullptr;
            const float h = tier.h, w = tier.w;
            const std::vector<ComPtr<ID2D1PathGeometry>>& shapes = outlines[static_cast<size_t>(b.prop)];
            ID2D1PathGeometry* geo = shapes[static_cast<size_t>(b.part)].Get();
            if (!geo) break;
            // A soft shadow behind it, down and to the right, keeps it apart from
            // mid-toned wallpapers.
            for (int i = 0; i < 3; ++i) {
                const float o = (0.7f + 0.35f * static_cast<float>(i)) * s;
                fillShifted(geo, o, o, kShadow, 0.07f);
            }
            // Needles: lit from the upper left, darkening to the lower right
            // (t = 0.175 + 0.275 x/w + 0.55 y/h runs lit -> mid -> dark).
            {
                const D2D1_POINT_2F d = {0.275f / w, 0.55f / h};
                const float d2 = d.x * d.x + d.y * d.y;
                const float l0 = -0.175f / d2, l1 = 0.825f / d2;
                if (auto g = Linear(stops, {l0 * d.x, l0 * d.y}, {l1 * d.x, l1 * d.y},
                                    {{0.0f, Color(kNeedleLit, 1)}, {0.5f, Color(kNeedleMid, 1)}, {1.0f, Color(kNeedleDark, 1)}}))
                    rdc->FillGeometry(geo, g.Get());
            }
            inside(geo);
            const D2D1_RECT_F all = {-w - 4.0f, -4.0f, w + 4.0f, 1.5f * h};
            // A darker band under the boughs along the hem.
            if (auto g = Linear(stops, {0, 0.72f * h}, {0, h}, {{0.0f, Color(kNeedleShadow, 0.0f)}, {1.0f, Color(kNeedleShadow, 0.4f)}}))
                rdc->FillRectangle(all, g.Get());
            // Needle tufts: little downward chevrons on the face, brightest on the lit side.
            {
                Rng tuft(0x7A5Fu + static_cast<uint64_t>(b.prop) * 131u + static_cast<uint64_t>(b.part) * 17u);
                const int rows = h > 20.0f * s ? 2 : 1;
                const float arm = std::max(1.2f * s, 0.045f * w), drop = 0.85f * arm;
                for (int r = 0; r < rows; ++r) {
                    const float yRow = (rows == 2 ? (r == 0 ? 0.55f : 0.8f) : 0.72f) * h;
                    const float half = 0.82f * EdgeX(h, w, yRow);
                    const float stepX = std::max(4.5f * s, 2.6f * arm);
                    for (float xt = -half + 0.5f * stepX * tuft.Range(0.6f, 1.4f); xt < half; xt += stepX * tuft.Range(0.85f, 1.2f)) {
                        const float yt = yRow + tuft.Range(-0.04f, 0.04f) * h;
                        const float lit = 1.0f - Saturate((xt / w + 0.2f) * 0.8f);
                        const D2D1_POINT_2F c = {xt, yt};
                        line({c.x - arm, c.y - drop}, c, kNeedleLit, 0.18f + 0.22f * lit, std::max(0.9f, 0.55f * s));
                        line({c.x + arm, c.y - drop}, c, kNeedleLit, 0.18f + 0.22f * lit, std::max(0.9f, 0.55f * s));
                        line({c.x - arm, c.y - drop + s}, {c.x, c.y + s}, kNeedleShadow, 0.2f, std::max(0.9f, 0.5f * s));
                        line({c.x + arm, c.y - drop + s}, {c.x, c.y + s}, kNeedleShadow, 0.2f, std::max(0.9f, 0.5f * s));
                    }
                }
            }
            // The shadow of the tier above: its outline nudged down and right, softened.
            if (ID2D1PathGeometry* up = above ? shapes[static_cast<size_t>(b.part) + 1].Get() : nullptr) {
                const float drop = above->apexY - tier.apexY;
                const float dx = 0.6f * s, dy = (1.8f + 0.02f * t.H / s) * s;
                for (int i = 0; i < 3; ++i) {
                    const float k = static_cast<float>(i) - 1.0f;  // -1, 0, 1: a little blur
                    fillShifted(up, dx + 0.4f * k * s, dy - drop + 0.6f * k * s, kShadow, 0.16f);
                }
            }
            // The string of lights: a dark wire looping across the face.
            if (tier.bulbs.size() >= 2) {
                Pts wire;
                for (const Bulb& bulb : tier.bulbs) wire.push_back({bulb.lx, bulb.ly - 0.45f * bulb.size});
                // Out a little past the end bulbs, round the tree.
                const D2D1_POINT_2F a = wire.front(), z = wire.back();
                wire.insert(wire.begin(), {a.x + (a.x > z.x ? 0.12f : -0.12f) * w, a.y - 0.04f * h});
                wire.push_back({z.x + (z.x > a.x ? 0.12f : -0.12f) * w, z.y - 0.04f * h});
                if (auto g = MakePath(factory.Get(), wire, false)) stroke(g.Get(), kWire, 0.5f, std::max(1.0f, 0.5f * s));
            }
            // Moonlight along the upper-left flank, all the way to its tip.
            Pts rim;
            Flank(rim, h, w, -1.0f, true);
            if (auto g = MakePath(factory.Get(), rim, false)) {
                if (auto br = Linear(stops, {0, 0}, {-w, h}, {{0.0f, Color(kRim, 0.75f)}, {1.0f, Color(kRim, 0.35f)}}))
                    rdc->DrawGeometry(g.Get(), br.Get(), 2.0f * hair, round.Get());
            }
            rdc->PopLayer();
            // A dark edge, strongest to the lower right, keeps the shape on any wallpaper.
            stroke(geo, kNeedleShadow, 0.6f, hair);
            Pts dark;
            Flank(dark, h, w, 1.0f, true);
            if (auto g = MakePath(factory.Get(), dark, false)) stroke(g.Get(), kNeedleShadow, 0.3f, hair);
            break;
        }
        case kBody: {
            const Snowman& m = snowmen_[static_cast<size_t>(b.prop)];
            Rng rng(0x5EED5u + static_cast<uint64_t>(b.prop) * 7919u);
            const float r1 = m.r[0], r2 = m.r[1], r3 = m.r[2];
            const float dir = static_cast<float>(m.dir);
            const D2D1_POINT_2F centre[3] = {{0, -m.c[0]}, {0, -m.c[1]}, {0, -m.c[2]}};
            ComPtr<ID2D1PathGeometry> ballGeo[3];
            for (int i = 0; i < 3; ++i) ballGeo[i] = MakePath(factory.Get(), Snowball(centre[i], m.r[i], rng), true);
            if (!ballGeo[0] || !ballGeo[1] || !ballGeo[2]) break;
            // Soft shadow behind, down and to the right.
            for (int i = 0; i < 3; ++i)
                for (int k = 0; k < 3; ++k) {
                    const float o = (0.7f + 0.35f * static_cast<float>(k)) * s;
                    fillShifted(ballGeo[i].Get(), o, o, kShadow, 0.05f);
                }
            // Twig arms, behind the middle ball: out from its sides, bending, with twiggy fingers.
            auto twig = [&](float side, bool raised) {
                const D2D1_POINT_2F root = {side * 0.78f * r2, centre[1].y - 0.12f * r2};
                const float up = raised ? -1.0f : 0.35f;
                const D2D1_POINT_2F elbow = {root.x + side * 0.72f * r2, root.y + up * 0.42f * r2};
                const D2D1_POINT_2F hand = {elbow.x + side * 0.62f * r2, elbow.y + (raised ? -0.62f : 0.18f) * r2};
                const float wRoot = std::max(1.3f, 0.085f * r2), wTip = std::max(1.0f, 0.055f * r2);
                const Rgb bark = Hex(0x5A3A22), barkLit = Hex(0x8A6040);
                line(root, elbow, bark, 1.0f, wRoot);
                line(elbow, hand, bark, 1.0f, wTip);
                // Fingers, and a side shoot on the forearm.
                const float ax = hand.x - elbow.x, ay = hand.y - elbow.y;
                const float len = std::sqrt(ax * ax + ay * ay) + 1e-3f;
                const float ux = ax / len, uy = ay / len;
                for (float turn : {-0.6f, 0.15f, 0.75f}) {
                    const float c = std::cos(turn), sn = std::sin(turn);
                    const D2D1_POINT_2F f = {hand.x + (ux * c - uy * sn) * 0.32f * r2, hand.y + (ux * sn + uy * c) * 0.32f * r2};
                    line(hand, f, bark, 1.0f, std::max(0.9f, 0.7f * wTip));
                }
                const D2D1_POINT_2F mid = {Lerp(elbow.x, hand.x, 0.45f), Lerp(elbow.y, hand.y, 0.45f)};
                line(mid, {mid.x + side * 0.12f * r2, mid.y - 0.28f * r2}, bark, 1.0f, std::max(0.9f, 0.7f * wTip));
                line({root.x, root.y - 0.4f * wRoot}, {elbow.x, elbow.y - 0.4f * wRoot}, barkLit, 0.5f, std::max(0.8f, 0.4f * wRoot));
            };
            twig(-1.0f, m.arms == 2 || (m.arms == 1 && dir < 0.0f));
            twig(1.0f, m.arms == 2 || (m.arms == 1 && dir > 0.0f));
            // The balls, bottom to top: lit from the upper left, cool blue in shade, a
            // bright rim on the lit edge, snow-light bouncing into the shaded edge, and a
            // soft shadow under each ball on the one below.
            for (int i = 0; i < 3; ++i) {
                const float r = m.r[i];
                const D2D1_POINT_2F c = centre[i];
                if (auto g = Radial(stops, {c.x - 0.35f * r, c.y - 0.42f * r}, {0, 0}, 1.45f * r, 1.45f * r,
                                    {{0.0f, Color(kWhite, 1)}, {0.5f, Color(Hex(0xF3F7FE), 1)},
                                     {0.8f, Color(Hex(0xD8E4F8), 1)}, {1.0f, Color(Hex(0xB8CDEE), 1)}}))
                    rdc->FillGeometry(ballGeo[i].Get(), g.Get());
                inside(ballGeo[i].Get());
                if (i < 2) {
                    for (int k = 0; k < 3; ++k) {
                        const float o = (0.04f * m.r[i + 1]) + static_cast<float>(k) * 0.8f * s;
                        fillShifted(ballGeo[i + 1].Get(), 0.3f * o, o, Hex(0x8FA9D6), 0.13f);
                    }
                }
                // A little packed-snow texture: faint dimples in the shade, glints in the light.
                const int specks = static_cast<int>(6.0f + 0.25f * r / s);
                for (int k = 0; k < specks; ++k) {
                    const float a = rng.Range(-0.6f, 2.2f), d = r * std::sqrt(rng.Float()) * 0.85f;
                    dot({c.x + d * std::cos(a), c.y + d * std::sin(a)}, std::max(0.45f, 0.35f * s), Hex(0xC9D8F1), 0.45f);
                }
                for (int k = 0; k < 3; ++k) {
                    const float a = rng.Range(3.4f, 4.6f), d = r * rng.Range(0.3f, 0.75f);
                    dot({c.x + d * std::cos(a), c.y + d * std::sin(a)}, std::max(0.45f, 0.3f * s), kWhite, 0.9f);
                }
                if (auto br = Linear(stops, {c.x - r, c.y - r}, {c.x + 0.2f * r, c.y + 0.2f * r},
                                     {{0.0f, Color(kWhite, 0.9f)}, {1.0f, Color(kWhite, 0.0f)}}))
                    rdc->DrawGeometry(ballGeo[i].Get(), br.Get(), 2.4f * hair, round.Get());
                if (auto br = Linear(stops, {c.x + r, c.y + r}, {c.x, c.y},
                                     {{0.0f, Color(Hex(0xE6EEFB), 0.5f)}, {1.0f, Color(Hex(0xE6EEFB), 0.0f)}}))
                    rdc->DrawGeometry(ballGeo[i].Get(), br.Get(), 2.0f * hair, round.Get());
                rdc->PopLayer();
                stroke(ballGeo[i].Get(), Hex(0x6B83AE), 0.45f, std::max(1.0f, 0.75f * hair));
            }
            // The hat's shade on the crown of the head.
            {
                const float hy = -m.hatY + 0.08f * r3;
                if (auto g = Radial(stops, {0.06f * r3, hy}, {0, 0}, 0.95f * r3, 0.3f * r3,
                                    {{0.0f, Color(Hex(0x8FA9D6), 0.45f)}, {1.0f, Color(Hex(0x8FA9D6), 0.0f)}}))
                    rdc->FillEllipse(D2D1::Ellipse({0.06f * r3, hy}, 0.95f * r3, 0.3f * r3), g.Get());
            }
            // Coal buttons down the front (a little to the side it faces).
            for (int k = 0; k < 3; ++k) {
                const D2D1_POINT_2F p = {dir * 0.13f * r2 * (1.0f - 0.2f * static_cast<float>(k == 1)),
                                         centre[1].y + (-0.38f + 0.36f * static_cast<float>(k)) * r2};
                const float br = std::max(0.9f * s * 0.9f, 0.075f * r2);
                dot(p, br, kCoal, 1.0f);
                dot({p.x - 0.3f * br, p.y - 0.3f * br}, 0.35f * br, kWhite, 0.55f);
            }
            {
                const D2D1_POINT_2F p = {dir * 0.1f * r1, centre[0].y - 0.35f * r1};
                const float br = std::max(0.9f * s * 0.9f, 0.07f * r1);
                dot(p, br, kCoal, 1.0f);
                dot({p.x - 0.3f * br, p.y - 0.3f * br}, 0.35f * br, kWhite, 0.5f);
            }
            // The face, turned a little to one side.
            {
                const D2D1_POINT_2F hc = centre[2];
                const float er = std::max(0.85f * s, 0.075f * r3);
                const float ex = dir * 0.12f * r3;
                for (int k = -1; k <= 1; k += 2) {
                    const float away = static_cast<float>(k) * dir < 0.0f ? 0.85f : 1.0f;  // the far eye looks a touch smaller
                    const D2D1_POINT_2F e = {hc.x + ex + static_cast<float>(k) * 0.21f * r3, hc.y - 0.1f * r3};
                    dot(e, er * away, kCoal, 1.0f);
                    dot({e.x - 0.3f * er, e.y - 0.32f * er}, 0.38f * er * away, kWhite, 0.85f);
                    if (r3 > 6.0f * s) dot({e.x - 0.02f * r3, e.y + 0.24f * r3}, 0.13f * r3, Hex(0xFFB3C1), 0.32f);  // rosy cheeks
                }
                // Carrot nose, pointing the way it faces.
                const D2D1_POINT_2F base = {hc.x + ex + dir * 0.06f * r3, hc.y + 0.06f * r3};
                const D2D1_POINT_2F tip = {base.x + dir * 0.72f * r3, base.y + 0.12f * r3};
                const float bt = std::max(0.9f * s, 0.11f * r3);
                Pts nose = {{base.x, base.y - bt}, tip, {base.x, base.y + bt}, {base.x - dir * 0.6f * bt, base.y}};
                if (auto g = MakePath(factory.Get(), nose, true)) {
                    if (auto br = Linear(stops, {base.x, base.y - bt}, {tip.x, tip.y + 0.5f * bt},
                                         {{0.0f, Color(Hex(0xFF9A2E), 1)}, {1.0f, Color(Hex(0xE2620F), 1)}}))
                        rdc->FillGeometry(g.Get(), br.Get());
                    stroke(g.Get(), Hex(0xA64A0C), 0.55f, std::max(0.8f, 0.5f * hair));
                    for (float f : {0.35f, 0.6f}) {
                        const D2D1_POINT_2F p = {Lerp(base.x, tip.x, f), Lerp(base.y, tip.y, f)};
                        const float half = bt * (1.0f - f);
                        line({p.x - dir * 0.05f * r3, p.y - half}, {p.x + dir * 0.02f * r3, p.y - 0.2f * half}, Hex(0xB5520E),
                             0.6f, std::max(0.7f, 0.4f * hair));
                    }
                }
                // A coal smile.
                for (int k = 0; k < 5; ++k) {
                    const float u = static_cast<float>(k) / 4.0f * 2.0f - 1.0f;
                    const D2D1_POINT_2F p = {hc.x + ex + u * 0.3f * r3, hc.y + 0.36f * r3 + 0.1f * r3 * (1.0f - u * u)};
                    dot(p, std::max(0.6f * s, (0.05f - 0.008f * std::fabs(u)) * r3), kCoal, 1.0f);
                }
            }
            // The scarf, wrapped round the neck and knotted at the side it faces.
            {
                const Knit& knit = kKnits[m.knit];
                const float ny = -(m.c[1] + 0.74f * r2);
                const float wN = 1.08f * r3, hN = 0.42f * r3;
                Pts band;
                const int n = 16;
                for (int i = 0; i <= n; ++i) {
                    const float u = static_cast<float>(i) / static_cast<float>(n) * 2.0f - 1.0f;
                    band.push_back({u * wN, ny - 0.5f * hN + 0.14f * r3 * u * u});
                }
                for (int i = n; i >= 0; --i) {
                    const float u = static_cast<float>(i) / static_cast<float>(n) * 2.0f - 1.0f;
                    band.push_back({u * wN * 1.02f, ny + 0.5f * hN + 0.16f * r3 * u * u});
                }
                if (auto g = MakePath(factory.Get(), band, true)) {
                    if (auto br = Linear(stops, {0, ny - 0.5f * hN}, {0, ny + 0.6f * hN},
                                         {{0.0f, Color(Mix(knit.main, kWhite, 0.15f), 1)}, {1.0f, Color(Mix(knit.main, kShadow, 0.3f), 1)}}))
                        rdc->FillGeometry(g.Get(), br.Get());
                    inside(g.Get());
                    // Knitted stripes across it.
                    for (float u : {-0.55f, -0.1f, 0.35f, 0.8f}) {
                        brush->SetColor(Color(knit.stripe, 0.9f));
                        rdc->FillRectangle(D2D1::RectF(u * wN, ny - hN, u * wN + 0.12f * r3, ny + hN), brush.Get());
                    }
                    // Shade to the right, where it turns away.
                    if (auto br = Linear(stops, {-wN, 0}, {wN, 0}, {{0.0f, Color(kShadow, 0.0f)}, {1.0f, Color(kShadow, 0.35f)}}))
                        rdc->FillRectangle(D2D1::RectF(-wN - 2, ny - hN, wN + 2, ny + hN), br.Get());
                    rdc->PopLayer();
                    stroke(g.Get(), Mix(knit.main, kShadow, 0.55f), 0.6f, std::max(0.9f, 0.6f * hair));
                }
                const D2D1_POINT_2F knot = {m.knotX, -m.knotY};
                if (auto br = Radial(stops, {knot.x - 0.06f * r3, knot.y - 0.06f * r3}, {0, 0}, 0.26f * r3, 0.22f * r3,
                                     {{0.0f, Color(Mix(knit.main, kWhite, 0.2f), 1)}, {1.0f, Color(Mix(knit.main, kShadow, 0.35f), 1)}}))
                    rdc->FillEllipse(D2D1::Ellipse(knot, 0.22f * r3, 0.19f * r3), br.Get());
            }
            break;
        }
        case kHat: {
            const Snowman& m = snowmen_[static_cast<size_t>(b.prop)];
            const float r3 = m.r[2];
            const Knit& knit = kKnits[m.knit];
            if (m.hat == 0) {
                // A top hat: a slightly flared crown with a sheen, a coloured band, and a brim.
                const Rgb felt = Hex(0x23252E);
                const D2D1_RECT_F brim = {-0.95f * r3, -0.22f * r3, 0.95f * r3, 0.1f * r3};
                Pts crown = {{-0.56f * r3, -0.08f * r3}, {-0.6f * r3, -1.02f * r3}, {-0.56f * r3, -1.1f * r3},
                             {0.56f * r3, -1.1f * r3},  {0.6f * r3, -1.02f * r3},  {0.56f * r3, -0.08f * r3}};
                auto crownGeo = MakePath(factory.Get(), crown, true);
                // Brim first (the crown stands on it).
                if (auto br = Linear(stops, {-0.95f * r3, 0}, {0.95f * r3, 0},
                                     {{0.0f, Color(Hex(0x3A3D49), 1)}, {0.45f, Color(felt, 1)}, {1.0f, Color(Hex(0x101116), 1)}}))
                    rdc->FillEllipse(D2D1::Ellipse({0.0f, 0.5f * (brim.top + brim.bottom)}, 0.95f * r3, 0.16f * r3), br.Get());
                if (crownGeo) {
                    if (auto br = Linear(stops, {-0.6f * r3, 0}, {0.6f * r3, 0},
                                         {{0.0f, Color(Hex(0x40434F), 1)}, {0.35f, Color(felt, 1)}, {1.0f, Color(Hex(0x0F1015), 1)}}))
                        rdc->FillGeometry(crownGeo.Get(), br.Get());
                    inside(crownGeo.Get());
                    brush->SetColor(Color(kWhite, 0.1f));
                    rdc->FillRectangle(D2D1::RectF(-0.4f * r3, -1.2f * r3, -0.27f * r3, 0.0f), brush.Get());
                    // The band.
                    const Rgb band = m.knit == 3 ? Hex(0xB8262E) : knit.main;
                    brush->SetColor(Color(band, 1.0f));
                    rdc->FillRectangle(D2D1::RectF(-0.7f * r3, -0.36f * r3, 0.7f * r3, -0.1f * r3), brush.Get());
                    brush->SetColor(Color(kWhite, 0.25f));
                    rdc->FillRectangle(D2D1::RectF(-0.7f * r3, -0.36f * r3, 0.7f * r3, -0.33f * r3), brush.Get());
                    if (auto br = Linear(stops, {-0.6f * r3, 0}, {0.6f * r3, 0}, {{0.0f, Color(kShadow, 0.0f)}, {1.0f, Color(kShadow, 0.35f)}}))
                        rdc->FillRectangle(D2D1::RectF(-0.7f * r3, -0.36f * r3, 0.7f * r3, -0.1f * r3), br.Get());
                    rdc->PopLayer();
                    stroke(crownGeo.Get(), Hex(0x07080B), 0.6f, std::max(0.8f, 0.6f * hair));
                }
                // The crown's top, catching the light.
                brush->SetColor(Color(Hex(0x34363F), 1.0f));
                rdc->FillEllipse(D2D1::Ellipse({0.0f, -1.08f * r3}, 0.56f * r3, 0.08f * r3), brush.Get());
                // A sprig of holly on the band now and then.
                if (m.knit % 2 == 0) {
                    const float hx = static_cast<float>(m.dir) * 0.38f * r3, hy = -0.24f * r3;
                    for (float a : {-0.5f, 0.5f}) {
                        const float c = std::cos(a + (m.dir > 0 ? 0.0f : kPi)), sn = std::sin(a + (m.dir > 0 ? 0.0f : kPi));
                        Pts leaf;
                        for (int i = 0; i <= 8; ++i) {
                            const float u = static_cast<float>(i) / 8.0f;
                            const float wv = 0.1f * r3 * std::sin(kPi * u) * (1.0f + 0.25f * std::sin(kTau * 2.0f * u));
                            leaf.push_back({hx + c * u * 0.3f * r3 - sn * wv, hy + sn * u * 0.3f * r3 + c * wv});
                        }
                        for (int i = 8; i >= 0; --i) {
                            const float u = static_cast<float>(i) / 8.0f;
                            const float wv = -0.1f * r3 * std::sin(kPi * u) * (1.0f + 0.25f * std::sin(kTau * 2.0f * u));
                            leaf.push_back({hx + c * u * 0.3f * r3 - sn * wv, hy + sn * u * 0.3f * r3 + c * wv});
                        }
                        if (auto g = MakePath(factory.Get(), leaf, true)) fill(g.Get(), Hex(0x2E7D3A), 1.0f);
                    }
                    for (int k = 0; k < 3; ++k)
                        dot({hx + (static_cast<float>(k) - 1.0f) * 0.07f * r3, hy - 0.04f * r3 + (k == 1 ? -0.05f * r3 : 0.0f)},
                            std::max(0.8f, 0.065f * r3), Hex(0xD7262E), 1.0f);
                }
            } else {
                // A bobble hat: a knitted dome with a stripe and ribs, a turned-up cuff, a fluffy bobble.
                Pts dome;
                const int n = 24;
                for (int i = 0; i <= n; ++i) {
                    const float a = kPi * static_cast<float>(i) / static_cast<float>(n);
                    dome.push_back({-0.98f * r3 * std::cos(a), -0.95f * r3 * std::sin(a)});
                }
                auto domeGeo = MakePath(factory.Get(), dome, true);
                if (domeGeo) {
                    if (auto br = Radial(stops, {-0.35f * r3, -0.6f * r3}, {0, 0}, 1.4f * r3, 1.3f * r3,
                                         {{0.0f, Color(Mix(knit.main, kWhite, 0.2f), 1)}, {1.0f, Color(Mix(knit.main, kShadow, 0.35f), 1)}}))
                        rdc->FillGeometry(domeGeo.Get(), br.Get());
                    inside(domeGeo.Get());
                    brush->SetColor(Color(knit.stripe, 0.95f));
                    rdc->FillRectangle(D2D1::RectF(-r3, -0.62f * r3, r3, -0.46f * r3), brush.Get());
                    for (int k = -4; k <= 4; ++k) {
                        const float xk = static_cast<float>(k) * 0.2f * r3;
                        line({xk * 0.6f, -0.95f * r3}, {xk, 0.0f}, kShadow, 0.12f, std::max(0.8f, 0.5f * hair));
                    }
                    rdc->PopLayer();
                    stroke(domeGeo.Get(), Mix(knit.main, kShadow, 0.6f), 0.55f, std::max(0.8f, 0.6f * hair));
                }
                // Cuff.
                const D2D1_ROUNDED_RECT cuff = D2D1::RoundedRect(D2D1::RectF(-1.02f * r3, -0.3f * r3, 1.02f * r3, 0.08f * r3),
                                                                 0.12f * r3, 0.12f * r3);
                if (auto br = Linear(stops, {0, -0.3f * r3}, {0, 0.08f * r3},
                                     {{0.0f, Color(Mix(knit.stripe, kWhite, 0.1f), 1)}, {1.0f, Color(Mix(knit.stripe, kShadow, 0.25f), 1)}}))
                    rdc->FillRoundedRectangle(cuff, br.Get());
                for (int k = -7; k <= 7; ++k)
                    line({static_cast<float>(k) * 0.13f * r3, -0.27f * r3}, {static_cast<float>(k) * 0.13f * r3, 0.05f * r3},
                         kShadow, 0.12f, std::max(0.8f, 0.5f * hair));
                brush->SetColor(Color(Mix(knit.stripe, kShadow, 0.5f), 0.5f));
                rdc->DrawRoundedRectangle(cuff, brush.Get(), std::max(0.8f, 0.6f * hair));
                // The bobble: a fluffy ball.
                Rng fluff(0xB0B1Eu + static_cast<uint64_t>(b.prop));
                Pts bob;
                const D2D1_POINT_2F bc = {0.0f, -1.08f * r3};
                for (int i = 0; i < 28; ++i) {
                    const float a = kTau * static_cast<float>(i) / 28.0f;
                    const float rr = 0.3f * r3 * (i % 2 ? fluff.Range(0.86f, 0.95f) : fluff.Range(1.02f, 1.12f));
                    bob.push_back({bc.x + rr * std::cos(a), bc.y + rr * std::sin(a)});
                }
                if (auto g = MakePath(factory.Get(), bob, true)) {
                    if (auto br = Radial(stops, {bc.x - 0.1f * r3, bc.y - 0.1f * r3}, {0, 0}, 0.38f * r3, 0.38f * r3,
                                         {{0.0f, Color(kWhite, 1)}, {0.7f, Color(knit.stripe, 1)}, {1.0f, Color(Mix(knit.stripe, kShadow, 0.3f), 1)}}))
                        rdc->FillGeometry(g.Get(), br.Get());
                    stroke(g.Get(), Mix(knit.stripe, kShadow, 0.5f), 0.4f, std::max(0.7f, 0.5f * hair));
                }
            }
            break;
        }
        case kTail: {
            // The scarf's loose end, hanging from the knot: stripes across, a fringe.
            const Snowman& m = snowmen_[static_cast<size_t>(b.prop)];
            const float r3 = m.r[2];
            const Knit& knit = kKnits[m.knit];
            const float len = 1.5f * r3, half = 0.17f * r3;
            const float lean = 0.1f * r3 * static_cast<float>(m.dir);
            Pts tail;
            const int n = 10;
            for (int i = 0; i <= n; ++i) {
                const float u = static_cast<float>(i) / static_cast<float>(n);
                tail.push_back({-half + lean * u * u + 0.04f * r3 * std::sin(kPi * u), u * len});
            }
            for (int i = n; i >= 0; --i) {
                const float u = static_cast<float>(i) / static_cast<float>(n);
                tail.push_back({half * (1.0f - 0.1f * u) + lean * u * u + 0.04f * r3 * std::sin(kPi * u), u * len});
            }
            if (auto g = MakePath(factory.Get(), tail, true)) {
                if (auto br = Linear(stops, {-half, 0}, {half, 0},
                                     {{0.0f, Color(Mix(knit.main, kWhite, 0.15f), 1)}, {1.0f, Color(Mix(knit.main, kShadow, 0.3f), 1)}}))
                    rdc->FillGeometry(g.Get(), br.Get());
                inside(g.Get());
                for (float u : {0.35f, 0.62f}) {
                    brush->SetColor(Color(knit.stripe, 0.92f));
                    rdc->FillRectangle(D2D1::RectF(-r3, u * len, r3, u * len + 0.13f * r3), brush.Get());
                }
                rdc->PopLayer();
                stroke(g.Get(), Mix(knit.main, kShadow, 0.55f), 0.55f, std::max(0.8f, 0.6f * hair));
            }
            for (int k = 0; k < 4; ++k) {
                const float xk = Lerp(-0.8f * half, 0.75f * half, static_cast<float>(k) / 3.0f) + lean;
                line({xk, len}, {xk + 0.02f * r3, len + 0.22f * r3}, knit.main, 0.95f, std::max(0.9f, 0.06f * r3));
            }
            break;
        }
        default: break;
        }
        rdc->SetTransform(D2D1::Matrix3x2F::Identity());
        rdc->PopAxisAlignedClip();
    }
    hr = rdc->EndDraw();
    rdc->SetTarget(nullptr);
    if (FAILED(hr)) return hr;

    // Strips for each tier: about 6 DIP wide, one boundary on the axis.
    for (Tree& t : trees_) {
        for (Tier& tier : t.tiers) {
            tier.strips.clear();
            const D2D1_RECT_U& rc = tier.art.src;
            const UINT32 axis = static_cast<UINT32>(std::lround(tier.art.cx));
            const UINT32 halfL = axis - rc.left, halfR = rc.right - axis;
            const int n = Clamp(static_cast<int>(std::ceil(static_cast<float>(halfL) / (6.0f * s))), 2, 8);
            for (int i = 0; i < n; ++i) tier.strips.push_back(rc.left + halfL * static_cast<UINT32>(i) / static_cast<UINT32>(n));
            for (int i = 0; i <= n; ++i) tier.strips.push_back(axis + halfR * static_cast<UINT32>(i) / static_cast<UINT32>(n));
        }
    }
    page_ = std::move(page);
    return S_OK;
}
