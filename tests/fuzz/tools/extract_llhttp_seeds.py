#!/usr/bin/env python3
"""Extract the raw requests and responses of llhttp's test suite (test/request/*.md and
test/response/*.md, MIT) as fuzz seeds.

Each test in those files is an ```http block (with a <!-- meta={"type": "request..."} -->
or "response..." comment) followed by the expected ```log. The block is turned into bytes as llhttp's
test/md-test.ts does: trailing newline removed, escaped newlines joined, every newline
made CRLF, then \\r \\n \\t \\f \\xHH and octal escapes expanded.

Writes one seed per request into <out>/http_request_parser/llhttp/ and, for the chunked
ones, the bytes after the header section (followed by another request, as the other seeds)
into <out>/chunked_decoder/llhttp/; one seed per response into
<out>/http_response_parser/llhttp/. Seeds already present anywhere under <out> (same
bytes) and seeds over 4096 bytes (the -max_len used by CI) are skipped.

Usage: extract_llhttp_seeds.py <llhttp source dir> <tests/fuzz/corpus>
"""

import hashlib
import json
import pathlib
import re
import sys

MAX_SIZE = 4096
NEXT_REQUEST = b"GET /next HTTP/1.1\r\nHost: localhost\r\n\r\n"


def to_bytes(block: str) -> bytes:
    """The input bytes of an ```http block, as llhttp's test runner builds them: the text
    as UTF-8, and each escape as the single byte it stands for."""
    # One character per byte, so that every replacement below works on bytes
    s = block.encode("utf-8").decode("latin-1")
    s = re.sub(r"\n\Z", "", s)
    s = re.sub(r"\\(\r\n|\r|\n)", "", s)
    s = re.sub(r"\r\n|\r|\n", "\r\n", s)
    s = s.replace("\\r", "\r").replace("\\n", "\n").replace("\\t", "\t").replace("\\f", "\f")
    s = re.sub(r"\\x([0-9a-fA-F]{1,2})", lambda m: chr(int(m.group(1), 16)), s)
    s = re.sub(r"\\([0-7]{1,3})", lambda m: chr(int(m.group(1), 8) & 0xFF), s)
    return s.encode("latin-1")


def slug(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "_", text.lower()).strip("_")[:48] or "test"


def tests(md: pathlib.Path, kind: str = "request"):
    """(heading, meta, input bytes) of each `kind` test (request or response) of a llhttp .md file."""
    heading, meta = "", None
    lines = md.read_text(encoding="utf-8").split("\n")
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith("#"):
            heading = line.lstrip("#").strip()
        elif line.startswith("<!-- meta="):
            meta = json.loads(line[len("<!-- meta="):line.rindex("-->")])
        elif line.startswith("```http") and meta is not None:
            block = []
            i += 1
            while not lines[i].startswith("```"):
                block.append(lines[i])
                i += 1
            if meta.get("type", "").startswith(kind):
                yield heading, meta, to_bytes("\n".join(block) + "\n")
            meta = None
        i += 1


def chunked_body(request: bytes):
    """The bytes after the header section of a chunked request, or None."""
    end = request.find(b"\r\n\r\n")
    if end < 0:
        return None
    headers = request[:end].lower()
    if not re.search(rb"\r\ntransfer-encoding:[^\r\n]*chunked", headers):
        return None
    return request[end + 4:] + NEXT_REQUEST


def main():
    llhttp, corpus = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    out_request = corpus / "http_request_parser" / "llhttp"
    out_chunked = corpus / "chunked_decoder" / "llhttp"
    out_response = corpus / "http_response_parser" / "llhttp"
    outputs = (out_request, out_chunked, out_response)
    known = {hashlib.sha1(p.read_bytes()).digest() for p in corpus.rglob("*") if p.is_file() and p.parent not in outputs}
    for out in outputs:
        out.mkdir(parents=True, exist_ok=True)
        for old in out.iterdir():
            old.unlink()

    written = dict.fromkeys(outputs, 0)

    def write(out, name, seed):
        if seed is None or len(seed) > MAX_SIZE:
            return
        digest = hashlib.sha1(seed).digest()
        if digest in known:
            return
        known.add(digest)
        (out / name).write_bytes(seed)
        written[out] += 1

    for md in sorted((llhttp / "test" / "request").glob("*.md")):
        for n, (heading, meta, data) in enumerate(tests(md)):
            name = f"{md.stem}_{n:02d}_{slug(heading)}"
            write(out_request, name, data)
            write(out_chunked, name, chunked_body(data))
    for md in sorted((llhttp / "test" / "response").glob("*.md")):
        for n, (heading, meta, data) in enumerate(tests(md, "response")):
            write(out_response, f"{md.stem}_{n:02d}_{slug(heading)}", data)
    for out, count in written.items():
        print(f"{out}: {count} seeds")


if __name__ == "__main__":
    main()
