# Fuzz targets

libFuzzer targets, built with `-DTHINGER_HTTP_ENABLE_FUZZING=ON` (Clang only), with
AddressSanitizer and UndefinedBehaviorSanitizer. The request parser is the most exposed code
of the library and is kept in-house, so it is fuzzed for crashes, for its own invariants, and
against an independent parser.

| Target | What it checks |
|---|---|
| `fuzz_http_request_parser` | `request_factory` (full and headers-only modes) and the framing rules: same outcome however the input is split; an accepted header section is well formed (CRLF line endings, no folding, within limits, `HTTP/1.DIGIT`); `has_valid_framing()` agrees with an independent check of the raw `Content-Length` / `Transfer-Encoding` lines; then the whole request goes through `body_reader` |
| `fuzz_chunked_decoder` | `chunked_decoder`: no out-of-bounds access (every piece and output buffer is an exact-size heap allocation); the same body, outcome and bytes consumed however the input is split, whatever the output buffer size, and when chunk data is read straight from the input (`data_pending` / `data_consumed`); errors and the end of the body are final; bytes after the body are never consumed nor looked at |
| `fuzz_http_differential` | Differential against `boost::beast::http::request_parser` (see below) |
| `fuzz_url_decode`, `fuzz_utf8_validate` | No crash or undefined behavior |

Splits and output buffer sizes are derived from a hash of the input, so every failure
reproduces from the input alone.

Seeds are in `corpus/`: `http_request_parser` (used by both request targets) has the cases of
`tests/integration/request_framing_test.cpp` and the known request smuggling variants (CL.TE,
TE.CL, TE.TE obfuscations, obs-fold, chunk extensions, bare LF, chunk sizes with `0x`, signs
or overflow, duplicate and list Content-Length, HTTP versions); `chunked_decoder` has the
chunked bodies of `tests/unit/http/server/body_reader_test.cpp`, each followed by another
request.

Running one locally (Apple Clang has no libFuzzer, use Homebrew LLVM on macOS):

```sh
CC=/opt/homebrew/opt/llvm/bin/clang CXX=/opt/homebrew/opt/llvm/bin/clang++ \
  cmake -B build-fuzz -DTHINGER_HTTP_ENABLE_FUZZING=ON -DTHINGER_HTTP_BUILD_TESTS=OFF -DTHINGER_HTTP_ENABLE_LOGGING=OFF
cmake --build build-fuzz --target fuzz_http_differential
mkdir -p corpus-diff
UBSAN_OPTIONS=halt_on_error=1 build-fuzz/tests/fuzz/fuzz_http_differential corpus-diff \
  tests/fuzz/corpus/http_request_parser -max_len=4096 -max_total_time=600 -jobs=8 -workers=8
```

## Differential fuzzing against Boost.Beast

Each input is processed the way the server does it (`fuzz::process_request` in
`fuzz_common.hpp`: `request_factory` in headers-only mode, `has_valid_framing()`, then
`body_reader` on the bytes after the headers) and by Beast's request parser. Beast is only
used by this target as a reference, it is never linked into the library.

A request smuggling bug is a disagreement on where a request ends, so the target aborts when:

- we accept a request Beast rejects, or complete one Beast still needs data for (or the
  other way around);
- both accept it but disagree on where it ends (the bytes left for the next request), the
  framing (chunked or Content-Length), the decoded body, the method, the target or the
  version.

Rejecting what Beast accepts is fine: we are stricter on purpose. Pipelined requests are
followed while the connection would stay open.

### Bugs found

- **A request following a chunked body without its final CRLF was absorbed as the trailer
  section.** Trailer lines were accepted as long as they had no control characters, so with
  `0\r\n` and no final `\r\n`, the next request line and headers were consumed as trailers: a
  front end ending the body at `0\r\n` would see two requests where the server saw one.
  Trailer lines must now be field lines (`token ":" value`). Regression tests in
  `body_reader_test.cpp` and `request_framing_test.cpp`.
- **Chunk extensions were skipped without checking their grammar** (any non-control byte up
  to CR). They now must follow `*( BWS ";" BWS token [ BWS "=" BWS ( token / quoted-string ) ] )`.
- **Multi-digit HTTP versions were accepted and truncated** to 8 bits (`HTTP/1.257` read as
  1.1, `HTTP/256.1` as 0.1). The version is now `HTTP/1.DIGIT`: major versions other than 1
  do not use this message syntax.

### Intentional discrepancies

Where we reject what Beast accepts (no action needed, the target ignores them by design):

| Case | Us | Beast |
|---|---|---|
| Whitespace around a field value (`Name:value`, `Name:  value`, `value `, HTAB) | exactly `": "` required, trailing whitespace kept (so an invalid Content-Length / Transfer-Encoding) and HTAB rejected | OWS trimmed |
| Obsolete line folding, in headers or trailers | rejected | unfolded |
| `Content-Length: 5, 5` | rejected | accepted (same values) |
| `Transfer-Encoding` other than a single `chunked` (`gzip, chunked`, `chunked, chunked`, two headers, `identity`, unknown codings) | rejected | chunked if the last coding is chunked; otherwise the request has **no body**, so the bytes that follow are the next request |
| `Transfer-Encoding` in an HTTP/1.0 request | rejected (RFC 9112 section 6.1) | accepted |
| Request target other than origin form, with invalid percent-encoding or `..` | rejected | any visible characters |
| Whitespace between the chunk size and `;` (`5 ;ext`) | rejected, as llhttp does | accepted (BWS) |
| Control characters in a quoted chunk extension value | rejected (qdtext / quoted-pair) | accepted up to the closing quote |
| Header section over 100 lines or 16 KB | rejected (431) | 8 KB limit by default (disabled in the target) |

Where we accept what Beast rejects: these are recognized by `normalize_known_discrepancy()`
in `fuzz_http_differential.cpp`, which rewrites the input (keeping its size and framing) so
Beast no longer rejects it for that reason, and compares again, so the rest of the request is
still checked:

| Case | Why it is fine |
|---|---|
| `HTTP/1.2` to `HTTP/1.9` (Beast only accepts 1.0 and 1.1) | RFC 9110 section 2.5: handled as HTTP/1.1 |
| `Connection` / `Proxy-Connection` values that are not a valid token list | we only look for the tokens we know in it; it does not frame the message |
| `Content-Length`, `Transfer-Encoding` or invalid `Connection` fields in the trailer section | Beast handles trailer fields like header fields; we ignore the trailer section, it is never merged into the headers |

Not compared: keep-alive (Beast and we agree on the defaults, but we do not validate the
`Connection` list), unknown methods (we keep them as `UNKNOWN`, so only known methods are
compared by name), and header fields other than the framing ones.
