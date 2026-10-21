// Peggle 2's PS4-only content: the game engine as the two powers use it (resources by name, animations, particle
// effects, drawing, style shots). Each function is the call sequence of a place in the Xbox 360 executable that does
// the same; the place is named in the comment (addresses of the recompiled listing, app/generated/default).
#include <algorithm>
#include <cmath>
#include <cstring>

#include <rex/logging.h>

#include "dlc/peggle1_dlc_game.h"

namespace peggle1::dlc {
namespace {
constexpr uint32_t kApp = 0x82D8D748;  // the application object's pointer; +1760 resource manager, +2248 art lines
}

// ---- resources ----
// 821675C8: type, lookup with r7 = 1 (a missing id is an empty reference, not an exception), copy the reference.
uint32_t Resource::Reference(const Guest& g) {
  if (tried) return reference;
  tried = true;
  const uint32_t slot = Permanent(g, 8);
  if (!slot) return 0;
  Frame frame(g, 160);
  const uint32_t temporary = frame.Alloc(8);
  const uint32_t kind = g.Call(type);
  const uint32_t manager = g.u32(g.u32(kApp) + 1760);
  uint64_t value;
  {
    GuestString text(g, frame, id);
    const uint32_t found = g.Call(__imp__sub_8238A088, {temporary, manager, kind, text.address(), 1});
    value = g.u64(found);
  }
  g.Call(__imp__sub_8214FC30, {slot, value});
  g.Call(__imp__sub_8214FC30, {temporary, 0});
  if (!value) {
    REXLOG_WARN("[dlc] the archive has no resource {} (stock Xbox 360 archive?): what uses it is left out", id);
    return 0;
  }
  reference = slot;
  return reference;
}

// The same lookup without keeping the reference: is there a resource of that id and type.
bool HasResource(const Guest& g, PPCFunc* type, std::string_view id) {
  Frame frame(g, 224);
  const uint32_t temporary = frame.Alloc(8);
  const uint32_t kind = g.Call(type);
  const uint32_t manager = g.u32(g.u32(kApp) + 1760);
  uint64_t value;
  {
    GuestString text(g, frame, id);
    const uint32_t found = g.Call(__imp__sub_8238A088, {temporary, manager, kind, text.address(), 1});
    value = g.u64(found);
  }
  g.Call(__imp__sub_8214FC30, {temporary, 0});
  return value != 0;
}

uint32_t PopAnimOf(const Guest& g, uint32_t reference) {
  return reference ? g.Call(__imp__sub_8215A1C0, {reference}) : 0;
}
uint32_t EffectOf(const Guest& g, uint32_t reference) {
  return reference ? g.Call(__imp__sub_82153120, {reference}) : 0;
}
uint32_t ImageOf(const Guest& g, uint32_t reference) {  // 822BAF30
  if (!reference) return 0;
  const uint32_t manager = g.Call(__imp__sub_82399DC0);
  const uint32_t image = g.Call(__imp__sub_8214F248, {manager, reference});
  if (!image) return 0;
  return (g.Call(__imp__sub_82153480, {image}) & 0xFF) ? image : 0;
}

// ---- animations ----
uint32_t NewAnim(const Guest& g, uint32_t popanim) {  // 822A9D98
  if (!popanim) return 0;
  const uint32_t memory = g.Call(__imp__sub_82369B48, {212});
  return memory ? g.Call(__imp__sub_8218CF10, {memory, 0, popanim}) : 0;
}
void DeleteAnim(const Guest& g, uint32_t anim) {  // 8222B330: its deleting destructor is slot 9
  if (anim) g.Virtual(anim, 36, {1});
}
bool PlayAnim(const Guest& g, uint32_t anim, std::string_view label, bool loop) {
  if (!anim) return false;
  Frame frame(g, 128);
  const uint32_t options = frame.Alloc(8);  // {int frame offset; bool loop; bool reset}
  g.set8(options + 4, loop ? 1 : 0);
  g.set8(options + 5, 1);
  GuestString text(g, frame, label);
  const uint32_t played = g.Call(__imp__sub_8218D7C8, {anim, text.address(), options});
  return (played & 0xFF) != 0;
}
void TickAnim(const Guest& g, uint32_t anim) {  // 822A9B0C
  if (anim) g.Virtual(anim, 40);
}
void AdvanceAnim(const Guest& g, uint32_t anim, float dt) {  // 822A9BE0
  if (!anim) return;
  g.Floats(double(dt));
  g.Virtual(anim, 44);
}
void DrawAnim(const Guest& g, uint32_t anim, uint32_t graphics, float x, float y) {  // 822A9A80, 822A9C40
  if (!anim) return;
  const uint32_t popanim = g.u32(anim + 152);
  if (!popanim) return;
  g.set32(popanim + 68, uint32_t(int32_t(x)));
  g.set32(popanim + 72, uint32_t(int32_t(y)));
  g.Call(__imp__sub_8218D2A8, {anim, graphics});
}

// ---- particle effects ----
void SpawnEffect(const Guest& g, uint32_t simulation, uint32_t effect, float x, float y, float scale) {  // 8215EA5C
  if (!effect) return;
  const uint32_t adapter = g.u32(simulation + 60);
  const uint32_t instance = g.Virtual(adapter, 84, {effect});
  if (!instance) return;  // effects are switched off
  g.setf(instance + 3044, x);
  g.setf(instance + 3048, y);
  g.setf(instance + 3056, scale);
}
uint32_t CloneEffect(const Guest& g, uint32_t effect) {  // 822BB4B8 (TeslaCoil's constructor)
  if (!effect) return 0;
  const uint32_t instance = g.Call(__imp__sub_823B54F0, {effect});
  if (instance) g.set32(instance + 3032, 0);
  return instance;
}
void DeleteEffect(const Guest& g, uint32_t effect) {  // 821C5F78
  if (effect) g.Virtual(effect, 8, {1});
}
void DrawEffect(const Guest& g, uint32_t effect, uint32_t graphics, float x, float y, float scale) {  // 822BDDE8
  if (!effect) return;
  const float s = scale * ArtScale(g);
  const float matrix[9] = {s, 0, x, 0, s, y, 0, 0, 1};
  for (int i = 0; i < 9; ++i) g.setf(effect + 2952 + 4 * i, matrix[i]);
  g.Call(__imp__sub_823A6708, {effect, graphics});
}

// ---- the board on the screen ----
Geometry BoardGeometry(const Guest& g, uint32_t simulation) {  // 822BDDE8, 8215EA5C
  Geometry out;
  const uint32_t adapter = g.u32(simulation + 60);
  if (!adapter) return out;
  if (const uint32_t rect = g.Virtual(adapter, 4)) {
    out.x = g.i32(rect), out.y = g.i32(rect + 4), out.w = g.i32(rect + 8), out.h = g.i32(rect + 12);
  }
  Frame frame(g, 16);
  const uint32_t buffer = frame.Alloc(8);
  g.ctx.r3.u64 = buffer;  // a returned Vec2: the result's address first, then the object
  g.ctx.r4.u64 = adapter;
  g.Lookup(g.u32(g.u32(adapter) + 8))(g.ctx, g.base);
  const uint32_t origin = g.ctx.r3.u32;
  if (Guest::Plausible(origin)) out.origin_x = g.f32(origin), out.origin_y = g.f32(origin + 4);
  out.scale = g.f32(adapter + 256);
  return out;
}
float ArtScale(const Guest& g) { return float(double(g.i32(g.u32(kApp) + 2248)) * 0.000434028); }

// ---- drawing ----
void SetColor(const Guest& g, uint32_t graphics, int r, int green, int b, int a) {
  Frame frame(g, 16);
  const uint32_t color = frame.Alloc(16);
  g.set32(color, uint32_t(r));
  g.set32(color + 4, uint32_t(green));
  g.set32(color + 8, uint32_t(b));
  g.set32(color + 12, uint32_t(a));
  g.Call(__imp__sub_823BE5A8, {graphics, color});
}
void FillRect(const Guest& g, uint32_t graphics, int x, int y, int w, int h) {
  g.Call(__imp__sub_823BF748, {graphics, uint64_t(int64_t(x)), uint64_t(int64_t(y)), uint64_t(int64_t(w)),
                               uint64_t(int64_t(h))});
}
void DrawQuad(const Guest& g, uint32_t graphics, uint32_t image, const float (&corners)[4][2]) {  // 822BB180
  if (!image) return;
  Frame frame(g, 6 * 32);
  const uint32_t vertices = frame.Alloc(6 * 32);  // {x, y, z = 0, w = 1, colour, 0, u, v}
  static constexpr int kOrder[6] = {0, 1, 2, 2, 3, 0};
  static constexpr float kUV[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  for (int i = 0; i < 6; ++i) {
    const uint32_t v = vertices + 32 * i;
    g.setf(v, corners[kOrder[i]][0]);
    g.setf(v + 4, corners[kOrder[i]][1]);
    g.setf(v + 12, 1.0f);
    g.setf(v + 24, kUV[kOrder[i]][0]);
    g.setf(v + 28, kUV[kOrder[i]][1]);
  }
  g.Call(__imp__sub_823BDD98, {graphics, image, vertices, 2});
}

// ---- curves ----
uint32_t NewCurve(const Guest& g, std::string_view data) {  // 82156D90: the constructor with the data string
  const uint32_t curve = Permanent(g, 192);
  if (!curve) return 0;
  Frame frame(g, 192);
  GuestString text(g, frame, data);
  g.Call(__imp__sub_823E6680, {curve, text.address(), 0});
  return curve;
}
float CurveAt(const Guest& g, uint32_t curve, float input) {  // 82156EEC
  if (!curve) return input;
  g.ctx.f1.f64 = double(input);
  g.Call(__imp__sub_823E5EC8, {curve});
  return float(g.ctx.f1.f64);
}

// ---- overlay ----
// The PS4 draws these layers as one image through a pixel shader. Here the same alpha is computed at the vertices of
// a mesh over the rectangle and interpolated by the rasteriser: kCellsX x kCellsY cells, each halved in both
// directions (up to kDepth times) while a hole's edge or a beam runs through it and the alpha at the middle of the
// cell or of one of its sides is more than kTolerance off what its corners would interpolate. Two triangles a cell;
// cells whose alpha is 0 at every corner are left out. The vertices go into one buffer of the game's heap, made at
// the first use. Measured on the PC (peggle1_dlc_log, "overlay"): Jimmy's two beams and three balls about 1,100
// triangles, Windy's screen with four fairies about 500.
namespace {
constexpr int kCellsX = 12, kCellsY = 8, kDepth = 3;
constexpr float kTolerance = 0.03f;
constexpr uint32_t kMaxTriangles = 3200;
constexpr uint32_t kVertexBytes = 32;  // {x, y, z, w, colour, 0, u, v}   [822BB180]
uint32_t g_overlay_vertices = 0;

struct OverlayEval {
  const Overlay& o;
  float tan_half = 0, inv_width = 0, inv_edge = 0, reach = 0;
  float axis[2][2] = {};
  explicit OverlayEval(const Overlay& overlay) : o(overlay) {
    inv_edge = o.edge > 0 ? 0.5f / o.edge : 0.0f;
    reach = o.size + 2.0f * o.edge;
    if (o.spots) {
      const float sw = std::clamp(o.spot_width, 0.01f, 0.9999f);
      tan_half = std::sqrt(1.0f - sw * sw) / sw;
      inv_width = 1.0f / (1.0f - sw);
      for (int i = 0; i < 2; ++i) {
        const float x = o.aim[i][0] - o.anchor[i][0], y = o.aim[i][1] - o.anchor[i][1];
        const float length = std::hypot(x, y);
        axis[i][0] = length > 0 ? x / length : 0.0f;
        axis[i][1] = length > 0 ? y / length : -1.0f;
      }
    }
  }
  // The layer's alpha at (u, v) of the rectangle, 0..1 (before the vertex colour's own alpha).
  float Alpha(float u, float v) const {
    float punch = 1.0f;
    for (int i = 0; i < o.points; ++i) {
      const float d = std::hypot(u - o.point[i][0], v - o.point[i][1]);
      punch = std::min(punch, (d - o.size) * inv_edge);
    }
    punch = std::clamp(punch * o.alpha, 0.0f, 1.0f);
    if (!o.spots) return punch;
    float spot = 1.0f;
    for (int i = 0; i < 2; ++i) {
      const float x = u - o.anchor[i][0], y = v - o.anchor[i][1];
      const float length = std::hypot(x, y);
      if (length <= 0) continue;
      const float c = std::fabs((x * axis[i][0] + y * axis[i][1]) / length);
      if (c > o.spot_width) spot = std::min(spot, (1.0f - c) * inv_width);
    }
    spot = std::clamp(spot * o.alpha, 0.0f, 1.0f);
    return o.points ? 1.0f - (1.0f - spot) * (1.0f - punch) : spot;
  }
  // Does the alpha change inside the cell with this centre and half-size?
  bool Varies(float cu, float cv, float hu, float hv) const {
    for (int i = 0; i < o.points; ++i) {
      const float dx = std::fabs(cu - o.point[i][0]), dy = std::fabs(cv - o.point[i][1]);
      const float near_x = std::max(dx - hu, 0.0f), near_y = std::max(dy - hv, 0.0f);
      if (near_x * near_x + near_y * near_y >= reach * reach) continue;  // outside this hole and its edge
      const float far_x = dx + hu, far_y = dy + hv;
      if (far_x * far_x + far_y * far_y > o.size * o.size) return true;  // not wholly inside the hole
    }
    if (o.spots) {
      const float radius = std::hypot(hu, hv);
      for (int i = 0; i < 2; ++i) {
        const float x = cu - o.anchor[i][0], y = cv - o.anchor[i][1];
        const float along = std::fabs(x * axis[i][0] + y * axis[i][1]);
        const float across = std::fabs(x * axis[i][1] - y * axis[i][0]);
        if (across <= (along + radius) * tan_half + radius * 1.02f) return true;
      }
    }
    return false;
  }
};

struct OverlayWriter {
  const Guest& g;
  const OverlayEval& eval;
  uint32_t next, triangles = 0;
  uint32_t rgb;
  float w, h;
  void Vertex(float u, float v, float alpha) {
    g.setf(next, u * w);
    g.setf(next + 4, v * h);
    g.set32(next + 8, 0);
    g.setf(next + 12, 1.0f);
    g.set32(next + 16, (uint32_t(alpha * 255.0f + 0.5f) << 24) | rgb);
    g.set32(next + 20, 0);
    g.setf(next + 24, eval.o.one_texel ? 0.5f : u);
    g.setf(next + 28, eval.o.one_texel ? 0.5f : v);
    next += kVertexBytes;
  }
  // The cell (u0, v0)-(u1, v1) with the alpha at its corners (a = top left, b = top right, c = bottom right, d).
  void Cell(float u0, float v0, float u1, float v1, float a, float b, float c, float d) {
    if ((a <= 0 && b <= 0 && c <= 0 && d <= 0) || triangles + 2 > kMaxTriangles) return;
    Vertex(u0, v0, a), Vertex(u1, v0, b), Vertex(u1, v1, c);
    Vertex(u1, v1, c), Vertex(u0, v1, d), Vertex(u0, v0, a);
    triangles += 2;
  }
  // The same cell, in four where its corners do not describe it (see the top of this part).
  void Split(float u0, float v0, float u1, float v1, float a, float b, float c, float d, int depth) {
    const float um = (u0 + u1) * 0.5f, vm = (v0 + v1) * 0.5f;
    if (depth < kDepth && eval.Varies(um, vm, um - u0, vm - v0)) {
      const float top = eval.Alpha(um, v0), right = eval.Alpha(u1, vm), bottom = eval.Alpha(um, v1);
      const float left = eval.Alpha(u0, vm), mid = eval.Alpha(um, vm);
      const auto off = [](float value, float expected) { return std::fabs(value - expected) > kTolerance; };
      if (off(top, (a + b) * 0.5f) || off(right, (b + c) * 0.5f) || off(bottom, (c + d) * 0.5f) ||
          off(left, (d + a) * 0.5f) || off(mid, (a + b + c + d) * 0.25f)) {
        Split(u0, v0, um, vm, a, top, mid, left, depth + 1);
        Split(um, v0, u1, vm, top, b, right, mid, depth + 1);
        Split(um, vm, u1, v1, mid, right, c, bottom, depth + 1);
        Split(u0, vm, um, v1, left, mid, bottom, d, depth + 1);
        return;
      }
    }
    Cell(u0, v0, u1, v1, a, b, c, d);
  }
};
}  // namespace

void DrawOverlay(const Guest& g, uint32_t graphics, const Overlay& o) {
  if (!o.image || o.w <= 0 || o.h <= 0 || o.alpha <= 0) return;
  if (!g_overlay_vertices) g_overlay_vertices = Permanent(g, kMaxTriangles * 3 * kVertexBytes);
  if (!g_overlay_vertices) return;
  const OverlayEval eval(o);
  // A vertex colour of 0 means "the Graphics colour" to the game: the blue byte is never 0 here.
  OverlayWriter out{g, eval, g_overlay_vertices, 0,
                    (uint32_t(o.r & 0xFF) << 16) | (uint32_t(o.green & 0xFF) << 8) | uint32_t(std::max(o.b, 1) & 0xFF),
                    float(o.w), float(o.h)};
  const float du = 1.0f / kCellsX, dv = 1.0f / kCellsY;
  float row[kCellsX + 1], below[kCellsX + 1];  // the alpha at the grid's corners, two rows at a time
  for (int x = 0; x <= kCellsX; ++x) row[x] = eval.Alpha(x * du, 0.0f);
  for (int y = 0; y < kCellsY; ++y) {
    const float v0 = y * dv, v1 = (y + 1) * dv;
    for (int x = 0; x <= kCellsX; ++x) below[x] = eval.Alpha(x * du, v1);
    for (int x = 0; x < kCellsX; ++x) {
      out.Split(x * du, v0, (x + 1) * du, v1, row[x], row[x + 1], below[x + 1], below[x], 0);
    }
    std::memcpy(row, below, sizeof(row));
  }
  if (Logging()) {
    static int logged = 0;
    if ((logged++ % 300) == 0) REXLOG_INFO("[dlc] overlay: {} triangles ({} points, beams {})", out.triangles, o.points, o.spots);
  }
  if (!out.triangles) return;
  const uint8_t colorize = g.u8(graphics + kGraphicsColorize);
  g.set8(graphics + kGraphicsColorize, 1);
  SetColor(g, graphics, o.r, o.green, o.b, 255);
  // The game's triangle function (824ABFD0) cuts every triangle to the Graphics' clip rectangle (+32) with a polygon
  // clipper unless that rectangle is the whole target (0, 0, the interface's +56 x +60). The mesh lies inside the
  // board, so for this call the clip rectangle is the whole target.
  const uint32_t target = g.u32(graphics + 612);
  uint32_t clip[4];
  for (int i = 0; i < 4; ++i) clip[i] = g.u32(graphics + 32 + 4 * i);
  if (Guest::Plausible(target)) {
    g.set32(graphics + 32, 0), g.set32(graphics + 36, 0);
    g.set32(graphics + 40, g.u32(target + 56)), g.set32(graphics + 44, g.u32(target + 60));
  }
  g.Call(__imp__sub_823BDD98, {graphics, o.image, g_overlay_vertices, out.triangles});
  for (int i = 0; i < 4; ++i) g.set32(graphics + 32 + 4 * i, clip[i]);
  g.set8(graphics + kGraphicsColorize, colorize);
}

// ---- rules ----
bool IsMultiplayer(const Guest& g, uint32_t self) {  // 822BD96C: true selects mUltraShockPointsMP
  return (g.Virtual(g.u32(self + power::kRules), 40) & 0xFF) != 0;
}
int GameState(const Guest& g, uint32_t self) {
  // The rules interface the power holds is the one at +4 of the single-player GameRules (its vtable 820D6728); the
  // state is the field its "in play" query tests for 2 or 6 (821BC2B0), the same field 821CD954 tests for 2.
  const uint32_t rules = g.u32(self + power::kRules);
  constexpr uint32_t kGameRulesInterface = 0x820D6728;
  return rules && g.u32(rules) == kGameRulesInterface ? g.i32(rules + 1032) : -1;
}

void TestOnBallFired(const Guest& g, uint32_t self) {
  const bool greens = Test("greens"), one_orange = Test("oneorange") || Test("firstorange");
  if (!greens && !one_orange) return;
  static uint32_t done_for = 0;  // once per power object (a level)
  if (done_for == self) return;
  done_for = self;
  const uint32_t simulation = Simulation(g, self);
  const std::vector<uint32_t> targets = Pointers(g, simulation + sim::kTargets);
  // The orange peg that stays: the one closest to straight below the cannon (a shot fired without aiming).
  const Geometry board = BoardGeometry(g, simulation);
  const float centre = board.scale > 0 ? float(board.w) / board.scale * 0.5f : 0.0f;
  uint32_t keep = 0;
  float best = 1e9f;
  for (const uint32_t t : targets) {
    if (g.i32(t + target::kType) != 1) continue;
    const float d = std::fabs(g.f32(t + 20) - centre) * 4.0f + g.f32(t + 24);
    if (d < best) best = d, keep = t;
  }
  int made = 0, taken = 0, seen = 0;
  for (const uint32_t t : targets) {
    const int type = g.i32(t + target::kType);
    if (greens && type == 0 && made < 14 && (seen++ % 2) == 0) {
      g.set32(t + target::kType, 3);
      ++made;
    } else if (one_orange && type == 1 && t != keep) {
      g.set32(t + target::kType, 0);
      ++taken;
    }
  }
  if (Test("firstorange") && keep) {
    // The one orange peg left counts as hit (state 2, as a ball's hit leaves it): the level is won when this ball
    // is gone, whatever it meets. For runs that need a finished level in the save.
    g.set32(keep + target::kState, 2);
  }
  REXLOG_INFO("[dlc] TEST: {} blue pegs made green, {} orange pegs made blue", made, taken);
}

void AwardStyleShot(const Guest& g, uint32_t self, std::string_view name, int points, float world_x, float world_y) {
  const uint32_t list = g.u32(g.u32(self + power::kRules) + 608);  // 822BD990
  if (!list) return;
  Frame frame(g, 192);
  const uint32_t award = frame.Alloc(48);
  {
    GuestString text(g, frame, name);
    g.Call(__imp__sub_821D4E18, {award, g.Pack(world_x, world_y), text.address(), uint64_t(int64_t(points))});
  }
  g.Call(__imp__sub_821D4F30, {list, award});
  if (Logging()) REXLOG_INFO("[dlc] style shot {}: {} points", name, points);
}

}  // namespace peggle1::dlc
