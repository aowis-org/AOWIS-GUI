// Reproducible CPU-side Projects graph benchmark. This deliberately uses the
// production layered DAG placement and bgfx scene staging paths, but does not
// initialize a GPU. It must not be reported as a GPU frame-time benchmark.
#include "tabs/project_graph_bgfx_scene.h"
#include "tabs/project_graph_placement.h"

#include <QApplication>
#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QPointF>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cstdio>

namespace
{
struct Result
{
    int revisions = 0;
    int nodes = 0;
    qint64 generated_kib = -1;
    qint64 layout_kib = -1;
    qint64 peak_kib = -1;
    double generation_ms = 0.0;
    double layout_ms = 0.0;
    double index_build_ms = 0.0;
    double scene_median_ms = 0.0;
    double scene_max_ms = 0.0;
    int visible_cards = 0;
    int indexed_candidates = 0;
    int culled_cards = 0;
    int ancestry_edges = 0;
    qsizetype vertices = 0;
    qsizetype indices = 0;
    bool overflow = false;
};

qint64 linuxProcessMemoryKib(const char *field)
{
#ifdef Q_OS_LINUX
    QFile status(QStringLiteral("/proc/self/status"));
    if (!status.open(QIODevice::ReadOnly | QIODevice::Text))
        return -1;
    const QByteArray prefix(field);
    // /proc/self/status is a virtual file whose reported size is zero.
    // QFile::atEnd() can be true before the first read; read until EOF.
    while (true)
    {
        const QByteArray line = status.readLine();
        if (line.isEmpty())
            break;
        if (line.startsWith(prefix))
        {
            const QList<QByteArray> words = line.mid(prefix.size()).simplified().split(' ');
            bool ok = false;
            const qint64 amount = words.isEmpty() ? -1 : words.first().toLongLong(&ok);
            return ok ? amount : -1;
        }
    }
#else
    Q_UNUSED(field)
#endif
    return -1;
}

QString revisionId(int database, int project, int revision)
{
    return QStringLiteral("db%1-p%2-r%3")
        .arg(database).arg(project).arg(revision, 7, 10, QChar('0'));
}

// Two databases, four projects each, with real parent/merge references and
// independent HEADs. A side branch forks every 64 revisions and is merged on
// the next revision. These are synthetic graph nodes, not DVMDB fixtures.
QVector<ProjectGraphNode> generate(int revisions)
{
    constexpr int databases = 2;
    constexpr int projects_per_database = 4;
    constexpr int total_projects = databases * projects_per_database;
    QVector<ProjectGraphNode> graph;
    graph.reserve(revisions + databases + total_projects * 2);
    const int base_per_project = revisions / total_projects;
    const int remainder = revisions % total_projects;
    for (int db = 0; db < databases; ++db)
    {
        ProjectGraphNode server;
        server.kind = ProjectGraphNode::Kind::Server;
        server.id = QStringLiteral("server-%1").arg(db);
        server.title = QStringLiteral("Survey database %1").arg(db);
        server.personal_server = db == 0;
        graph.append(server);
        for (int project = 0; project < projects_per_database; ++project)
        {
            const int ordinal = db * projects_per_database + project;
            const int count = base_per_project + (ordinal < remainder ? 1 : 0);
            ProjectGraphNode project_node;
            project_node.kind = ProjectGraphNode::Kind::Project;
            project_node.id = QStringLiteral("project-%1-%2").arg(db).arg(project);
            project_node.title = QStringLiteral("Network %1/%2").arg(db).arg(project);
            project_node.container_id = server.id;
            graph.append(project_node);
            for (int revision = 0; revision < count; ++revision)
            {
                ProjectGraphNode node;
                node.kind = ProjectGraphNode::Kind::Revision;
                node.id = revisionId(db, project, revision);
                node.display_id = node.id;
                node.title = QStringLiteral("Revision %1").arg(revision);
                node.branch = revision % 64 == 10 ? QStringLiteral("survey")
                    : QStringLiteral("main");
                node.project_group_id = project_node.id;
                if (revision > 0)
                {
                    if (revision % 64 == 10 && revision >= 2)
                        node.parents.append(revisionId(db, project, revision - 2));
                    else
                        node.parents.append(revisionId(db, project, revision - 1));
                    if (revision % 64 == 11)
                        node.parents.append(revisionId(db, project, revision - 2));
                }
                graph.append(node);
            }
            ProjectGraphNode head;
            head.kind = ProjectGraphNode::Kind::Branch;
            head.id = QStringLiteral("head-%1-%2").arg(db).arg(project);
            head.title = QStringLiteral("HEAD");
            head.branch = QStringLiteral("main");
            head.project_group_id = project_node.id;
            if (count > 0)
                head.head_revision_id = revisionId(db, project, count - 1);
            graph.append(head);
        }
    }
    return graph;
}

double milliseconds(const QElapsedTimer &timer)
{
    return double(timer.nsecsElapsed()) / 1000000.0;
}

Result measure(int revisions, int repeats, const QApplication &application)
{
    Result result;
    result.revisions = revisions;
    QElapsedTimer timer;
    timer.start();
    QVector<ProjectGraphNode> nodes = generate(revisions);
    result.generation_ms = milliseconds(timer);
    result.generated_kib = linuxProcessMemoryKib("VmRSS:");
    timer.restart();
    ProjectGraphPlacement::place(nodes);
    result.layout_ms = milliseconds(timer);
    result.layout_kib = linuxProcessMemoryKib("VmRSS:");
    result.nodes = nodes.size();

    MapBgfxHudCanvas canvas;
    MapBgfxHudTextCache atlas(2048);
    MapBgfxHudCanvas text_canvas;
    MapBgfxGlyphAtlas glyphs(1024);
    ProjectGraphBgfxScene::TextTarget text_target;
    text_target.canvas = &text_canvas;
    text_target.glyphs = &glyphs;
    ProjectGraphBgfxScene::View view;
    view.viewport = QRectF(0, 0, 1280, 800);
    view.palette = application.palette();
    view.font = application.font();
    view.zoom = 1.0;
    view.top_down = true;
    const ProjectGraphNode *anchor = nullptr;
    for (const ProjectGraphNode &node : nodes)
    {
        if (node.kind == ProjectGraphNode::Kind::Revision)
        {
            anchor = &node;
            break;
        }
    }
    if (anchor != nullptr)
        view.offset = view.viewport.center() - anchor->position;

    // Index construction is a one-time, data/layout-change cost. Do not
    // include it in camera movement frames, but report it separately.
    ProjectGraphBgfxScene::Index index;
    timer.restart();
    index.rebuild(nodes, {}, view.top_down);
    result.index_build_ms = milliseconds(timer);
    QVector<double> samples;
    samples.reserve(repeats);
    for (int i = 0; i < repeats; ++i)
    {
        // Move the viewport between measurements to measure real pan frames,
        // not just repeatedly submitting an identical cached image.
        view.offset += QPointF(55.0, 30.0);
        timer.restart();
        const ProjectGraphBgfxScene::Stats stats = ProjectGraphBgfxScene::build(
            &canvas, &atlas, text_target, index, view);
        samples.append(milliseconds(timer));
        result.visible_cards = stats.cards;
        result.indexed_candidates = stats.candidate_items;
        result.culled_cards = stats.culled_cards;
        result.ancestry_edges = stats.ancestry_edges;
        result.vertices = canvas.vertices().size();
        result.indices = canvas.indices().size();
        result.overflow = result.overflow || canvas.overflowed();
    }
    std::sort(samples.begin(), samples.end());
    result.scene_median_ms = samples.at(samples.size() / 2);
    result.scene_max_ms = samples.last();
    result.peak_kib = linuxProcessMemoryKib("VmHWM:");
    return result;
}

void output(const Result &result)
{
    // One CSV record per dataset, suitable for diffing across patches.
    std::printf("%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%d,%lld,%lld,%d,%lld,%lld,%lld\n",
                result.revisions, result.nodes, result.generation_ms,
                result.layout_ms, result.index_build_ms, result.scene_median_ms,
                result.scene_max_ms, result.visible_cards, result.culled_cards,
                result.ancestry_edges, result.indexed_candidates,
                static_cast<long long>(result.vertices),
                static_cast<long long>(result.indices), result.overflow ? 1 : 0,
                static_cast<long long>(result.generated_kib),
                static_cast<long long>(result.layout_kib),
                static_cast<long long>(result.peak_kib));
    std::fflush(stdout);
}
}

int main(int argc, char **argv)
{
    // Qt font metrics and atlas rasterization require a QGui application.
    // No GUI window, GPU context or network service is created.
    QApplication app(argc, argv);
    QVector<int> sizes{1000, 10000};
    int repeats = 3;
    const QStringList arguments = app.arguments();
    for (int i = 1; i < arguments.size(); ++i)
    {
        const QString argument = arguments.at(i);
        if (argument == QStringLiteral("--stress"))
            sizes = {1000, 10000, 100000};
        else if (argument.startsWith(QStringLiteral("--sizes=")))
        {
            sizes.clear();
            const QStringList requested = argument.mid(8).split(',');
            for (const QString &part : requested)
            {
                bool ok = false;
                const int size = part.toInt(&ok);
                if (!ok || size < 1 || size > 100000)
                {
                    std::fprintf(stderr, "Invalid size (1..100000): %s\n",
                                 qPrintable(part));
                    return 2;
                }
                sizes.append(size);
            }
        }
        else if (argument.startsWith(QStringLiteral("--repeats=")))
        {
            bool ok = false;
            repeats = argument.mid(10).toInt(&ok);
            if (!ok || repeats < 1 || repeats > 30)
            {
                std::fprintf(stderr, "Invalid repeats (1..30)\n");
                return 2;
            }
        }
        else if (argument == QStringLiteral("--help"))
        {
            std::puts("Usage: aowis-project-graph-performance-benchmark [--sizes=1000,10000] [--stress] [--repeats=3]");
            std::puts("CPU-only: actual production DAG placement + bgfx scene staging. No GPU timings.");
            return 0;
        }
        else
        {
            std::fprintf(stderr, "Unknown argument: %s\n", qPrintable(argument));
            return 2;
        }
    }

    std::puts("revisions,nodes,generate_ms,layout_ms,index_build_ms,scene_median_ms,scene_max_ms,visible_cards,culled_cards,ancestry_edges,indexed_candidates,vertices,indices,overflow,after_generate_rss_kib,after_layout_rss_kib,peak_rss_kib");
    double ten_thousand_layout_ms = -1.0;
    for (const int size : sizes)
    {
        if (size >= 100000 && ten_thousand_layout_ms > 2500.0)
        {
            std::fprintf(stderr,
                "Refusing 100k stress workload: 10k layout took %.1f ms "
                "(safety threshold: 2500 ms).\n", ten_thousand_layout_ms);
            return 3;
        }
        std::fprintf(stderr, "Graph benchmark: starting %d revisions (%d scene passes)\n",
                     size, repeats);
        std::fflush(stderr);
        const Result result = measure(size, repeats, app);
        output(result);
        std::fprintf(stderr, "Graph benchmark: finished %d revisions, layout %.1f ms\n",
                     size, result.layout_ms);
        if (size == 10000)
            ten_thousand_layout_ms = result.layout_ms;
        if (result.nodes < size || result.ancestry_edges < size / 2)
        {
            std::fprintf(stderr, "Benchmark fixture/scene incorrect at %d revisions\n", size);
            return 1;
        }
    }
    return 0;
}
