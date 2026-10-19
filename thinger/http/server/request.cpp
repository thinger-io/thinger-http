#include "../../util/logger.hpp"
#include "../../util/compression.hpp"
#include <utility>

#include "request.hpp"
#include "trusted_proxies.hpp"
#include "body_decoders.hpp"
#include "routing/route.hpp"

namespace thinger::http{

    using nlohmann::json;
    using std::string;

    request::request(
        const std::shared_ptr<server_connection>& http_connection,
        const std::shared_ptr<http_stream>& http_stream,
        std::shared_ptr<http_request> http_request) :
        http_connection_{http_connection},
        http_stream_{http_stream},
        http_request_{std::move(http_request)}
    {
        if (http_request_) {
            body_reader_.set_framing(http_request_->is_chunked_transfer(), http_request_->pending_body_size());
        }
        body_reader_.set_limit(default_max_body_size);
        if (http_connection) {
            expect_continue_ = http_stream && http_stream->get_continue() == http_stream::continue_state::pending;
            // (capturing only `this` fits in the function, without allocating)
            body_reader_.set_source([this](uint8_t* buffer, size_t size) -> thinger::awaitable<size_t> {
                auto conn = http_connection_.lock();
                // Nothing more is read once the connection closes gracefully: it drops the
                // rest itself (see server_connection::linger())
                if (!conn || conn->lingering()) co_return 0;
                // The body is needed now: ask the client for it (Expect: 100-continue)
                if (expect_continue_) [[unlikely]] {
                    expect_continue_ = false;
                    if (auto stream = http_stream_.lock()) conn->send_continue(*stream);
                }
                auto [ec, bytes] = co_await conn->get_socket()->read_some(buffer, size);
                // a body that keeps arriving keeps the connection alive (e.g. a long upload)
                if (bytes > 0) conn->extend_timeout();
                co_return bytes;
            });
        }
    }

    request::~request()= default;

    const string& request::operator[](const std::string& param) const{
        return get_uri_parameter(param);
    }

    bool request::has(const std::string& param) const {
        return params_.contains(param);
    }

    bool request::erase(const std::string& param){
        return params_.erase(param)>0;
    }

    std::shared_ptr<http_request> request::get_http_request(){
        return http_request_;
    }

    std::shared_ptr<const http_request> request::get_http_request() const{
        return http_request_;
    }

    std::shared_ptr<server_connection> request::get_http_connection() const {
        return http_connection_.lock();
    }
    
    std::shared_ptr<http_stream> request::get_http_stream() const {
        return http_stream_.lock();
    }

    void request::set_auth_user(const std::string& auth_user){
        auth_user_ = auth_user;
    }

    const std::string& request::get_auth_user() const{
        return auth_user_;
    }
    
    void request::set_matched_route(const route* route) {
        matched_route_ = route;
    }
    
    const route* request::get_matched_route() const {
        return matched_route_;
    }
    
    void request::set_uri_parameter(const std::string& param, const std::string& value){
        // erase all existing entries with the specified key
        params_.erase(param);

        // insert the new key-value pair
        params_.insert(std::make_pair(param, value));
    }

    void request::add_uri_parameter(const std::string& param, const std::string& value){
        params_.insert({param, value});
    }

    const std::string& request::get_uri_parameter(const std::string& param) const{
        auto it = params_.find(param);
        if(it!=params_.end()){
            return it->second;
        }
        LOG_WARNING("cannot find required parameter: {}", param);
        static std::string empty_string;
        return empty_string;
    }

    const std::multimap<std::string, std::string>& request::get_uri_parameters() const{
        return params_;
    }

    void request::set_auth_groups(const std::set<std::string> &groups) {
        groups_ = groups;
    }

    const std::set<std::string> & request::get_auth_groups() const {
        return groups_;
    }

    std::string request::debug_parameters() const {
        std::stringstream str;
        for(const auto& param: params_){
            str << "(" << param.first << ":" << param.second << ") ";
        }
        return str.str();
    }

    std::string request::get_peer_ip() const{
        if (!peer_ip_.empty()) return peer_ip_;
        auto http_connection = http_connection_.lock();
        return http_connection ? http_connection->get_socket()->get_remote_ip() : "";
    }

    std::string request::get_request_ip() const{
        auto peer = get_peer_ip();
        if (!trusted_proxies_ || trusted_proxies_->empty() || !http_request_) return peer;
        return trusted_proxies_->resolve(peer, "http", *http_request_).ip;
    }

    std::string request::get_request_scheme() const{
        std::string scheme = http_request_ && http_request_->is_ssl() ? "https" : "http";
        if (!trusted_proxies_ || trusted_proxies_->empty() || !http_request_) return scheme;
        return trusted_proxies_->resolve(get_peer_ip(), scheme, *http_request_).scheme;
    }

    bool request::keep_alive() const{
        return http_request_ && http_request_->keep_alive();
    }

    // Convenience methods implementation

    std::string request::query(const std::string& key) const {
        if (http_request_ && http_request_->has_uri_parameter(key)) {
            return http_request_->get_uri_parameter(key);
        }
        return "";
    }

    std::string request::query(const std::string& key, const std::string& default_value) const {
        if (http_request_ && http_request_->has_uri_parameter(key)) {
            return http_request_->get_uri_parameter(key);
        }
        return default_value;
    }

    std::string request::body() const {
        return http_request_ ? http_request_->get_body() : "";
    }

    nlohmann::json request::json() const {
        if (!http_request_) {
            return nlohmann::json{};
        }
        const auto& content = http_request_->get_body();
        if (content.empty()) {
            return nlohmann::json{};
        }
        auto j = nlohmann::json::parse(content, nullptr, false);
        if (j.is_discarded()) {
            return nlohmann::json{};
        }
        return j;
    }

    std::string request::header(const std::string& key) const {
        return http_request_ ? http_request_->get_header(key) : "";
    }

    const body_settings& request::get_body_settings() const {
        return body_settings_ ? *body_settings_ : body_settings::defaults();
    }

    // --- Body reading ---

    void request::set_read_ahead(const uint8_t* data, size_t size) {
        body_reader_.set_read_ahead(data, size);
    }

    size_t request::content_length() const {
        return http_request_ ? http_request_->get_content_length() : 0;
    }

    bool request::is_chunked() const {
        return http_request_ && http_request_->is_chunked_transfer();
    }

    std::shared_ptr<asio::socket> request::get_socket() const {
        auto conn = http_connection_.lock();
        return conn ? conn->get_socket() : nullptr;
    }

    std::vector<uint8_t> request::take_read_ahead() {
        return body_reader_.take_read_ahead();
    }

    size_t request::read_ahead_available() const {
        return body_reader_.read_ahead_available();
    }

    void request::note_body_read() {
        if (body_reader_.has_pending() || body_reader_.error() != body_error::none) return;
        if (auto stream = http_stream_.lock()) stream->set_input_pending(false);
    }

    thinger::awaitable<size_t> request::read(uint8_t* buffer, size_t size) {
        size_t bytes = co_await body_reader_.read(buffer, size);
        note_body_read();
        co_return bytes;
    }

    thinger::awaitable<size_t> request::read_some(uint8_t* buffer, size_t max_size) {
        size_t bytes = co_await body_reader_.read_some(buffer, max_size);
        note_body_read();
        co_return bytes;
    }

    // Decode the body in place according to its Content-Encoding (gzip, x-gzip, deflate, or
    // several of them; a body with any other coding is left to the handler, with its
    // Content-Encoding), up to `max_size` bytes once decoded
    static body_error decode_content_encoding(http_request& http_request, size_t max_size) {
        if (!http_request.has_header("Content-Encoding")) return body_error::none;

        // (the values of every Content-Encoding header make one list)
        std::string encoding;
        for (const auto& value : http_request.get_headers_with_key("Content-Encoding")) {
            if (!encoding.empty()) encoding += ", ";
            encoding += value;
        }
        switch (::thinger::util::decode_content(encoding, http_request.get_body(), max_size)) {
            case ::thinger::util::content_decoding::decoded:
                http_request.remove_headers("Content-Encoding");
                return body_error::none;
            case ::thinger::util::content_decoding::unsupported:
                return body_error::none;
            case ::thinger::util::content_decoding::too_large:
                LOG_ERROR("Decompressed {} request body exceeds the maximum body size", encoding);
                return body_error::too_large;
            case ::thinger::util::content_decoding::invalid:
                break;
        }
        LOG_ERROR("Failed to decompress {} request body", encoding);
        return body_error::bad_encoding;
    }

    thinger::awaitable<bool> request::read_body() {
        if (!http_request_) co_return false;

        // Nothing left to read (already read, or no body)
        if (!body_reader_.has_pending()) co_return body_reader_.error() == body_error::none;

        bool read = co_await body_reader_.read_all(http_request_->get_body(), body_reader_.limit());
        note_body_read();
        if (!read) co_return false;

        auto error = decode_content_encoding(*http_request_, body_reader_.limit());
        if (error != body_error::none) {
            body_reader_.set_error(error);
            co_return false;
        }
        co_return true;
    }

    bool request::has_pending_body() const {
        return http_request_ && body_reader_.has_pending();
    }

    body_error request::get_body_error() const {
        return body_reader_.error();
    }

    thinger::awaitable<multipart_form> request::read_multipart(multipart_options options) {
        co_return co_await http::read_multipart(*this, std::move(options));
    }

    thinger::awaitable<bool> request::discard_body(size_t max_size) {
        bool discarded = co_await body_reader_.discard(max_size);
        note_body_read();
        co_return discarded;
    }

}
