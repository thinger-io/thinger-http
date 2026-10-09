// Fuzz the chunked transfer coding decoder (chunked_decoder) with the input as the bytes
// following the request headers, fed in pieces derived from the input itself.
//
// Invariants:
//  - it never reads or writes out of bounds: every piece and output buffer is a heap
//    allocation of exactly its size, so AddressSanitizer catches any overrun
//  - it never consumes more input, nor produces more output, than it was given room for
//  - the decoded body, the outcome (done / failed / needs more) and the bytes consumed are
//    the same however the input is split and whatever the output buffer size, including
//    when chunk data is read straight from the input (data_pending / data_consumed), as
//    body_reader does once the read-ahead is exhausted
//  - an error is final, and so is the end of the body: no further input is consumed
//  - bytes after the end of the body are never consumed (they belong to the next request),
//    and the end is detected without looking at them

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include "fuzz_common.hpp"

using thinger::http::chunked_decoder;

namespace {

    struct decoded {
        std::string body;
        size_t consumed = 0;
        bool done = false;
        bool failed = false;

        bool operator==(const decoded&) const = default;
    };

    std::string describe(const decoded& d) {
        return "consumed=" + std::to_string(d.consumed) + " done=" + std::to_string(d.done) +
               " failed=" + std::to_string(d.failed) + " body=[" + fuzz::escape(d.body) + "]";
    }

    // Decode one piece of input, exactly as large as it is (heap copy, so any overread is
    // caught), into output buffers of `out_size` bytes. Returns the input bytes consumed.
    size_t decode_piece(chunked_decoder& decoder, const uint8_t* data, size_t size, size_t out_size, decoded& result) {
        std::vector<uint8_t> piece(data, data + size);
        size_t offset = 0;
        while (offset < size && !decoder.done() && !decoder.failed()) {
            auto out = std::make_unique<uint8_t[]>(out_size);
            size_t produced = 12345; // must be overwritten
            size_t consumed = decoder.decode(piece.data() + offset, size - offset, out.get(), out_size, produced);
            fuzz::check(consumed <= size - offset, "consumed more input than given");
            fuzz::check(produced <= out_size, "produced more output than room for");
            fuzz::check(produced <= consumed, "produced more output than input consumed");
            result.body.append(reinterpret_cast<const char*>(out.get()), produced);
            offset += consumed;
            // No progress is only allowed when the decoder stopped
            if (consumed == 0) {
                fuzz::check(decoder.done() || decoder.failed(), "no progress without stopping");
                break;
            }
        }
        return offset;
    }

    // Feed `data` in pieces from `gen` (or all at once if no generator), with output buffers
    // of `out_size` bytes. With `direct_data`, chunk data is read straight from the input
    // when the decoder is within a chunk, the way body_reader reads from the connection.
    decoded decode(const uint8_t* data, size_t size, fuzz::split_generator* gen, size_t out_size, bool direct_data) {
        chunked_decoder decoder;
        decoded result;
        size_t offset = 0;
        while (offset < size && !decoder.done() && !decoder.failed()) {
            size_t piece = gen ? gen->piece(size - offset) : size - offset;
            if (direct_data) {
                if (size_t pending = decoder.data_pending()) {
                    size_t bytes = std::min(pending, piece);
                    result.body.append(reinterpret_cast<const char*>(data + offset), bytes);
                    decoder.data_consumed(bytes);
                    fuzz::check(decoder.data_pending() == pending - bytes, "data_consumed() did not update data_pending()");
                    offset += bytes;
                    continue;
                }
            }
            size_t consumed = decode_piece(decoder, data + offset, piece, out_size, result);
            offset += consumed;
            if (consumed < piece) {
                fuzz::check(decoder.done() || decoder.failed(), "piece left unconsumed without stopping");
            }
        }
        result.consumed = offset;
        result.done = decoder.done();
        result.failed = decoder.failed();
        fuzz::check(!(result.done && result.failed), "done and failed at once");

        // Once stopped, the decoder stays stopped and consumes nothing else
        if (result.done || result.failed) {
            const uint8_t more[] = "5\r\nhello\r\n0\r\n\r\n";
            decoded extra;
            size_t consumed = decode_piece(decoder, more, sizeof(more) - 1, 64, extra);
            fuzz::check(consumed == 0 && extra.body.empty(), "decoder consumed input after stopping");
            fuzz::check(decoder.done() == result.done && decoder.failed() == result.failed, "decoder state changed after stopping");
            fuzz::check(decoder.data_pending() == 0, "data pending after stopping");
        }
        return result;
    }

}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Reference: all at once, with room for the whole body
    decoded reference = decode(data, size, nullptr, size + 1, false);
    std::string input = fuzz::escape({reinterpret_cast<const char*>(data), size});

    auto require_same = [&](const decoded& other, const char* how) {
        if (!(other == reference)) {
            fuzz::fail("chunked decoding depends on how the input is split",
                       std::string(how) + "\ninput: " + input + "\nreference: " + describe(reference) +
                       "\nother:     " + describe(other));
        }
    };

    // One byte at a time, one output byte at a time
    {
        chunked_decoder decoder;
        decoded result;
        size_t offset = 0;
        while (offset < size && !decoder.done() && !decoder.failed()) {
            offset += decode_piece(decoder, data + offset, 1, 1, result);
        }
        result.consumed = offset;
        result.done = decoder.done();
        result.failed = decoder.failed();
        require_same(result, "byte by byte");
    }

    // Pseudo-random pieces and output sizes, decoded or read straight while within chunk data
    for (uint64_t salt = 1; salt <= 4; salt++) {
        fuzz::split_generator gen(data, size, salt);
        size_t out_size = gen.piece(64);
        require_same(decode(data, size, &gen, out_size, salt % 2 == 0), "random pieces");
    }

    // The end of the body is found without looking past it: the bytes it consumed decode
    // the same way on their own, and anything after them is never touched
    if (reference.done) {
        decoded prefix = decode(data, reference.consumed, nullptr, size + 1, false);
        require_same(prefix, "only the bytes consumed");

        std::vector<uint8_t> extended(data, data + size);
        const char junk[] = "\r\n0\r\n\r\nGET / HTTP/1.1\r\n\r\n";
        extended.insert(extended.end(), junk, junk + sizeof(junk) - 1);
        decoded longer = decode(extended.data(), extended.size(), nullptr, extended.size(), false);
        require_same(longer, "with more bytes after the input");
    }

    // A failure is detected at the same point with more data after it: it never depends on
    // bytes not yet received
    if (reference.failed) {
        std::vector<uint8_t> extended(data, data + size);
        extended.push_back('\n');
        decoded longer = decode(extended.data(), extended.size(), nullptr, extended.size(), false);
        require_same(longer, "with more bytes after a failure");
    }

    return 0;
}
