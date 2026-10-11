# Comparison with other servers

The table in the main [README](../../README.md#performance) comes from `run.sh`. It builds
thinger-http and the same routes on Crow and cpp-httplib (`servers/`), runs them, Node.js
(`servers/node-server.js`, 1 and 8 processes) and nginx with njs (`servers/nginx.conf`,
`servers/njs-routes.js`) one at a time, and measures them with wrk.

| Case | Request |
|---|---|
| `hello` | `GET /hello`, plain text answer |
| `params` | `GET /v1/users/:user/devices/:device`, small JSON answer |
| `echo` | `POST /v1/echo` with a 76-byte JSON body, echoed as JSON |
| `close` | `GET /hello` with `Connection: close`, a new connection per request |

It needs Linux with `taskset`, CMake, a C++20 compiler, wrk, nginx with the njs module and
Node.js; on Ubuntu 24.04 as root it installs the missing packages itself. For example, in the
image used by the CI:

```bash
docker run --rm -v "$PWD":/src -w /src thinger/compiler:mold-latest \
  env OUT=/src/compare-results ROUNDS=5 benchmark/compare/run.sh
```

Settings (environment): `ROUNDS` (5), `DURATION` in seconds (10), `WORKERS` (8),
`SERVER_CPUS` (`0-7`), `CLIENT_CPUS` (`22-37`), `SERVERS` and `SCENARIOS` to run a subset,
and `CMAKE_ARGS` for extra CMake options. Pick CPU sets that exist on your machine and don't
overlap; run it on an otherwise idle machine. Results go to `results.csv` and `summary.txt`
(medians) in `OUT`.
