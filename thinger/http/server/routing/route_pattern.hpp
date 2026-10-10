#ifndef THINGER_HTTP_ROUTE_PATTERN_HPP
#define THINGER_HTTP_ROUTE_PATTERN_HPP

#include <bitset>
#include <cstdint>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <boost/container/small_vector.hpp>

namespace thinger::http::detail {

// Part of a route pattern: literal text, or a parameter (:name or :name(regex))
struct pattern_token {
    bool parameter = false;
    std::string text;   // literal text, or parameter name
    std::string regex;  // parameter regex, empty for :name (any non-empty text without '/')
};

// Split a route pattern into literal text and parameters. A parameter is ':' followed by
// a name ([a-zA-Z_][a-zA-Z0-9_]*) and, optionally, a regex in balanced parentheses.
// Throws std::invalid_argument if the parentheses of a parameter regex are not balanced.
std::vector<pattern_token> parse_route_pattern(std::string_view pattern);

// Whether a parameter regex depends on the text around it: anchors (^, $), word
// boundaries, lookaheads or back-references
bool regex_depends_on_context(std::string_view regex);

// Regex matching the tokens, a capture group per parameter (and the groups of their regexes)
std::string pattern_regex(std::span<const pattern_token> tokens);

// Values captured while matching a path, one per parameter, in order. A capture with no
// data (default constructed) is a parameter that did not participate in the match.
using route_captures = boost::container::small_vector<std::string_view, 8>;

// Matcher of a sequence of tokens: one path segment (between slashes), or the rest of the
// path from a segment on. Parameter regexes made of characters, character classes and
// quantifiers (e.g. [a-zA-Z0-9_-]{1,32}, [0-9]+, .+, a UUID) are compiled to a sequence
// of character sets with repetitions, and a lone parameter with alternative words
// (e.g. properties|buckets) to the list of words, both matched without std::regex; any
// other regex (groups, anchors...) makes the matcher fall back to std::regex.
class path_matcher {
public:
    explicit path_matcher(std::span<const pattern_token> tokens);

    // Whether the whole text matches; on success, appends a capture per parameter
    bool match(std::string_view text, route_captures& captures) const;

    // Whether a match may include '/' (so it may span several path segments)
    bool may_match_slash() const { return may_match_slash_; }

    // A single parameter that may match '/', as :path(.+): matches the rest of the path
    bool is_catch_all() const { return catch_all_; }

    // Regex of the tokens, without parameter names: matchers with the same key match
    // the same texts
    const std::string& key() const { return key_; }

    // Whether some text may match both matchers: false only when they cannot start with
    // the same character (a cheap check, true when in doubt)
    bool may_overlap(const path_matcher& other) const;

private:
    static constexpr uint32_t unbounded = UINT32_MAX;

    // A character of a set, repeated between min and max times
    struct atom {
        std::bitset<256> chars;
        uint32_t min = 1;
        uint32_t max = 1;
    };

    // Parameter matching the atoms [first, last)
    struct group {
        uint16_t first;
        uint16_t last;
    };

    static constexpr size_t max_atoms = 64;

    // Characters a match may start with (all of them if unknown, or if it may be empty)
    std::bitset<256> first_chars() const;

    static bool compile_regex(std::string_view regex, std::vector<atom>& atoms);
    bool match_atoms(size_t index, size_t position, std::string_view text, size_t* starts) const;

    std::string key_;
    bool may_match_slash_ = false;
    bool catch_all_ = false;

    // Compiled forms
    std::vector<atom> atoms_;
    std::vector<group> groups_;
    std::optional<std::vector<std::string>> words_;

    // Fallback, with the capture group of each parameter
    std::optional<std::regex> regex_;
    std::vector<size_t> regex_groups_;
};

} // namespace thinger::http::detail

#endif // THINGER_HTTP_ROUTE_PATTERN_HPP
