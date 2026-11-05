#include "graph/layered_dag_layout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <random>
#include <unordered_map>
#include <vector>

using namespace AowisGraphLayout;

#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "layered_dag_layout failed at line %d: %s\n", __LINE__, #expression); \
    return 1; } } while (false)

static bool same(const LayeredDag &lhs, const LayeredDag &rhs)
{
    return lhs.valid && rhs.valid && lhs.positions == rhs.positions && lhs.ranks == rhs.ranks
        && lhs.width == rhs.width && lhs.height == rhs.height;
}

static bool checkGeometry(const std::vector<LayeredVertex> &vertices, const LayeredDag &layout)
{
    if (!layout.valid || layout.positions.size() != vertices.size()
        || layout.parent_routes.size() != vertices.size()) return false;
    std::unordered_map<std::string, std::size_t> lookup;
    for (std::size_t i = 0; i < vertices.size(); ++i)
    {
        lookup.emplace(vertices[i].id, i);
        const std::pair<double, double> p = layout.positions[i];
        if (!std::isfinite(p.first) || !std::isfinite(p.second)
            || p.first < vertices[i].width / 2.0 - 1e-6
            || p.second < vertices[i].height / 2.0 - 1e-6
            || p.first + vertices[i].width / 2.0 > layout.width + 1e-6
            || p.second + vertices[i].height / 2.0 > layout.height + 1e-6)
            return false;
        for (std::size_t j = i + 1; j < vertices.size(); ++j)
        {
            const double dx = std::abs(p.first - layout.positions[j].first);
            const double dy = std::abs(p.second - layout.positions[j].second);
            if (dx < (vertices[i].width + vertices[j].width) / 2.0 - 1e-6
                && dy < (vertices[i].height + vertices[j].height) / 2.0 - 1e-6)
                return false;
        }
    }
    for (std::size_t i = 0; i < vertices.size(); ++i)
    {
        int loaded = 0;
        for (const std::string &parent : vertices[i].parents)
        {
            const std::unordered_map<std::string, std::size_t>::const_iterator found = lookup.find(parent);
            if (found == lookup.end()) continue;
            ++loaded;
            if (!(layout.ranks[found->second] < layout.ranks[i]
                  && layout.positions[found->second].second < layout.positions[i].second)) return false;
            const std::vector<LayeredRoute> &routes = layout.parent_routes[i];
            const std::vector<LayeredRoute>::const_iterator route =
                std::find_if(routes.begin(), routes.end(), [&](const LayeredRoute &candidate)
                { return candidate.parent == parent; });
            if (route == routes.end() || route->cubic.size() < 4
                || (route->cubic.size() - 1) % 3 != 0) return false;
            const std::pair<double, double> first = route->cubic.front();
            const std::pair<double, double> last = route->cubic.back();
            if (std::abs(first.first - layout.positions[found->second].first) > 1e-6
                || std::abs(last.first - layout.positions[i].first) > 1e-6
                || std::abs(first.second - layout.positions[found->second].second
                            - vertices[found->second].height / 2.0) > 1e-6
                || std::abs(last.second - layout.positions[i].second
                            + vertices[i].height / 2.0) > 1e-6)
                return false;
            for (const std::pair<double, double> &point : route->cubic)
                if (!std::isfinite(point.first) || !std::isfinite(point.second)
                    || point.first < -1e-5 || point.first > layout.width + 1e-5
                    || point.second < -1e-5 || point.second > layout.height + 1e-5)
                    return false;
            // Sample the actual Bezier strokes, not just node centers. All
            // transitions must stay out of unrelated revision cards, even
            // when a long edge crosses several populated intermediate ranks.
            for (std::size_t segment = 1; segment + 2 < route->cubic.size(); segment += 3)
            {
                const std::pair<double, double> a = route->cubic[segment - 1];
                const std::pair<double, double> b = route->cubic[segment];
                const std::pair<double, double> c = route->cubic[segment + 1];
                const std::pair<double, double> d = route->cubic[segment + 2];
                for (int step = 1; step < 20; ++step)
                {
                    const double t = step / 20.0;
                    const double u = 1.0 - t;
                    const double x = u*u*u*a.first + 3*u*u*t*b.first
                        + 3*u*t*t*c.first + t*t*t*d.first;
                    const double y = u*u*u*a.second + 3*u*u*t*b.second
                        + 3*u*t*t*c.second + t*t*t*d.second;
                    for (std::size_t card = 0; card < vertices.size(); ++card)
                    {
                        if (card == i || card == found->second) continue;
                        if (std::abs(x - layout.positions[card].first)
                                < vertices[card].width / 2.0 - 1.0
                            && std::abs(y - layout.positions[card].second)
                                < vertices[card].height / 2.0 - 1.0)
                            return false;
                    }
                }
            }
        }
        if (static_cast<int>(layout.parent_routes[i].size()) != loaded) return false;
    }
    return true;
}

int main()
{
    const std::vector<LayeredVertex> empty;
    CHECK(layoutLayeredDag(empty).valid);
    CHECK(!layoutLayeredDag({{"same", {}}, {"same", {}}}).valid);
    CHECK(!layoutLayeredDag({{"self", {"other"}}, {"other", {"self"}}}).valid);
    CHECK(!layoutLayeredDag({{"invalid", {}, 0, 12}}).valid);

    // Only the root reference connects to the revision. Each fork has one
    // incoming visual edge from its persisted creation-source branch.
    const std::vector<LayeredVertex> branch_origins = {
        {"revision", {}, 224, 90},
        {"main", {"revision"}, 196, 70},
        {"feature", {"main"}, 196, 70},
        {"child", {"feature"}, 196, 70}
    };
    const LayeredDag branch_layout = layoutLayeredDag(branch_origins);
    CHECK(branch_layout.valid);
    CHECK(branch_layout.ranks[2] > branch_layout.ranks[1]);
    CHECK(branch_layout.ranks[3] > branch_layout.ranks[2]);
    CHECK(checkGeometry(branch_origins, branch_layout));

    const std::vector<LayeredVertex> linear = {
        {"oldest", {}, 224, 84},
        {"older", {"oldest"}, 224, 84},
        {"newer", {"older"}, 224, 132},
        {"current", {"newer"}, 224, 228}
    };
    const LayeredDag chain = layoutLayeredDag(linear);
    CHECK(checkGeometry(linear, chain));
    CHECK(chain.width >= 224.0 && chain.width < 224.01);
    for (std::size_t i = 1; i < linear.size(); ++i)
    {
        CHECK(chain.positions[i].first == chain.positions[0].first);
        CHECK(chain.ranks[i] == static_cast<int>(i));
    }

    // Non-leaf main HEAD and feature HEAD point to different immutable nodes.
    // A shared HEAD is simply two refs on one card, i.e. a larger node.
    const std::vector<LayeredVertex> non_leaf = {
        {"root", {}, 224, 84}, {"main-head", {"root"}, 224, 132},
        {"feature-head", {"main-head"}, 224, 108}
    };
    CHECK(checkGeometry(non_leaf, layoutLayeredDag(non_leaf)));

    const std::vector<LayeredVertex> fork_merge = {
        {"root", {}, 224, 84},
        {"main1", {"root"}, 224, 108},
        {"feature1", {"root"}, 224, 84},
        {"feature2", {"feature1"}, 224, 180},
        {"main2", {"main1"}, 224, 84},
        {"merge", {"main2", "feature2"}, 224, 228},
        {"topic", {"root"}, 224, 84},
        {"long-edge", {"root", "merge"}, 224, 108}
    };
    // Barycentric crossing reduction must uncross independent relationships
    // even when lexical vertex ordering would have made an X.
    const std::vector<LayeredVertex> uncross = {
        {"parent-a", {}, 224, 84}, {"parent-b", {}, 224, 84},
        {"child-a", {"parent-b"}, 224, 84},
        {"child-b", {"parent-a"}, 224, 84}
    };
    const LayeredDag uncrossed = layoutLayeredDag(uncross);
    CHECK(checkGeometry(uncross, uncrossed));
    CHECK(uncrossed.positions[2].first > uncrossed.positions[3].first);

    const LayeredDag dag = layoutLayeredDag(fork_merge);
    CHECK(checkGeometry(fork_merge, dag));
    CHECK(dag.width >= 3 * 224.0);
    std::vector<LayeredVertex> shuffled = fork_merge;
    std::reverse(shuffled.begin(), shuffled.end());
    const LayeredDag shuffled_dag = layoutLayeredDag(shuffled);
    CHECK(checkGeometry(shuffled, shuffled_dag));
    for (std::size_t i = 0; i < fork_merge.size(); ++i)
        for (std::size_t j = 0; j < shuffled.size(); ++j)
            if (fork_merge[i].id == shuffled[j].id)
            {
                CHECK(dag.positions[i] == shuffled_dag.positions[j]);
                CHECK(dag.ranks[i] == shuffled_dag.ranks[j]);
                CHECK(dag.parent_routes[i].size() == shuffled_dag.parent_routes[j].size());
            }
    CHECK(same(dag, layoutLayeredDag(fork_merge)));

    std::vector<LayeredVertex> large;
    large.push_back({"root", {}, 224, 84});
    for (int i = 0; i < 24; ++i)
        large.push_back({"branch-" + std::to_string(i), {"root"}, 224,
                         i % 3 == 0 ? 228.0 : 108.0});
    for (int i = 0; i < 12; ++i)
        large.push_back({"merge-" + std::to_string(i),
                         {"branch-" + std::to_string(2 * i),
                          "branch-" + std::to_string(2 * i + 1)},
                         224, i % 2 == 0 ? 132.0 : 84.0});
    large.push_back({"last", {"root", "merge-0", "merge-11"}, 224, 276});
    CHECK(checkGeometry(large, layoutLayeredDag(large)));

    // Fixed-seed generated revision histories: malformed geometry should
    // fail the test reproducibly, never depend on machine or iteration order.
    std::mt19937 generator(12);
    for (int scenario = 0; scenario < 30; ++scenario)
    {
        std::vector<LayeredVertex> generated;
        for (int revision = 0; revision < 50; ++revision)
        {
            LayeredVertex vertex;
            vertex.id = "rev-" + std::to_string(1000 + revision);
            vertex.height = 84.0 + 24.0 * (revision % 5);
            if (revision > 0)
            {
                const int parent = std::uniform_int_distribution<int>(
                    std::max(0, revision - 8), revision - 1)(generator);
                vertex.parents.push_back(generated[std::size_t(parent)].id);
                if (revision > 10 && revision % 4 == 0)
                {
                    const int second = std::uniform_int_distribution<int>(
                        std::max(0, revision - 12), revision - 2)(generator);
                    if (generated[std::size_t(second)].id != vertex.parents.front())
                        vertex.parents.push_back(generated[std::size_t(second)].id);
                }
            }
            generated.push_back(vertex);
        }
        CHECK(checkGeometry(generated, layoutLayeredDag(generated)));
    }

    // A tall project may have tens of thousands of successive revisions.
    // Barycentric updates must touch the modified rank only; a full-layer
    // scan inside the per-rank loop used to make this quadratic. The large
    // case is intentionally bounded and does not run full geometry sampling.
    std::vector<LayeredVertex> long_history;
    long_history.reserve(10000);
    for (int revision = 0; revision < 10000; ++revision)
    {
        LayeredVertex vertex;
        vertex.id = "long-" + std::to_string(revision);
        if (revision != 0)
            vertex.parents.push_back(long_history.back().id);
        long_history.push_back(vertex);
    }
    const LayeredDag long_layout = layoutLayeredDag(long_history);
    CHECK(long_layout.valid && long_layout.ranks.back() == 9999);
    CHECK(long_layout.parent_routes.back().size() == 1);
    CHECK(long_layout.width >= 224.0 && long_layout.width < 224.01);

    std::printf("Native layered DAG layout: geometry, forks, merges, long edges, card sizes and order passed\n");
}
