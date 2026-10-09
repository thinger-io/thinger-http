#include "route_tree.hpp"
#include "route.hpp"
#include "../../../util/logger.hpp"
#include <algorithm>

namespace thinger::http {

using detail::path_matcher;
using detail::pattern_token;

struct route_tree::node {
    // A route matching the rest of the path from this node's segment on
    struct rest_route {
        std::shared_ptr<const path_matcher> matcher;
        leaf target;
    };

    // A child by a constrained segment, tried in the order of `order` (when it was created)
    struct constrained_child {
        std::shared_ptr<const path_matcher> matcher;
        std::unique_ptr<node> child;
        size_t order;
    };

    // Children by the next segment, in order of priority
    std::map<std::string, std::unique_ptr<node>, std::less<>> statics;
    std::vector<constrained_child> constrained;
    std::unique_ptr<node> parameter;
    std::vector<rest_route> rest;  // catch-alls last

    // Route ending at this node
    std::unique_ptr<leaf> end;
};

// Path structure of a route: what matches each segment
struct route_tree::step {
    enum kind_type { fixed, constrained, parameter, rest } kind;
    std::string key;  // static text, or matcher key
    std::shared_ptr<const path_matcher> matcher;
    size_t order = 0;  // of the constrained node, among its siblings
};

struct route_tree::shape {
    const route* target;
    std::vector<step> steps;
};

namespace {

    // Whether some path segment may match both steps (true when it cannot be ruled out)
    template<typename step>
    bool segment_overlap(const step& a, const step& b) {
        if (a.kind == step::fixed && b.kind == step::fixed) return a.key == b.key;
        if (a.kind == step::fixed || b.kind == step::fixed) {
            const auto& text = a.kind == step::fixed ? a : b;
            const auto& other = a.kind == step::fixed ? b : a;
            if (other.kind == step::parameter) return !text.key.empty();
            detail::route_captures captures;
            return other.matcher->match(text.key, captures);
        }
        if (a.kind == step::constrained && b.kind == step::constrained && a.key != b.key) {
            return a.matcher->may_overlap(*b.matcher);
        }
        return true;
    }

    // Whether some path may match both routes, from step `from` on
    template<typename step>
    bool routes_overlap(const std::vector<step>& a, const std::vector<step>& b, size_t from) {
        for (size_t i = from;; ++i) {
            if (i == a.size() || i == b.size()) return i == a.size() && i == b.size();
            if (a[i].kind == step::rest && b[i].kind == step::rest) return a[i].matcher->may_overlap(*b[i].matcher);
            if (a[i].kind == step::rest || b[i].kind == step::rest) return true;
            if (!segment_overlap(a[i], b[i])) return false;
        }
    }

} // namespace

route_tree::route_tree() : root_(std::make_unique<node>()) {}
route_tree::~route_tree() = default;
route_tree::route_tree(route_tree&&) noexcept = default;
route_tree& route_tree::operator=(route_tree&&) noexcept = default;

void route_tree::insert(const route& target, std::string_view method_name) {
    auto tokens = detail::parse_route_pattern(target.get_pattern());

    // Parameter names, in order of their captures
    std::vector<std::string> names;
    for (const auto& token : tokens) {
        if (!token.parameter) continue;
        bool repeated = std::find(names.begin(), names.end(), token.text) != names.end();
        names.push_back(repeated ? std::string{} : token.text);
    }

    // Split the pattern into segments: the text between slashes
    std::vector<std::vector<pattern_token>> segments(1);
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

    // A regex depending on its context is matched with the whole route on the whole path
    bool whole = std::any_of(tokens.begin(), tokens.end(), [](const auto& token) {
        return token.parameter && detail::regex_depends_on_context(token.regex);
    });
    if (whole) segments = {tokens};

    shape added{&target, {}};
    node* current = root_.get();
    for (size_t i = 0; i < segments.size(); ++i) {
        const auto& segment = segments[i];

        // Static segment
        if (std::none_of(segment.begin(), segment.end(), [](const auto& token) { return token.parameter; })) {
            std::string text = segment.empty() ? std::string{} : segment.front().text;
            auto& child = current->statics[text];
            if (!child) child = std::make_unique<node>();
            added.steps.push_back({step::fixed, std::move(text), nullptr});
            current = child.get();
            continue;
        }

        // Parameter
        if (segment.size() == 1 && segment.front().regex.empty()) {
            if (!current->parameter) current->parameter = std::make_unique<node>();
            added.steps.push_back({step::parameter, {}, nullptr});
            current = current->parameter.get();
            continue;
        }

        // Constrained segment: the same constraints (whatever the parameter names) share a node
        auto matcher = std::make_shared<const path_matcher>(segment);
        if (!whole && !matcher->may_match_slash()) {
            auto it = std::find_if(current->constrained.begin(), current->constrained.end(),
                                   [&](const auto& entry) { return entry.matcher->key() == matcher->key(); });
            if (it == current->constrained.end()) {
                current->constrained.push_back({matcher, std::make_unique<node>(), current->constrained.size()});
                it = std::prev(current->constrained.end());
            }
            added.steps.push_back({step::constrained, matcher->key(), it->matcher, it->order});
            current = it->child.get();
            continue;
        }

        // A parameter that may match '/': the rest of the route matches the rest of the path
        std::vector<pattern_token> rest;
        for (size_t j = i; j < segments.size(); ++j) {
            if (j > i) rest.push_back({false, "/", {}});
            rest.insert(rest.end(), segments[j].begin(), segments[j].end());
        }
        auto rest_matcher = std::make_shared<const path_matcher>(rest);
        auto existing = std::find_if(current->rest.begin(), current->rest.end(),
                                     [&](const auto& r) { return r.matcher->key() == rest_matcher->key(); });
        if (existing != current->rest.end()) {
            LOG_WARNING("Route {} {} repeats route {} {} and will never match", method_name,
                        target.get_pattern(), method_name, existing->target.target->get_pattern());
            return;
        }
        added.steps.push_back({step::rest, rest_matcher->key(), rest_matcher});
        check_ambiguity(added, method_name);

        // Catch-alls after the routes with more after their wildcard
        auto position = current->rest.end();
        if (!rest_matcher->is_catch_all()) {
            position = std::find_if(current->rest.begin(), current->rest.end(),
                                    [](const auto& r) { return r.matcher->is_catch_all(); });
        }
        current->rest.insert(position, {rest_matcher, leaf{&target, std::move(names)}});
        shapes_.push_back(std::move(added));
        return;
    }

    if (current->end) {
        LOG_WARNING("Route {} {} repeats route {} {} and will never match", method_name,
                    target.get_pattern(), method_name, current->end->target->get_pattern());
        return;
    }
    check_ambiguity(added, method_name);
    current->end = std::make_unique<leaf>(leaf{&target, std::move(names)});
    shapes_.push_back(std::move(added));
}

void route_tree::check_ambiguity(const shape& added, std::string_view method_name) const {
    for (const auto& existing : shapes_) {
        const auto& a = existing.steps;
        const auto& b = added.steps;

        // Where the routes part in the tree decides which one is tried first
        size_t i = 0;
        while (i < a.size() && i < b.size() && a[i].kind == b[i].kind && a[i].key == b[i].key) ++i;
        if (i == a.size() || i == b.size()) continue;

        // Only the registration order decides between different constraints (the first
        // one registered there), and between wildcards of the same kind (the first route)
        bool by_order = a[i].kind == b[i].kind &&
                        (a[i].kind == step::constrained ||
                         (a[i].kind == step::rest && a[i].matcher->is_catch_all() == b[i].matcher->is_catch_all()));
        if (by_order && routes_overlap(a, b, i)) {
            bool existing_first = a[i].kind == step::rest || a[i].order < b[i].order;
            const auto& first = existing_first ? existing : added;
            const auto& second = existing_first ? added : existing;
            LOG_WARNING("Routes {} {} and {} {} may match the same paths: the first one takes precedence",
                        method_name, first.target->get_pattern(), method_name, second.target->get_pattern());
        }
    }
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
