// Peggle 2's PS4-only content: Windy's power, class PurplePegs ("Fairy Flock").
//
// The Xbox 360 archive ships PACKAGES\PURPLEPEGS.RTON for a class the executable does not have. This is that class,
// written from the PS4 executable (class PurplePegs: constructor 0x1D0200, properties 0x1CCB20; the addresses below
// are that executable's; listing out/ps4_dlc/ps4_powers/purplepegs.lite.asm, not in the repository).
//
// WHAT IT DOES (PS4):
//   The power is always on (state 2, never counted in turns). Every green peg hit in normal play calls DoPower
//   (0x1D2210): the board slows to a stop over 0.5 s (time scale curve A), mCfgNumPurplePegs fairies (4) appear
//   around the green peg one after the other (0x1D2390: delay i/N*0.35 s, on a circle), each waits 0.35 s, picks a
//   random blue peg that may become purple and that no other fairy has (0x1D49C0, shuffled with the game's random
//   numbers), flies to it (0x1D6390: at least 1 s, longer with the distance) and turns it purple on arrival
//   (0x1D2AD0: armour broken first; sound Play_PurplePeg_Fairy_Spawn). When every fairy has arrived the board
//   speeds up again over 0.5 s. A fairy peg stays purple until it is hit: the per-turn purple draw takes back only
//   purples that carry flag bits 0, 1 or 3, which the conversion clears (360: 822523A8, the same test). Hitting one
//   (0x1D17D0) plays Play_PurplePeg_Fairy_Hit, sends its fairy away and counts towards two style shots: "Big Purps"
//   (mCfgStyleShotLotsOfPurpsSingleShot fairy pegs in one shot) and "Fairy Fiesta" (...MultiShot in a level).
//
// HOW IT SITS IN THE 360 GAME: a child class of MasterPower (822B78F8), 53 virtual functions copied from the base
// class's table with these replaced:
//   0 class, 1 "abstract" = false, 2 destructor, 11 any target hit, 18 time scale (the rules copy it into the
//   simulation every tick), 19 tick, 27 ball fired, 44 the power itself (the base's green-peg handler 822B7B68 calls
//   it for every green peg while the state is not 0 and the repeat flag +49 is set).
//   Its renderer (22 virtual functions, a copy of the base renderer's table): 6 busy (the rules wait for it before
//   the turn goes on, 82252678), 14 after the background: the board's dimming, 15 after the balls: the fairies,
//   20 tick and 21 frame: the fairies' animations.
//
// WHAT IS DRAWN: each fairy is the PS4's animation POPANIM_MASTERS_WENDY_BUTTER_FLY (labels fly, idle, out; converted
// into the 360 archive's Masters_Wendy_Shared group by tools/ps4_dlc) with the game's PIEFFECT_PI_FAIRY_TRAIL behind
// it; PIEFFECT_PI_PURPLE_SPARKLES on the green peg; the board darkens by curve B (a black rectangle, alpha B * 100
// of 255, PS4 0x1D5550). NOT DRAWN YET: the PS4's ultraviolet screen (IMAGE_MASTERS_WENDY_ULTRAVIOLET_SCREEN through
// the shaders FAIRY_MAGIC and FAIRY_PUNCHOUT, 0x1D4F90 / 0x1D3D90).
//   The 360's base deactivates a power at the end of a turn when mTurns >= 0 and no turns are left (82252D50), state
//   2 included; the PS4's leaves state 2 alone. So mTurns is held at -1 here ("permanent" in the 360's terms).
//
// STATE: the game's objects hold only the reflected properties; fairies and timers are host objects, one set per
// power object, found by its address.
#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>

#include "dlc/peggle1_dlc_game.h"

namespace peggle1::dlc {
namespace {

// The object: MasterPower (136 bytes), the renderer's vtable, the properties.
constexpr uint32_t kRendererAt = 136;
constexpr uint32_t kCfgNumPurplePegs = 140;
constexpr uint32_t kCfgSingleShot = 144, kCfgSingleShotPts = 148, kCfgSingleShotPtsMP = 152;
constexpr uint32_t kCfgMultiShot = 156, kCfgMultiShotPts = 160, kCfgMultiShotPtsMP = 164;
constexpr uint32_t kNumPurplePegs = 168;  // a key of the 360's data file that the PS4 class no longer has
constexpr uint32_t kObjectSize = 184;

// The PS4 class's properties (0x1CCB20), all uint32 there, and the 360 file's extra key.
constexpr Property kProperties[] = {
    {"mCfgNumPurplePegs", kUint, 4, kCfgNumPurplePegs},
    {"mCfgStyleShotLotsOfPurpsSingleShot", kUint, 4, kCfgSingleShot},
    {"mCfgStyleShotLotsOfPurpsSingleShotPts", kUint, 4, kCfgSingleShotPts},
    {"mCfgStyleShotLotsOfPurpsSingleShotPtsMP", kUint, 4, kCfgSingleShotPtsMP},
    {"mCfgStyleShotLotsOfPurpsMultiShot", kUint, 4, kCfgMultiShot},
    {"mCfgStyleShotLotsOfPurpsMultiShotPts", kUint, 4, kCfgMultiShotPts},
    {"mCfgStyleShotLotsOfPurpsMultiShotPtsMP", kUint, 4, kCfgMultiShotPtsMP},
    {"mNumPurplePegs", kUint, 4, kNumPurplePegs},
};

constexpr float kTick = 1.0f / 60.0f;  // the PS4 code counts every timer in 1/60 s per power tick

// The PS4 code's curves, as the data strings it gives the framework's CurvedVal. The 360 executable has the same
// class, so the game itself evaluates them (made when the first power object is: the class has static data).
constexpr char kCurveDown[] = "b;0,1,0.01,1,~jGq    K0v+S     U#Ls]";  // 0x1D0580: time scale 1 -> 0, dimming 1 -> 0
constexpr char kCurveUp[] = "b;0,1,0.01,1,#}(*        jq3O@ 7}NoU";    // 0x1D0580: 0 -> 0.989
constexpr char kCurvePathTime[] = "b;0,1,0.01,1,#-#D         ~~QPU";   // 0x1D5A89: a fairy's progress on its path
constexpr char kCurveKick[] = "b;0,1,0.01,1,#2{3         ~~az_";       // 0x1D6529: a fairy knocked away by a ball
struct GameCurves {
  bool made = false;
  uint32_t down = 0, up = 0, path_time = 0, kick = 0;
} g_curves;
void MakeCurves(const Guest& g) {
  if (g_curves.made || !Motion()) return;
  g_curves.made = true;
  g_curves.down = NewCurve(g, kCurveDown);
  g_curves.up = NewCurve(g, kCurveUp);
  g_curves.path_time = NewCurve(g, kCurvePathTime);
  g_curves.kick = NewCurve(g, kCurveKick);
}

// One of the two curves played over `duration` seconds; a constant before and after.
struct Curve {
  float duration = 0, time = 0, constant = 1;
  bool running = false, up = false;
  void Set(float value) {
    constant = value;
    running = false;
  }
  void Down(float seconds) { duration = seconds, time = 0, up = false, running = true; }
  void Up(float seconds) { duration = seconds, time = 0, up = true, running = true; }
  void Advance(float dt) {
    if (!running) return;
    time += dt;
    if (time >= duration) {
      running = false;
      constant = up ? 0.989f : 0.0f;  // the curves' last points
    }
  }
  float Value(const Guest& g) const {
    if (!running) return constant;
    const float t = duration > 0 ? std::min(1.0f, time / duration) : 1.0f;
    if (const uint32_t curve = up ? g_curves.up : g_curves.down) return CurveAt(g, curve, t);
    // Without the game's curves (peggle1_dlc_motion off): the strings' three decoded points, eased between.
    const float t1 = up ? 0.878f : 0.444f, y0 = up ? 0.0f : 1.0f, y1 = up ? 0.856f : 0.144f, y2 = up ? 0.989f : 0.0f;
    const auto ease = [](float x) { return x * x * (3.0f - 2.0f * x); };
    return t < t1 ? y0 + (y1 - y0) * ease(t / t1) : y1 + (y2 - y1) * ease((t - t1) / (1.0f - t1));
  }
};

// The framework's Bezier (PS4 0x3647E0, 0x364F90): cubic segments through points, each with two control points and
// a time.
struct Path {
  std::vector<float> time, px, py, cx, cy;
  void Clear() { time.clear(), px.clear(), py.clear(), cx.clear(), cy.clear(); }
  void Evaluate(float t, float& x, float& y) const {
    const size_t count = time.size();
    if (count < 2) return;
    if (t <= time[0]) {
      x = px[0], y = py[0];
      return;
    }
    if (t >= time[count - 1]) {
      x = px[count - 1], y = py[count - 1];
      return;
    }
    size_t i = 0;
    while (i + 2 < count && time[i + 1] <= t) ++i;
    const float span = time[i + 1] - time[i];
    const float u = span > 0 ? (t - time[i]) / span : 0.0f;
    const auto cubic = [u](float p0, float c0, float c1, float p1) {
      const float a = p1 - 3.0f * c1 + 3.0f * c0 - p0, b = 3.0f * c1 - 6.0f * c0 + 3.0f * p0, c = 3.0f * c0 - 3.0f * p0;
      return ((a * u + b) * u + c) * u + p0;
    };
    x = cubic(px[i], cx[2 * i], cx[2 * i + 1], px[i + 1]);
    y = cubic(py[i], cy[2 * i], cy[2 * i + 1], py[i + 1]);
  }
};

struct Fairy {
  uint32_t source = 0;   // the green peg it came from
  uint32_t target = 0;   // the peg it flies to / sits on
  float x = 0, y = 0;    // board units
  Path path;
  float delay = 0, state_time = 0;
  float path_time = 0, path_duration = 0;
  int state = 0;         // 0 wait, 1 appear, 2 flying, 3 sitting, 4 leaving, 5 out, 6 gone
  bool is_new = true;
  uint32_t anim = 0;     // GameAnim
  uint32_t trail = 0;    // particle effect
  float rate = 1.0f;     // the animation's own speed (PS4: 0.75 + 0.5 * random)
};

struct State {
  int fx_state = 0;  // 0 off, 1 slowing down, 2 stopped, 3 speeding up
  float timer = 0;
  Curve a, b;        // the time scale; the dimming of the board (0..1)
  std::vector<uint32_t> pending;  // green pegs hit and not yet answered
  std::vector<Fairy> fairies;
  uint32_t hits_this_shot = 0, hits_total = 0;
  bool test_fired = false;  // peggle1_dlc_test anyhit: the power was triggered in this shot
  State() {
    a.Set(1.0f);
    b.Set(0.0f);
  }
};

std::recursive_mutex g_mutex;  // the game calls the power from its update thread; recursive in case a call back in nests
std::unordered_map<uint32_t, State> g_states;
uint32_t g_class = 0, g_vtable = 0, g_renderer_vtable = 0;
uint32_t g_factory = 0, g_symbols = 0;
uint32_t g_sound_spawn = 0, g_sound_hit = 0;
Resource g_butterfly{"POPANIM_MASTERS_WENDY_BUTTER_FLY", __imp__sub_82168D48};
Resource g_screen{"IMAGE_MASTERS_WENDY_ULTRAVIOLET_SCREEN", __imp__sub_8214F1B0};

State& StateOf(uint32_t self) { return g_states[self]; }  // under g_mutex

bool Alive(const std::vector<uint32_t>& targets, uint32_t target) {
  return target && std::find(targets.begin(), targets.end(), target) != targets.end();
}

void Purple(const Guest& g, uint32_t self, uint32_t target) {
  if (g.i32(target + target::kState) == 0) g.set32(target + target::kState, 1);  // the armour breaks (PS4 0x1D2AD0)
  if (g.i32(target + target::kType) == 2) return;
  g.set32(target + target::kType, 2);
  g.set32(target + target::kFlags, g.u32(target + target::kFlags) & ~0x0Bu);
  PostSound(g, self, g_sound_spawn);
}

// PS4 0x1D49C0: the first peg of a shuffled copy of the targets that is blue, not hit, may become purple and is no
// fairy's already.
uint32_t PickTarget(const Guest& g, uint32_t self, const State& s) {
  const uint32_t simulation = Simulation(g, self);
  Frame frame(g, 32);
  const uint32_t copy = frame.Alloc(16);
  g.Call(__imp__sub_821513E0, {copy, simulation + sim::kTargets});
  g.Call(__imp__sub_8223AEA8, {simulation, copy});
  uint32_t found = 0;
  int total = 0, blue = 0, allowed = 0;
  for (const uint32_t t : Pointers(g, copy)) {
    ++total;
    if (g.i32(t + target::kType) != 0 || g.i32(t + target::kState) > 1) continue;
    ++blue;
    if (!(g.u32(t + target::kFlags) & 4) && !Test("anypeg")) continue;  // levels without purple pegs have none
    ++allowed;
    if (std::any_of(s.fairies.begin(), s.fairies.end(), [t](const Fairy& f) { return f.target == t; })) continue;
    found = t;
    break;
  }
  if (!found && Logging()) {
    REXLOG_INFO("[dlc] Windy: no peg for a fairy ({} targets, {} blue and not hit, {} of them may be purple)", total,
                blue, allowed);
  }
  if (const uint32_t storage = g.u32(copy)) g.Call(__imp__sub_825EE8C0, {storage});
  return found;
}

// PS4 0x1D6390: the fairy's path from where it is to (to_x, to_y), a Bezier that wobbles from side to side.
//   With `velocity` (a ball knocked the fairy off its peg, 0x1D17D0) the path starts with four points along that
//   direction, 15 units far by the kick curve, in 0.7 s.
//   Then a point every 10 units of the way (their places a little random), the last one the destination, at least a
//   second in all and longer with the distance. Each segment's two control points stand 1 to 2 units off its ends to
//   one side, the side changing from segment to segment, and 4 to 6 % of the way along it.
// The random numbers are drawn in the PS4's order: the game's generator, and libc's rand for the first side.
void SetPath(const Guest& g, uint32_t simulation, Fairy& f, float to_x, float to_y, const float* velocity) {
  Path& p = f.path;
  p.Clear();
  f.path_time = 0;
  p.px.push_back(f.x), p.py.push_back(f.y), p.time.push_back(0.0f);
  if (!Motion()) {  // a straight line (the control points on it), at least a second, longer with the distance
    const float dx = to_x - f.x, dy = to_y - f.y;
    f.path_duration =
        std::max(1.0f, std::pow(std::hypot(dx, dy) / 70.0f * (1.0f + 0.2f * Random01(g, simulation)), 0.75f));
    p.px.push_back(to_x), p.py.push_back(to_y), p.time.push_back(f.path_duration);
    p.cx = {f.x + dx / 3.0f, f.x + dx * 2.0f / 3.0f};
    p.cy = {f.y + dy / 3.0f, f.y + dy * 2.0f / 3.0f};
    return;
  }
  int first = 0;        // the point the way to the destination starts from
  float base = 0.0f;    // its time
  if (velocity && std::isfinite(velocity[0]) && std::isfinite(velocity[1])) {
    float dx = velocity[0], dy = velocity[1];
    const float length = std::hypot(dx, dy);
    if (length != 0) dx /= length, dy /= length;
    const float reach = std::clamp(std::hypot(dx, dy) * 15.0f, 15.0f, 30.0f);
    for (int i = 1; i < 5; ++i) {
      const float c = g_curves.kick ? CurveAt(g, g_curves.kick, float(i) * 0.25f) : float(i) * 0.25f;
      p.px.push_back(f.x + c * dx * reach), p.py.push_back(f.y + c * dy * reach);
      p.time.push_back(float(i) * 0.7f * 0.25f);
    }
    first = 4, base = 0.7f;
  }
  const float sx = p.px[first], sy = p.py[first];
  const float dx = to_x - sx, dy = to_y - sy;
  const float distance = std::hypot(dx, dy);
  const float duration =
      base + std::max(1.0f, std::pow(distance * (1.0f / 70.0f) * (1.0f + 0.2f * Random01(g, simulation)), 0.75f));
  const int count = int(float(first) + std::min(512.0f, std::max(1.0f, distance * 0.1f) + 1.0f));
  const int last = count - 1;
  p.px.resize(count, 0.0f), p.py.resize(count, 0.0f), p.time.resize(count, 0.0f);
  p.px[last] = to_x, p.py[last] = to_y, p.time[last] = duration;
  p.cx.assign(size_t(2 * last), 0.0f), p.cy.assign(size_t(2 * last), 0.0f);
  const float nx = distance > 0 ? -dy / distance : 0.0f, ny = distance > 0 ? dx / distance : 0.0f;
  const float step = 1.0f / float(last);
  int side = (std::rand() & 1) ^ 1;
  for (int i = 0; i < count; ++i) {
    float along = std::max(0.0f, step * float(i - first));
    if (i > first && i < last) {
      along = std::clamp(along + step * 0.05f * Random01(g, simulation), 0.0f, 1.0f);
      p.px[i] = sx + dx * along, p.py[i] = sy + dy * along;
      p.time[i] = base + along * (duration - base);
    }
    if (i == 0) continue;
    const float r1 = Random01(g, simulation), r2 = Random01(g, simulation);
    const float off = (r1 + 1.0f) * (side ? -1.0f : 1.0f);
    const float k1 = r2 * 0.02f + 0.04f;
    p.cx[2 * (i - 1)] = nx * off + p.px[i - 1] + dx * k1 * along;
    p.cy[2 * (i - 1)] = ny * off + p.py[i - 1] + dy * k1 * along;
    const float k2 = -(Random01(g, simulation) * 0.02f + 0.04f);
    side ^= 1;
    p.cx[2 * (i - 1) + 1] = nx * off + p.px[i] + dx * k2 * along;
    p.cy[2 * (i - 1) + 1] = ny * off + p.py[i] + dy * k2 * along;
  }
  f.path_duration = duration;
}

// PS4 0x1D2E10: where the fairy is on its path, `ahead` seconds from now.
void PathPosition(const Guest& g, const Fairy& f, float ahead, float& x, float& y) {
  if (f.path_duration <= 0 || f.path_time >= f.path_duration) return;
  const float progress = std::clamp((f.path_time + ahead) / f.path_duration, 0.0f, 1.0f);
  const float eased = g_curves.path_time ? CurveAt(g, g_curves.path_time, progress)
                                         : progress * progress * (3.0f - 2.0f * progress);
  f.path.Evaluate(eased * f.path_duration, x, y);
}

void SetRate(const Guest& g, const Fairy& f, float factor) {
  if (f.anim) g.setf(f.anim + kAnimRate, f.rate * factor);
}

void Release(const Guest& g, Fairy& f) {
  DeleteAnim(g, f.anim);
  DeleteEffect(g, f.trail);
  f.anim = f.trail = 0;
}

// PS4 0x1D1F80: out over the top of the board, to the side the fairy is on; `velocity` = the ball's that hit its peg.
void FlyAway(const Guest& g, uint32_t simulation, Fairy& f, const float* velocity) {
  const Geometry board = BoardGeometry(g, simulation);
  const float width = board.scale > 0 ? float(board.w) / board.scale : 0.0f;
  const float height = board.scale > 0 ? float(board.h) / board.scale : 0.0f;
  float angle = -1.5707963f + Random01(g, simulation) * 3.1415927f * 0.26f;
  if (f.x < width * 0.5f) angle -= 0.8168f;
  SetPath(g, simulation, f, width * 0.5f + 0.875f * width * std::cos(angle),
          height * 0.5f + 0.875f * height * std::sin(angle), velocity);
  f.target = 0;
  f.state = 4;
  SetRate(g, f, 4.0f);
}

// PS4 0x1D2390.
void SpawnFairies(const Guest& g, uint32_t self, State& s, uint32_t source, const std::vector<uint32_t>& targets) {
  if (!Alive(targets, source)) return;
  const uint32_t count = std::min<uint32_t>(g.u32(self + kCfgNumPurplePegs), 32);
  const float sx = g.f32(source + 20), sy = g.f32(source + 24);
  const uint32_t simulation = Simulation(g, self);
  const Geometry board = BoardGeometry(g, simulation);
  SpawnEffect(g, simulation, EffectOf(g, kPurpleSparklesEffect), board.origin_x + sx * board.scale,
              board.origin_y + sy * board.scale, 2.0f);
  const uint32_t popanim = PopAnimOf(g, g_butterfly.Reference(g));
  const uint32_t trail = EffectOf(g, kFairyTrailEffect);
  for (uint32_t i = 0; i < count; ++i) {
    Fairy f;
    f.source = source;
    const float angle = 6.2831853f * float(i) / float(count);
    f.x = sx + 23.0f * std::cos(angle);
    f.y = sy + 23.0f * std::sin(angle);
    f.delay = float(i) / float(count) * 0.35f;
    f.rate = 0.75f + 0.5f * Random01(g, simulation);
    f.anim = NewAnim(g, popanim);
    f.trail = CloneEffect(g, trail);
    s.fairies.push_back(f);
  }
  if (Logging()) REXLOG_INFO("[dlc] Windy: {} fairies from the green peg {:08X}", count, source);
}

void SetFxState(const Guest& g, uint32_t self, State& s, int state, const std::vector<uint32_t>& targets) {
  s.fx_state = state;  // PS4 0x1D0580
  if (state == 1) {
    s.a.Down(0.5f);
    s.b.Up(0.25f);
    for (const uint32_t source : s.pending) SpawnFairies(g, self, s, source, targets);
    s.pending.clear();
  } else if (state == 3) {
    s.a.Up(0.5f);
    s.b.Down(0.5f);
  } else if (state == 0) {
    s.a.Set(1.0f);
    s.b.Set(0.0f);
  }
}

void Leave(const Guest& g, Fairy& f) {  // state 5: the trail stops, the animation "out" plays
  f.state = 5, f.state_time = 0;
  if (f.trail) g.set8(f.trail + kEffectEmits, 0);
  SetRate(g, f, 1.0f);
  if (!PlayAnim(g, f.anim, "out", false)) f.state_time = 2.0f;
}

void UpdateFairy(const Guest& g, uint32_t self, State& s, Fairy& f, uint32_t simulation,
                 const std::vector<uint32_t>& targets) {  // PS4 0x1D2AD0
  f.state_time += kTick;
  if (f.state >= 2) f.path_time += kTick;  // PS4: in the pre-update, 0x1D2950
  switch (f.state) {
    case 0:
      if (f.state_time > f.delay) {
        f.state = 1, f.state_time = 0;
        PlayAnim(g, f.anim, "fly", true);
        SetRate(g, f, 1.0f);
      }
      break;
    case 1:
      if (f.state_time > 0.35f) {
        const uint32_t t = PickTarget(g, self, s);
        if (t) {
          f.target = t;
          SetPath(g, simulation, f, g.f32(t + 20), g.f32(t + 24), nullptr);
          f.state = 2;
          SetRate(g, f, 4.0f);  // PS4 0x1D62A0
        } else {
          FlyAway(g, simulation, f, nullptr);
        }
        f.state_time = 0;
      }
      break;
    case 2:
      if (f.path_time >= f.path_duration) {
        if (Alive(targets, f.target)) {
          Purple(g, self, f.target);
          if (Logging()) REXLOG_INFO("[dlc] Windy: a fairy turned the peg {:08X} purple", f.target);
        }
        f.state = 3, f.state_time = 0;
        PlayAnim(g, f.anim, "idle", true);
        SetRate(g, f, 1.0f);
      }
      break;
    case 3:
      if (!Alive(targets, f.target) || g.i32(f.target + target::kState) == 2) Leave(g, f);
      break;
    case 4:
      if (f.path_time >= f.path_duration) Leave(g, f);
      break;
    case 5:  // gone when the "out" animation has played (a second at most)
      if (!f.anim || g.u8(f.anim + kAnimFinished) || f.state_time > 1.0f) {
        Release(g, f);
        f.state = 6;
      }
      break;
    default:
      break;
  }
  if (f.state == 2 || f.state == 4) {
    PathPosition(g, f, 0.0f, f.x, f.y);
  } else if (f.state == 3 && Alive(targets, f.target)) {
    f.x = g.f32(f.target + 20), f.y = g.f32(f.target + 24);
  }
}

// ---- the virtual functions (r3 = this) ----
REX_FUNC(WindyClass) { ctx.r3.u64 = g_class; }
REX_FUNC(WindyNotAbstract) { ctx.r3.u64 = 0; }

REX_FUNC(WindyDestroy) {  // (this, flags): bit 0 = free the memory   [the shape of 821B3538]
  const Guest g{ctx, base};
  const uint32_t self = ctx.r3.u32, flags = ctx.r4.u32;
  {
    std::lock_guard lock(g_mutex);
    if (const auto found = g_states.find(self); found != g_states.end()) {
      for (Fairy& f : found->second.fairies) Release(g, f);
      g_states.erase(found);
    }
  }
  g.set32(self + kRendererAt, power::kRendererVtable);
  g.Call(__imp__sub_822B7AB8, {self});
  if (flags & 1) g.Call(__imp__sub_825EE8C0, {self});
  ctx.r3.u64 = self;
}

REX_FUNC(WindyTimeScale) {  // PS4 0x1D4890
  const Guest g{ctx, base};
  std::lock_guard lock(g_mutex);
  const float scale = StateOf(ctx.r3.u32).a.Value(g);
  ctx.f1.f64 = double(scale);
}

REX_FUNC(WindyDoPower) {  // (this, target, ball)   PS4 0x1D2210
  const Guest g{ctx, base};
  const uint32_t self = ctx.r3.u32, target = ctx.r4.u32;
  if (!target) return;
  const std::vector<uint32_t> targets = Pointers(g, Simulation(g, self) + sim::kTargets);
  std::lock_guard lock(g_mutex);
  State& s = StateOf(self);
  s.pending.push_back(target);
  if (s.a.Value(g) == 0.0f) {
    s.fx_state = 2;
    s.timer = 0;
  } else {
    SetFxState(g, self, s, 1, targets);
  }
}

REX_FUNC(WindyUpdate) {  // every tick   PS4 0x1D25C0 (and the pre-update 0x1D2950)
  const Guest g{ctx, base};
  const uint32_t self = ctx.r3.u32;
  // The 360's rules would switch a power with mTurns >= 0 off at the end of a turn (see the top of this file).
  if (g.i32(self + power::kTurns) >= 0) g.set32(self + power::kTurns, uint32_t(-1));
  if (g.i32(self + power::kState) == 0) g.set32(self + power::kState, 2);
  std::lock_guard lock(g_mutex);
  State& s = StateOf(self);
  if (s.fx_state == 0 && s.fairies.empty()) return;
  const uint32_t simulation = Simulation(g, self);
  const std::vector<uint32_t> targets = Pointers(g, simulation + sim::kTargets);
  s.a.Advance(kTick);
  s.b.Advance(kTick);
  s.timer += kTick;
  switch (s.fx_state) {
    case 1:
      if (!s.a.running) s.fx_state = 2;
      break;
    case 2:
      if (std::all_of(s.fairies.begin(), s.fairies.end(), [](const Fairy& f) {
            return f.path_duration == 0 || f.path_time >= f.path_duration;
          })) {
        SetFxState(g, self, s, 3, targets);
      }
      break;
    case 3:
      if (!s.a.running) {
        for (Fairy& f : s.fairies) f.is_new = false;
        SetFxState(g, self, s, 0, targets);
      }
      break;
    default:
      break;
  }
  for (size_t i = 0; i < s.fairies.size(); ++i) UpdateFairy(g, self, s, s.fairies[i], simulation, targets);
  std::erase_if(s.fairies, [](const Fairy& f) { return f.state == 6; });
}

REX_FUNC(WindyTargetHit) {  // (this, target, by): every target hit   PS4 0x1D17D0
  const Guest g{ctx, base};
  const uint32_t self = ctx.r3.u32, target = ctx.r4.u32, by = ctx.r5.u32;
  if (!target) return;
  std::lock_guard lock(g_mutex);
  State& s = StateOf(self);
  if (Test("anyhit") && !s.test_fired && s.fx_state == 0) {  // test runs: the first peg of a shot acts as a green one
    s.test_fired = true;
    s.pending.push_back(target);
    SetFxState(g, self, s, 1, Pointers(g, Simulation(g, self) + sim::kTargets));
    return;
  }
  const auto f = std::find_if(s.fairies.begin(), s.fairies.end(), [target](const Fairy& x) {
    return x.target == target && x.state >= 2 && x.state <= 3;
  });
  if (f == s.fairies.end()) return;
  const uint32_t simulation = Simulation(g, self);
  Purple(g, self, target);  // hit before its fairy arrived: it counts as a fairy peg all the same
  const bool multiplayer = IsMultiplayer(g, self);
  const float x = g.f32(target + 20), y = g.f32(target + 24);
  const uint32_t single = g.u32(self + kCfgSingleShot), multi = g.u32(self + kCfgMultiShot);
  if (single && ++s.hits_this_shot >= single) {
    s.hits_this_shot -= single;
    AwardStyleShot(g, self, "StyleShotLotsOfPurpsSingleShot",
                   g.i32(self + (multiplayer ? kCfgSingleShotPtsMP : kCfgSingleShotPts)), x, y);
  }
  if (multi && ++s.hits_total >= multi) {
    s.hits_total -= multi;
    AwardStyleShot(g, self, "StyleShotLotsOfPurpsMultiShot",
                   g.i32(self + (multiplayer ? kCfgMultiShotPtsMP : kCfgMultiShotPts)), x, y);
  }
  PostSound(g, self, g_sound_hit);
  // The ball's velocity, when a ball hit it (its body is at +44, the velocity at +8 of that: 821B3848).
  const uint32_t body = Guest::Plausible(by) ? g.u32(by + 44) : 0;
  const float velocity[2] = {Guest::Plausible(body) ? g.f32(body + 8) : 0.0f,
                             Guest::Plausible(body) ? g.f32(body + 12) : 0.0f};
  FlyAway(g, simulation, *f, Guest::Plausible(body) ? velocity : nullptr);
}

REX_FUNC(WindyBallFired) {  // (this, ball)   PS4 0x1D21F0
  const uint32_t self = ctx.r3.u32;
  __imp__sub_822B7D28(ctx, base);
  std::lock_guard lock(g_mutex);
  StateOf(self).hits_this_shot = 0;
  StateOf(self).test_fired = false;
  TestOnBallFired(Guest{ctx, base}, self);
}

// ---- the renderer's virtual functions (r3 = the renderer = the power + 136) ----
REX_FUNC(WindyBusy) {  // PS4 0x1D57F0: fairies on their way
  std::lock_guard lock(g_mutex);
  const State& s = StateOf(ctx.r3.u32 - kRendererAt);
  const bool busy = !s.pending.empty() || s.fx_state != 0 ||
                    std::any_of(s.fairies.begin(), s.fairies.end(), [](const Fairy& f) { return f.state < 3; });
  ctx.r3.u64 = busy ? 1 : 0;
}

REX_FUNC(WindyDrawDim) {  // (renderer, Graphics*), after the background, origin = the board's corner   PS4 0x1D5550
  const Guest g{ctx, base};
  const uint32_t self = ctx.r3.u32 - kRendererAt, graphics = ctx.r4.u32;
  float dim;
  {
    std::lock_guard lock(g_mutex);
    dim = StateOf(self).b.Value(g);
  }
  if (dim <= 0.0f) return;
  const Geometry board = BoardGeometry(g, Simulation(g, self));
  SetColor(g, graphics, 0, 0, 0, int(dim * 100.0f));
  FillRect(g, graphics, 0, 0, board.w, board.h);
}

// The ultraviolet screen: PS4 0x1D4F90 draws IMAGE_MASTERS_WENDY_ULTRAVIOLET_SCREEN through the shader FAIRY_MAGIC
// into an image of the board's size every frame, and 0x1D3D90 draws that over the board through FAIRY_PUNCHOUT:
// alpha 0.65 times the dimming curve, with a hole (0.075 of the board, edge 0.02) around each fairy of the flock
// that is on its way (up to 8, the newest first).
//   Here: the image with the punch-out's alpha at the vertices of a mesh (DrawOverlay). FAIRY_MAGIC (per pixel: red
//   and blue become 0.25 c + (1 + sin(2 pi (c + Mod))) / 8 * 0.75, then all three 0.75 of that; Mod = ticks / 100;
//   read from the PS4's compiled shader, which leaves out the texture-coordinate wobble of the 360 archive's source
//   text) is the same darkening as a colour for the whole image, without the cycling.
void DrawScreen(const Guest& g, uint32_t self, uint32_t graphics) {
  Overlay o;
  const Geometry board = BoardGeometry(g, Simulation(g, self));
  if (board.w <= 0 || board.h <= 0) return;
  {
    std::lock_guard lock(g_mutex);
    const State& s = StateOf(self);
    o.alpha = s.b.Value(g) * 0.65f;
    if (o.alpha <= 0.0f) return;
    for (auto f = s.fairies.rbegin(); f != s.fairies.rend() && o.points < 8; ++f) {
      if (!f->is_new) continue;
      o.point[o.points][0] = f->x * board.scale / float(board.w);
      o.point[o.points][1] = f->y * board.scale / float(board.h);
      ++o.points;
    }
  }
  o.image = ImageOf(g, g_screen.Reference(g));
  o.w = board.w, o.h = board.h;
  // A typical texel of the image is (0.55, 0.05, 0.55): 0.75 * (0.25 + 0.09375 / 0.55) of red and blue, 0.75 of green.
  o.r = 81, o.green = 191, o.b = 81;
  DrawOverlay(g, graphics, o);
}

// (renderer, Graphics*): the game calls the renderer's slots 14, 16, 17, 15 in this order with the board's corner as
// the origin, then 13 with the screen's (seen in a run: each slot logged its Graphics). Slot 16 is where the PS4
// class has its ultraviolet screen (0x1D3D90, its slot 17).
REX_FUNC(WindyDrawScreen) {
  const Guest g{ctx, base};
  if (Overlays()) DrawScreen(g, ctx.r3.u32 - kRendererAt, ctx.r4.u32);
}

REX_FUNC(WindyDrawFairies) {  // (renderer, Graphics*), after the balls   PS4 0x1D2F30 / 0x1D3100
  const Guest g{ctx, base};
  const uint32_t self = ctx.r3.u32 - kRendererAt, graphics = ctx.r4.u32;
  std::lock_guard lock(g_mutex);
  State& s = StateOf(self);
  if (s.fairies.empty()) return;
  const Geometry board = BoardGeometry(g, Simulation(g, self));
  const float art = ArtScale(g) / 0.46875f;  // the PS4's offsets are pixels of its 1080-line art
  // The animation is 263 x 263 units (its file's header) with its origin in a corner: centred on the fairy.
  const float half = 131.5f * ArtScale(g);
  for (const Fairy& f : s.fairies) {
    if (f.state < 1 || f.state > 5) continue;
    const float x = f.x * board.scale, y = f.y * board.scale;
    DrawEffect(g, f.trail, graphics, x + 30.0f * art, y - 60.0f * art, 1.0f);
    DrawAnim(g, f.anim, graphics, x - half, y - half);
  }
}

REX_FUNC(WindyRenderTick) {  // (renderer): every logic tick
  const Guest g{ctx, base};
  std::lock_guard lock(g_mutex);
  for (const Fairy& f : StateOf(ctx.r3.u32 - kRendererAt).fairies) {
    if (f.state < 1) continue;
    TickAnim(g, f.anim);
    if (f.trail) g.Call(__imp__sub_823BADE8, {f.trail});
  }
}

REX_FUNC(WindyRenderFrame) {  // (renderer, f1 dt): every frame   PS4 0x1D28B0
  const Guest g{ctx, base};
  const float dt = float(ctx.f1.f64);
  std::lock_guard lock(g_mutex);
  for (const Fairy& f : StateOf(ctx.r3.u32 - kRendererAt).fairies) {
    if (f.state >= 1) AdvanceAnim(g, f.anim, dt);
  }
}

REX_FUNC(WindyFactory) {  // () -> a new object   [the shape of 821B4148 and 821B3488]
  const Guest g{ctx, base};
  MakeCurves(g);
  const uint32_t self = g.Call(__imp__sub_82369B48, {kObjectSize});
  if (self) {
    std::memset(base + self, 0, kObjectSize);
    g.Call(__imp__sub_822B78F8, {self});
    g.set32(self, g_vtable);
    g.set32(self + kRendererAt, g_renderer_vtable);
    g.set32(self + power::kRenderer, self + kRendererAt);
    g.set32(self + kCfgNumPurplePegs, 4);  // the PS4 constructor's and data file's values
    g.set32(self + kCfgSingleShot, 3);
    g.set32(self + kCfgSingleShotPts, 15000);
    g.set32(self + kCfgSingleShotPtsMP, 5000);
    g.set32(self + kCfgMultiShot, 8);
    g.set32(self + kCfgMultiShotPts, 10000);
    g.set32(self + kCfgMultiShotPtsMP, 5000);
    g.set32(self + power::kState, 2);  // always on (PS4 0x1D033C..0x1D0389)
    g.set8(self + power::kRepeat, 1);
    g.set32(self + power::kTurns, uint32_t(-1));
  }
  ctx.r3.u64 = self;
}

REX_FUNC(WindySymbols) {  // (builder, type)
  const Guest g{ctx, base};
  DeclareProperties(g, ctx.r3.u32, ctx.r4.u32, g_class, true, kProperties);
}

}  // namespace

void RegisterWindy(const Guest& g) {
  g_vtable = CopyVtable(g, power::kVtable, power::kSlots);
  g_renderer_vtable = CopyVtable(g, power::kRendererVtable, power::kRendererSlots);
  g_factory = Thunk(WindyFactory);
  g_symbols = Thunk(WindySymbols);
  if (!g_vtable || !g_renderer_vtable || !g_factory || !g_symbols) return;
  const auto slot = [&g](uint32_t index, PPCFunc* fn) { g.set32(g_vtable + index * 4, Thunk(fn)); };
  slot(0, WindyClass);
  slot(1, WindyNotAbstract);
  slot(2, WindyDestroy);
  slot(11, WindyTargetHit);
  slot(18, WindyTimeScale);
  slot(19, WindyUpdate);
  slot(27, WindyBallFired);
  slot(44, WindyDoPower);
  const auto draw = [&g](uint32_t index, PPCFunc* fn) { g.set32(g_renderer_vtable + index * 4, Thunk(fn)); };
  draw(6, WindyBusy);
  draw(14, WindyDrawDim);
  draw(15, WindyDrawFairies);
  if (Overlays()) draw(16, WindyDrawScreen);
  draw(20, WindyRenderTick);
  draw(21, WindyRenderFrame);
  g_sound_spawn = PermanentChars(g, "Play_PurplePeg_Fairy_Spawn");
  g_sound_hit = PermanentChars(g, "Play_PurplePeg_Fairy_Hit");
  g_class = NewClass(g);
  if (g_class) FinishClass(g, g_class, "PurplePegs", g_factory, g_symbols, kObjectSize);
}

}  // namespace peggle1::dlc
