// The game's language from the system's, for peggle1_language = "" (the user's request, 2026-10-21: "make the default
// option follow the users system language"): the Switch's system language, or Windows' display language on the PC.

#pragma once

#include <string>

namespace peggle1::language {

// The system language as one of the game's nine (en, ja, de, fr, es, it, ko, zh, pt): both Chinese scripts give zh
// (the game has traditional Chinese only), regional variants their language, and any other language (Dutch, Russian)
// or a system language that cannot be read gives en. `system_name` receives the system's own name for it, for the log.
std::string FromSystem(std::string* system_name = nullptr);

}  // namespace peggle1::language
