#pragma once

// Self-contained Sugiyama-style layout for immutable revision DAGs.
//
// Stages: longest-path ranking, virtual vertices on long edges, alternating
// barycentric crossing reduction, constrained horizontal coordinate assignment
// using isotonic regression (no card overlap), and cubic edge routing through
// the virtual lanes. All identifiers are sorted before layout, so catalogue
// fetch order has no effect. Coordinates are top-down, in pixels.
//
// Original implementation: no Qt, Graphviz, third-party code or processes.
// Usable for arbitrary fixed-size-card DAGs elsewhere in the GUI.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AowisGraphLayout
{
struct LayeredVertex
{
    std::string id;
    std::vector<std::string> parents;
    double width = 224.0;
    double height = 84.0;
};

struct LayeredRoute
{
    std::string parent;
    std::vector<std::pair<double, double>> cubic; // P0, C1, C2, P3[, C1, C2, P3...]
};

struct LayeredDag
{
    bool valid = false;
    std::vector<std::pair<double, double>> positions; // Input order, card centers.
    std::vector<int> ranks;
    std::vector<std::vector<LayeredRoute>> parent_routes; // Input order.
    double width = 0.0;
    double height = 0.0;
};

inline LayeredDag layoutLayeredDag(const std::vector<LayeredVertex> &input,
                                   double node_gap = 48.0,
                                   double rank_gap = 80.0)
{
    LayeredDag output;
    const int count = static_cast<int>(input.size());
    output.positions.resize(input.size());
    output.ranks.resize(input.size());
    output.parent_routes.resize(input.size());
    if (!std::isfinite(node_gap) || node_gap < 0.0
        || !std::isfinite(rank_gap) || rank_gap < 1.0)
        return output;
    if (count == 0)
    {
        output.valid = true;
        return output;
    }

    std::vector<int> sorted(count);
    std::iota(sorted.begin(), sorted.end(), 0);
    std::sort(sorted.begin(), sorted.end(), [&](int a, int b)
    {
        return input[a].id < input[b].id;
    });
    std::unordered_map<std::string, int> lookup;
    lookup.reserve(input.size());
    for (int i = 0; i < count; ++i)
    {
        const LayeredVertex &vertex = input[sorted[i]];
        if (vertex.id.empty() || !std::isfinite(vertex.width)
            || !std::isfinite(vertex.height) || vertex.width <= 0.0
            || vertex.height <= 0.0 || !lookup.emplace(vertex.id, i).second)
            return output;
    }

    // All internal indexes refer to the stable sorted order, not input order.
    std::vector<std::vector<int>> parents(count), children(count);
    for (int i = 0; i < count; ++i)
    {
        for (const std::string &id : input[sorted[i]].parents)
        {
            const std::unordered_map<std::string, int>::const_iterator found = lookup.find(id);
            if (found != lookup.end())
            {
                if (found->second == i) return output;
                parents[i].push_back(found->second);
            }
        }
        std::sort(parents[i].begin(), parents[i].end());
        parents[i].erase(std::unique(parents[i].begin(), parents[i].end()), parents[i].end());
        for (int parent : parents[i])
            children[parent].push_back(i);
    }

    // Kahn topological order rejects corrupt cycles instead of silently
    // inventing ancestry directions. Longest-path ranks include both parents
    // of merges (not just their first-parent history).
    std::vector<int> indegree(count), ranks(count, 0);
    for (int i = 0; i < count; ++i)
        indegree[i] = static_cast<int>(parents[i].size());
    std::vector<int> ready;
    for (int i = 0; i < count; ++i)
        if (indegree[i] == 0) ready.push_back(i);
    int processed = 0;
    while (!ready.empty())
    {
        const int node = ready.back();
        ready.pop_back();
        ++processed;
        for (int child : children[node])
        {
            ranks[child] = std::max(ranks[child], ranks[node] + 1);
            if (--indegree[child] == 0)
                ready.push_back(child);
        }
    }
    if (processed != count)
        return output;
    const int maximum_rank = *std::max_element(ranks.begin(), ranks.end());

    struct Node
    {
        int rank = 0;
        int original = -1; // -1 means virtual long-edge routing vertex.
        double width = 18.0;
        double height = 0.0;
        double x = 0.0;
        double y = 0.0;
        std::vector<int> previous;
        std::vector<int> next;
    };
    std::vector<Node> graph;
    graph.reserve(input.size());
    std::vector<std::vector<int>> layers(maximum_rank + 1);
    for (int i = 0; i < count; ++i)
    {
        Node node;
        node.rank = ranks[i];
        node.original = i;
        node.width = input[sorted[i]].width;
        node.height = input[sorted[i]].height;
        graph.push_back(node);
        layers[ranks[i]].push_back(i);
    }

    struct Edge
    {
        int parent = -1;
        int child = -1;
        std::vector<int> path;
    };
    std::vector<Edge> edges;
    for (int child = 0; child < count; ++child)
        for (int parent : parents[child])
        {
            Edge edge;
            edge.parent = parent;
            edge.child = child;
            edge.path.push_back(parent);
            for (int rank = ranks[parent] + 1; rank < ranks[child]; ++rank)
            {
                Node virtual_node;
                virtual_node.rank = rank;
                const int index = static_cast<int>(graph.size());
                graph.push_back(virtual_node);
                layers[rank].push_back(index);
                edge.path.push_back(index);
            }
            edge.path.push_back(child);
            for (std::size_t i = 1; i < edge.path.size(); ++i)
            {
                const int before = edge.path[i - 1];
                const int after = edge.path[i];
                graph[before].next.push_back(after);
                graph[after].previous.push_back(before);
            }
            edges.push_back(edge);
        }

    std::vector<double> order(graph.size(), 0.0);
    // Updating the order for every rank after sorting one rank would perform
    // O(number_of_ranks * number_of_vertices) work per sweep. In a long
    // revision chain each rank has one node, so that becomes O(N^2) even
    // though the local sort changes at most one slot. Only the sorted rank's
    // order values can have changed.
    for (const std::vector<int> &layer : layers)
        for (int slot = 0; slot < static_cast<int>(layer.size()); ++slot)
            order[layer[slot]] = static_cast<double>(slot);

    // Count inversions between adjacent ranks using a Fenwick tree. Edges
    // with a common source or target do not cross. Complexity is O(E log V)
    // per sweep rather than quadratic in the width of a busy branch layer.
    const auto countCrossings = [&]() -> std::size_t
    {
        std::size_t crossings = 0;
        for (int rank = 0; rank < maximum_rank; ++rank)
        {
            std::vector<std::pair<int, int>> segments;
            for (int index : layers[rank])
                for (int next : graph[index].next)
                    segments.emplace_back(static_cast<int>(order[index]),
                                          static_cast<int>(order[next]));
            std::sort(segments.begin(), segments.end());
            std::vector<std::size_t> bit(layers[rank + 1].size() + 1, 0);
            const auto prefixSum = [&](int endpoint) -> std::size_t
            {
                std::size_t result = 0;
                for (int i = endpoint; i > 0; i -= i & -i)
                    result += bit[std::size_t(i)];
                return result;
            };
            std::size_t preceding = 0;
            for (std::size_t first = 0; first < segments.size();)
            {
                std::size_t last = first;
                while (last < segments.size()
                    && segments[last].first == segments[first].first)
                    ++last;
                for (std::size_t i = first; i < last; ++i)
                    crossings += preceding - prefixSum(segments[i].second + 1);
                for (std::size_t i = first; i < last; ++i)
                    for (int j = segments[i].second + 1;
                         j < static_cast<int>(bit.size()); j += j & -j)
                        ++bit[std::size_t(j)];
                preceding += last - first;
                first = last;
            }
        }
        return crossings;
    };

    std::vector<std::vector<int>> best_layers = layers;
    std::size_t best_crossings = countCrossings();
    // Zero is the global optimum. No barycentric sorting can improve it;
    // skipping those sweeps is especially valuable for long linear histories.
    for (int round = 0; round < 18 && best_crossings != 0; ++round)
    {
        for (int pass = 0; pass < 2; ++pass)
        {
            const bool downward = pass == 0;
            for (int rank_offset = 1; rank_offset <= maximum_rank; ++rank_offset)
            {
                const int rank = downward ? rank_offset : maximum_rank - rank_offset;
                std::vector<int> &layer = layers[rank];
                std::vector<std::pair<double, int>> scores;
                scores.reserve(layer.size());
                for (int node : layer)
                {
                    const std::vector<int> &adjacent = downward
                        ? graph[node].previous : graph[node].next;
                    double score = order[node];
                    if (!adjacent.empty())
                    {
                        score = 0.0;
                        for (int neighbor : adjacent) score += order[neighbor];
                        score /= static_cast<double>(adjacent.size());
                    }
                    scores.emplace_back(score, node);
                }
                std::stable_sort(scores.begin(), scores.end(), [](const std::pair<double, int> &a,
                                                                    const std::pair<double, int> &b)
                {
                    return a.first < b.first;
                });
                for (int slot = 0; slot < static_cast<int>(scores.size()); ++slot)
                    layer[slot] = scores[slot].second;
                for (int slot = 0; slot < static_cast<int>(layer.size()); ++slot)
                    order[layer[slot]] = static_cast<double>(slot);
            }
            const std::size_t crossings = countCrossings();
            if (crossings < best_crossings)
            {
                best_crossings = crossings;
                best_layers = layers;
            }
        }
    }
    layers = best_layers;
    for (const std::vector<int> &layer : layers)
        for (int slot = 0; slot < static_cast<int>(layer.size()); ++slot)
            order[layer[slot]] = static_cast<double>(slot);

    std::vector<double> rank_heights(layers.size(), 0.0);
    for (std::size_t rank = 0; rank < layers.size(); ++rank)
        for (int node : layers[rank])
            rank_heights[rank] = std::max(rank_heights[rank], graph[node].height);
    double vertical = 0.0;
    for (int rank = 0; rank <= maximum_rank; ++rank)
    {
        vertical += rank_heights[rank] / 2.0;
        for (int node : layers[rank]) graph[node].y = vertical;
        vertical += rank_heights[rank] / 2.0 + rank_gap;
    }

    // Start every rank with a compact, centered, non-overlapping packing.
    // Virtual vertices reserve a genuine routing lane through intermediate
    // ranks, so a long edge does not pass through an unrelated card.
    for (const std::vector<int> &layer : layers)
    {
        double cursor = 0.0;
        for (int node : layer)
        {
            cursor += graph[node].width / 2.0;
            graph[node].x = cursor;
            cursor += graph[node].width / 2.0 + node_gap;
        }
        const double span = cursor - node_gap;
        for (int node : layer) graph[node].x -= span / 2.0;
    }

    // Project desired barycenters onto non-overlapping node coordinates.
    // Pool-adjacent-violators is the exact least-squares projection for the
    // order constraints x[i+1]-x[i] >= (w[i]+w[i+1])/2 + node_gap.
    // Repeating up/down sweeps aligns forks, merges and long dummy chains.
    const auto relaxLayer = [&](const std::vector<int> &layer, bool downward)
    {
        struct Pool { int begin; int end; double sum; int count; };
        std::vector<Pool> pools;
        double prefix = 0.0;
        std::vector<double> prefixes(layer.size(), 0.0);
        for (int slot = 0; slot < static_cast<int>(layer.size()); ++slot)
        {
            const int node = layer[slot];
            if (slot > 0)
                prefix += (graph[layer[slot - 1]].width + graph[node].width) / 2.0 + node_gap;
            prefixes[slot] = prefix;
            const std::vector<int> &adjacent = downward
                ? graph[node].previous : graph[node].next;
            double desired = graph[node].x;
            if (!adjacent.empty())
            {
                desired = 0.0;
                for (int neighbor : adjacent) desired += graph[neighbor].x;
                desired /= static_cast<double>(adjacent.size());
                desired = desired * 0.80 + graph[node].x * 0.20;
            }
            pools.push_back({slot, slot, desired - prefix, 1});
            while (pools.size() >= 2)
            {
                Pool &right = pools.back();
                Pool &left = pools[pools.size() - 2];
                if (left.sum / left.count <= right.sum / right.count)
                    break;
                left.end = right.end;
                left.sum += right.sum;
                left.count += right.count;
                pools.pop_back();
            }
        }
        for (const Pool &pool : pools)
            for (int slot = pool.begin; slot <= pool.end; ++slot)
                graph[layer[slot]].x = pool.sum / pool.count + prefixes[slot];
    };
    for (int iteration = 0; iteration < 30; ++iteration)
    {
        for (int rank = 1; rank <= maximum_rank; ++rank)
            relaxLayer(layers[rank], true);
        for (int rank = maximum_rank - 1; rank >= 0; --rank)
            relaxLayer(layers[rank], false);
    }

    double left = std::numeric_limits<double>::infinity();
    double right = -std::numeric_limits<double>::infinity();
    double top = std::numeric_limits<double>::infinity();
    double bottom = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < count; ++i)
    {
        const Node &node = graph[i];
        left = std::min(left, node.x - node.width / 2.0);
        right = std::max(right, node.x + node.width / 2.0);
        top = std::min(top, node.y - node.height / 2.0);
        bottom = std::max(bottom, node.y + node.height / 2.0);
    }

    // A route changes x ONLY in the empty gap between adjacent ranks.
    // Virtual routing vertices get a reserved unobstructed vertical lane
    // through intermediate ranks. A simple center-to-center Bezier would
    // intersect tall cards when it crossed a populated intermediate layer.
    const auto appendCubic = [](LayeredRoute &route, double ax, double ay,
                                double bx, double by)
    {
        if (route.cubic.empty())
            route.cubic.emplace_back(ax, ay);
        if (std::abs(ax - bx) < 1e-9)
        {
            route.cubic.emplace_back(ax, ay + (by - ay) / 3.0);
            route.cubic.emplace_back(ax, ay + 2.0 * (by - ay) / 3.0);
        }
        else
        {
            route.cubic.emplace_back(ax, ay + (by - ay) * 0.40);
            route.cubic.emplace_back(bx, by - (by - ay) * 0.40);
        }
        route.cubic.emplace_back(bx, by);
    };
    for (const Edge &edge : edges)
    {
        LayeredRoute route;
        route.parent = input[sorted[edge.parent]].id;
        const Node &source = graph[edge.parent];
        double route_x = source.x;
        double route_y = source.y + source.height / 2.0;
        for (std::size_t segment = 1; segment < edge.path.size(); ++segment)
        {
            const Node &before = graph[edge.path[segment - 1]];
            const Node &after = graph[edge.path[segment]];
            const double previous_bottom = before.y + rank_heights[before.rank] / 2.0;
            const double next_top = after.y - rank_heights[after.rank] / 2.0;
            if (route_y < previous_bottom - 1e-8)
                appendCubic(route, route_x, route_y, route_x, previous_bottom);
            appendCubic(route, route_x, previous_bottom, after.x, next_top);
            route_x = after.x;
            route_y = next_top;
            const double stop = after.original >= 0
                ? after.y - after.height / 2.0
                : after.y + rank_heights[after.rank] / 2.0;
            if (route_y < stop - 1e-8)
                appendCubic(route, route_x, route_y, route_x, stop);
            route_y = stop;
        }
        for (const std::pair<double, double> &point : route.cubic)
        {
            left = std::min(left, point.first);
            right = std::max(right, point.first);
            top = std::min(top, point.second);
            bottom = std::max(bottom, point.second);
        }
        output.parent_routes[sorted[edge.child]].push_back(route);
    }
    if (!std::isfinite(left) || !std::isfinite(right) || !std::isfinite(top)
        || !std::isfinite(bottom) || right <= left || bottom <= top)
        return output;

    for (int i = 0; i < count; ++i)
    {
        output.positions[sorted[i]] = {graph[i].x - left, graph[i].y - top};
        output.ranks[sorted[i]] = graph[i].rank;
    }
    for (std::vector<LayeredRoute> &routes : output.parent_routes)
        for (LayeredRoute &route : routes)
            for (std::pair<double, double> &point : route.cubic)
            {
                point.first -= left;
                point.second -= top;
            }
    output.width = right - left;
    output.height = bottom - top;
    output.valid = true;
    return output;
}
}
