#!/bin/sh
#
# verify.sh -- run this ON THE QNX TARGET to check that every claim the
#              codelab makes about its own three programs is true here.
#
#   cd /data/codelab && ./verify.sh
#
# A tutorial whose output you cannot reproduce is a blog post. Each step
# below is an assertion with an exit code, not output for a human to
# squint at, and the two interesting ones are falsifiable:
#
#   step 2  asks the SAME server source twice, once with inheritance and
#           once with _NTO_CHF_FIXED_PRIORITY, and requires the first to
#           inherit on every request and the second on none.
#   step 3  requires the urgent client's worst-case latency to collapse
#           when the resource manager stops serialising its own handlers,
#           and to come back when the thread pool is shrunk to one -- so
#           both conditions are shown to be necessary, not just asserted.
#
# Run as root: raising a thread's priority needs PROCMGR_AID_PRIORITY.

set -u

BIN="${BIN:-.}"
PASS=0
FAIL=0

say() { printf '\n=== %s\n' "$1"; }
ok()  { printf '  PASS %s\n' "$1"; PASS=$((PASS+1)); }
bad() { printf '  FAIL %s\n' "$1"; FAIL=$((FAIL+1)); }

cleanup() {
    slay step1_hello step2_prio step3_pool ask 2>/dev/null || true
    sleep 1
}

# Pull "worst NNNNNus" out of ask's summary.
worst_of() {
    sed -n 's/.*worst \([0-9]*\)us.*/\1/p' "$1" | tail -1
}

cleanup

# ------------------------------------------------------------------
say "step 1: a resource manager exists and answers"

"$BIN/step1_hello" &
sleep 2          # registering the path is not instant on an emulated target

if [ -e /dev/clab1 ]; then
    ok "/dev/clab1 was created by resmgr_attach()"
else
    bad "/dev/clab1 missing -- resmgr_attach() failed"
    cleanup
    exit 1
fi

if "$BIN/ask" -D /dev/clab1 -n 3 | grep -q 'hello from a resource manager'; then
    ok "devctl() round trip works; step 1 cannot yet prove inheritance"
else
    bad "no answer came back from /dev/clab1"
fi
cleanup

# ------------------------------------------------------------------
say "step 2: priority inheritance, and the flag that turns it off"

"$BIN/step2_prio" &
sleep 2

if "$BIN/ask" -D /dev/clab2 -P 40 -n 5 -q && \
   "$BIN/ask" -D /dev/clab2 -P 12 -n 5 -q; then
    ok "server ran our work at OUR priority, at both 40 and 12"
else
    bad "priority was not inherited -- the whole mechanism is absent"
fi
cleanup

"$BIN/step2_prio" -f &
sleep 2

if "$BIN/ask" -D /dev/clab2 -P 40 -n 5 -q -f; then
    ok "_NTO_CHF_FIXED_PRIORITY turned inheritance off, as it should"
else
    bad "the fixed-priority channel STILL inherited -- the flag did not"
    bad "reach the channel (see make_fixed_priority_channel)"
fi
cleanup

# ------------------------------------------------------------------
say "step 3: a pool thread AND an unserialised handler"
echo "  A lazy client asks for 3s of thinking at priority 10. An urgent"
echo "  client asks for 1ms at priority 40, one second later. Three"
echo "  servers, same source, two flags:"
echo
echo "    -t 8        eight pool threads, and the lock that"
echo "                iofunc_func_init() installs for you"
echo "    -t 8 -u     eight pool threads, that lock cleared"
echo "    -t 1 -u     one thread, lock cleared"
echo
echo "  Only the middle one can serve the urgent client. The pool and"
echo "  the missing lock are both necessary, and neither is sufficient."
echo
echo "  Note -W below: the urgent client opens the device BEFORE the"
echo "  server gets busy. open() is a message to the same resource"
echo "  manager and queues on the same threads, so a client that opens"
echo "  late pays the queueing inside open() -- outside the timed"
echo "  region -- and reports a latency that looks fine. That mistake"
echo "  is how this step first appeared to pass."

run_case() {   # $1 = server flags, $2 = output file
    # shellcheck disable=SC2086
    "$BIN/step3_pool" $1 &
    sleep 3

    # Connect now, ask in 5 seconds, by which time the server is buried.
    "$BIN/ask" -D /dev/clab3 -P 40 -w 1000 -n 4 -W 5000000 -q > "$2" 2>&1 &
    URGENT=$!
    sleep 1

    "$BIN/ask" -D /dev/clab3 -P 10 -w 3000000 -n 4 -q >/dev/null 2>&1 &
    LAZY=$!

    wait $URGENT
    kill $LAZY 2>/dev/null || true
    cleanup
}

run_case "-t 8"     /tmp/clab_lock.txt
run_case "-t 8 -u"  /tmp/clab_free.txt
run_case "-t 1 -u"  /tmp/clab_one.txt

WLOCK=$(worst_of /tmp/clab_lock.txt)
WFREE=$(worst_of /tmp/clab_free.txt)
WONE=$(worst_of /tmp/clab_one.txt)
: "${WLOCK:=0}" "${WFREE:=0}" "${WONE:=0}"

printf '\n  urgent worst case:\n'
printf '    8 threads, framework lock : %9sus\n' "$WLOCK"
printf '    8 threads, lock cleared   : %9sus\n' "$WFREE"
printf '    1 thread,  lock cleared   : %9sus\n' "$WONE"
printf '\n'

if [ "$WFREE" -gt 0 ] && [ "$WLOCK" -gt $((WFREE * 5)) ]; then
    ok "the framework's own lock was serialising every request"
else
    bad "expected the locked server to be at least 5x worse"
    echo "  --- locked ---";   cat /tmp/clab_lock.txt
    echo "  --- unlocked ---"; cat /tmp/clab_free.txt
fi

if [ "$WFREE" -gt 0 ] && [ "$WONE" -gt $((WFREE * 5)) ]; then
    ok "one thread is still one thread -- the pool is needed too"
else
    bad "expected one thread to be at least 5x worse than eight"
    echo "  --- one thread ---"; cat /tmp/clab_one.txt
fi

# ------------------------------------------------------------------
printf '\n=== %d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
