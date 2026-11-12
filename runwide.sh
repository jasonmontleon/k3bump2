#!/bin/sh
# runwide.sh - run k3bump2 on several harts at once and record what each worker was doing when it stopped.
#
# First board run (2026-11-12, harts 0-6): hart 1 stopped after 341 s, then harts 4, 0, 3, 2, 5 within the
# next 31 s.
#
# The script:
#   - starts one worker per hart from an ordinary, unpinned shell (their output goes to the terminal);
#   - then turns into a NEW process that lives only on the hart(s) the workers do not use, so nothing it does
#     has to wait for a hart that has died (a shell that has run on a hart that later dies freezes with it);
#   - once a second reads, for each worker, its CPU time and state and its hart's line in /proc/stat;
#   - when a worker makes no progress it says which kind of stop it is:
#       hart takes no timer ticks at all            = the hart is dead (the lock-up)
#       task asleep inside a system call            = the hart is alive, the task waits for something (disk, ...)
#       task burning kernel time, log not growing   = the hart is alive, the task spins in the kernel
#     and copies the worker's writable memory to stopped-hart<N>/ (shows where in the pass it stopped).
# Everything it finds goes to the terminal and to ./watch.log (pushed to disk at once).
#
# usage: ./runwide.sh [cpus] [hours] [mapped]
#   cpus    0-6  or  0,2,4   (default 0-6: leaves one X100 hart for the watcher)
#   hours   per-worker run time (default 4)
#   mapped  control: every worker gets the page WITH a page-table entry
# Run from a scratch directory; it makes hart<N>/ subdirs here (old logs are kept as k3bump2.log.<time>).
# Ctrl-C stops all workers.  After a lock-up and power-cycle:  cat watch.log

set -u
here=$(cd "$(dirname "$0")" && pwd)
bin="$here/k3bump2"
[ -x "$bin" ] || { echo "no k3bump2 at $bin"; exit 2; }

expand() {	# "0-6" / "0,2,4" -> "0 1 2 ..."
  printf '%s\n' "$1" | tr ',' '\n' | while IFS= read -r t; do
    case "$t" in
      *-*) a=${t%-*}; b=${t#*-}; while [ "$a" -le "$b" ]; do echo "$a"; a=$((a+1)); done ;;
      *)   [ -n "$t" ] && echo "$t" ;;
    esac
  done | tr '\n' ' '
}

if [ "${1:-}" != --watch ]; then
  # ---- stage 1: start the workers ----
  secs=$(( ${2:-4} * 3600 ))
  extra=""; [ "${3:-}" = mapped ] && extra=mapped
  cpulist=$(expand "${1:-0-6}")
  stamp=$(date +%Y%m%d-%H%M%S)
  [ -s watch.log ] && mv watch.log "watch.log.$stamp"
  for c in $cpulist; do [ -d "stopped-hart$c" ] && mv "stopped-hart$c" "stopped-hart$c.$stamp"; done
  echo "runwide: $(date '+%F %T')  kernel $(uname -r)  harts $cpulist  ${2:-4} h${extra:+  CONTROL (page mapped)}" | tee watch.log
  echo "runwide: cmdline $(cat /proc/cmdline)" >> watch.log
  pids=""
  for c in $cpulist; do
    d="hart$c"; mkdir -p "$d"
    [ -s "$d/k3bump2.log" ] && mv "$d/k3bump2.log" "$d/k3bump2.log.$stamp"
    : > "$d/k3bump2.log"; rm -f "$d/.sz" "$d/.flag"
    ( cd "$d" && exec "$bin" -c "$c" -t "$secs" $extra ) &	# no -q: k3bump2 must write a passes line each second
    echo "$!" > "$d/.pid"; pids="$pids $!"
    echo "started k3bump2 on hart $c (pid $!) in $d/"
  done
  # ---- stage 2: become a fresh process on harts the workers do not use ----
  allowed=$(expand "$(awk '/^Cpus_allowed_list:/ {print $2}' /proc/self/status)")
  free=""; freex=""; first=""
  for a in $allowed; do
    case " $cpulist " in *" $a "*) continue ;; esac
    free="$free${free:+,}$a"; [ "$a" -le 7 ] && freex="$freex${freex:+,}$a"; [ -z "$first" ] && first=$a
  done
  export RW_CPUS="$cpulist" RW_PIDS="$pids" RW_SECS="$secs"
  for cand in "$free" "$freex" "$first"; do
    [ -n "$cand" ] && taskset -c "$cand" true 2>/dev/null && RW_ON="$cand" exec taskset -c "$cand" sh "$0" --watch
  done
  echo "runwide: WARNING: no hart is left for the watcher (allowed: $allowed).  It shares harts with the workers and may freeze with them."
  RW_ON="shared" exec sh "$0" --watch
fi

# ---- the watcher ----
cpulist=$RW_CPUS; pids=$RW_PIDS; secs=$RW_SECS
nl='
'
say() {	# terminal first, then the file, pushed to disk without waiting for it (one write per message)
  echo "$*"
  ( echo "$*" | dd of=watch.log oflag=append,sync conv=notrunc 2>/dev/null ) &
}
trap 'echo; echo stopping; kill $pids 2>/dev/null; exit 0' INT TERM
say "watcher on hart(s) $RW_ON, looking once a second.  Ctrl-C to stop all."

cpuline() {	# sets L_<n> to the current /proc/stat line of every cpu (shell built-ins only)
  while read -r name rest; do
    case "$name" in cpu[0-9]*) eval "L_${name#cpu}=\$rest" ;; intr) break ;; esac
  done < /proc/stat
}
taskstat() {	# $1 = pid: sets T_state T_cpu_time (user+system ticks) T_user T_sys T_on; returns 1 if the task is gone
  line=""; read -r line < "/proc/$1/stat" 2>/dev/null || return 1
  [ -n "$line" ] || return 1
  set -- ${line##*) }
  T_state=$1; T_user=${12}; T_sys=${13}; T_on=${37}; T_cpu_time=$(( T_user + T_sys ))
}
dump() {	# $1 = hart, $2 = pid: writable memory of a stopped worker, read from this hart
  o="stopped-hart$1"; mkdir -p "$o"
  cat "/proc/$2/maps" > "$o/maps" 2>/dev/null
  cat "/proc/$2/stat" > "$o/stat" 2>/dev/null; cat "/proc/$2/status" > "$o/status" 2>/dev/null
  cat /proc/stat > "$o/proc-stat" 2>/dev/null
  while read -r range perm rest; do
    case "$perm" in rw*) ;; *) continue ;; esac
    lo=$(( 0x${range%-*} )); hi=$(( 0x${range#*-} ))
    [ $(( hi - lo )) -le 16777216 ] || continue
    timeout 10 dd if="/proc/$2/mem" of="$o/mem.${range%-*}" bs=4096 skip=$(( lo / 4096 )) count=$(( (hi - lo) / 4096 )) 2>/dev/null
  done < "$o/maps"
  # page-table state of the 32 pages around the page (librustc_driver+0x2a10000..+0x2a2ffff at 0x3ff3000000), 8 bytes each
  timeout 10 dd if="/proc/$2/pagemap" of="$o/pagemap.3ff5a10000" bs=8 skip=$(( 0x3ff5a10000 / 4096 )) count=32 2>/dev/null
  sync "$o"/* 2>/dev/null
  echo "memory of hart $1's worker saved in $o/ ($(ls "$o" | wc -l) files)"
  echo "memory of hart $1's worker saved in $o/" | dd of=watch.log oflag=append,sync conv=notrunc 2>/dev/null
}

t0=$(date +%s); now=0; alive=1
for c in $cpulist; do eval "idle_$c=0 slow_$c=0 told_$c=0 pt_$c=-1 pl_$c='' sz_$c=-1"; done
while [ "$alive" = 1 ] && [ "$now" -lt $(( secs + 30 )) ]; do
  sleep 1; now=$(( now + 1 )); alive=0
  cpuline
  # 1. shell built-ins only: is each worker still getting CPU time, is its hart still ticking?
  for c in $cpulist; do
    p=$(cat "hart$c/.pid" 2>/dev/null) || continue
    eval "told=\$told_$c pt=\$pt_$c pl=\$pl_$c cur=\${L_$c:-}"
    taskstat "$p" || continue
    alive=1
    if [ "$T_cpu_time" = "$pt" ]; then eval "idle_$c=\$(( idle_$c + 1 ))"; else
      if [ "$told" = 1 ]; then say "=== hart $c: worker is running again after about $(eval "echo \$idle_$c") s (at $now s) ==="; eval "told_$c=0"; fi
      eval "idle_$c=0"
    fi
    if [ "$cur" = "$pl" ]; then eval "dead_$c=\$(( \${dead_$c:-0} + 1 ))"; else eval "dead_$c=0"; fi
    eval "pt_$c=\$T_cpu_time pl_$c=\$cur idle=\$idle_$c dead=\$dead_$c"
    if [ "$idle" -ge 3 ] && [ "$told" = 0 ]; then
      eval "told_$c=1"
      sc=""; read -r sc < "/proc/$p/syscall" 2>/dev/null
      if [ "$dead" -ge 3 ]; then kind="HART $c IS DEAD: no timer tick on it for $dead s (its /proc/stat line does not change)"
      elif [ "$T_state" = R ]; then kind="hart $c still ticks, but the worker gets no CPU time although it is runnable"
      else kind="hart $c is alive; the worker is asleep in the kernel (state $T_state)"; fi
      say "=== at $now s: worker on hart $c (pid $p) STOPPED.  $kind ===$nl    task: state $T_state, last ran on cpu $T_on, user $T_user sys $T_sys ticks; syscall: ${sc:-?}$nl    last log line: $(tail -n 1 "hart$c/k3bump2.log" 2>/dev/null)"
      ( dump "$c" "$p" ) &
    fi
  done
  # 2. every 4 s: a worker that still burns CPU time but whose log no longer grows is spinning in the kernel
  [ $(( now % 4 )) = 0 ] || continue
  for c in $cpulist; do
    p=$(cat "hart$c/.pid" 2>/dev/null) || continue
    eval "told=\$told_$c prev=\$sz_$c"
    [ "$told" = 0 ] && [ -d "/proc/$p" ] || continue
    sz=$(wc -c < "hart$c/k3bump2.log" 2>/dev/null || echo 0); eval "sz_$c=$sz"
    [ "$sz" = "$prev" ] || { eval "slow_$c=0"; continue; }
    eval "slow_$c=\$(( slow_$c + 1 )); slow=\$slow_$c"
    [ "$slow" -ge 2 ] || continue
    eval "told_$c=1"; taskstat "$p"; sc=""; read -r sc < "/proc/$p/syscall" 2>/dev/null
    say "=== at $now s: worker on hart $c (pid $p) STOPPED WRITING for 8 s but still uses CPU time: it spins in the kernel or in its own code ===$nl    task: state $T_state, last ran on cpu $T_on, user $T_user sys $T_sys ticks; syscall: ${sc:-?}$nl    last log line: $(tail -n 1 "hart$c/k3bump2.log" 2>/dev/null)"
    ( dump "$c" "$p" ) &
  done
done
[ "$alive" = 0 ] && say "all workers have ended (time limit reached or stopped by hand)"
echo done.
