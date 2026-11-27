// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PIANO_TORCH_ARGS_H
#define PIANO_TORCH_ARGS_H

#include <stdbool.h>
#include <string.h>

enum command { STATUS, ON, OFF, TOGGLE };

struct arguments {
  enum command command;
  long long ma;       // -1 uses the driver's default
  long long seconds;  // 0 uses the driver's default
};

static inline bool decimal(const char *text, long long limit, long long *value)
{
  if (!*text) return false;
  long long n = 0;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') return false;
    n = n * 10 + (*text - '0');
    if (n > limit) n = limit;
  }
  *value = n;
  return true;
}

static inline bool parse_arguments(int argc, char *const argv[], struct arguments *args)
{
  *args = (struct arguments){.command = STATUS, .ma = -1, .seconds = 0};
  if (argc == 1) return true;
  if (strcmp(argv[1], "on") == 0) {
    args->command = ON;
    return argc <= 4 && (argc < 3 || (decimal(argv[2], 400, &args->ma) && args->ma > 0)) &&
           (argc < 4 || decimal(argv[3], 1800, &args->seconds));
  }
  if (argc != 2) return false;
  if (strcmp(argv[1], "off") == 0) {
    args->command = OFF;
    args->ma = 0;
  } else if (strcmp(argv[1], "toggle") == 0) {
    args->command = TOGGLE;
  } else if (strcmp(argv[1], "status") != 0) {
    return false;
  }
  return true;
}

#endif
