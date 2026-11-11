#include "settings/wireguard_vpn_settings_widget.h"

#include "config/connection_wireguard_references.h"
#include "config/project_server_profile_storage.h"
#include "config/wireguard_profile_storage.h"
#include "network/wireguard_runtime_status.h"

#include <aowis/config/object_id.h>
#ifdef AOWIS_GUI_WIREGUARD_PROBE
#include <aowis/network/wireguard_conf.h>
#endif

#include <QAbstractItemView>
#include <QColor>
#include <QDateTime>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSet>
#include <QShowEvent>
#include <QSyntaxHighlighter>
#include <QStringView>
#include <QTextCursor>
#include <QTextBlock>
#include <QScrollBar>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <limits>
#include <string>

namespace
{
constexpr qint64 MaximumImportBytes = 128 * 1024;
constexpr int TelemetryIntervalMs = 1000;

// Shown in place of a hidden PrivateKey value: key-length, not base64, so it
// can never be mistaken for (or accepted as) a real key.
const QString &privateKeyMask()
{
    static const QString mask(44, QChar(0x2022));
    return mask;
}

// Locates the PrivateKey value on one line the way AOWIS-NETWORK's
// parseWireGuardConf() reads it: '#' starts a comment anywhere, keys are
// case-insensitive, value whitespace (including a CRLF '\r') is trimmed.
bool privateKeyValueSpan(const QString &line, qsizetype &begin, qsizetype &end)
{
    qsizetype content_end = line.indexOf(QLatin1Char('#'));
    if (content_end < 0) content_end = line.size();
    const qsizetype separator = line.indexOf(QLatin1Char('='));
    if (separator < 0 || separator >= content_end) return false;
    if (line.left(separator).trimmed().compare(QStringLiteral("PrivateKey"), Qt::CaseInsensitive) != 0)
        return false;
    begin = separator + 1;
    while (begin < content_end && line.at(begin).isSpace()) ++begin;
    end = content_end;
    while (end > begin && line.at(end - 1).isSpace()) --end;
    return true;
}

// Replaces every non-empty PrivateKey value by the mask; *keys receives the
// values in line order. Everything else is kept byte for byte.
QString maskPrivateKeys(const QString &configuration, QStringList *keys)
{
    keys->clear();
    QStringList lines = configuration.split(QLatin1Char('\n'));
    for (QString &line : lines)
    {
        qsizetype begin = 0;
        qsizetype end = 0;
        if (!privateKeyValueSpan(line, begin, end) || end == begin) continue;
        keys->append(line.mid(begin, end - begin));
        line.replace(begin, end - begin, privateKeyMask());
    }
    return lines.join(QLatin1Char('\n'));
}

// Inverse of maskPrivateKeys(): the n-th masked value gets the n-th hidden
// key. A value the user replaced is kept as typed; a deleted line drops its
// key. A surplus mask (duplicated line) stays literal and fails validation.
QString unmaskPrivateKeys(const QString &shown, const QStringList &keys)
{
    QStringList lines = shown.split(QLatin1Char('\n'));
    qsizetype next = 0;
    for (QString &line : lines)
    {
        qsizetype begin = 0;
        qsizetype end = 0;
        if (next >= keys.size() || !privateKeyValueSpan(line, begin, end)) continue;
        if (QStringView(line).mid(begin, end - begin) != privateKeyMask()) continue;
        line.replace(begin, end - begin, keys.at(next++));
    }
    return lines.join(QLatin1Char('\n'));
}
// WireGuard protocol timers (whitepaper §6.1): sessions are renewed after
// REKEY_AFTER_TIME (120 s) while packets flow and rejected after
// REJECT_AFTER_TIME (180 s). With persistent keepalive there is always
// traffic, so a handshake older than 180 s means renewal is failing.
constexpr qint64 RejectAfterTimeSeconds = 180;
// Initial handshake retries every ~5 s; three silent attempts is a real
// problem rather than a cold start.
constexpr qint64 FirstHandshakeGraceSeconds = 15;
// Shorter windows make rates jump when status signals trigger extra refreshes.
constexpr qint64 MinimumRateWindowMs = 500;


// Pure Qt text formatting: no native networking, third-party editor, or
// configuration normalization. In particular, secrets are never inspected
// beyond recognizing their directive names.
class WireGuardSyntaxHighlighter final : public QSyntaxHighlighter
{
public:
    explicit WireGuardSyntaxHighlighter(QTextDocument *document)
        : QSyntaxHighlighter(document)
    {
    }

    void updatePalette(const QPalette &palette)
    {
        const bool dark = palette.color(QPalette::Base).lightness() < 128;
        this->section_format = QTextCharFormat();
        this->section_format.setForeground(QColor(dark ? "#c5adff" : "#7035a7"));
        this->section_format.setFontWeight(QFont::Bold);
        this->directive_format = QTextCharFormat();
        this->directive_format.setForeground(QColor(dark ? "#86bcff" : "#0755a5"));
        this->directive_format.setFontWeight(QFont::DemiBold);
        this->unrecognized_format = QTextCharFormat();
        this->unrecognized_format.setForeground(QColor(dark ? "#edcb8d" : "#865619"));
        this->value_format = QTextCharFormat();
        this->value_format.setForeground(QColor(dark ? "#8dd6b2" : "#176c4b"));
        this->separator_format = QTextCharFormat();
        this->separator_format.setForeground(QColor(dark ? "#b5c1d1" : "#657082"));
        this->comment_format = QTextCharFormat();
        this->comment_format.setForeground(QColor(dark ? "#9daabb" : "#687587"));
        this->comment_format.setFontItalic(true);
        this->invalid_format = QTextCharFormat();
        this->invalid_format.setForeground(QColor(dark ? "#ff9999" : "#ad2828"));
        this->invalid_format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
        this->invalid_format.setUnderlineColor(QColor(dark ? "#ff9999" : "#ad2828"));
        this->rehighlight();
    }

protected:
    void highlightBlock(const QString &line) override
    {
        // Carry [Interface]/[Peer] context over blank lines and comments.
        this->setCurrentBlockState(this->previousBlockState());
        const qsizetype length = line.size();
        qsizetype begin = 0;
        while (begin < length && line.at(begin).isSpace()) ++begin;
        if (begin == length) return;
        if (line.at(begin) == QLatin1Char('#') || line.at(begin) == QLatin1Char(';'))
        {
            this->setFormat(begin, length - begin, this->comment_format);
            return;
        }

        if (line.at(begin) == QLatin1Char('['))
        {
            const qsizetype close = line.indexOf(QLatin1Char(']'), begin + 1);
            if (close < 0)
            {
                this->setCurrentBlockState(0);
                this->setFormat(begin, length - begin, this->invalid_format);
                return;
            }
            const QString section = line.mid(begin + 1, close - begin - 1).trimmed();
            const bool recognized = section == QStringLiteral("Interface")
                || section == QStringLiteral("Peer");
            this->setCurrentBlockState(section == QStringLiteral("Interface") ? 1
                                       : section == QStringLiteral("Peer") ? 2 : 0);
            this->setFormat(begin, close - begin + 1,
                            recognized ? this->section_format : this->unrecognized_format);
            qsizetype trailing = close + 1;
            while (trailing < length && line.at(trailing).isSpace()) ++trailing;
            if (trailing < length)
            {
                if (line.at(trailing) == QLatin1Char('#') || line.at(trailing) == QLatin1Char(';'))
                    this->setFormat(trailing, length - trailing, this->comment_format);
                else
                    this->setFormat(trailing, length - trailing, this->invalid_format);
            }
            return;
        }

        const qsizetype separator = line.indexOf(QLatin1Char('='), begin);
        if (separator < 0)
        {
            this->setFormat(begin, length - begin, this->invalid_format);
            return;
        }
        qsizetype key_end = separator;
        while (key_end > begin && line.at(key_end - 1).isSpace()) --key_end;
        if (key_end == begin)
        {
            this->setFormat(begin, length - begin, this->invalid_format);
            return;
        }
        const QString key = line.mid(begin, key_end - begin);
        static const QSet<QString> interface_keys = {
            QStringLiteral("PrivateKey"), QStringLiteral("Address"),
            QStringLiteral("ListenPort"), QStringLiteral("DNS"),
            QStringLiteral("MTU"), QStringLiteral("Table"),
            QStringLiteral("PreUp"), QStringLiteral("PostUp"),
            QStringLiteral("PreDown"), QStringLiteral("PostDown"),
            QStringLiteral("SaveConfig"), QStringLiteral("FwMark")
        };
        static const QSet<QString> peer_keys = {
            QStringLiteral("PublicKey"), QStringLiteral("PresharedKey"),
            QStringLiteral("AllowedIPs"), QStringLiteral("Endpoint"),
            QStringLiteral("PersistentKeepalive")
        };
        const bool recognized = (this->previousBlockState() == 1 && interface_keys.contains(key))
            || (this->previousBlockState() == 2 && peer_keys.contains(key));
        this->setFormat(begin, key_end - begin,
                        recognized ? this->directive_format : this->unrecognized_format);
        this->setFormat(separator, 1, this->separator_format);

        qsizetype value_begin = separator + 1;
        while (value_begin < length && line.at(value_begin).isSpace()) ++value_begin;
        // Treat # or ; as an inline comment only at a whitespace boundary.
        // A character inside a value is not silently reinterpreted as a comment.
        qsizetype comment = -1;
        for (qsizetype index = value_begin; index < length; ++index)
        {
            if ((line.at(index) == QLatin1Char('#') || line.at(index) == QLatin1Char(';'))
                && (index == value_begin || line.at(index - 1).isSpace()))
            {
                comment = index;
                break;
            }
        }
        qsizetype value_end = comment < 0 ? length : comment;
        while (value_end > value_begin && line.at(value_end - 1).isSpace()) --value_end;
        if (value_begin == value_end)
            this->setFormat(separator, 1, this->invalid_format);
        else
            this->setFormat(value_begin, value_end - value_begin, this->value_format);
        if (comment >= 0)
            this->setFormat(comment, length - comment, this->comment_format);
    }

private:
    QTextCharFormat section_format;
    QTextCharFormat directive_format;
    QTextCharFormat unrecognized_format;
    QTextCharFormat value_format;
    QTextCharFormat separator_format;
    QTextCharFormat comment_format;
    QTextCharFormat invalid_format;
};

QLabel *wrappedLabel(QWidget *parent)
{
    QLabel *label = new QLabel(parent);
    label->setWordWrap(true);
    return label;
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
    QHBoxLayout *damaged_row = new QHBoxLayout();
    this->damaged_notice = wrappedLabel(this);
    this->damaged_notice->setObjectName(QStringLiteral("wireguardVpnDamagedNotice"));
    this->remove_damaged_button = new QPushButton(this);
    this->remove_damaged_button->setObjectName(QStringLiteral("wireguardVpnRemoveDamaged"));
    damaged_row->addWidget(this->damaged_notice, 1);
    damaged_row->addWidget(this->remove_damaged_button);
    outer->addLayout(damaged_row);
    this->damaged_notice->hide();
    this->remove_damaged_button->hide();

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

    // Tab order: Status (index 0), Configuration (1), Used by (2).
    QWidget *status_tab = new QWidget(this->detail_tabs);
    QVBoxLayout *status_layout = new QVBoxLayout(status_tab);
    this->status_info = wrappedLabel(status_tab);
    this->status_info->setObjectName(QStringLiteral("wireguardVpnStatusInfo"));
    status_layout->addWidget(this->status_info);
    this->peer_telemetry_notice = wrappedLabel(status_tab);
    this->peer_telemetry_notice->setObjectName(QStringLiteral("wireguardVpnPeerTelemetryNotice"));
    this->peer_telemetry_notice->hide();
    status_layout->addWidget(this->peer_telemetry_notice);
    QFormLayout *status_fields = new QFormLayout();
    // Caption object names are resolved in retranslate().
    const std::function<QLabel *(const QString &, const QString &)> addStatusRow =
        [status_tab, status_fields](const QString &value_name, const QString &caption_name) -> QLabel *
    {
        QLabel *value = wrappedLabel(status_tab);
        value->setObjectName(value_name);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        QLabel *caption = new QLabel(status_tab);
        caption->setObjectName(caption_name);
        status_fields->addRow(caption, value);
        return value;
    };
    this->tunnel_state = addStatusRow(QStringLiteral("wireguardVpnTunnelState"), QStringLiteral("wireguardVpnStateCaption"));
    this->active_consumers = addStatusRow(QStringLiteral("wireguardVpnActiveConsumers"), QStringLiteral("wireguardVpnConsumersCaption"));
    this->last_handshake = addStatusRow(QStringLiteral("wireguardVpnLastHandshake"), QStringLiteral("wireguardVpnHandshakeCaption"));
    this->traffic = addStatusRow(QStringLiteral("wireguardVpnTraffic"), QStringLiteral("wireguardVpnTrafficCaption"));
    this->throughput = addStatusRow(QStringLiteral("wireguardVpnThroughput"), QStringLiteral("wireguardVpnThroughputCaption"));
    this->peer_endpoint = addStatusRow(QStringLiteral("wireguardVpnPeerEndpoint"), QStringLiteral("wireguardVpnPeerEndpointCaption"));
    this->keepalive = addStatusRow(QStringLiteral("wireguardVpnKeepalive"), QStringLiteral("wireguardVpnKeepaliveCaption"));
    this->uptime = addStatusRow(QStringLiteral("wireguardVpnUptime"), QStringLiteral("wireguardVpnUptimeCaption"));
    this->relays = addStatusRow(QStringLiteral("wireguardVpnRelays"), QStringLiteral("wireguardVpnRelaysCaption"));
    status_layout->addLayout(status_fields);
    this->tunnel_error = wrappedLabel(status_tab);
    status_layout->addWidget(this->tunnel_error);
    status_layout->addStretch();
    this->telemetry_timer = new QTimer(this);
    this->telemetry_timer->setInterval(TelemetryIntervalMs);
    connect(this->telemetry_timer, &QTimer::timeout, this, [this] { refreshStatus(); });
    this->telemetry_clock.start();
    this->detail_tabs->addTab(status_tab, QString());

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
    QHBoxLayout *editor_header = new QHBoxLayout();
    this->editor_caption = wrappedLabel(configuration_tab);
    this->editor_caption->setObjectName(QStringLiteral("wireguardVpnEditorCaption"));
    this->private_key_button = new QPushButton(configuration_tab);
    this->private_key_button->setObjectName(QStringLiteral("wireguardVpnShowPrivateKey"));
    this->private_key_button->setCheckable(true);
    editor_header->addWidget(this->editor_caption, 1);
    editor_header->addWidget(this->private_key_button);
    config_layout->addLayout(editor_header);
    this->configuration_edit = new QPlainTextEdit(configuration_tab);
    this->configuration_edit->setObjectName(QStringLiteral("wireguardVpnEditor"));
    this->configuration_edit->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont monospace = this->configuration_edit->font();
    monospace.setStyleHint(QFont::Monospace);
    monospace.setFamily(QStringLiteral("monospace"));
    this->configuration_edit->setFont(monospace);
    this->configuration_highlighter = new WireGuardSyntaxHighlighter(this->configuration_edit->document());
    static_cast<WireGuardSyntaxHighlighter *>(this->configuration_highlighter)
        ->updatePalette(this->configuration_edit->palette());
    this->configuration_edit->installEventFilter(this);
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
    connect(this->remove_damaged_button, &QPushButton::clicked, this, &WireGuardVpnSettingsWidget::removeDamagedProfiles);
    connect(this->save_button, &QPushButton::clicked, this, &WireGuardVpnSettingsWidget::saveProfile);
    connect(this->revert_button, &QPushButton::clicked, this, &WireGuardVpnSettingsWidget::revertProfile);
    connect(this->profile_list, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *, QListWidgetItem *) { onSelectionChanged(); });
    connect(this->name_edit, &QLineEdit::textChanged, this, [this] { refreshControls(); });
    connect(this->description_edit, &QPlainTextEdit::textChanged, this, [this] { refreshControls(); });
    connect(this->configuration_edit, &QPlainTextEdit::textChanged, this, [this] { refreshControls(); });
    connect(this->private_key_button, &QPushButton::toggled, this, &WireGuardVpnSettingsWidget::setPrivateKeyVisible);
    connect(this->usage_table, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item, int) {
        if (item != nullptr) emit serviceActivated(item->data(0, Qt::UserRole).toString());
    });
    connect(&WireGuardRuntimeStatus::instance(), &WireGuardRuntimeStatus::statusChanged,
            this, [this](const QString &profile_id) {
        if (findProfile(profile_id) == nullptr) return;
        this->refreshProfileList();
        if (this->selected_id == profile_id) this->refreshStatus();
    });
    this->retranslate();
    this->refreshFromStorage();
    this->loadSelection();
}

void WireGuardVpnSettingsWidget::refreshFromStorage()
{
    // Decide on the old model whether the editor holds unsaved work.
    const bool keep_editor = hasChanges();
    QList<Profile> drafts;
    for (const Profile &profile : this->profiles)
        if (profile.draft) drafts.append(profile);

    QList<Profile> stored;
    QString error;
    this->unreadable_profiles = 0;
    if (const aowis::config::WireGuardProfiles *storage = guiWireGuardProfiles(&error))
    {
        const aowis::config::Result<aowis::config::WireGuardProfileListing> listing = storage->list();
        if (listing)
        {
            this->unreadable_profiles = static_cast<int>(listing.value().unreadable.size());
            for (const aowis::config::WireGuardProfileSummary &summary : listing.value().profiles)
            {
                const aowis::config::Result<aowis::config::WireGuardProfile> full = storage->read(summary.info.id);
                if (!full)
                {
                    ++this->unreadable_profiles;
                    continue;
                }
                stored.append({QString::fromStdString(summary.info.id.value()),
                               QString::fromStdString(full.value().summary.info.revision),
                               QString::fromStdString(full.value().summary.name),
                               QString::fromStdString(full.value().summary.description),
                               QString::fromStdString(full.value().configuration), false});
            }
        }
        else error = QString::fromStdString(listing.error().message);
    }
    std::sort(stored.begin(), stored.end(), [](const Profile &left, const Profile &right)
    {
        const int order = QString::compare(left.name, right.name, Qt::CaseInsensitive);
        return order != 0 ? order < 0 : left.id < right.id;
    });
    this->profiles = stored + drafts;

    // "Used by" counts configured connections, not tunnels or sockets. This
    // also opens the connections collection before any deletion session.
    this->references.clear();
    for (const Profile &profile : stored)
        this->references.insert(profile.id, {});
    for (const ProjectServerProfile &connection : loadProjectServerProfiles())
    {
        if (!this->references.contains(connection.wireguard_profile_id)) continue;
        this->references[connection.wireguard_profile_id].append({connection.id, connection.name,
            QStringLiteral("DVMDB"),
            connection.connection_type == QStringLiteral("local") ? connection.local_path : connection.endpoint});
    }

    if (findProfile(this->selected_id) == nullptr)
        this->selected_id = this->profiles.isEmpty() ? QString() : this->profiles.first().id;
    if (!error.isEmpty())
    {
        this->damaged_notice->setText(tr("WireGuard profiles cannot be loaded: %1").arg(error));
        this->damaged_notice->show();
        this->remove_damaged_button->hide();
    }
    else if (this->unreadable_profiles > 0)
    {
        this->damaged_notice->setText(tr("%n stored WireGuard profile(s) cannot be read and are not listed.", "",
                                         this->unreadable_profiles));
        this->damaged_notice->show();
        this->remove_damaged_button->show();
    }
    else
    {
        this->damaged_notice->hide();
        this->remove_damaged_button->hide();
    }
    this->refreshProfileList();
    if (keep_editor)
    {
        this->refreshUsage();
        this->refreshStatus();
        this->refreshControls();
    }
    else this->loadSelection();
}

void WireGuardVpnSettingsWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // Connections (Projects) and profiles (inspector import) change elsewhere.
    if (!event->spontaneous()) this->refreshFromStorage();
    this->refreshStatus(); // (Re)starts telemetry polling when a tunnel runs.
}

void WireGuardVpnSettingsWidget::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    // Never poll the engine for a page nobody sees.
    this->telemetry_timer->stop();
}

void WireGuardVpnSettingsWidget::reportStorageError(const QString &title, const QString &message)
{
    QMessageBox::warning(this, title, message);
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
    profile.id = QStringLiteral("draft:") + QUuid::createUuid().toString(QUuid::WithoutBraces);
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
        || profile->configuration != currentConfiguration();
}

QString WireGuardVpnSettingsWidget::currentConfiguration() const
{
    const QString shown = this->configuration_edit->toPlainText();
    return this->private_key_visible ? shown : unmaskPrivateKeys(shown, this->hidden_private_keys);
}

void WireGuardVpnSettingsWidget::showConfiguration(const QString &configuration)
{
    if (this->private_key_visible)
    {
        this->hidden_private_keys.clear();
        this->configuration_edit->setPlainText(configuration);
    }
    else this->configuration_edit->setPlainText(maskPrivateKeys(configuration, &this->hidden_private_keys));
}

void WireGuardVpnSettingsWidget::setPrivateKeyVisible(bool visible)
{
    if (visible == this->private_key_visible) return;
    // Swap the presentation of the same configuration: unsaved edits, the
    // dirty state and the cursor line survive; only the key text changes.
    const QString configuration = currentConfiguration();
    const QTextCursor cursor = this->configuration_edit->textCursor();
    const int line = cursor.blockNumber();
    const int column = cursor.positionInBlock();
    const int scroll = this->configuration_edit->verticalScrollBar()->value();
    this->private_key_visible = visible;
    this->showConfiguration(configuration);
    const QTextBlock block = this->configuration_edit->document()->findBlockByNumber(line);
    if (block.isValid())
    {
        QTextCursor restored(block);
        restored.setPosition(block.position() + std::min(column, std::max(0, block.length() - 1)));
        this->configuration_edit->setTextCursor(restored);
    }
    this->configuration_edit->verticalScrollBar()->setValue(scroll);
    this->refreshPrivateKeyControls();
}

void WireGuardVpnSettingsWidget::refreshPrivateKeyControls()
{
    {
        const QSignalBlocker blocker(this->private_key_button);
        this->private_key_button->setChecked(this->private_key_visible);
    }
    this->private_key_button->setText(this->private_key_visible ? tr("Hide private key") : tr("Show/edit private key"));
    this->editor_caption->setText(this->private_key_visible
        ? tr("WireGuard .conf (the private key is visible)")
        : tr("WireGuard .conf (the private key is hidden and kept unchanged when you save)"));
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
        QString reference_text = tr("Not saved yet");
        if (this->references.contains(profile.id))
        {
            const qsizetype count = this->references.value(profile.id).size();
            reference_text = count == 0 ? tr("Not used")
                : count == 1 ? tr("Used by 1 connection")
                             : tr("Used by %1 connections").arg(static_cast<qlonglong>(count));
        }
        QString state_text;
        const WireGuardRuntimeStatus::Snapshot runtime = WireGuardRuntimeStatus::instance().snapshot(profile.id);
        switch (runtime.state)
        {
            case WireGuardRuntimeStatus::State::NotStarted: state_text = tr("Not started"); break;
            case WireGuardRuntimeStatus::State::Stopped: state_text = tr("Stopped"); break;
            case WireGuardRuntimeStatus::State::Starting: state_text = tr("Starting"); break;
            case WireGuardRuntimeStatus::State::Running: state_text = tr("Running"); break;
            case WireGuardRuntimeStatus::State::Error: state_text = tr("Error"); break;
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
    // Every newly shown profile starts with its private key hidden.
    this->private_key_visible = false;
    this->showConfiguration(profile == nullptr ? QString() : profile->configuration);
    this->loading_editor = false;
    this->refreshPrivateKeyControls();
    this->detail_tabs->setVisible(profile != nullptr);
    this->empty_notice->setVisible(profile == nullptr);
    this->refreshUsage();
    this->refreshStatus();
    this->refreshControls();
}

QString WireGuardVpnSettingsWidget::configurationValidation(const QString &configuration, QString *notes) const
{
    if (notes != nullptr) notes->clear();
    const QByteArray bytes = configuration.toUtf8();
    if (bytes.trimmed().isEmpty()) return tr("Enter or import a WireGuard configuration.");
    if (static_cast<std::size_t>(bytes.size()) > aowis::config::max_wireguard_configuration_bytes)
        return tr("The configuration exceeds the 2 MiB limit.");
    if (bytes.contains('\0')) return tr("The configuration must not contain NUL characters.");
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    // The parser tunnels use: what passes here is what the engine can run.
    const aowis::network::WireGuardConfParse parsed =
        aowis::network::parseWireGuardConf(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
    QStringList warnings;
    QString error;
    for (const aowis::network::WireGuardConfDiagnostic &diagnostic : parsed.diagnostics)
    {
        const QString message = QString::fromStdString(diagnostic.message);
        const QString located = diagnostic.line > 0
            ? tr("Line %1: %2").arg(static_cast<qulonglong>(diagnostic.line)).arg(message) : message;
        if (diagnostic.severity == aowis::network::WireGuardConfSeverity::error)
        {
            if (error.isEmpty()) error = located;
        }
        else if (warnings.size() < 5) warnings.append(located);
    }
    if (notes != nullptr) *notes = warnings.join(QLatin1Char('\n'));
    if (!parsed.ok()) return error.isEmpty() ? tr("The embedded WireGuard client cannot use this profile.") : error;
#endif
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
    QString notes;
    const QString error = configurationValidation(currentConfiguration(), &notes);
    bool unique_name = !this->name_edit->text().trimmed().isEmpty();
    for (const Profile &candidate : this->profiles)
    {
        if (candidate.id != this->selected_id
            && candidate.name.compare(this->name_edit->text().trimmed(), Qt::CaseInsensitive) == 0)
            unique_name = false;
    }
    this->save_button->setEnabled(dirty && unique_name && error.isEmpty());
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    const QString accepted = tr("The embedded WireGuard client can use this profile. This is not a connectivity check.");
#else
    const QString accepted = tr("This build has no embedded WireGuard client; the profile is stored as written.");
#endif
    this->validation_label->setText(!error.isEmpty() ? error
        : notes.isEmpty() ? accepted : accepted + QLatin1Char('\n') + notes);
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
    const QString config = currentConfiguration();
    const QString error = configurationValidation(config);
    if (!error.isEmpty())
    {
        QMessageBox::warning(this, tr("Invalid configuration"), error);
        return;
    }
    QString storage_error;
    const aowis::config::WireGuardProfiles *storage = guiWireGuardProfiles(&storage_error);
    if (storage == nullptr)
    {
        reportStorageError(tr("Profile not saved"), storage_error);
        return;
    }
    const aowis::config::WireGuardProfileInput input{name.toStdString(),
        this->description_edit->toPlainText().toStdString(), config.toStdString()};
    QString saved_id;
    if (profile->draft)
    {
        const aowis::config::Result<aowis::config::WireGuardProfile> created = storage->importText(input);
        if (!created)
        {
            reportStorageError(tr("Profile not saved"), QString::fromStdString(created.error().message));
            return;
        }
        saved_id = QString::fromStdString(created.value().summary.info.id.value());
        const QString draft_id = profile->id;
        for (qsizetype index = 0; index < this->profiles.size(); ++index)
        {
            if (this->profiles.at(index).id == draft_id)
            {
                this->profiles.removeAt(index);
                break;
            }
        }
    }
    else
    {
        const aowis::config::Result<aowis::config::ObjectId> id = aowis::config::ObjectId::parse(profile->id.toStdString());
        if (!id) return;
        const aowis::config::Result<aowis::config::WireGuardProfile> updated =
            storage->update(id.value(), profile->revision.toStdString(), input);
        if (!updated)
        {
            if (updated.error().code == aowis::config::ErrorCode::conflict)
            {
                reportStorageError(tr("Profile changed elsewhere"),
                    tr("This profile was changed by another AOWIS window. Your edits were not saved; the stored version is shown."));
                // Drop only this stale entry, so the stored version replaces
                // the editor while other unsaved drafts stay.
                const QString stale_id = profile->id;
                for (qsizetype index = 0; index < this->profiles.size(); ++index)
                {
                    if (this->profiles.at(index).id == stale_id)
                    {
                        this->profiles.removeAt(index);
                        break;
                    }
                }
                this->selected_id = stale_id;
                this->refreshFromStorage();
                this->loadSelection();
                return;
            }
            reportStorageError(tr("Profile not saved"), QString::fromStdString(updated.error().message));
            return;
        }
        saved_id = profile->id;
    }
    this->selected_id = saved_id;
    this->refreshFromStorage();
    this->loadSelection();
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
    if (profile->draft)
    {
        this->revertProfile(); // An unsaved draft exists only on this page.
        return;
    }
    // Fresh usage before asking (and the connections collection is opened
    // here, before the deletion session below).
    const QString id_text = profile->id;
    this->refreshFromStorage();
    profile = findProfile(id_text);
    if (profile == nullptr) return;
    if (!this->references.value(profile->id).isEmpty())
    {
        QMessageBox::warning(this, tr("Profile in use"),
            tr("This profile is used by configured connections. Change those connections before deleting it."));
        this->detail_tabs->setCurrentIndex(2); // Used by
        return;
    }
    if (QMessageBox::question(this, tr("Delete WireGuard profile?"),
        tr("Delete '%1'? Its stored configuration, including the private key, is removed.").arg(profile->name),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    QString storage_error;
    const aowis::config::WireGuardProfiles *storage = guiWireGuardProfiles(&storage_error);
    const aowis::config::Result<aowis::config::ObjectId> id = aowis::config::ObjectId::parse(profile->id.toStdString());
    if (storage == nullptr || !id)
    {
        reportStorageError(tr("Profile not deleted"), storage_error);
        return;
    }
    // The usage count runs inside the deleting owner session, so no other
    // AOWIS instance can assign the profile between the check and the delete.
    const aowis::config::Result<bool> erased = storage->erase(id.value(), profile->revision.toStdString(),
        [](const aowis::config::OwnerSession &session, const aowis::config::ObjectId &profile_id) {
            return countConnectionsUsingWireGuardProfile(session, QString::fromStdString(profile_id.value()));
        });
    if (!erased)
    {
        reportStorageError(erased.error().code == aowis::config::ErrorCode::profile_in_use
                ? tr("Profile in use") : tr("Profile not deleted"),
            erased.error().code == aowis::config::ErrorCode::profile_in_use
                ? tr("A configured connection now uses this profile.")
                : QString::fromStdString(erased.error().message));
        this->refreshFromStorage();
        return;
    }
    this->selected_id.clear();
    this->refreshFromStorage();
    this->loadSelection();
}

void WireGuardVpnSettingsWidget::removeDamagedProfiles()
{
    if (QMessageBox::question(this, tr("Remove damaged profiles?"),
        tr("Remove stored WireGuard profiles that cannot be read? Profiles written by a newer AOWIS version are kept."),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    QString storage_error;
    const aowis::config::WireGuardProfiles *storage = guiWireGuardProfiles(&storage_error);
    if (storage == nullptr)
    {
        reportStorageError(tr("Profiles not removed"), storage_error);
        return;
    }
    static_cast<void>(loadProjectServerProfiles()); // Opens connections before the sessions below.
    const aowis::config::Result<aowis::config::WireGuardProfileListing> listing = storage->list();
    int kept = 0;
    if (listing)
    {
        for (const aowis::config::UnreadableObject &damaged : listing.value().unreadable)
        {
            if (!damaged.id.has_value())
            {
                ++kept;
                continue;
            }
            const aowis::config::Result<bool> removed = storage->eraseUnreadable(*damaged.id,
                [](const aowis::config::OwnerSession &session, const aowis::config::ObjectId &profile_id) {
                    return countConnectionsUsingWireGuardProfile(session, QString::fromStdString(profile_id.value()));
                });
            if (!removed) ++kept;
        }
    }
    if (kept > 0)
        reportStorageError(tr("Profiles not removed"),
            tr("%n damaged profile(s) were kept: they are used by connections, written by a newer AOWIS version, or not removable.",
               "", kept));
    this->refreshFromStorage();
}

void WireGuardVpnSettingsWidget::refreshUsage()
{
    this->usage_table->clear();
    if (this->selected_id.isEmpty()) return;
    if (!this->references.contains(this->selected_id))
    {
        this->usage_info->setText(tr("Save the profile before connections can use it."));
        return;
    }
    const QList<ServiceReference> services = this->references.value(this->selected_id);
    this->usage_info->setText(services.isEmpty()
        ? tr("No configured connection uses this profile. Choose it in Projects → Connection → WireGuard.")
        : services.size() == 1
            ? tr("Used by 1 configured connection. This does not imply an active tunnel.")
            : tr("Used by %1 configured connections. This does not imply an active tunnel.")
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
    const WireGuardRuntimeStatus::Snapshot status =
        WireGuardRuntimeStatus::instance().snapshot(this->selected_id);
    this->tunnel_error->setText(status.last_error);
    this->tunnel_error->setVisible(!status.last_error.isEmpty());
    // Connections share the profile's single tunnel; list who uses it now.
    QStringList users = WireGuardRuntimeStatus::instance().connections(this->selected_id);
    for (QString &user : users)
        if (user.isEmpty()) user = tr("Unnamed connection");
    this->active_consumers->setText(users.isEmpty() ? tr("None") : users.join(QStringLiteral("\n")));
    switch (status.state)
    {
        case WireGuardRuntimeStatus::State::NotStarted:
            this->tunnel_state->setText(tr("Not started"));
            this->status_info->setText(tr("No tunnel has been started for this profile."));
            break;
        case WireGuardRuntimeStatus::State::Stopped:
            this->tunnel_state->setText(tr("Stopped"));
            this->status_info->setText(tr("No tunnel is currently running for this profile."));
            break;
        case WireGuardRuntimeStatus::State::Starting:
            this->tunnel_state->setText(tr("Starting"));
            this->status_info->setText(tr("A WireGuard tunnel is starting. Peer reachability is not yet confirmed."));
            break;
        case WireGuardRuntimeStatus::State::Running:
            this->tunnel_state->setText(tr("Running"));
            this->status_info->setText(status.http_reachable_connections > 0
                ? tr("An HTTP response was received through WireGuard.")
                : tr("The local WireGuard tunnel is running. Peer reachability is not yet confirmed."));
            break;
        case WireGuardRuntimeStatus::State::Error:
            this->tunnel_state->setText(tr("Error"));
            this->status_info->setText(tr("The last WireGuard tunnel attempt failed."));
            break;
    }
    const bool running = status.state == WireGuardRuntimeStatus::State::Running;
    this->refreshTelemetry(running);
    if (running && this->isVisible())
    {
        if (!this->telemetry_timer->isActive()) this->telemetry_timer->start();
    }
    else this->telemetry_timer->stop();
}

void WireGuardVpnSettingsWidget::refreshTelemetry(bool running)
{
    const QString none = QStringLiteral("—");
    WireGuardRuntimeStatus::TunnelTelemetry tunnel;
    const bool sampled = running && WireGuardRuntimeStatus::instance().telemetry(this->selected_id, tunnel);
    QStringList hints;
    if (!sampled)
    {
        for (QLabel *label : {this->last_handshake, this->traffic, this->throughput, this->peer_endpoint,
                              this->keepalive, this->uptime, this->relays})
            label->setText(none);
        this->last_handshake->setToolTip(QString());
        this->rate_sample = RateSample();
        if (running) hints.append(tr("The embedded WireGuard engine did not report telemetry."));
        this->peer_telemetry_notice->setText(hints.join(QStringLiteral("\n")));
        this->peer_telemetry_notice->setVisible(!hints.isEmpty());
        return;
    }

    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const qint64 clock_ms = this->telemetry_clock.elapsed();
    const qint64 uptime_seconds = std::max<qint64>(0, (now_ms - tunnel.started_unix_ms) / 1000);

    // Throughput from consecutive samples of the same tunnel instance; a
    // restarted tunnel or another profile starts measuring afresh.
    RateSample &rate = this->rate_sample;
    if (!rate.has_counters || rate.profile_id != this->selected_id
        || rate.tunnel_started_ms != tunnel.started_unix_ms)
        rate = {this->selected_id, tunnel.started_unix_ms, clock_ms, tunnel.received_bytes, tunnel.sent_bytes,
                0.0, 0.0, true, false};
    else if (clock_ms - rate.sampled_ms >= MinimumRateWindowMs)
    {
        const double seconds = static_cast<double>(clock_ms - rate.sampled_ms) / 1000.0;
        // Counters are monotonic for one tunnel; guard anyway.
        rate.received_per_second = tunnel.received_bytes >= rate.received
            ? static_cast<double>(tunnel.received_bytes - rate.received) / seconds : 0.0;
        rate.sent_per_second = tunnel.sent_bytes >= rate.sent
            ? static_cast<double>(tunnel.sent_bytes - rate.sent) / seconds : 0.0;
        rate.received = tunnel.received_bytes;
        rate.sent = tunnel.sent_bytes;
        rate.sampled_ms = clock_ms;
        rate.has_rate = true;
    }

    if (tunnel.last_handshake_unix_ms > 0)
    {
        const qint64 age_seconds = std::max<qint64>(0, (now_ms - tunnel.last_handshake_unix_ms) / 1000);
        this->last_handshake->setText(tr("%1 ago").arg(this->durationText(age_seconds)));
        this->last_handshake->setToolTip(this->locale().toString(
            QDateTime::fromMSecsSinceEpoch(tunnel.last_handshake_unix_ms), QLocale::LongFormat));
        if (age_seconds > RejectAfterTimeSeconds)
            hints.append(tunnel.persistent_keepalive_seconds > 0
                ? tr("The last handshake is older than 3 minutes although keepalive is enabled. WireGuard renews sessions every 2 minutes, so the peer has probably become unreachable.")
                : tr("No recent handshake: keepalive is off, so WireGuard only renews the session while traffic flows. This is normal for an idle tunnel."));
    }
    else
    {
        this->last_handshake->setText(tr("None yet"));
        this->last_handshake->setToolTip(QString());
        hints.append(uptime_seconds >= FirstHandshakeGraceSeconds
            ? tr("No WireGuard handshake has completed. Check the peer endpoint and both public keys, and that outgoing UDP to the peer is not blocked by a firewall.")
            : tr("Waiting for the first WireGuard handshake…"));
    }
    this->traffic->setText(tr("%1 received · %2 sent")
                               .arg(this->bytesText(tunnel.received_bytes), this->bytesText(tunnel.sent_bytes)));
    this->throughput->setText(rate.has_rate
        ? tr("%1 in · %2 out").arg(this->rateText(rate.received_per_second), this->rateText(rate.sent_per_second))
        : tr("Measuring…"));
    this->peer_endpoint->setText(tunnel.peer_endpoint.isEmpty() ? none : tunnel.peer_endpoint);
    this->keepalive->setText(tunnel.persistent_keepalive_seconds == 0
        ? tr("Off") : tr("Every %1").arg(this->durationText(static_cast<qint64>(tunnel.persistent_keepalive_seconds))));
    this->uptime->setText(this->durationText(uptime_seconds));
    this->relays->setText(tr("%1 open · %2 total · %3 failed")
                              .arg(tunnel.open_relays).arg(tunnel.accepted_relays).arg(tunnel.failed_dials));
    if (tunnel.failed_dials > 0)
        hints.append(tr("Connections through the tunnel failed: %1. The peer did not route them to the service, or the service refused them. Check AllowedIPs and the service address.").arg(tunnel.failed_dials));

    // A completed handshake is cryptographic proof the peer is reachable and
    // holds the matching keys; refine the lifecycle summary accordingly.
    const WireGuardRuntimeStatus::Snapshot status = WireGuardRuntimeStatus::instance().snapshot(this->selected_id);
    if (status.http_reachable_connections == 0 && tunnel.last_handshake_unix_ms > 0)
        this->status_info->setText(tr("The WireGuard peer completed a handshake. No HTTP response has been received through it yet."));
    this->peer_telemetry_notice->setText(hints.join(QStringLiteral("\n")));
    this->peer_telemetry_notice->setVisible(!hints.isEmpty());
}

QString WireGuardVpnSettingsWidget::durationText(qint64 seconds) const
{
    if (seconds < 60) return tr("%1 s").arg(seconds);
    if (seconds < 3600) return tr("%1 min %2 s").arg(seconds / 60).arg(seconds % 60);
    if (seconds < 86400) return tr("%1 h %2 min").arg(seconds / 3600).arg((seconds % 3600) / 60);
    return tr("%1 d %2 h").arg(seconds / 86400).arg((seconds % 86400) / 3600);
}

QString WireGuardVpnSettingsWidget::bytesText(quint64 bytes) const
{
    return this->locale().formattedDataSize(static_cast<qint64>(std::min<quint64>(bytes, static_cast<quint64>(std::numeric_limits<qint64>::max()))));
}

QString WireGuardVpnSettingsWidget::rateText(double bytes_per_second) const
{
    return tr("%1/s").arg(this->locale().formattedDataSize(static_cast<qint64>(bytes_per_second + 0.5)));
}

void WireGuardVpnSettingsWidget::retranslate()
{
    this->title->setText(tr("WireGuard VPN"));
    this->introduction->setText(tr("Import and manage reusable WireGuard profiles. Services can share a profile without creating separate configurations."));
    this->session_notice->setText(tr("Profiles are stored in your AOWIS configuration, including their private keys. The files are readable only by your user account but are not encrypted; treat backups of the configuration as secret."));
    this->remove_damaged_button->setText(tr("Remove damaged…"));
    this->empty_notice->setText(tr("No profile selected. Import a .conf file or create a new profile."));
    this->new_button->setText(tr("New"));
    this->import_button->setText(tr("Import…"));
    this->delete_button->setText(tr("Delete"));
    this->save_button->setText(tr("Save changes"));
    this->revert_button->setText(tr("Revert"));
    this->detail_tabs->setTabText(0, tr("Status"));
    this->detail_tabs->setTabText(1, tr("Configuration"));
    this->detail_tabs->setTabText(2, tr("Used by"));
    this->findChild<QLabel *>(QStringLiteral("wireguardVpnNameCaption"))->setText(tr("Profile name"));
    this->findChild<QLabel *>(QStringLiteral("wireguardVpnDescriptionCaption"))->setText(tr("Description"));
    this->refreshPrivateKeyControls();
    this->findChild<QLabel *>(QStringLiteral("wireguardVpnStateCaption"))->setText(tr("Tunnel"));
    this->findChild<QLabel *>(QStringLiteral("wireguardVpnConsumersCaption"))->setText(tr("Used by now"));
    this->findChild<QLabel *>(QStringLiteral("wireguardVpnTrafficCaption"))->setText(tr("Traffic"));
    this->findChild<QLabel *>(QStringLiteral("wireguardVpnHandshakeCaption"))->setText(tr("Last handshake"));
    this->findChild<QLabel *>(QStringLiteral("wireguardVpnThroughputCaption"))->setText(tr("Throughput"));
    this->findChild<QLabel *>(QStringLiteral("wireguardVpnPeerEndpointCaption"))->setText(tr("Peer endpoint"));
    this->findChild<QLabel *>(QStringLiteral("wireguardVpnKeepaliveCaption"))->setText(tr("Keepalive"));
    this->findChild<QLabel *>(QStringLiteral("wireguardVpnUptimeCaption"))->setText(tr("Uptime"));
    this->findChild<QLabel *>(QStringLiteral("wireguardVpnRelaysCaption"))->setText(tr("Relayed connections"));
    this->usage_table->setHeaderLabels({tr("Service"), tr("Type"), tr("Endpoint")});
    this->refreshProfileList();
    this->refreshUsage();
    this->refreshStatus();
    this->refreshControls();
}

bool WireGuardVpnSettingsWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == this->configuration_edit && this->configuration_highlighter != nullptr
        && (event->type() == QEvent::PaletteChange
            || event->type() == QEvent::ApplicationPaletteChange))
        static_cast<WireGuardSyntaxHighlighter *>(this->configuration_highlighter)
            ->updatePalette(this->configuration_edit->palette());
    return QWidget::eventFilter(watched, event);
}

void WireGuardVpnSettingsWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange && this->title != nullptr)
        this->retranslate();
}
