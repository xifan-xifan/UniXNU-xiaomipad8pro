#!/bin/bash
# macOS host: serialize the exact pack schema through CF, then replay the 27.3 consumer.
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/battery-health-host.XXXXXX")
trap 'rm -r "$T"' EXIT
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I "$P" "$P/tests/battery_health_host.cpp" -framework CoreFoundation -o "$T/battery_health_host"
"$T/battery_health_host"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fobjc-arc \
  "$P/tests/battery_storage_host.mm" -framework Foundation -o "$T/battery_storage_host"
"$T/battery_storage_host" "$T"
