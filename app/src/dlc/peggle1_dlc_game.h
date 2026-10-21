// Peggle 2's PS4-only content in the Xbox 360 build: the game's functions and layouts the two powers use.
//
// Everything here was read in the Xbox 360 executable's recompiled listing (app/generated/default); the note after
// each entry says where. "PS4" notes name the function of the PS4 executable (the user's own dump) whose behaviour a
// power reproduces; docs/peggle1-notes.md, "PS4 content in the Xbox 360 build", has the two specifications.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "dlc/peggle1_dlc_guest.h"

// ---- the game's functions (PowerPC convention: r3.., f1..; result r3 / f1) ----
extern "C" {
REX_FUNC(__imp__sub_82369B48);  // operator new(r3 size)                      [every factory, e.g. 821B4148]
REX_FUNC(__imp__sub_825EE8C0);  // operator delete(r3)                        [821B3538]
REX_FUNC(__imp__sub_8237FEC0);  // RtClass::RtClass(r3 this), 48 bytes        [821B23D8]
REX_FUNC(__imp__sub_821B22E8);  // MasterPower::GetRtClass()                  [821B23D8]
REX_FUNC(__imp__sub_82419708);  // the reflection system                      [821B23D8]
REX_FUNC(__imp__sub_8241C5F8);  // its type builder (r3 system)               [821B23D8]
REX_FUNC(__imp__sub_8236D6A0);  // std::string::string(r3 this, r4 chars)     [821B23D8]
REX_FUNC(__imp__sub_82382BB8);  // std::string::~string(r3 this)              [821B23D8]
REX_FUNC(__imp__sub_822B78F8);  // MasterPower::MasterPower(r3 this)          [821B3488]
REX_FUNC(__imp__sub_822B7AB8);  // MasterPower::~MasterPower(r3 this)         [821B3538]
REX_FUNC(__imp__sub_822B7B68);  // MasterPower slot 31: a green peg was hit (r3 this, r4 target, r5 ball)
REX_FUNC(__imp__sub_822B7D28);  // MasterPower slot 27: a ball was fired (r3 this, r4 ball)
REX_FUNC(__imp__sub_821B3488);  // MultiBall::MultiBall(r3 this), 200 bytes   [821B4148]
REX_FUNC(__imp__sub_821B3538);  // MultiBall deleting destructor (r3 this, r4 flags: bit 0 = free)
REX_FUNC(__imp__sub_82152068);  // Ball::Create()                             [821B3848]
REX_FUNC(__imp__sub_8223B138);  // Simulation::Add(r3 sim, r4 object, r5 0)   [821B3848]
REX_FUNC(__imp__sub_821A5E40);  // std::vector<T*>::push_back(r3 vector, r4 T**)  [821B3848]
REX_FUNC(__imp__sub_8219B390);  // Ball: after it was placed (r3 ball)        [821B3848]
REX_FUNC(__imp__sub_821513E0);  // std::vector<T*> copy (r3 out, r4 source)   [822523A8]
REX_FUNC(__imp__sub_8223AEA8);  // Simulation: shuffle a vector with the game's random numbers (r3 sim, r4 vector)
REX_FUNC(__imp__sub_82439218);  // random integer below r4 (r3 generator = sim + 252)   [the powers' own uses]
// resources by their string id [821675C8, 8215DFF4, 822BAF30]
REX_FUNC(__imp__sub_8238A088);  // lookup (r3 ref out, r4 manager, r5 type, r6 std::string* id, r7 1 = no throw)
REX_FUNC(__imp__sub_8214FC30);  // resource reference = (r3 ref, r4 64-bit value; 0 releases)
REX_FUNC(__imp__sub_8214F1B0);  // the type "Image"
REX_FUNC(__imp__sub_82168D48);  // the type "PopAnim"
REX_FUNC(__imp__sub_8238CDF8);  // the type "PIEffect"
REX_FUNC(__imp__sub_8215A1C0);  // reference -> PopAnim resource
REX_FUNC(__imp__sub_82153120);  // reference -> PIEffect resource
REX_FUNC(__imp__sub_82399DC0);  // the image manager, for:
REX_FUNC(__imp__sub_8214F248);  // reference -> Image (r3 manager, r4 ref)
REX_FUNC(__imp__sub_82153480);  // is that image usable (r3 image)
// animations: GameAnim, 212 bytes [8218CF10, 822A9A80, 822A9C40]
REX_FUNC(__imp__sub_8218CF10);  // GameAnim::GameAnim(r3 this, r4 owner or 0, r5 PopAnim resource)
REX_FUNC(__imp__sub_8218D7C8);  // play a label (r3 this, r4 std::string* label, r5 {int offset; u8 loop; u8 reset}*)
REX_FUNC(__imp__sub_8218D2A8);  // draw (r3 this, r4 Graphics*)
// particle effects owned by the caller [822BB510, 822BDCA8, 822BDDE8]
REX_FUNC(__imp__sub_823B54F0);  // clone of a PIEffect resource
REX_FUNC(__imp__sub_823BADE8);  // update (r3 effect)
REX_FUNC(__imp__sub_823A6708);  // draw (r3 effect, r4 Graphics*); its 3x3 matrix is at +2952
// Graphics [823BE5A8 and the lightning draw 822BB180]
REX_FUNC(__imp__sub_823BE5A8);  // set colour (r3 g, r4 Color* = four ints r g b a)
REX_FUNC(__imp__sub_823BF748);  // fill rectangle with the colour (r3 g, r4 x, r5 y, r6 w, r7 h)
REX_FUNC(__imp__sub_823BDD98);  // textured triangles (r3 g, r4 image, r5 vertices, r6 triangle count)
// style shots [822BD990]
REX_FUNC(__imp__sub_821D4E18);  // award (r3 out, r4 position, r5 std::string* name: emptied, r6 points)
REX_FUNC(__imp__sub_821D4F30);  // queue it (r3 the rules' award list, r4 award: its string is released)
// the framework's CurvedVal (136 bytes: the static one at 82D92588 ends at its guard word 82D92610) [82156D90]
REX_FUNC(__imp__sub_823E6680);  // CurvedVal::CurvedVal(r3 this, r4 std::string* curve data, r5 linked value or 0)
REX_FUNC(__imp__sub_823E5EC8);  // CurvedVal::GetOutVal(r3 this, f1 input) -> f1
}

namespace peggle1::dlc {

// ---- layouts ----
namespace power {  // MasterPower, 136 bytes [822B78F8, 822B7B68, 82252BD0, 82252D50]
inline constexpr uint32_t kAttached = 4;         // bool: the owner is the active player
inline constexpr uint32_t kRenderer = 8;         // MasterPowerRenderer* (the object's own, at +136)
inline constexpr uint32_t kState = 16;           // 0 off, 1 armed for the next ball, 2 always on, 3 active
inline constexpr uint32_t kBoard = 24;           // Board*
inline constexpr uint32_t kRules = 28;           // the rules' interface (rules + 8); vtable +0 = the simulation
inline constexpr uint32_t kBall = 32;            // the ball in play
inline constexpr uint32_t kTurnsLeft = 40;
inline constexpr uint32_t kTurns = 44;           // mTurns: < 0 = permanent
inline constexpr uint32_t kActivateNextBall = 48;
inline constexpr uint32_t kRepeat = 49;          // bool: slot 44 runs again for a green peg hit while already on
inline constexpr uint32_t kSize = 136;
inline constexpr uint32_t kVtable = 0x820E374C;          // 53 slots
inline constexpr uint32_t kRendererVtable = 0x820D4994;  // 22 slots
inline constexpr uint32_t kSlots = 53, kRendererSlots = 22;
}  // namespace power

namespace target {  // a peg or brick [822523A8, 822B7B68]
inline constexpr uint32_t kState = 140;  // 0 armoured, 1 normal, 2 hit
inline constexpr uint32_t kType = 144;   // 0 blue, 1 orange, 2 purple, 3 green
inline constexpr uint32_t kFlags = 148;  // bit 2 (4): may become purple; bits 0, 1, 3: a purple the per-turn draw may
                                         // take back [822523A8]
}  // namespace target

namespace sim {  // the simulation = rules interface vtable +0 [822523A8, 821B3848]
inline constexpr uint32_t kTargets = 100;  // std::vector<Target*>
inline constexpr uint32_t kBalls = 132;    // std::vector<Ball*>
inline constexpr uint32_t kRandom = 252;
}  // namespace sim

// ---- the reflection system ----
struct Property {
  const char* name;
  uint32_t kind, size;  // bool (3, 1), int (6, 4), uint32 (7, 4), float (8, 4)   [821B24B0, 822B7210]
  uint32_t offset;
};
inline constexpr uint32_t kBool = 3, kInt = 6, kUint = 7, kFloat = 8;

// Creates an empty class object (RtClass). The caller stores it where its GetRtClass looks before FinishClass, as
// the game does: the type builder may call the build-symbols function at once.
uint32_t NewClass(const Guest& g);
// Registers it as `name`, a child of MasterPower, created by the function at `factory`, with instances of `size`
// bytes whose properties the function at `symbols` declares.   [the sequence of 821B23D8]
void FinishClass(const Guest& g, uint32_t cls, const char* name, uint32_t factory, uint32_t symbols, uint32_t size);
// The body of a build-symbols function (r3 builder, r4 type). `parent`: also tie the type to its class and to
// MasterPower (a class that extends a game class leaves that to the game's own function).   [821B24B0]
void DeclareProperties(const Guest& g, uint32_t builder, uint32_t type, uint32_t cls, bool parent,
                       std::span<const Property> properties);

// ---- shared by the two powers ----
bool Enabled();    // peggle1_dlc_powers
bool Logging();    // peggle1_dlc_log
bool Overlays();   // peggle1_dlc_overlays
bool Motion();     // peggle1_dlc_motion
// A test switch of peggle1_dlc_test (comma-separated words), for unattended runs.
bool Test(const char* word);

uint32_t Simulation(const Guest& g, uint32_t self);  // the power's simulation
// Posts a sound event of the game's sound bank by name (`name`: permanent game memory).   [Board vtable +4, then +8]
void PostSound(const Guest& g, uint32_t self, uint32_t name);
// A copy of a std::vector<T*> of the game as host values.
std::vector<uint32_t> Pointers(const Guest& g, uint32_t vector);
// The game's random numbers: an integer below n, a float in [0, 1).
uint32_t Random(const Guest& g, uint32_t simulation, uint32_t n);
float Random01(const Guest& g, uint32_t simulation);

// ---- the engine (peggle1_dlc_engine.cpp) ----
// A resource by its string id, for resources the executable has no variable for. The reference lives in permanent
// game memory (as the game's own resource variables do) and is made at the first use; 0 = no such id.
struct Resource {
  const char* id;
  PPCFunc* type;
  uint32_t reference = 0;
  bool tried = false;
  uint32_t Reference(const Guest& g);
};
bool HasResource(const Guest& g, PPCFunc* type, std::string_view id);  // the archive has that id (no reference kept)
uint32_t PopAnimOf(const Guest& g, uint32_t reference);  // 0 when the resource is not loaded
uint32_t EffectOf(const Guest& g, uint32_t reference);
uint32_t ImageOf(const Guest& g, uint32_t reference);

// The game's own resource variables used here [out/ps4_dlc/listings/x360_resource_vars.tsv].
inline constexpr uint32_t kFairyTrailEffect = 0x82D8EF58;     // PIEFFECT_PI_FAIRY_TRAIL
inline constexpr uint32_t kPurpleSparklesEffect = 0x82D8F140; // PIEFFECT_PI_PURPLE_SPARKLES
inline constexpr uint32_t kPurpleVanishEffect = 0x82D8F150;   // PIEFFECT_PI_PURPLE_VANISH
inline constexpr uint32_t kBallImage = 0x82D8EDB8;            // IMAGE_BOARD_BALL (group BOARD_720)

uint32_t NewAnim(const Guest& g, uint32_t popanim);  // a GameAnim of that PopAnim resource
void DeleteAnim(const Guest& g, uint32_t anim);
bool PlayAnim(const Guest& g, uint32_t anim, std::string_view label, bool loop);
void TickAnim(const Guest& g, uint32_t anim);            // every logic tick
void AdvanceAnim(const Guest& g, uint32_t anim, float dt);  // every frame
void DrawAnim(const Guest& g, uint32_t anim, uint32_t graphics, float x, float y);
inline constexpr uint32_t kAnimRate = 172, kAnimFinished = 208;

// A particle effect the board owns and removes when it has ended: spawned at a screen position and left alone.
void SpawnEffect(const Guest& g, uint32_t simulation, uint32_t effect, float x, float y, float scale);
// A particle effect of the caller.
uint32_t CloneEffect(const Guest& g, uint32_t effect);
void DeleteEffect(const Guest& g, uint32_t effect);
void DrawEffect(const Guest& g, uint32_t effect, uint32_t graphics, float x, float y, float scale);
inline constexpr uint32_t kEffectEmits = 3037;  // bool

// The board on the screen: its rectangle, its origin (the rectangle's corner plus the screen shake) and the factor
// from world units to pixels.
struct Geometry {
  int x = 0, y = 0, w = 0, h = 0;
  float origin_x = 0, origin_y = 0, scale = 1;
};
Geometry BoardGeometry(const Guest& g, uint32_t simulation);
float ArtScale(const Guest& g);  // the art resolution / 2304 (0.3125 at 720 lines)

void SetColor(const Guest& g, uint32_t graphics, int r, int green, int b, int a);
void FillRect(const Guest& g, uint32_t graphics, int x, int y, int w, int h);
inline constexpr uint32_t kGraphicsMode = 600;      // int: 0 normal, 1 additive
inline constexpr uint32_t kGraphicsColorize = 604;  // bool: images take the colour
// A textured quad a-b-c-d (screen positions; u,v 0..1 over the whole image, a-b the top edge).
void DrawQuad(const Guest& g, uint32_t graphics, uint32_t image, const float (&corners)[4][2]);

// A curve of the framework's CurvedVal from its data string ("b;0,1,0.01,1,..."), the same strings the PS4 code
// gives its own CurvedVal: the object lives in permanent game memory. 0 when it could not be made.
uint32_t NewCurve(const Guest& g, std::string_view data);
// Its output for an input in [0, 1] (the input itself when there is no curve).
float CurveAt(const Guest& g, uint32_t curve, float input);

// A screen-covering layer with the alpha of the PS4's two overlay pixel shaders, drawn as a mesh whose vertices carry
// that alpha (the Xbox 360 build has no such shaders in the renderer's library; peggle1_dlc_engine.cpp, "overlay").
//   punch-out   EFFECTS\FAIRY_PUNCHOUT (= JIMMY_PUNCHOUT, the same file on the PS4): alpha = saturate(Alpha * v),
//               v = min(1, (distance to a point - Size) / (2 * EdgeSmooth)) over the points, distances in units of
//               the rectangle's width and height
//   spotlight   EFFECTS\JIMMY_SPOTLIGHT (read from the PS4's compiled shader): alpha = saturate(Alpha * v),
//               v = min over the two beams of (1 - |c|) / (1 - SpotWidth) where |c| > SpotWidth, else 1;
//               c = the cosine between (pixel - anchor) and (aim - anchor)
// Both set: the two layers of the PS4's MultiBall drawn as one, alpha = 1 - (1 - spotlight)(1 - punch-out).
struct Overlay {
  int w = 0, h = 0;          // the rectangle, at the Graphics origin
  uint32_t image = 0;        // its texture (stretched over the rectangle)
  bool one_texel = false;    // every vertex samples the image's centre instead (a sprite that is opaque there
                             // makes a plain coloured layer)
  int r = 255, green = 255, b = 255;
  float alpha = 1.0f;        // "Alpha"
  int points = 0;            // punch-out: up to 8 centres in 0..1 of the rectangle
  float point[8][2] = {};
  float size = 0.075f, edge = 0.01f;
  bool spots = false;        // spotlight: two beams
  float anchor[2][2] = {}, aim[2][2] = {};
  float spot_width = 0.99f;
};
void DrawOverlay(const Guest& g, uint32_t graphics, const Overlay& overlay);

bool IsMultiplayer(const Guest& g, uint32_t self);   // the rules' query TeslaCoil picks its MP points with
// The game's state as the single-player rules keep it: 2 = shots in normal play (the only state in which a green peg
// is reported to the power, 821CD954), 6 = fever; -1 when the power belongs to another rules class.
int GameState(const Guest& g, uint32_t self);
inline bool InFever(const Guest& g, uint32_t self) { return GameState(g, self) == 6; }
// Test switches of peggle1_dlc_test that set a level up when a ball is fired (greens, oneorange).
void TestOnBallFired(const Guest& g, uint32_t self);
// A style shot by name: the points are added and the title shown is the string table's entry of that name.
void AwardStyleShot(const Guest& g, uint32_t self, std::string_view name, int points, float world_x, float world_y);

void RegisterWindy(const Guest& g);  // the PurplePegs class (peggle1_dlc_windy.cpp)
void RegisterJimmy(const Guest& g);  // the vtables of the extended MultiBall (peggle1_dlc_jimmy.cpp)

}  // namespace peggle1::dlc
