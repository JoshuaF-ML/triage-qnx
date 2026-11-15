#!/bin/sh
#
# smoke.sh -- run this ON THE QNX TARGET to prove stage 1 works.
#
#   cd /data && ./smoke.sh
#
# It answers five questions, in order:
#   1. Does /dev/triage exist? (resource manager attached)
#   2. Does a request get answered at all?
#   3. Does the server run our work at OUR priority? (the whole thesis)
#   4. Do high-priority deadlines survive a low-priority flood?
#   5. Does the scheduler stop being pessimistic once the load goes away?
#
# All four are assertions, not things for a human to read and judge:
# brake_reflex checks the priority pair in every reply itself and exits
# non-zero if any pair differs, so step 3 fails loudly rather than printing
# numbers nobody checks.

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
echo "  priority our server thread ran at. They must be identical at"
echo "  whatever priority the client happens to be, so we ask twice from"
echo "  two different priorities. brake_reflex checks every reply itself"
echo "  and exits non-zero if any pair differs."
if "$BIN/brake_reflex" -n 3 -P 40 -p 100000 &&
   "$BIN/brake_reflex" -n 3 -P 12 -p 100000; then
    ok "work ran at the client's priority, from both prio 40 and prio 12"
else
    bad "priority was not inherited -- the central claim does not hold"
fi

say "4. urgent work under a low-priority flood"
"$BIN/triage_stat" -r >/dev/null 2>&1
"$BIN/chat_load" -n 4 -P 10 -s 12 &
CHAT_PID=$!
sleep 2
# READ THIS BEFORE TRUSTING THE NUMBER THIS PRINTS.
#
# The 90% below is a regression tripwire, NOT the project's claim. It is
# deliberately loose, and the honest reason is that on this target the
# claim is not testable at all.
#
# The host steals CPU from the guest. Step 5 runs this same workload with
# nothing competing and still sees 24 of 40 requests preempted and 165ms
# taken; a priority-40 request with no load was measured losing 9.3ms in
# one go. Nothing inside QNX can preempt priority 40 on an idle system, so
# that is Windows and WSL2 descheduling the vCPU. It is invisible to the
# guest, unbounded, and comparable in size to the 20ms deadline being
# measured. Observed run to run on identical code: 92%, 99%, 95%.
#
# So a tight gate here does not measure Triage, it measures the host's
# mood, and tuning the threshold until it passes would be fitting the test
# to the noise. Instead: this step fails only on a real regression, and
# the actual claim is tested two other ways -- step 3 asserts priority
# inheritance on every single request, which is pass/fail and never flakes,
# and the FCFS baseline in bench/ will measure the deadline advantage
# differentially, on bare metal, where the host term is gone.
#
# brake_reflex defaults to requiring 100%. Run it without -m there.
if "$BIN/brake_reflex" -n 100 -P 40 -d 20000 -p 50000 -m 90 -q; then
    ok "detection kept its deadline under flood (tripwire: >=90%)"
else
    bad "detection missed far more deadlines than expected -- regression?"
fi
wait $CHAT_PID 2>/dev/null || true

say "5. the model must relax again once the machine is quiet"
echo "  Interference is a decaying maximum, so a burst of load must not"
echo "  leave the scheduler permanently pessimistic. With the flood gone"
echo "  and nothing competing, every deadline should be met -- held to"
echo "  100% here, since the host's own theft is the only term left."
if "$BIN/brake_reflex" -n 40 -P 40 -d 20000 -p 30000 -q; then
    ok "deadlines all met again after the load stopped"
else
    bad "still missing deadlines on a quiet machine"
fi

say "server counters"
"$BIN/triage_stat"

say "shutting down"
kill $TRIAGE_PID 2>/dev/null || true
sleep 1

printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
