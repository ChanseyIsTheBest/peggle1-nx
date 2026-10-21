// Short video recordings of the game image (see peggle1_capture.h).
//
// From peggle1_video_s seconds after start (or from game frame peggle1_video_from_frame), peggle1_video_frames
// consecutive presented frames are written as they come to captures/video_<s>s.rgba (video_f<frame>.rgba) next to the
// executable (raw R8G8B8X8, frames back to back; streamed, so a long recording does not need RAM, ~55 MB per second at
// half resolution on disk) with a .txt beside it (width, height, measured fps, frame count) and a .frames file (the
// game frame number of each recorded frame, one per line). tools/make_video.sh turns it into an MP4 with ffmpeg;
// tools/compare_raw.py pairs two recordings by game frame.

#include "peggle1_capture.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>

REXCVAR_DEFINE_INT32(peggle1_video_s, 0, "Peggle1",
                     "Record a short video of the game image this many seconds after start, to captures/ next to "
                     "the executable (0 = off; works minimized). tools/make_video.sh makes the MP4")
    .range(0, 3600);
REXCVAR_DEFINE_INT32(peggle1_video_frames, 300, "Peggle1", "Frames in the video (peggle1_video_s); the game presents 30 fps")
    .range(1, 36000);
REXCVAR_DEFINE_INT32(peggle1_video_from_frame, 0, "Peggle1",
                     "Record the video from this game frame on (D3DDevice_Swap count) instead of peggle1_video_s: two "
                     "runs with the same input script then record the same game frames (0 = off)")
    .range(0, 10000000);
REXCVAR_DEFINE_BOOL(peggle1_video_half, true, "Peggle1",
                    "Record the video at half resolution (~55 MB of disk per second instead of ~220 MB)");

std::atomic<uint64_t> g_peggle1_game_frames{0};

REXCVAR_DEFINE_INT32(peggle1_shots_every_s, 0, "Peggle1",
                     "Test: one frame every N seconds into shots/ next to the executable (raw RGBA; 0 = off)")
    .range(0, 3600);

namespace peggle1::capture {
namespace {

std::mutex g_mutex;
std::condition_variable g_cv;
bool g_stop = false;
std::thread g_thread;

bool StopRequested() {
  std::lock_guard lock(g_mutex);
  return g_stop;
}

void Record(Grabber grab, int start_s, int from_frame, int frame_count) {
  if (from_frame > 0) {
    while (g_peggle1_game_frames.load(std::memory_order_relaxed) + 1 < uint64_t(from_frame)) {
      if (StopRequested()) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  } else {
    std::unique_lock lock(g_mutex);
    if (g_cv.wait_for(lock, std::chrono::seconds(start_s), [] { return g_stop; })) {
      return;
    }
  }
  const uint32_t step = REXCVAR_GET(peggle1_video_half) ? 2 : 1;
  const auto folder = rex::filesystem::GetExecutableFolder() / "captures";
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  const std::string name =
      from_frame > 0 ? "video_f" + std::to_string(from_frame) : "video_" + std::to_string(start_s) + "s";
  std::ofstream frames_file(folder / (name + ".frames"), std::ios::binary | std::ios::trunc);
  std::ofstream raw(folder / (name + ".rgba"), std::ios::binary | std::ios::trunc);
  if (!raw) {
    REXLOG_WARN("[capture] cannot write {}", rex::path_to_utf8(folder / (name + ".rgba")));
    return;
  }
  std::vector<uint8_t> frame;
  int written = 0;
  uint64_t skipped_flips = 0;  // flips that came and went while a capture was still being taken
  uint32_t width = 0, height = 0;
  const auto started = std::chrono::steady_clock::now();
  uint64_t last = g_peggle1_game_frames.load(std::memory_order_relaxed);
  REXLOG_INFO("[capture] recording {} frames from {}", frame_count,
              from_frame > 0 ? "game frame " + std::to_string(from_frame) : std::to_string(start_s) + " s");
  while (written < frame_count && !StopRequested()) {
    // One capture per game frame, a few ms after the flip so the presenter holds the new image.
    const uint64_t now = g_peggle1_game_frames.load(std::memory_order_relaxed);
    if (now == last) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    if (written) {
      skipped_flips += now - last - 1;
    }
    last = now;
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
    rex::ui::RawImage image;
    if (!grab || !grab(image) || !image.width || !image.height ||
        (width && (image.width != width || image.height != height))) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    width = image.width;
    height = image.height;
    const uint32_t w = width / step, h = height / step;
    frame.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
      const uint8_t* row = image.data.data() + size_t(y) * step * image.stride;
      uint8_t* out = frame.data() + size_t(y) * w * 4;
      if (step == 1) {
        std::memcpy(out, row, size_t(w) * 4);
      } else {
        for (uint32_t x = 0; x < w; ++x) {
          std::memcpy(out + size_t(x) * 4, row + size_t(x) * step * 4, 4);
        }
      }
    }
    raw.write(reinterpret_cast<const char*>(frame.data()), std::streamsize(frame.size()));
    frames_file << now << '\n';
    ++written;
    if (written % 600 == 0) {
      REXLOG_INFO("[capture] {} / {} frames", written, frame_count);
    }
  }
  raw.close();
  if (!written) {
    REXLOG_WARN("[capture] no frame recorded (no presenter or no guest output)");
    return;
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  const double fps = seconds > 0 ? double(written) / seconds : 60.0;
  std::ofstream(folder / (name + ".txt"), std::ios::binary)
      << width / step << ' ' << height / step << ' ' << fps << ' ' << written << '\n';
  REXLOG_INFO("[capture] video {}: {} frames of {}x{} in {:.2f} s ({:.1f} fps, {} flips skipped by the recorder) -> {}",
              name, written, width / step, height / step, seconds, fps, skipped_flips,
              rex::path_to_utf8(folder / (name + ".rgba")));
}

// peggle1_shots_every_s (2026-10-19, for the emulator and console runs of Renut's renderer): one frame every N seconds
// as <exe folder>/shots/shot_<seconds>s_<width>x<height>.rgba (raw RGBA; tools turn them into PNGs).
std::thread g_shots;

void Shots(Grabber grab, int every_s) {
  const auto folder = rex::filesystem::GetExecutableFolder() / "shots";
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  for (int n = 1; n <= 200; ++n) {
    {
      std::unique_lock lock(g_mutex);
      if (g_cv.wait_for(lock, std::chrono::seconds(every_s), [] { return g_stop; })) return;
    }
    rex::ui::RawImage image;
    if (!grab || !grab(image) || !image.width || !image.height) {
      REXLOG_WARN("[capture] shot at {} s: no image", n * every_s);
      continue;
    }
    const auto path = folder / ("shot_" + std::to_string(n * every_s) + "s_" + std::to_string(image.width) + "x" +
                                std::to_string(image.height) + ".rgba");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    for (uint32_t y = 0; y < image.height; ++y) {
      out.write(reinterpret_cast<const char*>(image.data.data() + size_t(y) * image.stride),
                std::streamsize(size_t(image.width) * 4));
    }
    REXLOG_INFO("[capture] shot at {} s ({} game frames) -> {}", n * every_s,
                g_peggle1_game_frames.load(std::memory_order_relaxed), rex::path_to_utf8(path));
  }
}

}  // namespace

void Start(std::function<rex::ui::Presenter*()> get_presenter) {
  StartGrabber([get_presenter = std::move(get_presenter)](rex::ui::RawImage& image) {
    rex::ui::Presenter* presenter = get_presenter ? get_presenter() : nullptr;
    return presenter && presenter->CaptureGuestOutput(image);
  });
}

void StartGrabber(Grabber grab) {
  {
    std::lock_guard lock(g_mutex);
    g_stop = false;
  }
  if (REXCVAR_GET(peggle1_shots_every_s) > 0 && !g_shots.joinable()) {
    g_shots = std::thread(Shots, grab, int(REXCVAR_GET(peggle1_shots_every_s)));
  }
  if ((REXCVAR_GET(peggle1_video_s) <= 0 && REXCVAR_GET(peggle1_video_from_frame) <= 0) || g_thread.joinable()) {
    return;
  }
  g_thread = std::thread(Record, std::move(grab), int(REXCVAR_GET(peggle1_video_s)),
                         int(REXCVAR_GET(peggle1_video_from_frame)), int(REXCVAR_GET(peggle1_video_frames)));
}

void Stop() {
  {
    std::lock_guard lock(g_mutex);
    g_stop = true;
  }
  g_cv.notify_all();
  if (g_thread.joinable()) {
    g_thread.join();
  }
  if (g_shots.joinable()) {
    g_shots.join();
  }
}

}  // namespace peggle1::capture
