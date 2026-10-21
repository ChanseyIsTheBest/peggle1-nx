// banjo-nx (2026-10-17): the presented frame of Renut's renderer, for the test captures (the PNGs, the short video
// and the streamed recording of src/native/nfsmw_nativo_captura.cpp), which take it from the SDK presenter's
// CaptureGuestOutput with banjo-nx's own renderer. Implemented in backend/present.cpp (tools/renut_patches).
#pragma once

#include <rex/ui/presenter.h>

namespace renut::gpu {

// The next Present scans the guest image out once more into an offscreen target of the frame's size and reads it
// back, waiting for the GPU (a test tool: it costs a GPU sync per capture). Works while the window is minimized.
// Returns false when no frame was presented within 'timeout_ms' or the renderer is not running.
bool CaptureFrame(rex::ui::RawImage &out, int timeout_ms = 1000);

} // namespace renut::gpu
