#pragma once

// Presentation-only metadata for the four Projects Inspector domains.
// Database/Project/Branch/Revision widgets consume these values without
// introducing persistence or selection dependencies into the forms.

#include "tabs/tab_projects_widget.h"
#include "projects/dvmdb_identity.h"

#include <QCoreApplication>

struct ProjectsInspectorPresentation
{
    QString heading = QCoreApplication::translate("ProjectsWidget", "Inspector");
    QString name;
    QString description;
    QString commit_message;
    QStringList tags;
    QString subtitle;
    QString metadata;
    bool editable = true;
};

class ProjectsInspectorPresentations final
{
public:
    static ProjectsInspectorPresentation empty()
    {
        ProjectsInspectorPresentation view;
        view.editable = false;
        view.subtitle = translate("Select a server, project, branch, or revision.");
        return view;
    }

    static ProjectsInspectorPresentation newDatabase()
    {
        ProjectsInspectorPresentation view;
        view.heading = translate("New database");
        view.subtitle = translate("Configure a data source. Nothing is saved until you select Create.");
        return view;
    }

    static ProjectsInspectorPresentation database(const ProjectServerProfile *server,
                                                   const QString &repository_id)
    {
        ProjectsInspectorPresentation view;
        if (server == nullptr) return view;
        view.heading = (server->connection_type == QStringLiteral("local")
            ? translate("Local Database: %1")
            : translate("Remote Database: %1")).arg(server->name);
        view.name = server->name;
        view.description = server->description;
        view.tags = server->tags;
        view.metadata = server->pending
            ? translate("Identity: pending verification (temporary ID: %1)").arg(server->id)
            : DvmdbIdentity::isUuidV7(server->id)
                ? translate("DVMDB repository UUIDv7: %1").arg(server->id)
                : translate("In-memory demonstration source (no DVMDB repository)");
        if (!repository_id.isEmpty() && !server->pending && repository_id != server->id)
            view.metadata += translate("\nIdentity mismatch: %1").arg(repository_id);
        return view;
    }

    static ProjectsInspectorPresentation project(const ProjectPreviewData *project)
    {
        ProjectsInspectorPresentation view;
        if (project == nullptr) return view;
        view.heading = translate("Project");
        view.name = project->name;
        view.description = project->description;
        view.tags = project->tags;
        view.metadata = project->database_backed
            ? translate("DVMDB project UUIDv7: %1").arg(project->backend_id)
            : translate("Preview project ID: %1").arg(project->id);
        if (project->database_backed && !project->repository_id.isEmpty())
            view.metadata += translate("\nRepository UUIDv7: %1").arg(project->repository_id);
        if (!project->source_example_resource.isEmpty())
            view.metadata += translate("\nSource: bundled example (%1)\n"
                "Network data is NOT stored on this server; this is only a GUI preview.")
                .arg(project->source_example_resource);
        return view;
    }

    static ProjectsInspectorPresentation branch(const QList<ProjectPreviewData> &projects,
                                                 const QString &selected_id)
    {
        ProjectsInspectorPresentation view;
        for (const ProjectPreviewData &entry : projects)
            for (const ProjectPreviewBranch &branch : entry.branches)
                if (branch.id == selected_id)
                {
                    view.heading = translate("Branch");
                    view.name = branch.name;
                    view.description = branch.description;
                    view.tags = branch.tags;
                    view.metadata = branch.head.isEmpty()
                        ? translate("No commits yet\nBranch: %1").arg(branch.name)
                        : translate("Head revision: %1\nBranch: %2\nRevision identity: %3")
                            .arg(branch.head, branch.name,
                                 DvmdbIdentity::revisionFormatName(branch.head));
                    if (!branch.origin_branch_id.isEmpty())
                    {
                        const QString origin_id = entry.database_backed
                            ? entry.id + QStringLiteral(":") + branch.origin_branch_id
                            : branch.origin_branch_id;
                        QString origin_name;
                        for (const ProjectPreviewBranch &candidate : entry.branches)
                            if (candidate.id == origin_id) { origin_name = candidate.name; break; }
                        view.metadata += translate("\nCreated from branch: %1")
                            .arg(origin_name.isEmpty() ? branch.origin_branch_id : origin_name);
                    }
                    if (!branch.fork_revision_id.isEmpty())
                        view.metadata += translate("\nOriginal fork revision UUIDv7: %1")
                            .arg(branch.fork_revision_id);
                    if (entry.database_backed)
                    {
                        const QString prefix = entry.id + QStringLiteral(":");
                        const QString suffix = branch.id.mid(prefix.size());
                        if (DvmdbIdentity::isUuidV7(suffix))
                            view.metadata += translate("\nBranch UUIDv7: %1").arg(suffix);
                    }
                }
        return view;
    }

    static ProjectsInspectorPresentation head(const ProjectPreviewBranch *branch,
                                              const ProjectPreviewData *project)
    {
        ProjectsInspectorPresentation view;
        if (branch == nullptr) return view;
        view.heading = translate("HEAD");
        view.name = branch->name;
        view.description = branch->description;
        view.tags = branch->tags;
        view.editable = false;
        view.subtitle = translate("Writable HEAD · Branch: %1").arg(branch->name);
        view.metadata = branch->head.isEmpty()
            ? translate("No commits yet")
            : translate("Base revision: %1").arg(branch->head);
        if (project != nullptr && project->database_backed)
        {
            const QString prefix = project->id + QStringLiteral(":");
            if (branch->id.startsWith(prefix))
                view.metadata += translate("\nBranch UUIDv7: %1")
                    .arg(branch->id.mid(prefix.size()));
        }
        if (!branch->fork_revision_id.isEmpty())
            view.metadata += translate("\nOriginal fork revision: %1")
                .arg(branch->fork_revision_id);
        return view;
    }

    static ProjectsInspectorPresentation revision(const ProjectPreviewRevision *revision,
                                                   const ProjectPreviewData *project,
                                                   const ProjectPreviewBranch *branch)
    {
        ProjectsInspectorPresentation view;
        if (revision == nullptr) return view;
        view.heading = translate("Revision");
        view.name = branch != nullptr ? branch->name : revision->branch;
        view.description = branch != nullptr ? branch->description : QString();
        view.tags = branch != nullptr ? branch->tags : QStringList();
        view.commit_message = revision->message.isEmpty() ? revision->title : revision->message;
        view.subtitle = translate("Immutable revision · commit message is a mutable display annotation");
        if (project != nullptr && project->database_backed)
        {
            QStringList heads;
            for (const ProjectPreviewBranch &branch : project->branches)
                if (branch.head == revision->id) heads.append(branch.name);
            view.metadata = translate("Revision UUIDv7: %1\nHEAD of: %2\nParents: %3")
                .arg(revision->id,
                     heads.isEmpty() ? translate("(none)") : heads.join(QStringLiteral(", ")),
                     revision->parents.isEmpty() ? translate("(root)") : revision->parents.join(QStringLiteral(", ")));
            if (branch != nullptr)
                view.metadata.prepend(translate("Branch UUIDv7: %1\n")
                    .arg(branch->id.mid(project->id.size() + 1)));
            if (!revision->record_sha256.isEmpty())
                view.metadata += translate("\nCanonical record SHA-256: %1").arg(revision->record_sha256);
        }
        else
            view.metadata = translate("Revision: %1\nBranch: %2\nParents: %3\nAuthor: %4\nCreated: %5")
                .arg(revision->id, revision->branch,
                     revision->parents.isEmpty() ? translate("(root)") : revision->parents.join(QStringLiteral(", ")),
                     revision->author, revision->created_at);
        return view;
    }

private:
    static QString translate(const char *message)
    {
        return QCoreApplication::translate("ProjectsWidget", message);
    }
};
