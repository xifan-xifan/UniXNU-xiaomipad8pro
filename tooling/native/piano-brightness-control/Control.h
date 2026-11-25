// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <IOKit/IOReturn.h>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace pianobrightness {

enum class Action { probe, level, adapterNits, reference, automatic };
enum Exit : int { ok = 0, usage = 64, outOfRange = 65, unavailable = 69, apiError = 74, unauthorized = 77 };

struct Command {
  Action action;
  float level = 0;
  int32_t value = 0;
};

inline bool decimal(const char *text, unsigned max, double &value)
{
  unsigned whole = 0;
  bool dot = false, digit = false;
  for (const char *p = text; *p; ++p) {
    if (*p == '.' && !dot) {
      dot = true;
      continue;
    }
    if (*p < '0' || *p > '9') return false;
    const unsigned n = unsigned(*p - '0');
    digit = true;
    if (!dot) {
      whole = 10 * whole + n;
      if (whole > max) return false;
    } else if (whole == max && n) {
      return false;
    }
  }
  if (!digit) return false;
  char *end = nullptr;
  errno = 0;
  value = std::strtod(text, &end);
  return errno != ERANGE && *end == '\0' && std::isfinite(value);
}

inline bool parse(int argc, const char *const argv[], Command &command)
{
  if (argc < 2) return false;
  if (!std::strcmp(argv[1], "--probe")) {
    command = {Action::probe};
    return argc == 2;
  }
  if (argc != 3) return false;
  if (!std::strcmp(argv[1], "--level")) {
    double level;
    if (!decimal(argv[2], 1, level)) return false;
    command = {Action::level, float(level)};
    return true;
  }
  if (!std::strcmp(argv[1], "--adapter-nits")) {
    double nits;
    if (!decimal(argv[2], 600, nits)) return false;
    command = {Action::adapterNits, 0, int32_t(std::lround(nits * 65536))};
    return true;
  }
  const bool reference = !std::strcmp(argv[1], "--reference");
  if (!reference && std::strcmp(argv[1], "--auto")) return false;
  if ((argv[2][0] != '0' && argv[2][0] != '1') || argv[2][1]) return false;
  command = {reference ? Action::reference : Action::automatic, 0, argv[2][0] - '0'};
  return true;
}

inline bool authorized(Action action, unsigned euid)
{
  return euid == 0 || action == Action::probe || action == Action::level;
}

inline Exit ioExit(IOReturn status)
{
  switch (status) {
  case kIOReturnSuccess: return ok;
  case kIOReturnNotPrivileged:
  case kIOReturnNotPermitted: return unauthorized;
  case kIOReturnBadArgument: return outOfRange;
  case kIOReturnUnsupported:
  case kIOReturnNotFound:
  case kIOReturnNotReady:
  case kIOReturnNoDevice: return unavailable;
  default: return apiError;
  }
}

}  // namespace pianobrightness
