#!/usr/bin/env bash
# Build and run the cgusb unit tests on the build host (D-007).
#
# Unity is taken from the installed ESP-IDF, so the test framework is the one
# idf.py would use; only the driver around it differs, because Espressif does
# not support the linux target on Windows.
#
#   test/host/run.sh
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
build="$here/build"

# Find Unity: honour IDF_PATH when it is exported, otherwise look where the
# Windows installer puts the framework.
idf_path="${IDF_PATH:-}"
if [ -z "$idf_path" ]; then
    for candidate in /c/Espressif/frameworks/esp-idf-v*; do
        [ -d "$candidate" ] && idf_path="$candidate"
    done
fi
unity="$idf_path/components/unity/unity/src"
if [ ! -f "$unity/unity.c" ]; then
    echo "Unity not found under '$unity'." >&2
    echo "Export IDF_PATH, or install ESP-IDF, and run again." >&2
    exit 1
fi

python_bin="${PYTHON:-python}"

mkdir -p "$build"
"$python_bin" "$here/gen_vectors.py" "$build/vectors.h"

cc="${CC:-gcc}"
"$cc" -std=c11 -O1 -g \
    -Wall -Wextra -Werror \
    -Wno-unused-parameter \
    -I "$root/components/cgusb/include" \
    -I "$unity" \
    -I "$build" \
    -o "$build/test_cgusb.exe" \
    "$here/test_cgusb.c" \
    "$root/components/cgusb/cgusb.c" \
    "$unity/unity.c"

"$build/test_cgusb.exe"
