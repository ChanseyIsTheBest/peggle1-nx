// banjo-nx: the boot-time choice between banjo-nx's native renderer and Renut's (banjo_renut_select.h).
#include "renut_engine/gpu/banjo_renut_select.h"

#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(PLUME_DEKO3D)
#include "renut_engine/gpu/backend/backend.h"      // peggle2-nx: IsDeko3D
#include "renut_engine/gpu/banjo_renut_deko3d.h"   // peggle2-nx: MissingGameShaders
#endif

REXCVAR_DEFINE_STRING(peggle1_renderer, "native", "Peggle1",
                      "Renderer, chosen at boot. native = the native PM4-ring renderer (the reference). renut = Renut's "
                      "Direct3D-level renderer (reNut by masterspike52, as banjo-nx carries it): the game's Direct3D runs "
                      "natively through plume (Vulkan; deko3d on the Switch with renut_gpu_backend = deko3d), with no PM4 "
                      "ring and no EDRAM model")
    .allowed({"native", "renut"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace banjo::renut_gpu {

// banjo-nx (2026-10-19, CPU proposal 22): Active() is inline (banjo_renut_select.h), a byte load after the first call.
namespace detail {
int8_t g_active = 0;
bool LatchActive() {
  static const bool active = [] {
#if defined(PEGGLE1_NO_VULKAN)
    // peggle2-nx (2026-10-20): no Vulkan driver in this build (app/CMakeLists.txt REXGLUE_SWITCH_LINK_NVK): the native
    // renderer, which is Vulkan, cannot run, so Renut's renderer on deko3d is the renderer whatever the key says.
    if (REXCVAR_GET(peggle1_renderer) != "renut")
      REXLOG_INFO("graphics: this build has no Vulkan driver: Renut's renderer on deko3d (peggle1_renderer = \"{}\" "
                  "ignored)", REXCVAR_GET(peggle1_renderer));
    if (const size_t missing = deko3d::MissingGameShaders()) {
      REXLOG_ERROR("[deko3d] {} of the game's shaders are in no GLSL package: deko3d_glsl.bin is missing beside the "
                   "NRO (the web installer makes it from your game files); the game's own pictures will not be drawn",
                   missing);
    }
    return true;
#endif
    if (REXCVAR_GET(peggle1_renderer) != "renut")
      return false;
#if defined(PLUME_DEKO3D)
    // peggle2-nx (2026-10-20): a build without translated game shaders (PEGGLE1_RENUT_RELEASE_SHADERS) needs the
    // web installer's deko3d_glsl.bin beside the NRO; without it the game's own shaders are missing and nothing of
    // the game would be drawn, so the native renderer runs instead (it takes its shaders from the installer's
    // peggle1_shaders.mesp).
    if (::renut::gpu::IsDeko3D()) {
      if (const size_t missing = deko3d::MissingGameShaders()) {
        REXLOG_ERROR("[deko3d] {} of the game's shaders are in no GLSL package: deko3d_glsl.bin is missing beside the "
                     "NRO (the web installer makes it from your game files). The native renderer runs instead of "
                     "Renut's (peggle1_renderer = \"renut\" ignored)", missing);
        rex::cvar::SetFlagByName("masseffect_renderer_native", "true");
        return false;
      }
    }
#endif
    return true;
  }();
  g_active = active ? 1 : -1;
  return active;
}
}  // namespace detail

}  // namespace banjo::renut_gpu
