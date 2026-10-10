#ifndef THINGER_HTTP_ROUTE_TREE_HPP
#define THINGER_HTTP_ROUTE_TREE_HPP

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "route_pattern.hpp"

namespace thinger::http {

class route;

// Routes of a method, as a tree of path segments (the text between slashes), so finding
// the route of a path costs about the same whatever the number of routes. A segment of a
// path is matched, in this order of priority:
//   1. a static segment ("/users")
//   2. a segment with a regex constraint (":id([0-9]+)"), or mixing text and parameters
//      (":name.:ext"): the routes with the same constraint at the same position share a
//      node, and different constraints are tried in the order they were first registered
//   3. a parameter (":id"), matching any non-empty segment
//   4. a parameter that may match '/' (":path(.+)"), matching the rest of the path with
//      the rest of its route: those with more after it ("/:path(.+)/raw") first, then
//      the lone ones (catch-alls), in registration order among each
// independently of the registration order otherwise. If the rest of the path does not
// match below the chosen option, the next one is tried, so a path matches some route
// whenever any route matches it. A route with a parameter regex depending on its context
// (anchors, word boundaries, lookaheads, back-references) is matched as a whole, on the
// whole path, after everything else.
class route_tree {
public:
    // Route found for a path, with the parameter names of its captures (an empty name
    // for a repeated parameter, whose value is the first one)
    struct leaf {
        const route* target;
        std::vector<std::string> names;
    };

    route_tree();
    ~route_tree();
    route_tree(route_tree&&) noexcept;
    route_tree& operator=(route_tree&&) noexcept;

    // Add a route (by reference: it must outlive the tree). Logs a warning if it repeats
    // the path structure of another route (it would never match: the first one wins),
    // or if it adds a constraint (or wildcard) next to another one that may match the
    // same segments (a cheap check: they may start with the same character), where the
    // registration order decides.
    void insert(const route& target, std::string_view method_name);

    // Route matching a path, appending its parameters to `captures`; null if none
    const leaf* find(std::string_view path, detail::route_captures& captures) const;

private:
    struct node;

    std::unique_ptr<node> root_;

    static const leaf* match(const node& current, std::string_view path, size_t position,
                             detail::route_captures& captures);
    static void warn_overlap(const route& added, const route& existing, std::string_view method_name);
};

} // namespace thinger::http

#endif // THINGER_HTTP_ROUTE_TREE_HPP
