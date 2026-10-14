#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/body_decoders.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <string>
#include <vector>

using namespace thinger::http;
using json = nlohmann::json;

namespace {

    std::string bytes(const std::vector<uint8_t>& data) {
        return {data.begin(), data.end()};
    }

    std::string bytes(std::initializer_list<int> data) {
        std::string result;
        for (int byte : data) result += static_cast<char>(byte);
        return result;
    }

    media_type type_of(const std::string& value) {
        auto type = media_type::parse(value);
        REQUIRE(type.has_value());
        return *type;
    }

    // Decode with a built-in decoder by name, as a route does
    json decode(const std::string& name, const std::string& body, const std::string& content_type = "",
                body_limits limits = {}) {
        body_decoder_registry registry;
        const auto* format = registry.find(name);
        REQUIRE(format != nullptr);
        auto type = type_of(content_type.empty() ? format->media_types.front() : content_type);
        return format->decode(body, type, limits);
    }

    // Status of the body_decode_error thrown decoding a body (0 if decoded)
    int decode_error(const std::string& name, const std::string& body, const std::string& content_type = "",
                     body_limits limits = {}) {
        try {
            decode(name, body, content_type, limits);
        } catch (const body_decode_error& e) {
            return static_cast<int>(e.status());
        }
        return 0;
    }

    // Nested arrays: `depth` levels, the innermost one empty
    std::string nested_json(size_t depth) {
        return std::string(depth, '[') + std::string(depth, ']');
    }

    std::string nested_binary(size_t depth, char one_element, char empty) {
        return std::string(depth - 1, one_element) + std::string(1, empty);
    }

    std::string nested_ubjson(size_t depth) {
        return std::string(depth, '[') + std::string(depth, ']');
    }

    const json sample = {
        {"name", "sensor-1"},
        {"location", "lab"},
        {"tags", {"a", "b"}}
    };

}

TEST_CASE("Media types are parsed as RFC 9110 defines them", "[body][unit]") {
    SECTION("Type and subtype are lowercase, parameters unquoted") {
        auto type = media_type::parse("Application/JSON; Charset=\"UTF-8\"");
        REQUIRE(type);
        REQUIRE(type->type == "application/json");
        REQUIRE(type->parameter("charset") == "UTF-8");
        REQUIRE(type->parameter("CHARSET") == "UTF-8");
        REQUIRE(type->parameter("boundary").empty());
    }

    SECTION("Whitespace, empty parameters and quoted pairs") {
        auto type = media_type::parse(" text/plain ; charset=utf-8 ;; format=\"a\\\"b; c\" ;");
        REQUIRE(type);
        REQUIRE(type->type == "text/plain");
        REQUIRE(type->parameter("charset") == "utf-8");
        REQUIRE(type->parameter("format") == "a\"b; c");
    }

    SECTION("Lowercase types without parameters are read as they are") {
        for (const char* value : {"application/json", "application/vnd.api+json", "text/plain",
                                  "application/x-www-form-urlencoded", "x-custom/a.b_c~1"}) {
            INFO(value);
            auto type = media_type::parse(value);
            REQUIRE(type);
            REQUIRE(type->type == value);
            REQUIRE(type->parameters.empty());
        }
    }

    SECTION("Media ranges") {
        REQUIRE(media_type::parse("*/*")->type == "*/*");
        REQUIRE(media_type::parse("application/*;q=0.5")->type == "application/*");
    }

    SECTION("Malformed values") {
        for (const char* value : {"", "json", "application/", "/json", "application/json x", "application/json; charset",
                                  "application/json; charset=", "application/json; charset=\"utf-8",
                                  "application/json; =utf-8", "*/json", "appli cation/json", "application/js\"on",
                                  "application/json, text/plain", "/", "application//json", "application/json/",
                                  "a/b/c", "application/j\xc3\xa9son"}) {
            INFO(value);
            REQUIRE_FALSE(media_type::parse(value));
        }
    }
}

TEST_CASE("Body decoder registry", "[body][unit]") {
    body_decoder_registry registry;

    SECTION("Built-in decoders by name and media type") {
        for (const char* name : {"json", "msgpack", "cbor", "ubjson", "form", "text", "binary"}) {
            REQUIRE(registry.find(name) != nullptr);
        }
        REQUIRE(registry.find_media_type("application/json")->name == "json");
        REQUIRE(registry.find_media_type("application/msgpack")->name == "msgpack");
        REQUIRE(registry.find_media_type("application/x-msgpack")->name == "msgpack");
        REQUIRE(registry.find_media_type("application/cbor")->name == "cbor");
        REQUIRE(registry.find_media_type("application/ubjson")->name == "ubjson");
        REQUIRE(registry.find_media_type("application/x-www-form-urlencoded")->name == "form");
        REQUIRE(registry.find_media_type("text/plain")->name == "text");
        REQUIRE(registry.find_media_type("application/octet-stream")->name == "binary");
        REQUIRE(registry.find_media_type("image/png") == nullptr);
    }

    SECTION("Structured syntax suffixes") {
        REQUIRE(registry.find_media_type("application/problem+json")->name == "json");
        REQUIRE(registry.find_media_type("application/vnd.api+json")->name == "json");
        REQUIRE(registry.find_media_type("application/senml+cbor")->name == "cbor");
        REQUIRE(registry.find_media_type("application/x+msgpack") == nullptr);
        REQUIRE(registry.find_media_type("text+json/plain") == nullptr);
    }

    SECTION("Decoders are removed and added") {
        REQUIRE(registry.remove("ubjson"));
        REQUIRE_FALSE(registry.remove("ubjson"));
        REQUIRE(registry.find("ubjson") == nullptr);
        REQUIRE(registry.find_media_type("application/ubjson") == nullptr);

        registry.add("csv", {"Text/CSV"}, [](std::string_view body, const media_type&, const body_limits&) {
            return json(std::string(body));
        });
        REQUIRE(registry.find_media_type("text/csv")->name == "csv");

        // A media type belongs to the last decoder added for it
        registry.add("html", {"text/html", "text/plain"}, body_decoder_registry::decode_text);
        REQUIRE(registry.find_media_type("text/plain")->name == "html");
        REQUIRE(registry.find("text")->media_types.empty());

        // Same name: replaced
        registry.add("csv", {"application/csv"}, body_decoder_registry::decode_text);
        REQUIRE(registry.find_media_type("text/csv") == nullptr);
        REQUIRE(registry.find_media_type("application/csv")->name == "csv");
    }

    SECTION("Plain media types as decoders list them, parsed once") {
        auto [type, format] = registry.find_plain_media_type("application/json");
        REQUIRE(type != nullptr);
        REQUIRE(type->type == "application/json");
        REQUIRE(type->parameters.empty());
        REQUIRE(format->name == "json");
        REQUIRE(registry.find_plain_media_type("application/x-msgpack").second->name == "msgpack");

        // Anything else is parsed
        for (const char* value : {"", "Application/JSON", "application/json; charset=utf-8", " application/json",
                                  "application/problem+json", "image/png"}) {
            INFO(value);
            REQUIRE(registry.find_plain_media_type(value).first == nullptr);
        }

        // Kept up to date as decoders are added and removed, also in copies
        registry.add("csv", {"Text/CSV"}, body_decoder_registry::decode_text);
        registry.add("html", {"text/html", "text/plain"}, body_decoder_registry::decode_text);
        REQUIRE(registry.remove("json"));
        body_decoder_registry copy = registry;
        for (const auto* decoders : {&registry, &copy}) {
            REQUIRE(decoders->find_plain_media_type("application/json").first == nullptr);
            REQUIRE(decoders->find_plain_media_type("text/csv").second == decoders->find("csv"));
            REQUIRE(decoders->find_plain_media_type("text/plain").second == decoders->find("html"));
            REQUIRE(decoders->find_plain_media_type("application/cbor").second == decoders->find("cbor"));
        }
    }
}

TEST_CASE("The same object decodes the same from every format", "[body][unit]") {
    REQUIRE(decode("json", sample.dump()) == sample);
    REQUIRE(decode("msgpack", bytes(json::to_msgpack(sample))) == sample);
    REQUIRE(decode("msgpack", bytes(json::to_msgpack(sample)), "application/x-msgpack") == sample);
    REQUIRE(decode("cbor", bytes(json::to_cbor(sample))) == sample);
    REQUIRE(decode("ubjson", bytes(json::to_ubjson(sample))) == sample);
    REQUIRE(decode("ubjson", bytes(json::to_ubjson(sample, true, true))) == sample);   // optimized containers
    REQUIRE(decode("form", "name=sensor-1&location=lab&tags=a&tags=b") == sample);

    json numbers = {{"int", -5}, {"uint", 7}, {"float", 1.5}, {"bool", true}, {"null", nullptr}, {"list", {1, 2, 3}}};
    REQUIRE(decode("json", numbers.dump()) == numbers);
    REQUIRE(decode("msgpack", bytes(json::to_msgpack(numbers))) == numbers);
    REQUIRE(decode("cbor", bytes(json::to_cbor(numbers))) == numbers);
    REQUIRE(decode("ubjson", bytes(json::to_ubjson(numbers))) == numbers);
}

TEST_CASE("Text and binary bodies", "[body][unit]") {
    REQUIRE(decode("text", "hello\nworld") == json("hello\nworld"));
    REQUIRE(decode("text", "caf\xc3\xa9", "text/plain; charset=UTF-8") == json("caf\xc3\xa9"));
    REQUIRE(decode("text", "plain", "text/plain; charset=us-ascii") == json("plain"));
    REQUIRE(decode_error("text", "caf\xe9") == 400);
    REQUIRE(decode_error("text", "caf\xe9", "text/plain; charset=iso-8859-1") == 415);

    auto binary = decode("binary", bytes({0x00, 0xff, 0x10}));
    REQUIRE(binary.is_binary());
    const std::vector<uint8_t>& data = binary.get_binary();
    REQUIRE(data == std::vector<uint8_t>{0x00, 0xff, 0x10});
}

TEST_CASE("Forms decode to objects of strings", "[body][unit]") {
    REQUIRE(decode("form", "a=1&b=2") == json{{"a", "1"}, {"b", "2"}});
    // Repeated names: an array of the values, in order
    REQUIRE(decode("form", "a=1&b=x&a=2&a=3") == json{{"a", {"1", "2", "3"}}, {"b", "x"}});
    // Percent-encoding, '+' as a space (in names too)
    REQUIRE(decode("form", "q=hello+world&e=%C3%A9&my+key=%2B%26%3D") ==
            json{{"q", "hello world"}, {"e", "\xc3\xa9"}, {"my key", "+&="}});
    // Without '=': empty value; only the first '=' separates
    REQUIRE(decode("form", "flag&x=&eq=a=b") == json{{"flag", ""}, {"x", ""}, {"eq", "a=b"}});
    // Empty pairs and pairs without name are skipped
    REQUIRE(decode("form", "&&a=1&=v&") == json{{"a", "1"}});
    REQUIRE(decode("form", "&") == json::object());
    REQUIRE(decode("form", "a=1", "application/x-www-form-urlencoded; charset=utf-8") == json{{"a", "1"}});

    REQUIRE(decode_error("form", "a=%zz") == 400);
    REQUIRE(decode_error("form", "a=%4") == 400);
    REQUIRE(decode_error("form", "a=%ff") == 400);     // not UTF-8
    REQUIRE(decode_error("form", "%c3=1") == 400);
    REQUIRE(decode_error("form", "a=1", "application/x-www-form-urlencoded; charset=latin1") == 415);
}

TEST_CASE("Invalid bodies are rejected", "[body][unit]") {
    REQUIRE(decode_error("json", "{\"name\":") == 400);
    REQUIRE(decode_error("json", "{} trailing") == 400);
    REQUIRE(decode_error("json", "\"\xff\"") == 400);
    REQUIRE(decode_error("msgpack", bytes({0x82, 0xa1, 'a'})) == 400);      // truncated map
    REQUIRE(decode_error("msgpack", bytes({0xc1})) == 400);                 // never used byte
    REQUIRE(decode_error("msgpack", bytes({0x81, 0x01, 0x02})) == 400);     // non-string key
    REQUIRE(decode_error("msgpack", bytes({0xc0, 0xc0})) == 400);           // data after the value
    REQUIRE(decode_error("cbor", bytes({0xa1, 0x61})) == 400);
    REQUIRE(decode_error("cbor", bytes({0xc1, 0x00})) == 400);              // tagged item
    REQUIRE(decode_error("ubjson", "[i") == 400);

    SECTION("Strings must be UTF-8 in every format") {
        REQUIRE(decode_error("msgpack", bytes({0xa2, 0xc3, 0x28})) == 400);
        REQUIRE(decode_error("msgpack", bytes({0x81, 0xa1, 0xff, 0x01})) == 400);    // key
        REQUIRE(decode_error("cbor", bytes({0x62, 0xed, 0xa0})) == 400);
        REQUIRE(decode_error("ubjson", bytes({'S', 'i', 0x01, 0xff})) == 400);
        REQUIRE(decode_error("ubjson", bytes({'C', 0xe9})) == 400);
        try {
            decode("msgpack", bytes({0xa2, 0xc3, 0x28}));
        } catch (const body_decode_error& e) {
            REQUIRE(std::string(e.what()) == "Invalid MessagePack: string not valid UTF-8");
        }
    }

    SECTION("JSON takes no charset other than UTF-8") {
        REQUIRE(decode("json", "[1]", "application/json; charset=UTF-8") == json{1});
        REQUIRE(decode_error("json", "[1]", "application/json; charset=utf-16") == 415);
    }

    SECTION("Binary strings are kept as binary values") {
        auto value = decode("msgpack", bytes({0xc4, 0x02, 0x00, 0xff}));
        REQUIRE(value.is_binary());
        value = decode("cbor", bytes({0x42, 0x00, 0xff}));
        REQUIRE(value.is_binary());
    }
}

TEST_CASE("Nesting is limited in every format", "[body][unit]") {
    body_limits limits;
    REQUIRE(limits.max_depth == 128);

    SECTION("Up to the limit") {
        REQUIRE(decode("json", nested_json(128)).is_array());
        REQUIRE(decode("msgpack", nested_binary(128, '\x91', '\x90')).is_array());
        REQUIRE(decode("cbor", nested_binary(128, '\x81', '\x80')).is_array());
        REQUIRE(decode("ubjson", nested_ubjson(128)).is_array());
    }

    SECTION("Over the limit") {
        REQUIRE(decode_error("json", nested_json(129)) == 400);
        REQUIRE(decode_error("msgpack", nested_binary(129, '\x91', '\x90')) == 400);
        REQUIRE(decode_error("cbor", nested_binary(129, '\x81', '\x80')) == 400);
        REQUIRE(decode_error("ubjson", nested_ubjson(129)) == 400);
        REQUIRE(decode_error("json", R"({"a":)" + nested_json(128) + "}") == 400);
        try {
            decode("json", nested_json(129));
        } catch (const body_decode_error& e) {
            REQUIRE(std::string(e.what()) == "Invalid JSON: nested deeper than 128 levels");
        }
    }

    SECTION("Deep inputs fail fast, without recursion") {
        // A million levels (a recursive decoder would overflow the stack)
        REQUIRE(decode_error("json", nested_json(1000000)) == 400);
        REQUIRE(decode_error("msgpack", std::string(1000000, '\x91')) == 400);
        REQUIRE(decode_error("cbor", std::string(1000000, '\x81')) == 400);
        REQUIRE(decode_error("ubjson", std::string(1000000, '[')) == 400);
        REQUIRE(decode_error("cbor", std::string(1000000, '\x9f')) == 400);   // indefinite length
    }

    SECTION("Configurable") {
        REQUIRE(decode_error("json", nested_json(3), "", {.max_depth = 2}) == 400);
        REQUIRE(decode("json", nested_json(2), "", {.max_depth = 2}).is_array());
        REQUIRE(decode("json", "1", "", {.max_depth = 0}) == json(1));
        REQUIRE(decode_error("json", "[]", "", {.max_depth = 0}) == 400);
    }
}

TEST_CASE("Declared lengths do not allocate more than the input", "[body][unit]") {
    auto start = std::chrono::steady_clock::now();

    // Huge arrays, maps, strings and binaries declared in a few bytes
    REQUIRE(decode_error("msgpack", bytes({0xdd, 0xff, 0xff, 0xff, 0xff, 0xc0})) == 400);
    REQUIRE(decode_error("msgpack", bytes({0xdf, 0xff, 0xff, 0xff, 0xff})) == 400);
    REQUIRE(decode_error("msgpack", bytes({0xdb, 0xff, 0xff, 0xff, 0xff, 'a', 'b'})) == 400);
    REQUIRE(decode_error("msgpack", bytes({0xc6, 0xff, 0xff, 0xff, 0xff, 0x00})) == 400);
    REQUIRE(decode_error("msgpack", bytes({0xc9, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00})) == 400);
    REQUIRE(decode_error("cbor", bytes({0x9b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xf6})) == 400);
    REQUIRE(decode_error("cbor", bytes({0x5b, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00})) == 400);
    REQUIRE(decode_error("cbor", bytes({0x7b, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 'a'})) == 400);
    REQUIRE(decode_error("ubjson", bytes({'S', 'L', 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 'a'})) == 400);
    REQUIRE(decode_error("ubjson", bytes({'[', '#', 'l', 0x7f, 0xff, 0xff, 0xff, 'Z'})) == 400);

    // UBJSON arrays of values without payload: billions of values from a few bytes
    REQUIRE(decode_error("ubjson", bytes({'[', '$', 'Z', '#', 'l', 0x7f, 0xff, 0xff, 0xff})) == 400);
    REQUIRE(decode_error("ubjson", bytes({'[', '$', 'T', '#', 'L', 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff})) == 400);
    REQUIRE(decode_error("ubjson", bytes({'[', '$', 'F', '#', 'I', 0x7f, 0xff})) == 400);
    // ...even a few of them: a body never decodes to more values than it has bytes
    // (6 bytes: the array and 5 nulls fit, 6 nulls do not)
    REQUIRE(decode_error("ubjson", bytes({'[', '$', 'Z', '#', 'i', 0x06})) == 400);
    REQUIRE(decode("ubjson", bytes({'[', '$', 'Z', '#', 'i', 0x05})) == json::array({nullptr, nullptr, nullptr, nullptr, nullptr}));

    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
}

TEST_CASE("Body types are matched against the accepted ones", "[body][unit]") {
    body_decoder_registry registry;

    SECTION("JSON by default, also without Content-Type") {
        auto match = match_body_type(registry, {"json"}, "");
        REQUIRE(match.accepted);
        REQUIRE(match.type.type == "application/json");
        REQUIRE(match.format->name == "json");
        REQUIRE(match_body_type(registry, {"json"}, "Application/Json; charset=utf-8").accepted);
        REQUIRE(match_body_type(registry, {"json"}, "application/merge-patch+json").accepted);
    }

    SECTION("Other types are not accepted") {
        auto match = match_body_type(registry, {"json"}, "application/msgpack");
        REQUIRE_FALSE(match.accepted);
        REQUIRE(match.error == "Unsupported Media Type: application/msgpack");
        REQUIRE_FALSE(match_body_type(registry, {"json"}, "text/plain").accepted);
        REQUIRE_FALSE(match_body_type(registry, {"json"}, "application/x-www-form-urlencoded").accepted);
        REQUIRE_FALSE(match_body_type(registry, {}, "").accepted);
        REQUIRE(match_body_type(registry, {"json"}, "json").error == "Invalid Content-Type");
    }

    SECTION("By decoder name or media type") {
        REQUIRE(match_body_type(registry, {"msgpack"}, "application/x-msgpack").accepted);
        REQUIRE(match_body_type(registry, {"application/cbor"}, "application/cbor").accepted);
        REQUIRE_FALSE(match_body_type(registry, {"application/msgpack"}, "application/x-msgpack").accepted);
        auto png = match_body_type(registry, {"image/png"}, "image/png");
        REQUIRE(png.accepted);
        REQUIRE(png.format == nullptr);
    }

    SECTION("Media types of the accepted types") {
        REQUIRE(accepted_media_types(registry, {"json", "msgpack", "image/png", "unknown", "application/msgpack"}) ==
                std::vector<std::string>{"application/json", "application/msgpack", "application/x-msgpack", "image/png"});
    }
}

TEST_CASE("Response encoding follows the Accept header", "[body][unit]") {
    auto format = [](const char* accept) { return negotiate_response_encoding(accept).format; };
    REQUIRE(format("") == "json");
    REQUIRE(format("*/*") == "json");
    REQUIRE(format("text/html") == "json");
    REQUIRE(format("application/msgpack") == "msgpack");
    REQUIRE(format("application/cbor") == "cbor");
    REQUIRE(format("application/ubjson") == "ubjson");
    REQUIRE(format("application/cbor;q=0.5, application/msgpack;q=0.8") == "msgpack");
    REQUIRE(format("application/json;q=0, */*;q=0.1") == "msgpack");
    REQUIRE(format("application/*;q=0.2, application/cbor") == "cbor");
    REQUIRE(format("application/cbor;q=2, application/ubjson") == "ubjson");   // invalid quality: ignored
    REQUIRE(format("application/json;q=0") == "json");                         // none accepted: JSON
    REQUIRE(negotiate_response_encoding("application/x-msgpack").media_type == "application/x-msgpack");

    REQUIRE(encode_body(sample, "json") == sample.dump());
    REQUIRE(json::from_msgpack(encode_body(sample, "msgpack")) == sample);
    REQUIRE(json::from_cbor(encode_body(sample, "cbor")) == sample);
    REQUIRE(json::from_ubjson(encode_body(sample, "ubjson")) == sample);
}

TEST_CASE("Response compression follows the Accept-Encoding header", "[body][compression][unit]") {
    using enum content_coding;
    struct expectation {
        std::string_view accept_encoding;
        content_coding coding;
    };
    for (auto [accept_encoding, coding] : std::initializer_list<expectation>{
             {"", identity},
             {"gzip", gzip},
             {"deflate", deflate},
             {"gzip, deflate", gzip},
             {"deflate, gzip", gzip},                    // gzip at equal quality
             {"GZIP", gzip},                             // codings are case-insensitive
             {"x-gzip", gzip},
             {"br", identity},
             {"gzip;q=0", identity},                     // not acceptable
             {"gzip;q=0, deflate", deflate},
             {"gzip;q=0.5, deflate;q=0.8", deflate},
             {"gzip;q=0.8, deflate;q=0.5", gzip},
             {"gzip ; Q=0.5", gzip},                     // spaces, and Q in uppercase
             {"gzip;q=0.001", gzip},
             {"*", gzip},
             {"*;q=0", identity},
             {"*;q=0.5, deflate;q=0.8", deflate},
             {"*;q=0.8, deflate;q=0.5", gzip},           // gzip through "*"
             {"gzip;q=0, *", deflate},
             {"identity", identity},
             {"gzip;q=0.5, identity", identity},         // identity preferred
             {"gzip, identity;q=0.5", gzip},
             {"gzip, identity", gzip},
             {"gzip;q=0.3, *;q=0.5", deflate},           // deflate through "*"
             {"gzip;q=0.3, deflate;q=0.3, *;q=0.5", identity},   // identity through "*"
             {"gzip;q=1.5", identity},                   // invalid weights drop the coding
             {"gzip;q=abc, deflate", deflate},
             {"gzip;q=", identity},
             {" , ,gzip,", gzip},
             {"gzip;q=0, gzip", identity},               // the first one counts
             {"gzipx, deflatex", identity},              // no substring matches
         }) {
        INFO(accept_encoding);
        REQUIRE(negotiate_content_coding(accept_encoding) == coding);
    }
}

TEST_CASE("Accept-Encoding gives each coding a quality", "[body][compression][unit]") {
    struct expectation {
        std::string_view accept_encoding;
        int gzip, deflate, br, identity;
    };
    for (auto [accept_encoding, gzip, deflate, br, identity] : std::initializer_list<expectation>{
             {"", -1, -1, -1, -1},
             {"br", -1, -1, 1000, -1},
             {"BR;q=0.5, x-gzip", 1000, -1, 500, -1},
             {"gzip, deflate, br, zstd", 1000, 1000, 1000, -1},
             {"*;q=0.2, br;q=0", 200, 200, 0, 200},
             {"identity;q=0, gzip", 1000, -1, -1, 0},
             {"br;q=abc, br;q=0.7", -1, -1, 700, -1},    // an invalid weight drops the element
         }) {
        INFO(accept_encoding);
        auto accepted = parse_accept_encoding(accept_encoding);
        REQUIRE(accepted.gzip == gzip);
        REQUIRE(accepted.deflate == deflate);
        REQUIRE(accepted.br == br);
        REQUIRE(accepted.identity == identity);
    }
}
