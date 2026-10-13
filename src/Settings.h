// User settings, persisted under HKCU\Software\snowystarr.
#pragma once

#include "Common.h"

// Every selectable flake design. The order is the order of the tiles in the
// settings window; the stored bitmask uses these values, so only append.
enum class FlakeKind : int {
    Dot,          // tiny powder grain
    Soft,         // soft, slightly blurred round flake (bokeh)
    Plus,         // small cross
    Asterisk,     // six thin spokes
    Sparkle,      // four-point twinkling star
    Dendrite,     // stellar dendrite (fern-like branches)
    Sectored,     // sectored plate
    Fern,         // dense fernlike dendrite
    HexPlate,     // hexagonal plate
    StellarPlate, // stellar plate with broad arm tips
    Needle,       // thin needle star
    Twelve,       // twelve-branched star
    Classic1, Classic2, Classic3, Classic4,   // classic pixel-art flakes
    Classic5, Classic6, Classic7, Classic8,
    Count
};

constexpr int kFlakeKindCount = static_cast<int>(FlakeKind::Count);
constexpr uint32_t FlakeBit(FlakeKind k) { return 1u << static_cast<int>(k); }
constexpr uint32_t kAllFlakesMask = (1u << kFlakeKindCount) - 1u;
constexpr uint32_t kClassicMask = FlakeBit(FlakeKind::Classic1) | FlakeBit(FlakeKind::Classic2) |
                                  FlakeBit(FlakeKind::Classic3) | FlakeBit(FlakeKind::Classic4) |
                                  FlakeBit(FlakeKind::Classic5) | FlakeBit(FlakeKind::Classic6) |
                                  FlakeBit(FlakeKind::Classic7) | FlakeBit(FlakeKind::Classic8);
constexpr uint32_t kCrystalMask = FlakeBit(FlakeKind::Dendrite) | FlakeBit(FlakeKind::Sectored) |
                                  FlakeBit(FlakeKind::Fern) | FlakeBit(FlakeKind::HexPlate) |
                                  FlakeBit(FlakeKind::StellarPlate) | FlakeBit(FlakeKind::Needle) |
                                  FlakeBit(FlakeKind::Twelve);
constexpr uint32_t kSoftMask = FlakeBit(FlakeKind::Dot) | FlakeBit(FlakeKind::Soft);
constexpr uint32_t kSimpleMask = FlakeBit(FlakeKind::Dot) | FlakeBit(FlakeKind::Plus) |
                                 FlakeBit(FlakeKind::Asterisk) | FlakeBit(FlakeKind::Sparkle);
constexpr uint32_t kDefaultFlakeMask = kSoftMask | kCrystalMask | FlakeBit(FlakeKind::Sparkle);

const wchar_t* FlakeKindName(FlakeKind k);

enum class SnowColor : int {
    White,
    Ice,       // pale blue
    Rainbow,   // pastel colours per flake
    Custom,
    Count
};

// Holiday lights. Every enum is stored by value: only append.
enum class BulbType : int {
    Mini,     // mini incandescent: slim glass, a short bright filament
    MiniLed,  // mini LED: a small cylindrical lens
    WideLed,  // wide-angle LED: a short lens with a dished face
    C5,       // C5 twinkle: a small flame-tipped bulb
    C6,       // C6 strawberry: a faceted cone
    C7,       // C7: the classic tapered bulb
    C9,       // C9: the big one
    Globe,    // G40 globe
    Bubble,   // bubble light: a glowing base and a tube of bubbling liquid
    Candle,   // flickering candle lamp: clear flame-tipped glass with a flame inside
    Count
};
constexpr int kBulbTypeCount = static_cast<int>(BulbType::Count);

enum class LightColors : int {
    Multicolor,
    WarmWhite,
    CoolWhite,
    RedGreen,
    BlueWhite,
    Custom,
    Count
};

enum class LightAnimation : int {
    Steady,
    Twinkle,      // bulbs drop out at random
    Pulse,        // the whole strand breathes
    Chase,        // sets of bulbs march along the strand
    Alternate,    // odd and even bulbs take turns
    Waves,        // brightness rolls along the strand
    Combination,  // cycles through the others
    Count
};

enum class WireColor : int { Green, White, Black, Count };

enum class LightLayout : int {
    AcrossTop,  // one drape across the top of every monitor
    Custom,     // the strands the user hung
    Count
};

// A strand the user hung: its hooks in order along it, as fractions of its
// monitor's width and height (so it survives a change of resolution), and how
// much slack each span between two hooks has (extra length over the straight
// distance, as a fraction of it).
struct LightHook {
    float u = 0.0f, v = 0.0f;
    bool operator==(const LightHook&) const = default;
};
struct LightStrand {
    std::wstring monitor;           // MonitorTarget::deviceId
    std::vector<LightHook> hooks;   // at least 2
    std::vector<float> slack;       // one per span: hooks.size() - 1
    bool operator==(const LightStrand&) const = default;
};
constexpr int kMaxStrands = 64;      // in all
constexpr int kMaxHooks = 160;       // per strand
constexpr float kDefaultSlack = 0.05f;
constexpr float kMaxSlack = 0.4f;

const wchar_t* BulbTypeName(BulbType t);
const wchar_t* LightAnimationName(LightAnimation a);
const wchar_t* LightColorsName(LightColors c);
const wchar_t* WireColorName(WireColor w);

struct Settings {
    uint32_t flakeMask = kDefaultFlakeMask;
    int amount = 45;          // 0..100, maps to flake density
    int speed = 50;           // 0..100 (50 = natural speed)
    int wind = 15;            // -100..100, negative blows left
    int gusts = 45;           // 0..100, how much the wind varies
    int size = 50;            // 0..100
    int drift = 55;           // 0..200, max drift height; 0 disables accumulation (above 100: snowbanks)
    SnowColor color = SnowColor::White;
    COLORREF customColor = RGB(200, 230, 255);
    bool shadow = true;       // faint dark halo so flakes read on light windows
    bool mouse = true;        // cursor pushes flakes and plows drifts
    bool pauseFullscreen = true;
    bool allMonitors = true;
    bool behindWindows = false;  // snow only on the desktop: windows cover it
    bool trees = true;        // little holiday trees with lights stand in the drifts
    int treeAmount = 35;      // 0..100: how many (35 = the usual few; more is a forest)
    bool snowmen = true;      // snowmen stand in the drifts too
    int snowmanAmount = 35;   // 0..100
    bool lights = true;       // strings of holiday lights
    BulbType bulb = BulbType::C7;
    LightColors lightColors = LightColors::Multicolor;
    COLORREF lightCustomColor = RGB(255, 190, 110);
    LightAnimation lightAnimation = LightAnimation::Twinkle;
    int lightSpeed = 50;      // 0..100 (50 = natural)
    int lightSize = 50;       // 0..100 (50 = the usual size)
    int lightGlow = 60;       // 0..100: how much light they throw
    WireColor wire = WireColor::Green;
    LightLayout lightLayout = LightLayout::AcrossTop;
    std::vector<LightStrand> strands;  // every monitor's (LightLayout::Custom)
    bool paused = false;

    bool operator==(const Settings&) const = default;

    // Ensure values are in range and at least one flake kind is enabled.
    void Sanitize();
};

Settings LoadSettings();
void SaveSettings(const Settings& s);

// Running from the Microsoft Store (MSIX) package rather than a plain install.
bool IsPackaged();

// Start-with-Windows: the HKCU Run key, or the startup task when running from
// the Microsoft Store (MSIX) package. Reads the live state.
bool IsStartWithWindows();
bool SetStartWithWindows(bool enable);

// Simple presets offered in the tray menu.
struct Preset {
    const wchar_t* name;
    int value;
};
extern const Preset kAmountPresets[5];
extern const Preset kSpeedPresets[3];
extern const Preset kWindPresets[5];
extern const Preset kDriftPresets[5];
extern const Preset kTreePresets[4];
extern const Preset kSnowmanPresets[4];
extern const Preset kLightSpeedPresets[3];
