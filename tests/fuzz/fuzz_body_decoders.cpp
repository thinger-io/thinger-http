// Body decoders (body_decoder_registry): every built-in decoder on the same input, which must
// either decode within the limits or fail with body_decode_error, never crash, recurse without
// bound or allocate more than the input justifies. Decoded values must be usable as routes use
// them: dumped (valid UTF-8), encoded again, and validated against a JSON Schema. The input is
// also parsed as a Content-Type and as an Accept header.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "thinger/http/server/body_decoders.hpp"
#include "fuzz_common.hpp"

#ifdef THINGER_HTTP_VALIJSON_ENABLED
#include <valijson/schema.hpp>
#include <valijson/schema_parser.hpp>
#include <valijson/validator.hpp>
#include <valijson/validation_results.hpp>
#include <valijson/adapters/nlohmann_json_adapter.hpp>
#endif

namespace {

    using json = nlohmann::json;
    using namespace thinger::http;

    struct measures {
        size_t depth = 0;               // nesting of arrays and objects
        size_t values = 0;
        bool binary_subtype = false;    // a binary value with a subtype (a MessagePack ext)
    };

    // Measures of a value, without recursion
    measures measure(const json& value) {
        measures result;
        std::vector<std::pair<const json*, size_t>> stack{{&value, 0}};
        while (!stack.empty()) {
            auto [current, level] = stack.back();
            stack.pop_back();
            ++result.values;
            if (current->is_binary() && current->get_binary().has_subtype()) result.binary_subtype = true;
            if (current->is_structured()) {
                ++level;
                result.depth = std::max(result.depth, level);
                for (const auto& child : *current) stack.emplace_back(&child, level);
            }
        }
        return result;
    }

#ifdef THINGER_HTTP_VALIJSON_ENABLED
    // A schema with the usual keywords, as routes have
    const valijson::Schema& schema() {
        static valijson::Schema* parsed = []() {
            static const json document = json::parse(R"({
                "type": ["object", "array", "string", "number", "integer", "boolean", "null"],
                "properties": {
                    "name": {"type": "string", "minLength": 1, "maxLength": 32},
                    "tags": {"type": "array", "items": {"type": "string"}, "maxItems": 8},
                    "value": {"type": "number", "minimum": -1000, "maximum": 1000},
                    "nested": {"type": "object", "properties": {"name": {"type": "string"}},
                               "additionalProperties": {"type": "array"}}
                },
                "items": {"anyOf": [{"type": "string"}, {"type": "number"}, {"type": "object", "required": ["a"]}]},
                "additionalProperties": {"not": {"type": "boolean"}},
                "required": ["name"]
            })");
            auto* result = new valijson::Schema();
            valijson::SchemaParser parser;
            valijson::adapters::NlohmannJsonAdapter adapter(document);
            parser.populateSchema(adapter, *result);
            return result;
        }();
        return *parsed;
    }

    void validate(const json& value) {
        valijson::Validator validator;
        valijson::adapters::NlohmannJsonAdapter adapter(value);
        valijson::ValidationResults results;
        validator.validate(schema(), adapter, &results);
    }
#endif

    void check_decoded(const body_format& format, std::string_view input, const json& value, const body_limits& limits) {
        auto measured = measure(value);
        fuzz::check(measured.depth <= limits.max_depth, "decoded value nested deeper than the limit", format.name);
        // A body never decodes to more values than it has bytes (a form, one more: "a" is {"a": ""})
        fuzz::check(measured.values <= input.size() + (format.name == "form" ? 1 : 0),
                    "decoded value with more values than bytes", format.name);
        if (format.name == "form") fuzz::check(value.is_object(), "form not decoded as an object");
        if (format.name == "text") fuzz::check(value.is_string(), "text not decoded as a string");
        if (format.name == "binary") fuzz::check(value.is_binary(), "binary not decoded as binary");

        // Usable as a route uses it: dumped (strings must be valid UTF-8), encoded again...
        std::string dumped;
        std::vector<uint8_t> msgpack;
        std::vector<uint8_t> cbor;
        try {
            dumped = value.dump();
            msgpack = json::to_msgpack(value);
            cbor = json::to_cbor(value);
        } catch (const std::exception& e) {
            fuzz::fail("decoded value cannot be encoded", format.name + ": " + e.what());
        }

        // ...and decoded again the same (binary values dump as {"bytes": [...], "subtype": ...})
        body_limits deeper{limits.max_depth + 2};
        auto json_type = *media_type::parse("application/json");
        try {
            auto again = body_decoder_registry::decode_json(dumped, json_type, deeper);
            fuzz::check(again.dump() == dumped, "JSON dump decoded differently", format.name);
            again = body_decoder_registry::decode_msgpack(
                std::string_view(reinterpret_cast<const char*>(msgpack.data()), msgpack.size()), json_type, limits);
            fuzz::check(again.dump() == dumped, "MessagePack encoding decoded differently", format.name);
            // (CBOR writes a binary subtype as a tag, and tagged items are rejected)
            if (!measured.binary_subtype) {
                again = body_decoder_registry::decode_cbor(
                    std::string_view(reinterpret_cast<const char*>(cbor.data()), cbor.size()), json_type, limits);
                fuzz::check(again.dump() == dumped, "CBOR encoding decoded differently", format.name);
            }
        } catch (const body_decode_error& e) {
            fuzz::fail("encoded value not decoded again", format.name + ": " + e.what() + "\n" + fuzz::escape(dumped));
        }

#ifdef THINGER_HTTP_VALIJSON_ENABLED
        validate(value);
#endif
    }

}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static const body_decoder_registry registry;
    std::string_view input(reinterpret_cast<const char*>(data), size);
    body_limits limits;

    // Routes never decode empty bodies (they are null)
    for (const auto& format : registry.formats()) {
        if (input.empty()) break;
        auto type = *media_type::parse(format.media_types.front());
        json value;
        try {
            value = format.decode(input, type, limits);
        } catch (const body_decode_error& e) {
            fuzz::check(e.status() == http_response::status::bad_request, "invalid body not answered 400", format.name);
            continue;
        } catch (const std::exception& e) {
            fuzz::fail("decoder threw something else than body_decode_error", format.name + ": " + e.what());
        }
        check_decoded(format, input, value, limits);
    }

    // A small depth limit takes other paths through the decoders
    body_limits shallow{3};
    for (const char* name : {"json", "msgpack", "cbor", "ubjson"}) {
        const auto* format = registry.find(name);
        try {
            auto value = format->decode(input, *media_type::parse(format->media_types.front()), shallow);
            fuzz::check(measure(value).depth <= 3, "decoded value nested deeper than a small limit", name);
        } catch (const body_decode_error&) {
        }
    }

    // The input as a Content-Type, and as an Accept header
    if (auto type = media_type::parse(input)) {
        fuzz::check(type->type.find('/') != std::string::npos, "media type without '/'");
        for (char c : type->type) fuzz::check(c < 'A' || c > 'Z', "media type not lowercase");
        auto match = match_body_type(registry, {"json", "msgpack", "form", "image/png"}, input);
        fuzz::check(match.accepted == (match.type.type == "image/png" ||
                                       (match.format && (match.format->name == "json" || match.format->name == "msgpack" ||
                                                         match.format->name == "form"))),
                    "media type accepted inconsistently", type->type);
    }
    auto encoding = negotiate_response_encoding(input);
    fuzz::check(encoding.format == "json" || encoding.format == "msgpack" || encoding.format == "cbor" ||
                encoding.format == "ubjson", "unknown response encoding");
    return 0;
}
