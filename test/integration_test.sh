#!/bin/bash
#
# integration_test.sh - gpio-sim based end-to-end test
#
# Requires root:
#   - modprobe gpio-sim, mount configfs, access /dev/gpiochip*
#   - FUSE mount (fusermount3 setuid allows unprivileged, but we run all as root)
#
# Usage: sudo ./test/integration_test.sh [PORT]
#
set -u

PORT="${1:-4277}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MNT="$(mktemp -d /tmp/gpio-remote-test.XXXXXX)"
SIM_NAME="gpio-remote-test"
SIM_BANK="bank0"

PASS=0
FAIL=0
SERVER_PID=""
CLIENT_PID=""

pass() { echo "PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "FAIL: $1"; FAIL=$((FAIL+1)); }
check() { # check <desc> <cmd...>
    local desc="$1"; shift
    if "$@" >/dev/null 2>&1; then pass "$desc"; else fail "$desc"; fi
}

sim_cleanup() {
    local dir="$1"
    [ -d "$dir" ] || return 0
    [ -e "$dir/live" ] && echo 0 > "$dir/live" 2>/dev/null
    sleep 0.5   # let the kernel tear down the platform device
    local bank="$dir/$SIM_BANK"
    rmdir "$bank"/line17 "$bank"/line27 "$bank"/line18 2>/dev/null
    rmdir "$bank" 2>/dev/null
    rmdir "$dir" 2>/dev/null
}

cleanup() {
    [ -n "$CLIENT_PID" ] && kill "$CLIENT_PID" 2>/dev/null
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null
    wait 2>/dev/null
    grep -qs "$MNT" /proc/mounts && fusermount3 -uz "$MNT" 2>/dev/null
    sim_cleanup /sys/kernel/config/gpio-sim/"$SIM_NAME"
    # unload module so the next run starts from a fresh (non-wedged) state
    modprobe -r gpio_sim 2>/dev/null || true
    rmdir "$MNT" 2>/dev/null
}
trap cleanup EXIT

echo "=== sysfs-gpio-remote integration test (port $PORT) ==="

# ---------------------------------------------------------------- setup
if [ "$(id -u)" != 0 ]; then
    echo "ERROR: must run as root (gpio-sim + /dev/gpiochip access)"; exit 2
fi

# always start from a freshly loaded module: a wedged module from a previous
# run silently refuses to instantiate new devices
modprobe -r gpio_sim 2>/dev/null || true
modprobe gpio_sim 2>/dev/null || true
if ! grep -qs '^gpio_sim' /proc/modules; then
    echo "ERROR: gpio-sim module not available"; exit 2
fi

if ! mountpoint -q /sys/kernel/config; then
    mount -t configfs none /sys/kernel/config || { echo "ERROR: cannot mount configfs"; exit 2; }
fi

SIM_DIR=/sys/kernel/config/gpio-sim/"$SIM_NAME"
if [ -d "$SIM_DIR" ]; then
    echo "stale gpio-sim device found, cleaning up..."
    sim_cleanup "$SIM_DIR"
    [ -d "$SIM_DIR" ] && { echo "ERROR: cannot remove stale $SIM_DIR"; exit 2; }
fi

mkdir -p "$SIM_DIR/$SIM_BANK"
echo 32 > "$SIM_DIR/$SIM_BANK/num_lines"
mkdir "$SIM_DIR/$SIM_BANK/line17"
echo "GPIO17" > "$SIM_DIR/$SIM_BANK/line17/name"
mkdir "$SIM_DIR/$SIM_BANK/line27"
echo "GPIO27" > "$SIM_DIR/$SIM_BANK/line27/name"
# third line named like an OpenWrt gpio-export pin (named-export tests)
mkdir "$SIM_DIR/$SIM_BANK/line18"
echo "pcie_on" > "$SIM_DIR/$SIM_BANK/line18/name"
echo 1 > "$SIM_DIR/live" || { echo "ERROR: cannot instantiate gpio-sim"; exit 2; }

# locate the sim gpiochip sysfs dir by its per-line pull attributes
# (pull path is <chip>/sim_gpioN/pull, so chip dir is two dirname levels up)
CHIPDEV=""
for i in $(seq 1 40); do
    PULL17=$(find /sys/devices -name pull -path '*sim_gpio17*' 2>/dev/null | head -1)
    if [ -n "$PULL17" ]; then
        CHIPDEV=$(dirname "$(dirname "$PULL17")")
        break
    fi
    sleep 0.25
done
PULL17="$CHIPDEV/sim_gpio17/pull"
PULL27="$CHIPDEV/sim_gpio27/pull"
if [ -z "$CHIPDEV" ] || [ ! -e "$PULL17" ]; then
    echo "ERROR: sim sysfs pull attributes not found (CHIPDEV='$CHIPDEV')"
    dmesg | tail -10
    exit 2
fi
echo "gpio-sim ready: $CHIPDEV (GPIO17/27 with pull control)"

# ---------------------------------------------------------------- start processes
pkill -f "gpio-remote-server -p $PORT" 2>/dev/null || true   # stale server from an interrupted run
fuser -k "$PORT/tcp" 2>/dev/null || true
sleep 0.5
"$ROOT/gpio-remote-server" -p "$PORT" -n 64 -vv > /tmp/gpio-remote-server.log 2>&1 &
SERVER_PID=$!
sleep 0.5
kill -0 "$SERVER_PID" 2>/dev/null || { echo "ERROR: server died, see /tmp/gpio-remote-server.log"; exit 2; }

"$ROOT/gpio-remote-client" -s 127.0.0.1 -p "$PORT" -n 64 -v "$MNT" \
    > /tmp/gpio-remote-client.log 2>&1 &
CLIENT_PID=$!
sleep 1
kill -0 "$CLIENT_PID" 2>/dev/null || { echo "ERROR: client died, see /tmp/gpio-remote-client.log"; tail -5 /tmp/gpio-remote-client.log; exit 2; }

# ---------------------------------------------------------------- test 1: directory layout
echo "--- layout ---"
check "mount lists export/unexport"    bash -c "ls $MNT | grep -qx export && ls $MNT | grep -qx unexport"
check "no gpio dirs before export"     bash -c "! ls $MNT | grep -q gpio"

# ---------------------------------------------------------------- test 2: export
echo "--- export ---"
echo 17 > "$MNT/export"
check "gpio17 dir appears"             bash -c "ls $MNT/gpio17 | grep -qx value"
check "direction defaults to in"       bash -c "[ \"\$(cat $MNT/gpio17/direction)\" = in ]"
check "edge defaults to none"          bash -c "[ \"\$(cat $MNT/gpio17/edge)\" = none ]"
check "active_low defaults to 0"       bash -c "[ \"\$(cat $MNT/gpio17/active_low)\" = 0 ]"

# ---------------------------------------------------------------- test 3: output
echo "--- output ---"
echo out > "$MNT/gpio17/direction"
check "direction is out"               bash -c "[ \"\$(cat $MNT/gpio17/direction)\" = out ]"
echo 1 > "$MNT/gpio17/value"
check "value reads back 1"             bash -c "[ \"\$(cat $MNT/gpio17/value)\" = 1 ]"
check "server sees value 1 (sim)"      bash -c "[ \"\$(cat $CHIPDEV/sim_gpio17/value)\" = 1 ]"
echo 0 > "$MNT/gpio17/value"
check "value reads back 0"             bash -c "[ \"\$(cat $MNT/gpio17/value)\" = 0 ]"

# ---------------------------------------------------------------- test 4: input + active_low
echo "--- input / active_low ---"
# NOTE: the pull must be set AFTER the line is input - gpio-sim's
# direction_input() does not re-apply the pull, so a pull written while the
# line is output-driven never reaches value_map.
echo in > "$MNT/gpio17/direction"
echo pull-up > "$PULL17"
echo "  (diag: sim17 pull=$(cat "$PULL17") value=$(cat "$CHIPDEV/sim_gpio17/value") fs=$(cat "$MNT/gpio17/value"))"
check "input reads 1 (pull-up)"        bash -c "[ \"\$(cat $MNT/gpio17/value)\" = 1 ]"
echo 1 > "$MNT/gpio17/active_low"
check "inverted reads 0 (active_low)"  bash -c "[ \"\$(cat $MNT/gpio17/value)\" = 0 ]"
echo 0 > "$MNT/gpio17/active_low"
check "restored reads 1"               bash -c "[ \"\$(cat $MNT/gpio17/value)\" = 1 ]"

# ---------------------------------------------------------------- test 5: edge via poll_helper
echo "--- edge / poll ---"
echo both > "$MNT/gpio17/edge"
check "edge=both registered"           bash -c "[ \"\$(cat $MNT/gpio17/edge)\" = both ]"
"$ROOT/test/poll_helper" "$MNT/gpio17/value" 1 > /tmp/gpio-remote-poll.log 2>&1 &
POLL_PID=$!
sleep 0.5          # let poll_helper prime and enter poll()
echo pull-down > "$PULL17"   # falling edge
wait $POLL_PID
check "poll_helper sees edge"          bash -c "[ \$(tail -1 /tmp/gpio-remote-poll.log | grep -c 'edge!') -eq 1 ]"
check "edge value reads 0 (pull-down)" bash -c "[ \"\$(cat $MNT/gpio17/value)\" = 0 ]"

echo none > "$MNT/gpio17/edge"
check "edge=none registered"           bash -c "[ \"\$(cat $MNT/gpio17/edge)\" = none ]"
"$ROOT/test/poll_helper" "$MNT/gpio17/value" 1 > /tmp/gpio-remote-poll2.log 2>&1
POLL2_RC=$?
echo "  (diag: poll2 rc=$POLL2_RC out=$(cat /tmp/gpio-remote-poll2.log))"
check "no edge when edge=none"         bash -c "[ $POLL2_RC -eq 1 ]"
echo pull-up > "$PULL17"

# ---------------------------------------------------------------- test 6: second pin
echo "--- second pin ---"
echo 27 > "$MNT/export"
check "gpio27 exported"                bash -c "ls $MNT/gpio27 | grep -qx direction"
echo out > "$MNT/gpio27/direction"
echo 1 > "$MNT/gpio27/value"
check "gpio27 value 1"                 bash -c "[ \"\$(cat $MNT/gpio27/value)\" = 1 ]"

# ---------------------------------------------------------------- test 7: invalid ops
echo "--- invalid ops ---"
check "export invalid pin fails"       bash -c "! echo 99 > $MNT/export 2>/dev/null"
check "unexported pin ENOENT"          bash -c "! cat $MNT/gpio5/value 2>/dev/null"
check "write to input value fails"     bash -c "! echo 1 > $MNT/gpio17/value 2>/dev/null"

# ---------------------------------------------------------------- test 8: disconnect / recovery
echo "--- disconnect / recovery ---"
kill "$SERVER_PID"; SERVER_PID=""
wait "$SERVER_PID" 2>/dev/null
# wait for the port to be released (graceful shutdown can take a moment)
for i in $(seq 1 20); do
    grep -qs ":$PORT " /proc/net/tcp && sleep 0.25 || break
done
check "read fails after server dies"   bash -c "! cat $MNT/gpio17/value 2>/dev/null"

"$ROOT/gpio-remote-server" -p "$PORT" -n 64 -vv >> /tmp/gpio-remote-server.log 2>&1 &
SERVER_PID=$!
sleep 5          # client backoff + recovery replay
kill -0 "$SERVER_PID" 2>/dev/null || { echo "ERROR: restarted server died, see /tmp/gpio-remote-server.log"; tail -3 /tmp/gpio-remote-server.log; exit 2; }
check "client reconnects"              bash -c "kill -0 $CLIENT_PID"
check "gpio17 replayed after reconnect" bash -c "ls $MNT/gpio17 >/dev/null 2>&1"
check "gpio27 replayed after reconnect" bash -c "ls $MNT/gpio27 >/dev/null 2>&1"
check "gpio27 value restored to 1"     bash -c "[ \"\$(cat $MNT/gpio27/value)\" = 1 ]"

# ---------------------------------------------------------------- test 9: unexport
echo "--- unexport ---"
echo 27 > "$MNT/unexport"
check "gpio27 dir gone"                bash -c "! ls $MNT/gpio27 >/dev/null 2>&1"
echo 17 > "$MNT/unexport"
check "gpio17 dir gone"                bash -c "! ls $MNT/gpio17 >/dev/null 2>&1"

# ---------------------------------------------------------------- test 10: named export
echo "--- named export ---"
echo pcie_on > "$MNT/export"
check "pcie_on dir appears"            bash -c "ls $MNT/pcie_on | grep -qx value"
check "pcie_on direction usable"       bash -c "echo out > $MNT/pcie_on/direction && [ \"\$(cat $MNT/pcie_on/direction)\" = out ]"
echo 1 > "$MNT/pcie_on/value"
check "pcie_on value 1"                bash -c "[ \"\$(cat $MNT/pcie_on/value)\" = 1 ]"
check "named unexport works"           bash -c "echo pcie_on > $MNT/unexport && ! ls $MNT/pcie_on >/dev/null 2>&1"

# ---------------------------------------------------------------- test 11: connect-time enumeration
echo "--- connect enumeration ---"
echo 17 > "$MNT/export"
echo out > "$MNT/gpio17/direction"
echo 1 > "$MNT/gpio17/value"
echo 27 > "$MNT/export"
echo out > "$MNT/gpio27/direction"
echo 1 > "$MNT/gpio27/value"
# kill only the client: the server keeps its exports, a fresh client must
# recover the view via MSG_REQ_LIST
kill "$CLIENT_PID"; CLIENT_PID=""
wait "$CLIENT_PID" 2>/dev/null
sleep 1
"$ROOT/gpio-remote-client" -s 127.0.0.1 -p "$PORT" -n 64 -v "$MNT" \
    >> /tmp/gpio-remote-client.log 2>&1 &
CLIENT_PID=$!
sleep 2
kill -0 "$CLIENT_PID" 2>/dev/null || { echo "ERROR: restarted client died, see /tmp/gpio-remote-client.log"; tail -5 /tmp/gpio-remote-client.log; exit 2; }
check "gpio17 restored via LIST"       bash -c "ls $MNT/gpio17 >/dev/null 2>&1"
check "gpio27 restored via LIST"       bash -c "ls $MNT/gpio27 >/dev/null 2>&1"
check "gpio17 value correct"           bash -c "[ \"\$(cat $MNT/gpio17/value)\" = 1 ]"
check "gpio27 value correct"           bash -c "[ \"\$(cat $MNT/gpio27/value)\" = 1 ]"

# ---------------------------------------------------------------- test 12: sysfs auto-mirror
echo "--- auto mirror ---"
FAKE="$(mktemp -d /tmp/gpio-remote-fake.XXXXXX)"
mkdir "$FAKE/ap_reset"
echo 1 > "$FAKE/ap_reset/value"
chmod 0444 "$FAKE/ap_reset/value"      # OpenWrt input-only pin style
# restart the server with a fake sysfs dir; the running client must
# reconnect and pick up the mirrored entry automatically
kill "$SERVER_PID"; SERVER_PID=""
wait "$SERVER_PID" 2>/dev/null
for i in $(seq 1 20); do
    grep -qs ":$PORT " /proc/net/tcp && sleep 0.25 || break
done
GPIO_REMOTE_SYSFS_DIR="$FAKE" "$ROOT/gpio-remote-server" -p "$PORT" -n 64 -vv \
    >> /tmp/gpio-remote-server.log 2>&1 &
SERVER_PID=$!
sleep 8          # client reconnect backoff + mirror + LIST
kill -0 "$SERVER_PID" 2>/dev/null || { echo "ERROR: mirror server died, see /tmp/gpio-remote-server.log"; tail -3 /tmp/gpio-remote-server.log; exit 2; }
check "ap_reset mirrored on connect"   bash -c "[ \"\$(cat $MNT/ap_reset/value)\" = 1 ]"
rm -rf "$FAKE"

# ---------------------------------------------------------------- summary
echo "=== $PASS passed, $FAIL failed ==="
[ $FAIL -eq 0 ]
