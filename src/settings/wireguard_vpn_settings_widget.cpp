#include "settings/wireguard_vpn_settings_widget.h"

#include <QAbstractItemView>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

namespace
{
constexpr qint64 MaximumImportBytes = 128 * 1024;

QLabel *wrappedLabel(QWidget *parent)
{
    QLabel *label = new QLabel(parent);
    label->setWordWrap(true);
    return label;
}

QString prettyBytes(quint64 bytes)
{
    if (bytes < 1024) return QObject::tr("%1 B").arg(static_cast<qulonglong>(bytes));
    if (bytes < 1024 * 1024) return QObject::tr("%1 KiB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024ULL * 1024ULL * 1024ULL)
        return QObject::tr("%1 MiB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    return QObject::tr("%1 GiB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
}
}

WireGuardVpnSettingsWidget::WireGuardVpnSettingsWidget(QWidget *parent)
    : QWidget(parent)
{
    this->setObjectName(QStringLiteral("wireguardVpnSettings"));
    QVBoxLayout *outer = new QVBoxLayout(this);
    outer->setContentsMargins(24, 20, 24, 20);
    outer->setSpacing(12);

    this->title = new QLabel(this);
    QFont title_font = this->title->font();
    title_font.setPointSize(title_font.pointSize() + 2);
    title_font.setBold(true);
    this->title->setFont(title_font);
    outer->addWidget(this->title);

    this->introduction = wrappedLabel(this);
    outer->addWidget(this->introduction);
    this->session_notice = wrappedLabel(this);
    this->session_notice->setObjectName(QStringLiteral("wireguardVpnStorageNotice"));
    outer->addWidget(this->session_notice);

    QSplitter *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    QWidget *list_panel = new QWidget(splitter);
    list_panel->setMinimumWidth(220);
    QVBoxLayout *list_layout = new QVBoxLayout(list_panel);
    list_layout->setContentsMargins(0, 0, 8, 0);
    QHBoxLayout *actions = new QHBoxLayout();
    this->new_button = new QPushButton(list_panel);
    this->new_button->setObjectName(QStringLiteral("wireguardVpnNew"));
    this->import_button = new QPushButton(list_panel);
    this->import_button->setObjectName(QStringLiteral("wireguardVpnImport"));
    this->delete_button = new QPushButton(list_panel);
    this->delete_button->setObjectName(QStringLiteral("wireguardVpnDelete"));
    actions->addWidget(this->new_button);
    actions->addWidget(this->import_button);
    actions->addWidget(this->delete_button);
    list_layout->addLayout(actions);
    this->profile_list = new QListWidget(list_panel);
    this->profile_list->setObjectName(QStringLiteral("wireguardVpnProfileList"));
    this->profile_list->setSelectionMode(QAbstractItemView::SingleSelection);
    list_layout->addWidget(this->profile_list, 1);

    QWidget *detail_panel = new QWidget(splitter);
    detail_panel->setMinimumWidth(340);
    QVBoxLayout *detail_layout = new QVBoxLayout(detail_panel);
    detail_layout->setContentsMargins(8, 0, 0, 0);
    this->empty_notice = wrappedLabel(detail_panel);
    detail_layout->addWidget(this->empty_notice);
    this->detail_tabs = new QTabWidget(detail_panel);
    this->detail_tabs->setObjectName(QStringLiteral("wireguardVpnDetails"));
    detail_layout->addWidget(this->detail_tabs, 1);

    QWidget *configuration_tab = new QWidget(this->detail_tabs);
    QVBoxLayout *config_layout = new QVBoxLayout(configuration_tab);
    QFormLayout *form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    QLabel *name_caption = new QLabel(configuration_tab);
    name_caption->setObjectName(QStringLiteral("wireguardVpnNameCaption"));
    this->name_edit = new QLineEdit(configuration_tab);
    this->name_edit->setObjectName(QStringLiteral("wireguardVpnName"));
    form->addRow(name_caption, this->name_edit);
    QLabel *description_caption = new QLabel(configuration_tab);
    description_caption->setObjectName(QStringLiteral("wireguardVpnDescriptionCaption"));
    this->description_edit = new QPlainTextEdit(configuration_tab);
    this->description_edit->setObjectName(QStringLiteral("wireguardVpnDescription"));
    this->description_edit->setMaximumHeight(70);
    form->addRow(description_caption, this->description_edit);
    config_layout->addLayout(form);
    QLabel *editor_caption = new QLabel(configuration_tab);
    editor_caption->setObjectName(QStringLiteral("wireguardVpnEditorCaption"));
    config_layout->addWidget(editor_caption);
    this->configuration_edit = new QPlainTextEdit(configuration_tab);
    this->configuration_edit->setObjectName(QStringLiteral("wireguardVpnEditor"));
    this->configuration_edit->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont monospace = this->configuration_edit->font();
    monospace.setStyleHint(QFont::Monospace);
    monospace.setFamily(QStringLiteral("monospace"));
    this->configuration_edit->setFont(monospace);
    config_layout->addWidget(this->configuration_edit, 1);
    this->validation_label = wrappedLabel(configuration_tab);
    this->validation_label->setObjectName(QStringLiteral("wireguardVpnValidation"));
    config_layout->addWidget(this->validation_label);
    QHBoxLayout *save_actions = new QHBoxLayout();
    this->save_button = new QPushButton(configuration_tab);
    this->save_button->setObjectName(QStringLiteral("wireguardVpnSave"));
    this->revert_button = new QPushButton(configuration_tab);
    this->revert_button->setObjectName(QStringLiteral("wireguardVpnRevert"));
    save_actions->addStretch();
    save_actions->addWidget(this->revert_button);
    save_actions->addWidget(this->save_button);
    config_layout->addLayout(save_actions);
    this->detail_tabs->addTab(configuration_tab, QString());

    QWidget *usage_tab = new QWidget(this->detail_tabs);
    QVBoxLayout *usage_layout = new QVBoxLayout(usage_tab);
    this->usage_info = wrappedLabel(usage_tab);
    usage_layout->addWidget(this->usage_info);
    this->usage_table = new QTreeWidget(usage_tab);
    this->usage_table->setObjectName(QStringLiteral("wireguardVpnUsedBy"));
    this->usage_table->setRootIsDecorated(false);
    this->usage_table->setAlternatingRowColors(true);
    this->usage_table->setColumnCount(3);
    this->usage_table->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    this->usage_table->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    this->usage_table->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    usage_layout->addWidget(this->usage_table, 1);
    this->detail_tabs->addTab(usage_tab, QString());

    QWidget *status_tab = new QWidget(this->detail_tabs);
    QVBoxLayout *status_layout = new QVBoxLayout(status_tab);
    this->status_info = wrappedLabel(status_tab);
    status_layout->addWidget(this->status_info);
    QFormLayout *status_fields = new QFormLayout();
    this->tunnel_state = wrappedLabel(status_tab);
    this->tunnel_state->setObjectName(QStringLiteral("wireguardVpnTunnelState"));
    this->active_consumers = wrappedLabel(status_tab);
    this->active_consumers->setObjectName(QStringLiteral("wireguardVpnActiveConsumers"));
    this->traffic = wrappedLabel(status_tab);
    this->traffic->setObjectName(QStringLiteral("wireguardVpnTraffic"));
    this->tunnel_error = wrappedLabel(status_tab);
    QLabel *state_caption = new QLabel(status_tab);
    state_caption->setObjectName(QStringLiteral("wireguardVpnStateCaption"));
    QLabel *consumers_caption = new QLabel(status_tab);
    consumers_caption->setObjectName(QStringLiteral("wireguardVpnConsumersCaption"));
    QLabel *traffic_caption = new QLabel(status_tab);
    traffic_caption->setObjectName(QStringLiteral("wireguardVpnTrafficCaption"));
    status_fields->addRow(state_caption, this->tunnel_state);
    status_fields->addRow(consumers_caption, this->active_consumers);
    status_fields->addRow(traffic_caption, this->traffic);
    status_layout->addLayout(status_fields);
    status_layout->addWidget(this->tunnel_error);
    this->peers_table = new QTreeWidget(status_tab);
    this->peers_table->setObjectName(QStringLiteral("wireguardVpnPeers"));
    this->peers_table->setRootIsDecorated(false);
    this->peers_table->setColumnCount(4);
    this->peers_table->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    status_layout->addWidget(this->peers_table, 1);
    this->detail_tabs->addTab(status_tab, QString());

    splitter->addWidget(list_panel);
    splitter->addWidget(detail_panel);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    outer->addWidget(splitter, 1);

    connect(this->new_button, &QPushButton::clicked, this, [this] {
        if (leaveCurrentProfile()) createProfile(tr("New profile"), QString());
    });
    connect(this->import_button, &QPushButton::clicked, this, &WireGuardVpnSettingsWidget::importProfile);
    connect(this->delete_button, &QPushButton::clicked, this, &WireGuardVpnSettingsWidget::deleteProfile);
    connect(this->save_button, &QPushButton::clicked, this, &WireGuardVpnSettingsWidget::saveProfile);
    connect(this->revert_button, &QPushButton::clicked, this, &WireGuardVpnSettingsWidget::revertProfile);
    connect(this->profile_list, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *, QListWidgetItem *) { onSelectionChanged(); });
    connect(this->name_edit, &QLineEdit::textChanged, this, [this] { refreshControls(); });
    connect(this->description_edit, &QPlainTextEdit::textChanged, this, [this] { refreshControls(); });
    connect(this->configuration_edit, &QPlainTextEdit::textChanged, this, [this] { refreshControls(); });
    connect(this->usage_table, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item, int) {
        if (item != nullptr) emit serviceActivated(item->data(0, Qt::UserRole).toString());
    });
    this->retranslate();
    this->refreshProfileList();
    this->loadSelection();
}

WireGuardVpnSettingsWidget::Profile *WireGuardVpnSettingsWidget::findProfile(const QString &id)
{
    for (Profile &profile : this->profiles)
        if (profile.id == id) return &profile;
    return nullptr;
}

const WireGuardVpnSettingsWidget::Profile *WireGuardVpnSettingsWidget::findProfile(const QString &id) const
{
    for (const Profile &profile : this->profiles)
        if (profile.id == id) return &profile;
    return nullptr;
}

QStringList WireGuardVpnSettingsWidget::profileIds() const
{
    QStringList ids;
    for (const Profile &profile : this->profiles)
        ids.append(profile.id);
    return ids;
}

QString WireGuardVpnSettingsWidget::profileIdForName(const QString &name) const
{
    for (const Profile &profile : this->profiles)
        if (profile.name == name) return profile.id;
    return {};
}

QString WireGuardVpnSettingsWidget::profileConfig(const QString &profile_id) const
{
    const Profile *profile = findProfile(profile_id);
    return profile == nullptr ? QString() : profile->configuration;
}

void WireGuardVpnSettingsWidget::createProfile(const QString &name, const QString &configuration)
{
    Profile profile;
    profile.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    profile.name = name;
    profile.configuration = configuration;
    profile.draft = true;
    this->profiles.append(profile);
    this->selected_id = profile.id;
    this->refreshProfileList();
    this->loadSelection();
    this->name_edit->setFocus();
    this->name_edit->selectAll();
}

void WireGuardVpnSettingsWidget::importProfile()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Import WireGuard configuration"),
        QString(), tr("WireGuard configurations (*.conf);;All files (*)"));
    if (path.isEmpty()) return;
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly) || source.size() > MaximumImportBytes)
    {
        QMessageBox::warning(this, tr("Import failed"), tr("Cannot read configuration, or the file is larger than 128 KiB."));
        return;
    }
    const QByteArray contents = source.readAll();
    if (source.error() != QFileDevice::NoError || contents.contains('\0')
        || contents.size() > MaximumImportBytes
        || QString::fromUtf8(contents).toUtf8() != contents)
    {
        QMessageBox::warning(this, tr("Import failed"), tr("The selected file is not a plain-text WireGuard configuration."));
        return;
    }
    if (!leaveCurrentProfile()) return;
    // Only the contents and a human-readable suggested name are imported;
    // the filesystem path is never stored as an identity or dependency.
    createProfile(QFileInfo(path).completeBaseName(), QString::fromUtf8(contents));
}

bool WireGuardVpnSettingsWidget::hasChanges() const
{
    const Profile *profile = findProfile(this->selected_id);
    if (profile == nullptr) return false;
    return profile->draft || profile->name != this->name_edit->text()
        || profile->description != this->description_edit->toPlainText()
        || profile->configuration != this->configuration_edit->toPlainText();
}

bool WireGuardVpnSettingsWidget::leaveCurrentProfile()
{
    if (!hasChanges()) return true;
    const QMessageBox::StandardButton answer = QMessageBox::question(this, tr("Discard changes?"),
        tr("This profile has unsaved changes. Discard them?"),
        QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Discard) return false;
    const Profile *profile = findProfile(this->selected_id);
    if (profile != nullptr && profile->draft)
    {
        const QString id = profile->id;
        for (qsizetype index = 0; index < this->profiles.size(); ++index)
        {
            if (this->profiles.at(index).id == id)
            {
                this->profiles.removeAt(index);
                break;
            }
        }
        this->references.remove(id);
        this->runtime.remove(id);
    }
    return true;
}

void WireGuardVpnSettingsWidget::onSelectionChanged()
{
    if (this->selecting) return;
    QListWidgetItem *item = this->profile_list->currentItem();
    const QString requested_id = item == nullptr ? QString() : item->data(Qt::UserRole).toString();
    if (requested_id == this->selected_id) return;
    if (!leaveCurrentProfile())
    {
        this->selecting = true;
        for (int row = 0; row < this->profile_list->count(); ++row)
        {
            if (this->profile_list->item(row)->data(Qt::UserRole).toString() == this->selected_id)
            {
                this->profile_list->setCurrentRow(row);
                break;
            }
        }
        this->selecting = false;
        return;
    }
    this->selected_id = requested_id;
    this->refreshProfileList(); // Remove any discarded draft and keep selection.
    this->loadSelection();
}

void WireGuardVpnSettingsWidget::refreshProfileList()
{
    this->selecting = true;
    this->profile_list->clear();
    int selected_row = -1;
    for (const Profile &profile : this->profiles)
    {
        QString reference_text = tr("Service usage unavailable");
        if (this->references.contains(profile.id))
        {
            const qsizetype count = this->references.value(profile.id).size();
            reference_text = count == 1 ? tr("Used by 1 service")
                                        : tr("Used by %1 services").arg(static_cast<qlonglong>(count));
        }
        QString state_text = tr("Status unavailable");
        if (this->runtime.contains(profile.id))
        {
            switch (this->runtime.value(profile.id).state)
            {
                case TunnelStatus::State::Stopped: state_text = tr("Stopped"); break;
                case TunnelStatus::State::Starting: state_text = tr("Starting"); break;
                case TunnelStatus::State::Running: state_text = tr("Running"); break;
                case TunnelStatus::State::Error: state_text = tr("Error"); break;
                case TunnelStatus::State::Unknown: break;
            }
        }
        QListWidgetItem *item = new QListWidgetItem(
            QStringLiteral("%1%2\n%3 · %4")
                .arg(profile.name, profile.draft ? tr(" (draft)") : QString(), reference_text, state_text),
            this->profile_list);
        item->setData(Qt::UserRole, profile.id);
        item->setToolTip(profile.description);
        if (profile.id == this->selected_id) selected_row = this->profile_list->count() - 1;
    }
    this->profile_list->setCurrentRow(selected_row);
    this->selecting = false;
}

void WireGuardVpnSettingsWidget::loadSelection()
{
    const Profile *profile = findProfile(this->selected_id);
    this->loading_editor = true;
    this->name_edit->setText(profile == nullptr ? QString() : profile->name);
    this->description_edit->setPlainText(profile == nullptr ? QString() : profile->description);
    this->configuration_edit->setPlainText(profile == nullptr ? QString() : profile->configuration);
    this->loading_editor = false;
    this->detail_tabs->setVisible(profile != nullptr);
    this->empty_notice->setVisible(profile == nullptr);
    this->refreshUsage();
    this->refreshStatus();
    this->refreshControls();
}

QString WireGuardVpnSettingsWidget::configurationValidation(const QString &configuration) const
{
    bool interface_seen = false;
    bool private_key_seen = false;
    int peers = 0;
    bool peer_has_public_key = false;
    QString section;
    const QStringList lines = configuration.split('\n');
    for (int index = 0; index < lines.size(); ++index)
    {
        const QString line = lines.at(index).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char(';'))) continue;
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']')))
        {
            if (section == QStringLiteral("Peer") && !peer_has_public_key)
                return tr("Each [Peer] section requires a PublicKey.");
            section = line.mid(1, line.size() - 2).trimmed();
            if (section == QStringLiteral("Interface"))
            {
                if (interface_seen) return tr("Only one [Interface] section is supported.");
                interface_seen = true;
            }
            else if (section == QStringLiteral("Peer"))
            {
                ++peers;
                peer_has_public_key = false;
            }
            else return tr("Unknown section at line %1.").arg(index + 1);
            continue;
        }
        const qsizetype separator = line.indexOf(QLatin1Char('='));
        if (separator <= 0 || section.isEmpty()) return tr("Invalid configuration line %1.").arg(index + 1);
        const QString key = line.left(separator).trimmed();
        if (key.isEmpty() || line.mid(separator + 1).trimmed().isEmpty())
            return tr("Missing key or value at line %1.").arg(index + 1);
        if (section == QStringLiteral("Interface") && key == QStringLiteral("PrivateKey")) private_key_seen = true;
        if (section == QStringLiteral("Peer") && key == QStringLiteral("PublicKey")) peer_has_public_key = true;
    }
    if (section == QStringLiteral("Peer") && !peer_has_public_key)
        return tr("Each [Peer] section requires a PublicKey.");
    if (!interface_seen || !private_key_seen || peers == 0)
        return tr("A profile requires [Interface] with PrivateKey and at least one [Peer] with PublicKey.");
    return {};
}

void WireGuardVpnSettingsWidget::refreshControls()
{
    if (this->loading_editor) return;
    const Profile *profile = findProfile(this->selected_id);
    const bool exists = profile != nullptr;
    this->delete_button->setEnabled(exists);
    const bool dirty = exists && hasChanges();
    this->revert_button->setEnabled(dirty);
    if (!exists)
    {
        this->save_button->setEnabled(false);
        this->validation_label->clear();
        return;
    }
    const QString error = configurationValidation(this->configuration_edit->toPlainText());
    bool unique_name = !this->name_edit->text().trimmed().isEmpty();
    for (const Profile &candidate : this->profiles)
    {
        if (candidate.id != this->selected_id
            && candidate.name.compare(this->name_edit->text().trimmed(), Qt::CaseInsensitive) == 0)
            unique_name = false;
    }
    this->save_button->setEnabled(dirty && unique_name && error.isEmpty());
    this->validation_label->setText(error.isEmpty()
        ? tr("Configuration structure looks valid. This is not a tunnel connectivity check.")
        : error);
}

void WireGuardVpnSettingsWidget::saveProfile()
{
    Profile *profile = findProfile(this->selected_id);
    if (profile == nullptr) return;
    const QString name = this->name_edit->text().trimmed();
    if (name.isEmpty())
    {
        QMessageBox::warning(this, tr("Invalid profile"), tr("Enter a profile name."));
        return;
    }
    for (const Profile &candidate : this->profiles)
    {
        if (candidate.id != profile->id && candidate.name.compare(name, Qt::CaseInsensitive) == 0)
        {
            QMessageBox::warning(this, tr("Invalid profile"), tr("Another profile already uses this name."));
            return;
        }
    }
    const QString config = this->configuration_edit->toPlainText();
    const QString error = configurationValidation(config);
    if (!error.isEmpty())
    {
        QMessageBox::warning(this, tr("Invalid configuration"), error);
        return;
    }
    profile->name = name;
    profile->description = this->description_edit->toPlainText();
    profile->configuration = config;
    profile->draft = false;
    this->refreshProfileList();
    this->refreshControls();
}

void WireGuardVpnSettingsWidget::revertProfile()
{
    const Profile *profile = findProfile(this->selected_id);
    if (profile == nullptr) return;
    if (profile->draft)
    {
        const QString id = profile->id;
        for (qsizetype index = 0; index < this->profiles.size(); ++index)
        {
            if (this->profiles.at(index).id == id)
            {
                this->profiles.removeAt(index);
                break;
            }
        }
        this->selected_id.clear();
        this->refreshProfileList();
    }
    this->loadSelection();
}

void WireGuardVpnSettingsWidget::deleteProfile()
{
    const Profile *profile = findProfile(this->selected_id);
    if (profile == nullptr) return;
    if (this->references.contains(profile->id) && !this->references.value(profile->id).isEmpty())
    {
        QMessageBox::warning(this, tr("Profile in use"),
            tr("This profile is referenced by configured services. Remove those references before deleting it."));
        this->detail_tabs->setCurrentIndex(1);
        return;
    }
    if (QMessageBox::question(this, tr("Delete WireGuard profile?"),
        tr("Delete '%1' from this session? Existing server connection settings will not be changed.").arg(profile->name),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    const QString id = profile->id;
    for (qsizetype index = 0; index < this->profiles.size(); ++index)
    {
        if (this->profiles.at(index).id == id)
        {
            this->profiles.removeAt(index);
            break;
        }
    }
    this->references.remove(id);
    this->runtime.remove(id);
    this->selected_id = this->profiles.isEmpty() ? QString() : this->profiles.first().id;
    this->refreshProfileList();
    this->loadSelection();
}

void WireGuardVpnSettingsWidget::setServiceReferences(
    const QString &profile_id, const QList<ServiceReference> &services)
{
    if (findProfile(profile_id) == nullptr) return;
    this->references.insert(profile_id, services);
    this->refreshProfileList();
    if (this->selected_id == profile_id) this->refreshUsage();
}

void WireGuardVpnSettingsWidget::setTunnelStatus(const QString &profile_id, const TunnelStatus &status)
{
    if (findProfile(profile_id) == nullptr) return;
    this->runtime.insert(profile_id, status);
    this->refreshProfileList();
    if (this->selected_id == profile_id) this->refreshStatus();
}

void WireGuardVpnSettingsWidget::refreshUsage()
{
    this->usage_table->clear();
    if (this->selected_id.isEmpty()) return;
    if (!this->references.contains(this->selected_id))
    {
        this->usage_info->setText(tr("Service references are not yet connected to the central profile manager. Existing file-path connections are unchanged."));
        return;
    }
    const QList<ServiceReference> services = this->references.value(this->selected_id);
    this->usage_info->setText(services.isEmpty() ? tr("No services use this profile.")
        : services.size() == 1
            ? tr("Used by 1 configured service. This does not imply an active tunnel.")
            : tr("Used by %1 configured services. This does not imply an active tunnel.")
                  .arg(static_cast<qlonglong>(services.size())));
    for (const ServiceReference &service : services)
    {
        QTreeWidgetItem *item = new QTreeWidgetItem(this->usage_table,
            {service.name, service.kind, service.endpoint});
        item->setData(0, Qt::UserRole, service.id);
    }
}

void WireGuardVpnSettingsWidget::refreshStatus()
{
    this->peers_table->clear();
    this->tunnel_error->clear();
    if (!this->runtime.contains(this->selected_id))
    {
        this->status_info->setText(tr("Live tunnel monitoring is not connected yet. No WireGuard tunnel has been started by this page."));
        this->tunnel_state->setText(tr("Unavailable"));
        this->active_consumers->setText(tr("Unavailable"));
        this->traffic->setText(tr("Unavailable"));
        return;
    }
    this->status_info->setText(tr("Latest status reported by the WireGuard runtime."));
    const TunnelStatus status = this->runtime.value(this->selected_id);
    switch (status.state)
    {
        case TunnelStatus::State::Unknown: this->tunnel_state->setText(tr("Unknown")); break;
        case TunnelStatus::State::Stopped: this->tunnel_state->setText(tr("Stopped")); break;
        case TunnelStatus::State::Starting: this->tunnel_state->setText(tr("Starting")); break;
        case TunnelStatus::State::Running: this->tunnel_state->setText(tr("Running")); break;
        case TunnelStatus::State::Error: this->tunnel_state->setText(tr("Error")); break;
    }
    this->active_consumers->setText(status.active_consumers < 0 ? tr("Unavailable")
        : QString::number(status.active_consumers));
    this->traffic->setText(tr("Received %1 / Sent %2").arg(prettyBytes(status.received_bytes), prettyBytes(status.sent_bytes)));
    this->tunnel_error->setText(status.error);
    for (const PeerStatus &peer : status.peers)
        new QTreeWidgetItem(this->peers_table,
            {peer.endpoint, peer.last_handshake, prettyBytes(peer.received_bytes), prettyBytes(peer.sent_bytes)});
}

void WireGuardVpnSettingsWidget::retranslate()
{
    this->title->setText(tr("WireGuard VPN"));
    this->introduction->setText(tr("Import and manage reusable WireGuard profiles. Services can share a profile without creating separate configurations."));
    this->session_notice->setText(tr("GUI preview: profiles exist only for this session. Configuration storage, service assignment and runtime integration will follow. Private keys are not written to disk."));
    this->empty_notice->setText(tr("No profile selected. Import a .conf file or create a new profile."));
    this->new_button->setText(tr("New"));
    this->import_button->setText(tr("Import…"));
    this->delete_button->setText(tr("Delete"));
    this->save_button->setText(tr("Save changes"));
    this->revert_button->setText(tr("Revert"));
    this->detail_tabs->setTabText(0, tr("Configuration"));
    this->detail_tabs->setTabText(1, tr("Used by"));
    this->detail_tabs->setTabText(2, tr("Status"));
    this->findChild<QLabel *>(QStringLiteral("wireguardVpnNameCaption"))->setText(tr("Profile name"));
    this->findChild<QLabel *>(QStringLiteral("wireguardVpnDescriptionCaption"))->setText(tr("Description"));
    this->findChild<QLabel *>(QStringLiteral("wireguardVpnEditorCaption"))->setText(tr("WireGuard .conf (private keys are visible while editing)"));
    this->findChild<QLabel *>(QStringLiteral("wireguardVpnStateCaption"))->setText(tr("Tunnel"));
    this->findChild<QLabel *>(QStringLiteral("wireguardVpnConsumersCaption"))->setText(tr("Active services"));
    this->findChild<QLabel *>(QStringLiteral("wireguardVpnTrafficCaption"))->setText(tr("Traffic"));
    this->usage_table->setHeaderLabels({tr("Service"), tr("Type"), tr("Endpoint")});
    this->peers_table->setHeaderLabels({tr("Peer endpoint"), tr("Last handshake"), tr("Received"), tr("Sent")});
    this->refreshProfileList();
    this->refreshUsage();
    this->refreshStatus();
    this->refreshControls();
}

void WireGuardVpnSettingsWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange && this->title != nullptr)
        this->retranslate();
}
