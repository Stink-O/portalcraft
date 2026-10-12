#!/bin/bash
# Summarise a prof_mc run: Minecraft's frame stats per phase (by wall-clock time against the PERF markers
# in the run's bridge.log), the portal-pass cost, the newest profiler report's top render-thread sections,
# and a flight recording's hot methods if one was made (PORTALCRAFT_JVM_ARGS=-XX:StartFlightRecording=...).
#   tools/mc_perf.sh <shots_subdir> [jfr_file]
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
RUN="$ROOT/cache/bridge/shots/$1"; MCLOG="$ROOT/cache/bridge/mc.log"
[ -f "$RUN/mc.log" ] && MCLOG="$RUN/mc.log"  # (the run's own copy when present)
[ -f "$RUN/bridge.log" ] || { echo "no $RUN/bridge.log"; exit 1; }
# bridge.log has no timestamps on script lines: take the phase times from the run's mc.log copy if present,
# else from the PERF markers the plugin echoes into Minecraft's log via the profiler toggles.
python3 - "$RUN/bridge.log" "$MCLOG" <<'PY'
import re, sys
blog, mclog = sys.argv[1], sys.argv[2]
mc = open(mclog, errors='replace').read().splitlines()
stats = []  # (seconds, fps, ms)
prof = []   # profiler toggle times
for l in mc:
    m = re.match(r'\[(\d+):(\d+):(\d+)\]', l)
    if not m: continue
    t = int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3])
    s = re.search(r'frame stats: ([\d.]+) fps, ([\d.]+) ms work', l)
    if s: stats.append((t, float(s[1]), float(s[2])))
    if 'Portalcraft: profiler started' in l: prof.append(t)
    p = re.search(r'portal passes: (\d+) frames, ([\d.]+) passes/frame .* ([\d.]+) ms/frame', l)
    if p: print(f'  passes @{l[1:9]}: {p[2]} passes/frame, {p[3]} ms/frame CPU')
phases = [l.split('PERF ')[1].split()[0] for l in open(blog, errors='replace') if 'log: PERF' in l and 'begin' in l]
# each phase's profiler window starts 5 s after its begin marker: use the profiler start times as anchors
for name, t0 in zip(phases, prof):
    w = [(f, ms) for (t, f, ms) in stats if t0 - 5 <= t <= t0 + 15]
    if w:
        print(f'phase {name}: {len(w)} windows, {sum(f for f,_ in w)/len(w):.1f} images/s (min {min(f for f,_ in w):.1f}), {sum(ms for _,ms in w)/len(w):.2f} ms work/frame')
PY
Z=$(ls -t "$ROOT"/bridge/fabric/run/debug/profiling/*.zip 2>/dev/null | head -3)
for z in $Z; do
  echo "== profiler report $(basename "$z")"
  unzip -p "$z" 'client/profiling.txt' 2>/dev/null | grep -E "^\[0[0-4]\] " | head -40
done
if [ -n "${2:-}" ] && [ -f "$2" ]; then
  echo "== flight recording: hot methods"; jfr view hot-methods "$2" 2>/dev/null | head -40
  echo "== GC pauses"; jfr view gc-pauses "$2" 2>/dev/null | head -20
fi
