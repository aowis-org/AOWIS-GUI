#pragma once

#include "tabs/tab_projects_widget.h"
#include "projects/dvmdb_identity.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QTextEdit>
#include <QSet>
#include <QSizePolicy>
#include <QStandardItemModel>
#include <QVBoxLayout>
#include <QWidget>

// Project and branch creation are independent forms. Their inputs, origin
// choices, validation and explanatory text are never reused between domains.
class ProjectsInspectorCreationForm : public QWidget
{
public:
    explicit ProjectsInspectorCreationForm(const QString &object_name, QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setObjectName(object_name);
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        QVBoxLayout *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 12, 0, 4);
        this->fields = new QFormLayout();
        this->fields->setRowWrapPolicy(QFormLayout::WrapAllRows);
        this->fields->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        this->name = new QLineEdit(this);
        this->fields->addRow(tr("Name"), this->name);
        layout->addLayout(this->fields);
        this->summary = new QLabel(this);
        this->summary->setWordWrap(true);
        this->summary->setMinimumWidth(0);
        QSizePolicy label_policy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        label_policy.setHeightForWidth(true);
        this->summary->setSizePolicy(label_policy);
        this->summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(this->summary);
        this->error = new QLabel(this);
        this->error->setWordWrap(true);
        this->error->setMinimumWidth(0);
        this->error->setSizePolicy(label_policy);
        layout->addWidget(this->error);
        layout->addStretch(1);
    }

    void reset()
    {
        this->name->clear();
        this->summary->clear();
        this->error->clear();
    }

    bool isDirty() const { return !this->name->text().trimmed().isEmpty(); }
    bool isValid() const { return this->error->text().isEmpty(); }

    QLineEdit *name = nullptr;
    QLabel *summary = nullptr;
    QLabel *error = nullptr;

protected:
    QFormLayout *fields = nullptr;
    void showValidation(const QString &summary_text, const QString &error_text)
    {
        this->summary->setText(summary_text);
        this->error->setText(error_text);
    }
};

class ProjectInspectorCreationForm final : public ProjectsInspectorCreationForm
{
public:
    explicit ProjectInspectorCreationForm(QWidget *parent = nullptr)
        : ProjectsInspectorCreationForm(QStringLiteral("projectsProjectCreationForm"), parent)
    {
        this->name->setObjectName(QStringLiteral("projectsProjectCreationName"));
        this->summary->setObjectName(QStringLiteral("projectsProjectCreationSummary"));
        this->error->setObjectName(QStringLiteral("projectsProjectCreationError"));
    }

    void validate(const QList<ProjectPreviewData> &projects, const QString &server_id)
    {
        const QString name_text = this->name->text().trimmed();
        QString error_text;
        if (name_text.isEmpty()) error_text = tr("Enter a name.");
        for (const ProjectPreviewData &entry : projects)
            if (entry.server_id == server_id
                && entry.name.compare(name_text, Qt::CaseInsensitive) == 0)
                error_text = tr("A project with this name already exists.");
        showValidation(tr("Creates an empty project with an initial main branch. ")
            + tr("No network data or additional revision will be created."), error_text);
    }
};

class BranchInspectorCreationForm final : public ProjectsInspectorCreationForm
{
public:
    explicit BranchInspectorCreationForm(QWidget *parent = nullptr)
        : ProjectsInspectorCreationForm(QStringLiteral("projectsBranchCreationForm"), parent)
    {
        this->name->setObjectName(QStringLiteral("projectsBranchCreationName"));
        this->summary->setObjectName(QStringLiteral("projectsBranchCreationSummary"));
        this->error->setObjectName(QStringLiteral("projectsBranchCreationError"));
        this->source = new QComboBox(this);
        this->source->setObjectName(QStringLiteral("projectsCreationSource"));
        this->source->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        this->source->setMinimumWidth(0);
        this->fields->addRow(tr("Start from"), this->source);
        this->tags = new QLineEdit(this);
        this->tags->setObjectName(QStringLiteral("projectsBranchCreationTags"));
        this->tags->setPlaceholderText(tr("hydraulics, testing"));
        this->fields->addRow(tr("Tags"), this->tags);
        this->description = new QTextEdit(this);
        this->description->setObjectName(QStringLiteral("projectsBranchCreationDescription"));
        this->description->setMinimumHeight(85);
        this->description->setMaximumHeight(160);
        this->fields->addRow(tr("Description"), this->description);
    }

    void begin(const ProjectPreviewData &project, const QString &chosen_origin)
    {
        reset();
        this->tags->clear();
        this->description->clear();
        this->project_origin = chosen_origin.isEmpty();
        this->source->setVisible(!this->project_origin);
        if (QWidget *label = this->fields->labelForField(this->source))
            label->setVisible(!this->project_origin);
        this->source->clear();
        for (const ProjectPreviewBranch &entry : project.branches)
        {
            this->source->addItem(entry.head.isEmpty()
                    ? tr("%1 · No commits yet").arg(entry.name)
                    : tr("%1 · HEAD %2").arg(entry.name, DvmdbIdentity::shortLabel(entry.head)),
                QStringLiteral("branch:") + entry.id);
            if (project.database_backed && entry.head.isEmpty())
            {
                const int index = this->source->count() - 1;
                if (QStandardItemModel *model = qobject_cast<QStandardItemModel *>(this->source->model()))
                    if (QStandardItem *item = model->item(index)) item->setEnabled(false);
            }
        }
        for (const ProjectPreviewRevision &entry : project.revisions)
            this->source->addItem(tr("Revision %1").arg(DvmdbIdentity::shortLabel(entry.id)),
                QStringLiteral("revision:") + entry.id);
        QString chosen = chosen_origin;
        if (chosen.isEmpty())
            for (const ProjectPreviewBranch &entry : project.branches)
                if (entry.name == QStringLiteral("main"))
                {
                    chosen = QStringLiteral("branch:") + entry.id;
                    break;
                }
        if (chosen.isEmpty() && !project.branches.isEmpty())
            chosen = QStringLiteral("branch:") + project.branches.first().id;
        if (this->source->count() == 0)
            this->source->addItem(tr("Initial empty revision"), QStringLiteral("empty"));
        const int index = this->source->findData(chosen);
        if (index >= 0) this->source->setCurrentIndex(index);
    }

    void validate(const ProjectPreviewData *project, const QString &expected_project_id,
                  QString (*resolve_source)(const ProjectPreviewData &, const QString &, const QString &))
    {
        const QString name_text = this->name->text().trimmed();
        QString error_text;
        if (name_text.isEmpty()) error_text = tr("Enter a name.");
        const QByteArray bytes = name_text.toUtf8();
        if (bytes.size() > 96 || QString::fromUtf8(bytes) != name_text
            || name_text == QStringLiteral(".") || name_text == QStringLiteral("..")
            || name_text.startsWith(QLatin1Char(' ')) || name_text.endsWith(QLatin1Char(' ')))
            error_text = tr("Branch name must be valid UTF-8, at most 96 bytes, without surrounding spaces.");
        for (const QChar character : name_text)
            if (character.unicode() < 0x20 || character.unicode() == 0x7f
                || character == QLatin1Char('/') || character == QLatin1Char('\\')
                || character == QLatin1Char('%') || character == QLatin1Char('?')
                || character == QLatin1Char('#'))
                error_text = tr("Branch name contains an unsupported control or path character.");
        if (project == nullptr || project->id != expected_project_id)
            error_text = tr("The source project is no longer selected.");
        if (project != nullptr)
            for (const ProjectPreviewBranch &entry : project->branches)
                if (entry.name.compare(name_text, Qt::CaseInsensitive) == 0)
                    error_text = tr("A branch with this name already exists.");
        if (this->project_origin)
        {
            if (this->description->toPlainText().trimmed().toUtf8().size() > 512)
                error_text = tr("Description must not exceed 512 UTF-8 bytes.");
            QSet<QString> distinct_tags;
            for (const QString &part : this->tags->text().split(QLatin1Char(','), Qt::SkipEmptyParts))
            {
                const QString tag = part.trimmed();
                if (tag.isEmpty()) continue;
                if (tag.toUtf8().size() > 32 || distinct_tags.contains(tag))
                    error_text = tr("Tags must be unique and at most 32 UTF-8 bytes each.");
                distinct_tags.insert(tag);
            }
            if (distinct_tags.size() > 12)
                error_text = tr("A branch may have at most 12 tags.");
            showValidation(tr("Creates a new unborn HEAD in this project. No revision is created."), error_text);
            return;
        }
        QString revision;
        const QString origin = this->source->currentData().toString();
        if (origin.startsWith(QStringLiteral("branch:")) && project != nullptr)
        {
            for (const ProjectPreviewBranch &entry : project->branches)
                if (entry.id == origin.mid(7))
                {
                    revision = entry.head;
                    if (project->database_backed && revision.isEmpty())
                        error_text = tr("Commit the source branch before creating another database branch.");
                    break;
                }
        }
        else if (origin.startsWith(QStringLiteral("revision:")))
        {
            revision = origin.mid(9);
            if (project != nullptr && project->database_backed
                && resolve_source(*project, revision, QString()).isEmpty())
                error_text = tr("The selected revision cannot be reached from a branch HEAD.");
        }
        else if (origin == QStringLiteral("empty"))
        {
            if (project != nullptr && project->database_backed)
                error_text = tr("Commit the source branch before creating another database branch.");
        }
        else if (origin.isEmpty() && project != nullptr && project->database_backed)
            error_text = tr("Commit the source branch before creating another database branch.");
        else error_text = tr("Choose a starting revision.");
        QString source_branch_id;
        QString source_branch_name;
        QString source_revision_hash;
        if (project != nullptr)
        {
            QString source_name;
            if (origin.startsWith(QStringLiteral("branch:")))
                for (const ProjectPreviewBranch &entry : project->branches)
                    if (entry.id == origin.mid(7))
                    {
                        source_name = entry.name;
                        break;
                    }
            if (origin.startsWith(QStringLiteral("revision:")))
                source_name = resolve_source(*project, revision, QString());
            for (const ProjectPreviewBranch &entry : project->branches)
                if (entry.name == source_name)
                {
                    source_branch_id = entry.id;
                    source_branch_name = entry.name;
                    break;
                }
            for (const ProjectPreviewRevision &entry : project->revisions)
                if (entry.id == revision)
                {
                    source_revision_hash = entry.record_sha256;
                    break;
                }
        }
        QString summary_text = tr("Fork revision UUID: %1\n").arg(
            revision.isEmpty() ? tr("No commits yet") : revision);
        if (!source_branch_id.isEmpty())
            summary_text += tr("Source branch: %1\nSource branch UUID: %2\n")
                .arg(source_branch_name, source_branch_id);
        if (!source_revision_hash.isEmpty())
            summary_text += tr("Revision SHA-256: %1\n").arg(source_revision_hash);
        summary_text += tr("Creating a branch adds a reference only; it does not create a revision.");
        showValidation(summary_text, error_text);
    }

    QComboBox *source = nullptr;
    QLineEdit *tags = nullptr;
    QTextEdit *description = nullptr;
    bool project_origin = false;
};
