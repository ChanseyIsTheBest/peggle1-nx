// The game's language from the system's (see peggle1_language.h). Only the system headers here: switch.h's typedefs
// collide with rex's (windowed_app_main_switch.cpp), windows.h's macros with everything.

#include "peggle1_language.h"

#include <cstdio>
#include <cstring>

#if defined(__SWITCH__)
#include <switch.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace peggle1::language {

std::string FromSystem(std::string* system_name) {
#if defined(__SWITCH__)
  // The language the console's System Settings show (setGetSystemLanguage gives a code such as "en-US" or "zh-Hant";
  // setMakeLanguage turns it into the SetLanguage enum).
  u64 code = 0;
  SetLanguage language = SetLanguage_ENUS;
  bool read = false;
  if (R_SUCCEEDED(setInitialize())) {
    read = R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &language));
    setExit();
  }
  if (system_name) {
    char name[sizeof(code) + 1] = {};
    std::memcpy(name, &code, sizeof(code));
    *system_name = read ? std::string(name) : std::string("unreadable");
  }
  if (!read) return "en";
  switch (language) {
    case SetLanguage_JA: return "ja";
    case SetLanguage_FR:
    case SetLanguage_FRCA: return "fr";
    case SetLanguage_DE: return "de";
    case SetLanguage_IT: return "it";
    case SetLanguage_ES:
    case SetLanguage_ES419: return "es";
    case SetLanguage_ZHCN:
    case SetLanguage_ZHTW:
    case SetLanguage_ZHHANS:
    case SetLanguage_ZHHANT: return "zh";
    case SetLanguage_KO: return "ko";
    case SetLanguage_PT:
    case SetLanguage_PTBR: return "pt";
    default: return "en";  // English (US, GB), Dutch, Russian
  }
#elif defined(_WIN32)
  const LANGID id = GetUserDefaultUILanguage();
  if (system_name) {
    char name[16];
    std::snprintf(name, sizeof(name), "LANGID 0x%04X", static_cast<unsigned>(id));
    *system_name = name;
  }
  switch (PRIMARYLANGID(id)) {
    case LANG_JAPANESE: return "ja";
    case LANG_GERMAN: return "de";
    case LANG_FRENCH: return "fr";
    case LANG_SPANISH: return "es";
    case LANG_ITALIAN: return "it";
    case LANG_KOREAN: return "ko";
    case LANG_CHINESE: return "zh";
    case LANG_PORTUGUESE: return "pt";
    default: return "en";
  }
#else
  if (system_name) *system_name = "not read on this platform";
  return "en";
#endif
}

}  // namespace peggle1::language
