// lightsedit_test: exercises the hanging-mode editor (LightsEditor) without
// any windows: snapping, corners, side-run clips, the monitor a strand
// belongs to, hit-testing, dragging, the wheel, deleting, undo and redo.
// Developer builds only; prints each failed check and exits non-zero if any.
//
//   lightsedit_test [--hud out.png] [--hud-scale 1.5]
//
// --hud also renders the hanging-mode HUD offscreen to a PNG (a few states
// stacked top to bottom), to look at without showing a window.
#include "LightsEditor.h"
#include "LightsHud.h"

#include <objbase.h>

#include <cstdio>

namespace {

int g_checks = 0, g_failures = 0;

void Check(bool ok, const char* what, int line) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    printf("  FAIL (line %d): %s\n", line, what);
}
#define CHECK(cond) Check((cond), #cond, __LINE__)

bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// Two monitors side by side: A at 100%, B at 150% and a little higher.
std::vector<EditMonitor> Monitors() {
    return {{L"A", 0.0f, 0.0f, 1920.0f, 1080.0f, 1.0f}, {L"B", 1920.0f, -200.0f, 2560.0f, 1440.0f, 1.5f}};
}

LightStrand Strand(const wchar_t* monitor, std::vector<LightHook> hooks, std::vector<float> slack) {
    LightStrand s;
    s.monitor = monitor;
    s.hooks = std::move(hooks);
    s.slack = std::move(slack);
    return s;
}

void Click(LightsEditor& e, float x, float y) {
    e.Press(x, y);
    e.Release(x, y);
}

// Where a span hangs lowest (the engine's own rest shape), on monitor A.
D2D1_POINT_2F Belly(D2D1_POINT_2F a, D2D1_POINT_2F b, float slack, float scale) {
    std::vector<D2D1_POINT_2F> pts;
    Lights::SpanShape(a, b, slack, scale, pts);
    D2D1_POINT_2F low = a;
    for (const D2D1_POINT_2F& p : pts)
        if (p.y > low.y) low = p;
    return low;
}

void Section(const char* name) { printf("%s\n", name); }

void TestSnapping() {
    Section("snapping and corners");
    LightsEditor e;
    e.Begin(Monitors(), {});
    e.Move(10.0f, 500.0f);
    LightsEdit a = e.EditFor(0);
    CHECK(a.active && a.cursorValid);
    CHECK(Near(a.cursor.u, 4.0f / 1920.0f) && Near(a.cursor.v, 500.0f / 1080.0f));
    CHECK(a.snap == 1);
    e.Move(18.0f, 500.0f);
    CHECK(e.EditFor(0).snap == 1);
    e.Move(19.0f, 500.0f);
    a = e.EditFor(0);
    CHECK(a.snap == 0 && Near(a.cursor.u, 19.0f / 1920.0f));
    e.Move(5.0f, 5.0f);
    a = e.EditFor(0);
    CHECK(a.snap == (1 | 2) && Near(a.cursor.u, 4.0f / 1920.0f) && Near(a.cursor.v, 4.0f / 1080.0f));
    e.Move(1915.0f, 1075.0f);
    a = e.EditFor(0);
    CHECK(a.snap == (4 | 8) && Near(a.cursor.u, 1916.0f / 1920.0f) && Near(a.cursor.v, 1076.0f / 1080.0f));
    e.Move(1000.0f, 3.0f);
    CHECK(e.EditFor(0).snap == 2);
    // Monitor B at 150%: the zone is 27 px and the inset 6 px.
    e.Move(1920.0f + 26.0f, 300.0f);
    LightsEdit b = e.EditFor(1);
    CHECK(b.cursorValid && b.snap == 1 && Near(b.cursor.u, 6.0f / 2560.0f) && Near(b.cursor.v, 500.0f / 1440.0f));
    CHECK(!e.EditFor(0).cursorValid);
    CHECK(e.PointerMonitor() == 1);
    e.Move(1920.0f + 28.0f, 300.0f);
    CHECK(e.EditFor(1).snap == 0);
    e.Move(1920.0f + 2559.0f, -200.0f + 1439.0f);
    CHECK(e.EditFor(1).snap == (4 | 8));
}

void TestPlacing() {
    Section("placing a strand, sag, ending");
    LightsEditor e;
    e.Begin(Monitors(), {});
    Click(e, 300.0f, 200.0f);
    CHECK(e.Placing() && e.Strands().size() == 1 && e.Strands()[0].hooks.size() == 1);
    CHECK(e.Strands()[0].monitor == L"A");
    CHECK(e.EditFor(0).placing == 0);
    e.Move(900.0f, 220.0f);
    CHECK(e.EditFor(0).cursorValid && Near(e.EditFor(0).cursor.u, 900.0f / 1920.0f));
    CHECK(Near(e.Sag(), kDefaultSlack));
    e.Wheel(2.0f);
    CHECK(Near(e.Sag(), kDefaultSlack - 0.02f));
    e.Wheel(-5.0f);
    CHECK(Near(e.Sag(), kDefaultSlack + 0.03f));
    e.Wheel(-100.0f);
    CHECK(Near(e.Sag(), kMaxSlack));
    e.Wheel(100.0f);
    CHECK(Near(e.Sag(), 0.0f));
    e.Wheel(-7.0f);
    CHECK(Near(e.Sag(), 0.07f) && Near(e.EditFor(0).placingSlack, 0.07f));
    Click(e, 900.0f, 220.0f);
    CHECK(e.Strands()[0].hooks.size() == 2 && e.Strands()[0].slack.size() == 1);
    CHECK(Near(e.Strands()[0].slack[0], 0.07f));
    Click(e, 1500.0f, 250.0f);
    CHECK(e.Strands()[0].hooks.size() == 3 && Near(e.Strands()[0].slack[1], 0.07f));
    // Clicking the last hook again ends the strand.
    Click(e, 1502.0f, 251.0f);
    CHECK(!e.Placing() && e.Strands()[0].hooks.size() == 3);
    CHECK(e.EditFor(0).placing == -1);
    CHECK(!e.EndStrand());

    // One hook is no strand.
    Click(e, 500.0f, 600.0f);
    CHECK(e.Placing() && e.Strands().size() == 2);
    CHECK(e.EndStrand());
    CHECK(!e.Placing() && e.Strands().size() == 1);

    // Press, drag and let go: a span at once, and still placing.
    e.Press(200.0f, 700.0f);
    e.Move(400.0f, 760.0f);
    e.Release(600.0f, 760.0f);
    CHECK(e.Placing() && e.Strands().size() == 2 && e.Strands()[1].hooks.size() == 2);
    CHECK(Near(e.Strands()[1].hooks[1].u, 600.0f / 1920.0f));
    e.EndStrand();
    // A click that wobbles a pixel or two stays a click.
    e.Press(200.0f, 900.0f);
    e.Release(202.0f, 901.0f);
    CHECK(e.Placing() && e.Strands()[2].hooks.size() == 1);
    e.EndStrand();
    CHECK(e.StrandCount() == 2);
}

void TestSideRuns() {
    Section("side runs and corners");
    LightsEditor e;
    e.Begin(Monitors(), {});
    Click(e, 1915.0f, 100.0f);
    e.Move(1915.0f, 1000.0f);
    // 900 px at 100% is 8 clips' worth: the preview shows the 7 between.
    LightsEdit p = e.EditFor(0);
    CHECK(p.placing == 0 && p.strands[0].hooks.size() == 8 && Near(p.placingSlack, LightsEditor::kSideSlack));
    CHECK(Near(e.Sag(), LightsEditor::kSideSlack));
    Click(e, 1915.0f, 1000.0f);
    const LightStrand& s = e.Strands()[0];
    CHECK(s.hooks.size() == 9 && s.slack.size() == 8);
    bool onEdge = true, even = true, slack = true;
    for (size_t i = 0; i < s.hooks.size(); ++i) {
        onEdge = onEdge && Near(s.hooks[i].u, 1916.0f / 1920.0f);
        if (i > 0) even = even && Near(s.hooks[i].v - s.hooks[i - 1].v, 900.0f / 1080.0f / 8.0f, 1e-3f);
    }
    for (float k : s.slack) slack = slack && Near(k, LightsEditor::kSideSlack);
    CHECK(onEdge && even && slack);
    e.EndStrand();

    // Corner to corner down the left edge, then along the bottom: the bottom span is an ordinary one.
    Click(e, 3.0f, 3.0f);
    CHECK(Near(e.Strands()[1].hooks[0].u, 4.0f / 1920.0f) && Near(e.Strands()[1].hooks[0].v, 4.0f / 1080.0f));
    Click(e, 3.0f, 1077.0f);
    CHECK(e.Strands()[1].hooks.size() == 11);  // 1072 px: 10 spans
    CHECK(Near(e.Strands()[1].hooks.back().v, 1076.0f / 1080.0f));
    Click(e, 1917.0f, 1077.0f);
    CHECK(e.Strands()[1].hooks.size() == 12 && Near(e.Strands()[1].slack.back(), kDefaultSlack));
    e.EndStrand();

    // Along the top: not a side run.
    Click(e, 100.0f, 3.0f);
    Click(e, 900.0f, 3.0f);
    CHECK(e.Strands()[2].hooks.size() == 2 && Near(e.Strands()[2].slack[0], kDefaultSlack));
    e.EndStrand();

    // A short run gets one span with the clips' slack.
    Click(e, 2.0f, 300.0f);
    Click(e, 2.0f, 380.0f);
    CHECK(e.Strands()[3].hooks.size() == 2 && Near(e.Strands()[3].slack[0], LightsEditor::kSideSlack));
    e.EndStrand();

    // The drape across the top counts as on the edges (its end hooks sit on them).
    LightsEditor top;
    top.Begin(Monitors(), Lights::AcrossTop(L"A", 1920, 1080, 1.0f));
    top.Move(1.0f, 7.0f);
    CHECK(top.Hover() == LightsEditor::Over::Hook && top.EditFor(0).hoverHook == 0);
}

void TestMonitors() {
    Section("a strand stays on its monitor");
    LightsEditor e;
    e.Begin(Monitors(), {});
    Click(e, 500.0f, 300.0f);
    e.Move(1920.0f + 600.0f, 300.0f);
    LightsEdit a = e.EditFor(0), b = e.EditFor(1);
    CHECK(a.cursorValid && (a.snap & 4) && Near(a.cursor.u, 1916.0f / 1920.0f) && Near(a.cursor.v, 300.0f / 1080.0f));
    CHECK(!b.cursorValid && b.strands.empty() && b.placing == -1);
    CHECK(e.PointerMonitor() == 1);
    Click(e, 1920.0f + 600.0f, 300.0f);
    CHECK(e.Strands()[0].monitor == L"A" && e.Strands()[0].hooks.size() == 2);
    CHECK(Near(e.Strands()[0].hooks[1].u, 1916.0f / 1920.0f));
    // Below monitor A (B reaches lower): clamped to A's bottom edge.
    e.Move(1000.0f, 1200.0f);
    CHECK(e.EditFor(0).cursorValid && (e.EditFor(0).snap & 8));
    e.EndStrand();

    // A strand started on B is B's, with local indices on B.
    Click(e, 1920.0f + 1000.0f, 500.0f);
    CHECK(e.Strands()[1].monitor == L"B");
    CHECK(e.EditFor(1).placing == 0 && e.EditFor(0).placing == -1);
    CHECK(e.EditFor(0).strands.size() == 1 && e.EditFor(1).strands.size() == 1);
    Click(e, 1920.0f + 1600.0f, 600.0f);
    e.EndStrand();
    CHECK(e.StrandCount() == 2);
}

void TestHover() {
    Section("hover: hooks and wires, as drawn");
    const LightHook h0 = {0.2f, 0.3f}, h1 = {0.6f, 0.3f};
    LightsEditor e;
    e.Begin(Monitors(), {Strand(L"A", {h0, h1}, {0.1f})});
    const D2D1_POINT_2F a = {h0.u * 1920.0f, h0.v * 1080.0f}, b = {h1.u * 1920.0f, h1.v * 1080.0f};
    const D2D1_POINT_2F low = Belly(a, b, 0.1f, 1.0f);
    CHECK(low.y > a.y + 50.0f);  // it does sag
    e.Move(low.x, low.y + 5.0f);
    CHECK(e.Hover() == LightsEditor::Over::Wire);
    CHECK(e.EditFor(0).hoverStrand == 0 && e.EditFor(0).hoverHook == -1);
    CHECK(Near(e.Sag(), 0.1f));
    CHECK(e.EditFor(0).cursorValid);  // a press there starts a new strand
    e.Move(low.x, low.y + 12.0f);
    CHECK(e.Hover() == LightsEditor::Over::Nothing && e.Sag() < 0.0f);
    // The straight line between the hooks is not where the wire is.
    e.Move(0.5f * (a.x + b.x), a.y);
    CHECK(e.Hover() == LightsEditor::Over::Nothing);
    e.Move(a.x + 6.0f, a.y);
    CHECK(e.Hover() == LightsEditor::Over::Hook && e.EditFor(0).hoverHook == 0);
    CHECK(!e.EditFor(0).cursorValid);  // over a hook a press grabs it
    e.Move(a.x, a.y - 9.0f);
    CHECK(e.Hover() == LightsEditor::Over::Hook);
    e.Move(a.x - 12.0f, a.y);
    CHECK(e.Hover() == LightsEditor::Over::Nothing);
    // While placing nothing is hovered.
    Click(e, 100.0f, 900.0f);
    e.Move(a.x + 2.0f, a.y);
    CHECK(e.Hover() == LightsEditor::Over::Nothing && e.EditFor(0).hoverStrand == -1);
    e.EndStrand();

    // On B the reach is in DIP: 8 DIP is 12 px at 150%.
    LightsEditor f;
    f.Begin(Monitors(), {Strand(L"B", {{0.2f, 0.3f}, {0.6f, 0.3f}}, {0.1f})});
    const D2D1_POINT_2F ba = {0.2f * 2560.0f, 0.3f * 1440.0f}, bb = {0.6f * 2560.0f, 0.3f * 1440.0f};
    const D2D1_POINT_2F blow = Belly(ba, bb, 0.1f, 1.5f);
    f.Move(1920.0f + blow.x, -200.0f + blow.y + 11.0f);
    CHECK(f.Hover() == LightsEditor::Over::Wire && f.EditFor(1).hoverStrand == 0);
    f.Move(1920.0f + blow.x, -200.0f + blow.y + 14.0f);
    CHECK(f.Hover() == LightsEditor::Over::Nothing);
    f.Leave();
    CHECK(!f.EditFor(1).cursorValid && f.PointerMonitor() == -1);
}

void TestDrag() {
    Section("dragging hooks");
    LightsEditor e;
    e.Begin(Monitors(), {Strand(L"A", {{0.25f, 0.25f}, {0.5f, 0.3f}, {0.75f, 0.25f}}, {0.05f, 0.05f})});
    const float hx = 0.5f * 1920.0f, hy = 0.3f * 1080.0f;
    e.Move(hx + 3.0f, hy + 2.0f);
    CHECK(e.Hover() == LightsEditor::Over::Hook && e.EditFor(0).hoverHook == 1);
    e.Press(hx + 3.0f, hy + 2.0f);
    CHECK(e.DraggingHook() && e.Hover() == LightsEditor::Over::Hook);
    // Grabbed off-centre: it moves with the pointer without jumping to it.
    e.Move(hx + 103.0f, hy + 52.0f);
    CHECK(Near(e.Strands()[0].hooks[1].u, (hx + 100.0f) / 1920.0f) &&
          Near(e.Strands()[0].hooks[1].v, (hy + 50.0f) / 1080.0f));
    CHECK(e.EditFor(0).cursorValid && e.EditFor(0).hoverHook == 1);
    e.Release(hx + 103.0f, hy + 52.0f);
    CHECK(!e.DraggingHook() && e.Strands()[0].hooks.size() == 3);
    CHECK(e.Undo() && Near(e.Strands()[0].hooks[1].u, 0.5f) && Near(e.Strands()[0].hooks[1].v, 0.3f));
    CHECK(e.Redo() && Near(e.Strands()[0].hooks[1].u, (hx + 100.0f) / 1920.0f));
    // Dragged to the edge it snaps; off the monitor it stays on it.
    const float nx = hx + 100.0f, ny = hy + 50.0f;
    e.Move(nx, ny);
    e.Press(nx, ny);
    e.Move(10.0f, ny);
    CHECK(Near(e.Strands()[0].hooks[1].u, 4.0f / 1920.0f));
    e.Move(-500.0f, 5000.0f);
    CHECK(Near(e.Strands()[0].hooks[1].u, 4.0f / 1920.0f) && Near(e.Strands()[0].hooks[1].v, 1076.0f / 1080.0f));
    // Esc puts it back.
    CHECK(e.CancelDrag());
    CHECK(Near(e.Strands()[0].hooks[1].u, nx / 1920.0f) && Near(e.Strands()[0].hooks[1].v, ny / 1080.0f));
    CHECK(!e.Pressed());
    // A press on a hook that doesn't move changes nothing (and adds no undo step).
    e.Move(nx, ny);
    e.Press(nx, ny);
    e.Release(nx, ny);
    CHECK(Near(e.Strands()[0].hooks[1].u, nx / 1920.0f));
    CHECK(e.Undo() && Near(e.Strands()[0].hooks[1].u, 0.5f));  // the first drag
    // Capture lost mid-drag: the hook stays where it got to.
    e.Move(hx, hy);
    e.Press(hx, hy);
    e.Move(hx + 40.0f, hy);
    e.CancelPress();
    CHECK(!e.Pressed() && Near(e.Strands()[0].hooks[1].u, (hx + 40.0f) / 1920.0f));
}

void TestWheel() {
    Section("wheel on spans and hooks");
    LightsEditor e;
    e.Begin(Monitors(), {Strand(L"A", {{0.2f, 0.3f}, {0.4f, 0.3f}, {0.6f, 0.3f}}, {0.1f, 0.2f})});
    const D2D1_POINT_2F a = {0.2f * 1920.0f, 0.3f * 1080.0f}, b = {0.4f * 1920.0f, 0.3f * 1080.0f};
    const D2D1_POINT_2F low = Belly(a, b, 0.1f, 1.0f);
    e.Move(low.x, low.y);
    CHECK(e.Hover() == LightsEditor::Over::Wire);
    e.Wheel(-3.0f);
    CHECK(Near(e.Strands()[0].slack[0], 0.13f) && Near(e.Strands()[0].slack[1], 0.2f));
    // The wire sagged away from the pointer; the wheel still works on it.
    e.Wheel(1.0f);
    CHECK(Near(e.Strands()[0].slack[0], 0.12f) && Near(e.Sag(), 0.12f));
    e.Wheel(-100.0f);
    CHECK(Near(e.Strands()[0].slack[0], kMaxSlack));
    // One undo step for the lot.
    CHECK(e.Undo() && Near(e.Strands()[0].slack[0], 0.1f));
    CHECK(e.Redo() && Near(e.Strands()[0].slack[0], kMaxSlack));
    CHECK(e.Undo());
    // A hook: both its spans.
    e.Move(b.x, b.y);
    CHECK(e.Hover() == LightsEditor::Over::Hook && Near(e.Sag(), 0.15f));
    e.Wheel(-1.0f);
    CHECK(Near(e.Strands()[0].slack[0], 0.11f) && Near(e.Strands()[0].slack[1], 0.21f));
    // An end hook: its one span.
    e.Move(a.x, a.y);
    e.Wheel(10.0f);
    CHECK(Near(e.Strands()[0].slack[0], 0.01f) && Near(e.Strands()[0].slack[1], 0.21f));
    e.Wheel(10.0f);
    CHECK(Near(e.Strands()[0].slack[0], 0.0f));
    // Over nothing: nothing.
    e.Move(1000.0f, 1000.0f);
    const std::vector<LightStrand> before = e.Strands();
    e.Wheel(-3.0f);
    e.Wheel(std::nanf(""));
    CHECK(e.Strands() == before);
}

void TestDelete() {
    Section("delete");
    LightsEditor e;
    e.Begin(Monitors(), {Strand(L"A", {{0.1f, 0.2f}, {0.3f, 0.25f}, {0.5f, 0.2f}}, {0.05f, 0.15f}),
                         Strand(L"A", {{0.1f, 0.6f}, {0.3f, 0.6f}}, {0.05f}),
                         Strand(L"A", {{0.6f, 0.6f}, {0.9f, 0.6f}}, {0.05f})});
    CHECK(!e.Delete());  // nothing under the pointer
    e.Move(0.3f * 1920.0f, 0.25f * 1080.0f);
    CHECK(e.Delete());
    CHECK(e.Strands()[0].hooks.size() == 2 && e.Strands()[0].slack.size() == 1);
    CHECK(Near(e.Strands()[0].slack[0], 0.1f, 1e-3f));  // the two spans' slack, by length (symmetric here)
    CHECK(Near(e.Strands()[0].hooks[1].u, 0.5f));
    // A hook of a two-hook strand takes the strand with it.
    e.Move(0.1f * 1920.0f, 0.6f * 1080.0f);
    CHECK(e.Delete() && e.Strands().size() == 2);
    // The wire: the whole strand.
    const D2D1_POINT_2F low = Belly({0.6f * 1920.0f, 0.6f * 1080.0f}, {0.9f * 1920.0f, 0.6f * 1080.0f}, 0.05f, 1.0f);
    e.Move(low.x, low.y);
    CHECK(e.Hover() == LightsEditor::Over::Wire);
    CHECK(e.Delete() && e.Strands().size() == 1);
    CHECK(e.Undo() && e.Strands().size() == 2);
    CHECK(e.Undo() && e.Strands().size() == 3);
    CHECK(e.Undo() && e.Strands()[0].hooks.size() == 3);
    CHECK(!e.Undo());
    // While placing: the last hook.
    e.Move(1000.0f, 900.0f);
    Click(e, 1000.0f, 900.0f);
    Click(e, 1300.0f, 900.0f);
    CHECK(e.Strands().back().hooks.size() == 2);
    CHECK(e.Delete() && e.Placing() && e.Strands().back().hooks.size() == 1);
    CHECK(e.Delete() && !e.Placing() && e.Strands().size() == 3);
}

void TestUndo() {
    Section("undo and redo");
    LightsEditor e;
    e.Begin(Monitors(), {});
    CHECK(!e.Undo() && !e.Redo());
    // While placing, undo takes the last hook and placing goes on.
    Click(e, 100.0f, 100.0f);
    Click(e, 400.0f, 120.0f);
    Click(e, 700.0f, 100.0f);
    CHECK(e.Undo() && e.Placing() && e.Strands()[0].hooks.size() == 2);
    CHECK(e.Redo() && e.Placing() && e.Strands()[0].hooks.size() == 3);
    e.EndStrand();
    // Once ended, undo takes the last hook and the strand stays ended.
    CHECK(e.Undo() && !e.Placing() && e.Strands()[0].hooks.size() == 2);
    // ... and a strand down to one hook goes.
    CHECK(e.Undo() && e.Strands().empty());
    CHECK(!e.Undo());
    CHECK(e.Redo());
    CHECK(e.Strands().size() == 1 && e.Strands()[0].hooks.size() == 2);
    // A new edit clears what could be redone.
    CHECK(e.Undo());
    Click(e, 1000.0f, 600.0f);
    CHECK(!e.Redo());
    e.EndStrand();

    // 100 deep.
    LightsEditor d;
    d.Begin(Monitors(), {});
    for (int i = 0; i < 150; ++i) Click(d, 100.0f + static_cast<float>(i % 2) * 300.0f, 100.0f + static_cast<float>(i) * 5.0f);
    CHECK(d.Strands()[0].hooks.size() == 150);
    int undone = 0;
    while (d.Undo()) ++undone;
    CHECK(undone == 100 && d.Strands()[0].hooks.size() == 50);
    int redone = 0;
    while (d.Redo()) ++redone;
    CHECK(redone == 100 && d.Strands()[0].hooks.size() == 150);
    // Full at kMaxHooks: the strand ends there.
    for (int i = 150; i < 200 && d.Placing(); ++i)
        Click(d, 100.0f + static_cast<float>(i % 2) * 300.0f, 100.0f + static_cast<float>(i) * 5.0f);
    CHECK(!d.Placing() && d.Strands()[0].hooks.size() == static_cast<size_t>(kMaxHooks));
    CHECK(d.Strands()[0].slack.size() == static_cast<size_t>(kMaxHooks) - 1);
}

void TestClearAndFinish() {
    Section("clear all, finish, other monitors");
    LightsEditor e;
    e.Begin(Monitors(), {Strand(L"A", {{0.1f, 0.1f}, {0.5f, 0.1f}}, {0.05f}),
                         Strand(L"Z", {{0.1f, 0.1f}, {0.9f, 0.1f}}, {0.05f}),
                         Strand(L"B", {{0.1f, 0.1f}, {0.9f, 0.1f}}, {0.05f})});
    CHECK(e.StrandCount() == 2);
    CHECK(e.EditFor(0).strands.size() == 1 && e.EditFor(1).strands.size() == 1);
    CHECK(e.ClearAll());
    CHECK(e.Strands().size() == 1 && e.Strands()[0].monitor == L"Z");
    CHECK(!e.ClearAll());
    CHECK(e.Undo() && e.Strands().size() == 3);
    // Finish keeps a strand being placed with two hooks, drops one with one.
    Click(e, 300.0f, 500.0f);
    Click(e, 700.0f, 500.0f);
    std::vector<LightStrand> out = e.Finish();
    CHECK(out.size() == 4 && !e.Placing());
    bool valid = true;
    for (const LightStrand& s : out) valid = valid && s.hooks.size() >= 2 && s.slack.size() == s.hooks.size() - 1;
    CHECK(valid);
    Click(e, 300.0f, 700.0f);
    CHECK(e.Finish().size() == 4);
    CHECK(!e.EditFor(2).active && !e.EditFor(-1).active);
}

void TestRobustness() {
    Section("robustness");
    LightsEditor e;
    e.Begin({}, {Strand(L"A", {{0.1f, 0.1f}, {0.5f, 0.1f}}, {0.05f})});
    e.Move(100.0f, 100.0f);
    Click(e, 100.0f, 100.0f);
    CHECK(!e.Placing() && !e.EditFor(0).active && e.Finish().size() == 1);
    // Bad input from settings is cleaned up; bad pointer input is ignored.
    e.Begin(Monitors(), {Strand(L"A", {{std::nanf(""), 2.0f}, {0.5f, -1.0f}}, {}),
                         Strand(L"A", {{0.5f, 0.5f}}, {}),
                         Strand(L"A", {{0.1f, 0.1f}, {0.2f, 0.1f}}, {std::nanf("")})});
    CHECK(e.Strands().size() == 2);
    CHECK(e.Strands()[0].slack.size() == 1 && Near(e.Strands()[0].hooks[0].v, 1.0f) && Near(e.Strands()[0].hooks[1].v, 0.0f));
    CHECK(std::isfinite(e.Strands()[1].slack[0]));
    e.Move(500.0f, 500.0f);
    e.Move(std::nanf(""), 3.0f);
    CHECK(!e.EditFor(0).cursorValid);
    Click(e, std::nanf(""), 3.0f);
    CHECK(!e.Placing() && e.Strands().size() == 2);
    // A degenerate monitor doesn't divide by zero.
    LightsEditor z;
    z.Begin({{L"Q", 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}}, {});
    Click(z, 0.0f, 0.0f);
    const LightsEdit q = z.EditFor(0);
    CHECK(q.active && std::isfinite(q.cursor.u) && std::isfinite(q.cursor.v));
    // Hooks never leave their monitor.
    LightsEditor f;
    f.Begin(Monitors(), {});
    Click(f, 960.0f, 540.0f);
    for (float x : {-9000.0f, 9000.0f, 1919.5f, 0.0f})
        for (float y : {-9000.0f, 9000.0f, 1079.5f, 0.0f}) {
            f.Move(x, y);
            const LightsEdit a = f.EditFor(0);
            CHECK(a.cursorValid && a.cursor.u >= 0.0f && a.cursor.u <= 1.0f && a.cursor.v >= 0.0f && a.cursor.v <= 1.0f);
        }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring hudOut;
    float hudScale = 1.5f;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--hud" && i + 1 < argc) hudOut = argv[++i];
        else if (a == L"--hud-scale" && i + 1 < argc) hudScale = static_cast<float>(_wtof(argv[++i]));
    }

    TestSnapping();
    TestPlacing();
    TestSideRuns();
    TestMonitors();
    TestHover();
    TestDrag();
    TestWheel();
    TestDelete();
    TestUndo();
    TestClearAndFinish();
    TestRobustness();
    printf("%d checks, %d failed\n", g_checks, g_failures);

    if (!hudOut.empty()) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const HRESULT hr = LightsHud::SaveSnapshot(hudOut.c_str(), hudScale);
        printf("HUD snapshot %ls: %s (0x%08X)\n", hudOut.c_str(), SUCCEEDED(hr) ? "written" : "failed",
               static_cast<unsigned>(hr));
        if (FAILED(hr)) ++g_failures;
        if (SUCCEEDED(co)) CoUninitialize();
    }
    return g_failures == 0 ? 0 : 1;
}
