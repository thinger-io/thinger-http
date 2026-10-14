#include "request_context.hpp"

#include <stdexcept>
#include <string>

namespace thinger::http {

request_context::request_context(const request_context& other) {
    entries_.reserve(other.entries_.size());
    for (const auto& source : other.entries_) {
        auto value = source.value->clone();
        if (!value) {
            throw std::logic_error(std::string("request_context: the value of key '") + source.name +
                                   "' cannot be copied");
        }
        entries_.push_back(entry{source.key, source.type, source.name, std::move(value)});
    }
}

request_context& request_context::operator=(const request_context& other) {
    if (this != &other) {
        request_context copy(other);
        entries_.swap(copy.entries_);
    }
    return *this;
}

} // namespace thinger::http
