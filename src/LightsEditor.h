// Hanging lights: the editor behind hanging mode. It knows nothing of windows
// or messages, so it can be tested on its own: App feeds it the pointer (in
// virtual-screen px) and keys, and sends each monitor's renderer EditFor().
//
// It keeps every strand (hooks as fractions of their monitor), the strand being
// placed, the hook being dragged and what the pointer is over. Hooks snap to
// the monitor edges (corners both ways), a run down a side edge is clipped
// every so often so it scallops gently instead of bowing into the screen, a
// strand stays on the monitor it was started on, and every edit can be undone.
// Hit-testing uses Lights::SpanShape, so the pointer finds a wire where it is
// drawn.
#pragma once

#include "Lights.h"
#include "Settings.h"

#include <deque>

// A monitor strands can be hung on.
struct EditMonitor {
    std::wstring id;                     // MonitorTarget::deviceId
    float left = 0.0f, top = 0.0f;       // virtual-screen px
    float width = 0.0f, height = 0.0f;   // px
    float dpiScale = 1.0f;               // px per DIP
};

class LightsEditor {
public:
    static constexpr float kSnapDip = 18.0f;      // a point this close to a monitor edge snaps to it
    static constexpr float kInsetDip = 4.0f;      // ... and sits this far in, so the wire shows
    static constexpr float kClipDip = 110.0f;     // a run down a side edge is clipped about this often
    static constexpr float kSideSlack = 0.025f;   // ... and scallops this much between the clips
    static constexpr float kHookHitDip = 10.0f;   // the pointer is over a hook this close to it
    static constexpr float kWireHitDip = 8.0f;    // ... and over a wire this close
    static constexpr float kSlackStep = 0.01f;    // per wheel notch
    static constexpr size_t kUndoDepth = 100;

    // What the pointer is over.
    enum class Over { Nothing, Hook, Wire };

    // Starts editing `strands` (every monitor's; those on monitors not in the
    // list are kept as they are) on `monitors`, with no undo history.
    void Begin(std::vector<EditMonitor> monitors, std::vector<LightStrand> strands);
    // Ends the strand being placed and hands back every strand.
    std::vector<LightStrand> Finish();

    // ---- Input (virtual-screen px) ----------------------------------------
    void Move(float x, float y);
    // The pointer is off the monitors, or over something else (the HUD).
    void Leave();
    // Left button down: a hook (starting a strand when none is being placed),
    // or grabs the hook under the pointer to drag it.
    void Press(float x, float y);
    // ... and up. A press dragged out while placing also hooks here.
    void Release(float x, float y);
    // The button is no longer ours (capture lost): what was dragged stays.
    void CancelPress();
    // Esc during a drag: the hook goes back where it was. False: no drag.
    bool CancelDrag();
    // Ends the strand being placed, kept when it has two hooks or more.
    // False when none was being placed.
    bool EndStrand();
    // Wheel notches (positive: away from the user, less sag): the span being
    // placed, or the span or hook under the pointer.
    void Wheel(float notches);
    // Removes the hook or strand under the pointer (while placing: the last hook).
    bool Delete();
    bool Undo();
    bool Redo();
    // Takes down every strand on these monitors (undoable).
    bool ClearAll();

    // ---- State --------------------------------------------------------------
    bool Placing() const { return placing_ >= 0; }
    bool Pressed() const { return press_ != PressKind::None; }
    bool DraggingHook() const { return press_ == PressKind::Drag; }
    Over Hover() const;
    // Monitor under the pointer (-1: none).
    int PointerMonitor() const { return pointerValid_ ? pointerMon_ : -1; }
    // The slack of the span being placed or under the pointer (-1: none).
    float Sag() const;
    // Strands on these monitors (the one being placed included).
    int StrandCount() const;
    // What monitor `monitor` draws: its strands, the live span, the cursor and
    // the hover, indices local to its strands. While a strand is being placed
    // the cursor is on that strand's monitor (clamped to it), wherever the
    // pointer is; while a hook is dragged it is where the hook goes. Inactive
    // for a monitor not in the list.
    LightsEdit EditFor(int monitor) const;
    // Every strand, as edited so far (the one being placed may have one hook).
    const std::vector<LightStrand>& Strands() const { return strands_; }

private:
    enum class PressKind { None, Place, Drag };
    struct Hit {
        int strand = -1, hook = -1, span = -1;  // a hook, or a span of the wire (hook -1)
        bool operator==(const Hit&) const = default;
    };
    struct Snapshot {
        std::vector<LightStrand> strands;
        int placing = -1;
    };
    // A strand's rest shape in its monitor's px, for hit-testing.
    struct Shape {
        std::vector<D2D1_POINT_2F> pts;
        std::vector<size_t> spanEnd;     // one past each span's last point
        std::vector<D2D1_RECT_F> bounds; // each span's
    };

    int MonitorOf(int strand) const;
    int MonitorAt(float x, float y) const;
    float Scale(int monitor) const;
    D2D1_POINT_2F ToPx(int monitor, const LightHook& h) const;
    LightHook Snapped(int monitor, float x, float y, int& snap) const;
    int EdgesOf(int monitor, const LightHook& h) const;
    bool SideRun(int monitor, const LightHook& a, const LightHook& b) const;
    int Clips(int monitor, const LightHook& a, const LightHook& b, size_t hooks) const;
    bool TooClose(int monitor, const LightHook& a, const LightHook& b) const;
    void AddHook(int strand, const LightHook& h);
    bool PlaceHook();
    void RemoveHook(int strand, int hook);
    void RemoveStrand(int strand);
    void Push();
    void Apply(const Snapshot& snap);
    bool StepHistory(std::deque<Snapshot>& from, std::deque<Snapshot>& to);
    void Changed();
    void UpdateCursor();
    void UpdateHover();
    void BuildShapes() const;
    Hit HitTest(int monitor, float x, float y) const;

    std::vector<EditMonitor> monitors_;
    std::vector<LightStrand> strands_;
    int placing_ = -1;                   // strand being placed
    float placingSlack_ = kDefaultSlack; // for its next span

    // The pointer.
    bool pointerValid_ = false;
    float px_ = 0.0f, py_ = 0.0f;
    int pointerMon_ = -1;
    // Where the next hook would go.
    bool cursorValid_ = false;
    int cursorMon_ = -1;
    LightHook cursor_;
    int cursorSnap_ = 0;
    Hit hover_;

    // A button held down.
    PressKind press_ = PressKind::None;
    float pressX_ = 0.0f, pressY_ = 0.0f;
    bool pressMoved_ = false;
    int dragStrand_ = -1, dragHook_ = -1;
    float grabX_ = 0.0f, grabY_ = 0.0f;  // px from the pointer to the dragged hook
    Snapshot dragBefore_;
    bool dragPushed_ = false;

    std::deque<Snapshot> undo_, redo_;
    bool wheelSeries_ = false;  // wheel steps on one target since the last other edit: one undo step
    Hit wheelTarget_;

    mutable std::vector<Shape> shapes_;
    mutable bool shapesStale_ = true;
    mutable std::vector<D2D1_POINT_2F> scratch_;
};
