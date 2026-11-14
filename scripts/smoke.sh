#!/bin/sh
#
# smoke.sh -- run this ON THE QNX TARGET to prove stage 1 works.
#
#   cd /data && ./smoke.sh
#
# It answers four questions, in order:
#   1. Does /dev/triage exist? (resource manager attached)
#   2. Does a request get answered at all?
#   3. Does the server run our work at OUR priority? (the whole thesis)
#   4. Do high-priority deadlines survive a low-priority flood?
#
# Expected: in step 3 the two priority numbers printed per request are
# equal. In step 4 brake_reflex should report ~100% met while chat_load
# is hammering the same device.

set -u

BIN="${BIN:-.}"
PASS=0
FAIL=0

say()  { printf '\n=== %s\n' "$1"; }
ok()   { printf '  PASS %s\n' "$1"; PASS=$((PASS+1)); }
bad()  { printf '  FAIL %s\n' "$1"; FAIL=$((FAIL+1)); }

say "0. cleaning up any previous run"
slay triage 2>/dev/null || true
sleep 1

say "1. starting the resource manager"
"$BIN/triage" -v &
TRIAGE_PID=$!
sleep 1

if [ -e /dev/triage ]; then
    ok "/dev/triage exists"
else
    bad "/dev/triage was not created -- resmgr_attach failed"
    exit 1
fi

ls -l /dev/triage

say "2. one request, no load"
if "$BIN/brake_reflex" -n 5 -p 100000; then
    ok "requests answered"
else
    bad "requests answered but deadlines were missed with no load"
fi

say "3. priority inheritance"
echo "  the two numbers in prio=A/B are the client priority and the"
echo "  priority our server thread ran at. They should be identical."
"$BIN/brake_reflex" -n 3 -P 40 -p 100000
"$BIN/brake_reflex" -n 3 -P 12 -p 100000

say "4. urgent work under a low-priority flood"
"$BIN/triage_stat" -r >/dev/null 2>&1
"$BIN/chat_load" -n 4 -P 10 -s 12 &
CHAT_PID=$!
sleep 2
if "$BIN/brake_reflex" -n 100 -P 40 -d 20000 -p 50000 -q; then
    ok "all detection deadlines met while chat flooded the device"
else
    bad "detection missed deadlines under load -- see the summary above"
fi
wait $CHAT_PID 2>/dev/null || true

say "server counters"
"$BIN/triage_stat"

say "shutting down"
kill $TRIAGE_PID 2>/dev/null || true
sleep 1

printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
