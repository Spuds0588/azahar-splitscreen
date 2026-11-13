#!/usr/bin/env bash
# Session QA monitor for human-driven split-screen runs.
#
#   tools/qa/monitor.sh start [interval]   # sample in background -> slots/monitor.log
#   tools/qa/monitor.sh stop               # stop sampling
#   tools/qa/monitor.sh report             # summarise the last run
#
# One sample per second per running instance: cpu (% of one core), rss (MB),
# plus the newest line of slots/<seat>.fps (fps/speed/frame_ms) and the number
# of new Error/Critical log lines since the previous sample. The single window
# (`azahar-viewer`) is sampled the same way, with its own presented fps.
set -u
cd "$(dirname "$0")/../.."
LOG=slots/monitor.log
PIDF=slots/monitor.pid

seat_of() { # pid -> seat dir name from its --user argument
    tr '\0' ' ' < "/proc/$1/cmdline" 2>/dev/null |
        sed -n 's/.*--user .*\/slots\/\([^/ ]*\)\/user.*/\1/p'
}

cpu_ticks() { awk '{print $14 + $15}' "/proc/$1/stat" 2>/dev/null; }

err_count() { # seat -> Error/Critical lines in its instance log
    grep -c -E '<(Error|Critical)>' "slots/$1/user/log/azahar_log.txt" 2>/dev/null || echo 0
}

start() {
    local interval="${1:-1}"
    [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null && { echo "already running"; return 0; }
    : > "$LOG"
    setsid bash "$0" __loop "$interval" </dev/null >/dev/null 2>&1 &
    echo $! > "$PIDF"
    echo "monitoring -> $LOG (pid $(cat "$PIDF"))"
}

loop() {
    local interval="${1:-1}"
    (
        declare -A prev_ticks prev_err
        while :; do
            ts=$(date +%H:%M:%S)
            for pid in $(pgrep -f '^.*/build/bin/Release/azahar --user' 2>/dev/null); do
                seat=$(seat_of "$pid")
                [ -z "$seat" ] && continue
                now=$(cpu_ticks "$pid")
                [ -z "$now" ] && continue
                before=${prev_ticks[$pid]:-$now}
                prev_ticks[$pid]=$now
                cpu=$(( (now - before) * 100 / (interval * $(getconf CLK_TCK)) ))
                rss=$(awk '/VmRSS/{printf "%d", $2/1024}' "/proc/$pid/status" 2>/dev/null)
                read -r fps speed frame < <(tail -1 "slots/Player ${seat#p}.fps" 2>/dev/null |
                    sed 's/fps=\([0-9.]*\).*speed=\([0-9.]*\).*frame_ms=\([0-9.]*\)/\1 \2 \3/' | tr -d '\n')
                errs=$(err_count "$seat")
                new=$(( errs - ${prev_err[$seat]:-$errs} ))
                prev_err[$seat]=$errs
                printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                    "$ts" "$seat" "$pid" "$cpu" "${rss:-0}" "${fps:--}" "${speed:--}" "${frame:--}" "$new" >> "$LOG"
            done
            # The single window is sampled like a seat: cpu/rss plus the fps it
            # reports for itself (presented frames per second).
            for pid in $(pgrep -x azahar-viewer 2>/dev/null); do
                now=$(cpu_ticks "$pid")
                [ -z "$now" ] && continue
                before=${prev_ticks[$pid]:-$now}
                prev_ticks[$pid]=$now
                cpu=$(( (now - before) * 100 / (interval * $(getconf CLK_TCK)) ))
                rss=$(awk '/VmRSS/{printf "%d", $2/1024}' "/proc/$pid/status" 2>/dev/null)
                fps=$(tail -1 slots/viewer.log 2>/dev/null |
                    sed -n 's/.*presented_fps=\([0-9.]*\).*/\1/p')
                printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                    "$ts" "viewer" "$pid" "$cpu" "${rss:-0}" "${fps:--}" "-" "-" "0" >> "$LOG"
            done
            sleep "$interval"
        done
    )
}

stop() {
    [ -f "$PIDF" ] || { echo "not running"; return 0; }
    kill "$(cat "$PIDF")" 2>/dev/null
    rm -f "$PIDF"
    echo "stopped"
}

report() {
    [ -s "$LOG" ] || { echo "no samples in $LOG"; return 1; }
    echo "== seats"
    awk -F'\t' '
        $6 != "-" {
            n[$2]++; f[$2]+=$6; if (!($2 in mn) || $6<mn[$2]) mn[$2]=$6; if ($6>mx[$2]) mx[$2]=$6
            s[$2]+=$7; m[$2]+=$8; e[$2]+=$9
        }
        { c[$2]+=$4; cn[$2]++; if ($4>cmax[$2]) cmax[$2]=$4; r[$2]=$5; if (!($2 in r0)) r0[$2]=$5 }
        END {
            for (k in n) printf "%-12s fps avg %5.1f min %5.1f max %5.1f | speed avg %5.1f%% | frame avg %5.2fms | cpu avg %4.0f%% peak %4.0f%% | rss %d->%d MB | log errors %d\n",
                k, f[k]/n[k], mn[k], mx[k], s[k]/n[k], m[k]/n[k], c[k]/(cn[k]+0), cmax[k], r0[k], r[k], e[k]
        }' "$LOG"
    echo "== samples: $(wc -l < "$LOG") ($(head -1 "$LOG" | cut -f1) - $(tail -1 "$LOG" | cut -f1))"
    local dips
    dips=$(awk -F'\t' '$6 != "-" && $6 < 55' "$LOG" | wc -l)
    echo "== fps<55 samples: $dips"
    [ "$dips" -gt 0 ] && awk -F'\t' '$6 != "-" && $6 < 55 {printf "   %s %s fps=%s frame=%sms cpu=%s%%\n", $1, $2, $6, $8, $4}' "$LOG" | head -10
    echo "== error/critical lines per seat (benign ClCertA/FS-delay noise excluded)"
    for s in slots/p*/; do
        s=$(basename "$s")
        [ -f "slots/$s/user/log/azahar_log.txt" ] || continue
        n=$(grep -E '<(Error|Critical)>' "slots/$s/user/log/azahar_log.txt" |
            grep -vcE 'ClCertA file missing|Delay generator was not initalized' || true)
        [ "${n:-0}" -gt 0 ] && { echo "  $s: $n"; grep -E '<(Error|Critical)>' "slots/$s/user/log/azahar_log.txt" |
            grep -vE 'ClCertA file missing|Delay generator was not initalized' | cut -c1-140 | sort -u | head -4 | sed 's/^/     /'; }
    done
}

case "${1:-}" in
    start) start "${2:-1}" ;;
    __loop) loop "${2:-1}" ;;
    stop) stop ;;
    report) report ;;
    *) echo "usage: $0 {start [interval]|stop|report}"; exit 1 ;;
esac
