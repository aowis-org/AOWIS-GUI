#include "settings/personal_repository_settings_widget.h"

#include <QEvent>
#include <QFormLayout>
#include <QLabel>
#include <QHBoxLayout>
#include <QFont>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

PersonalRepositorySettingsWidget::PersonalRepositorySettingsWidget(QWidget *parent)
    : QWidget(parent), monitor(this)
{
    setObjectName(QStringLiteral("personalRepositorySettings"));
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 18, 18, 18);

    QLabel *heading = new QLabel(tr("Personal working repository"), this);
    heading->setObjectName(QStringLiteral("personalRepositoryHeading"));
    QFont title_font = heading->font();
    title_font.setBold(true);
    heading->setFont(title_font);
    layout->addWidget(heading);

    QLabel *description = new QLabel(tr(
        "This is separate from the shared and read-only databases in Projects. "
        "Draft editing is disabled until the private repository has been verified. "
        "Configuration and identity verification do not authenticate the server."), this);
    description->setObjectName(QStringLiteral("personalRepositoryDescription"));
    description->setWordWrap(true);
    layout->addWidget(description);

#ifdef AOWIS_STANDALONE_DVMDB_ENABLED
    const PersonalRepositoryTarget target = resolvePersonalRepositoryTarget();
    QLabel *embedded = new QLabel(
        target.kind == PersonalRepositoryTarget::Kind::Embedded
            ? tr("Dedicated personal DVMDB: %1. "
                 "It is separate from the navigable Local database. "
                 "Native identity/role verification and draft checkout are not enabled yet.")
                  .arg(target.storage_path)
            : tr("The private DVMDB path is also configured as a local project source. "
                 "Remove the conflicting source before enabling private editing."), this);
    embedded->setObjectName(QStringLiteral("personalRepositoryStandaloneStatus"));
    embedded->setProperty("personalRepositoryEmbedded", target.kind == PersonalRepositoryTarget::Kind::Embedded);
    embedded->setProperty("personalRepositoryPath", target.storage_path);
    embedded->setWordWrap(true);
    layout->addWidget(embedded);
#else
    QFormLayout *form = new QFormLayout();
    this->endpoint = new QLineEdit(this);
    this->endpoint->setObjectName(QStringLiteral("personalRepositoryEndpoint"));
    this->endpoint->setPlaceholderText(QStringLiteral("http://127.0.0.1:8755"));
    QLabel *endpoint_label = new QLabel(tr("Private DVMDB URL:"), this);
    endpoint_label->setObjectName(QStringLiteral("personalRepositoryEndpointTitle"));
    form->addRow(endpoint_label, this->endpoint);
    this->repository_id = new QLineEdit(this);
    this->repository_id->setObjectName(QStringLiteral("personalRepositoryExpectedId"));
    this->repository_id->setPlaceholderText(tr("UUIDv7 from GET /dvmdb/v1/repository"));
    QLabel *identity_label = new QLabel(tr("Expected repository UUID:"), this);
    identity_label->setObjectName(QStringLiteral("personalRepositoryExpectedIdTitle"));
    form->addRow(identity_label, this->repository_id);
    layout->addLayout(form);

    QHBoxLayout *actions = new QHBoxLayout();
    this->save_button = new QPushButton(tr("Save and verify"), this);
    this->save_button->setObjectName(QStringLiteral("personalRepositorySave"));
    actions->addWidget(this->save_button);
    QPushButton *recheck = new QPushButton(tr("Check again"), this);
    recheck->setObjectName(QStringLiteral("personalRepositoryRecheck"));
    actions->addWidget(recheck);
    QPushButton *remove = new QPushButton(tr("Remove configuration"), this);
    remove->setObjectName(QStringLiteral("personalRepositoryRemove"));
    actions->addWidget(remove);
    layout->addLayout(actions);

    connect(this->save_button, &QPushButton::clicked, this, [this] { save(); });
    connect(recheck, &QPushButton::clicked, this, [this] { verify(); });
    connect(remove, &QPushButton::clicked, this, [this] { clear(); });
#endif

    this->status = new QLabel(this);
    this->status->setWordWrap(true);
    this->status->setObjectName(QStringLiteral("personalRepositoryStatus"));
    layout->addWidget(this->status);

    QLabel *warning = new QLabel(tr(
        "Security: a repository UUID and URL are not authentication. "
        "Use an isolated private service; do not expose writable HTTP DVMDB to untrusted users. "
        "Credentials must not be stored in GUI settings."), this);
    warning->setObjectName(QStringLiteral("personalRepositoryWarning"));
    warning->setWordWrap(true);
    layout->addWidget(warning);
    layout->addStretch();
    restore();
}

void PersonalRepositorySettingsWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange)
        refreshLocalizedText();
}

void PersonalRepositorySettingsWidget::refreshLocalizedText()
{
    if (QLabel *heading = findChild<QLabel *>(QStringLiteral("personalRepositoryHeading")))
        heading->setText(tr("Personal working repository"));
    if (QLabel *description = findChild<QLabel *>(QStringLiteral("personalRepositoryDescription")))
        description->setText(tr("This is separate from the shared and read-only databases in Projects. "
                                "Draft editing is disabled until the private repository has been verified. "
                                "Configuration and identity verification do not authenticate the server."));
#ifdef AOWIS_STANDALONE_DVMDB_ENABLED
    if (QLabel *embedded = findChild<QLabel *>(QStringLiteral("personalRepositoryStandaloneStatus")))
    {
        embedded->setText(embedded->property("personalRepositoryEmbedded").toBool()
            ? tr("Dedicated personal DVMDB: %1. "
                 "It is separate from the navigable Local database. "
                 "Native identity/role verification and draft checkout are not enabled yet.")
                  .arg(embedded->property("personalRepositoryPath").toString())
            : tr("The private DVMDB path is also configured as a local project source. "
                 "Remove the conflicting source before enabling private editing."));
    }
#else
    if (QLabel *endpoint_label = findChild<QLabel *>(QStringLiteral("personalRepositoryEndpointTitle")))
        endpoint_label->setText(tr("Private DVMDB URL:"));
    if (QLabel *identity_label = findChild<QLabel *>(QStringLiteral("personalRepositoryExpectedIdTitle")))
        identity_label->setText(tr("Expected repository UUID:"));
    if (this->repository_id != nullptr)
        this->repository_id->setPlaceholderText(tr("UUIDv7 from GET /dvmdb/v1/repository"));
    if (this->save_button != nullptr)
        this->save_button->setText(tr("Save and verify"));
    if (QPushButton *recheck = findChild<QPushButton *>(QStringLiteral("personalRepositoryRecheck")))
        recheck->setText(tr("Check again"));
    if (QPushButton *remove = findChild<QPushButton *>(QStringLiteral("personalRepositoryRemove")))
        remove->setText(tr("Remove configuration"));
#endif
    if (QLabel *warning = findChild<QLabel *>(QStringLiteral("personalRepositoryWarning")))
        warning->setText(tr("Security: a repository UUID and URL are not authentication. "
                            "Use an isolated private service; do not expose writable HTTP DVMDB to untrusted users. "
                            "Credentials must not be stored in GUI settings."));
    if (this->status != nullptr)
    {
        const LocalStatusDetail previous_error = this->local_status_detail;
        QString detail = this->displayed_status_detail;
        if (previous_error == LocalStatusDetail::SaveFailed)
            detail = tr("Could not save the personal repository configuration.");
        else if (previous_error == LocalStatusDetail::RemoveFailed)
            detail = tr("Could not remove the personal repository configuration.");
        updateStatus(this->displayed_status_state, detail);
        this->local_status_detail = previous_error;
    }
}

PersonalRepositoryMonitor::State PersonalRepositorySettingsWidget::verificationState() const
{
    return this->monitor.currentState();
}

void PersonalRepositorySettingsWidget::restore()
{
#ifdef AOWIS_STANDALONE_DVMDB_ENABLED
    updateStatus(PersonalRepositoryMonitor::State::Unconfigured,
                 resolvePersonalRepositoryTarget().kind == PersonalRepositoryTarget::Kind::Embedded
                     ? tr("Standalone private repository is selected, but its identity and role "
                          "have not yet been verified. Editing remains disabled.")
                     : tr("Private working repository conflicts with a project source. "
                          "Editing remains disabled."));
#else
    const std::optional<PersonalRepositoryProfile> profile = loadPersonalRepositoryProfile();
    if (!profile)
    {
        updateStatus(PersonalRepositoryMonitor::State::Unconfigured, QString());
        return;
    }
    this->endpoint->setText(profile->endpoint);
    this->repository_id->setText(profile->repository_id);
    verify();
#endif
}

void PersonalRepositorySettingsWidget::save()
{
#ifndef AOWIS_STANDALONE_DVMDB_ENABLED
    const PersonalRepositoryProfile profile{
        this->endpoint->text().trimmed(), this->repository_id->text().trimmed()};
    QString error;
    if (!validatePersonalRepositoryProfile(profile, &error))
    {
        this->monitor.stop();
        updateStatus(PersonalRepositoryMonitor::State::Unavailable, error);
        return;
    }
    if (!savePersonalRepositoryProfile(profile))
    {
        this->monitor.stop();
        updateStatus(PersonalRepositoryMonitor::State::Unavailable,
                     tr("Could not save the personal repository configuration."));
        this->local_status_detail = LocalStatusDetail::SaveFailed;
        return;
    }
    verify();
#endif
}

void PersonalRepositorySettingsWidget::clear()
{
#ifndef AOWIS_STANDALONE_DVMDB_ENABLED
    if (!clearPersonalRepositoryProfile())
    {
        updateStatus(PersonalRepositoryMonitor::State::Unavailable,
                     tr("Could not remove the personal repository configuration."));
        this->local_status_detail = LocalStatusDetail::RemoveFailed;
        return;
    }
    this->monitor.stop();
    this->endpoint->clear();
    this->repository_id->clear();
    updateStatus(PersonalRepositoryMonitor::State::Unconfigured, QString());
#endif
}

void PersonalRepositorySettingsWidget::verify()
{
#ifndef AOWIS_STANDALONE_DVMDB_ENABLED
    // Recheck the SAVED profile, never unsaved edits or the active source.
    const std::optional<PersonalRepositoryProfile> profile = loadPersonalRepositoryProfile();
    if (!profile)
    {
        this->monitor.stop();
        updateStatus(PersonalRepositoryMonitor::State::Unconfigured, QString());
        return;
    }
    this->monitor.start(*profile,
        [this](PersonalRepositoryMonitor::State state, const QString &detail)
        {
            updateStatus(state, detail);
        });
#endif
}

void PersonalRepositorySettingsWidget::updateStatus(
    PersonalRepositoryMonitor::State state, const QString &detail)
{
    this->displayed_status_state = state;
    this->displayed_status_detail = detail;
    this->local_status_detail = LocalStatusDetail::None;
    QString text;
    switch (state)
    {
        case PersonalRepositoryMonitor::State::Unconfigured:
            text = tr("Not configured — private draft editing unavailable."); break;
        case PersonalRepositoryMonitor::State::Checking:
            text = tr("Checking private repository identity and role…"); break;
        case PersonalRepositoryMonitor::State::Verified:
            text = tr("Private repository identity verified (not authenticated). "
                      "Draft editing will be enabled in VC-04."); break;
        case PersonalRepositoryMonitor::State::WrongIdentity:
            text = tr("Wrong repository — personal editing disabled."); break;
        case PersonalRepositoryMonitor::State::WrongRole:
            text = tr("Repository is not marked private — personal editing disabled."); break;
        case PersonalRepositoryMonitor::State::Unavailable:
            text = tr("Private repository unavailable — personal editing disabled."); break;
    }
    if (!detail.isEmpty()) text += QStringLiteral(" ") + detail;
    this->status->setText(text);
}
