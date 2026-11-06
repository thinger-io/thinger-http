#include "http_application.hpp"
#include "response_compression.hpp"
#include "../common/http_request.hpp"
#include "../common/http_response.hpp"
#include "../common/syntax.hpp"
#include "../../util/ascii.hpp"
#include "../../util/logger.hpp"
#include <boost/algorithm/string/join.hpp>
#include <algorithm>

namespace thinger::http::detail {

// The options of http_application::enable_cors(), checked, with what does not depend on the
// request already computed. Immutable: read from any thread answering a request
struct cors_policy {
    bool any_origin = false;
    std::vector<std::string> origins;
    std::function<bool(std::string_view, const http_request&)> origin_filter;
    std::string methods;
    std::optional<std::string> allowed_headers;
    std::string exposed_headers;
    std::string max_age;
    bool credentials = false;
    bool preflight_continue = false;
};

namespace {

    constexpr const char* allow_origin = "Access-Control-Allow-Origin";
    constexpr const char* allow_credentials = "Access-Control-Allow-Credentials";
    constexpr const char* expose_headers = "Access-Control-Expose-Headers";
    constexpr const char* allow_methods = "Access-Control-Allow-Methods";
    constexpr const char* allow_headers = "Access-Control-Allow-Headers";
    constexpr const char* max_age = "Access-Control-Max-Age";
    constexpr const char* request_method = "Access-Control-Request-Method";
    constexpr const char* request_headers = "Access-Control-Request-Headers";

    bool all_tokens(const std::vector<std::string>& values, std::string_view what) {
        for (const auto& value : values) {
            if (!syntax::is_token(value)) {
                LOG_ERROR("Invalid CORS options: '{}' is not a valid {}", value, what);
                return false;
            }
        }
        return true;
    }

    // A value for Access-Control-Allow-Origin: no control character, HTAB included
    bool valid_origin(std::string_view origin) {
        return syntax::is_field_value(origin) && origin.find('\t') == std::string_view::npos;
    }

    bool valid_origins(const std::vector<std::string>& origins, bool credentials) {
        bool any = std::find(origins.begin(), origins.end(), "*") != origins.end();
        if (any && origins.size() > 1) {
            LOG_ERROR("Invalid CORS options: '*' cannot be listed with other origins");
            return false;
        }
        // Browsers reject this pair (Fetch, CORS check)
        if (any && credentials) {
            LOG_ERROR("Invalid CORS options: credentials cannot be used with any origin ('*')");
            return false;
        }
        for (const auto& origin : origins) {
            if (!valid_origin(origin)) {
                LOG_ERROR("Invalid CORS options: origin '{}' has control characters", origin);
                return false;
            }
        }
        return true;
    }

    bool valid_options(const cors_options& options) {
        // With a filter the list is not used
        if (!options.origin_filter && !valid_origins(options.origins, options.credentials)) return false;
        if (!all_tokens(options.methods, "method")) return false;
        if (options.allowed_headers && !all_tokens(*options.allowed_headers, "header name")) return false;
        if (!all_tokens(options.exposed_headers, "header name")) return false;
        if (options.max_age && options.max_age->count() < 0) {
            LOG_ERROR("Invalid CORS options: max_age cannot be negative");
            return false;
        }
        return true;
    }

    void add_unless_set(http_response& message, const char* name, std::string_view value) {
        if (!message.has_header(name)) message.add_header(name, std::string(value));
    }

    // The value of a header of the request, repeated ones as one list
    std::string joined(const http_request& request, const char* name) {
        std::string list;
        for (const auto& value : request.get_headers_with_key(name)) {
            if (!list.empty()) list += ", ";
            list += value;
        }
        return list;
    }

    // What a preflight says besides what any response says
    void apply_preflight(const cors_policy& policy, const cors_decision& decision, http_response& message) {
        if (!policy.methods.empty()) add_unless_set(message, allow_methods, policy.methods);
        if (policy.allowed_headers) {
            if (!policy.allowed_headers->empty()) add_unless_set(message, allow_headers, *policy.allowed_headers);
        } else {
            compression::add_vary(message, request_headers);
            if (!decision.requested_headers.empty()) add_unless_set(message, allow_headers, decision.requested_headers);
        }
        if (!policy.max_age.empty()) add_unless_set(message, max_age, policy.max_age);
    }

}

std::shared_ptr<const cors_policy> make_cors_policy(cors_options options) {
    if (!valid_options(options)) return nullptr;

    auto policy = std::make_shared<cors_policy>();
    policy->any_origin = !options.origin_filter && options.origins.size() == 1 && options.origins.front() == "*";
    policy->origin_filter = std::move(options.origin_filter);
    if (!policy->any_origin && !policy->origin_filter) policy->origins = std::move(options.origins);
    policy->methods = boost::algorithm::join(options.methods, ", ");
    if (options.allowed_headers) policy->allowed_headers = boost::algorithm::join(*options.allowed_headers, ", ");
    policy->exposed_headers = boost::algorithm::join(options.exposed_headers, ", ");
    if (options.max_age) policy->max_age = std::to_string(options.max_age->count());
    policy->credentials = options.credentials;
    policy->preflight_continue = options.preflight_continue;
    return policy;
}

static std::string granted_origin(const cors_policy& policy, const http_request& request) {
    if (policy.any_origin) return "*";

    // Repeated headers make one value, which no entry of the list equals: not a single origin
    std::string origin = joined(request, "Origin");
    if (origin.empty()) return origin;

    bool granted = policy.origin_filter
        ? policy.origin_filter(origin, request)
        : std::any_of(policy.origins.begin(), policy.origins.end(), [&](const std::string& listed) {
              return ::thinger::util::ascii::iequals(listed, origin);
          });
    if (!granted) origin.clear();
    return origin;
}

cors_decision decide_cors(const cors_policy& policy, const http_request& request) {
    cors_decision decision;
    decision.origin = granted_origin(policy, request);
    // As Fetch and rs/cors: any other OPTIONS is a request as any other
    if (request.get_method() == method::OPTIONS && !request.get_header(request_method).empty()) {
        decision.preflight = true;
        decision.answer_preflight = !policy.preflight_continue;
        if (!policy.allowed_headers) decision.requested_headers = joined(request, request_headers);
    }
    return decision;
}

void apply_cors(const cors_policy& policy, const cors_decision& decision, http_response& message) {
    // The answer depends on the Origin of the request, granted or not, and even without one
    if (!policy.any_origin) compression::add_vary(message, "Origin");
    if (decision.origin.empty()) return;

    // What the handler or a middleware set stays, as res.setHeader() after cors() in Express
    add_unless_set(message, allow_origin, decision.origin);
    // Never with "*", whoever set it: the filter may grant an Origin: *, or the handler set it
    if (const auto& allowed = message.get_header(allow_origin); policy.credentials && !allowed.empty() && allowed != "*") {
        add_unless_set(message, allow_credentials, "true");
    }
    if (!policy.exposed_headers.empty()) add_unless_set(message, expose_headers, policy.exposed_headers);
    if (decision.preflight) apply_preflight(policy, decision, message);
}

} // namespace thinger::http::detail
