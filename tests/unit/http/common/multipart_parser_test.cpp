#include <catch2/catch_test_macros.hpp>
#include <thinger/http/common/multipart_parser.hpp>
#include <algorithm>
#include <string>
#include <vector>

using namespace thinger::http;

namespace {

    struct parsed_part {
        multipart_part part;
        std::string content;
        bool ended = false;
    };

    struct parse_result {
        std::vector<parsed_part> parts;
        bool done = false;
        multipart_error error = multipart_error::none;

        bool operator==(const parse_result& other) const {
            if (done != other.done || error != other.error || parts.size() != other.parts.size()) return false;
            for (size_t i = 0; i < parts.size(); ++i) {
                const auto& a = parts[i];
                const auto& b = other.parts[i];
                // On error, the content of the part being read depends on how the body arrived:
                // what the pieces before the failing one held
                bool partial = error != multipart_error::none && i + 1 == parts.size() && !a.ended;
                if ((!partial && a.content != b.content) || a.ended != b.ended || a.part.headers != b.part.headers ||
                    a.part.name != b.part.name || a.part.filename != b.part.filename ||
                    a.part.has_filename != b.part.has_filename || a.part.content_type != b.part.content_type ||
                    a.part.disposition != b.part.disposition) {
                    return false;
                }
            }
            return true;
        }
    };

    std::string describe(const parse_result& result) {
        std::string text = "done=" + std::to_string(result.done) + " error=" + std::to_string(static_cast<int>(result.error));
        for (const auto& part : result.parts) {
            text += "\n  part name=" + part.part.name + " filename=" + part.part.filename +
                    " content=" + std::to_string(part.content.size()) + " bytes, ended=" + std::to_string(part.ended);
        }
        return text;
    }

    // Parse `body` fed in the given pieces (the rest in one piece), then finish it
    parse_result parse(const std::string& body, const std::string& boundary, const std::vector<size_t>& pieces,
                       multipart_limits limits = {}) {
        multipart_parser parser(boundary, limits);
        parse_result result;
        size_t offset = 0;
        auto handle = [&](multipart_parser::event event) {
            switch (event) {
                case multipart_parser::event::part:
                    result.parts.push_back({parser.part(), {}, false});
                    break;
                case multipart_parser::event::data:
                    REQUIRE(!result.parts.empty());
                    REQUIRE(!parser.data().empty());
                    result.parts.back().content.append(parser.data());
                    break;
                case multipart_parser::event::part_end:
                    REQUIRE(!result.parts.empty());
                    result.parts.back().ended = true;
                    break;
                case multipart_parser::event::end:
                    result.done = true;
                    break;
                case multipart_parser::event::error:
                    result.error = parser.error();
                    break;
                case multipart_parser::event::need_more:
                    break;
            }
        };
        auto feed = [&](std::string_view input) {
            // The parser never consumes nothing without an event, so this ends
            while (!result.done && result.error == multipart_error::none) {
                auto event = parser.parse(input);
                handle(event);
                if (event == multipart_parser::event::need_more) {
                    REQUIRE(input.empty());
                    break;
                }
            }
        };
        for (size_t piece : pieces) {
            if (offset >= body.size()) break;
            size_t size = std::min(piece, body.size() - offset);
            feed(std::string_view(body).substr(offset, size));
            offset += size;
        }
        if (offset < body.size()) feed(std::string_view(body).substr(offset));
        if (!result.done && result.error == multipart_error::none) handle(parser.finish());
        // Errors and the end are final
        if (result.error != multipart_error::none) {
            std::string_view more = "more";
            REQUIRE(parser.parse(more) == multipart_parser::event::error);
            REQUIRE(parser.finish() == multipart_parser::event::error);
        }
        return result;
    }

    parse_result parse(const std::string& body, const std::string& boundary = "boundary", multipart_limits limits = {}) {
        return parse(body, boundary, {body.size()}, limits);
    }

    // The result of parsing `body` whole, checked to be the same however it is split: in two
    // at every position, in three around every position, and byte by byte
    parse_result parse_every_split(const std::string& body, const std::string& boundary = "boundary",
                                   multipart_limits limits = {}) {
        auto whole = parse(body, boundary, limits);
        auto check = [&](const std::vector<size_t>& pieces) {
            auto split = parse(body, boundary, pieces, limits);
            if (!(split == whole)) {
                std::string sizes;
                for (size_t piece : pieces) sizes += std::to_string(piece) + " ";
                INFO("pieces: " << sizes << "\nwhole: " << describe(whole) << "\nsplit: " << describe(split));
                REQUIRE(split == whole);
            }
        };
        for (size_t position = 0; position <= body.size(); ++position) {
            check({position});
            if (position >= 1 && position + 1 <= body.size()) check({position - 1, 2});
        }
        check(std::vector<size_t>(body.size(), 1));
        return whole;
    }

    std::string part(const std::string& headers, const std::string& content, const std::string& boundary = "boundary") {
        return "--" + boundary + "\r\n" + headers + "\r\n" + content + "\r\n";
    }

    std::string field(const std::string& name, const std::string& value, const std::string& boundary = "boundary") {
        return part("Content-Disposition: form-data; name=\"" + name + "\"\r\n", value, boundary);
    }

    std::string close(const std::string& boundary = "boundary") {
        return "--" + boundary + "--\r\n";
    }

}

TEST_CASE("Multipart boundaries", "[multipart][unit]") {
    REQUIRE(multipart_parser::valid_boundary("boundary"));
    REQUIRE(multipart_parser::valid_boundary("----WebKitFormBoundary7MA4YWxkTrZu0gW"));
    REQUIRE(multipart_parser::valid_boundary("a"));
    REQUIRE(multipart_parser::valid_boundary(std::string(70, 'x')));
    REQUIRE(multipart_parser::valid_boundary("'()+_,-./:=? x"));
    REQUIRE(multipart_parser::valid_boundary("with space"));

    REQUIRE_FALSE(multipart_parser::valid_boundary(""));
    REQUIRE_FALSE(multipart_parser::valid_boundary(std::string(71, 'x')));
    REQUIRE_FALSE(multipart_parser::valid_boundary("ends with space "));
    for (const char* invalid : {"a\"b", "a;b", "a\rb", "a\nb", "a\tb", "a@b", "a\\b", "caf\xc3\xa9", "a*b", "a%b"}) {
        INFO(invalid);
        REQUIRE_FALSE(multipart_parser::valid_boundary(invalid));
    }

    multipart_parser parser("bad;boundary");
    REQUIRE(parser.failed());
    REQUIRE(parser.error() == multipart_error::invalid_boundary);
    std::string_view input = "--bad;boundary--\r\n";
    REQUIRE(parser.parse(input) == multipart_parser::event::error);
}

TEST_CASE("Multipart form with fields and files", "[multipart][unit]") {
    // As curl -F sends it
    const std::string body =
        "--------------------------d74496d66958873e\r\n"
        "Content-Disposition: form-data; name=\"name\"\r\n"
        "\r\n"
        "sensor-1\r\n"
        "--------------------------d74496d66958873e\r\n"
        "Content-Disposition: form-data; name=\"config\"; filename=\"config.json\"\r\n"
        "Content-Type: application/json\r\n"
        "\r\n"
        "{\"interval\": 10}\n\r\n"
        "--------------------------d74496d66958873e\r\n"
        "Content-Disposition: form-data; name=\"blob\"; filename=\"data.bin\"\r\n"
        "Content-Type: application/octet-stream\r\n"
        "\r\n" + std::string("\r\n\r\0\xff\r\r\n-", 10) + "\r\n"
        "--------------------------d74496d66958873e--\r\n";

    auto result = parse_every_split(body, "------------------------d74496d66958873e");
    REQUIRE(result.done);
    REQUIRE(result.error == multipart_error::none);
    REQUIRE(result.parts.size() == 3);

    REQUIRE(result.parts[0].part.disposition == "form-data");
    REQUIRE(result.parts[0].part.name == "name");
    REQUIRE_FALSE(result.parts[0].part.is_file());
    REQUIRE(result.parts[0].part.content_type == "text/plain");
    REQUIRE(result.parts[0].content == "sensor-1");
    REQUIRE(result.parts[0].ended);

    REQUIRE(result.parts[1].part.name == "config");
    REQUIRE(result.parts[1].part.is_file());
    REQUIRE(result.parts[1].part.filename == "config.json");
    REQUIRE(result.parts[1].part.content_type == "application/json");
    REQUIRE(result.parts[1].part.header("content-type") == "application/json");
    REQUIRE(result.parts[1].part.headers.size() == 2);
    REQUIRE(result.parts[1].content == "{\"interval\": 10}\n");

    REQUIRE(result.parts[2].content == std::string("\r\n\r\0\xff\r\r\n-", 10));
}

TEST_CASE("Multipart preamble and epilogue", "[multipart][unit]") {
    SECTION("Ignored") {
        auto body = "This is the preamble.\r\nIt is to be ignored --boundary\r\n--boundaryx\r\n" + field("a", "1") +
                    "--boundary--\r\nThis is the epilogue, also ignored\r\n--boundary\r\n";
        auto result = parse_every_split(body);
        REQUIRE(result.done);
        REQUIRE(result.parts.size() == 1);
        REQUIRE(result.parts[0].content == "1");
    }

    SECTION("A preamble that is only a CRLF") {
        auto result = parse_every_split("\r\n" + field("a", "1") + close());
        REQUIRE(result.done);
        REQUIRE(result.parts.size() == 1);
    }

    SECTION("The closing delimiter may end the body, with or without padding") {
        for (const char* end : {"--boundary--", "--boundary--  \t", "--boundary--\r\n", "--boundary-- \r\nepilogue"}) {
            INFO(end);
            auto result = parse_every_split(field("a", "1") + end);
            REQUIRE(result.done);
            REQUIRE(result.parts.size() == 1);
        }
    }

    SECTION("No parts") {
        auto result = parse_every_split("--boundary--\r\n");
        REQUIRE(result.done);
        REQUIRE(result.parts.empty());
        result = parse_every_split("preamble\r\n--boundary--");
        REQUIRE(result.done);
        REQUIRE(result.parts.empty());
    }

    SECTION("Transport padding after a delimiter") {
        auto result = parse_every_split("--boundary \t \r\nContent-Disposition: form-data; name=\"a\"\r\n\r\n1\r\n--boundary\t\r\n\r\n2\r\n--boundary--\r\n");
        REQUIRE(result.done);
        REQUIRE(result.parts.size() == 2);
        REQUIRE(result.parts[0].content == "1");
        REQUIRE(result.parts[1].content == "2");
    }
}

TEST_CASE("Multipart boundary-like data inside the content", "[multipart][unit]") {
    // None of these is a delimiter: not at the start of a line, or followed by something
    // else than "--", whitespace or CRLF
    for (const std::string content : {
             "x--boundary--", "\r\n--boundar", "\r\n--boundaryx", "\r\n--boundary-x",
             "\r\n--boundary-", "\n--boundary\r\n", "\r--boundary\r\n", "\r\n-boundary\r\n", "\r\n--Boundary\r\n",
             "\r\n\r\n--boundar\r\n--boundaryy\r\n", "\r\r\n\r\n-\r\n--\r\n--b\r\n--bo", "\r\n--boundary-\r\n"}) {
        INFO(content);
        auto result = parse_every_split(field("a", content) + field("b", "after") + close());
        REQUIRE(result.done);
        REQUIRE(result.parts.size() == 2);
        REQUIRE(result.parts[0].content == content);
        REQUIRE(result.parts[1].content == "after");
    }

    SECTION("A boundary that is a prefix of another") {
        auto body = field("a", "\r\n--ab\r\n", "abc") + close("abc");
        auto result = parse_every_split(body, "abc");
        REQUIRE(result.done);
        REQUIRE(result.parts.size() == 1);
        REQUIRE(result.parts[0].content == "\r\n--ab\r\n");
        // With the shorter boundary, "--abc" is not a delimiter of "ab" either ("c" follows it)
        result = parse_every_split(field("a", "x", "ab") + "--abc\r\n" + close("ab"), "ab");
        REQUIRE(result.done);
        REQUIRE(result.parts[0].content == "x\r\n--abc");
    }

    SECTION("Boundaries ending in dashes, and with spaces") {
        auto result = parse_every_split(field("a", "1", "b-") + close("b-"), "b-");
        REQUIRE(result.done);
        REQUIRE(result.parts.size() == 1);
        REQUIRE(result.parts[0].content == "1");
        result = parse_every_split(field("a", "1", "a b") + close("a b"), "a b");
        REQUIRE(result.done);
        REQUIRE(result.parts[0].content == "1");
    }
}

TEST_CASE("Multipart empty parts", "[multipart][unit]") {
    SECTION("Empty content") {
        auto result = parse_every_split(field("a", "") + field("b", "") + close());
        REQUIRE(result.done);
        REQUIRE(result.parts.size() == 2);
        REQUIRE(result.parts[0].content.empty());
        REQUIRE(result.parts[0].ended);
        REQUIRE(result.parts[1].content.empty());
    }

    SECTION("No headers") {
        auto result = parse_every_split("--boundary\r\n\r\ncontent\r\n--boundary--\r\n");
        REQUIRE(result.done);
        REQUIRE(result.parts.size() == 1);
        REQUIRE(result.parts[0].part.headers.empty());
        REQUIRE(result.parts[0].part.disposition.empty());
        REQUIRE(result.parts[0].part.name.empty());
        REQUIRE(result.parts[0].content == "content");
    }

    SECTION("The delimiter right after the header section, as Go reads it") {
        auto result = parse_every_split("--boundary\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\n--boundary\r\n\r\n--boundary--");
        REQUIRE(result.done);
        REQUIRE(result.parts.size() == 2);
        REQUIRE(result.parts[0].content.empty());
        REQUIRE(result.parts[1].content.empty());
    }

    SECTION("A file input without a file, as browsers send it") {
        auto result = parse_every_split(part("Content-Disposition: form-data; name=\"upload\"; filename=\"\"\r\n"
                                             "Content-Type: application/octet-stream\r\n", "") + close());
        REQUIRE(result.done);
        REQUIRE(result.parts[0].part.is_file());
        REQUIRE(result.parts[0].part.filename.empty());
        REQUIRE(result.parts[0].content.empty());
    }
}

TEST_CASE("Multipart part headers", "[multipart][unit]") {
    auto parse_headers = [](const std::string& headers) {
        return parse_every_split(part(headers, "content") + close());
    };

    SECTION("Case-insensitive names and parameters") {
        auto result = parse_headers("content-disposition: Form-Data; NAME=\"a\"; FileName=\"b.txt\"\r\nCONTENT-TYPE: text/csv\r\n");
        REQUIRE(result.done);
        REQUIRE(result.parts[0].part.disposition == "form-data");
        REQUIRE(result.parts[0].part.name == "a");
        REQUIRE(result.parts[0].part.filename == "b.txt");
        REQUIRE(result.parts[0].part.content_type == "text/csv");
    }

    SECTION("Whitespace around values, ';' and '=', and a trailing ';'") {
        auto result = parse_headers("Content-Disposition:form-data ;name = a ;  filename=\"x y.txt\" ;\r\nX-Other:  \t value \t \r\n");
        REQUIRE(result.done);
        REQUIRE(result.parts[0].part.name == "a");
        REQUIRE(result.parts[0].part.filename == "x y.txt");
        REQUIRE(result.parts[0].part.header("x-other") == "value");
    }

    SECTION("Quoted strings: escaped quotes, and backslashes of Windows paths kept") {
        auto result = parse_headers("Content-Disposition: form-data; name=\"a\\\"b\"; filename=\"C:\\Users\\me\\a\\\\b.txt\"\r\n");
        REQUIRE(result.done);
        REQUIRE(result.parts[0].part.name == "a\"b");
        REQUIRE(result.parts[0].part.filename == "C:\\Users\\me\\a\\b.txt");
        REQUIRE(result.parts[0].part.safe_filename() == "b.txt");
    }

    SECTION("UTF-8 filenames as browsers send them") {
        auto result = parse_headers("Content-Disposition: form-data; name=\"f\"; filename=\"caf\xc3\xa9 \xe2\x82\xac.txt\"\r\n");
        REQUIRE(result.done);
        REQUIRE(result.parts[0].part.filename == "caf\xc3\xa9 \xe2\x82\xac.txt");
    }

    SECTION("filename* (RFC 5987) wins over filename") {
        auto result = parse_headers("Content-Disposition: form-data; name=\"f\"; filename=\"fallback.txt\"; filename*=UTF-8''na%C3%AFve%20file.txt\r\n");
        REQUIRE(result.parts[0].part.filename == "na\xc3\xafve file.txt");
        result = parse_headers("Content-Disposition: form-data; name=\"f\"; filename*=iso-8859-1'en'%E9t%E9.txt\r\n");
        REQUIRE(result.parts[0].part.is_file());
        REQUIRE(result.parts[0].part.filename == "\xc3\xa9t\xc3\xa9.txt");
        result = parse_headers("Content-Disposition: form-data; name=\"f\"; filename*=\"us-ascii''plain.txt\"\r\n");
        REQUIRE(result.parts[0].part.filename == "plain.txt");
    }

    SECTION("An unsupported or malformed filename* is ignored") {
        for (const char* value : {"koi8-r''%C1", "UTF-8''%C3", "UTF-8''%G1", "UTF-8''%4", "no-quotes", "UTF-8'%41", "us-ascii''%E9"}) {
            INFO(value);
            auto result = parse_headers(std::string("Content-Disposition: form-data; name=\"f\"; filename=\"fallback\"; filename*=") + value + "\r\n");
            REQUIRE(result.done);
            REQUIRE(result.parts[0].part.filename == "fallback");
            result = parse_headers(std::string("Content-Disposition: form-data; name=\"f\"; filename*=") + value + "\r\n");
            REQUIRE(result.done);
            REQUIRE_FALSE(result.parts[0].part.is_file());
        }
    }

    SECTION("Other disposition types, and parts without Content-Disposition") {
        auto result = parse_headers("Content-Disposition: attachment; filename=\"a.txt\"\r\n");
        REQUIRE(result.parts[0].part.disposition == "attachment");
        REQUIRE(result.parts[0].part.is_file());
        result = parse_headers("Content-Type: text/html\r\n");
        REQUIRE(result.done);
        REQUIRE(result.parts[0].part.disposition.empty());
        REQUIRE(result.parts[0].part.content_type == "text/html");
    }

    SECTION("Malformed header sections") {
        for (const std::string& headers : std::vector<std::string>{
                 "Content-Disposition: form-data;\r\n name=\"a\"\r\n",          // folding
                 "Content-Disposition: form-data\r\n\tname=\"a\"\r\n",
                 "Content-Disposition form-data\r\n",                           // no colon
                 ": value\r\n",                                                 // no name
                 "Content-Disposition : form-data\r\n",                         // space before the colon
                 "Bad Name: x\r\n",
                 "X: a\rb\r\n",                                                 // CR in a value
                 std::string("X: a\0b\r\n", 8),                                 // NUL
                 "X: a\x01" "b\r\n",
                 "X: a\x7f\r\n",
                 "Content-Disposition: form-data; name=\"a\r\n",                // unterminated quoted string
                 "Content-Disposition: form-data; name=\"a\"; name=\"b\"\r\n",  // ambiguous
                 "Content-Disposition: form-data; filename=\"a\"; FILENAME=\"b\"\r\n",
                 "Content-Disposition: form-data\r\nContent-Disposition: form-data\r\n",
                 "Content-Type: text/plain\r\ncontent-type: text/html\r\n",
                 "Content-Disposition: ; name=a\r\n",
                 "Content-Disposition: form-data; name\r\n",
                 "Content-Disposition: form-data; name=\r\n",
                 "Content-Disposition: form-data; name=a b\r\n",
                 "Content-Disposition: form-data name=a\r\n",
                 "Content-Disposition: form-data; =a\r\n",
                 "Content-Disposition: form-data; name=caf\xc3\xa9\r\n"}) {
            INFO(headers);
            auto result = parse_headers(headers);
            REQUIRE(result.error == multipart_error::malformed);
        }
    }
}

TEST_CASE("Multipart malformed bodies", "[multipart][unit]") {
    // LF line endings: no delimiter at all
    REQUIRE(parse_every_split("--boundary\nContent-Disposition: form-data; name=\"a\"\n\n1\n--boundary--\n").error ==
            multipart_error::truncated);
    for (const std::string& body : std::vector<std::string>{
             "--boundary\r\nContent-Disposition: form-data; name=\"a\"\n\r\n1\r\n--boundary--\r\n",
             "--boundary\r\nContent-Disposition: form-data; name=\"a\"\r\n\n1\r\n--boundary--\r\n",
             "--boundary x\r\n\r\n1\r\n--boundary--\r\n",                    // garbage on a delimiter line
             "--boundary\rx\r\n\r\n1\r\n--boundary--\r\n",
             "--boundary\r\n\r\n1\r\n--boundary--x\r\n",                      // after the closing delimiter
             "--boundary\r\n\r\n1\r\n--boundary-- x\r\n",
             "--boundary\r\n\r\n1\r\n--boundary--\rx",
             "--boundary\r\n\r\n1\r\n--boundary  x\r\n\r\n--boundary--"}) {
        INFO(body);
        auto result = parse_every_split(body);
        REQUIRE(result.error == multipart_error::malformed);
    }
}

TEST_CASE("Multipart truncated bodies", "[multipart][unit]") {
    const std::string body = "preamble\r\n" + field("a", "value\r\n--boundar") +
                             part("Content-Disposition: form-data; name=\"f\"; filename=\"f.bin\"\r\n", "data") +
                             "--boundary--";
    REQUIRE(parse(body).done);
    // Every shorter body (but the one that ends at the closing delimiter, which is the body)
    for (size_t size = 0; size < body.size(); ++size) {
        INFO(size);
        auto result = parse(body.substr(0, size));
        REQUIRE_FALSE(result.done);
        REQUIRE(result.error == multipart_error::truncated);
    }
    REQUIRE(parse(body + "\r").error == multipart_error::truncated);
    REQUIRE(parse("").error == multipart_error::truncated);
    REQUIRE(parse("no delimiter at all").error == multipart_error::truncated);
}

TEST_CASE("Multipart filenames", "[multipart][unit]") {
    struct sample {
        std::string filename;
        std::string safe;
    };
    for (const auto& [filename, safe] : std::vector<sample>{
             {"photo.jpg", "photo.jpg"},
             {"../../etc/passwd", "passwd"},
             {"..\\..\\windows\\system32\\drivers", "drivers"},
             {"C:\\Users\\me\\photo.jpg", "photo.jpg"},
             {"/absolute/path.txt", "path.txt"},
             {"dir/", ""},
             {"..", ""},
             {".", ""},
             {"a/..", ""},
             {"a\\.", ""},
             {".hidden", ".hidden"},
             {"...", "..."},
             {std::string("evil\0.txt", 9), "evil.txt"},
             {"line\r\nbreak.txt", "linebreak.txt"},
             {std::string(".\0.", 3), ""},
             {"tab\there\x7f.txt", "tabhere.txt"},
             {"caf\xc3\xa9.txt", "caf\xc3\xa9.txt"},
             {"", ""}}) {
        INFO(filename);
        REQUIRE(multipart_part::sanitize_filename(filename) == safe);
    }

    // As received: the raw filename, and its safe name
    auto result = parse_every_split(part("Content-Disposition: form-data; name=\"f\"; filename=\"../../../etc/cron.d/job\"\r\n", "x") + close());
    REQUIRE(result.parts[0].part.filename == "../../../etc/cron.d/job");
    REQUIRE(result.parts[0].part.safe_filename() == "job");
    result = parse_every_split(part("Content-Disposition: form-data; name=\"f\"; filename*=UTF-8''..%2F..%2Fx%00y\r\n", "x") + close());
    REQUIRE(result.parts[0].part.filename == std::string("../../x\0y", 9));
    REQUIRE(result.parts[0].part.safe_filename() == "xy");
}

TEST_CASE("Multipart limits", "[multipart][unit]") {
    auto file = [](const std::string& name, const std::string& content) {
        return part("Content-Disposition: form-data; name=\"" + name + "\"; filename=\"" + name + ".bin\"\r\n", content);
    };

    SECTION("Parts") {
        multipart_limits limits;
        limits.max_parts = 3;
        auto three = field("a", "1") + field("b", "2") + file("c", "3");
        REQUIRE(parse_every_split(three + close(), "boundary", limits).done);
        auto result = parse_every_split(three + field("d", "4") + close(), "boundary", limits);
        REQUIRE(result.error == multipart_error::too_many_parts);
        REQUIRE(result.parts.size() == 3);
    }

    SECTION("Many parts") {
        std::string body;
        for (int i = 0; i < 1000; ++i) body += field("f" + std::to_string(i), std::to_string(i));
        auto result = parse(body + close());
        REQUIRE(result.done);
        REQUIRE(result.parts.size() == 1000);
        REQUIRE(result.parts[999].content == "999");
        REQUIRE(parse(body + field("x", "") + close()).error == multipart_error::too_many_parts);
        // Byte by byte, the same
        REQUIRE(parse(body + close(), "boundary", std::vector<size_t>(body.size() + 20, 1)) == result);
    }

    SECTION("Files") {
        multipart_limits limits;
        limits.max_files = 1;
        REQUIRE(parse_every_split(field("a", "1") + file("b", "2") + field("c", "3") + close(), "boundary", limits).done);
        REQUIRE(parse_every_split(file("a", "1") + file("b", "2") + close(), "boundary", limits).error ==
                multipart_error::too_many_files);
    }

    SECTION("Header section") {
        auto headers = "Content-Disposition: form-data; name=\"a\"\r\n";
        multipart_limits limits;
        limits.max_header_size = std::string(headers).size() + 2;   // and the empty line
        REQUIRE(parse_every_split(field("a", "1") + close(), "boundary", limits).done);
        limits.max_header_size--;
        REQUIRE(parse_every_split(field("a", "1") + close(), "boundary", limits).error == multipart_error::header_too_large);
        // A long line with a malformed byte early: too large however it is split
        limits.max_header_size = 64;
        REQUIRE(parse_every_split(part("X\x01: " + std::string(100, 'x') + "\r\n", "1") + close(), "boundary", limits).error ==
                multipart_error::header_too_large);
        REQUIRE(parse_every_split(part("X\x01: x\r\n", "1") + close(), "boundary", limits).error == multipart_error::malformed);
    }

    SECTION("Filename") {
        multipart_limits limits;
        limits.max_filename_size = 5;
        REQUIRE(parse_every_split(file("a", "1") + close(), "boundary", limits).done);   // "a.bin"
        REQUIRE(parse_every_split(file("ab", "1") + close(), "boundary", limits).error == multipart_error::filename_too_large);
    }

    SECTION("Field and file sizes, exactly at the limit and one byte over") {
        multipart_limits limits;
        limits.max_field_size = 10;
        limits.max_file_size = 20;
        REQUIRE(parse_every_split(field("a", std::string(10, 'x')) + file("f", std::string(20, 'y')) + close(), "boundary", limits).done);
        REQUIRE(parse_every_split(field("a", std::string(11, 'x')) + close(), "boundary", limits).error ==
                multipart_error::field_too_large);
        REQUIRE(parse_every_split(file("f", std::string(21, 'y')) + close(), "boundary", limits).error ==
                multipart_error::file_too_large);
        // Content that looked like a delimiter counts too
        REQUIRE(parse_every_split(field("a", std::string(8, 'x') + "\r\n--boundaryx") + close(), "boundary", limits).error ==
                multipart_error::field_too_large);
        // Files are not fields
        REQUIRE(parse_every_split(file("f", std::string(15, 'y')) + close(), "boundary", limits).done);
    }

    SECTION("Fields in total") {
        multipart_limits limits;
        limits.max_fields_size = 10;
        limits.max_field_size = 8;
        REQUIRE(parse_every_split(field("a", "12345") + file("f", std::string(100, 'y')) + field("b", "12345") + close(),
                                  "boundary", limits).done);
        REQUIRE(parse_every_split(field("a", "12345") + field("b", "123456") + close(), "boundary", limits).error ==
                multipart_error::fields_too_large);
        // Both crossed by the same piece: the one crossed first, whatever the split
        REQUIRE(parse_every_split(field("a", "12345") + field("b", "123456789") + close(), "boundary", limits).error ==
                multipart_error::fields_too_large);
        REQUIRE(parse_every_split(field("a", "1") + field("b", "123456789") + close(), "boundary", limits).error ==
                multipart_error::field_too_large);
        // At the same byte: the one of the part
        REQUIRE(parse_every_split(field("a", "12") + field("b", "123456789") + close(), "boundary", limits).error ==
                multipart_error::field_too_large);
    }
}

TEST_CASE("Multipart errors answered", "[multipart][unit]") {
    using status = http_response::status;
    REQUIRE(multipart_error_status(multipart_error::malformed) == status::bad_request);
    REQUIRE(multipart_error_status(multipart_error::truncated) == status::bad_request);
    REQUIRE(multipart_error_status(multipart_error::invalid_boundary) == status::bad_request);
    REQUIRE(multipart_error_status(multipart_error::not_multipart) == status::unsupported_media_type);
    REQUIRE(multipart_error_status(multipart_error::file_too_large) == status::payload_too_large);
    REQUIRE(multipart_error_status(multipart_error::too_many_parts) == status::payload_too_large);
    REQUIRE(multipart_error_status(multipart_error::body_too_large) == status::payload_too_large);
    REQUIRE(multipart_error_status(multipart_error::file_failed) == status::internal_server_error);
    REQUIRE(std::string(multipart_error_message(multipart_error::malformed)) == "Malformed multipart body");
}
