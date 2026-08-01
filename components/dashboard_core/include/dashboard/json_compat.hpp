// cJSON include shim.
//
// ESP-IDF's bundled cJSON is reached as <cJSON.h>; Debian/Ubuntu's libcjson-dev installs it
// as <cjson/cJSON.h>. The host unit tests build the same parser sources as the firmware, so
// every file that parses JSON includes this instead of cJSON directly.

#pragma once

#if defined(__has_include)
#if __has_include(<cJSON.h>)
#include <cJSON.h>
#elif __has_include(<cjson/cJSON.h>)
#include <cjson/cJSON.h>
#else
#error "cJSON headers not found (install libcjson-dev for the host build)"
#endif
#else
#include <cJSON.h>
#endif
