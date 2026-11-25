// peggle1-nx (2026-11-24, the user: "Convert all videos to h265 please"): Peggle's movie, PopCap_Logo_MedQual_720p.wmv (the PopCap logo: WMV3 1280x720 at 29.97 fps, 8.4 s, WMA Pro audio),
// decoded on the host instead of by Microsoft's WMV9 decoder in guest code: FFmpeg's WMV3 decoder for the game's own
// file, and nxvideo (tools/switch/nxvideo: the Switch's NVDEC) for the H.265 copy the installer makes (the WMV3 video
// re-encoded frame for frame inside the same ASF file, its audio, timing and sequence header kept).
// nights-nx's app/src/nights_wmv_ffmpeg.cpp (its notes below), which is culdcept-nx's src/cpu/culdcept_wmv_ffmpeg.cpp
// with B pictures (credit nights-nx and culdcept-nx; culdcept-nx's method from sasrt-nx and tw2de_nx; banjo-nx-ult has
// the same on N&B's names). The game links the XMedia WMV9 wrapper of Culdcept Saga's and N&B's build
// (_tools/ultimate_kit/find_wmv9.py: each function the same instructions as N&B's but for addresses and registers, at
// Culdcept's distances apart), so the statuses are Culdcept's (9 no more output, 10 GetData's last), not NiGHTS's:
//   decode      sub_824C8F40 (N&B WmvideoC9DecDecode)        get output  sub_824C8FA0 (WmvideoC9DecGetOutput)
//   flush       sub_824C8E38 (WmvideoC9DecReset)         destroy     sub_824C8E90 (WmvideoC9DecClose)
//   GetData     sub_824C8B78 (WMVDecCBGetData_HANDLER)   status      sub_824C8AA0 (N&B sub_82AA9070)
//   the stream node's payload fetch (mode 2)  sub_82494788 (WMDecCBGetData)
// None of them has a direct caller in the generated code: the hooks take every call.
// Changed from nights-nx's: the addresses and statuses; the cvars' names; FFmpeg's WMV3 decoder without
// AV_CODEC_FLAG_LOW_DELAY (Zuma's Revenge's and Bejeweled 3's intros are WMV3 main profile with B pictures: with the
// flag FFmpeg gives the pictures in decode order; without it, in display order with one picture held back, as for
// NiGHTS's VC-1; a movie without B pictures has low delay from its sequence header); no store-card marks (this
// port's generated code keeps none: g_banjo_store_cards is false, and the guest window's first MB is the game's).
// NiGHTS's NVDEC / VIC clock hold is kept (the user allowed it for video decoding: <game>_movie_max_clock).
//
// nights-nx (2026-11-22, the user: "help nights into dreams have l4z and zxc in its web installer and rencode video and
// audio to opus and h265"; culdcept-nx's app/src/cpu/culdcept_wmv_ffmpeg.cpp of 2026-11-19/21, credit it, whose method
// is sasrt-nx's and tw2de_nx's): NiGHTS's movies decoded on the host instead of by Microsoft's WMV9 decoder in guest
// code.
//
// NiGHTS's movies (afs/*.wmv, 22 files, 516 MB) are ASF with **VC-1 advanced profile** video (fourcc WVC1: 1280x720 or
// 640x480 at 59.94 fps, with B pictures) and WMA Pro audio; Culdcept's are WMV3 (main profile) without B pictures. The
// player and its codec interface are the same XDK library in a later build, found by the shape of Culdcept's functions
// (2026-11-22): the codec's interface C, +32 its wrapper V (+32 width, +36 height, +68 the WMV9 decoder, +72 the frame's
// pts, +84 / +88 the sequence header and its length, set by sub_82453DC0, +108 the frame's I420 size): destroy
// sub_82453A70(C), decode sub_82453B20(C, u32* pcb), get output sub_82453B80(C, buf, size, u32* pcb, u32*, u64* pts,
// u32* flag), flush sub_82453A18(C), GetData sub_82453758(C, offset, u8** p, min, u32* n, u32* not_end), the decoder's
// status to an HRESULT sub_82453678, the stream node's payload fetch sub_82427B40. This build's decoder statuses are
// not Culdcept's: 0 ok, 2 no more output (Culdcept 9), GetData's last chunk 6 (Culdcept 10).
//
// nights_wmv_ffmpeg (here <game>_wmv_ffmpeg): 0 the game's decoder; 1 (the default) FFmpeg (VC-1 or WMV3, the frame's chunks taken through
// GetData as the guest's decoder takes them); 2 both: the game's decoder runs and its picture is shown, FFmpeg decodes
// the same bytes (taken as the stream node hands them out) and each picture the game gives is compared with FFmpeg's
// last few ([wmv] lines), and every call's status logged (nights_wmv_log_calls).
//
// What the game's decoder shows (PC, 2026-11-22, mode 2 and nights_wmv_log_hashes): after each frame, the newest
// picture FFmpeg has given (with B pictures FFmpeg holds a reference picture back, and so does the game's decoder), at
// a movie's first frame that frame's own picture (FirstPicture), and no picture for a skipped frame (a one-byte
// payload). Mode 1 does the same: the intro's four movies (120, 135, 1,244 and 401 pictures) shown identical to the
// game's decoder, picture for picture and time for time, so it is the default.
//
// A movie whose first frame is H.265 (the installer re-encodes the video, frame for frame, keeping the ASF file, its
// audio, timing and the sequence header the game's player sets up from) is decoded by nxvideo (tools/switch/nxvideo:
// devkitPro's FFmpeg 7.1 with averne's nvtegra backend, on the Switch's NVDEC; its software HEVC decoder where NVDEC
// does not open) in mode 1 (mode 0 hands every frame to the game's decoder before its bytes are seen).
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/xmemory.h>
#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if PEGGLE1_NXVIDEO
#include <switch.h>  // svcGetThreadPriority / svcSetThreadPriority (peggle1_movie_decode_priority)

#include "nxvideo.h"
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
}

REXCVAR_DEFINE_INT32(peggle1_wmv_ffmpeg, 1, "Peggle1",
                     "The movies' WMV video: 0 the game's decoder (guest code), 1 FFmpeg on the host (the same "
                     "pictures), 2 both, compared ([wmv] log lines; the game's picture is shown). The installer's H.265 "
                     "movies need 1")
    .range(0, 2);
REXCVAR_DEFINE_INT32(peggle1_wmv_log_calls, 0, "Peggle1",
                     "With peggle1_wmv_ffmpeg 2: a [wmv] line for each of the first this many decode and get-output "
                     "calls of each movie (the game's statuses, sizes and times)")
    .range(0, 100000);
REXCVAR_DEFINE_INT32(peggle1_wmv_log_hashes, 0, "Peggle1",
                     "A [wmv] line with a hash of each of the first this many pictures a movie shows, in any mode (to "
                     "compare a run of the game's decoder with one of FFmpeg's)")
    .range(0, 100000);
REXCVAR_DEFINE_INT32(peggle1_movie_nvdec, 1, "Peggle1",
                     "The movies on the Switch's hardware decoder (NVDEC, through nxvideo): 0 none (software), 1 the "
                     "H.265 movies the installer made")
    .range(0, 1);
// culdcept-nx (2026-11-20): nxv_decode waits on the hardware on the game's thread; at the game's priority (0x3B, the
// lowest) it waits for a core behind the game's other threads, which Horizon does not time-slice.
REXCVAR_DEFINE_INT32(peggle1_movie_decode_priority, 0x2C, "Peggle1",
                     "The thread's priority while it waits for an H.265 frame on NVDEC (0x1C-0x3B, lower runs first; "
                     "the game's threads 0x3B); 0 = the thread's own")
    .range(0, 0x3B);
// peggle1-nx (2026-11-24, the user: "You can make the nvdev clock the max for video decoding thats different vs cpu
// gpu ram clocks"): kept, on. Only the video decoder (NVDEC) and the VIC that copies its pictures, only while an
// H.265 movie plays; the CPU, GPU and memory clocks are never changed.
// culdcept-nx (2026-11-20): nvtegra's DFS sets NVDEC's clock to the rate frames come at, and the game asks for a frame
// only after the last is shown: the clock fell until a frame took a whole display frame.
REXCVAR_DEFINE_BOOL(peggle1_movie_max_clock, true, "Peggle1",
                    "While an H.265 movie plays on NVDEC: NVDEC at the clock nvtegra's DFS starts from (its maximum) "
                    "and the VIC at its maximum. false: DFS's own clock");
// culdcept-nx (2026-11-21): the picture copied out of NVDEC's surface by the VIC into an aligned picture of nxvideo's
// own (FFmpeg's nvtegra copies with the CPU otherwise: 18 ms a 720p frame).
REXCVAR_DEFINE_BOOL(peggle1_movie_vic_copy, true, "Peggle1",
                    "H.265 movies on NVDEC: the picture copied out of NVDEC's surface by the VIC into an aligned NV12 "
                    "picture, the same bytes as the CPU's copy (false: FFmpeg's own picture, copied by the CPU)");
REXCVAR_DEFINE_BOOL(peggle1_movie_verify, false, "Peggle1",
                    "H.265 movies on NVDEC: every 30th VIC picture compared with FFmpeg's CPU copy of it (the log's "
                    "nxvideo line; each comparison costs a CPU copy: a test, not for playing)");
REXCVAR_DEFINE_STRING(peggle1_movie_verify_dump, "", "Peggle1",
                      "With peggle1_movie_verify: a folder the first compared pair of pictures is written to (I420), "
                      "e.g. sdmc:/switch/peggle1-nx/logs");

REX_EXTERN(__imp__sub_824C8F40);  // decode (C, u32* pcb)
REX_EXTERN(__imp__sub_824C8FA0);  // get output (C, buf, size, u32* pcb, u32*, u64* pts, u32* flag)
REX_EXTERN(__imp__sub_824C8E38);  // flush (C)
REX_EXTERN(__imp__sub_824C8E90);  // destroy (C)
REX_EXTERN(__imp__sub_824C8B78);  // GetData (C, offset, u8** p, min, u32* n, u32* not_end)
REX_EXTERN(__imp__sub_82494788);  // the stream node's payload fetch (S, u8** p, u32* n, ...)
REX_EXTERN(__imp__sub_824C8AA0);  // the decoder's status to an HRESULT

namespace {

uint32_t Ld32(const uint8_t *base, uint32_t va) {
  uint32_t v;
  std::memcpy(&v, base + va, 4);
  return __builtin_bswap32(v);
}
uint16_t Ld16(const uint8_t *base, uint32_t va) {
  uint16_t v;
  std::memcpy(&v, base + va, 2);
  return __builtin_bswap16(v);
}
uint64_t Ld64(const uint8_t *base, uint32_t va) {
  uint64_t v;
  std::memcpy(&v, base + va, 8);
  return __builtin_bswap64(v);
}
void St32(uint8_t *base, uint32_t va, uint32_t v) {
  if (!va)
    return;
  v = __builtin_bswap32(v);
  std::memcpy(base + va, &v, 4);
}
void St64(uint8_t *base, uint32_t va, uint64_t v) {
  if (!va)
    return;
  v = __builtin_bswap64(v);
  std::memcpy(base + va, &v, 8);
}

constexpr uint32_t kV = 32;             // C+32: the wrapper V
constexpr uint32_t kWidth = 32;         // V+32
constexpr uint32_t kHeight = 36;        // V+36
constexpr uint32_t kPts = 72;           // V+72 (u64)
constexpr uint32_t kSequence = 84;      // V+84: the sequence header (ASF stream properties' extra bytes)
constexpr uint32_t kSequenceLen = 88;   // V+88 (u16)
constexpr uint32_t kFrameSize = 108;    // V+108: width * height * 3 / 2
// Culdcept's and N&B's build of the decoder (find_wmv9.py; banjo-nx-ult's banjo_wmv_ffmpeg.cpp): 0 ok, 9 no more
// output; GetData: 0 more to come, 10 the last.
constexpr uint32_t kStatusOk = 0, kStatusNoMoreOutput = 9, kGetDataLast = 10;

// A picture FFmpeg gave, kept for mode 2's comparison (I420, w x h).
struct Kept {
  std::vector<uint8_t> i420;
  char type = '?';
  uint64_t index = 0;  // FFmpeg's output count
};

struct Movie {
  std::mutex mutex;
  AVCodecContext *avctx = nullptr;
  AVPacket *packet = nullptr;
  AVFrame *frame = nullptr;
  // 2026-11-22: the picture shown, as the game's decoder shows it: after each frame, the newest picture the decoder has
  // given (mode 2 found this on every compared picture: with B pictures FFmpeg holds a reference picture back, and the
  // game's decoder shows the same picture FFmpeg has given last); before the decoder gives any (the first frame of a
  // movie or after a flush), the first frame's picture drained from a decoder of its own (FirstPicture).
  AVFrame *shown = nullptr;
  bool fresh = true;               // the next frame is the first since the open or a flush
  bool vc1 = false;                // the sequence header is VC-1's (advanced profile, start codes)
  bool have = false;               // a picture to give get output
  std::vector<uint8_t> bytes;      // this frame's
  std::vector<uint8_t> captured;   // mode 2: what the guest's decoder took
  std::deque<Kept> kept;           // mode 2: FFmpeg's last pictures
  uint64_t outputs = 0;            // mode 2: FFmpeg's pictures so far
  uint64_t frames = 0, decoded = 0, failed = 0, damaged = 0, compared = 0, matched = 0;
  uint64_t calls = 0;              // mode 2: decode and get-output calls logged
  double decode_ms = 0.0, copy_ms = 0.0;
  double nxv_ms = 0.0;
  uint64_t timed = 0;
  std::chrono::steady_clock::time_point since{};
  uint32_t width = 0, height = 0;
  bool checked = false;  // the first frame looked at
  bool hevc = false;     // the movie's video is H.265
  bool nxv_used = false;
  bool black = false;    // an H.265 movie without a decoder (the PC, or nxvideo did not open): black pictures
#if PEGGLE1_NXVIDEO
  NxvDecoder *nxv = nullptr;
  NxvPicture pic{};
#endif
};

// An H.265 access unit in Annex B: a start code, then a VPS (32), SPS (33), PPS (34) or access unit delimiter (35).
// A VC-1 advanced profile frame starts with a frame start code (00 00 01 0D) or none (it is ASF's framing), never with
// one of these.
bool LooksHevc(const std::vector<uint8_t> &b) {
  size_t i = 0;
  if (b.size() >= 6 && b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1)
    i = 4;
  else if (b.size() >= 5 && b[0] == 0 && b[1] == 0 && b[2] == 1)
    i = 3;
  else
    return false;
  const uint8_t type = (b[i] >> 1) & 0x3F;
  return (b[i] & 0x80) == 0 && type >= 32 && type <= 35 && b[i + 1] == 1;  // forbidden bit 0, layer 0, temporal id 1
}

// The sequence header is VC-1 advanced profile's when it holds a sequence header start code (00 00 01 0F); WMV3's is
// 4 or 5 bytes of bits.
bool LooksVc1(const uint8_t *p, uint32_t n) {
  for (uint32_t i = 0; i + 3 < n; ++i)
    if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && p[i + 3] == 0x0F)
      return true;
  return false;
}

std::mutex g_movies_mutex;
std::unordered_map<uint32_t, Movie *> g_movies;  // by the guest codec's address

Movie &MovieFor(uint32_t c) {
  std::lock_guard lock(g_movies_mutex);
  Movie *&m = g_movies[c];
  if (!m)
    m = new Movie();
  return *m;
}

void CloseDecoder(Movie &m) {
#if PEGGLE1_NXVIDEO
  if (m.nxv)
    nxv_close(m.nxv);
  m.nxv = nullptr;
#endif
  m.checked = m.hevc = m.nxv_used = m.black = false;
  if (m.frame)
    av_frame_free(&m.frame);
  if (m.shown)
    av_frame_free(&m.shown);
  if (m.packet)
    av_packet_free(&m.packet);
  if (m.avctx)
    avcodec_free_context(&m.avctx);
  m.have = false;
  m.fresh = true;
  m.kept.clear();
  m.outputs = 0;
}

// A VC-1 (or WMV3) decoder for the movie's sequence header (the ASF codec data the guest keeps at V+84), or nullptr.
AVCodecContext *OpenContext(const uint8_t *seq, uint32_t seq_len, uint32_t width, uint32_t height, bool vc1) {
  const AVCodec *codec = avcodec_find_decoder(vc1 ? AV_CODEC_ID_VC1 : AV_CODEC_ID_WMV3);
  if (!codec) {
    static std::atomic<bool> said{false};
    if (!said.exchange(true))
      REXLOG_WARN("[wmv] FFmpeg has no {} decoder in this build: the game's decoder plays the movies",
                  vc1 ? "VC-1" : "WMV3");
    return nullptr;
  }
  AVCodecContext *c = avcodec_alloc_context3(codec);
  c->width = int(width);
  c->height = int(height);
  c->coded_width = int(width);
  c->coded_height = int(height);
  // peggle1-nx (2026-11-24): no AV_CODEC_FLAG_LOW_DELAY for WMV3 either (nights-nx and culdcept-nx set it for WMV3,
  // whose movies had no B pictures): with B pictures the flag makes FFmpeg give them in decode order. Without B
  // pictures the sequence header gives low delay by itself.
  c->thread_count = 1;
  if (seq && seq_len && seq_len < 256) {
    c->extradata = static_cast<uint8_t *>(av_mallocz(seq_len + AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(c->extradata, seq, seq_len);
    c->extradata_size = int(seq_len);
  }
  if (avcodec_open2(c, codec, nullptr) < 0) {
    REXLOG_WARN("[wmv] FFmpeg's {} decoder did not open ({}x{}, sequence header of {} bytes)", vc1 ? "VC-1" : "WMV3",
                width, height, seq_len);
    avcodec_free_context(&c);
    return nullptr;
  }
  return c;
}

// The picture of a first frame alone: a decoder of its own given the frame and drained (the movie's own decoder holds
// a VC-1 reference picture back until the next frame, while the game's shows it at once). Into m.shown.
bool FirstPicture(Movie &m, const uint8_t *base, uint32_t v, const std::vector<uint8_t> &bytes) {
  const uint32_t seq = Ld32(base, v + kSequence), seq_len = Ld16(base, v + kSequenceLen);
  AVCodecContext *c = OpenContext(seq ? base + seq : nullptr, seq_len, m.width, m.height, m.vc1);
  if (!c)
    return false;
  AVPacket *p = av_packet_alloc();
  AVFrame *f = av_frame_alloc();
  bool got = false;
  if (av_new_packet(p, int(bytes.size())) >= 0) {
    std::memcpy(p->data, bytes.data(), bytes.size());
    if (avcodec_send_packet(c, p) >= 0 && avcodec_send_packet(c, nullptr) >= 0) {
      while (avcodec_receive_frame(c, f) >= 0) {
        if (!got) {  // the first one out: the frame's own picture
          av_frame_unref(m.shown);
          av_frame_move_ref(m.shown, f);
          got = true;
        } else {
          av_frame_unref(f);
        }
      }
    }
  }
  av_frame_free(&f);
  av_packet_free(&p);
  avcodec_free_context(&c);
  return got;
}

// FFmpeg's VC-1 (or WMV3) decoder for this movie.
bool OpenDecoder(Movie &m, const uint8_t *base, uint32_t v) {
  if (m.avctx)
    return true;
  const uint32_t seq = Ld32(base, v + kSequence), seq_len = Ld16(base, v + kSequenceLen);
  m.vc1 = seq && seq_len && LooksVc1(base + seq, seq_len);
  m.width = Ld32(base, v + kWidth);
  m.height = Ld32(base, v + kHeight);
  m.avctx = OpenContext(seq ? base + seq : nullptr, seq_len, m.width, m.height, m.vc1);
  if (!m.avctx)
    return false;
  m.packet = av_packet_alloc();
  m.frame = av_frame_alloc();
  m.shown = av_frame_alloc();
  m.fresh = true;
  std::string hex;
  for (uint32_t i = 0; i < seq_len && i < 24; ++i)
    hex += fmt::format("{:02X} ", base[seq + i]);
  REXLOG_INFO("[wmv] FFmpeg's {} decoder for a {}x{} movie (sequence header of {} bytes: {}; peggle1_wmv_ffmpeg {})",
              m.vc1 ? "VC-1" : "WMV3", m.width, m.height, seq_len, hex, REXCVAR_GET(peggle1_wmv_ffmpeg));
  return true;
}

// nxvideo for an H.265 movie: false when it is not one.
bool NxvDecode(Movie &m, const uint8_t *base, uint32_t v, const std::vector<uint8_t> &bytes) {
  if (!m.checked) {
    m.checked = true;
    m.hevc = LooksHevc(bytes);
    m.nxv_used = m.hevc;
#if PEGGLE1_NXVIDEO
    if (m.nxv_used) {
      m.width = Ld32(base, v + kWidth);
      m.height = Ld32(base, v + kHeight);
      char why[160] = {};
      nxv_options(REXCVAR_GET(peggle1_movie_max_clock) ? 1 : 0, REXCVAR_GET(peggle1_movie_vic_copy) ? 1 : 0,
                  REXCVAR_GET(peggle1_movie_verify) ? 1 : 0, REXCVAR_GET(peggle1_movie_verify_dump).c_str());
      m.nxv = nxv_open(NXV_HEVC, nullptr, 0, int(m.width), int(m.height), REXCVAR_GET(peggle1_movie_nvdec) >= 1, why,
                       sizeof(why));
      REXLOG_INFO("[wmv] an H.265 movie: {}", m.nxv ? why : "did not open: shown black at its own pace");
      m.black = !m.nxv;
    }
#else
    if (m.hevc)
      REXLOG_WARN("[wmv] an H.265 movie, and this build has no H.265 decoder (nxvideo is the Switch's): shown black at "
                  "its own pace");
    m.black = m.hevc;
#endif
  }
  if (!m.nxv_used)
    return false;
  if (m.black) {
    // 2026-11-22 (PC, test_full): a movie no decoder gives a picture for, answered "no picture" frame after frame, ran
    // through in an instant, and the title's looping one held the game (no frame drawn after it started). Black
    // pictures at the movie's own times instead: the game goes on as with a movie.
    m.have = true;
    ++m.decoded;
    return true;
  }
#if PEGGLE1_NXVIDEO
  const int boost = REXCVAR_GET(peggle1_movie_decode_priority);
  s32 own = 0;
  const bool raised = boost > 0 && R_SUCCEEDED(svcGetThreadPriority(&own, CUR_THREAD_HANDLE)) && own > boost &&
                      R_SUCCEEDED(svcSetThreadPriority(CUR_THREAD_HANDLE, u32(boost)));
  const auto t0 = std::chrono::steady_clock::now();
  const int r = m.nxv ? nxv_decode(m.nxv, bytes.data(), int(bytes.size()), &m.pic) : -1;
  m.nxv_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (raised)
    svcSetThreadPriority(CUR_THREAD_HANDLE, u32(own));
  m.have = r == 1;
#else
  m.have = false;
#endif
  if (m.have)
    ++m.decoded;
  else if (++m.failed <= 8)
    REXLOG_WARN("[wmv] nxvideo gave no picture for frame {} ({} bytes)", m.frames, bytes.size());
  return true;
}

// FFmpeg's picture f as I420 of w x h (mode 2's copy); index 0 is a first frame's own picture (FirstPicture).
void KeepPicture(Movie &m, const AVFrame *f, uint32_t w, uint32_t h, bool first = false) {
  Kept k;
  k.i420.assign(size_t(w) * h * 3 / 2, 0);
  const uint32_t fw = std::min<uint32_t>(w, uint32_t(f->width)), fh = std::min<uint32_t>(h, uint32_t(f->height));
  const size_t planes[3] = {0, size_t(w) * h, size_t(w) * h + size_t(w / 2) * (h / 2)};
  for (int p = 0; p < 3; ++p) {
    const uint32_t pw = p ? fw / 2 : fw, ph = p ? fh / 2 : fh, pitch = p ? w / 2 : w;
    for (uint32_t y = 0; y < ph; ++y)
      std::memcpy(k.i420.data() + planes[p] + size_t(y) * pitch, f->data[p] + size_t(y) * size_t(f->linesize[p]), pw);
  }
  k.type = first ? 'F' : av_get_picture_type_char(f->pict_type);
  k.index = first ? 0 : ++m.outputs;
  m.kept.push_back(std::move(k));
  while (m.kept.size() > 5)
    m.kept.pop_front();
}

// One frame's bytes through FFmpeg: m.shown, the picture to show for it (see Movie::shown), and m.have (mode 2: every
// picture out kept).
void Decode(Movie &m, const uint8_t *base, uint32_t v, const std::vector<uint8_t> &bytes, bool keep) {
  m.have = false;
  ++m.frames;
  if (!bytes.empty() && NxvDecode(m, base, v, bytes))
    return;
  if (bytes.empty() || !OpenDecoder(m, base, v)) {
    ++m.failed;
    return;
  }
  av_packet_unref(m.packet);
  if (av_new_packet(m.packet, int(bytes.size())) < 0)
    return;
  std::memcpy(m.packet->data, bytes.data(), bytes.size());
  bool got = false;
  if (avcodec_send_packet(m.avctx, m.packet) >= 0) {
    while (avcodec_receive_frame(m.avctx, m.frame) >= 0) {
      got = true;
      ++m.decoded;
      if (m.frame->decode_error_flags || (m.frame->flags & AV_FRAME_FLAG_CORRUPT))
        ++m.damaged;
      if (keep && m.frame->format == AV_PIX_FMT_YUV420P)
        KeepPicture(m, m.frame, m.width, m.height);
      av_frame_unref(m.shown);
      av_frame_move_ref(m.shown, m.frame);  // the newest
    }
  }
  const bool fresh = m.fresh;
  m.fresh = false;
  if (!got && fresh && FirstPicture(m, base, v, bytes)) {
    got = true;
    if (keep && m.shown->format == AV_PIX_FMT_YUV420P)
      KeepPicture(m, m.shown, m.width, m.height, true);
  }
  m.have = m.shown->data[0] != nullptr;  // a frame that gave none shows the last picture again
  if (!got && ++m.failed <= 8)
    REXLOG_INFO("[wmv] FFmpeg gave no new picture for frame {} ({} bytes): {}", m.frames, bytes.size(),
                m.have ? "the last one is shown again" : "nothing to show");
}

// The frame's chunks through the guest's GetData (C, 0, &p, 4, &n, &not_end) until its last, as the guest's decoder
// takes them: the pts (V+72) and the payload's release stay the guest's. Out-arguments in a scratch area below the stack.
void TakeFrame(PPCContext &ctx, uint8_t *base, uint32_t c, std::vector<uint8_t> &bytes) {
  bytes.clear();
  const uint32_t r1 = ctx.r1.u32;
  const uint32_t frame = r1 - 128;
  const uint32_t p_out = frame + 80, n_out = frame + 84, end_out = frame + 88;
  for (int i = 0; i < 4096; ++i) {
    ctx.r1.u64 = frame;
    ctx.r3.u64 = c;
    ctx.r4.u64 = 0;
    ctx.r5.u64 = p_out;
    ctx.r6.u64 = 4;
    ctx.r7.u64 = n_out;
    ctx.r8.u64 = end_out;
    __imp__sub_824C8B78(ctx, base);
    const uint32_t status = ctx.r3.u32;
    const uint32_t p = Ld32(base, p_out), n = Ld32(base, n_out), not_end = Ld32(base, end_out);
    if (p && n)
      bytes.insert(bytes.end(), base + p, base + p + n);
    if ((status != 0 && status != kGetDataLast) || !n || !not_end)
      break;
  }
  ctx.r1.u64 = r1;
}

// FFmpeg's picture (YUV420P) into the player's I420 buffer: Y pitch W, then U and V pitch W / 2.
void WriteI420(Movie &m, uint8_t *base, uint32_t buf, uint32_t w, uint32_t h) {
  const AVFrame *f = m.shown;
  const uint32_t fw = std::min<uint32_t>(w, uint32_t(f->width)), fh = std::min<uint32_t>(h, uint32_t(f->height));
  const uint32_t planes[3] = {buf, buf + w * h, buf + w * h + (w / 2) * (h / 2)};
  for (int k = 0; k < 3; ++k) {
    const uint32_t pw = k ? fw / 2 : fw, ph = k ? fh / 2 : fh, pitch = k ? w / 2 : w;
    for (uint32_t y = 0; y < ph; ++y)
      std::memcpy(base + planes[k] + y * pitch, f->data[k] + size_t(y) * size_t(f->linesize[k]), pw);
  }
}

#if PEGGLE1_NXVIDEO
// nxvideo's picture (YUV420P, or NV12 from NVDEC) into the player's I420 buffer.
void WriteI420Nxv(const NxvPicture &p, uint8_t *base, uint32_t buf, uint32_t w, uint32_t h) {
  const uint32_t fw = std::min<uint32_t>(w, uint32_t(p.width)), fh = std::min<uint32_t>(h, uint32_t(p.height));
  const uint32_t y_plane = buf, u_plane = buf + w * h, v_plane = buf + w * h + (w / 2) * (h / 2);
  for (uint32_t y = 0; y < fh; ++y)
    std::memcpy(base + y_plane + y * w, p.data[0] + size_t(y) * size_t(p.linesize[0]), fw);
  if (p.nv12) {
    for (uint32_t y = 0; y < fh / 2; ++y) {
      const uint8_t *uv = p.data[1] + size_t(y) * size_t(p.linesize[1]);
      uint8_t *u = base + u_plane + y * (w / 2), *vv = base + v_plane + y * (w / 2);
      for (uint32_t x = 0; x < fw / 2; ++x) {
        u[x] = uv[2 * x];
        vv[x] = uv[2 * x + 1];
      }
    }
  } else {
    for (uint32_t y = 0; y < fh / 2; ++y) {
      std::memcpy(base + u_plane + y * (w / 2), p.data[1] + size_t(y) * size_t(p.linesize[1]), fw / 2);
      std::memcpy(base + v_plane + y * (w / 2), p.data[2] + size_t(y) * size_t(p.linesize[2]), fw / 2);
    }
  }
}

void NxvLine(Movie &m, uint32_t c) {
  if (!m.nxv) return;
  NxvStats st;
  nxv_stats(m.nxv, &st);
  if (!st.frames) return;
  const double n = double(st.frames);
  REXLOG_INFO("[wmv] {:08X}: nxvideo a frame ({}): send {:.2f} ms, receive {:.2f} ms, transfer {:.2f} ms; NVDEC {} MHz "
              "(DFS's start {} MHz, put back {} times), VIC {} MHz; copied out by the VIC {}, by the CPU {}{}",
              c, st.frames, st.send_ms / n, st.receive_ms / n, st.transfer_ms / n, st.nvdec_hz / 1000000,
              st.nvdec_max_hz / 1000000, st.restores, st.vic_hz / 1000000, st.vic_frames, st.cpu_frames,
              st.checked ? fmt::format("; verify: {} pictures compared with the CPU's copy, {} {} ({} samples)",
                                       st.checked, st.checked_differ, st.checked_differ ? "DIFFERENT" : "different",
                                       st.checked_samples)
                         : std::string());
}
#endif

void TimeLine(Movie &m, uint32_t c, double decode_ms, double copy_ms) {
  const auto now = std::chrono::steady_clock::now();
  m.decode_ms += decode_ms;
  m.copy_ms += copy_ms;
  ++m.timed;
  if (m.since.time_since_epoch().count() == 0)
    m.since = now;
  if (now - m.since >= std::chrono::seconds(10)) {
    REXLOG_INFO("[wmv] {:08X}: {} frames on the host in {:.1f} s ({}x{}): {:.2f} ms decoding ({:.2f} of it in nxvideo) "
                "and {:.2f} ms copying a frame; {} decoded, {} not, {} damaged so far",
                c, m.timed, std::chrono::duration<double>(now - m.since).count(), m.width, m.height,
                m.decode_ms / double(m.timed), m.nxv_ms / double(m.timed), m.copy_ms / double(m.timed), m.decoded,
                m.failed, m.damaged);
    m.decode_ms = m.copy_ms = m.nxv_ms = 0.0;
#if PEGGLE1_NXVIDEO
    NxvLine(m, c);
#endif
    m.timed = 0;
    m.since = now;
  }
}

bool LogCall(Movie &m) {
  static const uint64_t limit = uint64_t(std::max(0, int(REXCVAR_GET(peggle1_wmv_log_calls))));
  return m.calls++ < limit;
}

// peggle1_wmv_log_hashes: the picture get output left in buf (FNV-1a 64 of its I420), the n-th of the movie.
std::mutex g_hash_mutex;
std::unordered_map<uint32_t, uint64_t> g_shown;  // pictures shown so far, by the guest codec's address
void LogShown(const uint8_t *base, uint32_t c, uint32_t buf, uint32_t size_out, uint32_t w, uint32_t h, uint64_t pts) {
  static const uint64_t limit = uint64_t(std::max(0, int(REXCVAR_GET(peggle1_wmv_log_hashes))));
  if (!limit)
    return;
  uint64_t n;
  {
    std::lock_guard lock(g_hash_mutex);
    n = ++g_shown[c];
  }
  if (n > limit)
    return;
  uint64_t hash = 0xcbf29ce484222325ull;
  if (size_out)
    for (size_t i = 0, e = size_t(w) * h * 3 / 2; i < e; ++i)
      hash = (hash ^ base[buf + i]) * 0x100000001b3ull;
  REXLOG_INFO("[wmv] shown {:08X} #{} {}x{} pts {}: {}", c, n, w, h, pts,
              size_out ? fmt::format("{:016X}", hash) : std::string("none"));
}
void ForgetShown(uint32_t c) {
  std::lock_guard lock(g_hash_mutex);
  g_shown.erase(c);
}

thread_local Movie *t_capture = nullptr;  // mode 2: the movie whose decode call is running on this thread

}  // namespace

// Decode: one frame.
REX_HOOK_RAW(sub_824C8F40) {
  const int mode = REXCVAR_GET(peggle1_wmv_ffmpeg);
  const uint32_t c = ctx.r3.u32, pcb = ctx.r4.u32;
  const uint32_t v = Ld32(base, c + kV);
  Movie &m = MovieFor(c);
  std::lock_guard lock(m.mutex);
  static std::atomic<bool> said{false};
  if (!said.exchange(true))
    REXLOG_INFO("[wmv] the movies' video: {} (peggle1_wmv_ffmpeg {})",
                mode == 0 ? "the game's decoder"
                          : mode == 1 ? "FFmpeg on the host" : "the game's decoder and FFmpeg, compared",
                mode);
  if (mode == 0) {
    // The game's decoder for everything (the installer's H.265 movies need mode 1: only it takes the frame's bytes
    // before the game's decoder does).
    __imp__sub_824C8F40(ctx, base);
    return;
  }
  if (mode == 2) {
    m.captured.clear();
    t_capture = &m;
    __imp__sub_824C8F40(ctx, base);  // the game's decoder; its payload fetches captured
    t_capture = nullptr;
    const uint32_t result = ctx.r3.u32;
    if (LogCall(m))
      REXLOG_INFO("[wmv] {:08X} decode: the game's HRESULT {:08X}, size out {}, pts {} ({} bytes taken)", c, result,
                  Ld32(base, pcb), Ld64(base, v + kPts), m.captured.size());
    Decode(m, base, v, m.captured, true);
    ctx.r3.u64 = result;
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  TakeFrame(ctx, base, c, m.bytes);
  if (m.bytes.empty()) {
    // Nothing more in the stream: the game's decoder answers as it would (it finds no bytes either).
    ctx.r3.u64 = c;
    ctx.r4.u64 = pcb;
    __imp__sub_824C8F40(ctx, base);
    return;
  }
  // 2026-11-22: a skipped frame (a payload of one byte in NiGHTS's movies): the game's decoder answers S_OK with no
  // picture, and the player asks for none (PC, the 640x480 logo: decode 2, "size out 0"). FFmpeg still takes it, so its
  // later pictures stay the game's; the installer keeps it as it is in an H.265 movie, where nxvideo never sees it.
  const bool skipped = m.bytes.size() <= 1;
  if (skipped && m.checked && m.hevc)
    ++m.frames;
  else
    Decode(m, base, v, m.bytes, false);
  const auto t1 = std::chrono::steady_clock::now();
  if (skipped)
    m.have = false;
  St32(base, pcb, m.have ? Ld32(base, v + kFrameSize) : 0);
  ctx.r3.u64 = (m.have || skipped) ? kStatusOk : kStatusNoMoreOutput;
  __imp__sub_824C8AA0(ctx, base);
  if (m.have)
    TimeLine(m, c, std::chrono::duration<double, std::milli>(t1 - t0).count(), 0.0);
}

// Get output: the decoded frame into the player's buffer (I420).
REX_HOOK_RAW(sub_824C8FA0) {
  const int mode = REXCVAR_GET(peggle1_wmv_ffmpeg);
  const uint32_t c = ctx.r3.u32, buf = ctx.r4.u32, size = ctx.r5.u32, pcb = ctx.r6.u32, p7 = ctx.r7.u32,
                 pts = ctx.r8.u32, flag = ctx.r9.u32;
  const uint32_t v = Ld32(base, c + kV);
  const uint32_t w = Ld32(base, v + kWidth), h = Ld32(base, v + kHeight);
  if (mode == 0) {
    __imp__sub_824C8FA0(ctx, base);
    LogShown(base, c, buf, pcb ? Ld32(base, pcb) : 0, w, h, pts ? Ld64(base, pts) : 0);
    return;
  }
  Movie &m = MovieFor(c);
  std::lock_guard lock(m.mutex);
  if (mode == 2) {
    __imp__sub_824C8FA0(ctx, base);  // the game's picture into buf
    const uint32_t result = ctx.r3.u32;
    const uint32_t out_size = pcb ? Ld32(base, pcb) : 0;
    LogShown(base, c, buf, out_size, w, h, pts ? Ld64(base, pts) : 0);
    if (LogCall(m))
      REXLOG_INFO("[wmv] {:08X} get output: the game's HRESULT {:08X}, size {}, p7 {}, pts {}, flag {}", c, result,
                  out_size, p7 ? Ld32(base, p7) : 0, pts ? Ld64(base, pts) : 0, flag ? Ld32(base, flag) : 0);
    if (out_size != 0 && !m.kept.empty() && m.compared < 400) {
      ++m.compared;
      // The game's picture against each of FFmpeg's last ones: the newest that matches, or the closest.
      const size_t n = size_t(w) * h * 3 / 2;
      std::string line;
      int match = -1;
      uint64_t best = ~uint64_t(0);
      int best_at = -1;
      for (int i = int(m.kept.size()) - 1; i >= 0; --i) {
        const Kept &k = m.kept[size_t(i)];
        if (k.i420.size() != n) continue;
        uint64_t differing = 0;
        for (size_t s = 0; s < n; ++s) differing += base[buf + s] != k.i420[s];
        line += fmt::format(" #{}({}) {}", k.index, k.type, differing);
        if (!differing && match < 0) match = int(k.index);
        if (differing < best) {
          best = differing;
          best_at = int(k.index);
        }
      }
      if (match >= 0) ++m.matched;
      if (m.compared <= 40 || match < 0)
        REXLOG_INFO("[wmv] {:08X} picture {} ({}x{}): game vs FFmpeg's pictures (samples that differ):{}; {}", c,
                    m.compared, w, h, line,
                    match >= 0 ? fmt::format("identical to #{} (FFmpeg has given {})", match, m.outputs)
                               : fmt::format("NONE identical, the closest #{}", best_at));
    }
    m.have = false;
    ctx.r3.u64 = result;
    return;
  }
  const uint32_t frame_size = Ld32(base, v + kFrameSize);
  if (frame_size > size || (h & 1)) {  // as the game's: E_ too small / odd height
    ctx.r3.u64 = 0x80500006u;
    return;
  }
  uint32_t status = kStatusNoMoreOutput;
  if (m.nxv_used) {
    if (m.have && m.black) {
      // black (Y 16, U and V 128) in place of a picture no decoder gave
      std::memset(base + buf, 16, size_t(w) * h);
      std::memset(base + buf + size_t(w) * h, 128, size_t(w) * h / 2);
      St32(base, pcb, frame_size);
      status = kStatusOk;
    } else
#if PEGGLE1_NXVIDEO
    if (m.have) {
      const auto t0 = std::chrono::steady_clock::now();
      WriteI420Nxv(m.pic, base, buf, w, h);
      m.copy_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      St32(base, pcb, frame_size);
      status = kStatusOk;
    } else
#endif
    {
      St32(base, pcb, 0);
    }
    St32(base, p7, 0);
  } else if (m.have && m.shown->format == AV_PIX_FMT_YUV420P) {
    const auto t0 = std::chrono::steady_clock::now();
    WriteI420(m, base, buf, w, h);
    m.copy_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    St32(base, pcb, frame_size);
    St32(base, p7, 0);
    status = kStatusOk;
  } else {
    if (m.have && m.shown->format != AV_PIX_FMT_YUV420P) {
      static std::atomic<int> said{0};
      if (said.fetch_add(1) < 4)
        REXLOG_WARN("[wmv] FFmpeg's picture is in pixel format {}, not YUV420P: not shown", int(m.shown->format));
    }
    St32(base, pcb, 0);
    St32(base, p7, 0);
  }
  m.have = false;
  St64(base, pts, Ld64(base, v + kPts));
  St32(base, flag, 1);
  LogShown(base, c, buf, status == kStatusOk ? frame_size : 0, w, h, Ld64(base, v + kPts));
  ctx.r3.u64 = status;
  __imp__sub_824C8AA0(ctx, base);
}

// Flush (a seek, a stop): FFmpeg's references go too.
REX_HOOK_RAW(sub_824C8E38) {
  const uint32_t c = ctx.r3.u32;
  {
    Movie &m = MovieFor(c);
    std::lock_guard lock(m.mutex);
    if (m.avctx)
      avcodec_flush_buffers(m.avctx);
#if PEGGLE1_NXVIDEO
    if (m.nxv)
      nxv_flush(m.nxv);
#endif
    if (m.shown)
      av_frame_unref(m.shown);
    m.fresh = true;  // the next frame is shown as a first one (FirstPicture)
    m.have = false;
    m.kept.clear();
  }
  __imp__sub_824C8E38(ctx, base);
}

// Destroy: the movie's decoder freed (the next movie's codec may take the same address).
REX_HOOK_RAW(sub_824C8E90) {
  const uint32_t c = ctx.r3.u32;
  {
    Movie &m = MovieFor(c);
    std::lock_guard lock(m.mutex);
    if ((m.avctx || m.nxv_used) && (m.frames || m.failed)) {
#if PEGGLE1_NXVIDEO
      const char *by = m.nxv_used ? (m.nxv && nxv_is_hw(m.nxv) ? "NVDEC" : "nxvideo (software)") : "FFmpeg";
#else
      const char *by = m.nxv_used ? "nothing (no H.265 decoder)" : "FFmpeg";
#endif
      REXLOG_INFO("[wmv] {:08X}: movie closed{}: {} frames, {} pictures by {}, {} without one, {} damaged; compared {}, "
                  "identical {}",
                  c, m.hevc ? " (H.265)" : (m.vc1 ? " (VC-1)" : ""), m.frames, m.decoded, by, m.failed, m.damaged,
                  m.compared, m.matched);
#if PEGGLE1_NXVIDEO
      NxvLine(m, c);
#endif
    }
    CloseDecoder(m);
    ForgetShown(c);
    m.frames = m.decoded = m.failed = m.damaged = m.compared = m.matched = m.calls = 0;
    m.timed = 0;
    m.since = {};
  }
  __imp__sub_824C8E90(ctx, base);
}

// Mode 2: the payload fragments the stream node hands the game's decoder, taken as they go (the video decode thread's
// decode call only: t_capture).
REX_HOOK_RAW(sub_82494788) {
  Movie *m = t_capture;
  const uint32_t pp = ctx.r4.u32, pn = ctx.r5.u32;
  __imp__sub_82494788(ctx, base);
  if (m && int32_t(ctx.r3.u32) >= 0 && pp && pn) {
    const uint32_t p = Ld32(base, pp), n = Ld32(base, pn);
    if (p && n && n < (16u << 20))
      m->captured.insert(m->captured.end(), base + p, base + p + n);
  }
}
