// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Control.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

using namespace pianobrightness;

static bool parse(std::initializer_list<const char *> args, Command &command)
{
  return pianobrightness::parse(int(args.size()), args.begin(), command);
}

int main()
{
  Command c{};
  assert(parse({"tool", "--probe"}, c) && c.action == Action::probe);
  assert(parse({"tool", "--level", ".125"}, c) && c.action == Action::level && c.level == 0.125f);
  assert(parse({"tool", "--level", "1."}, c) && c.level == 1);
  assert(parse({"tool", "--adapter-nits", "0.00000762939453125"}, c) && c.value == 1);
  assert(parse({"tool", "--adapter-nits", "20.5"}, c) && c.action == Action::adapterNits && c.value == (20 << 16) + 32768);
  assert(parse({"tool", "--adapter-nits", "600"}, c) && c.value == 600 << 16);
  assert(parse({"tool", "--reference", "1"}, c) && c.action == Action::reference && c.value == 1);
  assert(parse({"tool", "--auto", "0"}, c) && c.action == Action::automatic && c.value == 0);
  for (const char *bad : {"", ".", " .5", ".5 ", "NaN", "Inf", "-0", "0.4x", "1.1", "1.00000000000000000001", "0..5", "1e-1"})
    assert(!parse({"tool", "--level", bad}, c));
  for (const char *bad : {"0.0", "-1", "01", "2", "true", ""}) {
    assert(!parse({"tool", "--reference", bad}, c));
    assert(!parse({"tool", "--auto", bad}, c));
  }
  assert(!parse({"tool"}, c) && !parse({"tool", "--level"}, c) && !parse({"tool", "--adapter-nits", "600.001"}, c));
  assert(!parse({"tool", "--probe", "1"}, c) && !parse({"tool", "--reference", "0", "1"}, c));
  assert(authorized(Action::probe, 501) && authorized(Action::level, 501));
  for (Action root : {Action::adapterNits, Action::reference, Action::automatic})
    assert(authorized(root, 0) && !authorized(root, 501));
  assert(ioExit(kIOReturnBadArgument) == outOfRange && ioExit(kIOReturnSuccess) == ok);
  assert(ioExit(kIOReturnNotPrivileged) == unauthorized && ioExit(kIOReturnNotPermitted) == unauthorized);
  assert(ioExit(kIOReturnNoDevice) == unavailable && ioExit(kIOReturnNotReady) == unavailable && ioExit(kIOReturnUnsupported) == unavailable);
  assert(ioExit(kIOReturnIOError) == apiError && ioExit(kIOReturnNoMemory) == apiError);
  std::puts("piano-brightness-control host: parser, signed 16.16, authorization and IOKit errors passed");
}
