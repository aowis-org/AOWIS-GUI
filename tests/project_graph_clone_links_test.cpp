// Dashed links between clones: project cards on different databases with the
// same DVMDB project UUIDv7, chained in reading order, with side or vertical
// ports depending on where the cards are.
#include "tabs/project_graph_clone_links.h"

#include <QRectF>
#include <QVector>

#include <cmath>
#include <cstdio>

namespace
{
int failures = 0;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        std::fprintf(stderr, "project_graph_clone_links: %s\n", message);
        ++failures;
    }
}

bool near(const QPointF &a, const QPointF &b)
{
    return std::fabs(a.x() - b.x()) < 1e-6 && std::fabs(a.y() - b.y()) < 1e-6;
}

ProjectGraphNode projectCard(const QString &id, const QString &server, const QPointF &position,
                             const QString &identity)
{
    ProjectGraphNode node;
    node.kind = ProjectGraphNode::Kind::Project;
    node.id = id;
    node.container_id = server;
    node.position = position;
    node.clone_identity = identity;
    return node;
}

QRectF bounds(const ProjectGraphNode &node)
{
    return QRectF(node.position.x() - 100.0, node.position.y() - 30.0, 200.0, 60.0);
}

const QString first_uuid = QStringLiteral("019aabbc-2222-7222-8222-123456789abc");
const QString second_uuid = QStringLiteral("019aabbc-3333-7333-8333-123456789abc");

// A revision card as the Projects widget builds it: "<project>|<revision>".
ProjectGraphNode revisionCard(const QString &project_card, const QString &project, const QString &revision,
                              const QStringList &parents, const QPointF &position)
{
    ProjectGraphNode node;
    node.kind = ProjectGraphNode::Kind::Revision;
    node.id = project + QLatin1Char('|') + revision;
    node.display_id = revision;
    node.project_group_id = project_card;
    node.position = position;
    for (const QString &parent : parents) node.parents.append(project + QLatin1Char('|') + parent);
    return node;
}

// Appends one copy of a project: its card and a history given as
// "revision:parent,parent" entries.
void addCopy(QVector<ProjectGraphNode> &nodes, const QString &card, const QString &identity, double x,
             const QStringList &history)
{
    nodes.append(projectCard(card, QStringLiteral("server-") + card, QPointF(x, 0), identity));
    double y = 150;
    for (const QString &entry : history)
    {
        const QString revision = entry.section(QLatin1Char(':'), 0, 0);
        const QString parents = entry.section(QLatin1Char(':'), 1);
        nodes.append(revisionCard(card, QStringLiteral("p-") + card, revision,
                                  parents.split(QLatin1Char(','), Qt::SkipEmptyParts), QPointF(x, y)));
        y += 120;
    }
}

QStringList syncedRevisions(const QVector<ProjectGraphNode> &nodes)
{
    QStringList result;
    for (const ProjectGraphCloneLinks::Link &link : ProjectGraphCloneLinks::computeSync(nodes, bounds))
    {
        const ProjectGraphNode &first = nodes.at(link.first);
        const ProjectGraphNode &second = nodes.at(link.second);
        if (first.display_id != second.display_id || first.project_group_id == second.project_group_id)
            return {QStringLiteral("invalid link")};
        result.append(first.display_id);
    }
    result.sort();
    return result;
}

void testSyncFrontiers()
{
    // One copy is behind: the line ends at the last commit both have.
    {
        QVector<ProjectGraphNode> nodes;
        addCopy(nodes, QStringLiteral("a"), first_uuid, 0,
                {QStringLiteral("r1:"), QStringLiteral("r2:r1"), QStringLiteral("r3:r2"),
                 QStringLiteral("r4:r3"), QStringLiteral("r5:r4")});
        addCopy(nodes, QStringLiteral("b"), first_uuid, 600,
                {QStringLiteral("r1:"), QStringLiteral("r2:r1"), QStringLiteral("r3:r2")});
        ProjectGraphCloneLinks::markSyncFrontiers(nodes);
        require(syncedRevisions(nodes) == QStringList{QStringLiteral("r3")},
                "a copy that is behind is not linked at its last common commit");
        const QVector<ProjectGraphCloneLinks::Link> links = ProjectGraphCloneLinks::computeSync(nodes, bounds);
        require(links.size() == 1 && links.first().route.size() == 4
                    && near(links.first().route.first(), QPointF(100, 390))
                    && near(links.first().route.last(), QPointF(500, 390)),
                "the sync line does not join the two revision cards side by side");
        int marked = 0;
        for (const ProjectGraphNode &node : nodes) marked += node.clone_sync_keys.size();
        require(marked == 2, "more than the two last common revisions were marked");
        // Re-running replaces the marks instead of adding to them.
        ProjectGraphCloneLinks::markSyncFrontiers(nodes);
        marked = 0;
        for (const ProjectGraphNode &node : nodes) marked += node.clone_sync_keys.size();
        require(marked == 2, "marking twice duplicated the sync marks");
    }
    // Equal copies: the line is at HEAD.
    {
        QVector<ProjectGraphNode> nodes;
        addCopy(nodes, QStringLiteral("a"), first_uuid, 0, {QStringLiteral("r1:"), QStringLiteral("r2:r1")});
        addCopy(nodes, QStringLiteral("b"), first_uuid, 600, {QStringLiteral("r1:"), QStringLiteral("r2:r1")});
        ProjectGraphCloneLinks::markSyncFrontiers(nodes);
        require(syncedRevisions(nodes) == QStringList{QStringLiteral("r2")}, "equal copies are not linked at HEAD");
    }
    // Diverged copies: the line is at the fork point.
    {
        QVector<ProjectGraphNode> nodes;
        addCopy(nodes, QStringLiteral("a"), first_uuid, 0,
                {QStringLiteral("r1:"), QStringLiteral("r2:r1"), QStringLiteral("x3:r2")});
        addCopy(nodes, QStringLiteral("b"), first_uuid, 600,
                {QStringLiteral("r1:"), QStringLiteral("r2:r1"), QStringLiteral("y3:r2"), QStringLiteral("y4:y3")});
        ProjectGraphCloneLinks::markSyncFrontiers(nodes);
        require(syncedRevisions(nodes) == QStringList{QStringLiteral("r2")},
                "diverged copies are not linked where they split");
    }
    // Two lines of history (a feature branch) both shared: one line each.
    {
        QVector<ProjectGraphNode> nodes;
        addCopy(nodes, QStringLiteral("a"), first_uuid, 0,
                {QStringLiteral("r1:"), QStringLiteral("r2:r1"), QStringLiteral("r3:r2"),
                 QStringLiteral("f1:r2"), QStringLiteral("f2:f1")});
        addCopy(nodes, QStringLiteral("b"), first_uuid, 600,
                {QStringLiteral("r1:"), QStringLiteral("r2:r1"), QStringLiteral("r3:r2"), QStringLiteral("f1:r2")});
        ProjectGraphCloneLinks::markSyncFrontiers(nodes);
        require(syncedRevisions(nodes) == QStringList({QStringLiteral("f1"), QStringLiteral("r3")}),
                "each shared line of history does not get its own sync line");
    }
    // A merge both have: its merged parents are behind it, not separate lines.
    {
        QVector<ProjectGraphNode> nodes;
        addCopy(nodes, QStringLiteral("a"), first_uuid, 0,
                {QStringLiteral("r1:"), QStringLiteral("s1:r1"), QStringLiteral("r2:r1"), QStringLiteral("m:r2,s1")});
        addCopy(nodes, QStringLiteral("b"), first_uuid, 600,
                {QStringLiteral("r1:"), QStringLiteral("s1:r1"), QStringLiteral("r2:r1"), QStringLiteral("m:r2,s1")});
        ProjectGraphCloneLinks::markSyncFrontiers(nodes);
        require(syncedRevisions(nodes) == QStringList{QStringLiteral("m")},
                "a shared merge left its parents marked as separate sync points");
    }
    // Nothing shared, or not clones at all: no sync line.
    {
        QVector<ProjectGraphNode> nodes;
        addCopy(nodes, QStringLiteral("a"), first_uuid, 0, {QStringLiteral("r1:")});
        addCopy(nodes, QStringLiteral("b"), first_uuid, 600, {QStringLiteral("q1:")});
        addCopy(nodes, QStringLiteral("c"), second_uuid, 1200, {QStringLiteral("r1:")});
        addCopy(nodes, QStringLiteral("d"), QString(), 1800, {QStringLiteral("r1:")});
        ProjectGraphCloneLinks::markSyncFrontiers(nodes);
        require(syncedRevisions(nodes).isEmpty(), "copies without a common commit, or non-clones, were linked");
    }
    // Three copies: every pair shows where it agrees.
    {
        QVector<ProjectGraphNode> nodes;
        addCopy(nodes, QStringLiteral("a"), first_uuid, 0,
                {QStringLiteral("r1:"), QStringLiteral("r2:r1"), QStringLiteral("r3:r2")});
        addCopy(nodes, QStringLiteral("b"), first_uuid, 600, {QStringLiteral("r1:"), QStringLiteral("r2:r1")});
        addCopy(nodes, QStringLiteral("c"), first_uuid, 1200, {QStringLiteral("r1:")});
        ProjectGraphCloneLinks::markSyncFrontiers(nodes);
        require(syncedRevisions(nodes) == QStringList({QStringLiteral("r1"), QStringLiteral("r1"), QStringLiteral("r2")}),
                "three copies do not show the last common commit of every pair");
    }
    // A sync card that is not displayed (folded away) draws no line.
    {
        QVector<ProjectGraphNode> nodes;
        addCopy(nodes, QStringLiteral("a"), first_uuid, 0, {QStringLiteral("r1:"), QStringLiteral("r2:r1")});
        addCopy(nodes, QStringLiteral("b"), first_uuid, 600, {QStringLiteral("r1:")});
        ProjectGraphCloneLinks::markSyncFrontiers(nodes);
        QVector<ProjectGraphNode> displayed;
        for (const ProjectGraphNode &node : nodes)
            if (!(node.project_group_id == QStringLiteral("b") && node.display_id == QStringLiteral("r1")))
                displayed.append(node);
        require(ProjectGraphCloneLinks::computeSync(displayed, bounds).isEmpty(),
                "a sync line was drawn to a card that is not displayed");
    }
}
}

int main()
{
    // Three copies, given out of order: two links, left to right.
    {
        const QVector<ProjectGraphNode> nodes{
            projectCard(QStringLiteral("c"), QStringLiteral("s3"), QPointF(1000, 150), first_uuid),
            projectCard(QStringLiteral("a"), QStringLiteral("s1"), QPointF(0, 150), first_uuid),
            projectCard(QStringLiteral("b"), QStringLiteral("s2"), QPointF(500, 150), first_uuid)};
        const QVector<ProjectGraphCloneLinks::Link> links = ProjectGraphCloneLinks::compute(nodes, bounds);
        require(links.size() == 2, "three clones are not chained by two links");
        require(links.size() == 2 && links.at(0).first == 1 && links.at(0).second == 2
                    && links.at(1).first == 2 && links.at(1).second == 0,
                "clones are not chained left to right");
        if (!links.isEmpty())
        {
            const QVector<QPointF> &route = links.at(0).route;
            require(route.size() == 4, "a link is not one cubic Bezier");
            require(route.size() == 4 && near(route.at(0), QPointF(100, 150))
                        && near(route.at(3), QPointF(400, 150)),
                    "side-by-side clones are not joined edge to edge at mid height");
            require(route.size() == 4 && route.at(1).x() > route.at(0).x()
                        && route.at(2).x() < route.at(3).x()
                        && near(QPointF(0, route.at(1).y()), QPointF(0, 150)),
                    "side link control points do not leave the cards horizontally");
        }
    }

    // Unlinked cases: single cards, different projects, missing identity,
    // and non-project cards that happen to carry the field.
    {
        ProjectGraphNode server = projectCard(QStringLiteral("server"), QString(), QPointF(500, 0), first_uuid);
        server.kind = ProjectGraphNode::Kind::Server;
        ProjectGraphNode revision = projectCard(QStringLiteral("rev"), QStringLiteral("p"), QPointF(600, 300), first_uuid);
        revision.kind = ProjectGraphNode::Kind::Revision;
        const QVector<ProjectGraphNode> nodes{
            projectCard(QStringLiteral("a"), QStringLiteral("s1"), QPointF(0, 150), first_uuid),
            projectCard(QStringLiteral("b"), QStringLiteral("s2"), QPointF(500, 150), second_uuid),
            projectCard(QStringLiteral("c"), QStringLiteral("s3"), QPointF(900, 150), QString()),
            projectCard(QStringLiteral("d"), QStringLiteral("s4"), QPointF(1300, 150), QString()),
            server, revision};
        require(ProjectGraphCloneLinks::compute(nodes, bounds).isEmpty(),
                "cards of different, unknown or non-project identities were linked");
        require(ProjectGraphCloneLinks::compute({}, bounds).isEmpty(), "an empty graph produced links");
    }

    // Two cloned projects: one link each, never across projects.
    {
        const QVector<ProjectGraphNode> nodes{
            projectCard(QStringLiteral("a1"), QStringLiteral("s1"), QPointF(0, 150), first_uuid),
            projectCard(QStringLiteral("b1"), QStringLiteral("s1"), QPointF(250, 150), second_uuid),
            projectCard(QStringLiteral("a2"), QStringLiteral("s2"), QPointF(600, 150), first_uuid),
            projectCard(QStringLiteral("b2"), QStringLiteral("s2"), QPointF(850, 150), second_uuid)};
        const QVector<ProjectGraphCloneLinks::Link> links = ProjectGraphCloneLinks::compute(nodes, bounds);
        require(links.size() == 2, "two cloned projects do not get one link each");
        for (const ProjectGraphCloneLinks::Link &link : links)
            require(nodes.at(link.first).clone_identity == nodes.at(link.second).clone_identity,
                    "a link joins two different projects");
    }

    // Cards that overlap horizontally (stacked): bottom of the upper card to
    // the top of the lower one, whichever is listed first.
    {
        const QVector<ProjectGraphNode> nodes{
            projectCard(QStringLiteral("lower"), QStringLiteral("s2"), QPointF(40, 400), first_uuid),
            projectCard(QStringLiteral("upper"), QStringLiteral("s1"), QPointF(0, 150), first_uuid)};
        const QVector<ProjectGraphCloneLinks::Link> links = ProjectGraphCloneLinks::compute(nodes, bounds);
        require(links.size() == 1, "stacked clones are not linked");
        if (links.size() == 1)
        {
            const QVector<QPointF> &route = links.first().route;
            require(near(route.at(0), QPointF(0, 180)) && near(route.at(3), QPointF(40, 370)),
                    "stacked clones are not joined bottom to top");
            require(route.at(1).y() > route.at(0).y() && route.at(2).y() < route.at(3).y(),
                    "vertical link control points do not leave the cards vertically");
        }
    }

    // Same x: ordered top to bottom, deterministic.
    {
        const QVector<ProjectGraphNode> nodes{
            projectCard(QStringLiteral("b"), QStringLiteral("s2"), QPointF(0, 500), first_uuid),
            projectCard(QStringLiteral("a"), QStringLiteral("s1"), QPointF(0, 150), first_uuid)};
        const QVector<ProjectGraphCloneLinks::Link> links = ProjectGraphCloneLinks::compute(nodes, bounds);
        require(links.size() == 1 && links.first().first == 1 && links.first().second == 0,
                "clones at the same x are not ordered top to bottom");
    }

    testSyncFrontiers();

    if (failures != 0) return 1;
    std::puts("Project graph clone link tests passed");
    return 0;
}
