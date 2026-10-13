// lightslab: renders a scene with Christmas lights offscreen to a PNG, for
// working on how the lights look and move (developer builds only).
//
//   lightslab --out lights.png [--bulb c7] [--anim twinkle] [--colors multi]
//             [--speed 50] [--size 50] [--glow 60] [--wire green]
//             [--w 1600] [--h 420] [--scale 1.5] [--t 4] [--frames 1] [--step 0.1]
//             [--bg dark|light|desk] [--bgimage file] [--snow 0] [--drift 0]
//             [--hooks "u,v;u,v;..."] [--slack 0.05] [--gallery]
//             [--hdr] [--edit] [--placing] [--cursor u,v] [--hover strand,hook]
//             [--brush "x0,y0,x1,y1,t0,t1"] [--wind 15] [--gusts 45] [--calm]
//             [--crop x,y,w,h] [--zoom 3]
//
// --frames N stacks N frames (--step s apart) top to bottom, so an animation
// can be read in one image. --gallery stacks one row per bulb type instead
// (each row --h px tall). --brush moves the pointer from (x0,y0) to (x1,y1) px
// between t0 and t1 s, to brush the strands. --hooks hangs one strand on those
// hooks (fractions of the scene) instead of the drape across the top.
#include "FlakeArt.h"
#include "Lights.h"
#include "Scene.h"
#include "Settings.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <objbase.h>
#include <wincodec.h>

#include <cstdio>
#include <cwchar>
#include <map>

namespace {

struct Options {
    std::wstring out;
    Settings s;
    int w = 1600, h = 420;
    float scale = 1.5f;
    double t = 4.0, step = 0.1;
    int frames = 1;
    std::wstring bg = L"dark", bgImage;
    int snow = 0, drift = 0;
    std::vector<LightHook> hooks;
    float slack = kDefaultSlack;
    bool gallery = false, hdr = false, edit = false, placing = false, calm = false;
    bool cursorValid = false;
    LightHook cursor;
    int hoverStrand = -1, hoverHook = -1;
    bool brush = false;
    float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0, bt0 = 0, bt1 = 0;
    bool crop = false;
    int cx = 0, cy = 0, cw = 0, ch = 0;
    int zoom = 1;
};

[[noreturn]] void Die(const wchar_t* what) {
    fwprintf(stderr, L"lightslab: %s\n", what);
    exit(1);
}

void Check(HRESULT hr, const wchar_t* what) {
    if (FAILED(hr)) {
        fwprintf(stderr, L"lightslab: %s failed (0x%08X)\n", what, static_cast<unsigned>(hr));
        exit(1);
    }
}

template <typename E>
E ParseEnum(const std::wstring& v, std::initializer_list<const wchar_t*> names) {
    int i = 0;
    for (const wchar_t* n : names) {
        if (_wcsicmp(v.c_str(), n) == 0) return static_cast<E>(i);
        ++i;
    }
    fwprintf(stderr, L"lightslab: unknown value '%s'\n", v.c_str());
    exit(1);
}

std::vector<float> Numbers(const std::wstring& v) {
    std::vector<float> out;
    const wchar_t* p = v.c_str();
    while (*p) {
        wchar_t* end = nullptr;
        const float f = wcstof(p, &end);
        if (end == p) {
            ++p;
            continue;
        }
        out.push_back(f);
        p = end;
    }
    return out;
}

Options Parse(int argc, wchar_t** argv) {
    Options o;
    o.s.amount = 0;
    o.s.trees = o.s.snowmen = false;
    o.s.mouse = true;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&]() -> std::wstring {
            if (i + 1 >= argc) Die((L"missing value after " + a).c_str());
            return argv[++i];
        };
        if (a == L"--out") o.out = next();
        else if (a == L"--bulb")
            o.s.bulb = ParseEnum<BulbType>(next(), {L"mini", L"miniled", L"wideled", L"c5", L"c6", L"c7", L"c9", L"globe",
                                                    L"bubble", L"candle"});
        else if (a == L"--anim")
            o.s.lightAnimation = ParseEnum<LightAnimation>(
                next(), {L"steady", L"twinkle", L"pulse", L"chase", L"alternate", L"waves", L"combination"});
        else if (a == L"--colors")
            o.s.lightColors =
                ParseEnum<LightColors>(next(), {L"multi", L"warm", L"cool", L"redgreen", L"bluewhite", L"custom"});
        else if (a == L"--custom") {
            const std::vector<float> c = Numbers(next());
            if (c.size() == 3) o.s.lightCustomColor = RGB(static_cast<int>(c[0]), static_cast<int>(c[1]), static_cast<int>(c[2]));
        } else if (a == L"--wire") o.s.wire = ParseEnum<WireColor>(next(), {L"green", L"white", L"black"});
        else if (a == L"--speed") o.s.lightSpeed = _wtoi(next().c_str());
        else if (a == L"--size") o.s.lightSize = _wtoi(next().c_str());
        else if (a == L"--glow") o.s.lightGlow = _wtoi(next().c_str());
        else if (a == L"--wind") o.s.wind = _wtoi(next().c_str());
        else if (a == L"--gusts") o.s.gusts = _wtoi(next().c_str());
        else if (a == L"--w") o.w = _wtoi(next().c_str());
        else if (a == L"--h") o.h = _wtoi(next().c_str());
        else if (a == L"--scale") o.scale = static_cast<float>(_wtof(next().c_str()));
        else if (a == L"--t") o.t = _wtof(next().c_str());
        else if (a == L"--frames") o.frames = _wtoi(next().c_str());
        else if (a == L"--step") o.step = _wtof(next().c_str());
        else if (a == L"--bg") o.bg = next();
        else if (a == L"--bgimage") o.bgImage = next();
        else if (a == L"--snow") o.snow = _wtoi(next().c_str());
        else if (a == L"--drift") o.drift = _wtoi(next().c_str());
        else if (a == L"--slack") o.slack = static_cast<float>(_wtof(next().c_str()));
        else if (a == L"--hooks") {
            const std::vector<float> n = Numbers(next());
            for (size_t k = 0; k + 1 < n.size(); k += 2) o.hooks.push_back({n[k], n[k + 1]});
        } else if (a == L"--gallery") o.gallery = true;
        else if (a == L"--hdr") o.hdr = true;
        else if (a == L"--edit") o.edit = true;
        else if (a == L"--placing") o.placing = true;
        else if (a == L"--calm") o.calm = true;
        else if (a == L"--cursor") {
            const std::vector<float> n = Numbers(next());
            if (n.size() == 2) {
                o.cursorValid = true;
                o.cursor = {n[0], n[1]};
            }
        } else if (a == L"--hover") {
            const std::vector<float> n = Numbers(next());
            if (n.size() == 2) {
                o.hoverStrand = static_cast<int>(n[0]);
                o.hoverHook = static_cast<int>(n[1]);
            }
        } else if (a == L"--brush") {
            const std::vector<float> n = Numbers(next());
            if (n.size() != 6) Die(L"--brush wants x0,y0,x1,y1,t0,t1");
            o.brush = true;
            o.bx0 = n[0], o.by0 = n[1], o.bx1 = n[2], o.by1 = n[3], o.bt0 = n[4], o.bt1 = n[5];
        } else if (a == L"--crop") {
            const std::vector<float> n = Numbers(next());
            if (n.size() != 4) Die(L"--crop wants x,y,w,h");
            o.crop = true;
            o.cx = static_cast<int>(n[0]), o.cy = static_cast<int>(n[1]), o.cw = static_cast<int>(n[2]),
            o.ch = static_cast<int>(n[3]);
        } else if (a == L"--zoom") o.zoom = std::max(1, _wtoi(next().c_str()));
        else Die((L"unknown option " + a).c_str());
    }
    if (o.out.empty()) Die(L"--out is required");
    if (o.w < 16 || o.h < 16 || o.w > 8192 || o.h > 8192) Die(L"bad size");
    o.frames = Clamp(o.frames, 1, 64);
    o.s.lights = true;
    if (!o.hooks.empty()) {
        if (o.hooks.size() < 2) Die(L"--hooks wants at least two hooks");
        LightStrand st;
        st.monitor = L"lab";
        st.hooks = o.hooks;
        st.slack.assign(o.hooks.size() - 1, o.slack);
        o.s.strands = {st};
        o.s.lightLayout = LightLayout::Custom;
    }
    o.s.Sanitize();
    return o;
}

struct Gpu {
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID2D1Device> d2d;
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<IWICImagingFactory> wic;
};

Gpu MakeGpu() {
    Gpu g;
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                                   &g.d3d, nullptr, nullptr);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &g.d3d,
                               nullptr, nullptr);
    Check(hr, L"D3D11CreateDevice");
    ComPtr<IDXGIDevice> dxgi;
    Check(g.d3d.As(&dxgi), L"IDXGIDevice");
    D2D1_CREATION_PROPERTIES props{};
    props.threadingMode = D2D1_THREADING_MODE_SINGLE_THREADED;
    Check(D2D1CreateDevice(dxgi.Get(), props, &g.d2d), L"D2D1CreateDevice");
    Check(g.d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g.dc), L"CreateDeviceContext");
    g.dc->SetDpi(96.0f, 96.0f);
    g.dc->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g.wic)),
          L"WIC factory");
    return g;
}

// An opaque BGRA image, top to bottom.
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
};

ComPtr<ID2D1Bitmap1> LoadBackground(Gpu& g, const std::wstring& path) {
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    Check(g.wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec),
          L"open --bgimage");
    Check(dec->GetFrame(0, &frame), L"decode --bgimage");
    Check(g.wic->CreateFormatConverter(&conv), L"converter");
    Check(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                           WICBitmapPaletteTypeCustom),
          L"convert --bgimage");
    ComPtr<ID2D1Bitmap1> bmp;
    Check(g.dc->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp), L"--bgimage bitmap");
    return bmp;
}

void DrawBackground(Gpu& g, const Options& o, int w, int h, ID2D1Bitmap1* image) {
    ID2D1DeviceContext* dc = g.dc.Get();
    const D2D1_RECT_F all = D2D1::RectF(0, 0, static_cast<float>(w), static_cast<float>(h));
    if (image) {
        const D2D1_SIZE_F s = image->GetSize();
        // Cover the area, cropping the image's sides or top and bottom.
        const float k = std::max(static_cast<float>(w) / s.width, static_cast<float>(h) / s.height);
        const float sw = static_cast<float>(w) / k, sh = static_cast<float>(h) / k;
        const D2D1_RECT_F src = {0.5f * (s.width - sw), 0.0f, 0.5f * (s.width + sw), sh};
        dc->DrawBitmap(image, all, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &src);
        return;
    }
    if (o.bg == L"light") {
        dc->Clear(D2D1::ColorF(0xF3F3F3));
        return;
    }
    uint32_t top = 0x07072A, bottom = 0x03031A;  // the references' deep navy
    if (o.bg == L"desk") top = 0x2A5C93, bottom = 0x0E2A4A;
    D2D1_GRADIENT_STOP stops[] = {{0.0f, D2D1::ColorF(top)}, {1.0f, D2D1::ColorF(bottom)}};
    ComPtr<ID2D1GradientStopCollection> coll;
    ComPtr<ID2D1LinearGradientBrush> brush;
    Check(dc->CreateGradientStopCollection(stops, 2, &coll), L"gradient");
    Check(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {0, static_cast<float>(h)}),
                                        coll.Get(), &brush),
          L"gradient brush");
    dc->FillRectangle(all, brush.Get());
}

Image Render(Gpu& g, const Options& o, Scene& scene, const FlakeAtlas& atlas, int w, int h, ID2D1Bitmap1* bgImage) {
    const D2D1_BITMAP_PROPERTIES1 target = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f,
        96.0f);
    ComPtr<ID2D1Bitmap1> bmp;
    Check(g.dc->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(h)), nullptr, 0, target, &bmp),
          L"target bitmap");
    g.dc->SetTarget(bmp.Get());
    g.dc->BeginDraw();
    g.dc->SetTransform(D2D1::Matrix3x2F::Identity());
    DrawBackground(g, o, w, h, bgImage);
    scene.Draw(g.dc.Get(), atlas);
    Check(g.dc->EndDraw(), L"EndDraw");
    g.dc->SetTarget(nullptr);

    const D2D1_BITMAP_PROPERTIES1 readable = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> cpu;
    Check(g.dc->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(h)), nullptr, 0, readable, &cpu),
          L"readback bitmap");
    Check(cpu->CopyFromBitmap(nullptr, bmp.Get(), nullptr), L"CopyFromBitmap");
    D2D1_MAPPED_RECT map{};
    Check(cpu->Map(D2D1_MAP_OPTIONS_READ, &map), L"Map");
    Image img;
    img.w = w;
    img.h = h;
    img.px.resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) memcpy(&img.px[static_cast<size_t>(y) * w * 4], map.bits + static_cast<size_t>(y) * map.pitch, static_cast<size_t>(w) * 4);
    cpu->Unmap();
    for (size_t i = 3; i < img.px.size(); i += 4) img.px[i] = 255;
    return img;
}

Image CropZoom(const Image& in, const Options& o) {
    if (!o.crop && o.zoom == 1) return in;
    int x0 = 0, y0 = 0, cw = in.w, ch = in.h;
    if (o.crop) {
        x0 = Clamp(o.cx, 0, in.w - 1);
        y0 = Clamp(o.cy, 0, in.h - 1);
        cw = Clamp(o.cw, 1, in.w - x0);
        ch = Clamp(o.ch, 1, in.h - y0);
    }
    Image out;
    out.w = cw * o.zoom;
    out.h = ch * o.zoom;
    out.px.resize(static_cast<size_t>(out.w) * out.h * 4);
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x)
            memcpy(&out.px[(static_cast<size_t>(y) * out.w + x) * 4],
                   &in.px[(static_cast<size_t>(y0 + y / o.zoom) * in.w + (x0 + x / o.zoom)) * 4], 4);
    return out;
}

// Stacks images top to bottom with a thin grey rule between them.
Image Stack(const std::vector<Image>& parts) {
    Image out;
    const int rule = parts.size() > 1 ? 2 : 0;
    for (const Image& p : parts) {
        out.w = std::max(out.w, p.w);
        out.h += p.h;
    }
    out.h += rule * static_cast<int>(parts.size() - 1);
    out.px.assign(static_cast<size_t>(out.w) * out.h * 4, 0x60);
    for (size_t i = 3; i < out.px.size(); i += 4) out.px[i] = 255;
    int y = 0;
    for (const Image& p : parts) {
        for (int r = 0; r < p.h; ++r)
            memcpy(&out.px[static_cast<size_t>(y + r) * out.w * 4], &p.px[static_cast<size_t>(r) * p.w * 4], static_cast<size_t>(p.w) * 4);
        y += p.h + rule;
    }
    return out;
}

void SavePng(Gpu& g, const Image& img, const std::wstring& path) {
    ComPtr<IWICBitmap> bmp;
    Check(g.wic->CreateBitmapFromMemory(static_cast<UINT>(img.w), static_cast<UINT>(img.h), GUID_WICPixelFormat32bppBGRA,
                                        static_cast<UINT>(img.w * 4), static_cast<UINT>(img.px.size()),
                                        const_cast<BYTE*>(img.px.data()), &bmp),
          L"WIC bitmap");
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    Check(g.wic->CreateStream(&stream), L"stream");
    Check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), L"open --out");
    Check(g.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc), L"encoder");
    Check(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache), L"encoder init");
    Check(enc->CreateNewFrame(&frame, nullptr), L"frame");
    Check(frame->Initialize(nullptr), L"frame init");
    Check(frame->WriteSource(bmp.Get(), nullptr), L"write");
    Check(frame->Commit(), L"commit frame");
    Check(enc->Commit(), L"commit");
}

// One scene, simulated to each capture time and rendered.
std::vector<Image> Run(Gpu& g, const Options& o, const Settings& s, const FlakeAtlas& atlas, int w, int h, int frames,
                       ID2D1Bitmap1* bgImage) {
    Scene scene;
    SceneGeometry geo;
    geo.width = w;
    geo.height = h;
    geo.dpiScale = o.scale;
    geo.groundY = o.drift > 0 ? static_cast<float>(h) - 40.0f * o.scale : static_cast<float>(h);
    geo.originX = 0.0f;
    geo.originScale = o.scale;
    geo.additiveGlow = !o.hdr;
    geo.monitor = L"lab";
    SimParams p = MakeSimParams(s);
    if (o.snow <= 0) p.density = 0.0f;
    if (o.drift <= 0) p.driftMaxDip = 0.0f;
    scene.SetScenerySettle(0.0f);
    scene.Configure(geo, p);
    if (o.edit) {
        LightsEdit e;
        e.active = true;
        e.strands = s.lightLayout == LightLayout::Custom ? s.strands : Lights::AcrossTop(L"lab", w, h, o.scale);
        e.placing = o.placing && !e.strands.empty() ? 0 : -1;
        e.cursorValid = o.cursorValid;
        e.cursor = o.cursor;
        e.hoverStrand = o.hoverStrand;
        e.hoverHook = o.hoverHook;
        scene.SetLightsEdit(e);
    }
    const double dt = 1.0 / 60.0;
    std::vector<Image> out;
    double time = 0.0;
    float lastX = o.bx0, lastY = o.by0;
    for (int f = 0; f < frames; ++f) {
        const double until = o.t + o.step * f;
        while (time < until - 1e-9) {
            time += dt;
            CursorState c;
            if (o.brush && time >= o.bt0 && time <= o.bt1 && o.bt1 > o.bt0) {
                const float u = static_cast<float>((time - o.bt0) / (o.bt1 - o.bt0));
                c.valid = true;
                c.x = Lerp(o.bx0, o.bx1, u);
                c.y = Lerp(o.by0, o.by1, u);
                c.vx = (c.x - lastX) / static_cast<float>(dt);
                c.vy = (c.y - lastY) / static_cast<float>(dt);
                lastX = c.x;
                lastY = c.y;
            }
            scene.Update(static_cast<float>(dt), time, c);
        }
        out.push_back(CropZoom(Render(g, o, scene, atlas, w, h, bgImage), o));
    }
    return out;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), L"CoInitializeEx");
    const Options o = Parse(argc, argv);
    if (o.calm) SetEnvironmentVariableW(L"SNOWYSTARR_LIGHTS_CALM", L"1");
    Gpu g = MakeGpu();
    FlakeAtlas atlas;
    FlakeArtOptions fo;
    fo.shadow = o.s.shadow;
    Check(atlas.Build(g.dc.Get(), g.wic.Get(), fo), L"flake atlas");
    ComPtr<ID2D1Bitmap1> bgImage;
    if (!o.bgImage.empty()) bgImage = LoadBackground(g, o.bgImage);

    std::vector<Image> parts;
    if (o.gallery) {
        for (int b = 0; b < kBulbTypeCount; ++b) {
            Settings s = o.s;
            s.bulb = static_cast<BulbType>(b);
            std::vector<Image> row = Run(g, o, s, atlas, o.w, o.h, 1, bgImage.Get());
            parts.push_back(std::move(row.front()));
        }
    } else {
        parts = Run(g, o, o.s, atlas, o.w, o.h, o.frames, bgImage.Get());
    }
    SavePng(g, Stack(parts), o.out);
    fwprintf(stdout, L"lightslab: wrote %s\n", o.out.c_str());
    return 0;
}
