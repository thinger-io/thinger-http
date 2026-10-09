#include "route_pattern.hpp"
#include <algorithm>
#include <deque>
#include <stdexcept>

namespace thinger::http::detail {

namespace {

    bool is_name_start(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }

    bool is_name_char(char c) {
        return is_name_start(c) || (c >= '0' && c <= '9');
    }

    bool is_alnum(char c) {
        return is_name_char(c) && c != '_';
    }

    // Position of the parenthesis closing the one at `open`, skipping escaped characters
    // and character classes; npos if it is not closed
    size_t closing_parenthesis(std::string_view text, size_t open) {
        int depth = 0;
        bool in_class = false;
        for (size_t i = open; i < text.size(); ++i) {
            char c = text[i];
            if (c == '\\') {
                ++i;
            } else if (in_class) {
                if (c == ']') in_class = false;
            } else if (c == '[') {
                in_class = true;
            } else if (c == '(') {
                ++depth;
            } else if (c == ')' && --depth == 0) {
                return i;
            }
        }
        return std::string_view::npos;
    }

    // Capture groups of a regex: '(' not escaped, not in a class, not followed by '?'
    size_t capture_groups(std::string_view regex) {
        size_t groups = 0;
        bool in_class = false;
        for (size_t i = 0; i < regex.size(); ++i) {
            char c = regex[i];
            if (c == '\\') {
                ++i;
            } else if (in_class) {
                if (c == ']') in_class = false;
            } else if (c == '[') {
                in_class = true;
            } else if (c == '(' && (i + 1 == regex.size() || regex[i + 1] != '?')) {
                ++groups;
            }
        }
        return groups;
    }

    // Whether a regex may match '/'. Conservative: true unless it has none of the
    // constructs that may (any character, negated or POSIX classes, ranges including '/',
    // escapes of other characters...)
    bool regex_may_match_slash(std::string_view regex) {
        bool in_class = false;
        for (size_t i = 0; i < regex.size(); ++i) {
            char c = regex[i];
            if (c == '\\') {
                if (++i == regex.size()) return true;
                char escaped = regex[i];
                if (escaped == '/' || (is_alnum(escaped) && std::string_view("dwsbB").find(escaped) == std::string_view::npos)) {
                    return true;
                }
            } else if (in_class) {
                if (c == ']') {
                    in_class = false;
                } else if (c == '/' || c == '[') {
                    return true;
                } else if (c == '-' && i + 1 < regex.size() && regex[i + 1] != ']') {
                    auto low = static_cast<unsigned char>(regex[i - 1]);
                    auto high = static_cast<unsigned char>(regex[i + 1]);
                    if (high == '\\' || (low <= '/' && high >= '/')) return true;
                }
            } else if (c == '[') {
                // negated classes, and [] (empty in ECMAScript, a literal ']' elsewhere)
                if (i + 1 < regex.size() && (regex[i + 1] == '^' || regex[i + 1] == ']')) return true;
                in_class = true;
            } else if (c == '.' || c == '/') {
                return true;
            }
        }
        return false;
    }

    // Alternative words of a regex such as "properties|buckets|flows"; empty if it has
    // anything else than alternatives of literal characters
    std::optional<std::vector<std::string>> regex_words(std::string_view regex) {
        if (regex.find('|') == std::string_view::npos) return std::nullopt;
        std::vector<std::string> words(1);
        for (char c : regex) {
            if (c == '|') {
                words.emplace_back();
            } else if (c == '\0' || std::string_view(".^$()[]{}*+?\\").find(c) != std::string_view::npos) {
                return std::nullopt;
            } else {
                words.back() += c;
            }
        }
        return words;
    }

    std::bitset<256> char_range(int low, int high) {
        std::bitset<256> chars;
        for (int c = low; c <= high; ++c) chars.set(c);
        return chars;
    }

    // Characters of \d, \w and \s (and their complements \D, \W, \S), as std::regex
    // matches them on char in the default locale
    bool class_escape(char escape, std::bitset<256>& chars) {
        switch (escape) {
            case 'd': case 'D':
                chars = char_range('0', '9');
                break;
            case 'w': case 'W':
                chars = char_range('a', 'z') | char_range('A', 'Z') | char_range('0', '9');
                chars.set('_');
                break;
            case 's': case 'S':
                chars = char_range('\t', '\r');
                chars.set(' ');
                break;
            default:
                return false;
        }
        if (escape >= 'A' && escape <= 'Z') chars.flip();
        return true;
    }

    // Character class [...] starting at `i`; leaves `i` past it
    bool parse_class(std::string_view regex, size_t& i, std::bitset<256>& chars) {
        size_t j = i + 1;
        bool negated = j < regex.size() && regex[j] == '^';
        if (negated) ++j;
        if (j < regex.size() && regex[j] == ']') return false;  // [] and [^]

        std::bitset<256> set;
        while (true) {
            if (j >= regex.size()) return false;
            char c = regex[j];
            if (c == ']') {
                ++j;
                break;
            }
            if (c == '[') return false;  // POSIX classes

            int low = -1;
            if (c == '\\') {
                if (j + 1 >= regex.size()) return false;
                char escaped = regex[j + 1];
                if (is_alnum(escaped)) {
                    std::bitset<256> escaped_chars;
                    if (!class_escape(escaped, escaped_chars)) return false;
                    set |= escaped_chars;
                } else {
                    low = static_cast<unsigned char>(escaped);
                }
                j += 2;
            } else {
                low = static_cast<unsigned char>(c);
                ++j;
            }
            if (low < 0) continue;

            // Range low-high, unless '-' is the last character of the class
            if (j + 1 < regex.size() && regex[j] == '-' && regex[j + 1] != ']') {
                int high;
                char h = regex[j + 1];
                if (h == '\\') {
                    if (j + 2 >= regex.size() || is_alnum(regex[j + 2])) return false;
                    high = static_cast<unsigned char>(regex[j + 2]);
                    j += 3;
                } else if (h == '[') {
                    return false;
                } else {
                    high = static_cast<unsigned char>(h);
                    j += 2;
                }
                if (high < low) return false;
                set |= char_range(low, high);
            } else {
                set.set(low);
            }
        }
        chars = negated ? ~set : set;
        i = j;
        return true;
    }

    // Quantifier {n}, {n,} or {n,m} starting at `i`; leaves `i` past it
    bool parse_braces(std::string_view regex, size_t& i, uint32_t& min, uint32_t& max, uint32_t unbounded) {
        auto number = [&](size_t& j, uint32_t& value) {
            size_t start = j;
            uint64_t result = 0;
            while (j < regex.size() && regex[j] >= '0' && regex[j] <= '9') {
                result = result * 10 + (regex[j++] - '0');
                if (result >= unbounded) return false;
            }
            value = static_cast<uint32_t>(result);
            return j > start;
        };
        size_t j = i + 1;
        if (!number(j, min)) return false;
        max = min;
        if (j < regex.size() && regex[j] == ',') {
            ++j;
            max = unbounded;
            if (j < regex.size() && regex[j] != '}' && !number(j, max)) return false;
        }
        if (j >= regex.size() || regex[j] != '}' || max < min) return false;
        i = j + 1;
        return true;
    }

} // namespace

std::vector<pattern_token> parse_route_pattern(std::string_view pattern) {
    std::vector<pattern_token> tokens;
    size_t i = 0;
    while (i < pattern.size()) {
        if (pattern[i] == ':' && i + 1 < pattern.size() && is_name_start(pattern[i + 1])) {
            size_t end = i + 2;
            while (end < pattern.size() && is_name_char(pattern[end])) ++end;
            pattern_token token{true, std::string(pattern.substr(i + 1, end - i - 1)), {}};
            if (end < pattern.size() && pattern[end] == '(') {
                size_t close = closing_parenthesis(pattern, end);
                if (close == std::string_view::npos) {
                    throw std::invalid_argument("Unbalanced parentheses in route pattern: " + std::string(pattern));
                }
                token.regex = pattern.substr(end + 1, close - end - 1);
                end = close + 1;
            }
            tokens.push_back(std::move(token));
            i = end;
            continue;
        }
        if (tokens.empty() || tokens.back().parameter) tokens.push_back({});
        tokens.back().text += pattern[i++];
    }
    return tokens;
}

bool regex_depends_on_context(std::string_view regex) {
    bool in_class = false;
    for (size_t i = 0; i < regex.size(); ++i) {
        char c = regex[i];
        if (c == '\\') {
            if (++i == regex.size()) return false;
            if (!in_class && (regex[i] == 'b' || regex[i] == 'B' || (regex[i] >= '1' && regex[i] <= '9'))) return true;
        } else if (in_class) {
            if (c == ']') in_class = false;
        } else if (c == '[') {
            if (i + 1 < regex.size() && regex[i + 1] == '^') ++i;
            // [] and [^] end where they start in ECMAScript
            if (i + 1 < regex.size() && regex[i + 1] == ']') {
                ++i;
            } else {
                in_class = true;
            }
        } else if (c == '^' || c == '$' || regex.substr(i, 3) == "(?=" || regex.substr(i, 3) == "(?!") {
            return true;
        }
    }
    return false;
}

std::string pattern_regex(std::span<const pattern_token> tokens) {
    std::string regex;
    for (const auto& token : tokens) {
        if (token.parameter) {
            regex += '(';
            regex += token.regex.empty() ? "[^/]+" : token.regex;
            regex += ')';
            continue;
        }
        for (char c : token.text) {
            if (std::string_view(".^$|()[]{}*+?\\").find(c) != std::string_view::npos) regex += '\\';
            regex += c;
        }
    }
    return regex;
}

path_matcher::path_matcher(std::span<const pattern_token> tokens)
    : key_(pattern_regex(tokens))
{
    if (tokens.size() == 1 && tokens.front().parameter) {
        words_ = regex_words(tokens.front().regex);
        if (words_) {
            may_match_slash_ = std::any_of(words_->begin(), words_->end(),
                                           [](const std::string& word) { return word.find('/') != std::string::npos; });
            return;
        }
    }

    bool compiled = true;
    for (const auto& token : tokens) {
        if (!token.parameter) {
            for (char c : token.text) {
                atom literal;
                literal.chars.set(static_cast<unsigned char>(c));
                atoms_.push_back(literal);
            }
            continue;
        }
        size_t first = atoms_.size();
        if (token.regex.empty()) {
            atom any;
            any.chars.set();
            any.chars.reset('/');
            any.max = unbounded;
            atoms_.push_back(any);
        } else if (!compile_regex(token.regex, atoms_)) {
            compiled = false;
            break;
        }
        groups_.push_back({static_cast<uint16_t>(first), static_cast<uint16_t>(atoms_.size())});
    }

    if (compiled && atoms_.size() <= max_atoms) {
        may_match_slash_ = std::any_of(atoms_.begin(), atoms_.end(), [](const atom& a) { return a.chars['/']; });
        catch_all_ = may_match_slash_ && atoms_.size() == 1 && groups_.size() == 1;
        return;
    }

    // Fall back to std::regex
    atoms_.clear();
    groups_.clear();
    regex_.emplace(key_);
    size_t group = 1;
    for (const auto& token : tokens) {
        if (token.parameter) {
            regex_groups_.push_back(group);
            group += 1 + capture_groups(token.regex);
            may_match_slash_ = may_match_slash_ || regex_may_match_slash(token.regex);
        } else {
            may_match_slash_ = may_match_slash_ || token.text.find('/') != std::string::npos;
        }
    }
    catch_all_ = false;
}

bool path_matcher::compile_regex(std::string_view regex, std::vector<atom>& atoms) {
    size_t i = 0;
    while (i < regex.size()) {
        atom a;
        char c = regex[i];
        if (c == '[') {
            if (!parse_class(regex, i, a.chars)) return false;
        } else if (c == '.') {
            a.chars.set();
            a.chars.reset('\n');
            a.chars.reset('\r');
            ++i;
        } else if (c == '\\') {
            if (i + 1 >= regex.size()) return false;
            char escaped = regex[i + 1];
            if (is_alnum(escaped)) {
                if (!class_escape(escaped, a.chars)) return false;
            } else {
                a.chars.set(static_cast<unsigned char>(escaped));
            }
            i += 2;
        } else if (c == '\0' || std::string_view("()|^$*+?{}]").find(c) != std::string_view::npos) {
            return false;
        } else {
            a.chars.set(static_cast<unsigned char>(c));
            ++i;
        }

        // Quantifier (greedy: lazy ones may change how captures split)
        if (i < regex.size()) {
            bool quantified = true;
            switch (regex[i]) {
                case '*': a.min = 0; a.max = unbounded; ++i; break;
                case '+': a.min = 1; a.max = unbounded; ++i; break;
                case '?': a.min = 0; a.max = 1; ++i; break;
                case '{':
                    if (!parse_braces(regex, i, a.min, a.max, unbounded)) return false;
                    break;
                default: quantified = false;
            }
            if (quantified && i < regex.size() && regex[i] == '?') return false;
        }

        if (atoms.size() >= max_atoms) return false;
        atoms.push_back(a);
    }
    return true;
}

bool path_matcher::match_atoms(size_t index, size_t position, std::string_view text, size_t* starts) const {
    if (index == atoms_.size()) return position == text.size();
    starts[index] = position;
    const auto& a = atoms_[index];

    // Longest run of the atom characters, then backtrack down to its minimum (greedy)
    size_t limit = std::min<size_t>(a.max, text.size() - position);
    size_t run = 0;
    while (run < limit && a.chars[static_cast<unsigned char>(text[position + run])]) ++run;
    if (run < a.min) return false;
    if (index + 1 == atoms_.size()) return position + run == text.size();
    for (size_t count = run + 1; count-- > a.min;) {
        if (match_atoms(index + 1, position + count, text, starts)) return true;
    }
    return false;
}

bool path_matcher::match(std::string_view text, route_captures& captures) const {
    if (words_) {
        if (std::find(words_->begin(), words_->end(), text) == words_->end()) return false;
        captures.push_back(text);
        return true;
    }

    if (regex_) {
        std::cmatch matches;
        if (!std::regex_match(text.data(), text.data() + text.size(), matches, *regex_)) return false;
        for (size_t group : regex_groups_) {
            const auto& capture = matches[group];
            captures.push_back(capture.matched ? std::string_view(capture.first, capture.length()) : std::string_view{});
        }
        return true;
    }

    // A single atom, as a parameter with a character class: check length and characters
    if (atoms_.size() == 1) {
        const auto& a = atoms_.front();
        if (text.size() < a.min || text.size() > a.max) return false;
        for (char c : text) {
            if (!a.chars[static_cast<unsigned char>(c)]) return false;
        }
        if (!groups_.empty()) captures.push_back(text);
        return true;
    }

    size_t starts[max_atoms + 1];
    if (!match_atoms(0, 0, text, starts)) return false;
    starts[atoms_.size()] = text.size();
    for (const auto& g : groups_) {
        captures.push_back(text.substr(starts[g.first], starts[g.last] - starts[g.first]));
    }
    return true;
}

bool path_matcher::may_overlap(const path_matcher& other) const {
    // Alternative words: whether the other matcher matches any of them
    route_captures captures;
    if (words_ || other.words_) {
        const auto& words = words_ ? *words_ : *other.words_;
        const auto& matcher = words_ ? other : *this;
        return std::any_of(words.begin(), words.end(), [&](const std::string& word) { return matcher.match(word, captures); });
    }
    if (regex_ || other.regex_) return true;

    // Intersection of both atom sequences as automata: states (atom, repetitions so far),
    // repetitions beyond the minimum of an unbounded atom being equivalent
    struct automaton {
        const std::vector<atom>& atoms;
        std::vector<size_t> offsets;  // first state of each atom; the last one accepts

        explicit automaton(const std::vector<atom>& a) : atoms(a) {
            size_t states = 0;
            for (const auto& item : atoms) {
                offsets.push_back(states);
                states += cap(item) + 1;
            }
            offsets.push_back(states);
        }
        static uint32_t cap(const atom& a) { return a.max == unbounded ? a.min : a.max; }
        size_t size() const { return offsets.back() + 1; }
        size_t accept() const { return offsets.back(); }
        std::pair<size_t, uint32_t> decode(size_t state) const {
            size_t index = std::upper_bound(offsets.begin(), offsets.end(), state) - offsets.begin() - 1;
            return {index, static_cast<uint32_t>(state - offsets[index])};
        }
    };

    automaton a(atoms_);
    automaton b(other.atoms_);
    if (a.size() * b.size() > (1u << 20)) return true;

    std::vector<bool> visited(a.size() * b.size());
    std::deque<std::pair<size_t, size_t>> pending;
    auto visit = [&](size_t x, size_t y) {
        if (visited[x * b.size() + y]) return;
        visited[x * b.size() + y] = true;
        pending.emplace_back(x, y);
    };
    visit(0, 0);
    while (!pending.empty()) {
        auto [x, y] = pending.front();
        pending.pop_front();
        bool x_accepts = x == a.accept();
        bool y_accepts = y == b.accept();
        if (x_accepts && y_accepts) return true;

        // Leave an atom once repeated its minimum
        if (!x_accepts) {
            auto [index, count] = a.decode(x);
            if (count >= a.atoms[index].min) visit(a.offsets[index + 1], y);
        }
        if (!y_accepts) {
            auto [index, count] = b.decode(y);
            if (count >= b.atoms[index].min) visit(x, b.offsets[index + 1]);
        }

        // Consume a character both atoms accept
        if (!x_accepts && !y_accepts) {
            auto [x_index, x_count] = a.decode(x);
            auto [y_index, y_count] = b.decode(y);
            const auto& x_atom = a.atoms[x_index];
            const auto& y_atom = b.atoms[y_index];
            bool x_more = x_atom.max == unbounded || x_count < x_atom.max;
            bool y_more = y_atom.max == unbounded || y_count < y_atom.max;
            if (x_more && y_more && (x_atom.chars & y_atom.chars).any()) {
                visit(a.offsets[x_index] + std::min(x_count + 1, automaton::cap(x_atom)),
                      b.offsets[y_index] + std::min(y_count + 1, automaton::cap(y_atom)));
            }
        }
    }
    return false;
}

} // namespace thinger::http::detail
