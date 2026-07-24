#!/bin/bash
# Build and run the host test of the configuration model (webconfigs.cpp).
#
# Hardware-free: the real webconfigs.cpp is linked against the real parameter
# system and a synthetic device set, so it runs on the development host and in
# CI without a BeagleBone. See config_test.cpp for what is stubbed.
#
# Exit status is the test result, so CI can gate on it.

set -e
cd "$(dirname "$0")"
TOOLS=$PWD
ROOT=$PWD/../..

STUBS=$TOOLS/stubs
WEB=$ROOT/10.05_web/2_src
BASE=$ROOT/10.01_base/2_src/arm
COMMON=$ROOT/90_common/src
PICO=$ROOT/91_3rd_party/picojson
CIVET=$ROOT/91_3rd_party/civetweb

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

CXX=${CXX:-c++}
CC=${CC:-cc}

# civetweb is only linked so webconfigs.cpp's unused HTTP handlers resolve.
$CC -c -O2 -DNO_SSL -DUSE_WEBSOCKET -DNO_CGI \
	-I "$CIVET" "$CIVET/civetweb.c" -o "$OUT/civetweb.o"

# The stub include directory comes first so the lightweight bus-adapter, panel,
# MSCP-server and device_configuration headers shadow the hardware ones.
INCLUDES="-I $STUBS -I $WEB -I $BASE -I $COMMON -I $PICO -I $CIVET"

$CXX -std=c++11 -Wall -Wextra $INCLUDES \
	"$TOOLS/config_test.cpp" \
	"$WEB/webconfigs.cpp" \
	"$WEB/weblogging.cpp" \
	"$WEB/device_label.cpp" \
	"$WEB/weblog.cpp" \
	"$BASE/parameter.cpp" \
	"$COMMON/bitcalc.cpp" \
	"$OUT/civetweb.o" \
	-lpthread -o "$OUT/config_test"

# The console channel is civetweb-free, so it links against nothing but its own
# translation unit.
$CXX -std=c++11 -Wall -Wextra -I "$WEB" \
	"$TOOLS/console_channel_test.cpp" \
	"$WEB/webconsole_channel.cpp" \
	-lpthread -o "$OUT/console_channel_test"

# The disk-status and power-gate helpers are pure functions with no device or
# civetweb dependency, so they link against nothing but their own units.
$CXX -std=c++11 -Wall -Wextra -I "$WEB" \
	"$TOOLS/status_power_test.cpp" \
	"$WEB/device_status.cpp" \
	"$WEB/webcontrol.cpp" \
	-o "$OUT/status_power_test"

"$OUT/config_test"
"$OUT/console_channel_test"
"$OUT/status_power_test"
