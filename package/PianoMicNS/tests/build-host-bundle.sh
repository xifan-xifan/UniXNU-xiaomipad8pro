#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Build a signed, instrumented CFPlugIn fixture next to the test executable.
set -euo pipefail
arch=$2
bundle="$1.$arch/PianoMicNS.driver"
rn=../third_party/rnnoise/src
flags=(-arch "$arch" -mmacosx-version-min=14.0 -std=c11 -O1 -Wall -Wextra -Werror
       "-fsanitize=address,undefined" -fno-sanitize-recover=all -fno-omit-frame-pointer
       -I.. -I../../PianoAudio -I../third_party/rnnoise/include)
mkdir -p "$bundle/Contents/MacOS"
clang "${flags[@]}" -Wno-null-dereference -c "$rn/rnn.c" -o "$1.$arch.rnn.o"
clang "${flags[@]}" -bundle ../PianoMicNS.c ../MicNsCore.c "$rn"/{denoise,rnn_data,pitch,kiss_fft,celt_lpc}.c \
    "$1.$arch.rnn.o" -framework CoreAudio -framework CoreFoundation -framework IOKit \
    -Wl,-exported_symbol,_PianoMicNS_Create -o "$bundle/Contents/MacOS/PianoMicNS"
cp ../Info.plist "$bundle/Contents/Info.plist"
codesign --force --sign - --timestamp=none "$bundle"
printf '%s\n' "$bundle"
