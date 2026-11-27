// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../piano-torch-args.h"
#include <assert.h>

int main(void)
{
  struct arguments args;
  assert(parse_arguments(1, (char *[]){"piano-torch"}, &args) && args.command == STATUS);
  assert(parse_arguments(2, (char *[]){"piano-torch", "status"}, &args) && args.command == STATUS);
  assert(parse_arguments(2, (char *[]){"piano-torch", "toggle"}, &args) && args.command == TOGGLE);
  assert(parse_arguments(2, (char *[]){"piano-torch", "off"}, &args) && args.command == OFF && args.ma == 0);
  assert(parse_arguments(2, (char *[]){"piano-torch", "on"}, &args) &&
         args.command == ON && args.ma == -1 && args.seconds == 0);
  assert(parse_arguments(4, (char *[]){"piano-torch", "on", "45", "12"}, &args) &&
         args.ma == 45 && args.seconds == 12);
  assert(parse_arguments(4, (char *[]){"piano-torch", "on", "100", "0"}, &args) &&
         args.ma == 100 && args.seconds == 0);
  assert(parse_arguments(4, (char *[]){"piano-torch", "on", "1", "1"}, &args) &&
         args.ma == 1 && args.seconds == 1);
  assert(parse_arguments(4, (char *[]){"piano-torch", "on", "00099", "1799"}, &args) &&
         args.ma == 99 && args.seconds == 1799);
  assert(parse_arguments(4, (char *[]){"piano-torch", "on", "100", "1800"}, &args) &&
         args.ma == 100 && args.seconds == 1800);
  assert(parse_arguments(4, (char *[]){"piano-torch", "on", "200", "60"}, &args) &&
         args.ma == 200 && args.seconds == 60);
  assert(parse_arguments(4, (char *[]){"piano-torch", "on", "400", "1800"}, &args) &&
         args.ma == 400 && args.seconds == 1800);
  assert(parse_arguments(4, (char *[]){"piano-torch", "on", "401", "1801"}, &args) &&
         args.ma == 400 && args.seconds == 1800);
  char oversized[] = "184467440737095516160";
  assert(parse_arguments(4, (char *[]){"piano-torch", "on", oversized, oversized}, &args) &&
         args.ma == 400 && args.seconds == 1800);
  char digits[4097];
  memset(digits, '9', sizeof digits - 1);
  digits[sizeof digits - 1] = '\0';
  assert(parse_arguments(4, (char *[]){"piano-torch", "on", digits, digits}, &args) &&
         args.ma == 400 && args.seconds == 1800);
  digits[sizeof digits - 2] = 'x';
  assert(!parse_arguments(3, (char *[]){"piano-torch", "on", digits}, &args));
  assert(!parse_arguments(4, (char *[]){"piano-torch", "on", "50", digits}, &args));
  assert(!parse_arguments(3, (char *[]){"piano-torch", "off", "1"}, &args));
  assert(!parse_arguments(3, (char *[]){"piano-torch", "status", "1"}, &args));
  assert(!parse_arguments(3, (char *[]){"piano-torch", "toggle", "1"}, &args));
  assert(!parse_arguments(5, (char *[]){"piano-torch", "on", "100", "1", "2"}, &args));
  assert(!parse_arguments(2, (char *[]){"piano-torch", "statuss"}, &args));
  assert(!parse_arguments(3, (char *[]){"piano-torch", "on", "0"}, &args));
  assert(!parse_arguments(3, (char *[]){"piano-torch", "on", ""}, &args));
  assert(!parse_arguments(3, (char *[]){"piano-torch", "on", " 5"}, &args));
  assert(!parse_arguments(3, (char *[]){"piano-torch", "on", "-5"}, &args));
  assert(!parse_arguments(3, (char *[]){"piano-torch", "on", "-0"}, &args));
  assert(!parse_arguments(3, (char *[]){"piano-torch", "on", "+100"}, &args));
  assert(!parse_arguments(3, (char *[]){"piano-torch", "on", "0x64"}, &args));
  assert(!parse_arguments(3, (char *[]){"piano-torch", "on", "100 "}, &args));
  assert(!parse_arguments(4, (char *[]){"piano-torch", "on", "50", ""}, &args));
  assert(!parse_arguments(4, (char *[]){"piano-torch", "on", "50", "-1"}, &args));
  assert(!parse_arguments(4, (char *[]){"piano-torch", "on", "50", "9999999999999s"}, &args));
  return 0;
}
