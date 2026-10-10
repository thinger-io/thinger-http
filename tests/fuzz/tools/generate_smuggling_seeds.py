#!/usr/bin/env python3
"""Generate request smuggling seeds for the request parser targets.

Written for this project (no code or payloads copied from other tools):

- quirks/: one seed per parsing quirk of the classes that HTTP Garden
  (github.com/narfindustries/http-garden, GPL-3.0, so only its published bug classes are
  reused, not its code) found in real parsers: line terminators (bare CR / LF, CR CR LF),
  whitespace in the request line and around field names and values, bytes like NUL, VT,
  FF, 0x85 or 0xA0 where they do not belong, Content-Length and chunk sizes read as their
  longest valid prefix or with C-style prefixes (+, -, 0x, _), Transfer-Encoding lists,
  folded framing fields, trailer fields, and request level mutations (pipelined,
  duplicated, split or shifted requests).
- obfuscation/: the Transfer-Encoding (and Content-Length) obfuscations request smuggling
  scanners try, each in the CL.TE, TE.CL and (some) TE.TE shapes, followed by a smuggled
  request.

Also writes the chunked bodies of the chunk-level quirks (followed by another request) into
chunked_decoder/quirks/. Seeds already present anywhere under the corpus (same bytes) are
skipped. Every seed fits in 4096 bytes (the -max_len used by CI).

Usage: generate_smuggling_seeds.py <tests/fuzz/corpus>
"""

import hashlib
import pathlib
import sys

CRLF = b"\r\n"
SMUGGLED = b"GET /smuggled HTTP/1.1\r\nHost: localhost\r\n\r\n"
NEXT = b"GET /next HTTP/1.1\r\nHost: localhost\r\n\r\n"


def request(headers=(), body=b"", method=b"POST", target=b"/", version=b"HTTP/1.1", eol=CRLF, end=None):
    """A request with a Host header first, `headers` (bytes lines, without terminator) and
    `body`. `eol` terminates the request line and header lines, `end` the header section."""
    line = method + b" " + target + b" " + version
    out = line + eol + b"Host: localhost" + eol
    for header in headers:
        out += header + eol
    return out + (eol if end is None else end) + body


def chunked(*headers, body=b"5\r\nhello\r\n0\r\n\r\n", **kwargs):
    return request((b"Transfer-Encoding: chunked",) + headers, body, **kwargs)


def cl(value, body=b"hello", *headers, **kwargs):
    return request((b"Content-Length: " + value,) + headers, body, **kwargs)


def quirks():
    """(name, request seed, chunked body seed or None)"""
    q = []

    def add(name, data, body=None):
        q.append((name, data, body))

    # Line terminators
    add("eol_bare_cr_request_line", b"GET / HTTP/1.1\rHost: localhost\r\n\r\n")
    add("eol_bare_cr_header", b"GET / HTTP/1.1\r\nHost: localhost\rX: y\r\n\r\n")
    add("eol_bare_cr_end", b"GET / HTTP/1.1\r\nHost: localhost\r\n\r" + NEXT)
    add("eol_cr_cr_lf_header", b"GET / HTTP/1.1\r\nHost: localhost\r\r\n\r\n")
    add("eol_cr_cr_lf_end", b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\r\n")
    add("eol_lf_cr_lf_end", b"GET / HTTP/1.1\r\nHost: localhost\n\r\n")
    add("eol_lf_lf_end", b"GET / HTTP/1.1\nHost: localhost\n\n")
    add("eol_mixed_end", b"GET / HTTP/1.1\r\nHost: localhost\r\n\n" + NEXT)
    add("eol_cr_in_value", request((b"X: a\rb",), method=b"GET"))
    add("eol_lf_in_value_te", b"POST / HTTP/1.1\r\nHost: localhost\r\nX: y\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\n0\r\n\r\n" + SMUGGLED)
    add("eol_lf_before_colon", request((b"Transfer-Encoding\n: chunked", b"Content-Length: 5"), b"0\r\n\r\nX"))
    add("eol_empty_field_line", b"GET / HTTP/1.1\r\nHost: localhost\r\n:\r\n\r\n")
    add("eol_colon_line_then_cl", b"POST / HTTP/1.1\r\nHost: localhost\r\n:\r\nContent-Length: 5\r\n\r\nhello")

    # Request line whitespace and syntax
    for name, line in (
        ("rl_leading_space", b" GET / HTTP/1.1"),
        ("rl_leading_crlf_twice", b"\r\n\r\nGET / HTTP/1.1"),
        ("rl_tab_separators", b"GET\t/\tHTTP/1.1"),
        ("rl_vt_separator", b"GET\x0b/ HTTP/1.1"),
        ("rl_ff_separator", b"GET /\x0cHTTP/1.1"),
        ("rl_trailing_space", b"GET / HTTP/1.1 "),
        ("rl_trailing_tab", b"GET / HTTP/1.1\t"),
        ("rl_two_spaces_after_target", b"GET /  HTTP/1.1"),
        ("rl_space_in_target", b"GET /a b HTTP/1.1"),
        ("rl_nul_in_target", b"GET /a\x00b HTTP/1.1"),
        ("rl_del_in_target", b"GET /a\x7fb HTTP/1.1"),
        ("rl_high_byte_in_target", b"GET /\xa0 HTTP/1.1"),
        ("rl_fragment", b"GET /a#frag HTTP/1.1"),
        ("rl_query_with_hash", b"GET /a?b=c#d HTTP/1.1"),
        ("rl_percent_00", b"GET /a%00b HTTP/1.1"),
        ("rl_percent_short", b"GET /a%2 HTTP/1.1"),
        ("rl_authority_form", b"CONNECT localhost:443 HTTP/1.1"),
        ("rl_absolute_form", b"GET http://localhost/a HTTP/1.1"),
        ("rl_asterisk_options", b"OPTIONS * HTTP/1.1"),
        ("rl_no_version", b"GET /"),
        ("rl_version_lowercase", b"GET / http/1.1"),
        ("rl_version_leading_zero", b"GET / HTTP/01.1"),
        ("rl_version_two_digit_minor", b"GET / HTTP/1.01"),
        ("rl_version_no_minor", b"GET / HTTP/1"),
        ("rl_version_no_minor_dot", b"GET / HTTP/1."),
        ("rl_version_2", b"GET / HTTP/2"),
        ("rl_version_3_0", b"GET / HTTP/3.0"),
        ("rl_version_1_9", b"GET / HTTP/1.9"),
        ("rl_version_space", b"GET / HTTP /1.1"),
        ("rl_method_nul", b"GE\x00T / HTTP/1.1"),
        ("rl_method_lowercase_post", b"post / HTTP/1.1"),
        ("rl_method_extension", b"PROPFIND / HTTP/1.1"),
        ("rl_method_token_chars", b"M-SEARCH! / HTTP/1.1"),
        ("rl_method_rtsp", b"DESCRIBE / HTTP/1.1"),
        ("rl_method_pri", b"PRI / HTTP/1.1"),
    ):
        add(name, line + b"\r\nHost: localhost\r\n\r\n")
    add("rl_h2_preface", b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n")
    add("rl_http09_simple", b"GET /\r\n")

    # Field names
    for name, header in (
        ("name_space_before_colon_cl", b"Content-Length : 5"),
        ("name_tab_before_colon_cl", b"Content-Length\t: 5"),
        ("name_leading_space_cl", b" Content-Length: 5"),
        ("name_nul_cl", b"Content-Length\x00: 5"),
        ("name_a0_cl", b"Content-Length\xa0: 5"),
        ("name_85_cl", b"Content-Length\x85: 5"),
        ("name_underscore_cl", b"Content_Length: 5"),
        ("name_slash", b"Content/Length: 5"),
        ("name_at", b"X@Y: 5"),
        ("name_del", b"X\x7fY: 5"),
        ("name_empty", b": 5"),
        ("name_no_colon", b"Content-Length 5"),
        ("name_lowercase_cl", b"content-length: 5"),
        ("name_uppercase_cl", b"CONTENT-LENGTH: 5"),
    ):
        add(name, request((header,), b"hello"))

    # Field values
    for name, header in (
        ("value_nul", b"X: a\x00b"),
        ("value_del", b"X: a\x7fb"),
        ("value_vt", b"X: a\x0bb"),
        ("value_ff", b"X: a\x0cb"),
        ("value_a0", b"X: a\xa0b"),
        ("value_85", b"X: \x85"),
        ("value_utf8", b"X: \xc3\xa9t\xc3\xa9"),
        ("value_tab_inside", b"X: a\tb"),
        ("value_only_spaces", b"X:    "),
        ("value_leading_tab", b"X:\ta"),
    ):
        add(name, request((header,), method=b"GET"))

    # Content-Length values
    for name, value in (
        ("cl_prefix_valid", b"5a"),
        ("cl_prefix_space_digit", b"5 5"),
        ("cl_decimal_point", b"5.0"),
        ("cl_exponent", b"5e0"),
        ("cl_underscore", b"0_5"),
        ("cl_minus_zero", b"-0"),
        ("cl_octal_like", b"010"),
        ("cl_hex_upper", b"0X5"),
        ("cl_trailing_comma", b"5,"),
        ("cl_leading_comma", b",5"),
        ("cl_list_different", b"5, 6"),
        ("cl_list_three", b"5,5,5"),
        ("cl_arabic_digit", b"\xd9\xa5"),
        ("cl_fullwidth_digit", b"\xef\xbc\x95"),
        ("cl_nul_suffix", b"5\x00"),
        ("cl_vt_prefix", b"\x0b5"),
        ("cl_ff_suffix", b"5\x0c"),
        ("cl_2pow32_plus5", b"4294967301"),
        ("cl_2pow63", b"9223372036854775808"),
        ("cl_quoted", b"\"5\""),
    ):
        add(name, cl(value, b"hello" + NEXT))
    add("cl_first_short_then_long", request((b"Content-Length: 1", b"Content-Length: 5"), b"hello"))
    add("cl_first_long_then_short", request((b"Content-Length: 5", b"Content-Length: 1"), b"hello"))
    add("cl_empty_then_value", request((b"Content-Length:", b"Content-Length: 5"), b"hello"))
    add("cl_folded_digits", request((b"Content-Length: 1", b" 0"), b"helloworld"))
    add("cl_get_body", cl(b"5", b"hello" + NEXT, method=b"GET"))
    add("cl_head_body", cl(b"5", b"hello" + NEXT, method=b"HEAD"))
    add("cl_options_body", cl(b"5", b"hello" + NEXT, method=b"OPTIONS"))
    add("cl_expect_continue", cl(b"5", b"hello", b"Expect: 100-continue"))
    add("cl_short", cl(b"3", b"hello" + NEXT))
    add("cl_long", cl(b"7", b"hello" + NEXT))
    add("cl_zero_with_body", cl(b"0", b"hello" + NEXT))

    # Transfer-Encoding values
    for name, value in (
        ("te_comma_before", b",chunked"),
        ("te_comma_after", b"chunked,"),
        ("te_comma_spaces", b"chunked , chunked"),
        ("te_nul", b"chunked\x00"),
        ("te_uppercase", b"CHUNKED"),
        ("te_mixed_case", b"cHuNkEd"),
        ("te_prefix", b"chunk"),
        ("te_suffix", b"chunkedx"),
        ("te_parameter", b"chunked;q=1"),
        ("te_empty_list_items", b", , chunked"),
        ("te_gzip_only", b"gzip"),
        ("te_deflate_chunked", b"deflate, chunked"),
        ("te_chunked_gzip", b"chunked, gzip"),
        ("te_a0_prefix", b"\xa0chunked"),
        ("te_85_suffix", b"chunked\x85"),
    ):
        add(name, request((b"Transfer-Encoding: " + value,), b"5\r\nhello\r\n0\r\n\r\n" + NEXT))
    add("te_empty", request((b"Transfer-Encoding:", b"Content-Length: 5"), b"hello"))
    add("te_folded_value", request((b"Transfer-Encoding: x", b" chunked"), b"5\r\nhello\r\n0\r\n\r\n"))
    add("te_folded_after_empty", request((b"Transfer-Encoding:", b" chunked"), b"5\r\nhello\r\n0\r\n\r\n"))
    add("te_http10_chunked_cl", request((b"Transfer-Encoding: chunked", b"Content-Length: 5"), b"0\r\n\r\n", version=b"HTTP/1.0"))
    add("te_connection_close_pipelined", chunked(b"Connection: close", body=b"0\r\n\r\n" + NEXT))
    add("te_http10_keepalive_pipelined", chunked(b"Connection: keep-alive", body=b"0\r\n\r\n" + NEXT, version=b"HTTP/1.0"))

    # Chunk lines (with a chunked body seed for the decoder target)
    for name, body in (
        ("chunk_size_0x", b"0x5\r\nhello\r\n0\r\n\r\n"),
        ("chunk_size_0_underscore", b"0_5\r\nhello\r\n0\r\n\r\n"),
        ("chunk_size_plus", b"+5\r\nhello\r\n0\r\n\r\n"),
        ("chunk_size_minus", b"-5\r\nhello\r\n0\r\n\r\n"),
        ("chunk_size_longest_prefix", b"5g\r\nhello\r\n0\r\n\r\n"),
        ("chunk_size_tab_after", b"5\t\r\nhello\r\n0\r\n\r\n"),
        ("chunk_size_space_ext", b"5 ;a=b\r\nhello\r\n0\r\n\r\n"),
        ("chunk_size_17_digits_wrap", b"10000000000000005\r\nhello\r\n0\r\n\r\n"),
        ("chunk_size_leading_zeros_long", b"00000000000000000005\r\nhello\r\n0\r\n\r\n"),
        ("chunk_size_bare_cr", b"5\rhello\r\n0\r\n\r\n"),
        ("chunk_size_cr_cr_lf", b"5\r\r\nhello\r\n0\r\n\r\n"),
        ("chunk_data_lf_only", b"5\r\nhello\n0\r\n\r\n"),
        ("chunk_data_cr_only", b"5\r\nhello\r0\r\n\r\n"),
        ("chunk_data_missing_crlf", b"5\r\nhello0\r\n\r\n"),
        ("chunk_data_extra_crlf", b"5\r\nhello\r\n\r\n0\r\n\r\n"),
        ("chunk_data_longer", b"3\r\nhello\r\n0\r\n\r\n"),
        ("chunk_data_shorter", b"7\r\nhello\r\n0\r\n\r\n"),
        ("chunk_last_bare_lf", b"5\r\nhello\r\n0\n\n"),
        ("chunk_last_cr_cr", b"5\r\nhello\r\n0\r\n\r\r"),
        ("chunk_last_ext", b"5\r\nhello\r\n0;a=b\r\n\r\n"),
        ("chunk_last_ext_quoted_crlf", b"5\r\nhello\r\n0;a=\"\r\n\"\r\n\r\n"),
        ("chunk_ext_lf_in_value", b"5;a=b\nc\r\nhello\r\n0\r\n\r\n"),
        ("chunk_ext_no_name", b"5;=b\r\nhello\r\n0\r\n\r\n"),
        ("chunk_ext_space_after_semicolon", b"5; a=b\r\nhello\r\n0\r\n\r\n"),
        ("chunk_ext_spaces_around_equals", b"5;a = b\r\nhello\r\n0\r\n\r\n"),
        ("chunk_ext_quoted_unterminated", b"5;a=\"b\r\nhello\r\n0\r\n\r\n"),
        ("chunk_ext_high_byte", b"5;a=\xff\r\nhello\r\n0\r\n\r\n"),
        ("trailer_cl", b"5\r\nhello\r\n0\r\nContent-Length: 5\r\n\r\n"),
        ("trailer_te", b"5\r\nhello\r\n0\r\nTransfer-Encoding: chunked\r\n\r\n"),
        ("trailer_no_space", b"5\r\nhello\r\n0\r\nX:y\r\n\r\n"),
        ("trailer_space_before_colon", b"5\r\nhello\r\n0\r\nX : y\r\n\r\n"),
        ("trailer_request_line", b"5\r\nhello\r\n0\r\nGET /x HTTP/1.1\r\n\r\n"),
        ("trailer_empty_name", b"5\r\nhello\r\n0\r\n: y\r\n\r\n"),
        ("trailer_bare_cr", b"5\r\nhello\r\n0\r\nX: y\r\r\n"),
        ("trailer_unterminated", b"5\r\nhello\r\n0\r\nX: y\r\n"),
    ):
        add(name, chunked(body=body + NEXT), body + NEXT)

    # Request level: pipelining, duplicated, split and shifted requests
    get = b"GET /a HTTP/1.1\r\nHost: localhost\r\n\r\n"
    post = b"POST /b HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello"
    chunk = b"POST /c HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n"
    add("pipeline_get_post_chunked", get + post + chunk)
    add("pipeline_chunked_chunked_get", chunk + chunk + get)
    add("pipeline_duplicate_post", post + post)
    add("pipeline_split_request_line", get + b"GET /b HTT")
    add("pipeline_split_headers", get + b"GET /b HTTP/1.1\r\nHost: loc")
    add("pipeline_shift_cl_into_next", post.replace(b"5\r\n\r\nhello", b"9\r\n\r\nhello") + get)
    add("pipeline_shift_chunk_into_next", chunk.replace(b"5\r\nhello", b"9\r\nhello") + get)
    add("pipeline_crlf_between", get + b"\r\n" + get)
    add("pipeline_http10_then_get", b"GET /a HTTP/1.0\r\n\r\n" + get)
    add("pipeline_close_then_get", b"GET /a HTTP/1.1\r\nConnection: close\r\n\r\n" + get)
    add("pipeline_upgrade_then_bytes", b"GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n" + get)
    add("pipeline_upgrade_with_body", b"POST /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: h2c\r\nConnection: Upgrade\r\nContent-Length: 5\r\n\r\nhello" + get)
    add("pipeline_upgrade_without_connection", b"GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n\r\n" + get)
    add("pipeline_connect_with_body", b"CONNECT /a HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello" + get)
    add("pipeline_many_small", get * 8)
    return q


# Transfer-Encoding header lines that some parser may still read as chunked
TE_OBFUSCATIONS = [
    ("lowercase", b"transfer-encoding: chunked"),
    ("uppercase", b"TRANSFER-ENCODING: CHUNKED"),
    ("value_capitalized", b"Transfer-Encoding: Chunked"),
    ("no_space", b"Transfer-Encoding:chunked"),
    ("two_spaces", b"Transfer-Encoding:  chunked"),
    ("tab_value", b"Transfer-Encoding:\tchunked"),
    ("vt_value", b"Transfer-Encoding:\x0bchunked"),
    ("ff_value", b"Transfer-Encoding:\x0cchunked"),
    ("cr_value", b"Transfer-Encoding:\rchunked"),
    ("space_before_colon", b"Transfer-Encoding : chunked"),
    ("tab_before_colon", b"Transfer-Encoding\t: chunked"),
    ("leading_space_name", b" Transfer-Encoding: chunked"),
    ("leading_tab_name", b"\tTransfer-Encoding: chunked"),
    ("underscore_name", b"Transfer_Encoding: chunked"),
    ("x_prefix_value", b"Transfer-Encoding: xchunked"),
    ("x_suffix_value", b"Transfer-Encoding: chunkedx"),
    ("trailing_space_value", b"Transfer-Encoding: chunked "),
    ("trailing_tab_value", b"Transfer-Encoding: chunked\t"),
    ("quoted_value", b"Transfer-Encoding: \"chunked\""),
    ("single_quoted_value", b"Transfer-Encoding: 'chunked'"),
    ("nul_in_value", b"Transfer-Encoding: chun\x00ked"),
    ("high_byte_in_value", b"Transfer-Encoding: chunk\xe9d"),
    ("identity_chunked", b"Transfer-Encoding: identity, chunked"),
    ("chunked_identity", b"Transfer-Encoding: chunked, identity"),
    ("gzip_chunked", b"Transfer-Encoding: gzip, chunked"),
    ("chunked_chunked", b"Transfer-Encoding: chunked, chunked"),
    ("comma_chunked", b"Transfer-Encoding: , chunked"),
    ("semicolon", b"Transfer-Encoding: chunked;"),
    ("folded_value", b"Transfer-Encoding:\r\n chunked"),
    ("folded_tab_value", b"Transfer-Encoding: x\r\n\tchunked"),
    ("lf_smuggled_header", b"X: x\nTransfer-Encoding: chunked"),
    ("cr_smuggled_header", b"X: x\rTransfer-Encoding: chunked"),
    ("lf_before_colon", b"Transfer-Encoding\n: chunked"),
    ("content_encoding", b"Content-Encoding: chunked"),
    ("te_header", b"TE: chunked"),
    ("nul_in_name", b"Transfer-Encoding\x00: chunked"),
    ("a0_in_name", b"Transfer-Encoding\xa0: chunked"),
    ("duplicate_chunked_x", b"Transfer-Encoding: chunked\r\nTransfer-Encoding: x"),
    ("duplicate_x_chunked", b"Transfer-Encoding: x\r\nTransfer-Encoding: chunked"),
    ("duplicate_empty_chunked", b"Transfer-Encoding:\r\nTransfer-Encoding: chunked"),
]

# Content-Length header lines that some parser may read differently
CL_OBFUSCATIONS = [
    ("plus", b"Content-Length: +{n}"),
    ("leading_zeros", b"Content-Length: 000{n}"),
    ("space_before_colon", b"Content-Length : {n}"),
    ("tab_value", b"Content-Length:\t{n}"),
    ("trailing_garbage", b"Content-Length: {n}x"),
    ("list", b"Content-Length: {n}, {n}"),
    ("duplicate_zero_first", b"Content-Length: 0\r\nContent-Length: {n}"),
    ("duplicate_zero_last", b"Content-Length: {n}\r\nContent-Length: 0"),
    ("folded", b"Content-Length:\r\n {n}"),
    ("lowercase_name", b"content-length: {n}"),
]


def obfuscations():
    """(name, request seed)"""
    out = []
    smuggle = b"0\r\n\r\n" + SMUGGLED
    # A chunk carrying the smuggled request, for a back end that reads it as chunked
    gpost = b"POST /smuggled HTTP/1.1\r\nHost: localhost\r\nContent-Length: 15\r\n\r\nx=1"
    te_cl_body = b"%x\r\n" % len(gpost) + gpost + b"\r\n0\r\n\r\n"
    for name, te in TE_OBFUSCATIONS:
        # CL.TE: Content-Length covers the whole body, the chunked body ends early
        out.append((f"clte_{name}", request((b"Content-Length: %d" % len(smuggle), te), smuggle)))
        # TE.CL: Content-Length covers only the first chunk size line
        out.append((f"tecl_{name}", request((te, b"Content-Length: 4"), te_cl_body)))
    for name, te in TE_OBFUSCATIONS[:12]:
        # TE.TE: a plain chunked header and an obfuscated one
        out.append((f"tete_{name}", request((b"Transfer-Encoding: chunked", te), smuggle)))
    for name, line in CL_OBFUSCATIONS:
        header = line.replace(b"{n}", b"%d" % len(SMUGGLED))
        out.append((f"cl_{name}", request((header,), SMUGGLED + NEXT)))
        out.append((f"clte_cl_{name}", request((header, b"Transfer-Encoding: chunked"), smuggle)))
    return out


def main():
    corpus = pathlib.Path(sys.argv[1])
    outputs = {
        "quirks": corpus / "http_request_parser" / "quirks",
        "obfuscation": corpus / "http_request_parser" / "obfuscation",
        "chunked": corpus / "chunked_decoder" / "quirks",
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

    for name, data, body in quirks():
        write(outputs["quirks"], name, data)
        if body is not None:
            write(outputs["chunked"], name, body)
    for name, data in obfuscations():
        write(outputs["obfuscation"], name, data)
    for out, count in written.items():
        print(f"{out}: {count} seeds")


if __name__ == "__main__":
    main()
