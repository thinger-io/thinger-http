#include "response_compression.hpp"
#include "../common/http_request.hpp"
#include "../common/http_response.hpp"
#include "../common/syntax.hpp"
#include "../../util/ascii.hpp"
#include "../../util/compression.hpp"

namespace thinger::http {

namespace {

    using ::thinger::util::ascii::iequals;

    // Weight of the parameters of an Accept-Encoding element (";q=0.5", the only parameter
    // defined): 1000 without one, -1 if invalid
    int coding_weight(std::string_view parameters) {
        int quality = 1000;
        while (!parameters.empty() && quality >= 0) {
            auto next = parameters.find(';');
            auto parameter = parameters.substr(0, next);
            parameters = next == std::string_view::npos ? std::string_view{} : parameters.substr(next + 1);
            auto equals = parameter.find('=');
            if (equals != std::string_view::npos && iequals(syntax::trim(parameter.substr(0, equals)), "q")) {
                quality = syntax::parse_qvalue(syntax::trim(parameter.substr(equals + 1)));
            }
        }
        return quality;
    }

    // Quality of a content coding in `codings` (`any` for "*"), null if it is not one of them
    int* coding_quality(accepted_codings& codings, int& any, std::string_view coding) {
        if (iequals(coding, "gzip") || iequals(coding, "x-gzip")) return &codings.gzip;
        if (iequals(coding, "deflate")) return &codings.deflate;
        if (iequals(coding, "br")) return &codings.br;
        if (iequals(coding, "identity")) return &codings.identity;
        if (coding == "*") return &any;
        return nullptr;
    }

    content_coding negotiate(const accepted_codings& accepted) {
        int best = std::max(accepted.gzip, accepted.deflate);
        if (best <= 0 || accepted.identity > best) return content_coding::identity;
        return accepted.gzip >= accepted.deflate ? content_coding::gzip : content_coding::deflate;
    }

    bool is_compressible_content_type(std::string_view content_type) {
        return content_type.starts_with("text/")
            || content_type.starts_with("application/json")
            || content_type.starts_with("application/xml")
            || content_type.starts_with("application/javascript")
            || content_type.starts_with("application/x-javascript")
            || content_type.starts_with("image/svg+xml");
    }

    // Whether a comma-separated header (any of its values) holds `token`, case-insensitive
    bool has_list_token(const http_response& message, std::string_view name, std::string_view token) {
        for (const auto& value : message.get_headers_with_key(name)) {
            if (syntax::any_element(value, [&](std::string_view item) { return iequals(item, token); })) return true;
        }
        return false;
    }

    // Cache-Control: no-transform (RFC 9111, 5.2.2.6): no compression then, as Express
    bool no_transform(const http_response& message) {
        return message.has_header(header::cache_control) && has_list_token(message, header::cache_control, "no-transform");
    }

}

accepted_codings parse_accept_encoding(std::string_view accept_encoding) {
    accepted_codings listed{-1, -1, -1, -1};
    int any = -1;
    syntax::for_each_element(accept_encoding, [&](std::string_view element) {
        auto semicolon = element.find(';');
        auto coding = syntax::trim(element.substr(0, semicolon));
        if (coding.empty()) return;
        int quality = semicolon == std::string_view::npos ? 1000 : coding_weight(element.substr(semicolon + 1));
        if (quality < 0) return;
        // the first time a coding is listed counts
        int* entry = coding_quality(listed, any, coding);
        if (entry && *entry < 0) *entry = quality;
    });

    // "*" stands for the codings not listed; identity not listed nor excluded is still
    // acceptable, as the last choice
    for (int* quality : {&listed.gzip, &listed.deflate, &listed.br, &listed.identity}) {
        if (*quality < 0) *quality = any;
    }
    return listed;
}

content_coding negotiate_content_coding(std::string_view accept_encoding) {
    return negotiate(parse_accept_encoding(accept_encoding));
}

namespace compression {

    accepted_codings accepted_by(const http_request& request) {
        std::string accept_encoding;
        for (const auto& value : request.get_headers_with_key("Accept-Encoding")) {
            if (!accept_encoding.empty()) accept_encoding += ", ";
            accept_encoding += value;
        }
        return parse_accept_encoding(accept_encoding);
    }

    bool compressible(const http_response& message, size_t size, std::string_view content_type) {
        return size >= threshold && is_compressible_content_type(content_type) && !no_transform(message);
    }

    bool compressible(const http_response& message) {
        return compressible(message, message.get_content().size(), message.get_content_type());
    }

    void add_vary(http_response& message, std::string_view field) {
        if (has_list_token(message, "Vary", field) || has_list_token(message, "Vary", "*")) return;
        const auto& vary = message.get_header("Vary");
        if (vary.empty()) {
            message.add_header("Vary", std::string(field));
        } else {
            message.set_header("Vary", vary + ", " + std::string(field));
        }
    }

    void compress(http_response& message, const http_request& request) {
        if (!compressible(message)) return;
        // Sent compressed or not depending on the request: caches must tell both apart
        add_vary(message, "Accept-Encoding");
        if (message.has_header("Content-Encoding")) return;

        auto coding = negotiate(accepted_by(request));
        if (coding == content_coding::identity) return;
        const auto& content = message.get_content();
        auto compressed = coding == content_coding::gzip ? ::thinger::util::gzip::compress(content)
                                                         : ::thinger::util::deflate::compress(content);
        if (compressed) {
            message.set_content(std::move(*compressed));
            message.add_header("Content-Encoding", coding == content_coding::gzip ? "gzip" : "deflate");
        }
    }

}

} // namespace thinger::http
