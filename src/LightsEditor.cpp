#include "LightsEditor.h"

namespace {

constexpr float kPressDragDip = 6.0f;  // a press dragged this far while placing also hooks where it is let go
constexpr float kSameDip = 6.0f;       // a hook this close to the last one would make no span

float Dist2(float ax, float ay, float bx, float by) {
    const float dx = bx - ax, dy = by - ay;
    return dx * dx + dy * dy;
}

// Squared distance from p to the segment ab.
float SegmentDist2(D2D1_POINT_2F p, D2D1_POINT_2F a, D2D1_POINT_2F b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len2 = dx * dx + dy * dy;
    const float t = len2 > 0.0f ? Saturate(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2) : 0.0f;
    return Dist2(p.x, p.y, a.x + t * dx, a.y + t * dy);
}

LightHook LerpHook(const LightHook& a, const LightHook& b, float t) { return {Lerp(a.u, b.u, t), Lerp(a.v, b.v, t)}; }

}  // namespace

void LightsEditor::Begin(std::vector<EditMonitor> monitors, std::vector<LightStrand> strands) {
    monitors_ = std::move(monitors);
    for (EditMonitor& m : monitors_) {
        // Nothing divides by a zero size or scales by a bad DPI.
        m.width = std::max(m.width, 1.0f);
        m.height = std::max(m.height, 1.0f);
        if (!(m.dpiScale > 0.0f) || !std::isfinite(m.dpiScale)) m.dpiScale = 1.0f;
    }
    strands_.clear();
    for (LightStrand& s : strands) {
        if (s.hooks.size() < 2 || static_cast<int>(strands_.size()) >= kMaxStrands) continue;
        if (s.hooks.size() > static_cast<size_t>(kMaxHooks)) s.hooks.resize(kMaxHooks);
        for (LightHook& h : s.hooks) {
            h.u = std::isfinite(h.u) ? Saturate(h.u) : 0.5f;
            h.v = std::isfinite(h.v) ? Saturate(h.v) : 0.0f;
        }
        s.slack.resize(s.hooks.size() - 1, kDefaultSlack);
        for (float& k : s.slack) k = std::isfinite(k) ? Clamp(k, 0.0f, kMaxSlack) : kDefaultSlack;
        strands_.push_back(std::move(s));
    }
    placing_ = -1;
    placingSlack_ = kDefaultSlack;
    pointerValid_ = false;
    pointerMon_ = -1;
    cursorValid_ = false;
    cursorMon_ = -1;
    cursorSnap_ = 0;
    hover_ = {};
    press_ = PressKind::None;
    dragStrand_ = dragHook_ = -1;
    dragPushed_ = false;
    undo_.clear();
    redo_.clear();
    Changed();
}

std::vector<LightStrand> LightsEditor::Finish() {
    press_ = PressKind::None;
    dragStrand_ = dragHook_ = -1;
    EndStrand();
    std::vector<LightStrand> out;
    out.reserve(strands_.size());
    for (const LightStrand& s : strands_)
        if (s.hooks.size() >= 2) out.push_back(s);
    return out;
}

// ---------------------------------------------------------------------------
// Monitors and hooks
// ---------------------------------------------------------------------------

int LightsEditor::MonitorOf(int strand) const {
    if (strand < 0 || strand >= static_cast<int>(strands_.size())) return -1;
    const std::wstring& id = strands_[static_cast<size_t>(strand)].monitor;
    for (size_t m = 0; m < monitors_.size(); ++m)
        if (monitors_[m].id == id) return static_cast<int>(m);
    return -1;
}

// The monitor under a point, or the nearest one (a captured drag can leave them all).
int LightsEditor::MonitorAt(float x, float y) const {
    int best = -1;
    float bestD = 0.0f;
    for (size_t m = 0; m < monitors_.size(); ++m) {
        const EditMonitor& mon = monitors_[m];
        const float dx = std::max({mon.left - x, 0.0f, x - (mon.left + mon.width)});
        const float dy = std::max({mon.top - y, 0.0f, y - (mon.top + mon.height)});
        const float d = dx * dx + dy * dy;
        if (best < 0 || d < bestD) {
            best = static_cast<int>(m);
            bestD = d;
        }
        if (d == 0.0f) break;
    }
    return best;
}

float LightsEditor::Scale(int monitor) const {
    return monitor >= 0 && monitor < static_cast<int>(monitors_.size()) ? monitors_[static_cast<size_t>(monitor)].dpiScale
                                                                         : 1.0f;
}

D2D1_POINT_2F LightsEditor::ToPx(int monitor, const LightHook& h) const {
    const EditMonitor& m = monitors_[static_cast<size_t>(monitor)];
    return {h.u * m.width, h.v * m.height};
}

// A virtual-screen point on `monitor` (clamped to it) as a hook, snapped to
// the edges it is near: on them, a few DIP in so the wire shows.
LightHook LightsEditor::Snapped(int monitor, float x, float y, int& snap) const {
    const EditMonitor& m = monitors_[static_cast<size_t>(monitor)];
    const float zone = kSnapDip * m.dpiScale;
    const float inset = std::min(kInsetDip * m.dpiScale, 0.5f * std::min(m.width, m.height));
    float lx = Clamp(x - m.left, 0.0f, m.width), ly = Clamp(y - m.top, 0.0f, m.height);
    snap = 0;
    if (lx <= zone) {
        lx = inset;
        snap |= 1;
    } else if (lx >= m.width - zone) {
        lx = m.width - inset;
        snap |= 4;
    }
    if (ly <= zone) {
        ly = inset;
        snap |= 2;
    } else if (ly >= m.height - zone) {
        ly = m.height - inset;
        snap |= 8;
    }
    return {Saturate(lx / m.width), Saturate(ly / m.height)};
}

// The edges a hook is on (as LightsEdit::snap): within the snapping distance,
// so hooks hung before (or across the top) count too.
int LightsEditor::EdgesOf(int monitor, const LightHook& h) const {
    const EditMonitor& m = monitors_[static_cast<size_t>(monitor)];
    const float zone = kSnapDip * m.dpiScale + 0.5f;
    const D2D1_POINT_2F p = ToPx(monitor, h);
    int edges = 0;
    if (p.x <= zone) edges |= 1;
    else if (p.x >= m.width - zone) edges |= 4;
    if (p.y <= zone) edges |= 2;
    else if (p.y >= m.height - zone) edges |= 8;
    return edges;
}

// Two hooks on the same side edge: the span between them runs down it.
bool LightsEditor::SideRun(int monitor, const LightHook& a, const LightHook& b) const {
    return (EdgesOf(monitor, a) & EdgesOf(monitor, b) & (1 | 4)) != 0;
}

// How many spans a side run from a to b is clipped into (b's included), so
// that a strand with `hooks` hooks still fits kMaxHooks.
int LightsEditor::Clips(int monitor, const LightHook& a, const LightHook& b, size_t hooks) const {
    const D2D1_POINT_2F pa = ToPx(monitor, a), pb = ToPx(monitor, b);
    const float dip = std::fabs(pb.y - pa.y) / Scale(monitor);
    const int n = std::max(1, static_cast<int>(std::lround(dip / kClipDip)));
    return std::max(1, std::min(n, kMaxHooks - static_cast<int>(hooks)));
}

bool LightsEditor::TooClose(int monitor, const LightHook& a, const LightHook& b) const {
    const D2D1_POINT_2F pa = ToPx(monitor, a), pb = ToPx(monitor, b);
    const float d = kSameDip * Scale(monitor);
    return Dist2(pa.x, pa.y, pb.x, pb.y) < d * d;
}

// Hooks `h` onto the end of `strand` (room and distance already checked): a
// span with the placing slack, or a side run clipped every so often.
void LightsEditor::AddHook(int strand, const LightHook& h) {
    LightStrand& st = strands_[static_cast<size_t>(strand)];
    if (st.hooks.empty()) {
        st.hooks.push_back(h);
    } else {
        const int m = MonitorOf(strand);
        const LightHook a = st.hooks.back();
        if (m >= 0 && SideRun(m, a, h)) {
            const int n = Clips(m, a, h, st.hooks.size());
            for (int k = 1; k < n; ++k) {
                st.hooks.push_back(LerpHook(a, h, static_cast<float>(k) / static_cast<float>(n)));
                st.slack.push_back(kSideSlack);
            }
            st.hooks.push_back(h);
            st.slack.push_back(kSideSlack);
        } else {
            st.hooks.push_back(h);
            st.slack.push_back(placingSlack_);
        }
    }
    Changed();
}

// The cursor as the next hook of the strand being placed. False when it would
// make no span or the strand is full.
bool LightsEditor::PlaceHook() {
    if (placing_ < 0 || !cursorValid_) return false;
    const LightStrand& st = strands_[static_cast<size_t>(placing_)];
    const int m = MonitorOf(placing_);
    if (m < 0 || st.hooks.size() >= static_cast<size_t>(kMaxHooks)) return false;
    if (!st.hooks.empty() && TooClose(m, st.hooks.back(), cursor_)) return false;
    Push();
    AddHook(placing_, cursor_);
    // A full strand ends there.
    if (strands_[static_cast<size_t>(placing_)].hooks.size() >= static_cast<size_t>(kMaxHooks)) EndStrand();
    UpdateCursor();
    UpdateHover();
    return true;
}

void LightsEditor::RemoveHook(int strand, int hook) {
    LightStrand& st = strands_[static_cast<size_t>(strand)];
    const size_t k = static_cast<size_t>(hook);
    if (st.hooks.size() <= 2) {
        RemoveStrand(strand);
        return;
    }
    if (k == 0) {
        st.hooks.erase(st.hooks.begin());
        st.slack.erase(st.slack.begin());
    } else if (k + 1 >= st.hooks.size()) {
        st.hooks.pop_back();
        st.slack.pop_back();
    } else {
        // Its two spans become one, as slack as they were between them (by length).
        const int m = MonitorOf(strand);
        float ab = 1.0f, bc = 1.0f;
        if (m >= 0) {
            const D2D1_POINT_2F a = ToPx(m, st.hooks[k - 1]), b = ToPx(m, st.hooks[k]), c = ToPx(m, st.hooks[k + 1]);
            ab = std::sqrt(Dist2(a.x, a.y, b.x, b.y));
            bc = std::sqrt(Dist2(b.x, b.y, c.x, c.y));
        }
        if (ab + bc > 0.0f) st.slack[k - 1] = Clamp((st.slack[k - 1] * ab + st.slack[k] * bc) / (ab + bc), 0.0f, kMaxSlack);
        st.slack.erase(st.slack.begin() + static_cast<std::ptrdiff_t>(k));
        st.hooks.erase(st.hooks.begin() + static_cast<std::ptrdiff_t>(k));
    }
    hover_ = {};
    Changed();
}

void LightsEditor::RemoveStrand(int strand) {
    strands_.erase(strands_.begin() + strand);
    if (placing_ == strand) placing_ = -1;
    else if (placing_ > strand) --placing_;
    if (dragStrand_ == strand) {
        press_ = PressKind::None;
        dragStrand_ = dragHook_ = -1;
    } else if (dragStrand_ > strand) {
        --dragStrand_;
    }
    hover_ = {};
    Changed();
}

// ---------------------------------------------------------------------------
// Undo
// ---------------------------------------------------------------------------

// Before an edit: what to go back to.
void LightsEditor::Push() {
    undo_.push_back({strands_, placing_});
    while (undo_.size() > kUndoDepth) undo_.pop_front();
    redo_.clear();
}

void LightsEditor::Apply(const Snapshot& snap) {
    const int wasPlacing = placing_;
    strands_ = snap.strands;
    // Placing goes on only for the strand it went on for (a hook undone while
    // placing); otherwise the strand is done, and one left with a single hook goes.
    placing_ = snap.placing >= 0 && snap.placing == wasPlacing && MonitorOf(snap.placing) >= 0 ? snap.placing : -1;
    for (int i = static_cast<int>(strands_.size()) - 1; i >= 0; --i) {
        if (i == placing_ || strands_[static_cast<size_t>(i)].hooks.size() >= 2) continue;
        strands_.erase(strands_.begin() + i);
        if (placing_ > i) --placing_;
    }
    press_ = PressKind::None;
    dragStrand_ = dragHook_ = -1;
    hover_ = {};
    Changed();
    UpdateCursor();
    UpdateHover();
}

// One step back (or forward) through the history. Steps that would change
// nothing (a strand that never got its second hook) are passed over.
bool LightsEditor::StepHistory(std::deque<Snapshot>& from, std::deque<Snapshot>& to) {
    if (press_ != PressKind::None) return false;
    while (!from.empty()) {
        to.push_back({strands_, placing_});
        const Snapshot snap = std::move(from.back());
        from.pop_back();
        Apply(snap);
        if (strands_ != to.back().strands || placing_ != to.back().placing) {
            while (to.size() > kUndoDepth) to.pop_front();
            return true;
        }
        to.pop_back();
    }
    return false;
}

bool LightsEditor::Undo() { return StepHistory(undo_, redo_); }
bool LightsEditor::Redo() { return StepHistory(redo_, undo_); }

// The strands changed: the hit shapes are stale and a run of wheel steps is over.
void LightsEditor::Changed() {
    shapesStale_ = true;
    wheelSeries_ = false;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void LightsEditor::Move(float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y) || monitors_.empty()) {
        Leave();
        return;
    }
    pointerValid_ = true;
    px_ = x;
    py_ = y;
    pointerMon_ = MonitorAt(x, y);
    if (press_ != PressKind::None && !pressMoved_) {
        // A hook follows at once (fine adjustments); a press that places
        // becomes a drag only after a few DIP, so a click stays a click.
        const float d = press_ == PressKind::Drag ? 1.0f : kPressDragDip * Scale(pointerMon_);
        pressMoved_ = Dist2(x, y, pressX_, pressY_) >= d * d;
    }
    if (press_ == PressKind::Drag && pressMoved_) {
        if (!dragPushed_) {
            undo_.push_back(std::move(dragBefore_));
            while (undo_.size() > kUndoDepth) undo_.pop_front();
            redo_.clear();
            dragPushed_ = true;
        }
        const int m = MonitorOf(dragStrand_);
        if (m >= 0) {
            int snap = 0;
            strands_[static_cast<size_t>(dragStrand_)].hooks[static_cast<size_t>(dragHook_)] =
                Snapped(m, x + grabX_, y + grabY_, snap);
            Changed();
        }
    }
    UpdateCursor();
    UpdateHover();
}

void LightsEditor::Leave() {
    pointerValid_ = false;
    pointerMon_ = -1;
    UpdateCursor();
    UpdateHover();
}

void LightsEditor::Press(float x, float y) {
    Move(x, y);
    if (press_ != PressKind::None || !pointerValid_ || pointerMon_ < 0) return;
    pressX_ = x;
    pressY_ = y;
    pressMoved_ = false;
    if (placing_ >= 0) {
        press_ = PressKind::Place;
        const LightStrand& st = strands_[static_cast<size_t>(placing_)];
        // Clicking the strand's last hook again ends it, like a double click.
        if (cursorValid_ && !st.hooks.empty() && TooClose(MonitorOf(placing_), st.hooks.back(), cursor_)) {
            EndStrand();
            return;
        }
        PlaceHook();
        return;
    }
    if (hover_.strand >= 0 && hover_.hook >= 0) {
        // Grabbed where it is: the hook keeps its offset from the pointer, so it doesn't jump.
        const int m = MonitorOf(hover_.strand);
        if (m < 0) return;
        press_ = PressKind::Drag;
        dragStrand_ = hover_.strand;
        dragHook_ = hover_.hook;
        const D2D1_POINT_2F p =
            ToPx(m, strands_[static_cast<size_t>(dragStrand_)].hooks[static_cast<size_t>(dragHook_)]);
        grabX_ = monitors_[static_cast<size_t>(m)].left + p.x - x;
        grabY_ = monitors_[static_cast<size_t>(m)].top + p.y - y;
        dragBefore_ = {strands_, placing_};
        dragPushed_ = false;
        UpdateCursor();
        UpdateHover();
        return;
    }
    if (static_cast<int>(strands_.size()) >= kMaxStrands || !cursorValid_) return;
    // A new strand, on the monitor it starts on.
    press_ = PressKind::Place;
    Push();
    LightStrand st;
    st.monitor = monitors_[static_cast<size_t>(cursorMon_)].id;
    st.hooks.push_back(cursor_);
    strands_.push_back(std::move(st));
    placing_ = static_cast<int>(strands_.size()) - 1;
    Changed();
    UpdateCursor();
    UpdateHover();
}

void LightsEditor::Release(float x, float y) {
    Move(x, y);
    const PressKind was = press_;
    const bool moved = pressMoved_;
    press_ = PressKind::None;
    dragStrand_ = dragHook_ = -1;
    UpdateCursor();
    if (was == PressKind::Place && moved) PlaceHook();
    UpdateHover();
}

void LightsEditor::CancelPress() {
    if (press_ == PressKind::None) return;
    press_ = PressKind::None;
    dragStrand_ = dragHook_ = -1;
    UpdateCursor();
    UpdateHover();
}

bool LightsEditor::CancelDrag() {
    if (press_ != PressKind::Drag) return false;
    if (dragPushed_ && !undo_.empty()) {
        strands_ = std::move(undo_.back().strands);
        undo_.pop_back();
    }
    press_ = PressKind::None;
    dragStrand_ = dragHook_ = -1;
    hover_ = {};
    Changed();
    UpdateCursor();
    UpdateHover();
    return true;
}

bool LightsEditor::EndStrand() {
    if (placing_ < 0) return false;
    const int s = placing_;
    placing_ = -1;
    // One hook is no strand. (No undo step: undoing goes back past its first hook anyway.)
    if (strands_[static_cast<size_t>(s)].hooks.size() < 2) RemoveStrand(s);
    Changed();
    UpdateCursor();
    UpdateHover();
    return true;
}

void LightsEditor::Wheel(float notches) {
    if (!std::isfinite(notches) || notches == 0.0f || press_ == PressKind::Drag) return;
    const float step = -notches * kSlackStep;
    if (placing_ >= 0) {
        placingSlack_ = Clamp(placingSlack_ + step, 0.0f, kMaxSlack);
        return;
    }
    if (hover_.strand < 0) return;
    LightStrand& st = strands_[static_cast<size_t>(hover_.strand)];
    // The span under the pointer, or both spans of a hook.
    const int spans = static_cast<int>(st.slack.size());
    int j0 = hover_.span, j1 = hover_.span;
    if (hover_.hook >= 0) {
        j0 = hover_.hook - 1;
        j1 = hover_.hook;
    }
    j0 = std::max(j0, 0);
    j1 = std::min(j1, spans - 1);
    if (j0 > j1) return;
    // One undo step for a run of notches on the same target. The hover stays put
    // meanwhile (the wire moves away from the pointer as it sags) until the pointer moves.
    if (!wheelSeries_ || !(wheelTarget_ == hover_)) {
        Push();
        wheelSeries_ = true;
        wheelTarget_ = hover_;
    }
    for (int j = j0; j <= j1; ++j) {
        float& k = st.slack[static_cast<size_t>(j)];
        k = Clamp(k + step, 0.0f, kMaxSlack);
    }
    shapesStale_ = true;
}

bool LightsEditor::Delete() {
    if (press_ != PressKind::None) return false;
    if (placing_ >= 0) {
        // While placing: the last hook goes (and with it, the strand).
        Push();
        LightStrand& st = strands_[static_cast<size_t>(placing_)];
        if (st.hooks.size() <= 1) {
            RemoveStrand(placing_);
        } else {
            st.hooks.pop_back();
            st.slack.pop_back();
            Changed();
        }
    } else if (hover_.strand >= 0) {
        Push();
        if (hover_.hook >= 0) RemoveHook(hover_.strand, hover_.hook);
        else RemoveStrand(hover_.strand);
    } else {
        return false;
    }
    UpdateCursor();
    UpdateHover();
    return true;
}

bool LightsEditor::ClearAll() {
    CancelPress();
    bool any = false;
    for (int i = 0; i < static_cast<int>(strands_.size()) && !any; ++i) any = MonitorOf(i) >= 0;
    if (!any) return false;
    Push();
    // Strands on monitors not here now stay (they can't be seen to be cleared).
    std::vector<LightStrand> kept;
    for (int i = 0; i < static_cast<int>(strands_.size()); ++i)
        if (MonitorOf(i) < 0) kept.push_back(std::move(strands_[static_cast<size_t>(i)]));
    strands_ = std::move(kept);
    placing_ = -1;
    hover_ = {};
    Changed();
    UpdateCursor();
    UpdateHover();
    return true;
}

// ---------------------------------------------------------------------------
// Pointer state
// ---------------------------------------------------------------------------

// Where the next hook would go: on the strand being placed's monitor (clamped
// to it), where the dragged hook goes, or under the pointer.
void LightsEditor::UpdateCursor() {
    cursorValid_ = false;
    cursorSnap_ = 0;
    if (!pointerValid_) {
        cursorMon_ = -1;
        return;
    }
    float x = px_, y = py_;
    int m = pointerMon_;
    if (press_ == PressKind::Drag) {
        m = MonitorOf(dragStrand_);
        x += grabX_;
        y += grabY_;
    } else if (placing_ >= 0) {
        m = MonitorOf(placing_);
    }
    cursorMon_ = m;
    if (m < 0) return;
    cursor_ = Snapped(m, x, y, cursorSnap_);
    cursorValid_ = true;
}

void LightsEditor::UpdateHover() {
    if (press_ == PressKind::Drag) {
        hover_ = {dragStrand_, dragHook_, -1};
    } else if (placing_ >= 0 || !pointerValid_ || pointerMon_ < 0) {
        hover_ = {};
    } else {
        const EditMonitor& m = monitors_[static_cast<size_t>(pointerMon_)];
        hover_ = HitTest(pointerMon_, px_ - m.left, py_ - m.top);
    }
    if (!(hover_ == wheelTarget_)) wheelSeries_ = false;
}

void LightsEditor::BuildShapes() const {
    if (!shapesStale_) return;
    shapesStale_ = false;
    shapes_.resize(strands_.size());
    for (size_t i = 0; i < strands_.size(); ++i) {
        Shape& sh = shapes_[i];
        sh.pts.clear();
        sh.spanEnd.clear();
        sh.bounds.clear();
        const int m = MonitorOf(static_cast<int>(i));
        if (m < 0) continue;
        const LightStrand& st = strands_[i];
        for (size_t j = 0; j + 1 < st.hooks.size(); ++j) {
            scratch_.clear();
            const float slack = j < st.slack.size() ? st.slack[j] : kDefaultSlack;
            Lights::SpanShape(ToPx(m, st.hooks[j]), ToPx(m, st.hooks[j + 1]), slack, Scale(m), scratch_,
                              monitors_[static_cast<size_t>(m)].width);
            D2D1_RECT_F b = {HUGE_VALF, HUGE_VALF, -HUGE_VALF, -HUGE_VALF};
            for (const D2D1_POINT_2F& p : scratch_) {
                if (!std::isfinite(p.x) || !std::isfinite(p.y)) continue;
                sh.pts.push_back(p);
                b = {std::min(b.left, p.x), std::min(b.top, p.y), std::max(b.right, p.x), std::max(b.bottom, p.y)};
            }
            sh.spanEnd.push_back(sh.pts.size());
            sh.bounds.push_back(b);
        }
    }
}

// What is under (x, y), px on `monitor`: the nearest hook within reach, else
// the nearest wire.
LightsEditor::Hit LightsEditor::HitTest(int monitor, float x, float y) const {
    BuildShapes();
    const float s = Scale(monitor);
    const D2D1_POINT_2F p = {x, y};
    Hit hit;
    float best = kHookHitDip * s * kHookHitDip * s;
    for (int i = 0; i < static_cast<int>(strands_.size()); ++i) {
        if (MonitorOf(i) != monitor) continue;
        const LightStrand& st = strands_[static_cast<size_t>(i)];
        for (size_t k = 0; k < st.hooks.size(); ++k) {
            const D2D1_POINT_2F h = ToPx(monitor, st.hooks[k]);
            const float d = Dist2(p.x, p.y, h.x, h.y);
            if (d <= best) {
                best = d;
                hit = {i, static_cast<int>(k), -1};
            }
        }
    }
    if (hit.strand >= 0) return hit;

    const float reach = kWireHitDip * s;
    best = reach * reach;
    for (int i = 0; i < static_cast<int>(strands_.size()); ++i) {
        if (MonitorOf(i) != monitor) continue;
        const Shape& sh = shapes_[static_cast<size_t>(i)];
        size_t start = 0;
        for (size_t j = 0; j < sh.spanEnd.size(); ++j) {
            const size_t end = sh.spanEnd[j];
            const D2D1_RECT_F& b = sh.bounds[j];
            if (x >= b.left - reach && x <= b.right + reach && y >= b.top - reach && y <= b.bottom + reach) {
                for (size_t k = start + 1; k < end; ++k) {
                    const float d = SegmentDist2(p, sh.pts[k - 1], sh.pts[k]);
                    if (d <= best) {
                        best = d;
                        hit = {i, -1, static_cast<int>(j)};
                    }
                }
            }
            start = end;
        }
    }
    return hit;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

LightsEditor::Over LightsEditor::Hover() const {
    if (press_ == PressKind::Drag || (hover_.strand >= 0 && hover_.hook >= 0)) return Over::Hook;
    return hover_.strand >= 0 ? Over::Wire : Over::Nothing;
}

float LightsEditor::Sag() const {
    if (placing_ >= 0) {
        // A run down a side edge gets the clips' slack whatever the wheel says.
        const LightStrand& st = strands_[static_cast<size_t>(placing_)];
        const int m = MonitorOf(placing_);
        if (cursorValid_ && m >= 0 && cursorMon_ == m && !st.hooks.empty() && SideRun(m, st.hooks.back(), cursor_))
            return kSideSlack;
        return placingSlack_;
    }
    if (press_ == PressKind::Drag || hover_.strand < 0) return -1.0f;
    const LightStrand& st = strands_[static_cast<size_t>(hover_.strand)];
    const int spans = static_cast<int>(st.slack.size());
    if (hover_.hook < 0) return hover_.span >= 0 && hover_.span < spans ? st.slack[static_cast<size_t>(hover_.span)] : -1.0f;
    // A hook: its spans, on either side.
    float sum = 0.0f;
    int n = 0;
    for (int j : {hover_.hook - 1, hover_.hook}) {
        if (j < 0 || j >= spans) continue;
        sum += st.slack[static_cast<size_t>(j)];
        ++n;
    }
    return n > 0 ? sum / static_cast<float>(n) : -1.0f;
}

int LightsEditor::StrandCount() const {
    int n = 0;
    for (int i = 0; i < static_cast<int>(strands_.size()); ++i)
        if (MonitorOf(i) >= 0) ++n;
    return n;
}

LightsEdit LightsEditor::EditFor(int monitor) const {
    LightsEdit e;
    if (monitor < 0 || monitor >= static_cast<int>(monitors_.size())) return e;
    e.active = true;
    e.placingSlack = placingSlack_;
    for (int i = 0; i < static_cast<int>(strands_.size()); ++i) {
        if (MonitorOf(i) != monitor) continue;
        const int local = static_cast<int>(e.strands.size());
        if (i == placing_) e.placing = local;
        if (i == hover_.strand) {
            e.hoverStrand = local;
            e.hoverHook = hover_.hook;
        }
        e.strands.push_back(strands_[static_cast<size_t>(i)]);
    }
    // Over a hook, a press grabs it rather than hanging a new one there.
    const bool overHook = press_ != PressKind::Drag && placing_ < 0 && hover_.hook >= 0;
    if (cursorValid_ && cursorMon_ == monitor && !overHook) {
        e.cursorValid = true;
        e.cursor = cursor_;
        e.snap = cursorSnap_;
    }
    // A run down a side edge shows the clips it will get.
    if (e.placing >= 0 && e.cursorValid) {
        LightStrand& st = e.strands[static_cast<size_t>(e.placing)];
        if (!st.hooks.empty() && SideRun(monitor, st.hooks.back(), cursor_)) {
            const LightHook a = st.hooks.back();
            const int n = Clips(monitor, a, cursor_, st.hooks.size());
            for (int k = 1; k < n; ++k) {
                st.hooks.push_back(LerpHook(a, cursor_, static_cast<float>(k) / static_cast<float>(n)));
                st.slack.push_back(kSideSlack);
            }
            e.placingSlack = kSideSlack;
        }
    }
    return e;
}
