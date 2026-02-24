#include "../../util/logger.hpp"
#include "../../util/compression.hpp"
#include <utility>

#include "request.hpp"
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

    std::shared_ptr<server_connection> request::get_http_connection() const {
        return http_connection_.lock();
    }
    
    std::shared_ptr<http_stream> request::get_http_stream() const {
        return http_stream_.lock();
    }

    /*
    void request::add_matched_param(const std::string& param){
        uri_params_.emplace_back(param);
    }
     */


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
    
    auth_level request::get_required_auth_level() const {
        return matched_route_ ? matched_route_->get_auth_level() : auth_level::PUBLIC;
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

    std::string request::get_request_ip() const{
        auto http_connection = http_connection_.lock();
        return http_connection ? http_connection->get_socket()->get_remote_ip() : "";
    }

    /*
    exec_result request::get_request_data() const{
        const std::string& content = http_request_->get_body();
        const std::string& content_type = http_request_->get_content_type();

        // set content
        if(!content.empty()){
            if(boost::istarts_with(content_type, mime_types::application_json)){
                try{
                    return {true, nlohmann::json::parse(content)};
                }catch(...){
                    return {false, "invalid json payload", http_response::status::bad_request};
                }
            }else if(boost::istarts_with(content_type, mime_types::application_octect_stream)){
                std::vector<uint8_t> data(content.begin(), content.end());
                return {true, nlohmann::json::binary(std::move(data))};
            }else if(boost::istarts_with(content_type, mime_types::text_html) || boost::istarts_with(content_type, mime_types::text_plain)){
                return {true, content};
            }else if(boost::istarts_with(content_type, mime_types::application_form_urlencoded)){
                std::multimap<std::string, std::string> parameters;
                util::url::parse_url_encoded_data(content, parameters);
                return {true, std::move(parameters)};
            }else if(boost::istarts_with(content_type, mime_types::application_msgpack)){
                try{
                    return {true, nlohmann::json::from_msgpack(content)};
                }catch(...){
                    return {false, "invalid msgpack payload", http_response::status::bad_request};
                }
            }
            else if(boost::istarts_with(content_type, mime_types::application_cbor)){
                try{
                    return {true, nlohmann::json::from_cbor(content)};
                }catch(...){
                    return {false, "invalid cbor payload", http_response::status::bad_request};
                }
            }
            else if(boost::istarts_with(content_type, mime_types::application_ubjson)){
                try{
                    return {true, nlohmann::json::from_ubjson(content)};
                }catch(...){
                    return {false, "invalid ubjson payload", http_response::status::bad_request};
                }
            }else{
                // unknown content type, return as binary
                std::vector<uint8_t> vec(content.begin(), content.end());
                return {true, nlohmann::json::binary_t(std::move(vec))};
            }
        }

        return {true, nullptr};
    }*/

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

    // --- Deferred body reading support ---

    void request::set_read_ahead(const uint8_t* data, size_t size) {
        if (data && size > 0) {
            read_ahead_.assign(data, data + size);
            read_ahead_offset_ = 0;
        }
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

    size_t request::read_ahead_available() const {
        return read_ahead_.size() > read_ahead_offset_ ? read_ahead_.size() - read_ahead_offset_ : 0;
    }

    // --- Raw I/O (bypasses chunked decoding) ---

    thinger::awaitable<size_t> request::raw_read_some(uint8_t* buffer, size_t max_size) {
        // Consume from read-ahead first (using offset, O(1) per call)
        size_t avail = read_ahead_available();
        if (avail > 0) {
            size_t from_ahead = std::min(avail, max_size);
            std::memcpy(buffer, read_ahead_.data() + read_ahead_offset_, from_ahead);
            read_ahead_offset_ += from_ahead;
            if (read_ahead_offset_ >= read_ahead_.size()) {
                read_ahead_.clear();
                read_ahead_offset_ = 0;
            }
            co_return from_ahead;
        }

        // Read from socket
        auto sock = get_socket();
        if (sock) {
            auto [ec, bytes] = co_await sock->read_some(buffer, max_size);
            co_return bytes;
        }

        co_return 0;
    }

    // --- Chunked transfer encoding decoder ---

    static bool is_hex_char(uint8_t c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    static size_t hex_value(uint8_t c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    }

    thinger::awaitable<size_t> request::read_some_chunked(uint8_t* buffer, size_t max_size) {
        size_t output = 0;

        while (output == 0 && chunk_state_ != chunk_state::done) {
            if (chunk_state_ == chunk_state::data && chunk_remaining_ > 0) {
                // Fast path: read data directly into user buffer (no copy overhead)
                size_t to_read = std::min(chunk_remaining_, max_size - output);
                size_t bytes = co_await raw_read_some(buffer + output, to_read);
                if (bytes == 0) co_return output;
                chunk_remaining_ -= bytes;
                output += bytes;
                if (chunk_remaining_ == 0) {
                    chunk_state_ = chunk_state::data_cr;
                }
            } else {
                // Slow path: read a batch of raw bytes for framing
                uint8_t raw[512];
                size_t raw_bytes = co_await raw_read_some(raw, sizeof(raw));
                if (raw_bytes == 0) co_return output;

                size_t i = 0;
                while (i < raw_bytes && chunk_state_ != chunk_state::done && output < max_size) {
                    uint8_t byte = raw[i];

                    switch (chunk_state_) {
                        case chunk_state::size:
                            if (is_hex_char(byte)) {
                                chunk_size_accum_ = chunk_size_accum_ * 16 + hex_value(byte);
                                i++;
                            } else if (byte == '\r') {
                                chunk_state_ = chunk_state::size_lf;
                                i++;
                            } else {
                                // Skip chunk extensions (e.g. ";ext=val")
                                i++;
                            }
                            break;

                        case chunk_state::size_lf:
                            if (byte == '\n') {
                                if (chunk_size_accum_ == 0) {
                                    // Last chunk — expect trailing CRLF
                                    chunk_state_ = chunk_state::trailer_lf;
                                } else {
                                    chunk_remaining_ = chunk_size_accum_;
                                    chunk_size_accum_ = 0;
                                    chunk_state_ = chunk_state::data;
                                }
                                i++;

                                // If entering data state and raw buffer has more bytes,
                                // copy data directly from raw buffer (avoid another read)
                                if (chunk_state_ == chunk_state::data && i < raw_bytes) {
                                    size_t data_avail = std::min(chunk_remaining_, raw_bytes - i);
                                    size_t to_copy = std::min(data_avail, max_size - output);
                                    std::memcpy(buffer + output, raw + i, to_copy);
                                    output += to_copy;
                                    i += to_copy;
                                    chunk_remaining_ -= to_copy;
                                    if (chunk_remaining_ == 0) {
                                        chunk_state_ = chunk_state::data_cr;
                                    }
                                }
                            } else {
                                i++; // malformed, skip
                            }
                            break;

                        case chunk_state::data:
                            // We shouldn't reach here (handled above), but safety
                            {
                                size_t data_avail = std::min(chunk_remaining_, raw_bytes - i);
                                size_t to_copy = std::min(data_avail, max_size - output);
                                std::memcpy(buffer + output, raw + i, to_copy);
                                output += to_copy;
                                i += to_copy;
                                chunk_remaining_ -= to_copy;
                                if (chunk_remaining_ == 0) {
                                    chunk_state_ = chunk_state::data_cr;
                                }
                            }
                            break;

                        case chunk_state::data_cr:
                            if (byte == '\r') chunk_state_ = chunk_state::data_lf;
                            i++;
                            break;

                        case chunk_state::data_lf:
                            if (byte == '\n') {
                                chunk_state_ = chunk_state::size;
                                chunk_size_accum_ = 0;
                            }
                            i++;
                            break;

                        case chunk_state::trailer_lf:
                            if (byte == '\r') {
                                i++;
                                // Expect final \n
                                if (i < raw_bytes && raw[i] == '\n') {
                                    i++;
                                }
                                chunk_state_ = chunk_state::done;
                            } else {
                                // Trailer header line — skip until we find empty CRLF
                                i++;
                            }
                            break;

                        case chunk_state::done:
                            break;
                    }
                }

                // Push unconsumed raw bytes back to read-ahead for next call
                if (i < raw_bytes) {
                    read_ahead_.assign(raw + i, raw + raw_bytes);
                    read_ahead_offset_ = 0;
                }
            }
        }

        co_return output;
    }

    // --- Public read API (dispatches to raw or chunked) ---

    thinger::awaitable<size_t> request::read(uint8_t* buffer, size_t size) {
        if (is_chunked()) {
            // For chunked, read decoded data until we have `size` bytes or EOF
            size_t total = 0;
            while (total < size) {
                size_t bytes = co_await read_some_chunked(buffer + total, size - total);
                if (bytes == 0) break;
                total += bytes;
            }
            co_return total;
        }

        // Non-chunked: read exact size
        size_t total = 0;

        // Consume from read-ahead first
        size_t avail = read_ahead_available();
        if (avail > 0) {
            size_t from_ahead = std::min(avail, size);
            std::memcpy(buffer, read_ahead_.data() + read_ahead_offset_, from_ahead);
            read_ahead_offset_ += from_ahead;
            total += from_ahead;
            if (read_ahead_offset_ >= read_ahead_.size()) {
                read_ahead_.clear();
                read_ahead_offset_ = 0;
            }
        }

        // Read remaining from socket
        if (total < size) {
            auto sock = get_socket();
            if (sock) {
                size_t remaining = size - total;
                auto [ec, bytes] = co_await sock->read(buffer + total, remaining);
                total += bytes;
            }
        }

        co_return total;
    }

    thinger::awaitable<size_t> request::read_some(uint8_t* buffer, size_t max_size) {
        if (is_chunked()) {
            co_return co_await read_some_chunked(buffer, max_size);
        }
        co_return co_await raw_read_some(buffer, max_size);
    }

    thinger::awaitable<bool> request::read_body() {
        if (!http_request_) co_return false;

        if (is_chunked()) {
            // Chunked: read decoded chunks until EOF, respecting max_body_size
            auto& body = http_request_->get_body();
            uint8_t buf[8192];
            while (true) {
                size_t bytes = co_await read_some_chunked(buf, sizeof(buf));
                if (bytes == 0) break;
                if (body.size() + bytes > max_body_size_) co_return false;
                body.append(reinterpret_cast<char*>(buf), bytes);
            }

            // Decompress chunked body if Content-Encoding is set
            if (http_request_->has_header("Content-Encoding")) {
                std::string encoding = http_request_->get_header("Content-Encoding");
                if (encoding == "gzip") {
                    auto decompressed = ::thinger::util::gzip::decompress(body);
                    if (decompressed) {
                        body = std::move(*decompressed);
                        http_request_->remove_header("Content-Encoding");
                    } else {
                        LOG_ERROR("Failed to decompress gzip request body");
                        co_return false;
                    }
                } else if (encoding == "deflate") {
                    auto decompressed = ::thinger::util::deflate::decompress(body);
                    if (decompressed) {
                        body = std::move(*decompressed);
                        http_request_->remove_header("Content-Encoding");
                    } else {
                        LOG_ERROR("Failed to decompress deflate request body");
                        co_return false;
                    }
                }
            }

            co_return true;
        }

        // Content-Length based
        size_t cl = http_request_->get_content_length();
        if (cl == 0) co_return true;

        auto& body = http_request_->get_body();
        body.resize(cl);

        size_t bytes_read = co_await read(reinterpret_cast<uint8_t*>(body.data()), cl);
        if (bytes_read != cl) co_return false;

        // Decompress body if Content-Encoding is set
        if (http_request_->has_header("Content-Encoding")) {
            std::string encoding = http_request_->get_header("Content-Encoding");
            if (encoding == "gzip") {
                auto decompressed = ::thinger::util::gzip::decompress(body);
                if (decompressed) {
                    body = std::move(*decompressed);
                    http_request_->remove_header("Content-Encoding");
                } else {
                    LOG_ERROR("Failed to decompress gzip request body");
                    co_return false;
                }
            } else if (encoding == "deflate") {
                auto decompressed = ::thinger::util::deflate::decompress(body);
                if (decompressed) {
                    body = std::move(*decompressed);
                    http_request_->remove_header("Content-Encoding");
                } else {
                    LOG_ERROR("Failed to decompress deflate request body");
                    co_return false;
                }
            }
        }

        co_return true;
    }

}
