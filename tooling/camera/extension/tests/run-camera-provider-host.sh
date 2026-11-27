#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Run the CMIO and CPU output tests in CI without IOSurface or VideoToolbox services.
set -eu
exec bash "$(dirname "$0")/run-host.sh" --local-buffers
