// Downloadable content (see peggle1_dlc.h). From sa2-nx's sa2_dlc.cpp (2026-10-21).
//
// Peggle's DLC is "Peggle Nights" (content type 0x00000002, marketplace content; package
// 6AE5772224F8D86683B03E3A1B7509A48D1D4AAE58, holding content.pak, spa.bin and this_is_nights). Copy your own package
// into dlc/ next to the executable. Title Update #3's nightstu.pak, which the game reads as update:/nightstu.pak or
// d:/nightstu.pak, goes into the game folder itself.
// peggle1-nx (2026-11-24, the user: "like the ultimate base make sure peggle doesnt extract the nights dlc and its read
// in place like the dlc folder"; the ultimate base's port 265, rr6-nx's commit a3c240ba, credit rr6-nx): the package is
// read in place. Its first start writes only its .header into the content store (user data folder,
// 0000000000000000/58410889/Headers/00000002/), from which the game lists it (XamContentCreateEnumerator); every start
// registers the package with the content manager, which mounts it read only when the game opens it
// (StfsContainerDevice). The installer stores the package as its header P and P.xmop.lz4b beside it (the whole package,
// its XMA sounds as Opus, in LZ4 / ZXC blocks): the device reads the package through it. A copy an earlier version
// unpacked into 0000000000000000/58410889/00000002/<package>/ is no longer read (the log says where it is).

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
  char title[9];
  std::snprintf(title, sizeof(title), "%08X", kernel->title_id());
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
    // (a stand-in beside a package, P.xmop.lz4b or P.lz4b, is not a package: its magic is RXLZ4BLK)
    if (!entry.is_regular_file(ec) || !IsContentPackage(entry.path())) {
      continue;
    }
    const std::string name = rex::path_to_utf8(entry.path().filename());
    // peggle1-nx (2026-11-24, the ultimate base's port 265, rr6-nx's): read in place, whether installed now or before.
    content->RegisterPackageFile(name, entry.path());
    REXLOG_INFO("[dlc] {} read in place from its package (nothing unpacked)", name);
    // A copy an earlier version unpacked into the content store is no longer read.
    const std::filesystem::path unpacked =
        runtime->user_data_root() / "0000000000000000" / title / "00000002" / entry.path().filename();
    std::error_code uec;
    if (!runtime->user_data_root().empty() && std::filesystem::is_directory(unpacked, uec)) {
      uintmax_t bytes = 0;
      for (const auto& f : std::filesystem::recursive_directory_iterator(
               unpacked, std::filesystem::directory_options::skip_permission_denied, uec)) {
        if (f.is_regular_file(uec)) bytes += f.file_size(uec);
      }
      REXLOG_INFO("[dlc] {} was unpacked by an earlier version into {} ({:.1f} MB): no longer read, safe to delete",
                  name, rex::path_to_utf8(unpacked), double(bytes) / 1048576.0);
    }
    bool present = false;
    for (const auto& item : installed) {
      present = present || item.file_name() == name;
    }
    if (present) {
      REXLOG_INFO("[dlc] {} already installed", name);
      continue;
    }
    // Only the package's header (XAM lists the DLC from it); the files stay in the package.
    const auto result = content->InstallContent(entry.path(), /*extract_files=*/false);
    if (result == 0) {
      REXLOG_INFO("[dlc] installed {} (its header)", name);
    } else {
      REXLOG_WARN("[dlc] could not install {}: {:08X}", name, result);
    }
  }
}

}  // namespace peggle1::dlc
