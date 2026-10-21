// Short video recordings of the game image for testing, taken from the presenter's guest output (what the game
// presents), so they work with the window minimized and nothing can cover them. Same design and file format as the
// banjo-nx recorder: tools/make_video.sh turns the raw file into an MP4 and a contact sheet.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

namespace rex::ui {
class Presenter;
struct RawImage;
}

// Frames the game has presented (counted by the hook on its D3DDevice_Swap, peggle1_frame_hooks.cpp); the recorder
// waits on it.
extern std::atomic<uint64_t> g_peggle1_game_frames;

namespace peggle1::capture {

// Starts the recorder thread when peggle1_video_s > 0.
void Start(std::function<rex::ui::Presenter*()> get_presenter);

// The same with any frame source (Renut's renderer reads its presented frame back: renut::gpu::CaptureFrame).
using Grabber = std::function<bool(rex::ui::RawImage&)>;
void StartGrabber(Grabber grab);

// Stops it (writes what it has, if anything).
void Stop();

}  // namespace peggle1::capture
