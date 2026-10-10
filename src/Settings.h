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

struct Settings {
    uint32_t flakeMask = kDefaultFlakeMask;
    int amount = 45;          // 0..100, maps to flake density
    int speed = 50;           // 0..100 (50 = natural speed)
    int wind = 15;            // -100..100, negative blows left
    int gusts = 45;           // 0..100, how much the wind varies
    int size = 50;            // 0..100
    int drift = 55;           // 0..100, max drift height; 0 disables accumulation
    SnowColor color = SnowColor::White;
    COLORREF customColor = RGB(200, 230, 255);
    bool shadow = true;       // faint dark halo so flakes read on light windows
    bool mouse = true;        // cursor pushes flakes and plows drifts
    bool pauseFullscreen = true;
    bool allMonitors = true;
    bool paused = false;

    bool operator==(const Settings&) const = default;

    // Ensure values are in range and at least one flake kind is enabled.
    void Sanitize();
};

Settings LoadSettings();
void SaveSettings(const Settings& s);

// Start-with-Windows (HKCU Run key). Reads the live registry state.
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
extern const Preset kDriftPresets[4];
