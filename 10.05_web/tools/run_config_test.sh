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
	"$WEB/webpower.cpp" \
	"$WEB/weblogging.cpp" \
	"$WEB/device_label.cpp" \
	"$WEB/weblog.cpp" \
	"$BASE/parameter.cpp" \
	"$COMMON/bitcalc.cpp" \
	"$OUT/civetweb.o" \
	-lpthread -o "$OUT/config_test"

# The credentials: webauth.cpp with the name rules and account provisioning of
# webshares.cpp behind it. The provisioning is root-only, so a test run creates
# no account; what it drives is the verification, the name rules and the
# settings.json round trip.
$CXX -std=c++11 -Wall -Wextra $INCLUDES \
	"$TOOLS/auth_test.cpp" \
	"$WEB/webauth.cpp" \
	"$WEB/webshares.cpp" \
	"$WEB/websystem.cpp" \
	"$WEB/weblog.cpp" \
	"$BASE/utils.cpp" \
	"$OUT/civetweb.o" \
	-lpthread -o "$OUT/auth_test"

# The seed file an SD card carries. It reads a small part of TOML and nothing
# else, so it links against its own translation unit alone.
$CXX -std=c++11 -Wall -Wextra -I "$WEB" \
	"$TOOLS/seed_test.cpp" \
	"$WEB/webseed.cpp" \
	-o "$OUT/seed_test"

# The console channel is civetweb-free, so it links against nothing but its own
# translation unit.
$CXX -std=c++11 -Wall -Wextra -I "$WEB" \
	"$TOOLS/console_channel_test.cpp" \
	"$WEB/webconsole_channel.cpp" \
	"$WEB/webrecording.cpp" \
	-lpthread -o "$OUT/console_channel_test"

# The session recorder is civetweb-free too; it links with the channel so the
# test can drive both together.
$CXX -std=c++11 -Wall -Wextra -I "$WEB" \
	"$TOOLS/recording_test.cpp" \
	"$WEB/webconsole_channel.cpp" \
	"$WEB/webrecording.cpp" \
	-lpthread -o "$OUT/recording_test"

# The disk-status and power-gate helpers are pure functions with no device or
# civetweb dependency, so they link against nothing but their own units.
$CXX -std=c++11 -Wall -Wextra -I "$WEB" \
	"$TOOLS/status_power_test.cpp" \
	"$WEB/device_status.cpp" \
	"$WEB/webcontrol.cpp" \
	-o "$OUT/status_power_test"

# The metric rate arithmetic is a pure function of a total and a timestamp, with
# no device, clock or civetweb dependency.
$CXX -std=c++11 -Wall -Wextra -I "$WEB" \
	"$TOOLS/metrics_test.cpp" \
	"$WEB/device_metrics.cpp" \
	-o "$OUT/metrics_test"

# The TCP serial-line backend is PRU/logger-free, so it links against nothing
# but its own translation unit and drives itself over a loopback socket.
DEV=$ROOT/10.02_devices/2_src
$CXX -std=c++11 -Wall -Wextra -I "$DEV" \
	"$TOOLS/serial_tcp_line_test.cpp" \
	"$DEV/serial_tcp_line.cpp" \
	-lpthread -o "$OUT/serial_tcp_line_test"

# The SIMH .tap tape image is a pure file format, PRU/logger-free, so it links
# against nothing but its own translation unit and works on a temp file.
$CXX -std=c++11 -Wall -Wextra -I "$DEV" \
	"$TOOLS/simh_tape_test.cpp" \
	"$DEV/simh_tape.cpp" \
	-o "$OUT/simh_tape_test"

"$OUT/config_test"
"$OUT/auth_test"
"$OUT/seed_test"
"$OUT/console_channel_test"
"$OUT/recording_test"
"$OUT/status_power_test"
"$OUT/metrics_test"
"$OUT/serial_tcp_line_test"
"$OUT/simh_tape_test"
