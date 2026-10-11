#!/usr/bin/env python3
"""Generate seeds for the response parser targets (fuzz_http_response_parser and
fuzz_http_response_differential): what a server sends on a connection before closing it.

Written for this project:

- cases/: the responses of tests/unit/http/client/response_factory_test.cpp and
  tests/integration/client_response_framing_test.cpp, valid and malformed.
- quirks/: one response per parsing quirk, in the classes of generate_smuggling_seeds.py
  applied to responses: status lines (versions, status codes, reason phrases), line
  terminators, whitespace and stray bytes in field names and values, the Content-Length and
  Transfer-Encoding obfuscations of request smuggling (shared with that script), chunk lines
  and trailers, interim responses, responses without a body whatever their headers, bodies
  delimited by the end of the connection, and response splitting (responses followed by
  another one, or by part of one).

Seeds already present anywhere under the corpus (same bytes) are skipped. Every seed fits
in 4096 bytes (the -max_len used by CI).

Usage: generate_response_seeds.py <tests/fuzz/corpus>
"""

import hashlib
import pathlib
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from generate_smuggling_seeds import CL_OBFUSCATIONS, NEXT, TE_OBFUSCATIONS, quirks as request_quirks  # noqa: E402

CRLF = b"\r\n"
# Another response on the same connection, as a server splitting its response would send
NEXT_RESPONSE = b"HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nsmuggled"


def response(headers=(), body=b"", status=b"200 OK", version=b"HTTP/1.1", eol=CRLF, end=None):
    """A response with `headers` (bytes lines, without terminator) and `body`. `eol`
    terminates the status line and header lines, `end` the header section."""
    out = version + b" " + status + eol
    for header in headers:
        out += header + eol
    return out + (eol if end is None else end) + body


def chunked(*headers, body=b"5\r\nhello\r\n0\r\n\r\n", **kwargs):
    return response((b"Transfer-Encoding: chunked",) + headers, body, **kwargs)


def cl(value, body=b"hello", *headers, **kwargs):
    return response((b"Content-Length: " + value,) + headers, body, **kwargs)


def cases():
    """(name, seed): the responses of the unit and integration tests"""
    c = []

    def add(name, data):
        c.append((name, data))

    add("cl", cl(b"5", b"hello"))
    add("cl_then_next", cl(b"5", b"hello" + NEXT_RESPONSE))
    add("cl_zero", cl(b"0"))
    add("cl_repeated_same", response((b"Content-Length: 5", b"Content-Length: 5"), b"hello"))
    add("cl_leading_zeros", cl(b"005", b"hello"))
    add("chunked", chunked(body=b"5;name=value\r\nhello\r\n6\r\n world\r\n0\r\nTrailer: x\r\n\r\n" + NEXT_RESPONSE))
    add("chunked_capitalized", response((b"Transfer-Encoding: Chunked",), b"0\r\n\r\n"))
    add("chunked_ows", response((b"Transfer-Encoding:  chunked ",), b"0\r\n\r\n"))
    add("until_close", response((), b"all of this, Content-Length: 5\r\n\r\nHTTP/1.1 200 OK\r\n\r\n"))
    add("until_close_http10", response((b"Content-Type: text/plain",), b"body", version=b"HTTP/1.0"))
    add("no_content_cl", response((b"Content-Length: 5",), b"hello", status=b"204 No Content"))
    add("not_modified_cl", response((b"Content-Length: 5",), b"hello", status=b"304 Not Modified"))
    add("no_content_empty", response(status=b"204 No Content"))
    add("connect_established", response((b"Content-Length: 5",), b"tunnel", status=b"200 Connection Established"))
    add("interim_continue_early_hints", b"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </a.css>; rel=preload\r\n\r\n" + cl(b"5", b"hello"))
    add("interim_processing_cl", b"HTTP/1.1 102 Processing\r\nContent-Length: 5\r\n\r\n" + cl(b"2", b"ok"))
    add("switching_protocols", b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n\x81\x02hi")
    add("switching_protocols_other", b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: other\r\nConnection: Upgrade\r\n\r\n")
    add("keep_alive_close", response((b"Connection: close", b"Content-Length: 0")))
    add("keep_alive_http10", response((b"Connection: keep-alive", b"Content-Length: 0"), version=b"HTTP/1.0"))
    add("status_no_reason_space", b"HTTP/1.1 204\r\n\r\n")
    add("status_empty_reason", b"HTTP/1.1 204 \r\n\r\n")
    add("status_reason_tab_utf8", b"HTTP/1.1 200 \tOK \xc3\xa9\r\nContent-Length: 0\r\n\r\n")
    add("version_1_0", b"HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n")
    add("version_1_2", b"HTTP/1.2 200 OK\r\nContent-Length: 0\r\n\r\n")
    add("ows", b"HTTP/1.1 200\r\nX-Value:\t value \r\nContent-Length:5 \r\n\r\nhello")
    add("ows_values", response((b"A:1", b"B:  2 \t", b"C:\t3", b"D:", b"Content-Length:0")))
    add("obs_text_value", response((b"X-A: a \t b\xff", b"Content-Length: 0")))
    add("obs_fold_cl", b"HTTP/1.1 200 OK\r\nContent-Lengt: 5\r\n h\r\n\r\nhello")
    add("obs_fold_tab", b"HTTP/1.1 200 OK\r\nX-A: 1\r\n\t2\r\nContent-Length: 0\r\n\r\n")
    add("clte", response((b"Content-Length: 3", b"Transfer-Encoding: chunked"), b"3\r\nabc\r\n0\r\n\r\n"))
    add("tecl", response((b"Transfer-Encoding: chunked", b"Content-Length: 3"), b"3\r\nabc\r\n0\r\n\r\n"))
    add("te_gzip_chunked", response((b"Transfer-Encoding: gzip, chunked",), b"0\r\n\r\n"))
    add("te_gzip", response((b"Transfer-Encoding: gzip",), b"abc"))
    add("te_twice", response((b"Transfer-Encoding: chunked", b"Transfer-Encoding: chunked"), b"0\r\n\r\n"))
    add("te_http10", response((b"Transfer-Encoding: chunked",), b"0\r\n\r\n", version=b"HTTP/1.0"))
    add("cl_different", response((b"Content-Length: 3", b"Content-Length: 4"), b"abcd"))
    add("cl_garbage", cl(b"3abc", b"abc"))
    add("status_2000", b"HTTP/1.1 2000 OK\r\nContent-Length: 0\r\n\r\n")
    add("version_1_10", b"HTTP/1.10 200 OK\r\nContent-Length: 0\r\n\r\n")
    add("version_2_0", b"HTTP/2.0 200 OK\r\nContent-Length: 0\r\n\r\n")
    add("bare_lf", b"HTTP/1.1 200 OK\nContent-Length: 0\n\n")
    add("other_protocol", b"SSH-2.0-OpenSSH_9.6\r\n")
    add("space_before_colon", b"HTTP/1.1 200 OK\r\nContent-Length : 0\r\n\r\n")
    add("chunk_size_overflow", chunked(body=b"fffffffffffffffffff\r\n"))
    add("chunk_size_0x", chunked(body=b"0x5\r\nhello\r\n0\r\n\r\n"))
    add("chunk_data_longer", chunked(body=b"5\r\nhelloXX0\r\n\r\n"))
    add("cl_truncated", cl(b"10", b"short"))
    add("chunked_truncated", chunked(body=b"5\r\nhello\r\n"))
    add("redirect_cl_smuggled", response((b"Location: /final", b"Content-Length: 0"), NEXT_RESPONSE, status=b"302 Found"))
    add("redirect_until_close", response((b"Location: /final",), NEXT_RESPONSE, status=b"302 Found"))
    add("redirect_clte_smuggled", response((b"Location: /final", b"Content-Length: 0", b"Transfer-Encoding: chunked"), NEXT_RESPONSE, status=b"302 Found"))
    return c


def quirks():
    """(name, seed)"""
    q = []

    def add(name, data):
        q.append((name, data))

    # Status lines
    for name, line in (
        ("sl_version_0_9", b"HTTP/0.9 200 OK"),
        ("sl_version_1_9", b"HTTP/1.9 200 OK"),
        ("sl_version_2", b"HTTP/2 200 OK"),
        ("sl_version_leading_zero", b"HTTP/01.1 200 OK"),
        ("sl_version_two_digit_minor", b"HTTP/1.01 200 OK"),
        ("sl_version_lowercase", b"http/1.1 200 OK"),
        ("sl_version_rtsp", b"RTSP/1.0 200 OK"),
        ("sl_version_ice", b"ICE/1.0 200 OK"),
        ("sl_version_space", b"HTTP /1.1 200 OK"),
        ("sl_two_spaces", b"HTTP/1.1  200 OK"),
        ("sl_tab_separator", b"HTTP/1.1\t200 OK"),
        ("sl_leading_space", b" HTTP/1.1 200 OK"),
        ("sl_status_000", b"HTTP/1.1 000 OK"),
        ("sl_status_099", b"HTTP/1.1 099 OK"),
        ("sl_status_199", b"HTTP/1.1 199 Interim"),
        ("sl_status_599", b"HTTP/1.1 599 Custom"),
        ("sl_status_600", b"HTTP/1.1 600 Custom"),
        ("sl_status_999", b"HTTP/1.1 999 Custom"),
        ("sl_status_two_digits", b"HTTP/1.1 20 OK"),
        ("sl_status_four_digits", b"HTTP/1.1 2000 OK"),
        ("sl_status_sign", b"HTTP/1.1 +200 OK"),
        ("sl_status_hex", b"HTTP/1.1 0x0 OK"),
        ("sl_status_no_space", b"HTTP/1.1 200OK"),
        ("sl_reason_nul", b"HTTP/1.1 200 O\x00K"),
        ("sl_reason_cr", b"HTTP/1.1 200 O\rK"),
        ("sl_reason_del", b"HTTP/1.1 200 O\x7fK"),
        ("sl_reason_high_bytes", b"HTTP/1.1 200 \xff\xfe"),
        ("sl_reason_trailing_space", b"HTTP/1.1 200 OK "),
        ("sl_only_version", b"HTTP/1.1"),
        ("sl_only_status", b"HTTP/1.1 200"),
    ):
        add(name, line + b"\r\nContent-Length: 5\r\n\r\nhello" + NEXT_RESPONSE)
    add("sl_leading_crlf", b"\r\n" + cl(b"5"))
    add("sl_leading_lf", b"\n" + cl(b"5"))
    add("sl_request_line", NEXT + cl(b"5"))

    # Line terminators
    add("eol_bare_lf_all", response((b"Content-Length: 5",), b"hello", eol=b"\n"))
    add("eol_bare_cr_status", b"HTTP/1.1 200 OK\rContent-Length: 5\r\n\r\nhello")
    add("eol_bare_cr_header", b"HTTP/1.1 200 OK\r\nX: y\rContent-Length: 5\r\n\r\nhello")
    add("eol_cr_cr_lf", b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\r\n\r\nhello")
    add("eol_lf_end", b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\nhello")
    add("eol_cr_end", b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\rhello")
    add("eol_lf_in_value_te", b"HTTP/1.1 200 OK\r\nX: y\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\n0\r\n\r\n" + NEXT_RESPONSE)

    # Field names and values
    for name, header in (
        ("name_space_before_colon_cl", b"Content-Length : 5"),
        ("name_tab_before_colon_cl", b"Content-Length\t: 5"),
        ("name_leading_space_cl", b" Content-Length: 5"),
        ("name_nul_cl", b"Content-Length\x00: 5"),
        ("name_a0_cl", b"Content-Length\xa0: 5"),
        ("name_empty", b": 5"),
        ("name_no_colon", b"Content-Length 5"),
        ("name_uppercase_cl", b"CONTENT-LENGTH: 5"),
        ("value_nul", b"X: a\x00b"),
        ("value_vt", b"X: a\x0bb"),
        ("value_del", b"X: a\x7fb"),
        ("value_a0", b"X: a\xa0b"),
        ("value_only_spaces", b"X:    "),
        ("value_tabs", b"X:\t\ta\tb\t"),
    ):
        add(name, response((header, b"Content-Length: 5"), b"hello" + NEXT_RESPONSE))

    # Content-Length values
    for name, value in (
        ("cl_prefix_valid", b"5a"),
        ("cl_space_digit", b"5 5"),
        ("cl_decimal", b"5.0"),
        ("cl_minus", b"-5"),
        ("cl_minus_zero", b"-0"),
        ("cl_hex", b"0x5"),
        ("cl_list_same", b"5, 5"),
        ("cl_empty", b""),
        ("cl_2pow64", b"18446744073709551616"),
        ("cl_2pow64_plus5", b"18446744073709551621"),
        ("cl_huge", b"99999999999999999999999"),
        ("cl_trailing_tab", b"5\t"),
    ):
        add(name, cl(value, b"hello" + NEXT_RESPONSE))
    add("cl_first_short_then_long", response((b"Content-Length: 1", b"Content-Length: 5"), b"hello"))
    add("cl_larger_than_body", cl(b"20", b"hello" + NEXT_RESPONSE))
    add("cl_zero_with_body", cl(b"0", b"hello" + NEXT_RESPONSE))
    for name, line in CL_OBFUSCATIONS:
        add(f"cl_{name}", response((line.replace(b"{n}", b"5"),), b"hello" + NEXT_RESPONSE))

    # Transfer-Encoding: the obfuscations of request smuggling, alone and with Content-Length
    smuggle = b"0\r\n\r\n" + NEXT_RESPONSE
    for name, te in TE_OBFUSCATIONS:
        add(f"te_{name}", response((te,), b"5\r\nhello\r\n" + smuggle))
        add(f"clte_{name}", response((b"Content-Length: %d" % len(smuggle), te), smuggle))
    add("te_http10_cl", response((b"Transfer-Encoding: chunked", b"Content-Length: 5"), b"0\r\n\r\n", version=b"HTTP/1.0"))
    add("te_chunked_then_gzip_headers", response((b"Transfer-Encoding: chunked", b"Transfer-Encoding: gzip"), b"5\r\nhello\r\n0\r\n\r\n"))

    # Chunk lines and trailers: the chunk quirks of the request seeds
    for name, _, body in request_quirks():
        if body is not None and body.endswith(NEXT):
            add(name, chunked(body=body[:-len(NEXT)] + NEXT_RESPONSE))

    # Responses without a body, whatever their headers
    for status in (b"100 Continue", b"102 Processing", b"103 Early Hints", b"150 Interim", b"204 No Content", b"304 Not Modified"):
        code = status[:3].decode()
        add(f"nobody_{code}_cl", response((b"Content-Length: 5",), b"hello", status=status) + NEXT_RESPONSE)
        add(f"nobody_{code}_chunked", response((b"Transfer-Encoding: chunked",), b"5\r\nhello\r\n0\r\n\r\n", status=status) + NEXT_RESPONSE)
        add(f"nobody_{code}_clte", response((b"Content-Length: 5", b"Transfer-Encoding: chunked"), b"0\r\n\r\n", status=status) + NEXT_RESPONSE)

    # Interim responses
    add("interim_many", b"HTTP/1.1 100 Continue\r\n\r\n" * 20 + cl(b"2", b"ok"))
    add("interim_only", b"HTTP/1.1 100 Continue\r\n\r\n")
    add("interim_folded", b"HTTP/1.1 103 Early Hints\r\nLink: </a>\r\n </b>\r\n\r\n" + cl(b"2", b"ok"))
    add("interim_http10", b"HTTP/1.0 100 Continue\r\n\r\n" + cl(b"2", b"ok"))
    add("interim_then_101", b"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\ndata")
    add("switching_without_upgrade", b"HTTP/1.1 101 Switching Protocols\r\nContent-Length: 5\r\n\r\nhello")
    add("switching_chunked", b"HTTP/1.1 101 Switching Protocols\r\nTransfer-Encoding: chunked\r\nUpgrade: x\r\nConnection: upgrade\r\n\r\n0\r\n\r\n")
    add("upgrade_headers_200", response((b"Upgrade: websocket", b"Connection: Upgrade", b"Content-Length: 5"), b"hello" + NEXT_RESPONSE))

    # Bodies delimited by the end of the connection, and response splitting
    add("until_close_with_response", response((b"X: y",), NEXT_RESPONSE + NEXT_RESPONSE))
    add("until_close_connection_close", response((b"Connection: close",), b"body"))
    add("split_cl_then_partial", cl(b"5", b"hello" + NEXT_RESPONSE[:20]))
    add("split_chunked_then_crlf", chunked() + b"\r\n" + NEXT_RESPONSE)
    add("split_three", cl(b"5", b"hello") + chunked() + NEXT_RESPONSE)
    add("split_close_then_more", response((b"Connection: close", b"Content-Length: 5"), b"hello" + NEXT_RESPONSE))
    add("split_http10_then_more", response((b"Content-Length: 5",), b"hello" + NEXT_RESPONSE, version=b"HTTP/1.0"))
    add("split_garbage_after", cl(b"5", b"hello\r\n\r\n"))

    # Limits
    add("limit_many_headers", response(tuple(b"X-%d: a" % i for i in range(101)) + (b"Content-Length: 0",)))
    add("limit_long_reason", b"HTTP/1.1 200 " + b"x" * 3000 + b"\r\nContent-Length: 0\r\n\r\n")
    return q


def main():
    corpus = pathlib.Path(sys.argv[1])
    outputs = {
        "cases": corpus / "http_response_parser" / "cases",
        "quirks": corpus / "http_response_parser" / "quirks",
    }
    generated = set(outputs.values())
    known = {hashlib.sha1(p.read_bytes()).digest() for p in corpus.rglob("*") if p.is_file() and p.parent not in generated}
    for out in generated:
        out.mkdir(parents=True, exist_ok=True)
        for old in out.iterdir():
            old.unlink()

    written = dict.fromkeys(generated, 0)

    def write(out, name, data):
        assert len(data) <= 4096, name
        digest = hashlib.sha1(data).digest()
        if digest in known:
            return
        known.add(digest)
        (out / name).write_bytes(data)
        written[out] += 1

    for name, data in cases():
        write(outputs["cases"], name, data)
    for name, data in quirks():
        write(outputs["quirks"], name, data)
    for out, count in written.items():
        print(f"{out}: {count} seeds")


if __name__ == "__main__":
    main()
