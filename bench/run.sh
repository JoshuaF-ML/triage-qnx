#!/bin/sh
#
# run.sh -- run this ON THE QNX TARGET to compare Triage against the
#           first-come-first-served baseline under identical load.
#
#   cd /data && ./run.sh
#   cd /data && ./run.sh -n 200 -s 25      # longer, less noisy
#
# WHAT IS HELD CONSTANT
# ---------------------
# Same client binaries, same priorities, same deadline, same request
# count, same flood, same engine, same machine, same boot. The clients are
# pointed at a different device with -D and nothing else changes. If the
# numbers differ, the server is the only thing that can have caused it.
#
# WHAT IS VARIED
# --------------
# Four servers, in this order:
#
#   triage          the real thing: priority inheritance, deadline-driven
#                   tier selection, interference model
#   fcfs queue      one worker, strict arrival order, fixed tier. Models a
#                   real deployment -- one model instance behind a queue.
#   fcfs pool       the same thread pool as Triage, same size, but with
#                   _NTO_CHF_FIXED_PRIORITY and no deadline logic. Holds
#                   concurrency constant so the remainder is attributable
#                   to priority inheritance and tier selection.
#   fcfs queue matched
#                   THE CONTROL THAT MATTERS. The baseline running exactly
#                   the tiers Triage chose for itself -- small for
#                   detection, large for chat. Tier choice is therefore
#                   held constant too, and the only thing left is
#                   scheduling. If Triage wins here, the win is priority
#                   inheritance and FCFS ordering; if it does not, the win
#                   was tier selection all along, and the honest thing is
#                   to say so.
#
#   fcfs queue@small
#                   everything cheap. Included for completeness and
#                   labelled as confounded: running detection at small
#                   also runs the chat flood at small, which drains the
#                   queue faster and helps detection for a reason that has
#                   nothing to do with the tier detection used. This was
#                   the first control written, and it was the wrong one.
#
# WHAT TO READ, AND WHAT NOT TO
# -----------------------------
# The number that matters is the DETECTION deadline hit rate while chat
# floods the same device. Absolute latencies on QEMU are not meaningful --
# this host steals CPU from the guest in deadline-sized quantities, at
# random (see docs/architecture.md). That theft hits all four servers in
# this run equally, which is exactly why a differential comparison on one
# boot is worth something while a single absolute number is not.

set -u

BIN="${BIN:-.}"
N="${N:-150}"           # detection requests per server
FLOOD_S="${FLOOD_S:-20}"
DEADLINE_US="${DEADLINE_US:-20000}"
PERIOD_US="${PERIOD_US:-50000}"
DET_PRIO="${DET_PRIO:-40}"
CHAT_PRIO="${CHAT_PRIO:-10}"
CHAT_THREADS="${CHAT_THREADS:-4}"
OUT="${OUT:-bench-results.txt}"

while getopts "n:s:d:o:h" c 2>/dev/null; do
    case "$c" in
    n) N="$OPTARG" ;;
    s) FLOOD_S="$OPTARG" ;;
    d) DEADLINE_US="$OPTARG" ;;
    o) OUT="$OPTARG" ;;
    h) sed -n '2,45p' "$0"; exit 0 ;;
    *) echo "usage: $0 [-n requests] [-s flood_seconds] [-d deadline_us] [-o file]" >&2
       exit 1 ;;
    esac
done

# Collected results, one line per server: label|met|total|pct|worst|mean
RESULTS=""

cleanup() {
    slay triage 2>/dev/null
    slay fcfs   2>/dev/null
    sleep 1
}

# run_case <label> <device> <extra brake_reflex args>
#
# Starts the flood, waits for it to get going, runs the detection client,
# and scrapes its summary. The flood is started fresh for every server so
# each one faces the same thing.
run_case() {
    label="$1"
    dev="$2"
    extra="$3"

    printf '\n=== %s\n' "$label"

    # Fail loudly. An earlier version of this script just recorded 0/0 when
    # a server had failed to start, which looks in the results table exactly
    # like a server that lost badly -- the single most misleading thing a
    # benchmark harness can do.
    if [ ! -e "$dev" ]; then
        echo "ERROR: $dev does not exist -- the server failed to start." >&2
        echo "       Refusing to record a result for '$label'." >&2
        exit 1
    fi

    "$BIN/triage_stat" -D "$dev" -r >/dev/null 2>&1

    # Not /dev/null. The flood's own output is the only place a failure to
    # generate load shows up, and silencing it cost a whole set of
    # meaningless results once already.
    "$BIN/chat_load" -D "$dev" -n "$CHAT_THREADS" -P "$CHAT_PRIO" \
                     -s "$((FLOOD_S + 5))" > /tmp/chat.$$ 2>&1 &
    chat_pid=$!
    sleep 2

    # shellcheck disable=SC2086
    "$BIN/brake_reflex" -D "$dev" -n "$N" -P "$DET_PRIO" \
                        -d "$DEADLINE_US" -p "$PERIOD_US" -q $extra \
                        > /tmp/br.$$ 2>&1
    cat /tmp/br.$$

    # VERIFY THE SERVER IS THE SERVER IT CLAIMS TO BE.
    #
    # brake_reflex reports "prio  N checked, M not inherited". For Triage M
    # must be 0; for any baseline M must equal N, because not inheriting is
    # the defining property of the baseline. A baseline that inherits is
    # Triage with the deadline logic removed, and it will score like Triage
    # -- which is exactly what happened when the fixed-priority channel flag
    # was set in the wrong namespace and nothing complained.
    checked=$(sed -n 's/^  prio  *\([0-9]*\) checked.*/\1/p' /tmp/br.$$)
    notinh=$(sed -n 's/^  prio .*checked, *\([0-9]*\) not inherited.*/\1/p' /tmp/br.$$)
    checked=${checked:-0}; notinh=${notinh:-0}

    case "$label" in
    triage*)
        if [ "$notinh" -ne 0 ]; then
            echo "ERROR: Triage failed to inherit priority on $notinh of" >&2
            echo "       $checked requests. That is the project's central" >&2
            echo "       claim, so no benchmark result is meaningful." >&2
            exit 1
        fi
        ;;
    *)
        if [ "$checked" -gt 0 ] && [ "$notinh" -ne "$checked" ]; then
            echo "ERROR: '$label' is a baseline, but it inherited priority" >&2
            echo "       on $((checked - notinh)) of $checked requests." >&2
            echo "       It is not a baseline. Refusing to record it." >&2
            exit 1
        fi
        ;;
    esac

    met=$(sed -n 's/^  met  *\([0-9]*\).*/\1/p' /tmp/br.$$)
    missed=$(sed -n 's/^  missed  *\([0-9]*\).*/\1/p' /tmp/br.$$)
    worst=$(sed -n 's/^  worst  *\([0-9]*\)us.*/\1/p' /tmp/br.$$)
    mean=$(sed -n 's/^  mean  *\([0-9]*\)us.*/\1/p' /tmp/br.$$)
    # The client's own round trip, which is the only figure that includes
    # time the server spent making the client wait before it knew the
    # request existed. The server's worst/mean start at the top of its
    # handler and cannot see queueing; that is how a 193ms wait was
    # reported as 4.7ms for weeks. Both are recorded, and the gap between
    # them is itself a result.
    rtt_w=$(sed -n 's/^  rtt .*worst \([0-9]*\)us.*/\1/p' /tmp/br.$$)
    rtt_m=$(sed -n 's/^  rtt  *mean \([0-9]*\)us.*/\1/p' /tmp/br.$$)
    errs=$(sed -n 's/^  errors  *\([0-9]*\).*/\1/p' /tmp/br.$$)
    rm -f /tmp/br.$$

    met=${met:-0}; missed=${missed:-0}; worst=${worst:-0}
    mean=${mean:-0}; errs=${errs:-0}
    rtt_w=${rtt_w:-0}; rtt_m=${rtt_m:-0}

    # A client that cannot time itself makes this whole table the server's
    # word against nobody, which is the failure this re-run exists to fix.
    if [ "$rtt_w" -eq 0 ]; then
        echo "ERROR: '$label' produced no client-side round trip figure." >&2
        echo "       brake_reflex is too old to report rtt -- rebuild and" >&2
        echo "       push the demos before running this." >&2
        exit 1
    fi
    total=$((met + missed))
    if [ "$total" -gt 0 ]; then
        pct=$((met * 100 / total))
    else
        pct=0
    fi

    echo "--- server counters"
    "$BIN/triage_stat" -D "$dev" > /tmp/st.$$ 2>&1
    cat /tmp/st.$$

    # VERIFY THE LOAD EXISTED.
    #
    # This whole comparison is about behaviour under contention, so a run
    # where the flood silently generated nothing is not a result -- it is
    # a measurement of an idle machine wearing the label of a loaded one.
    # That happened: chat_load parsed -D but ignored it and kept opening
    # /dev/triage, so every baseline case ran against no load at all and
    # the table looked plausible. Hence this check.
    chat_done=$(sed -n 's/.*chat=\([0-9]*\).*/\1/p' /tmp/st.$$ | tail -1)
    chat_done=${chat_done:-0}
    rm -f /tmp/st.$$

    echo "--- flood: $(cat /tmp/chat.$$ | tail -1)"
    rm -f /tmp/chat.$$

    kill $chat_pid 2>/dev/null
    wait $chat_pid 2>/dev/null

    if [ "$chat_done" -eq 0 ]; then
        echo "ERROR: the chat flood completed 0 requests against $dev." >&2
        echo "       '$label' was measured on an idle machine, which is" >&2
        echo "       not the thing this benchmark claims to measure." >&2
        exit 1
    fi
    echo "--- flood served $chat_done chat requests"

    RESULTS="$RESULTS$label|$met|$total|$pct|$worst|$mean|$errs|$rtt_w|$rtt_m
"
}

cleanup

echo "Triage vs FCFS baseline"
echo "detection: $N requests at prio $DET_PRIO, ${DEADLINE_US}us deadline,"
echo "           every ${PERIOD_US}us"
echo "flood:     $CHAT_THREADS chat threads at prio $CHAT_PRIO for ${FLOOD_S}s"
echo "host:      $(uname -a)"

# ------------------------------------------------- one shared calibration
#
# Every server calibrates its own work rate at startup, and on this target
# that measurement varies by more than 2x. Left alone, four servers started
# minutes apart would each decide a different amount of arithmetic was
# worth a millisecond -- so a "12ms tier" would not be the same work on
# both sides, and a difference in deadline hit rates could be nothing but a
# difference in calibration.
#
# That is not a hypothetical. It happened: the pool-mode baseline scored
# 98% because its chat tier had calibrated cheap, and 308 chat requests at
# a nominal 120ms each could not possibly have fitted in the window they
# appeared to.
#
# So measure once, and hand the same number to all four servers with -W.
# WORK can also be given explicitly, and for a comparable result it should
# be. The measured rate varies run to run on this host (386, 400 and higher
# observed), and it is not a nuisance parameter -- it IS the load intensity,
# since it decides how much arithmetic a "12ms tier" really costs. Left to
# vary, it moves the baselines enormously: pool mode scored 30% at a high
# rate and 97% at 400 on identical code. So treat it as the independent
# variable: fix it, and sweep it to see where each server breaks.
if [ -z "${WORK:-}" ]; then
    "$BIN/triage" -p /dev/triage-cal > /tmp/cal.$$ 2>&1 &
    cal_pid=$!
    sleep 2
    WORK=$(sed -n 's/.*calibrated at \([0-9]*\) work units.*/\1/p' /tmp/cal.$$)
    kill $cal_pid 2>/dev/null
    wait $cal_pid 2>/dev/null
    rm -f /tmp/cal.$$
    slay triage 2>/dev/null
    sleep 1
fi

if [ -z "${WORK:-}" ] || [ "$WORK" -le 0 ] 2>/dev/null; then
    echo "ERROR: could not read a calibration figure from the server." >&2
    echo "       Refusing to run: without one shared work rate the two" >&2
    echo "       servers would not be doing the same work per tier." >&2
    exit 1
fi
echo "work rate: $WORK units/us, applied to every server below"

# ---------------------------------------------------------------- Triage
"$BIN/triage" -W "$WORK" >/dev/null 2>&1 &
sleep 1
if [ ! -e /dev/triage ]; then
    echo "error: /dev/triage was not created" >&2
    exit 1
fi
run_case "triage" /dev/triage ""
cleanup

# ------------------------------------------------- baseline, queue@medium
"$BIN/fcfs" -m queue -k medium -P "$CHAT_PRIO" -W "$WORK" >/dev/null 2>&1 &
sleep 1
if [ ! -e /dev/fcfs ]; then
    echo "error: /dev/fcfs was not created" >&2
    exit 1
fi
run_case "fcfs queue medium" /dev/fcfs "-b"
cleanup

# -------------------------------------------------- baseline, pool@medium
"$BIN/fcfs" -m pool -k medium -P "$CHAT_PRIO" -W "$WORK" >/dev/null 2>&1 &
sleep 1
run_case "fcfs pool medium" /dev/fcfs "-b"
cleanup

# ------------------------------------------------ baseline, matched tiers
# Triage picks small for detection under this load and large for chat
# (2s deadline, 300ms nominal cost -- it fits easily). Give the baseline
# the same choices so tier selection is no longer a variable.
"$BIN/fcfs" -m queue -k small,large -P "$CHAT_PRIO" -W "$WORK" >/dev/null 2>&1 &
sleep 1
run_case "fcfs queue matched" /dev/fcfs "-b"
cleanup

# -------------------------------------------------- baseline, queue@small
# Confounded on purpose, and kept to show the confound: this makes the
# chat flood cheap too.
"$BIN/fcfs" -m queue -k small -P "$CHAT_PRIO" -W "$WORK" >/dev/null 2>&1 &
sleep 1
run_case "fcfs queue all-small" /dev/fcfs "-b"
cleanup

# ------------------------------------------------------------- the table
{
    echo
    echo "================================================================"
    echo "RESULTS -- detection deadline hit rate under chat flood"
    echo "================================================================"
    echo "$N requests at prio $DET_PRIO, ${DEADLINE_US}us deadline, "
    echo "flooded by $CHAT_THREADS chat threads at prio $CHAT_PRIO."
    echo
    printf '%-20s %8s %7s %11s %11s %11s\n' \
           server met rate rtt_worst rtt_mean srv_worst
    printf '%-20s %8s %7s %11s %11s %11s\n' \
           ------ --- ---- --------- -------- ---------
    echo "$RESULTS" | while IFS='|' read -r label met total pct worst mean errs rtt_w rtt_m; do
        [ -n "$label" ] || continue
        printf '%-20s %4s/%-3s %6s%% %9sus %9sus %9sus' \
               "$label" "$met" "$total" "$pct" "$rtt_w" "$rtt_m" "$worst"
        [ "${errs:-0}" -gt 0 ] && printf '  (%s errors)' "$errs"
        printf '\n'
    done
    echo
    echo "rtt_* are measured BY THE CLIENT around its own devctl, so they"
    echo "include everything the server made it wait for. srv_worst is the"
    echo "server's own figure, which starts at the top of its handler and"
    echo "cannot see queueing -- a large gap between the two columns means"
    echo "the server was serialising requests it had not yet looked at."
    echo
    echo "Read the rate column. Absolute latencies on QEMU include CPU the"
    echo "host stole from the guest, which is why this is run as a"
    echo "differential on one boot rather than quoted as a measurement."
    echo
    echo "'fcfs queue medium' is what people actually deploy."
    echo "'fcfs queue matched' runs the same tiers Triage chose, so it"
    echo "  isolates scheduling from tier selection -- the honest control."
    echo "'fcfs queue all-small' is confounded: it cheapens the flood too."
} | tee "$OUT"

echo
echo "written to $OUT"
