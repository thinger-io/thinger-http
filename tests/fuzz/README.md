# Fuzz targets

libFuzzer targets, built with `-DTHINGER_HTTP_ENABLE_FUZZING=ON` (Clang only), with
AddressSanitizer and UndefinedBehaviorSanitizer. The request parser is the most exposed code
of the library and is kept in-house, so it is fuzzed for crashes, for its own invariants, and
against independent parsers. So is the response parser of the HTTP client (and of the
reverse proxy), which reads what any server sends.

| Target | What it checks |
|---|---|
| `fuzz_http_request_parser` | `request_factory` (full and headers-only modes) and the framing rules: same outcome however the input is split; an accepted header section is well formed (CRLF line endings, no folding, within limits, `HTTP/1.DIGIT`); `has_valid_framing()` agrees with an independent check of the raw `Content-Length` / `Transfer-Encoding` lines; then the whole request goes through `body_reader` |
| `fuzz_http_response_parser` | `response_factory`, on what a server sent before closing the connection, as the response to `GET`, `HEAD` and `CONNECT`: same outcome, error, end, framing and body however the input is split; each head (interim responses included), parsed alone as the reverse proxy does, is well formed when accepted (`HTTP/1.DIGIT`, a status from 100 to 599, CRLF line endings, no folding, valid names and values, within limits) and fails as the client fails on it; its framing agrees with an independent check of the raw `Content-Length` / `Transfer-Encoding` lines and with RFC 9112 section 6.3 (no body for `HEAD`, 1xx, 204, 304 and a 2xx to `CONNECT`); the body of the final response is the one an independent reading gives (`Content-Length` bytes, what `chunked_decoder` decodes from the same bytes, or the rest of the input); a connection kept alive never had a body delimited by its end nor switched protocols |
| `fuzz_http_response_differential` | Three-way differential of responses, as the client reads them: against llhttp in response mode; Beast's disagreements with llhttp are only counted (see below) |
| `fuzz_chunked_decoder` | `chunked_decoder`: no out-of-bounds access (every piece and output buffer is an exact-size heap allocation); the same body, outcome and bytes consumed however the input is split, whatever the output buffer size, and when chunk data is read straight from the input (`data_pending` / `data_consumed`); errors and the end of the body are final; bytes after the body are never consumed nor looked at |
| `fuzz_http_differential` | Differential against `boost::beast::http::request_parser` (see below) |
| `fuzz_http_differential3` | Three-way differential: against llhttp, Node.js's parser, in its strict default mode; Beast's disagreements with llhttp are only counted (see below) |
| `fuzz_url_decode`, `fuzz_utf8_validate` | No crash or undefined behavior |
| `fuzz_body_decoders` | Every built-in body decoder (`body_decoder_registry`: JSON, MessagePack, CBOR, UBJSON, form, text, binary) on the same input: it decodes within the limits (nesting depth, at most one value per byte) or fails with `body_decode_error` (400), nothing else; a decoded value dumps (valid UTF-8), and its JSON, MessagePack and CBOR encodings decode to the same value; it is validated against a JSON Schema without crashing. The input is also parsed as a Content-Type and an Accept header |
| `fuzz_content_decoding` | gzip and deflate decoding (`Content-Encoding` of client responses and server requests) with a maximum decoded size taken from the first two bytes of the input: each decoder decodes within the limit or fails, nothing else; the result is the one decoding without a limit gives, too large exactly when that is larger than the limit (or invalid past it, as decoding stops at the limit) and invalid exactly when that is invalid; the decoded data fits a limit of its own size and not one byte less, and compresses back to itself, as does the input; nothing but zeros may follow a gzip stream (nor anything a zlib or raw deflate one), the input split in two gzip members decodes to itself, and `decode_content()` of a coding (any case, `x-gzip`) or of two of them agrees with the decoders |

Splits and output buffer sizes are derived from a hash of the input, so every failure
reproduces from the input alone.

Seeds are in `corpus/`: `http_request_parser` is used by the three request targets,
`http_response_parser` by the two response targets, `chunked_decoder` has chunked bodies,
each followed by another request. Every seed fits the
`-max_len=4096` used by CI, and a seed is only added if no other has the same bytes.

| Seeds | Origin |
|---|---|
| `http_request_parser/*.txt` | Written for this project: the cases of `tests/integration/request_framing_test.cpp` and the known request smuggling variants (CL.TE, TE.CL, TE.TE obfuscations, obs-fold, chunk extensions, bare LF, chunk sizes with `0x`, signs or overflow, duplicate and list Content-Length, HTTP versions) |
| `chunked_decoder/*.txt` | Written for this project: the chunked bodies of `tests/unit/http/server/body_reader_test.cpp` |
| `body_decoders/*` | Written for this project: an object in each format, every MessagePack, CBOR and UBJSON value type, deep nesting, huge declared lengths, UBJSON arrays of values without payload, forms, a Content-Type and an Accept header |
| `content_decoding/*` | Written for this project (with Python's `gzip` and `zlib`): a two-byte limit followed by gzip and zlib streams, valid, empty, exactly at the limit and one byte over, truncated, with a broken CRC, two gzip members, trailing garbage, zero padding, raw deflate, bytes after a zlib stream, gzip of a zlib stream, 1 MiB of zeros (a small bomb) and data not compressed |
| `http_request_parser/llhttp/`, `chunked_decoder/llhttp/` | The requests of llhttp's test suite (`test/request/*.md` of llhttp v9.4.3, MIT License, Copyright Fedor Indutny), extracted by `tools/extract_llhttp_seeds.py` (the chunked bodies too) |
| `http_request_parser/quirks/`, `chunked_decoder/quirks/` | Generated by `tools/generate_smuggling_seeds.py`: one request per parsing quirk of the classes [HTTP Garden](https://github.com/narfindustries/http-garden) found in real parsers (line terminators, whitespace and stray bytes in the request line, field names and values, Content-Length and chunk sizes read as their longest valid prefix or with `+`, `-`, `0x`, `_`, Transfer-Encoding lists, trailer fields, pipelined, split and shifted requests). HTTP Garden is GPL-3.0: none of its code or payloads is used, only its published bug classes, written here from scratch |
| `http_response_parser/cases/`, `http_response_parser/quirks/` | Generated by `tools/generate_response_seeds.py`, written for this project: the responses of `tests/unit/http/client/response_factory_test.cpp` and `tests/integration/client_response_framing_test.cpp`, and one response per quirk of the classes above applied to responses (status lines, line terminators, field names and values, the `Content-Length` and `Transfer-Encoding` obfuscations and chunk quirks of `generate_smuggling_seeds.py`, interim responses, responses without a body whatever their headers, bodies delimited by the end of the connection, response splitting) |
| `http_response_parser/llhttp/` | The responses of llhttp's test suite (`test/response/*.md` of llhttp v9.4.3), extracted by `tools/extract_llhttp_seeds.py` |
| `http_request_parser/obfuscation/` | Generated by `tools/generate_smuggling_seeds.py`: the Transfer-Encoding and Content-Length obfuscations request smuggling scanners try, in the CL.TE, TE.CL and TE.TE shapes, each followed by a smuggled request. Written for this project, no scanner's code or payload list is copied |

To regenerate them (the llhttp ones from a source checkout of the llhttp release, which has
the `test/` directory, not the `release/` tag):

```sh
python3 tests/fuzz/tools/extract_llhttp_seeds.py <llhttp-9.4.3 source> tests/fuzz/corpus
python3 tests/fuzz/tools/generate_smuggling_seeds.py tests/fuzz/corpus
python3 tests/fuzz/tools/generate_response_seeds.py tests/fuzz/corpus
```

Running one locally (Apple Clang has no libFuzzer, use Homebrew LLVM on macOS):

```sh
CC=/opt/homebrew/opt/llvm/bin/clang CXX=/opt/homebrew/opt/llvm/bin/clang++ \
  cmake -B build-fuzz -DTHINGER_HTTP_ENABLE_FUZZING=ON -DTHINGER_HTTP_BUILD_TESTS=OFF -DTHINGER_HTTP_ENABLE_LOGGING=OFF
cmake --build build-fuzz --target fuzz_http_differential3
mkdir -p corpus-diff3
UBSAN_OPTIONS=halt_on_error=1 build-fuzz/tests/fuzz/fuzz_http_differential3 corpus-diff3 \
  tests/fuzz/corpus/http_request_parser -max_len=4096 -max_total_time=600 -jobs=8 -workers=8
```

`FUZZ_TRACE=1 build-fuzz/tests/fuzz/fuzz_http_differential3 <files>` prints the result of
each parser for every request of the given inputs.

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


## Three-way differential against llhttp and Beast

`fuzz_http_differential3` compares the same processing with
[llhttp](https://github.com/nodejs/llhttp) (MIT), Node.js's parser: strict by default (no
lenient flag is set) and the parser most studied against request smuggling, so a better
reference than Beast, which is lenient. llhttp is only fetched and built for the fuzz targets
(`THINGER_HTTP_ENABLE_FUZZING`), never linked into the library: `tests/fuzz/CMakeLists.txt`
downloads the `release/v9.4.3` tag (pinned by URL and SHA-256), which carries the generated C
sources, and builds them as a static library with the fuzzing flags, so the fuzzer is guided
by llhttp's coverage too.

The target aborts when we and llhttp disagree, as the Beast target does with Beast: we accept
a request llhttp rejects, one of them completes a request the other still needs data for, or
both accept it but differ on where it ends, the framing, the decoded body, the method, the
target or the version. Rejecting what llhttp accepts is fine. After a request llhttp sees as
an upgrade (`Upgrade` with `Connection: upgrade`) the connection carries another protocol, so
pipelined requests are no longer followed.

Beast parses every request too, but only as a third opinion: its disagreements with llhttp
are counted by class, the first input of each class printed, and the totals printed at exit
(`Beast vs llhttp disagreements`), never an error. So are the known discrepancies the target
normalizes.

### Bugs found by the llhttp differential

- **CONNECT was parsed as any other request.** With an origin-form target (the only one we
  accept), `CONNECT /a` with a `Content-Length` had its body read and the next request parsed
  after it, while for llhttp, as for any proxy implementing CONNECT, a CONNECT request has no
  content and what follows its header section is tunnel data (RFC 9110 section 9.3.6). CONNECT
  is not served (authority-form targets were already rejected), so it is now rejected.
  Regression tests in `request_factory_test.cpp` and `request_framing_test.cpp`.
- **Bytes above 0x7F were accepted in the request target** (raw UTF-8, `\xff`). The target is
  ASCII (RFC 9112 section 3.2, RFC 3986): other bytes must be percent-encoded, and RFC 9112
  section 3 asks to reject rather than autocorrect an invalid request line, as it may be
  crafted to bypass filters along the request chain. They are now rejected, as llhttp does.
  Regression tests in `request_factory_test.cpp` and `request_framing_test.cpp`.
- **A target starting with an encoded slash was accepted.** The origin form (a target
  starting with `/`) was checked on the percent-decoded target, so `GET %2Fhealth` was served
  as `/health`, though it is not in origin form (RFC 9112 section 3.2.1) and any other parser
  reads it differently (llhttp rejects it). The raw target must now start with `/`.
  Regression tests in `request_factory_test.cpp` and `request_framing_test.cpp`.

## Known discrepancies

Rejecting what a reference accepts needs no action. Where we accept what a reference
rejects, the case is marked **normalized**: `normalize_known_discrepancy()` in the target of
that reference recognizes it and rewrites the input (keeping its framing) so the reference no
longer rejects it for that reason, then compares again, so the rest of the request is still
checked. Cases where all three agree (bare CR or LF, `Content-Length` with a sign or other
than digits, both `Content-Length` and `Transfer-Encoding`, chunk sizes with `0x`, signs or
overflow, chunk data longer than its size...) are not listed.

| Case | Ours | Beast | llhttp |
|---|---|---|---|
| Field value without exactly one space after the colon (`Name:value`, `Name:  value`), trailing whitespace, HTAB in it | rejected (so are such `Content-Length` and `Transfer-Encoding`) | accepted, OWS trimmed | accepted, OWS trimmed |
| Obsolete line folding, in headers or trailers | rejected | unfolded | rejected |
| Whitespace before the first header line or a field name | rejected | accepted | rejected |
| `Content-Length: 5, 5` | rejected | accepted (same values) | rejected |
| Repeated `Content-Length` headers with the same value | accepted (RFC 9110 section 8.6) | accepted | rejected, **normalized** |
| `Transfer-Encoding` other than a single `chunked` (`gzip, chunked`, `chunked, chunked`, two headers, `identity`, unknown codings) | rejected | chunked if the last coding is chunked; otherwise **no body**, so the bytes that follow are the next request | chunked if the last coding (of any header) is chunked; otherwise rejected |
| Empty `Transfer-Encoding` | rejected | accepted | rejected |
| `Transfer-Encoding` in an HTTP/1.0 request | rejected (RFC 9112 section 6.1) | accepted | accepted |
| Request target other than origin form (absolute form, authority form, `*`) | rejected | accepted | accepted |
| Request target starting with an encoded slash (`%2Fa`) | rejected (fixed, see above) | accepted | rejected |
| Request target with invalid percent-encoding or `..` | rejected | accepted | accepted |
| Request target with bytes above 0x7F | rejected (fixed, see above) | accepted | rejected |
| `CONNECT` | rejected (fixed, see above) | parsed as any other request (body read) | no content: what follows is tunnel data |
| Methods llhttp does not know (any token, lowercase, RTSP methods like `DESCRIBE` in HTTP) | accepted as `UNKNOWN` (RFC 9110 section 9.1), for the application to answer | accepted | rejected, **normalized** (replaced by `GET`) |
| `PRI` method | accepted as `UNKNOWN`, as the HTTP/2 preface intends for an HTTP/1.1 server (RFC 9113 section 3.4) | accepted | always the HTTP/2 preface: waits for the rest of it, stops at it or rejects what does not follow it, **normalized** (replaced by `GET`) |
| `HTTP/1.2` to `HTTP/1.9` | accepted, handled as HTTP/1.1 (RFC 9110 section 2.5) | rejected, **normalized** | rejected, **normalized** |
| `HTTP/0.9` and `HTTP/2.0` request lines, request line without a version | rejected | rejected | accepted |
| Empty lines (CRLF) before the request line | rejected | rejected | skipped |
| Two spaces between request line elements | rejected | rejected | accepted |
| Whitespace between the chunk size and `;` (`5 ;ext`) | rejected | accepted (BWS) | rejected |
| Whitespace after `;` or around `=` in chunk extensions (`5; a = b`) | accepted (BWS, RFC 9112 section 7.1.1) | accepted | rejected, **normalized** (extension removed) |
| Chunk extension without a name (`5;=b`) | rejected | rejected | accepted |
| Control characters in a quoted chunk extension value | rejected (qdtext / quoted-pair) | accepted up to the closing quote | rejected |
| `Content-Length`, `Transfer-Encoding` in the trailer section | accepted: trailers are ignored, never merged into the headers, so they cannot change the framing | rejected, **normalized** | rejected, **normalized** |
| `Connection` / `Proxy-Connection` values that are not a valid token list | accepted: we only look for the tokens we know, it does not frame the message | rejected, **normalized** | accepted |
| Header section over 100 lines or 16 KB | rejected (431) | 8 KB limit by default (disabled in the targets) | no limit |

Not compared: keep-alive (we do not validate the `Connection` list), unknown methods (we keep
them as `UNKNOWN`, so only known methods are compared by name), and header fields other than
the framing ones.

## Responses: three-way differential against llhttp and Beast

`fuzz_http_response_differential` compares how the client reads responses with llhttp in
response mode, as Node.js's HTTP client uses it (strict, no lenient flag set, and
`on_headers_complete` returning 1 for the response to a `HEAD` request, as Node.js does).
Each input is what a server sent before closing the connection, read as the response to a
`GET` and to a `HEAD` request: interim responses are skipped, as the client does, and the end
of the input ends the connection (`response_factory::end_of_stream()`, `llhttp_finish()`),
which completes a body delimited by it. The target aborts when we and llhttp disagree: one
accepts a response the other rejects or still needs data for, or both accept it but differ on
where it ends, the status, the version, the body, its chunked framing, or whether the
connection delimited it. Rejecting what llhttp accepts is fine. Responses following a response
are compared too, while the connection would stay open. Beast's `response_parser` reads every
input as a third opinion, its disagreements with llhttp only counted.

### Behavior of the previous response parser

The parser of the client was rewritten with the rules below; these are the problems of the
previous one, now covered by `response_factory_test.cpp`, `client_response_framing_test.cpp`
and the seeds of `http_response_parser/cases/`:

- **`Content-Length` together with `Transfer-Encoding`, or `Transfer-Encoding` other than
  exactly `chunked`**: the last of them decided the framing, and any other coding was
  ignored (`gzip, chunked` was read as having no body, or by its `Content-Length`).
- **Bodies delimited by the end of the connection were not read**: a response without
  `Content-Length` nor `Transfer-Encoding` ended with its head, and the connection was kept,
  so its body was read as the response to the next request (a redirect to the same server).
- **Data received past a response was dropped, and the connection kept**: the next response
  was taken from wherever the connection was.
- **Interim responses** (`100 Continue`, `103 Early Hints`) were returned as the response,
  leaving the final one in the connection; **204 and 304** with a `Content-Length` waited for
  a body that never came (a 304 to a conditional request carries the length of the resource).
- **Numbers were not bounded**: versions and status codes of any number of digits, truncated
  (`HTTP/1.1 65736` read as 200), chunk sizes overflowing; control characters in the reason.
- **Chunk extensions and trailers failed** valid responses, as the chunk size had to be
  followed by CRLF, and the last chunk by the final CRLF.
- **Errors reached the caller as `invalid_argument`**, and the connection was not closed.

### Findings of the response targets

None in the parser. The first runs found two discrepancies with llhttp, both deliberate
(`chunked\t` as another coding, interim responses from HTTP/1.0 or with `Connection: close`;
see below), and issues of the harness only: `llhttp_message_needs_eof()` reads flags that a
`Transfer-Encoding` trailer field changes, so it is taken when the head ends; a response we
still need data for (a chunked body cut short) can be one llhttp reads otherwise, so it is
normalized too; and an input can need one rewrite of each known class.

### Known discrepancies of responses

As for requests, the cases where we accept what llhttp rejects, or read it otherwise, are
**normalized** by `normalize_known_discrepancy()` of the target: the input is rewritten so
llhttp no longer does it for that reason, and compared again.

| Case | Ours | llhttp |
|---|---|---|
| `HTTP/1.2` to `HTTP/1.9` | accepted, handled as HTTP/1.1 (RFC 9110 section 2.5) | rejected, **normalized** |
| `HTTP/0.9`, `HTTP/2.0`, `RTSP/1.0`, `ICE/1.0` status lines | rejected | accepted |
| Status codes out of 100-599 | rejected (RFC 9110 section 15) | accepted (000 to 999) |
| Control characters in the reason phrase | rejected | accepted (anything but CR and LF) |
| Empty lines (CRLF) before the status line | rejected | skipped |
| Repeated `Content-Length` headers with the same value | accepted (RFC 9110 section 8.6) | rejected, **normalized** |
| `Transfer-Encoding` other than a single `chunked` (`gzip, chunked`, `chunked, gzip`, `gzip`, two headers) | rejected: no other coding is supported, and the body would be handed coded | chunked if the last coding is chunked, otherwise the body ends with the connection (RFC 9112 section 6.3, item 4) |
| `Transfer-Encoding` in an HTTP/1.0 response | rejected (RFC 9112 section 6.1: faulty framing) | accepted |
| HTAB around `chunked` (`chunked\t`) | chunked: OWS is not part of the value (RFC 9110 section 5.5) | another coding, so the body ends with the connection; **normalized** (HTAB made SP) |
| Interim response other than 100 to 103 with `Content-Length` or `Transfer-Encoding` | no body (RFC 9112 section 6.3: none for any 1xx) | reads a body; **normalized** (made 100) |
| Interim response from HTTP/1.0 or with `Connection: close`, followed by the final response | skipped: the final response decides whether the connection stays open | rejected (data after `Connection: close`), **normalized** (made HTTP/1.1, `Connection` overwritten) |
| Whitespace after `;` or around `=` in chunk extensions | accepted (BWS, RFC 9112 section 7.1.1) | rejected, **normalized** (extension removed) |
| `Content-Length`, `Transfer-Encoding` in the trailer section | accepted: trailers are dropped, never merged into the headers | rejected, **normalized** |
| Header section over 100 lines or 16 KB | rejected | no limit |
| A 2xx to `CONNECT` | no body: the connection is a tunnel | not compared (llhttp does not know the request method of a response) |

Not compared: keep-alive beyond the framing (whether the connection is delimited by its end),
and header fields other than the framing ones.
