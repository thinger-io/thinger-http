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
    // or if both may match the same paths and only the registration order decides
    // between them.
    void insert(const route& target, std::string_view method_name);

    // Route matching a path, appending its parameters to `captures`; null if none
    const leaf* find(std::string_view path, detail::route_captures& captures) const;

private:
    struct node;
    struct step;
    struct shape;

    std::unique_ptr<node> root_;
    // Path structure of every route, for the duplicate and ambiguity warnings
    std::vector<shape> shapes_;

    static const leaf* match(const node& current, std::string_view path, size_t position,
                             detail::route_captures& captures);
    void check_ambiguity(const shape& added, std::string_view method_name) const;
};

} // namespace thinger::http

#endif // THINGER_HTTP_ROUTE_TREE_HPP
