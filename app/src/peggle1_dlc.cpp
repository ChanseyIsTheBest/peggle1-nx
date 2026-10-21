// Downloadable content (see peggle1_dlc.h). From sa2-nx's sa2_dlc.cpp (2026-10-21).
//
// Peggle's DLC is "Peggle Nights" (content type 0x00000002, marketplace content; package
// 6AE5772224F8D86683B03E3A1B7509A48D1D4AAE58, holding content.pak, spa.bin and this_is_nights). Copy your own package
// into dlc/ next to the executable: on the next start it is extracted into the content store (user data folder,
// 0000000000000000/58410889/00000002/<package>/) with its .header, and the game finds it through
// XamContentCreateEnumerator. Packages already in the store are skipped. Title Update #3's nightstu.pak, which the
// game reads as update:/nightstu.pak or d:/nightstu.pak, goes into the game folder itself.

#include "peggle1_dlc.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xcontent.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>

REXCVAR_DEFINE_STRING(peggle1_dlc_dir, "", "Peggle1",
                      "Folder with Xbox 360 DLC packages to install at startup (empty = dlc/ next to the executable)");

namespace peggle1::dlc {
namespace {

// STFS packages start with "LIVE", "PIRS" or "CON ".
bool IsContentPackage(const std::filesystem::path& path) {
  char magic[4] = {};
  std::FILE* f = rex::filesystem::OpenFile(path, "rb");
  if (!f) {
    return false;
  }
  const bool read = std::fread(magic, 1, 4, f) == 4;
  std::fclose(f);
  const std::string m(magic, 4);
  return read && (m == "LIVE" || m == "PIRS" || m == "CON ");
}

}  // namespace

void InstallPackages(rex::Runtime* runtime) {
  auto* kernel = runtime ? runtime->kernel_state() : nullptr;
  auto* content = kernel ? kernel->content_manager() : nullptr;
  if (!content) {
    return;
  }
  std::string dir = REXCVAR_GET(peggle1_dlc_dir);
  std::erase(dir, '"');  // string cvars can come back quoted
  const std::filesystem::path folder =
      dir.empty() ? rex::filesystem::GetExecutableFolder() / "dlc" : std::filesystem::path(dir);
  std::error_code ec;
  if (!std::filesystem::is_directory(folder, ec)) {
    return;
  }
  const auto installed = content->ListContent(uint32_t(rex::system::xam::DummyDeviceId::HDD), 0,
                                              rex::system::XContentType::kMarketplaceContent,
                                              kernel->title_id());
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
    if (!entry.is_regular_file(ec) || !IsContentPackage(entry.path())) {
      continue;
    }
    const std::string name = rex::path_to_utf8(entry.path().filename());
    bool present = false;
    for (const auto& item : installed) {
      present = present || item.file_name() == name;
    }
    if (present) {
      REXLOG_INFO("[dlc] {} already installed", name);
      continue;
    }
    const auto result = content->InstallContent(entry.path());
    if (result == 0) {
      REXLOG_INFO("[dlc] installed {}", name);
    } else {
      REXLOG_WARN("[dlc] could not install {}: {:08X}", name, result);
    }
  }
}

}  // namespace peggle1::dlc
