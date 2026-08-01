// Firmware identity.
//
// DASH_APP_VERSION and DASH_GIT_SHA are injected by the top-level CMakeLists.txt (from a
// -D flag, an environment variable, or `git describe`). The fallbacks below exist so that
// an editor / clangd / a source tarball with no git metadata still compiles.
//
// Anything that compares versions must go through dashboard::SemVer, which rejects
// non-semantic strings — so "0.0.0-dev" deliberately never satisfies an OTA upgrade check.

#pragma once

#ifndef DASH_APP_VERSION
#define DASH_APP_VERSION "0.0.0-dev"
#endif

#ifndef DASH_GIT_SHA
#define DASH_GIT_SHA "unknown"
#endif

namespace dash {

/// Semantic version of this build, e.g. "0.1.0". "0.0.0-dev" for an untagged local build.
constexpr const char* kAppVersion = DASH_APP_VERSION;

/// Short git SHA of the commit this firmware was built from, or "unknown".
constexpr const char* kGitSha = DASH_GIT_SHA;

/// Human-readable product name. Shown on the boot screen and in the setup portal.
constexpr const char* kProductName = "Desk Dashboard";

}  // namespace dash
