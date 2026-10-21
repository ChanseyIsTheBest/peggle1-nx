// Peggle 2's PS4-only content: Jimmy Lightning's power, class MultiBall, in its final form.
//
// The Xbox 360 executable has an early MultiBall (200 bytes; constructor 821B3488): a green peg hit shakes the peg
// for m_iSpawnDelay ticks and then spawns m_iBallsToCreate balls. The PS4 release finished it (class MultiBall there:
// constructor 0x1C69B0, properties 0x1BBD30; the addresses below are that executable's; listing
// out/ps4_dlc/ps4_powers/multiball.lite.asm, not in the repository).
//
// WHAT THE PS4 CLASS DOES:
//   green peg hit (0x1C7400)  the base class's handler, then one pending entry per peg: spawn delay, removal delay,
//                             the peg's and the ball's position. Also for green pegs hit during fever (0x1C9A60).
//   every tick (0x1C7C40)     a pending entry whose delay ran out spawns the balls (0x1C7710: above the peg by
//                             m_fOffset, spread evenly from m_fMinAngle to m_fMaxAngle around straight up, speed
//                             m_fForce) and queues the peg's removal m_iRemoveDelay ticks later; until then the peg
//                             shakes within m_fShakeRange
//   time scale (0x1C9770)     m_fSlowdown (0.1) while a ball is predicted to reach an unhit green peg within
//                             m_iSlowdownLookAhead physics steps (0x1C98B0); sound Play_Jimmy_Power_SlowDown_Start
//                             when that starts, Play_Jimmy_Power_Stop when the balls are gone or the hit did not come
//   ball in the bucket (0x1C9AD0)  from the m_FrenzyStyleNum-th ball of a shot on: style shot "Free Ball Frenzy",
//                             (count - 1) times the points
//   fever (0x1BB930, 0x1BBAD0)     style shot "Multiball Madness" when fever starts with the power on, and for every
//                             spawn after fever was seen
//   drawing                   a lightning bolt from the top of the board down to the peg for m_fLightningDuration
//                             (0x1C8040: the four lightning images in turn, 3 ticks each, 0.3 of the image wide,
//                             additive, light blue then darker blue from tick 20), the effect PIEFFECT_PI_JIMMY_POWER
//                             on the peg, and the background dimmed by m_cDimShade (black, alpha 128) while the power
//                             is on and balls are in play (0x1C8300)
//
// HOW IT SITS IN THE 360 GAME: the game's own class, with a larger object. Three of its functions are replaced as
// hooks: the class getter (821B23D8: the same registration with the larger size), the factory (821B4148: the larger
// allocation, the game's constructor, then this file's vtables) and the build-symbols function (821B24B0: the
// game's seven properties, then the PS4's further ones). The vtable is a copy of the game's (820D49F4) with these
// replaced: 2 destructor, 11 any target hit, 18 time scale, 19 tick, 27 ball fired, 29 reset, 31 green peg hit,
// 35 ball caught, 43 switched off. The 360 functions they replace (821B35D0, 821B3658, 821B3848) are no longer
// reached. The renderer's table (a copy of 820D4ACC): 13 last in the frame: the lightning, 14 after the background:
// the dimming.
//
// DIFFERENCES FROM THE PS4, all in what is shown or heard, none in the rules:
//   - the dimming is plain: the PS4 cuts holes around the balls and sweeps two spotlights through it (shaders
//     JIMMY_PUNCHOUT and JIMMY_SPOTLIGHT, which exist only compiled for the PS4)
//   - the look-ahead is a free-flight prediction of the ball against the green pegs (gravity, no bounces): the 360
//     executable can only predict with a copy of the whole simulation (8214FE60), the PS4 steps a copy of the ball
//   - the two sound events are not in the 360's sound banks: posted, silent until the banks are converted
//   - no controller rumble at the spawn
//
// STATE: the game's object holds the reflected properties (the 360's seven at their 360 offsets, the PS4's after
// them); pending entries and timers are host objects, one set per power object, found by its address.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>

#include "dlc/peggle1_dlc_game.h"

namespace peggle1::dlc {
namespace {

// The 360 class's fields [821B3488, 821B24B0].
constexpr uint32_t kBallsToCreate = 140, kForce = 144, kMinAngle = 148, kMaxAngle = 152, kOffset = 156;
constexpr uint32_t kSpawnDelay = 160, kShakeRange = 164;
constexpr uint32_t kRendererAt = 136;
// The PS4 class's further properties, after the 360 object (200 bytes).
constexpr uint32_t kRemoveDelay = 200, kSlowdownDuration = 204, kSlowdown = 208, kSlowdownLookAhead = 212;
constexpr uint32_t kLightningDuration = 216, kFrenzyPointsSP = 220, kFrenzyPointsMP = 224, kFrenzyNum = 228;
constexpr uint32_t kFeverPointsSP = 232, kFeverPointsMP = 236;
constexpr uint32_t kObjectSize = 248;

constexpr uint32_t kClassVariable = 0x82D91E6C;  // where the game keeps MultiBall's class [821B23D8]
constexpr uint32_t kGameVtable = 0x820D49F4;     // [821B3488]
constexpr uint32_t kGameRendererVtable = 0x820D4ACC;
constexpr uint32_t kFactory = 0x821B4148, kSymbols = 0x821B24B0;

constexpr Property kProperties[] = {
    {"m_iRemoveDelay", kInt, 4, kRemoveDelay},
    {"m_fSlowdownDuration", kFloat, 4, kSlowdownDuration},
    {"m_fSlowdown", kFloat, 4, kSlowdown},
    {"m_iSlowdownLookAhead", kInt, 4, kSlowdownLookAhead},
    {"m_fLightningDuration", kFloat, 4, kLightningDuration},
    {"m_FrenzyStylePointsSP", kInt, 4, kFrenzyPointsSP},
    {"m_FrenzyStylePointsMP", kInt, 4, kFrenzyPointsMP},
    {"m_FrenzyStyleNum", kInt, 4, kFrenzyNum},
    {"m_FeverStylePointsSP", kInt, 4, kFeverPointsSP},
    {"m_FeverStylePointsMP", kInt, 4, kFeverPointsMP},
};

// The PS4 data file's strings and colour (PACKAGES\MULTIBALL.RTON there), which this class holds itself.
constexpr char kFrenzyStyleName[] = "StyleShotFreeBallFrenzy_DLC3";
constexpr char kFeverStyleName[] = "StyleShotMultiballMadness_DLC3";
constexpr int kDimAlpha = 128;  // m_cDimShade = (0, 0, 0, 128)

constexpr float kTick = 1.0f / 60.0f;

struct Pending {
  int spawn_delay, remove_delay;
  float peg_x, peg_y, ball_x, ball_y;
  uint32_t target;
};
struct Removal {
  uint32_t target;
  int delay;
};
struct Lightning {
  float x, y;  // the peg, world units
  float life;
  int frame;
};

struct State {
  std::vector<Pending> pending;
  std::vector<Removal> removals;
  std::vector<Lightning> lightnings;
  int bucket_count = 0;
  float slow_hold = -1.0f, miss_timer = 99999.0f;
  bool slow_latched = false, sound_on = false;
  bool fever = false, fever_seen = false;
  bool test_fired = false;  // peggle1_dlc_test anyhit
  // The last tick's position and velocity of the balls, for the measured step (see WillHitPowerPeg).
  uint32_t tracked_ball = 0;
  float last_x = 0, last_y = 0, last_vx = 0, last_vy = 0;
  float step = 0, gravity = 0;  // position change per unit of velocity and velocity change, per tick
  float width = 0;              // the board's, world units (taken when a ball is fired)
  // The two spotlights of the dimming: PS4 0x1C7BC0 swings the angle between 0 and pi / 2, a radian a second.
  float spot_angle = 0;
  int spot_direction = 1;
};

std::recursive_mutex g_mutex;
std::unordered_map<uint32_t, State> g_states;
uint32_t g_vtable = 0, g_renderer_vtable = 0;
uint32_t g_sound_slow = 0, g_sound_stop = 0;
Resource g_power_effect{"PIEFFECT_PI_JIMMY_POWER", __imp__sub_8238CDF8};
// The PS4's lightning images. The 360 has images of these names in Gnorman's group (his own power's); the archive
// builder gives the PS4's the names below so that both can be loaded at once.
Resource g_lightning[4] = {{"IMAGE_JIMMY_LIGHTNING1", __imp__sub_8214F1B0},
                           {"IMAGE_JIMMY_LIGHTNING2", __imp__sub_8214F1B0},
                           {"IMAGE_JIMMY_LIGHTNING3", __imp__sub_8214F1B0},
                           {"IMAGE_JIMMY_LIGHTNING4", __imp__sub_8214F1B0}};

State& StateOf(uint32_t self) { return g_states[self]; }  // under g_mutex

int Points(const Guest& g, uint32_t self, uint32_t single, uint32_t multi) {
  return g.i32(self + (IsMultiplayer(g, self) ? multi : single));
}

// PS4 0x1C7710.
void SpawnBalls(const Guest& g, uint32_t self, State& s, const Pending& e) {
  const int count = g.i32(self + kBallsToCreate);
  if (count <= 0) return;
  const float x = e.peg_x, y = e.peg_y - g.f32(self + kOffset);
  const float force = g.f32(self + kForce);
  const float step = count > 1 ? (g.f32(self + kMaxAngle) - g.f32(self + kMinAngle)) / float(count - 1) : 0.0f;
  float angle = g.f32(self + kMinAngle) - 1.570795f;
  const uint32_t simulation = Simulation(g, self);
  const uint64_t position = g.Pack(x, y);
  {
    Frame frame(g, 16);
    const uint32_t holder = frame.Alloc(8);
    for (int i = 0; i < count && i < 64; ++i, angle += step) {  // the sequence of the 360's 821B3848
      const uint32_t ball = g.Call(__imp__sub_82152068);
      if (!ball) break;
      g.Call(__imp__sub_8223B138, {simulation, ball, 0});
      g.set32(holder, ball);
      g.Call(__imp__sub_821A5E40, {simulation + sim::kBalls, holder});
      g.setf(ball + 20, x);
      g.setf(ball + 24, y);
      const uint32_t body = g.u32(ball + 44);
      if (body) g.Virtual(body, 4, {position});
      g.Call(__imp__sub_8219B390, {ball});
      g.setf(ball + 200, x);
      g.setf(ball + 204, y);
      if (body) {
        g.Floats(double(force * std::cos(angle)), double(force * std::sin(angle)));
        g.Virtual(body, 32);
      }
    }
  }
  if (s.fever_seen) {  // PS4 0x1BBAD0
    AwardStyleShot(g, self, kFeverStyleName, Points(g, self, kFeverPointsSP, kFeverPointsMP), e.peg_x, e.peg_y);
  }
  s.lightnings.push_back({e.peg_x, e.peg_y, g.f32(self + kLightningDuration), 0});
  const Geometry board = BoardGeometry(g, simulation);
  SpawnEffect(g, simulation, EffectOf(g, g_power_effect.Reference(g)), board.origin_x + e.peg_x * board.scale,
              board.origin_y + e.peg_y * board.scale, 1.0f);
  if (Logging()) REXLOG_INFO("[dlc] Jimmy: {} balls from the green peg {:08X} at ({:.1f}, {:.1f})", count, e.target, x, y);
}

void PostStop(const Guest& g, uint32_t self, State& s) {
  PostSound(g, self, g_sound_stop);
  s.sound_on = false;
}

// PS4 0x1C7400, after the base class's part.
void QueueSpawn(const Guest& g, uint32_t self, State& s, uint32_t target, uint32_t ball) {
  if (std::any_of(s.pending.begin(), s.pending.end(), [target](const Pending& e) { return e.target == target; })) {
    return;
  }
  Pending e{g.i32(self + kSpawnDelay), g.i32(self + kRemoveDelay), g.f32(target + 20), g.f32(target + 24), 0, 0,
            target};
  if (ball) e.ball_x = g.f32(ball + 20), e.ball_y = g.f32(ball + 24);
  s.pending.push_back(e);
  if (Logging()) {
    REXLOG_INFO("[dlc] Jimmy: green peg {:08X} hit, ball at distance {:.2f}; measured step {:.4f}, gravity {:.5f}",
                target, std::hypot(e.ball_x - e.peg_x, e.ball_y - e.peg_y), s.step, s.gravity);
  }
  s.slow_latched = false;
  s.slow_hold = g.f32(self + kSlowdownDuration);
}

// PS4 0x1C98B0 steps a copy of the ball through the physics for m_iSlowdownLookAhead steps (bounces included) and
// reports an unhit green peg it touches after the first step. The 360 executable has no step for a single ball, so
// the flight is predicted here: the position change per tick for the ball's velocity and the velocity change per
// tick are measured from the ball's own last tick (`step` and `gravity`); every target is a circle the ball's centre
// meets at kContact (the game reports a ball's hit on a peg at 3.3 to 3.5 units); a contact turns the velocity's part
// along the line of centres round, times kBounce (measured: see JimmyUpdate's log line "bounce"); the board's side
// walls do the same. Bricks are taken as pegs at their centres.
struct Peg {
  float x, y;
  uint32_t address;
  bool power;  // green and not hit
};
constexpr int kMaxPegs = 512;
// kBounce: 40 bounces of a PC run logged by JimmyUpdate, the middle of them 0.80 to 0.84 (a few grazing ones lower).
constexpr float kContact = 3.4f, kBounce = 0.81f, kBallRadius = 1.7f;

// The targets as the prediction needs them (no allocation: called every tick).
int ReadPegs(const Guest& g, uint32_t simulation, Peg* pegs, bool& any_power) {
  const uint32_t begin = g.u32(simulation + sim::kTargets), end = g.u32(simulation + sim::kTargets + 4);
  any_power = false;
  if (!Guest::Plausible(begin) || end < begin) return 0;
  int count = 0;
  for (uint32_t a = begin; a < end && count < kMaxPegs; a += 4) {
    const uint32_t t = g.u32(a);
    const bool power = g.i32(t + target::kType) == 3 && g.i32(t + target::kState) == 1;
    any_power |= power;
    pegs[count++] = {g.f32(t + 20), g.f32(t + 24), t, power};
  }
  return count;
}

bool WillHitPowerPeg(const Guest& g, uint32_t self, const State& s, uint32_t ball, const Peg* pegs, int count) {
  const uint32_t body = g.u32(ball + 44);
  if (!body || s.step <= 0) return false;
  float x = g.f32(ball + 20), y = g.f32(ball + 24);
  float vx = g.f32(body + 8), vy = g.f32(body + 12);
  const int steps = std::clamp(g.i32(self + kSlowdownLookAhead), 0, 64);
  if (!Motion()) {  // free flight against the green pegs only: no bounce
    constexpr float kReach = 3.5f;
    for (int i = 0; i < steps; ++i) {
      vy += s.gravity;
      const float dx = vx * s.step, dy = vy * s.step;
      const float length2 = dx * dx + dy * dy;
      for (int k = 0; k < count; ++k) {
        if (!pegs[k].power) continue;
        const float px = pegs[k].x - x, py = pegs[k].y - y;
        const float along = length2 > 0 ? std::clamp((px * dx + py * dy) / length2, 0.0f, 1.0f) : 0.0f;
        const float cx = px - dx * along, cy = py - dy * along;
        if (cx * cx + cy * cy <= kReach * kReach && i > 0) return true;
      }
      x += dx, y += dy;
    }
    return false;
  }
  uint32_t last = 0;  // the target of the last contact (PS4: the same peg twice in a row does not count)
  for (int i = 0; i < steps; ++i) {
    vy += s.gravity;
    float left = 1.0f;  // of this tick
    for (int contact = 0; contact < 2 && left > 0; ++contact) {
      const float dx = vx * s.step * left, dy = vy * s.step * left;
      const float a = dx * dx + dy * dy;
      if (a <= 0) break;
      float first = 2.0f;
      int which = -1;
      for (int k = 0; k < count; ++k) {
        const float fx = x - pegs[k].x, fy = y - pegs[k].y;
        const float b = fx * dx + fy * dy;
        if (b >= 0) continue;  // moving away from it
        const float c = fx * fx + fy * fy - kContact * kContact;
        float t = 0.0f;
        if (c > 0) {
          const float d = b * b - a * c;
          if (d < 0) continue;
          t = (-b - std::sqrt(d)) / a;
          if (t > 1.0f) continue;
        }
        if (t < first) first = t, which = k;
      }
      if (which < 0) {
        x += dx, y += dy;
        break;
      }
      const Peg& p = pegs[which];
      x += dx * first, y += dy * first;
      if (p.power && i > 0 && p.address != last) return true;
      last = p.address;
      float nx = x - p.x, ny = y - p.y;
      const float length = std::hypot(nx, ny);
      if (length > 0) nx /= length, ny /= length;
      const float into = vx * nx + vy * ny;
      if (into < 0) vx -= (1.0f + kBounce) * into * nx, vy -= (1.0f + kBounce) * into * ny;
      left *= 1.0f - first;
    }
    if (s.width > 0) {
      if ((x < kBallRadius && vx < 0) || (x > s.width - kBallRadius && vx > 0)) vx = -vx * kBounce;
    }
  }
  return false;
}

// ---- the virtual functions (r3 = this) ----
REX_FUNC(JimmyDestroy) {  // (this, flags)
  {
    std::lock_guard lock(g_mutex);
    g_states.erase(ctx.r3.u32);
  }
  __imp__sub_821B3538(ctx, base);
}

REX_FUNC(JimmyGreenHit) {  // (this, target, ball)   PS4 0x1C7400
  const Guest g{ctx, base};
  const uint32_t self = ctx.r3.u32, target = ctx.r4.u32, ball = ctx.r5.u32;
  __imp__sub_822B7B68(ctx, base);
  if (!target || g.i32(target + target::kState) != 2) return;
  std::lock_guard lock(g_mutex);
  QueueSpawn(g, self, StateOf(self), target, ball);
}

REX_FUNC(JimmyTargetHit) {  // (this, target, by): every target hit
  const Guest g{ctx, base};
  const uint32_t self = ctx.r3.u32, target = ctx.r4.u32, by = ctx.r5.u32;
  if (!target) return;
  bool green;
  {
    std::lock_guard lock(g_mutex);
    State& s = StateOf(self);
    // The 360's rules report a green peg to the power only in the "shot in play" state (82253538); the PS4's
    // MultiBall also answers green pegs hit during fever (0x1C9A60).
    green = s.fever && g.i32(target + target::kType) == 3;
    if (Test("anyhit") && !s.test_fired) green = s.test_fired = true;
  }
  if (!green) return;
  if (Test("anyhit")) g.set32(target + target::kState, 2);
  ctx.r3.u64 = self, ctx.r4.u64 = target, ctx.r5.u64 = by;
  JimmyGreenHit(ctx, base);
}

REX_FUNC(JimmyUpdate) {  // every tick   PS4 0x1C7C40, and the timers of 0x1C7BC0
  const Guest g{ctx, base};
  const uint32_t self = ctx.r3.u32;
  std::lock_guard lock(g_mutex);
  State& s = StateOf(self);
  if (s.slow_hold >= 0) s.slow_hold -= kTick;
  if (s.miss_timer >= 0 && s.miss_timer < 99999.0f) s.miss_timer -= kTick;
  for (Lightning& l : s.lightnings) l.life -= kTick, ++l.frame;
  std::erase_if(s.lightnings, [](const Lightning& l) { return l.life <= 0; });
  s.spot_angle += kTick * float(s.spot_direction);  // PS4 0x1C7BC0
  if (s.spot_angle >= 1.5707964f || !(s.spot_angle > 0.0f)) s.spot_direction = -s.spot_direction;

  const bool fever = InFever(g, self);
  if (Logging()) {
    static int last_state = -1, last_power = -1;
    const int state = GameState(g, self), active = g.i32(self + power::kState);
    if (state != last_state || active != last_power) {
      REXLOG_INFO("[dlc] game state {} -> {}, power state {} -> {}", last_state, state, last_power, active);
      last_state = state, last_power = active;
    }
  }
  if (fever && !s.fever) {  // PS4 0x1BB930
    s.fever_seen = true;
    if (g.i32(self + power::kState) == 3) {
      const uint32_t ball = g.u32(self + power::kBall);
      AwardStyleShot(g, self, kFeverStyleName, Points(g, self, kFeverPointsSP, kFeverPointsMP),
                     ball ? g.f32(ball + 20) : 0.0f, ball ? g.f32(ball + 24) : 0.0f);
    }
  }
  s.fever = fever;

  const uint32_t simulation = Simulation(g, self);
  const std::vector<uint32_t> balls = Pointers(g, simulation + sim::kBalls);
  // The measured step of the first ball (see WillHitPowerPeg): taken while the simulation runs at full speed.
  const uint32_t first = balls.empty() ? 0 : balls[0];
  const uint32_t body = first ? g.u32(first + 44) : 0;
  if (body) {
    const float x = g.f32(first + 20), y = g.f32(first + 24), vx = g.f32(body + 8), vy = g.f32(body + 12);
    if (first == s.tracked_ball && !s.slow_latched && s.slow_hold <= 0) {
      const float speed2 = s.last_vx * s.last_vx + s.last_vy * s.last_vy;
      const float moved = (x - s.last_x) * s.last_vx + (y - s.last_y) * s.last_vy;
      const float dvx = vx - s.last_vx, dvy = vy - s.last_vy;
      // A free-flight tick: the velocity changed only downwards and by little.
      if (speed2 > 1e-6f && std::fabs(dvx) < 1e-5f && dvy > 0 && dvy < 0.05f && moved > 0) {
        s.step = moved / speed2;
        s.gravity = dvy;
      } else if (Logging() && speed2 > 1e-6f && std::hypot(dvx, dvy) > 0.1f) {
        // A bounce: the velocity's change is along the line of centres; how much of the speed along it came back.
        static int logged = 0;
        const float change = std::hypot(dvx, dvy);
        const float into = -(s.last_vx * dvx + s.last_vy * dvy) / change;
        if (into > 0.05f && logged++ < 40) {
          REXLOG_INFO("[dlc] Jimmy: bounce: speed along the line of centres {:.3f} in, {:.3f} out (ratio {:.3f})", into,
                      change - into, (change - into) / into);
        }
      }
    }
    s.tracked_ball = first, s.last_x = x, s.last_y = y, s.last_vx = vx, s.last_vy = vy;
  } else {
    s.tracked_ball = 0;
  }

  if (s.pending.empty() && s.removals.empty() && !s.sound_on) return;
  const std::vector<uint32_t> targets = Pointers(g, simulation + sim::kTargets);
  const auto alive = [&targets](uint32_t t) { return std::find(targets.begin(), targets.end(), t) != targets.end(); };
  for (size_t i = 0; i < s.pending.size();) {
    Pending& e = s.pending[i];
    if (e.spawn_delay-- <= 1) {
      const Pending done = e;
      s.pending.erase(s.pending.begin() + i);
      SpawnBalls(g, self, s, done);
      if (alive(done.target)) {
        g.Virtual(done.target, 36, {g.Pack(done.peg_x, done.peg_y)});  // back from the shake [821B3658]
        s.removals.push_back({done.target, done.remove_delay});
      }
      continue;
    }
    if (alive(e.target)) {  // the shake: libc rand, as on the PS4 and in the 360's 821B3658
      const float range = g.f32(self + kShakeRange);
      const float dx = float(std::rand()) / float(RAND_MAX) * range - range * 0.5f;
      const float dy = float(std::rand()) / float(RAND_MAX) * range - range * 0.5f;
      g.Virtual(e.target, 36, {g.Pack(e.peg_x + dx, e.peg_y + dy)});
    }
    ++i;
  }
  for (size_t i = 0; i < s.removals.size();) {
    if (s.removals[i].delay-- <= 0) {
      // The green peg is destroyed: the simulation's sweep removes objects with this flag (8223B390).
      if (alive(s.removals[i].target)) g.set8(s.removals[i].target + 53, 1);
      s.removals.erase(s.removals.begin() + i);
      continue;
    }
    ++i;
  }
  if (s.slow_latched && s.sound_on && s.miss_timer < 0 && balls.size() == 1) {
    PostStop(g, self, s);
    s.slow_latched = false;
    s.miss_timer = 99999.0f;
  }
  if (s.sound_on && balls.empty()) PostStop(g, self, s);
}

REX_FUNC(JimmyTimeScale) {  // PS4 0x1C9770
  const Guest g{ctx, base};
  const uint32_t self = ctx.r3.u32;
  std::lock_guard lock(g_mutex);
  State& s = StateOf(self);
  const uint32_t simulation = Simulation(g, self);
  double scale = 1.0;
  if (g.f32(simulation + 176) >= 1.0f) {  // not while the game itself runs slowly
    static Peg pegs[kMaxPegs];  // under g_mutex
    bool any_power = false, hit = false;
    const int count = ReadPegs(g, simulation, pegs, any_power);
    if (any_power) {
      const uint32_t begin = g.u32(simulation + sim::kBalls), end = g.u32(simulation + sim::kBalls + 4);
      for (uint32_t a = begin; Guest::Plausible(begin) && a < end && end - begin <= 0x1000; a += 4) {
        if (WillHitPowerPeg(g, self, s, g.u32(a), pegs, count)) {
          hit = true;
          break;
        }
      }
    }
    if (hit) {
      s.miss_timer = 0.5f;
      if (!s.sound_on) {
        PostSound(g, self, g_sound_slow);
        s.sound_on = true;
      }
      s.slow_latched = true;
    }
    if (hit || s.slow_hold > 0 || s.slow_latched) scale = double(g.f32(self + kSlowdown));
  }
  ctx.f1.f64 = scale;
}

REX_FUNC(JimmyBallFired) {  // (this, ball)   PS4 0x1C9A70 (its turn start: a new shot)
  const uint32_t self = ctx.r3.u32;
  __imp__sub_822B7D28(ctx, base);
  std::lock_guard lock(g_mutex);
  State& s = StateOf(self);
  s.bucket_count = 0;
  s.slow_latched = false;
  s.miss_timer = 99999.0f;
  s.test_fired = false;
  const Guest g{ctx, base};
  if (Motion()) {
    const Geometry board = BoardGeometry(g, Simulation(g, self));
    s.width = board.scale > 0 ? float(board.w) / board.scale : 0.0f;
  }
  TestOnBallFired(g, self);
}

REX_FUNC(JimmyBallCaught) {  // (this, ball)   PS4 0x1C9AD0
  const Guest g{ctx, base};
  const uint32_t self = ctx.r3.u32, ball = ctx.r4.u32;
  std::lock_guard lock(g_mutex);
  State& s = StateOf(self);
  ++s.bucket_count;
  if (s.bucket_count >= g.i32(self + kFrenzyNum)) {
    AwardStyleShot(g, self, kFrenzyStyleName, (s.bucket_count - 1) * Points(g, self, kFrenzyPointsSP, kFrenzyPointsMP),
                   ball ? g.f32(ball + 20) : 0.0f, ball ? g.f32(ball + 24) : 0.0f);
  }
}

REX_FUNC(JimmyReset) {  // PS4 0x1C7360
  const Guest g{ctx, base};
  const uint32_t self = ctx.r3.u32;
  g.set32(self + power::kBall, 0);
  std::lock_guard lock(g_mutex);
  State& s = StateOf(self);
  s.bucket_count = 0;
  if (s.slow_latched && s.sound_on) PostStop(g, self, s);
  s.slow_latched = false;
}

extern "C" REX_FUNC(__imp__sub_822B8458);  // MasterPower slot 43: the power was switched off
REX_FUNC(JimmySwitchedOff) {  // PS4 0x1C9AA0
  const uint32_t self = ctx.r3.u32;
  __imp__sub_822B8458(ctx, base);
  std::lock_guard lock(g_mutex);
  State& s = StateOf(self);
  s.slow_latched = false;
  s.slow_hold = -1.0f;
  s.miss_timer = 99999.0f;
}

// ---- the renderer's virtual functions (r3 = the renderer = the power + 136, r4 = Graphics*) ----
REX_FUNC(JimmyDrawDim) {  // after the background, origin = the board's corner   PS4 0x1C8300
  const Guest g{ctx, base};
  const uint32_t self = ctx.r3.u32 - kRendererAt, graphics = ctx.r4.u32;
  if (g.i32(self + power::kState) != 3) return;
  const uint32_t simulation = Simulation(g, self);
  if (g.u32(simulation + sim::kBalls) == g.u32(simulation + sim::kBalls + 4)) return;
  const Geometry board = BoardGeometry(g, simulation);
  // The layer's texture: the ball's sprite, of which only the (opaque) centre is sampled. (IMAGE_SOLID_WHITE draws
  // nothing through the textured-triangle function: seen in a run.)
  const uint32_t white = Overlays() ? ImageOf(g, kBallImage) : 0;
  if (!white || board.w <= 0 || board.h <= 0) {
    SetColor(g, graphics, 0, 0, 0, kDimAlpha);
    FillRect(g, graphics, 0, 0, board.w, board.h);
    return;
  }
  // The PS4 draws a black image of the board's size twice, each time with alpha m_cDimShade's 128 / 255:
  //   through JIMMY_SPOTLIGHT: two beams from below the board, anchors (0.3, 1.2) and (0.7, 1.2) of the board,
  //     aimed at (cos a, 0) and (sin a, 0) on its top edge, a swinging between 0 and pi / 2 (0x1C8717..0x1C8989);
  //   through JIMMY_PUNCHOUT: holes (0.075 of the board, edge 0.02) around the first eight balls (0x1C8E61..).
  // Here both are one mesh (DrawOverlay).
  Overlay o;
  o.image = white;
  o.one_texel = true;
  o.w = board.w, o.h = board.h;
  o.r = o.green = o.b = 0;
  o.alpha = float(kDimAlpha) / 255.0f;
  const uint32_t begin = g.u32(simulation + sim::kBalls), end = g.u32(simulation + sim::kBalls + 4);
  for (uint32_t a = begin; Guest::Plausible(begin) && a < end && o.points < 8; a += 4) {
    const uint32_t ball = g.u32(a);
    o.point[o.points][0] = g.f32(ball + 20) * board.scale / float(board.w);
    o.point[o.points][1] = g.f32(ball + 24) * board.scale / float(board.h);
    ++o.points;
  }
  float angle;
  {
    std::lock_guard lock(g_mutex);
    angle = StateOf(self).spot_angle;
  }
  o.spots = true;
  o.anchor[0][0] = 0.3f, o.anchor[0][1] = 1.2f, o.anchor[1][0] = 0.7f, o.anchor[1][1] = 1.2f;
  o.aim[0][0] = std::cos(angle), o.aim[0][1] = 0.0f, o.aim[1][0] = std::sin(angle), o.aim[1][1] = 0.0f;
  DrawOverlay(g, graphics, o);
}

REX_FUNC(JimmyDrawLightning) {  // last in the frame, origin = the screen's corner   PS4 0x1C8040
  const Guest g{ctx, base};
  const uint32_t self = ctx.r3.u32 - kRendererAt, graphics = ctx.r4.u32;
  std::lock_guard lock(g_mutex);
  const State& s = StateOf(self);
  if (s.lightnings.empty()) return;
  const Geometry board = BoardGeometry(g, Simulation(g, self));
  const uint32_t mode = g.u32(graphics + kGraphicsMode);
  const uint8_t colorize = g.u8(graphics + kGraphicsColorize);
  for (const Lightning& l : s.lightnings) {
    const uint32_t image = ImageOf(g, g_lightning[(l.frame / 3) % 4].Reference(g));
    if (Logging() && l.frame < 2) {
      REXLOG_INFO("[dlc] Jimmy: lightning frame {} image {:08X} ({} x {}), board origin ({:.0f}, {:.0f}) scale {:.2f}",
                  l.frame, image, image ? g.i32(image + 64) : 0, image ? g.i32(image + 68) : 0, board.origin_x,
                  board.origin_y, board.scale);
    }
    if (!image) continue;
    const float half = float(g.i32(image + 64)) * 0.15f;
    const float x = board.origin_x + l.x * board.scale;
    const float top = board.origin_y, bottom = board.origin_y + l.y * board.scale;
    const float corners[4][2] = {{x - half, top}, {x + half, top}, {x + half, bottom}, {x - half, bottom}};
    g.set8(graphics + kGraphicsColorize, 1);
    g.set32(graphics + kGraphicsMode, 1);
    if (l.frame <= 19) {
      SetColor(g, graphics, 200, 200, 255, 255);
    } else {
      SetColor(g, graphics, 128, 128, 255, 255);
    }
    DrawQuad(g, graphics, image, corners);
  }
  g.set32(graphics + kGraphicsMode, mode);
  g.set8(graphics + kGraphicsColorize, colorize);
}

}  // namespace

void RegisterJimmy(const Guest& g) {
  g_vtable = CopyVtable(g, kGameVtable, power::kSlots);
  g_renderer_vtable = CopyVtable(g, kGameRendererVtable, power::kRendererSlots);
  if (!g_vtable || !g_renderer_vtable) return;
  const auto slot = [&g](uint32_t index, PPCFunc* fn) { g.set32(g_vtable + index * 4, Thunk(fn)); };
  slot(2, JimmyDestroy);
  slot(11, JimmyTargetHit);
  slot(18, JimmyTimeScale);
  slot(19, JimmyUpdate);
  slot(27, JimmyBallFired);
  slot(29, JimmyReset);
  slot(31, JimmyGreenHit);
  slot(35, JimmyBallCaught);
  slot(43, JimmySwitchedOff);
  const auto draw = [&g](uint32_t index, PPCFunc* fn) { g.set32(g_renderer_vtable + index * 4, Thunk(fn)); };
  draw(13, JimmyDrawLightning);
  draw(14, JimmyDrawDim);
  g_sound_slow = PermanentChars(g, "Play_Jimmy_Power_SlowDown_Start");
  g_sound_stop = PermanentChars(g, "Play_Jimmy_Power_Stop");
}

}  // namespace peggle1::dlc

using peggle1::dlc::Guest;

// MultiBall's class getter: the game's sequence with the larger object size.
extern "C" REX_FUNC(__imp__sub_821B23D8);
REX_HOOK_RAW(sub_821B23D8) {
  if (!peggle1::dlc::Enabled()) {
    __imp__sub_821B23D8(ctx, base);
    return;
  }
  const Guest g{ctx, base};
  uint32_t cls = g.u32(peggle1::dlc::kClassVariable);
  if (!cls) {
    cls = peggle1::dlc::NewClass(g);
    g.set32(peggle1::dlc::kClassVariable, cls);
    if (cls) {
      peggle1::dlc::FinishClass(g, cls, "MultiBall", peggle1::dlc::kFactory, peggle1::dlc::kSymbols,
                                peggle1::dlc::kObjectSize);
    }
  }
  ctx.r3.u64 = cls;
}

// Its factory: the larger object, the game's constructor, the PS4 data file's values for the new fields.
extern "C" REX_FUNC(__imp__sub_821B4148);
REX_HOOK_RAW(sub_821B4148) {
  if (!peggle1::dlc::Enabled() || !peggle1::dlc::g_vtable) {
    __imp__sub_821B4148(ctx, base);
    return;
  }
  using namespace peggle1::dlc;
  const Guest g{ctx, base};
  const uint32_t self = g.Call(__imp__sub_82369B48, {kObjectSize});
  if (self) {
    std::memset(base + self, 0, kObjectSize);
    g.Call(__imp__sub_821B3488, {self});
    g.set32(self, g_vtable);
    g.set32(self + kRendererAt, g_renderer_vtable);
    g.set32(self + kRemoveDelay, 1);
    g.setf(self + kSlowdownDuration, 0.0f);
    g.setf(self + kSlowdown, 0.1f);
    g.set32(self + kSlowdownLookAhead, 6);
    g.setf(self + kLightningDuration, 0.75f);
    g.set32(self + kFrenzyPointsSP, 10000);
    g.set32(self + kFrenzyPointsMP, 5000);
    g.set32(self + kFrenzyNum, 2);
    g.set32(self + kFeverPointsSP, 25000);
    g.set32(self + kFeverPointsMP, 10000);
  }
  ctx.r3.u64 = self;
}

// Its build-symbols function (r3 builder, r4 type): the game's properties, then the PS4's.
extern "C" REX_FUNC(__imp__sub_821B24B0);
REX_HOOK_RAW(sub_821B24B0) {
  const uint32_t builder = ctx.r3.u32, type = ctx.r4.u32;
  __imp__sub_821B24B0(ctx, base);
  if (!peggle1::dlc::Enabled()) return;
  const Guest g{ctx, base};
  peggle1::dlc::DeclareProperties(g, builder, type, 0, false, peggle1::dlc::kProperties);
}
