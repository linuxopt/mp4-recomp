#!/bin/bash
# Builds linux/input_kbm_test.c with runtime/input_kbm.c and runs it; fails on any FAIL line
set -e -o pipefail
source "$(dirname "$(readlink -f "$0")")/env.sh"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
gcc -O1 -Wall -Wextra -Wno-unused-parameter -I "$W/runtime" -o "$T/input_kbm_test" "$W/linux/input_kbm_test.c" "$W/runtime/input_kbm.c" -lm
"$T/input_kbm_test"
