#include "app/top_control_bar.h"

#include "common/_sizes.h"
#include "config/gui_configuration.h"
#include "config/gui_language.h"
#include <QMessageBox>
#include "config/unit_profile_notifications.h"
#include "config/unit_profile_manager.h"
#include "config/unit_profile_menu.h"
#include "config/shortcut_registry.h"
#include "widgets/combo_checkboxes.h"
#ifdef Q_OS_WASM
#include "widgets/wasm_popup_menu.h"
#endif

#include <aowis/model/hydraulic/hydraulic_simulation_results.h>

#include <functional>
#include <QAbstractButton>
#include <QAction>
#include <QActionGroup>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QPalette>
#include <QPushButton>
#include <QResizeEvent>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonArray>
#include <QPointer>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStringList>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

namespace
{
QString shortcutTooltipToken(const QString &shortcut)
{
    const QStringList parts = shortcut.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    QStringList wrapped;
    for (const QString &part : parts)
        wrapped.append(QStringLiteral("[%1]").arg(part.trimmed()));
    return wrapped.join(QLatin1Char('+'));
}

QString simulationShortcutTooltip()
{
    return shortcutTooltipToken(guiShortcutPresentation(GuiShortcutId::SimulationRun))
        + QStringLiteral("<br>")
        + shortcutTooltipToken(guiShortcutPresentation(GuiShortcutId::SimulationRunAlternate));
}

class TopControlBarContent : public QWidget
{
public:
    explicit TopControlBarContent(QWidget *parent = nullptr)
        : QWidget(parent),
        left_group(new QWidget(this)),
        center_group(new QWidget(this)),
        right_group(new QWidget(this)),
        left_layout(new QHBoxLayout(this->left_group)),
        center_layout(new QHBoxLayout(this->center_group)),
        right_layout(new QHBoxLayout(this->right_group))
    {
        this->left_layout->setContentsMargins(0, 0, 0, 0);
        this->left_layout->setSpacing(10);
        this->center_layout->setContentsMargins(0, 0, 0, 0);
        this->center_layout->setSpacing(10);

        this->right_layout->setContentsMargins(0, 0, 0, 0);
        this->right_layout->setSpacing(10);

        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMinimumHeight(Sizes::TopControlBarHeight);
    }

    QHBoxLayout *leftLayout() const
    {
        return this->left_layout;
    }

    QHBoxLayout *centerLayout() const
    {
        return this->center_layout;
    }

    QHBoxLayout *rightLayout() const
    {
        return this->right_layout;
    }

    QSize sizeHint() const override
    {
        const int width = this->left_group->sizeHint().width()
                          + this->center_group->sizeHint().width()
                          + this->right_group->sizeHint().width()
                          + 64;
        return QSize(width, Sizes::TopControlBarHeight);
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        placeGroups();
    }

    bool event(QEvent *event) override
    {
        const bool handled = QWidget::event(event);
        if (event->type() == QEvent::LayoutRequest)
            placeGroups();
        return handled;
    }

private:
    void placeGroups()
    {
        constexpr int side_margin = 10;
        constexpr int group_spacing = 14;

        const QSize left_size = this->left_group->sizeHint();
        const QSize center_size = this->center_group->sizeHint();
        const QSize right_size = this->right_group->sizeHint();
        const int left_y = qMax(0, (height() - left_size.height()) / 2);
        const int center_y = qMax(0, (height() - center_size.height()) / 2);
        const int right_y = qMax(0, (height() - right_size.height()) / 2);

        const int left_x = side_margin;
        const int right_x = width() - side_margin - right_size.width();
        const int centered_x = (width() - center_size.width()) / 2;
        const int minimum_center_x = left_x + left_size.width() + group_spacing;
        const int maximum_center_x = right_x - group_spacing - center_size.width();

        int center_x = centered_x;
        if (minimum_center_x <= maximum_center_x)
            center_x = qBound(minimum_center_x, centered_x, maximum_center_x);
        else
            center_x = minimum_center_x;

        this->left_group->setGeometry(left_x, left_y, left_size.width(), left_size.height());
        this->center_group->setGeometry(center_x, center_y, center_size.width(), center_size.height());
        this->right_group->setGeometry(right_x, right_y, right_size.width(), right_size.height());
    }

    QWidget *left_group = nullptr;
    QWidget *center_group = nullptr;
    QWidget *right_group = nullptr;
    QHBoxLayout *left_layout = nullptr;
    QHBoxLayout *center_layout = nullptr;
    QHBoxLayout *right_layout = nullptr;
};

QLabel *createCaption(const QString &text, QWidget *parent)
{
    QLabel *label = new QLabel(text, parent);
    QFont font = label->font();

    if (font.pointSizeF() > 0.0)
        font.setPointSizeF(qMax(8.0, font.pointSizeF() - 2.0));
    else
        font.setPixelSize(10);

    label->setFont(font);
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, palette.color(QPalette::PlaceholderText));
    label->setPalette(palette);

    return label;
}

QWidget *createLabeledControl(QLabel *caption, QWidget *control, QWidget *parent)
{
    QWidget *container = new QWidget(parent);
    QVBoxLayout *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(1);
    layout->addWidget(caption);
    layout->addWidget(control);
    return container;
}

QFrame *createSeparator(QWidget *parent)
{
    QFrame *separator = new QFrame(parent);
    separator->setFrameShape(QFrame::VLine);
    separator->setFrameShadow(QFrame::Sunken);
    separator->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    return separator;
}

TopControlBarContent *barContent(QWidget *content)
{
    return static_cast<TopControlBarContent *>(content);
}

QSize toolbarIconButtonSize()
{
    const int extent = Sizes::TopControlBarHeight - 8;
    return QSize(extent, extent);
}

QSize toolbarIconSize()
{
    const int extent = Sizes::TopControlBarHeight - 10;
    return QSize(extent, extent);
}

void configureToolbarIconButton(QAbstractButton *button)
{
    button->setFixedSize(toolbarIconButtonSize());
    button->setIconSize(toolbarIconSize());
    button->setStyleSheet(QStringLiteral("padding: 0;"));
}

void configureStackedToolbarIconButton(QAbstractButton *button)
{
    const int button_extent = toolbarIconButtonSize().width() / 2;
    const int icon_extent = qMax(1, button_extent - 2);
    button->setFixedSize(button_extent, button_extent);
    button->setIconSize(QSize(icon_extent, icon_extent));
    button->setStyleSheet(QStringLiteral("padding: 0;"));
}

QString formatSimulationElapsedTime(quint64 elapsed_s)
{
    const quint64 total_minutes = elapsed_s / 60;
    const quint64 hours = total_minutes / 60;
    const quint64 minutes = total_minutes % 60;
    return QStringLiteral("%1:%2")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'));
}
}

TopControlBar::TopControlBar(QWidget *parent)
    : QToolBar(parent),
    content(new TopControlBarContent(this))
{
    setObjectName(QStringLiteral("top_control_bar"));
    setAllowedAreas(Qt::TopToolBarArea);
    setMovable(false);
    setFloatable(false);
    setContextMenuPolicy(Qt::PreventContextMenu);
    setIconSize(toolbarIconSize());
    setFixedHeight(Sizes::TopControlBarHeight);
    setContentsMargins(0, 0, 0, 0);

    if (layout() != nullptr)
        layout()->setContentsMargins(0, 0, 0, 0);

    this->content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    addWidget(this->content);

    addProjectControls();
    addFlowUnitCombo();
    addQualityHeadlossControls();
    addSimulationControls();
    addViewControls();
}

HydraulicHeadlossFormula TopControlBar::selectedSimulationHeadlossFormula() const
{
    if (this->combo_headloss_formula == nullptr || this->combo_headloss_formula->currentIndex() < 0)
        return HydraulicHeadlossFormula::HazenWilliams;

    const HeadlossFormula formula = static_cast<HeadlossFormula>(
        this->combo_headloss_formula->currentData().toInt());

    switch (formula)
    {
    case HeadlossFormula::HazenWilliams:
        return HydraulicHeadlossFormula::HazenWilliams;
    case HeadlossFormula::DarcyWeisbach:
        return HydraulicHeadlossFormula::DarcyWeisbach;
    case HeadlossFormula::ChezyManning:
        return HydraulicHeadlossFormula::ChezyManning;
    case HeadlossFormula::None:
        break;
    }

    return HydraulicHeadlossFormula::HazenWilliams;
}

QList<WaterQualityAnalysisType> TopControlBar::selectedSimulationQualityAnalyses() const
{
    QList<WaterQualityAnalysisType> analyses;
    if (this->combo_quality_analysis == nullptr)
        return analyses;

    const QList<int> indexes_checked = this->combo_quality_analysis->checkedIndexes();
    for (const int index : indexes_checked)
    {
        analyses.append(static_cast<WaterQualityAnalysisType>(
            this->combo_quality_analysis->itemData(index).toInt()));
    }

    return analyses;
}

void TopControlBar::setSelectedSimulationHeadlossFormula(HydraulicHeadlossFormula formula)
{
    if (this->combo_headloss_formula == nullptr)
        return;

    HeadlossFormula gui_formula = HeadlossFormula::HazenWilliams;
    switch (formula)
    {
    case HydraulicHeadlossFormula::HazenWilliams:
        gui_formula = HeadlossFormula::HazenWilliams;
        break;
    case HydraulicHeadlossFormula::DarcyWeisbach:
        gui_formula = HeadlossFormula::DarcyWeisbach;
        break;
    case HydraulicHeadlossFormula::ChezyManning:
        gui_formula = HeadlossFormula::ChezyManning;
        break;
    }

    const int index = this->combo_headloss_formula->findData(static_cast<int>(gui_formula));
    if (index >= 0)
        this->combo_headloss_formula->setCurrentIndex(index);
}

void TopControlBar::setSelectedSimulationQualityAnalyses(
    const QList<WaterQualityAnalysisType> &analyses)
{
    if (this->combo_quality_analysis == nullptr)
        return;

    for (int index = 0; index < this->combo_quality_analysis->count(); ++index)
    {
        const WaterQualityAnalysisType analysis = static_cast<WaterQualityAnalysisType>(
            this->combo_quality_analysis->itemData(index).toInt());
        this->combo_quality_analysis->setItemChecked(index, analyses.contains(analysis));
    }
}

void TopControlBar::setFullScreenState(bool fullscreen)
{
    if (this->button_fullscreen == nullptr)
        return;

    const QString icon_path = fullscreen
        ? QStringLiteral(":/icon/fullscreen_undo.png")
        : QStringLiteral(":/icon/fullscreen.png");
    this->button_fullscreen->setIcon(QIcon(icon_path));

    const QString fullscreen_shortcut = shortcutTooltipToken(
        guiShortcutPresentation(GuiShortcutId::Fullscreen));
    this->button_fullscreen->setToolTip(
        (fullscreen ? tr("Leave fullscreen %1") : tr("Enter fullscreen %1")).arg(fullscreen_shortcut));
}

void TopControlBar::setSimulationResultsAvailable(bool available)
{
    this->simulation_results_available = available;
    updateSimulationStatisticsAvailability();
}

void TopControlBar::updateSimulationStatisticsAvailability()
{
    if (this->button_sim_statistics == nullptr)
        return;

    const bool available =
        this->simulation_results_available || this->simulation_report_available;
    this->button_sim_statistics->setEnabled(available);
    this->button_sim_statistics->setToolTip(
        available
            ? QStringLiteral("Show simulation statistics")
            : QStringLiteral("Show simulation statistics<br>You need to run a simulation first"));
}

void TopControlBar::resetSimulationRunIcon()
{
    if (this->button_sim_start == nullptr)
        return;

    this->button_sim_start->setIcon(QIcon(QStringLiteral(":/icon/simulation_start.png")));
}

void TopControlBar::setSimulationRunRunningIcon()
{
    if (this->button_sim_start == nullptr)
        return;

    this->button_sim_start->setIcon(QIcon(QStringLiteral(":/icon/simulation_stop.png")));
    this->button_sim_start->setToolTip(
        QStringLiteral("Stop Simulation<br>Simulation running...<br>") + simulationShortcutTooltip());
}

void TopControlBar::setSimulationRunStoppingIcon()
{
    if (this->button_sim_start == nullptr)
        return;

    this->button_sim_start->setIcon(QIcon(QStringLiteral(":/icon/simulation_stop.png")));
    this->button_sim_start->setToolTip(
        QStringLiteral("Stopping Simulation...<br>The current EPANET solver step will finish before cancellation."));
}

void TopControlBar::setSimulationRunCancelledIcon()
{
    if (this->button_sim_start == nullptr)
        return;

    this->button_sim_start->setIcon(QIcon(QStringLiteral(":/icon/simulation_start.png")));
    this->button_sim_start->setToolTip(
        QStringLiteral("Run Configured Simulations<br>") + simulationShortcutTooltip()
            + QStringLiteral("<br><br>Last run: Cancelled"));
}

void TopControlBar::setSimulationRunResultIcon(
    const HydraulicSimulationResultTimeline &result_timeline)
{
    if (this->button_sim_start == nullptr)
        return;

    int warning_count = 0;
    int error_count = 0;
    for (const HydraulicSimulationDiagnostic &diagnostic : result_timeline.diagnostics)
    {
        if (diagnostic.severity == HydraulicSimulationDiagnosticSeverity::Warning)
            ++warning_count;
        else if (diagnostic.severity == HydraulicSimulationDiagnosticSeverity::Error
                 || diagnostic.severity == HydraulicSimulationDiagnosticSeverity::Fatal)
            ++error_count;
    }

    QString icon_path;
    QString last_run_status;
    if (!result_timeline.status.success)
    {
        icon_path = QStringLiteral(":/icon/simulation_error.png");
        last_run_status = QStringLiteral("Failed");
    }
    else if (error_count > 0)
    {
        icon_path = QStringLiteral(":/icon/simulation_error.png");
        last_run_status = QStringLiteral("Completed with %1 error%2")
            .arg(error_count)
            .arg(error_count == 1 ? QString() : QStringLiteral("s"));
    }
    else if (result_timeline.validity != HydraulicSimulationResultValidity::Valid)
    {
        icon_path = QStringLiteral(":/icon/simulation_error.png");
        last_run_status = QStringLiteral("Invalid result");
    }
    else if (warning_count > 0)
    {
        icon_path = QStringLiteral(":/icon/simulation_warning.png");
        last_run_status = QStringLiteral("Successful with %1 warning%2")
            .arg(warning_count)
            .arg(warning_count == 1 ? QString() : QStringLiteral("s"));
    }
    else
    {
        icon_path = QStringLiteral(":/icon/simulation_success.png");
        last_run_status = QStringLiteral("Successful");
    }

    this->button_sim_start->setIcon(QIcon(icon_path));
    this->button_sim_start->setToolTip(
        QStringLiteral("Run Configured Simulations<br>") + simulationShortcutTooltip()
            + QStringLiteral("<br><br>Last run: %1").arg(last_run_status));
}

void TopControlBar::setSimulationResultTimeline(
    const HydraulicSimulationResultTimeline &result_timeline)
{
    if (this->combo_sim_timepoint == nullptr)
        return;

    setSimulationPlaybackActive(false);
    const QSignalBlocker blocker(this->combo_sim_timepoint);
    this->combo_sim_timepoint->clear();
    this->simulation_result_count = result_timeline.results.size();

    for (const HydraulicSimulationResult &result : result_timeline.results)
    {
        const QString elapsed_time = formatSimulationElapsedTime(result.time_elapsed_s);
        this->combo_sim_timepoint->addItem(elapsed_time);

        if (result_timeline.simulation_start_utc.isValid())
        {
            const QDateTime timestamp = result_timeline.simulation_start_utc.addSecs(
                static_cast<qint64>(result.time_elapsed_s));
            this->combo_sim_timepoint->setItemData(
                this->combo_sim_timepoint->count() - 1,
                timestamp.toUTC().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'")),
                Qt::ToolTipRole);
        }
    }

    this->combo_sim_timepoint->setCurrentIndex(
        this->simulation_result_count > 0 ? 0 : -1);
    updateSimulationNavigationState();
}

void TopControlBar::clearSimulationResultTimeline()
{
    setSimulationPlaybackActive(false);
    this->simulation_result_count = 0;

    if (this->combo_sim_timepoint != nullptr)
    {
        const QSignalBlocker blocker(this->combo_sim_timepoint);
        this->combo_sim_timepoint->clear();
    }

    updateSimulationNavigationState();
}

void TopControlBar::setCurrentSimulationResultIndex(int result_index)
{
    if (this->combo_sim_timepoint == nullptr)
        return;

    if (result_index < 0 || result_index >= this->simulation_result_count)
    {
        const QSignalBlocker blocker(this->combo_sim_timepoint);
        this->combo_sim_timepoint->setCurrentIndex(-1);
        updateSimulationNavigationState();
        return;
    }

    if (this->combo_sim_timepoint->currentIndex() != result_index)
    {
        const QSignalBlocker blocker(this->combo_sim_timepoint);
        this->combo_sim_timepoint->setCurrentIndex(result_index);
    }

    if (this->simulation_playback_timer != nullptr
        && this->simulation_playback_timer->isActive()
        && result_index >= this->simulation_result_count - 1)
    {
        setSimulationPlaybackActive(false);
    }

    updateSimulationNavigationState();
}

void TopControlBar::updateSimulationNavigationState()
{
    const bool has_results = this->simulation_result_count > 0;
    const int current_index = this->combo_sim_timepoint != nullptr
        ? this->combo_sim_timepoint->currentIndex()
        : -1;

    if (this->combo_sim_timepoint != nullptr)
        this->combo_sim_timepoint->setEnabled(has_results);

    if (this->button_sim_step_previous != nullptr)
        this->button_sim_step_previous->setEnabled(has_results && current_index > 0);

    if (this->button_sim_step_next != nullptr)
    {
        this->button_sim_step_next->setEnabled(
            has_results
            && current_index >= 0
            && current_index < this->simulation_result_count - 1);
    }

    const bool can_play = this->simulation_result_count > 1;
    if (this->button_sim_playback != nullptr)
        this->button_sim_playback->setEnabled(can_play);

    if (this->combo_sim_speed != nullptr)
        this->combo_sim_speed->setEnabled(can_play);
}

void TopControlBar::setSimulationPlaybackActive(bool active)
{
    if (this->simulation_playback_timer == nullptr
        || this->button_sim_playback == nullptr)
    {
        return;
    }

    if (active && this->simulation_result_count > 1)
        this->simulation_playback_timer->start();
    else
        this->simulation_playback_timer->stop();

    const bool playing = this->simulation_playback_timer->isActive();
    this->button_sim_playback->setIcon(
        style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    this->button_sim_playback->setToolTip(
        playing
            ? QStringLiteral("Pause simulation timeline")
            : QStringLiteral("Play simulation timeline"));
}

void TopControlBar::requestSimulationResultIndex(int result_index)
{
    if (result_index < 0 || result_index >= this->simulation_result_count)
        return;

    emit signalSimulationResultIndexSelected(result_index);
}

void TopControlBar::setEpanetLogAvailable(bool available)
{
    this->simulation_report_available = available;
    updateSimulationStatisticsAvailability();
}

void TopControlBar::showSimulationSettings()
{
    if (this->dialog_simulation_settings)
    {
        if (this->dialog_simulation_settings->isMinimized())
        {
            this->dialog_simulation_settings->setWindowState(
                this->dialog_simulation_settings->windowState() & ~Qt::WindowMinimized);
        }

        this->dialog_simulation_settings->show();
        this->dialog_simulation_settings->raise();
        this->dialog_simulation_settings->activateWindow();
        return;
    }

    QWidget *dialog_parent = window();
    this->dialog_simulation_settings = new QDialog(
        dialog_parent,
        Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
    this->dialog_simulation_settings->setAttribute(Qt::WA_DeleteOnClose);
    this->dialog_simulation_settings->setWindowTitle(tr("Simulation Settings"));
    this->dialog_simulation_settings->setModal(false);

    QLabel *hint = new QLabel(
        tr("These settings are used for the next simulation run."),
        this->dialog_simulation_settings);
    hint->setWordWrap(true);

    ComboCheckboxes *quality_analysis = new ComboCheckboxes(
        this->dialog_simulation_settings);
    quality_analysis->setSummaryLimit(2);

    if (this->combo_quality_analysis != nullptr)
    {
        for (int index = 0; index < this->combo_quality_analysis->count(); ++index)
        {
            quality_analysis->addItem(
                this->combo_quality_analysis->itemText(index),
                this->combo_quality_analysis->itemData(index),
                this->combo_quality_analysis->isItemChecked(index),
                QString());
        }
    }

    QComboBox *headloss_formula = new QComboBox(this->dialog_simulation_settings);
    if (this->combo_headloss_formula != nullptr)
    {
        for (int index = 0; index < this->combo_headloss_formula->count(); ++index)
        {
            headloss_formula->addItem(
                this->combo_headloss_formula->itemText(index),
                this->combo_headloss_formula->itemData(index));
            headloss_formula->setItemData(
                index,
                this->combo_headloss_formula->itemData(index, Qt::ToolTipRole),
                Qt::ToolTipRole);
        }

        headloss_formula->setCurrentIndex(this->combo_headloss_formula->currentIndex());
    }

    connect(quality_analysis, &ComboCheckboxes::itemCheckedChanged,
            this, [this](int index, bool checked)
    {
        if (this->combo_quality_analysis == nullptr
            || index < 0
            || index >= this->combo_quality_analysis->count())
        {
            return;
        }

        this->combo_quality_analysis->setItemChecked(index, checked);
    });

    if (this->combo_quality_analysis != nullptr)
    {
        connect(this->combo_quality_analysis, &ComboCheckboxes::itemCheckedChanged,
                this->dialog_simulation_settings,
                [quality_analysis](int index, bool checked)
        {
            if (index < 0 || index >= quality_analysis->count())
                return;

            const QSignalBlocker blocker(quality_analysis);
            quality_analysis->setItemChecked(index, checked);
        });
    }

    connect(headloss_formula, &QComboBox::currentIndexChanged,
            this, [this](int index)
    {
        if (this->combo_headloss_formula == nullptr
            || index < 0
            || index >= this->combo_headloss_formula->count())
        {
            return;
        }

        this->combo_headloss_formula->setCurrentIndex(index);
    });

    if (this->combo_headloss_formula != nullptr)
    {
        connect(this->combo_headloss_formula, &QComboBox::currentIndexChanged,
                this->dialog_simulation_settings,
                [headloss_formula](int index)
        {
            if (index < 0 || index >= headloss_formula->count())
                return;

            const QSignalBlocker blocker(headloss_formula);
            headloss_formula->setCurrentIndex(index);
        });
    }

    QFormLayout *form_layout = new QFormLayout();
    form_layout->addRow(tr("Water quality analyses"), quality_analysis);
    form_layout->addRow(tr("Headloss formula"), headloss_formula);

    QDialogButtonBox *button_box = new QDialogButtonBox(
        QDialogButtonBox::Close,
        this->dialog_simulation_settings);
    connect(button_box, &QDialogButtonBox::rejected,
            this->dialog_simulation_settings, &QDialog::close);

    QVBoxLayout *layout = new QVBoxLayout(this->dialog_simulation_settings);
    layout->addWidget(hint);
    layout->addLayout(form_layout);
    layout->addStretch(1);
    layout->addWidget(button_box);

    this->dialog_simulation_settings->resize(470, 180);
    this->dialog_simulation_settings->show();
    this->dialog_simulation_settings->raise();
    this->dialog_simulation_settings->activateWindow();
}

void TopControlBar::addProjectControls()
{
    TopControlBarContent *bar_content = barContent(this->content);
    QWidget *workspace_context = new QWidget(this->content);
    QVBoxLayout *layout = new QVBoxLayout(workspace_context);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    QHBoxLayout *selectors = new QHBoxLayout();
    selectors->setContentsMargins(0, 0, 0, 0);
    selectors->setSpacing(5);
    this->combo_project = new QComboBox(workspace_context);
    this->combo_project->setObjectName(QStringLiteral("toolbarProjectSelector"));
    this->combo_project->setAccessibleName(tr("Project"));
    this->combo_project->setFixedWidth(160);
    this->combo_project->setMinimumHeight(28);
    this->combo_project->setToolTip(tr("Choose a project to open its hydraulic network"));
    this->combo_branch = new QComboBox(workspace_context);
    this->combo_branch->setObjectName(QStringLiteral("toolbarBranchSelector"));
    this->combo_branch->setAccessibleName(tr("Branch"));
    this->combo_branch->setFixedWidth(134);
    this->combo_branch->setMinimumHeight(28);
    this->combo_branch->setToolTip(tr("Select a branch to open; [none] leaves the current network unchanged"));
    selectors->addWidget(this->combo_project);
    selectors->addWidget(this->combo_branch);

    // Restore the original target-icon map-fit action to the main toolbar.
    // It uses the same signal/handler as before the Projects workspace redesign.
    QToolButton *button_show_on_map = new QToolButton(workspace_context);
    button_show_on_map->setObjectName(QStringLiteral("toolbarShowNetworkOnMap"));
    button_show_on_map->setAutoRaise(true);
    button_show_on_map->setIcon(QIcon(QStringLiteral(":/icon/target.png")));
    button_show_on_map->setIconSize(QSize(28, 28));
    button_show_on_map->setFixedSize(30, 30);
    button_show_on_map->setToolTip(tr("Show network on map"));
    button_show_on_map->setStyleSheet(QStringLiteral("padding: 0;"));
    connect(button_show_on_map, &QToolButton::clicked,
            this, &TopControlBar::signalShowNetworkOnMap);
    selectors->addWidget(button_show_on_map);
    layout->addLayout(selectors);

    QHBoxLayout *actions = new QHBoxLayout();
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(6);
    this->label_modified = createCaption(tr("No network"), workspace_context);
    this->label_modified->setObjectName(QStringLiteral("toolbarWorkingCopyStatus"));
    actions->addWidget(this->label_modified, 1);

    this->button_project_save = new QToolButton(workspace_context);
    this->button_project_save->setIcon(QIcon(QStringLiteral(":/icon/save.png")));
    this->button_project_save->setIconSize(QSize(20, 20));
    this->button_project_save->setFixedSize(27, 25);
    this->button_project_save->setToolTip(tr("Commit from the HEAD Inspector; toolbar Commit is not connected yet"));
    this->button_project_save->setEnabled(false);
    actions->addWidget(this->button_project_save);

    QToolButton *history_button = new QToolButton(workspace_context);
    history_button->setIcon(QIcon(QStringLiteral(":/icon/history.png")));
    history_button->setIconSize(QSize(20, 20));
    history_button->setFixedSize(27, 25);
    history_button->setObjectName(QStringLiteral("toolbarHistoryButton"));
    history_button->setToolTip(tr("Open History dock"));
    connect(history_button, &QToolButton::clicked,
            this, &TopControlBar::signalProjectHistoryRequested);
    actions->addWidget(history_button);
    layout->addLayout(actions);

    connect(this->combo_project, &QComboBox::currentTextChanged,
            this, &TopControlBar::signalProjectSelected);
    connect(this->combo_branch, &QComboBox::currentIndexChanged,
            this, [this](int index)
    {
        if (index > 0)
            emit signalBranchSelected(this->combo_branch->itemData(index).toString());
    });
    connect(this->button_project_save, &QToolButton::clicked,
            this, &TopControlBar::signalProjectSaveRequested);
    connect(this, &TopControlBar::languageChanged, workspace_context, [this, button_show_on_map, history_button] {
        this->combo_project->setAccessibleName(tr("Project"));
        this->combo_branch->setAccessibleName(tr("Branch"));
        this->combo_project->setToolTip(tr("Choose a project to open its hydraulic network"));
        this->combo_branch->setToolTip(tr("Select a branch to open; [none] leaves the current network unchanged"));
        const QSignalBlocker blocker(this->combo_branch);
        this->combo_branch->setItemText(0, tr("[none]"));
        button_show_on_map->setToolTip(tr("Show network on map"));
        history_button->setToolTip(tr("Open History dock"));
    });
    bar_content->leftLayout()->addWidget(workspace_context);
    setProjectContext({}, QString(), {}, QString());
}

void TopControlBar::setProjectContext(const QStringList &projects,
                                      const QString &project,
                                      const QStringList &branches,
                                      const QString &branch)
{
    if (this->combo_project == nullptr || this->combo_branch == nullptr)
        return;
    const QSignalBlocker project_blocker(this->combo_project);
    const QSignalBlocker branch_blocker(this->combo_branch);
    this->available_projects = projects;
    this->available_branches = branches;
    this->combo_project->clear();
    this->combo_project->addItems(projects);
    this->combo_project->setCurrentIndex(projects.indexOf(project));
    this->combo_branch->clear();
    this->combo_branch->addItem(tr("[none]"), QString());
    for (const QString &entry : branches)
        this->combo_branch->addItem(entry, entry);
    this->combo_branch->setCurrentIndex(qMax(0, branches.indexOf(branch) + 1));
    this->combo_branch->setEnabled(!branches.isEmpty());
}

void TopControlBar::setWorkingCopyModified(bool modified)
{
    if (modified) setNetworkStatus(tr("Modified"));
}

void TopControlBar::setNetworkStatus(const QString &status)
{
    if (this->label_modified != nullptr)
    {
        this->label_modified->setText(status);
        this->label_modified->setToolTip(status);
    }
}

void TopControlBar::setOpenedContext(const QString &project, const QString &branch)
{
    this->opened_project = project;
    this->opened_branch = branch;
}

void TopControlBar::restoreOpenedContext()
{
    if (this->combo_project == nullptr || this->combo_branch == nullptr) return;
    const QSignalBlocker project_blocker(this->combo_project);
    const QSignalBlocker branch_blocker(this->combo_branch);
    if (this->opened_project.isEmpty())
    {
        this->combo_project->setCurrentIndex(-1);
        this->combo_branch->setCurrentIndex(0);
        return;
    }
    this->combo_project->setCurrentIndex(this->combo_project->findText(this->opened_project));
    const int branch_index = this->combo_branch->findData(this->opened_branch);
    this->combo_branch->setCurrentIndex(qMax(0, branch_index));
}

void TopControlBar::addFlowUnitCombo()
{
    TopControlBarContent *bar_content = barContent(this->content);
    bar_content->centerLayout()->addWidget(createSeparator(this->content));

    QToolButton *button = new QToolButton(this->content);
    button->setMinimumHeight(30);
    button->setMinimumWidth(82);
    button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);

#ifdef Q_OS_WASM
    WasmPopupMenu *menu = new WasmPopupMenu(button);
    connect(button, &QToolButton::clicked, this, [button, menu] {
        if (menu->isPopupVisible()) menu->closeAll();
        else menu->popupBelow(button);
    });
#else
    button->setPopupMode(QToolButton::InstantPopup);
    QMenu *menu = new QMenu(button);
    button->setMenu(menu);
#endif

    const std::function<void()> refresh = [this, button, menu] {
        UnitProfileManager &manager = UnitProfileManager::instance();
        const QVector<UnitProfileManager::Profile> &profiles = manager.profiles();
        const int active = manager.activeIndex();
        button->setText(profiles.at(active).name + QStringLiteral("  ▾"));
        UnitProfileMenu::populate(menu, this, profiles, active,
                                  [](int index) { (void)UnitProfileManager::instance().select(index); },
                                  [this] { emit signalManageUnitsRequested(); });
    };
    UnitProfileNotifications::instance().subscribe(this, [this, refresh] {
        QTimer::singleShot(0, this, refresh);
    });
    refresh();
    QLabel *caption = createCaption(tr("Units"), this->content);
    connect(this, &TopControlBar::languageChanged, caption, [caption] { caption->setText(tr("Units")); });
    bar_content->centerLayout()->addWidget(createLabeledControl(caption, button, this->content));
}

void TopControlBar::addQualityHeadlossControls()
{
    TopControlBarContent *bar_content = barContent(this->content);

    this->combo_quality_analysis = new ComboCheckboxes(this->content);
    this->combo_quality_analysis->setFixedSize(190, 30);
    this->combo_quality_analysis->setSummaryLimit(2);

    this->combo_quality_analysis->addItem(
        QStringLiteral("CHEMICAL"),
        static_cast<int>(WaterQualityAnalysisType::Chemical),
        true,
        QStringLiteral("One dissolved constituent concentration, e.g. chlorine"));
    this->combo_quality_analysis->addItem(
        QStringLiteral("AGE"),
        static_cast<int>(WaterQualityAnalysisType::WaterAge),
        true,
        QStringLiteral("Water age"));
    this->combo_quality_analysis->addItem(
        QStringLiteral("TRACE"),
        static_cast<int>(WaterQualityAnalysisType::SourceTrace),
        true,
        QStringLiteral("Source tracing: percent of water originating from one node"));

    this->combo_headloss_formula = new QComboBox(this->content);
    this->combo_headloss_formula->setFixedSize(190, 30);
    this->combo_headloss_formula->addItem(
        QStringLiteral("Hazen-Williams"),
        static_cast<int>(HeadlossFormula::HazenWilliams));
    this->combo_headloss_formula->setItemData(
        this->combo_headloss_formula->count() - 1,
        QStringLiteral("Run simulation with the Hazen-Williams headloss formula.<br><br>Requires pipe roughness coefficient C."),
        Qt::ToolTipRole);
    this->combo_headloss_formula->addItem(
        QStringLiteral("Darcy-Weisbach"),
        static_cast<int>(HeadlossFormula::DarcyWeisbach));
    this->combo_headloss_formula->setItemData(
        this->combo_headloss_formula->count() - 1,
        QStringLiteral("Run simulation with the Darcy-Weisbach headloss formula.<br><br>Requires an absolute pipe roughness ε value."),
        Qt::ToolTipRole);
    this->combo_headloss_formula->addItem(
        QStringLiteral("Chezy-Manning"),
        static_cast<int>(HeadlossFormula::ChezyManning));
    this->combo_headloss_formula->setItemData(
        this->combo_headloss_formula->count() - 1,
        QStringLiteral("Run simulation with the Chezy-Manning headloss formula.<br><br>Requires Manning roughness coefficient n."),
        Qt::ToolTipRole);

    connect(this->combo_headloss_formula, &QComboBox::currentIndexChanged,
            this, [this](int index)
    {
        HeadlossFormulas formulas = HeadlossFormula::None;
        if (index >= 0)
        {
            const HeadlossFormula formula = static_cast<HeadlossFormula>(
                this->combo_headloss_formula->itemData(index).toInt());
            formulas |= formula;
        }

        emit signalHeadlossFormulaChanged(formulas);
    });

    QWidget *quality_headloss_container = new QWidget(this->content);
    QVBoxLayout *quality_headloss_layout =
        new QVBoxLayout(quality_headloss_container);
    quality_headloss_layout->setContentsMargins(0, 0, 0, 0);
    quality_headloss_layout->setSpacing(2);

    QLabel *quality_caption =
        createCaption(QString(), quality_headloss_container);
    QLabel *headloss_caption =
        createCaption(QString(), quality_headloss_container);
    const std::function<void()> refresh_captions = [quality_caption, headloss_caption] {
        quality_caption->setText(tr("Water quality"));
        headloss_caption->setText(tr("Headloss"));
        const int width = qMax(quality_caption->fontMetrics().horizontalAdvance(quality_caption->text()),
                               headloss_caption->fontMetrics().horizontalAdvance(headloss_caption->text()));
        quality_caption->setFixedWidth(width + 2);
        headloss_caption->setFixedWidth(width + 2);
    };
    connect(this, &TopControlBar::languageChanged, quality_caption, refresh_captions);
    refresh_captions();

    QHBoxLayout *quality_layout = new QHBoxLayout();
    quality_layout->setContentsMargins(0, 0, 0, 0);
    quality_layout->setSpacing(6);
    quality_layout->addWidget(quality_caption);
    quality_layout->addWidget(this->combo_quality_analysis);

    QHBoxLayout *headloss_layout = new QHBoxLayout();
    headloss_layout->setContentsMargins(0, 0, 0, 0);
    headloss_layout->setSpacing(6);
    headloss_layout->addWidget(headloss_caption);
    headloss_layout->addWidget(this->combo_headloss_formula);

    quality_headloss_layout->addLayout(quality_layout);
    quality_headloss_layout->addLayout(headloss_layout);
    bar_content->centerLayout()->addWidget(quality_headloss_container);
}

void TopControlBar::addSimulationControls()
{
    TopControlBarContent *bar_content = barContent(this->content);

    this->button_sim_start = new QPushButton(this->content);
    resetSimulationRunIcon();
    this->button_sim_start->setFlat(true);
    configureToolbarIconButton(this->button_sim_start);
    this->button_sim_start->setToolTip(
        QStringLiteral("Run Configured Simulations<br>") + simulationShortcutTooltip());

    QAction *simulation_alternate_action = this->button_sim_start->addAction(
        QString(),
        guiShortcutRegistry().keySequence(GuiShortcutId::SimulationRunAlternate),
        this->button_sim_start,
        &QPushButton::click);
    QAction *simulation_primary_action = this->button_sim_start->addAction(
        QString(),
        guiShortcutRegistry().keySequence(GuiShortcutId::SimulationRun),
        this->button_sim_start,
        &QPushButton::click);

    installShortcutEditContextMenu(
        this->button_sim_start,
        GuiShortcutId::SimulationRun);

    connect(&guiShortcutRegistry(), &GuiShortcutRegistry::shortcutChanged,
            this, [this, simulation_primary_action, simulation_alternate_action](GuiShortcutId id)
    {
        if (id == GuiShortcutId::SimulationRun)
        {
            simulation_primary_action->setShortcut(
                guiShortcutRegistry().shortcutCaptureActive()
                    ? QKeySequence()
                    : guiShortcutRegistry().keySequence(GuiShortcutId::SimulationRun));
        }
        else if (id == GuiShortcutId::SimulationRunAlternate)
        {
            simulation_alternate_action->setShortcut(
                guiShortcutRegistry().shortcutCaptureActive()
                    ? QKeySequence()
                    : guiShortcutRegistry().keySequence(GuiShortcutId::SimulationRunAlternate));
        }
        else
        {
            return;
        }

        this->button_sim_start->setToolTip(
            QStringLiteral("Run Configured Simulations<br>") + simulationShortcutTooltip());
    });

    connect(&guiShortcutRegistry(), &GuiShortcutRegistry::shortcutCaptureActiveChanged,
            this, [simulation_primary_action, simulation_alternate_action](bool active)
    {
        simulation_primary_action->setShortcut(
            active
                ? QKeySequence()
                : guiShortcutRegistry().keySequence(GuiShortcutId::SimulationRun));
        simulation_alternate_action->setShortcut(
            active
                ? QKeySequence()
                : guiShortcutRegistry().keySequence(GuiShortcutId::SimulationRunAlternate));
    });

    QWidget *result_button_stack = new QWidget(this->content);
    QVBoxLayout *result_button_stack_layout = new QVBoxLayout(result_button_stack);
    result_button_stack_layout->setContentsMargins(0, 0, 0, 0);
    result_button_stack_layout->setSpacing(0);

    this->button_sim_statistics = new QToolButton(result_button_stack);
    this->button_sim_statistics->setAutoRaise(true);
    this->button_sim_statistics->setIcon(
        QIcon(QStringLiteral(":/icon/dashboard.png")));
    configureStackedToolbarIconButton(this->button_sim_statistics);

    this->button_sim_settings = new QToolButton(result_button_stack);
    this->button_sim_settings->setAutoRaise(true);
    this->button_sim_settings->setIcon(
        QIcon(QStringLiteral(":/icon/settings.png")));
    this->button_sim_settings->setToolTip(QStringLiteral("Simulation settings"));
    configureStackedToolbarIconButton(this->button_sim_settings);

    const int control_height = toolbarIconButtonSize().height();
    const int half_height = control_height / 2;
    const int timeline_width = 104;

    QWidget *timeline_navigation = new QWidget(this->content);
    timeline_navigation->setFixedSize(timeline_width, control_height);
    QVBoxLayout *timeline_navigation_layout =
        new QVBoxLayout(timeline_navigation);
    timeline_navigation_layout->setContentsMargins(0, 0, 0, 0);
    timeline_navigation_layout->setSpacing(0);

    this->combo_sim_timepoint = new QComboBox(timeline_navigation);
    this->combo_sim_timepoint->setFixedSize(timeline_width, half_height);
    this->combo_sim_timepoint->setToolTip(
        QStringLiteral("Select simulation time point"));

    QWidget *step_buttons = new QWidget(timeline_navigation);
    step_buttons->setFixedSize(timeline_width, half_height);
    QHBoxLayout *step_buttons_layout = new QHBoxLayout(step_buttons);
    step_buttons_layout->setContentsMargins(0, 0, 0, 0);
    step_buttons_layout->setSpacing(0);

    this->button_sim_step_previous = new QToolButton(step_buttons);
    this->button_sim_step_previous->setAutoRaise(true);
    this->button_sim_step_previous->setIcon(
        style()->standardIcon(QStyle::SP_MediaSkipBackward));
    this->button_sim_step_previous->setFixedSize(
        timeline_width / 2,
        half_height);
    this->button_sim_step_previous->setIconSize(
        QSize(qMax(1, half_height - 4), qMax(1, half_height - 4)));
    this->button_sim_step_previous->setToolTip(
        QStringLiteral("Previous simulation time point"));

    this->button_sim_step_next = new QToolButton(step_buttons);
    this->button_sim_step_next->setAutoRaise(true);
    this->button_sim_step_next->setIcon(
        style()->standardIcon(QStyle::SP_MediaSkipForward));
    this->button_sim_step_next->setFixedSize(
        timeline_width - timeline_width / 2,
        half_height);
    this->button_sim_step_next->setIconSize(
        QSize(qMax(1, half_height - 4), qMax(1, half_height - 4)));
    this->button_sim_step_next->setToolTip(
        QStringLiteral("Next simulation time point"));

    step_buttons_layout->addWidget(this->button_sim_step_previous);
    step_buttons_layout->addWidget(this->button_sim_step_next);
    timeline_navigation_layout->addWidget(this->combo_sim_timepoint);
    timeline_navigation_layout->addWidget(step_buttons);

    QWidget *playback_stack = new QWidget(this->content);
    playback_stack->setFixedSize(54, control_height);
    QVBoxLayout *playback_stack_layout = new QVBoxLayout(playback_stack);
    playback_stack_layout->setContentsMargins(0, 0, 0, 0);
    playback_stack_layout->setSpacing(0);

    this->button_sim_playback = new QToolButton(playback_stack);
    this->button_sim_playback->setAutoRaise(true);
    this->button_sim_playback->setIcon(
        style()->standardIcon(QStyle::SP_MediaPlay));
    this->button_sim_playback->setFixedSize(54, half_height);
    this->button_sim_playback->setIconSize(
        QSize(qMax(1, half_height - 4), qMax(1, half_height - 4)));
    this->button_sim_playback->setToolTip(
        QStringLiteral("Play simulation timeline"));

    this->combo_sim_speed = new QComboBox(playback_stack);
    this->combo_sim_speed->setFixedSize(54, half_height);
    this->combo_sim_speed->setToolTip(
        QStringLiteral("Simulation playback speed"));
    this->combo_sim_speed->addItem(QStringLiteral("¼×"), 0.25);
    this->combo_sim_speed->addItem(QStringLiteral("½×"), 0.5);
    this->combo_sim_speed->addItem(QStringLiteral("1×"), 1.0);
    this->combo_sim_speed->addItem(QStringLiteral("2×"), 2.0);
    this->combo_sim_speed->addItem(QStringLiteral("4×"), 4.0);
    this->combo_sim_speed->setCurrentIndex(2);

    playback_stack_layout->addWidget(this->button_sim_playback);
    playback_stack_layout->addWidget(this->combo_sim_speed);

    this->simulation_playback_timer = new QTimer(this);
    this->simulation_playback_timer->setInterval(1000);

    setSimulationResultsAvailable(false);
    setEpanetLogAvailable(false);
    clearSimulationResultTimeline();

    connect(this->button_sim_start, &QPushButton::clicked, this, [this]
    {
        emit signalSimulationStart();
    });

    connect(this->button_sim_statistics, &QToolButton::clicked, this, [this]
    {
        emit signalShowSimulationStatistics();
    });

    connect(this->button_sim_settings, &QToolButton::clicked, this, [this]
    {
        showSimulationSettings();
    });

    connect(this->combo_sim_timepoint,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int result_index)
    {
        if (result_index >= 0)
            requestSimulationResultIndex(result_index);
    });

    connect(this->button_sim_step_previous, &QToolButton::clicked, this, [this]
    {
        requestSimulationResultIndex(
            this->combo_sim_timepoint->currentIndex() - 1);
    });

    connect(this->button_sim_step_next, &QToolButton::clicked, this, [this]
    {
        requestSimulationResultIndex(
            this->combo_sim_timepoint->currentIndex() + 1);
    });

    connect(this->button_sim_playback, &QToolButton::clicked, this, [this]
    {
        if (this->simulation_playback_timer->isActive())
        {
            setSimulationPlaybackActive(false);
            return;
        }

        if (this->simulation_result_count <= 1)
            return;

        if (this->combo_sim_timepoint->currentIndex()
            >= this->simulation_result_count - 1)
        {
            requestSimulationResultIndex(0);
        }

        setSimulationPlaybackActive(true);
    });

    connect(this->combo_sim_speed,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int)
    {
        const double speed = this->combo_sim_speed->currentData().toDouble();
        const int interval_ms = qMax(
            1,
            qRound(1000.0 / qMax(0.01, speed)));
        this->simulation_playback_timer->setInterval(interval_ms);
    });

    connect(this->simulation_playback_timer, &QTimer::timeout, this, [this]
    {
        const int current_index =
            this->combo_sim_timepoint->currentIndex();
        if (current_index < 0
            || current_index >= this->simulation_result_count - 1)
        {
            setSimulationPlaybackActive(false);
            return;
        }

        requestSimulationResultIndex(current_index + 1);
    });

    result_button_stack_layout->addWidget(this->button_sim_statistics);
    result_button_stack_layout->addWidget(this->button_sim_settings);

    bar_content->centerLayout()->addWidget(this->button_sim_start);
    bar_content->centerLayout()->addWidget(result_button_stack);
    bar_content->centerLayout()->addWidget(timeline_navigation);
    bar_content->centerLayout()->addWidget(playback_stack);
}

void TopControlBar::addViewControls()
{
    TopControlBarContent *bar_content = barContent(this->content);
    bar_content->rightLayout()->addWidget(createSeparator(this->content));

    QWidget *button_stack = new QWidget(this->content);
    QVBoxLayout *button_stack_layout = new QVBoxLayout(button_stack);
    button_stack_layout->setContentsMargins(0, 0, 0, 0);
    button_stack_layout->setSpacing(0);

    this->button_fullscreen = new QToolButton(button_stack);
    this->button_fullscreen->setAutoRaise(true);
    configureStackedToolbarIconButton(this->button_fullscreen);

    connect(this->button_fullscreen, &QToolButton::clicked, this, [this]
    {
        emit signalFullScreenToggle();
    });

    installShortcutEditContextMenu(
        this->button_fullscreen,
        GuiShortcutId::Fullscreen);

    connect(&guiShortcutRegistry(), &GuiShortcutRegistry::shortcutChanged,
            this, [this](GuiShortcutId id)
    {
        if (id == GuiShortcutId::Fullscreen)
            setFullScreenState(window() != nullptr && window()->isFullScreen());
    });

    this->export_button = new QToolButton(button_stack);
    this->export_button->setAutoRaise(true);
    this->export_button->setIcon(QIcon(QStringLiteral(":/icon/save.png")));
    configureStackedToolbarIconButton(this->export_button);

    connect(this->export_button, &QToolButton::clicked, this, [this]
    {
        emit signalExportEpanetNetwork();
    });

    setFullScreenState(false);
    button_stack_layout->addWidget(this->button_fullscreen);
    button_stack_layout->addWidget(this->export_button);
    bar_content->rightLayout()->addWidget(button_stack);

    this->language_button = new QToolButton(this->content);
    this->language_button->setObjectName(QStringLiteral("interfaceLanguage"));
    this->language_button->setIcon(QIcon(QStringLiteral(":/icon/dashboard_global.png")));
    this->language_button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
#ifdef Q_OS_WASM
    WasmPopupMenu *menu = new WasmPopupMenu(this->language_button);
    connect(this->language_button, &QToolButton::clicked, this, [this, menu] {
        if (menu->isPopupVisible())
            menu->closeAll();
        else
            menu->popupBelow(this->language_button);
    });
#else
    QMenu *menu = new QMenu(this->language_button);
    this->language_button->setPopupMode(QToolButton::InstantPopup);
    this->language_button->setMenu(menu);
    QActionGroup *languages = new QActionGroup(menu);
    languages->setExclusive(true);
#endif
    for (const GuiLanguageChoice &choice : GuiLanguages)
    {
        const QString code = QString::fromLatin1(choice.code);
        const QString label = code.toUpper() + QStringLiteral("  ") + QString::fromUtf8(choice.name);
        const std::function<void()> select = [this, code] {
            if (!GuiLanguage::instance().select(code))
                QMessageBox::warning(this, tr("Language"), tr("Could not load the selected language."));
        };
#ifdef Q_OS_WASM
        menu->addAction(label, select);
#else
        QAction *action = menu->addAction(label);
        action->setCheckable(true);
        languages->addAction(action);
        action->setData(code);
        connect(action, &QAction::triggered, this, select);
#endif
    }
    bar_content->rightLayout()->addWidget(this->language_button);
    refreshViewText();
}

void TopControlBar::refreshViewText()
{
    if (!this->language_button)
        return;
    const QString code = GuiLanguage::instance().code();
    this->language_button->setText(code.toUpper());
    this->language_button->setAccessibleName(tr("Language"));
    this->language_button->setToolTip(tr("Language"));
#ifndef Q_OS_WASM
    for (QAction *action : this->language_button->menu()->actions())
        action->setChecked(action->data().toString() == code);
#endif
    this->export_button->setToolTip(tr("Export EPANET network"));
    setFullScreenState(window()->isFullScreen());
}

void TopControlBar::changeEvent(QEvent *event)
{
    QToolBar::changeEvent(event);
    if (event->type() == QEvent::LanguageChange)
    {
        refreshViewText();
        emit languageChanged();
    }
}
