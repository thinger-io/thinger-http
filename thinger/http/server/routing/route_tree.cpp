#include "route_tree.hpp"
#include "route.hpp"
#include "../../../util/logger.hpp"
#include <algorithm>

namespace thinger::http {

using detail::path_matcher;
using detail::pattern_token;

namespace {

    using path_segment = std::vector<pattern_token>;

    // Parameter names, in order of their captures: empty for a repeated one
    std::vector<std::string> capture_names(const std::vector<pattern_token>& tokens) {
        std::vector<std::string> names;
        for (const auto& token : tokens) {
            if (!token.parameter) continue;
            bool repeated = std::find(names.begin(), names.end(), token.text) != names.end();
            names.push_back(repeated ? std::string{} : token.text);
        }
        return names;
    }

    // The pattern as segments: the text between slashes
    std::vector<path_segment> split_segments(const std::vector<pattern_token>& tokens) {
        std::vector<path_segment> segments(1);
        for (const auto& token : tokens) {
            if (token.parameter) {
                segments.back().push_back(token);
                continue;
            }
            for (size_t start = 0;;) {
                size_t slash = token.text.find('/', start);
                auto text = token.text.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
                if (!text.empty()) segments.back().push_back({false, std::move(text), {}});
                if (slash == std::string::npos) break;
                segments.emplace_back();
                start = slash + 1;
            }
        }
        return segments;
    }

    // The segments from `first` on, joined by slashes
    path_segment join_segments(const std::vector<path_segment>& segments, size_t first) {
        path_segment joined;
        for (size_t j = first; j < segments.size(); ++j) {
            if (j > first) joined.push_back({false, "/", {}});
            joined.insert(joined.end(), segments[j].begin(), segments[j].end());
        }
        return joined;
    }

    bool is_static(const path_segment& tokens) {
        return std::none_of(tokens.begin(), tokens.end(), [](const auto& token) { return token.parameter; });
    }

    void warn_repeat(const route& added, const route& existing, std::string_view method_name) {
        LOG_WARNING("Route {} {} repeats route {} {} and will never match", method_name,
                    added.get_pattern(), method_name, existing.get_pattern());
    }

}

struct route_tree::node {
    // A route matching the rest of the path from this node's segment on
    struct rest_route {
        std::unique_ptr<const path_matcher> matcher;
        leaf target;
    };

    // A child by a constrained segment, with the route that created it
    struct constrained_child {
        std::unique_ptr<const path_matcher> matcher;
        std::unique_ptr<node> child;
        const route* first;
    };

    // Children by the next segment, in order of priority
    std::map<std::string, std::unique_ptr<node>, std::less<>> statics;
    std::vector<constrained_child> constrained;  // in creation order
    std::unique_ptr<node> parameter;
    std::vector<rest_route> rest;  // catch-alls last

    // Route ending at this node
    std::unique_ptr<leaf> end;

    node* static_child(const std::string& text) {
        auto& child = statics[text];
        if (!child) child = std::make_unique<node>();
        return child.get();
    }

    node* parameter_child() {
        if (!parameter) parameter = std::make_unique<node>();
        return parameter.get();
    }

    // The same constraints (whatever the parameter names) share a node
    node* constrained_child_for(std::unique_ptr<const path_matcher> matcher, const route& target,
                                std::string_view method_name) {
        auto it = std::find_if(constrained.begin(), constrained.end(),
                               [&](const auto& entry) { return entry.matcher->key() == matcher->key(); });
        if (it == constrained.end()) {
            // Different constraints are tried in creation order
            for (const auto& sibling : constrained) {
                if (sibling.matcher->may_overlap(*matcher)) warn_overlap(target, *sibling.first, method_name);
            }
            constrained.push_back({std::move(matcher), std::make_unique<node>(), &target});
            it = std::prev(constrained.end());
        }
        return it->child.get();
    }

    void add_rest(std::unique_ptr<const path_matcher> matcher, leaf added, std::string_view method_name) {
        auto existing = std::find_if(rest.begin(), rest.end(),
                                     [&](const auto& r) { return r.matcher->key() == matcher->key(); });
        if (existing != rest.end()) {
            warn_repeat(*added.target, *existing->target.target, method_name);
            return;
        }
        // Wildcards of the same kind are tried in registration order
        for (const auto& r : rest) {
            if (r.matcher->is_catch_all() == matcher->is_catch_all() && r.matcher->may_overlap(*matcher)) {
                warn_overlap(*added.target, *r.target.target, method_name);
            }
        }

        // Catch-alls after the routes with more after their wildcard
        auto position = rest.end();
        if (!matcher->is_catch_all()) {
            position = std::find_if(rest.begin(), rest.end(), [](const auto& r) { return r.matcher->is_catch_all(); });
        }
        rest.insert(position, {std::move(matcher), std::move(added)});
    }

    void set_end(leaf added, std::string_view method_name) {
        if (end) {
            warn_repeat(*added.target, *end->target, method_name);
            return;
        }
        end = std::make_unique<leaf>(std::move(added));
    }
};

route_tree::route_tree() : root_(std::make_unique<node>()) {}
route_tree::~route_tree() = default;
route_tree::route_tree(route_tree&&) noexcept = default;
route_tree& route_tree::operator=(route_tree&&) noexcept = default;

void route_tree::insert(const route& target, std::string_view method_name) {
    auto tokens = detail::parse_route_pattern(target.get_pattern());
    auto names = capture_names(tokens);

    // A regex depending on its context is matched with the whole route on the whole path
    bool whole = std::any_of(tokens.begin(), tokens.end(), [](const auto& token) {
        return token.parameter && detail::regex_depends_on_context(token.regex);
    });
    auto segments = whole ? std::vector<path_segment>{tokens} : split_segments(tokens);

    node* current = root_.get();
    for (size_t i = 0; i < segments.size(); ++i) {
        const auto& segment = segments[i];
        if (is_static(segment)) {
            current = current->static_child(segment.empty() ? std::string{} : segment.front().text);
            continue;
        }
        if (segment.size() == 1 && segment.front().regex.empty()) {
            current = current->parameter_child();
            continue;
        }
        auto matcher = std::make_unique<const path_matcher>(segment);
        if (!whole && !matcher->may_match_slash()) {
            current = current->constrained_child_for(std::move(matcher), target, method_name);
            continue;
        }
        // A parameter that may match '/': the rest of the route matches the rest of the path
        current->add_rest(std::make_unique<const path_matcher>(join_segments(segments, i)),
                          leaf{&target, std::move(names)}, method_name);
        return;
    }
    current->set_end(leaf{&target, std::move(names)}, method_name);
}

void route_tree::warn_overlap(const route& added, const route& existing, std::string_view method_name) {
    LOG_WARNING("Route {} {} may overlap with route {} {}; the first registered takes precedence",
                method_name, added.get_pattern(), method_name, existing.get_pattern());
}

const route_tree::leaf* route_tree::find(std::string_view path, detail::route_captures& captures) const {
    return match(*root_, path, 0, captures);
}

const route_tree::leaf* route_tree::match(const node& current, std::string_view path, size_t position,
                                          detail::route_captures& captures) {
    size_t slash = path.find('/', position);
    bool last = slash == std::string_view::npos;
    auto segment = path.substr(position, last ? std::string_view::npos : slash - position);
    size_t mark = captures.size();

    auto descend = [&](const node& child) -> const leaf* {
        return last ? child.end.get() : match(child, path, slash + 1, captures);
    };

    if (!current.statics.empty()) {
        if (auto it = current.statics.find(segment); it != current.statics.end()) {
            if (auto* found = descend(*it->second)) return found;
        }
    }
    for (const auto& entry : current.constrained) {
        if (entry.matcher->match(segment, captures)) {
            if (auto* found = descend(*entry.child)) return found;
            captures.resize(mark);
        }
    }
    if (current.parameter && !segment.empty()) {
        captures.push_back(segment);
        if (auto* found = descend(*current.parameter)) return found;
        captures.resize(mark);
    }
    if (!current.rest.empty()) {
        auto rest = path.substr(position);
        for (const auto& r : current.rest) {
            if (r.matcher->match(rest, captures)) return &r.target;
        }
    }
    return nullptr;
}

} // namespace thinger::http
