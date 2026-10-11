#!/bin/bash
# Throughput comparison of thinger-http with Crow, cpp-httplib, Node.js and nginx (+njs) on the
# same routes. Meant to run inside a Linux container (Ubuntu 24.04, e.g. thinger/compiler) with
# the repository mounted or copied at $REPO; see README.md.
#
#   ROUNDS=5 DURATION=10 SERVER_CPUS=0-7 CLIENT_CPUS=22-37 WORKERS=8 benchmark/compare/run.sh
#
# Each server gets WORKERS threads/processes pinned to SERVER_CPUS (cpp-httplib gets one thread
# per connection, which its model needs), wrk runs on CLIENT_CPUS, and the servers run one at a
# time in a rotating order per round. Results: $OUT/results.csv and a median table.
set -u
REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
OUT=${OUT:-$PWD/compare-results}
ROUNDS=${ROUNDS:-5}; DURATION=${DURATION:-10}; WORKERS=${WORKERS:-8}
SERVER_CPUS=${SERVER_CPUS:-0-7}; CLIENT_CPUS=${CLIENT_CPUS:-22-37}
SERVERS=${SERVERS:-"thinger crow httplib node1 node8 nginx"}
SCENARIOS=${SCENARIOS:-"hello params echo close"}
BUILD=${BUILD:-/tmp/compare-build}
mkdir -p "$OUT"

if [ -n "${INSTALL:-1}" ] && command -v apt-get >/dev/null && [ "$(id -u)" = 0 ]; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq >/dev/null && apt-get install -y -qq wrk nginx libnginx-mod-http-js nodejs >/dev/null 2>&1
fi
for tool in wrk nginx node cmake; do command -v $tool >/dev/null || { echo "missing $tool"; exit 1; }; done
NJS_MODULE=$(find /usr/lib/nginx/modules /usr/lib64/nginx/modules -name 'ngx_http_js_module.so' 2>/dev/null | head -1)
[ -n "$NJS_MODULE" ] || { echo "missing the nginx njs module"; exit 1; }

# extra CMake options (e.g. -DFETCHCONTENT_SOURCE_DIR_*) go in CMAKE_ARGS
cmake -S "$REPO/benchmark/compare" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release ${CMAKE_ARGS:-} > "$OUT/cmake.log" 2>&1 &&
cmake --build "$BUILD" -j"$(nproc)" > "$OUT/build.log" 2>&1 || { echo "build failed, see $OUT/build.log"; exit 1; }

sed -e "s#MODULE_PATH#$NJS_MODULE#" -e "s#NJS_ROUTES#$REPO/benchmark/compare/servers/njs-routes.js#" \
    -e "s#WORKERS#$WORKERS#" "$REPO/benchmark/compare/servers/nginx.conf" > /tmp/compare-nginx.conf
cat > /tmp/compare-post.lua <<'LUA'
wrk.method = "POST"
wrk.body = '{"name":"sensor","values":[1,2,3,4,5],"meta":{"unit":"C","precision":2}}'
wrk.headers["Content-Type"] = "application/json"
LUA
echo 'wrk.headers["Connection"] = "close"' > /tmp/compare-close.lua

port_of() { case $1 in thinger) echo 9081 ;; crow) echo 9082 ;; httplib) echo 9083 ;; node1) echo 9084 ;; nginx) echo 9085 ;; node8) echo 9086 ;; esac; }
start() {
  local p; p=$(port_of "$1")
  case $1 in
    thinger) taskset -c "$SERVER_CPUS" "$BUILD/thinger_server" "$WORKERS" "$p" >/dev/null 2>&1 & ;;
    crow)    taskset -c "$SERVER_CPUS" "$BUILD/competitors" crow "$p" "$WORKERS" >/dev/null 2>&1 & ;;
    httplib) taskset -c "$SERVER_CPUS" "$BUILD/competitors" httplib "$p" 128 >/dev/null 2>&1 & ;;
    node1)   taskset -c "$SERVER_CPUS" node "$REPO/benchmark/compare/servers/node-server.js" "$p" 1 >/dev/null 2>&1 & ;;
    node8)   taskset -c "$SERVER_CPUS" node "$REPO/benchmark/compare/servers/node-server.js" "$p" "$WORKERS" >/dev/null 2>&1 & ;;
    nginx)   taskset -c "$SERVER_CPUS" nginx -c /tmp/compare-nginx.conf >/dev/null 2>&1 & ;;
  esac
  PID=$!
  for _ in $(seq 1 150); do (exec 3<>"/dev/tcp/127.0.0.1/$p") 2>/dev/null && return 0; sleep 0.1; done
  echo "$1 did not listen on $p" >&2; return 1
}
stop() {
  kill "$PID" 2>/dev/null; for _ in $(seq 1 50); do kill -0 "$PID" 2>/dev/null || break; sleep 0.1; done
  kill -9 "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
  pkill -9 -f node-server.js 2>/dev/null; pkill -9 -x nginx 2>/dev/null; sleep 1
}
args_of() { # scenario url
  case $1 in
    hello)  echo "-c128 $2/hello" ;;
    params) echo "-c128 $2/v1/users/alvaro/devices/sensor1" ;;
    echo)   echo "-c128 -s /tmp/compare-post.lua $2/v1/echo" ;;
    close)  echo "-c32 -s /tmp/compare-close.lua $2/hello" ;;
  esac
}

echo "scenario,server,round,rps,p50,p99,errors" > "$OUT/results.csv"
for s in $SCENARIOS; do
  for r in $(seq 1 "$ROUNDS"); do
    set -- $SERVERS
    for _ in $(seq 2 "$r"); do first=$1; shift; set -- "$@" "$first"; done
    for v in "$@"; do
      start "$v" || continue
      a=$(args_of "$s" "http://127.0.0.1:$(port_of "$v")")
      taskset -c "$CLIENT_CPUS" wrk -t8 -d2s $a >/dev/null 2>&1
      f="$OUT/raw-$s-$v-$r.txt"
      taskset -c "$CLIENT_CPUS" wrk -t8 -d"${DURATION}s" --latency $a > "$f" 2>&1
      stop
      err=$(grep -E 'Non-2xx|Socket errors' "$f" | tr -s ' ' | tr ',\n' '; ')
      echo "$s,$v,$r,$(awk '/Requests\/sec/ {print $2}' "$f"),$(awk '$1=="50%" {print $2}' "$f"),$(awk '$1=="99%" {print $2}' "$f"),${err}" >> "$OUT/results.csv"
    done
  done
done

# median requests/s and latencies per scenario and server
python3 - "$OUT/results.csv" <<'PY' | tee "$OUT/summary.txt"
import csv, statistics, sys
def us(v):
    for unit, k in (("us", 1), ("ms", 1000), ("s", 1e6)):
        if v.endswith(unit): return float(v[:-len(unit)]) * k
    return float("nan")
rows = list(csv.DictReader(open(sys.argv[1])))
for s in dict.fromkeys(r["scenario"] for r in rows):
    print(f"\n{s}")
    for v in dict.fromkeys(r["server"] for r in rows if r["scenario"] == s):
        x = [r for r in rows if r["scenario"] == s and r["server"] == v and r["rps"]]
        if not x: print(f"  {v:8} no data"); continue
        rps = statistics.median(float(r["rps"]) for r in x)
        p50 = statistics.median(us(r["p50"]) for r in x); p99 = statistics.median(us(r["p99"]) for r in x)
        errs = {r["errors"] for r in x if r["errors"]}
        print(f"  {v:8} {rps:12,.0f} req/s   p50 {p50:7.0f} us   p99 {p99:7.0f} us  {' '.join(errs)}")
PY
