#include "simulation/simulation_statistics_dialog.h"

#include "network/hydraulic_data.h"
#include "config/unit_aware_view.h"
#include "config/unit_profile_manager.h"
#include "simulation/simulation_statistics_unit_presentation.h"
#include "simulation/simulation_diagnostics_widget.h"

#include <cmath>
#include <QAbstractItemView>
#include <QFont>
#include <QEvent>
#include <QScrollBar>
#include <QFontDatabase>
#include <QHeaderView>
#include <QLabel>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QVariant>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextBrowser>
#include <QTextEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace
{
struct StatisticExtreme
{
    double value = 0.0;
    int result_index = -1;
};

struct IntegerStatisticExtreme
{
    qint64 value = 0;
    int result_index = -1;
};

struct AggregateStatistics
{
    IntegerStatisticExtreme hydraulic_iterations;
    StatisticExtreme relative_error;
    StatisticExtreme maximum_head_error_m;
    StatisticExtreme maximum_flow_change_m3_per_h;
    IntegerStatisticExtreme deficient_nodes;
    StatisticExtreme demand_reduction_percent;
    StatisticExtreme leakage_loss_percent;
};

QString formatElapsedTime(quint64 time_elapsed_s)
{
    const quint64 days = time_elapsed_s / 86400;
    const quint64 remainder_after_days = time_elapsed_s % 86400;
    const quint64 hours = remainder_after_days / 3600;
    const quint64 minutes = (remainder_after_days % 3600) / 60;
    const quint64 seconds = remainder_after_days % 60;
    const QString clock = QStringLiteral("%1:%2:%3")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));

    if (days == 0)
        return clock;

    return QStringLiteral("%1d %2").arg(days).arg(clock);
}

QString formatDouble(double value)
{
    if (!std::isfinite(value))
        return QStringLiteral("—");
    const double absolute_value = std::abs(value);
    if (absolute_value != 0.0 && (absolute_value < 0.0001 || absolute_value >= 1000000.0))
        return QString::number(value, 'e', 4);

    QString text = QString::number(value, 'f', 6);
    while (text.endsWith(QLatin1Char('0')))
        text.chop(1);
    if (text.endsWith(QLatin1Char('.')))
        text.chop(1);
    return text;
}

QString statisticTime(const QList<HydraulicSimulationResult> &results, int result_index)
{
    if (result_index < 0 || result_index >= results.size())
        return QStringLiteral("—");

    return formatElapsedTime(results.at(result_index).time_elapsed_s);
}

void updateMaximum(StatisticExtreme &extreme, double value, int result_index)
{
    if (extreme.result_index < 0 || value > extreme.value)
    {
        extreme.value = value;
        extreme.result_index = result_index;
    }
}

void updateMaximum(IntegerStatisticExtreme &extreme, qint64 value, int result_index)
{
    if (extreme.result_index < 0 || value > extreme.value)
    {
        extreme.value = value;
        extreme.result_index = result_index;
    }
}

AggregateStatistics aggregateStatistics(const QList<HydraulicSimulationResult> &results)
{
    AggregateStatistics aggregate;
    for (int result_index = 0; result_index < results.size(); ++result_index)
    {
        const HydraulicSimulationResultStatistics &statistics = results.at(result_index).statistics;
        updateMaximum(aggregate.hydraulic_iterations, statistics.hydraulic_iterations, result_index);
        updateMaximum(aggregate.relative_error, statistics.relative_error, result_index);
        updateMaximum(aggregate.maximum_head_error_m, statistics.maximum_head_error_m, result_index);
        updateMaximum(aggregate.maximum_flow_change_m3_per_h, statistics.maximum_flow_change_m3_per_h, result_index);
        updateMaximum(aggregate.deficient_nodes, statistics.deficient_nodes, result_index);
        updateMaximum(aggregate.demand_reduction_percent, statistics.demand_reduction_percent, result_index);
        updateMaximum(aggregate.leakage_loss_percent, statistics.leakage_loss_percent, result_index);
    }
    return aggregate;
}

QTreeWidgetItem *addSummaryGroup(QTreeWidget *tree, const QString &title)
{
    QTreeWidgetItem *group = new QTreeWidgetItem(tree, QStringList{title});
    group->setFirstColumnSpanned(true);

    QFont font = group->font(0);
    font.setBold(true);
    group->setFont(0, font);
    return group;
}

QTreeWidgetItem *addSummaryValue(QTreeWidgetItem *group, const QString &name, const QString &value, const QString &time = QString())
{
    QTreeWidgetItem *item = new QTreeWidgetItem(group);
    item->setText(0, name);
    item->setText(1, value);
    item->setText(2, time);
    item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    return item;
}

QTableWidgetItem *numericTableItem(const QString &text)
{
    QTableWidgetItem *item = new QTableWidgetItem(text);
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return item;
}

// Keep the source double in the item. Only rows painted by Qt request the
// selected-unit representation; the Model and table do not store rounded text.
QTableWidgetItem *measurementTableItem(double canonical)
{
    QTableWidgetItem *item = new QTableWidgetItem();
    item->setData(Qt::DisplayRole, canonical);
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return item;
}

class StatisticMeasurementDelegate final : public QStyledItemDelegate
{
public:
    StatisticMeasurementDelegate(bool head_error, QWidget *parent)
        : QStyledItemDelegate(parent), head_error_(head_error) {}

    QString displayText(const QVariant &value, const QLocale &locale) const override
    {
        Q_UNUSED(locale);
        return SimulationStatisticsUnits::format(
            UnitProfileManager::instance().presentation(), this->head_error_, value.toDouble());
    }

private:
    bool head_error_ = false;
};
}

SimulationStatisticsDialog::SimulationStatisticsDialog(HydraulicData *hydraulic_data, QWidget *parent)
    : QDialog(parent),
    hydraulic_data(hydraulic_data),
    tree_summary(new QTreeWidget(this)),
    table_timeline(new QTableWidget(this)),
    tabs(new QTabWidget(this)),
    text_epanet_log(new QTextBrowser(this))
{
    setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::WindowMaximizeButtonHint);
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Simulation Results"));
    setModal(false);
    resize(1050, 650);

    this->tree_summary->setColumnCount(3);
    this->tree_summary->setHeaderLabels(QStringList{tr("Statistic"), tr("Value"), tr("Occurred at")});
    this->tree_summary->setRootIsDecorated(false);
    this->tree_summary->setAlternatingRowColors(true);
    this->tree_summary->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    this->tree_summary->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    this->tree_summary->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);

    this->table_timeline->setColumnCount(8);
    this->table_timeline->setHorizontalHeaderLabels(QStringList{
        tr("Elapsed"),
        tr("Iterations"),
        tr("Relative\nerror"),
        tr("Max head\nerror [m]"),
        tr("Max flow\nchange [m³/h]"),
        tr("Deficient\nnodes"),
        tr("Demand\nreduction [%]"),
        tr("Leakage\nloss [%]")
    });
    this->table_timeline->setEditTriggers(QAbstractItemView::NoEditTriggers);
    this->table_timeline->setSelectionBehavior(QAbstractItemView::SelectRows);
    this->table_timeline->setSelectionMode(QAbstractItemView::SingleSelection);
    this->table_timeline->setAlternatingRowColors(true);
    this->table_timeline->verticalHeader()->setVisible(false);
    this->table_timeline->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    this->table_timeline->horizontalHeader()->setMinimumHeight(
        this->table_timeline->horizontalHeader()->fontMetrics().height() * 2 + 10);
    this->table_timeline->horizontalHeader()->setStretchLastSection(true);
    this->table_timeline->setItemDelegateForColumn(3,
        new StatisticMeasurementDelegate(true, this->table_timeline));
    this->table_timeline->setItemDelegateForColumn(4,
        new StatisticMeasurementDelegate(false, this->table_timeline));

    const QFont fixed_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    this->text_epanet_log->setFont(fixed_font);
    this->text_epanet_log->setOpenExternalLinks(true);
    this->text_epanet_log->setLineWrapMode(QTextEdit::NoWrap);
    this->text_epanet_log->setPlaceholderText(
        tr("No EPANET log is available for the current simulation."));

    QWidget *summary_page = new QWidget(this->tabs);
    QLabel *hint = new QLabel(
        tr("Summary values show the worst hydraulic value across the complete timeline."),
        summary_page);
    this->summary_hint = hint;
    hint->setWordWrap(true);

    QVBoxLayout *summary_layout = new QVBoxLayout(summary_page);
    summary_layout->setContentsMargins(0, 0, 0, 0);
    summary_layout->addWidget(hint);
    summary_layout->addWidget(this->tree_summary);

    this->widget_diagnostics = new SimulationDiagnosticsWidget(this->hydraulic_data, this->tabs);

    this->tabs->addTab(this->widget_diagnostics, tr("Diagnostics"));
    this->tabs->addTab(summary_page, tr("Summary"));
    this->tabs->addTab(this->table_timeline, tr("By timestep"));
    this->tabs->addTab(this->text_epanet_log, tr("EPANET Log"));

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(this->tabs);

    UnitPresentationContext &units = UnitProfileManager::instance().presentation();
    new UnitAwareView(this->tree_summary, units, [this] { refreshSummaryUnits(); });
    new UnitAwareView(this->table_timeline, units, [this] { refreshTimelineUnits(); });

    connect(this->hydraulic_data, &HydraulicData::signalSimulationResultTimelineChanged, this, [this](bool)
    {
        refresh();
    });
    connect(this->hydraulic_data, &HydraulicData::signalCurrentSimulationResultChanged,
            this, &SimulationStatisticsDialog::syncCurrentResultSelection);

    refresh();
}

void SimulationStatisticsDialog::changeEvent(QEvent *event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::LanguageChange && this->summary_hint != nullptr
        && this->widget_diagnostics != nullptr
        && this->tabs->indexOf(this->widget_diagnostics) >= 0)
        retranslateView();
}

void SimulationStatisticsDialog::retranslateView()
{
    setWindowTitle(tr("Simulation Results"));
    this->tree_summary->setHeaderLabels({tr("Statistic"), tr("Value"), tr("Occurred at")});
    this->summary_hint->setText(tr("Summary values show the worst hydraulic value across the complete timeline."));
    this->tabs->setTabText(this->tabs->indexOf(this->widget_diagnostics), tr("Diagnostics"));
    this->tabs->setTabText(this->tabs->indexOf(this->summary_hint->parentWidget()), tr("Summary"));
    this->tabs->setTabText(this->tabs->indexOf(this->table_timeline), tr("By timestep"));
    this->tabs->setTabText(this->tabs->indexOf(this->text_epanet_log), tr("EPANET Log"));
    this->text_epanet_log->setPlaceholderText(tr("No EPANET log is available for the current simulation."));
    this->table_timeline->setHorizontalHeaderLabels({
        tr("Elapsed"), tr("Iterations"), tr("Relative\nerror"),
        tr("Max head\nerror [m]"), tr("Max flow\nchange [m³/h]"),
        tr("Deficient\nnodes"), tr("Demand\nreduction [%]"), tr("Leakage\nloss [%]")});
    refreshTimelineUnits();
    // Summary strings are derived from the already-loaded timeline. Recompute
    // only that presentation, restoring expansion, selection and scroll state.
    QVector<bool> expanded;
    int selected_group = -1;
    int selected_child = -1;
    QTreeWidgetItem *selected = this->tree_summary->currentItem();
    for (int index = 0; index < this->tree_summary->topLevelItemCount(); ++index)
    {
        QTreeWidgetItem *group = this->tree_summary->topLevelItem(index);
        expanded.append(group->isExpanded());
        if (selected == group || (selected != nullptr && selected->parent() == group))
        {
            selected_group = index;
            if (selected != group)
                selected_child = group->indexOfChild(selected);
        }
    }
    const int scroll_position = this->tree_summary->verticalScrollBar()->value();
    const QSignalBlocker block(this->tree_summary);
    refreshSummary();
    for (int index = 0; index < expanded.size() && index < this->tree_summary->topLevelItemCount(); ++index)
        this->tree_summary->topLevelItem(index)->setExpanded(expanded.at(index));
    if (selected_group >= 0 && selected_group < this->tree_summary->topLevelItemCount())
    {
        QTreeWidgetItem *group = this->tree_summary->topLevelItem(selected_group);
        this->tree_summary->setCurrentItem(selected_child < 0 ? group : group->child(selected_child));
    }
    this->tree_summary->verticalScrollBar()->setValue(scroll_position);
    // Neither the timeline data nor the current timestep selection is reset.
}

void SimulationStatisticsDialog::setEpanetLog(const QString &epanet_log)
{
    if (this->text_epanet_log == nullptr)
        return;

    this->text_epanet_log->setPlainText(epanet_log);
}

void SimulationStatisticsDialog::showEpanetLogTab()
{
    if (this->tabs == nullptr || this->text_epanet_log == nullptr)
        return;

    const int tab_index = this->tabs->indexOf(this->text_epanet_log);
    if (tab_index >= 0)
        this->tabs->setCurrentIndex(tab_index);
}

void SimulationStatisticsDialog::showDiagnosticsTab()
{
    if (this->tabs == nullptr || this->widget_diagnostics == nullptr)
        return;

    const int tab_index = this->tabs->indexOf(this->widget_diagnostics);
    if (tab_index >= 0)
        this->tabs->setCurrentIndex(tab_index);
}

void SimulationStatisticsDialog::refresh()
{
    refreshSummary();
    refreshTimeline();
    syncCurrentResultSelection(this->hydraulic_data->currentSimulationResultIndex());
}

void SimulationStatisticsDialog::refreshSummaryUnits()
{
    const UnitPresentationContext &units = UnitProfileManager::instance().presentation();
    if (this->summary_head_error_item)
        this->summary_head_error_item->setText(1, SimulationStatisticsUnits::formatWithUnit(
            units, true, this->summary_head_error_item->data(1, Qt::UserRole).toDouble()));
    if (this->summary_flow_change_item)
        this->summary_flow_change_item->setText(1, SimulationStatisticsUnits::formatWithUnit(
            units, false, this->summary_flow_change_item->data(1, Qt::UserRole).toDouble()));
}

void SimulationStatisticsDialog::refreshTimelineUnits()
{
    const UnitPresentationContext &units = UnitProfileManager::instance().presentation();
    this->table_timeline->setHorizontalHeaderItem(3, new QTableWidgetItem(
        tr("Max head\nerror [%1]").arg(SimulationStatisticsUnits::selectedUnit(units, true))));
    this->table_timeline->setHorizontalHeaderItem(4, new QTableWidgetItem(
        tr("Max flow\nchange [%1]").arg(SimulationStatisticsUnits::selectedUnit(units, false))));
    // Delegates convert the existing canonical doubles at paint time.
    this->table_timeline->viewport()->update();
}

void SimulationStatisticsDialog::refreshSummary()
{
    this->summary_head_error_item = nullptr;
    this->summary_flow_change_item = nullptr;
    this->tree_summary->clear();
    const std::optional<HydraulicSimulationResultTimeline> &timeline_optional = this->hydraulic_data->simulationResultTimeline();
    if (!this->hydraulic_data->hasSimulationResults() || !timeline_optional.has_value())
    {
        QTreeWidgetItem *group = addSummaryGroup(this->tree_summary, tr("Simulation"));
        addSummaryValue(group, tr("Result"), tr("No valid simulation results available"));
        group->setExpanded(true);
        return;
    }

    const HydraulicSimulationResultTimeline &timeline = timeline_optional.value();
    const QList<HydraulicSimulationResult> &results = timeline.results;
    const AggregateStatistics aggregate = aggregateStatistics(results);

    QTreeWidgetItem *simulation_group = addSummaryGroup(this->tree_summary, tr("Simulation"));
    addSummaryValue(simulation_group, tr("Status"), timeline.status.success ? tr("Successful") : tr("Failed"));
    addSummaryValue(simulation_group, tr("Result timesteps"), QString::number(results.size()));
    addSummaryValue(simulation_group, tr("Duration"), formatElapsedTime(results.constLast().time_elapsed_s));
    addSummaryValue(
        simulation_group,
        tr("Start UTC"),
        timeline.simulation_start_utc.isValid()
            ? timeline.simulation_start_utc.toUTC().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'"))
            : QStringLiteral("—"));

    QTreeWidgetItem *hydraulics_group = addSummaryGroup(this->tree_summary, tr("Hydraulic convergence"));
    addSummaryValue(hydraulics_group, tr("Maximum iterations"),
                    QString::number(aggregate.hydraulic_iterations.value),
                    statisticTime(results, aggregate.hydraulic_iterations.result_index));
    addSummaryValue(hydraulics_group, tr("Maximum relative error"),
                    formatDouble(aggregate.relative_error.value),
                    statisticTime(results, aggregate.relative_error.result_index));
    this->summary_head_error_item = addSummaryValue(hydraulics_group, tr("Maximum head error"),
                    QString(), statisticTime(results, aggregate.maximum_head_error_m.result_index));
    this->summary_head_error_item->setData(1, Qt::UserRole, aggregate.maximum_head_error_m.value);
    this->summary_flow_change_item = addSummaryValue(hydraulics_group, tr("Maximum flow change"),
                    QString(), statisticTime(results, aggregate.maximum_flow_change_m3_per_h.result_index));
    this->summary_flow_change_item->setData(1, Qt::UserRole, aggregate.maximum_flow_change_m3_per_h.value);
    refreshSummaryUnits();

    QTreeWidgetItem *demand_group = addSummaryGroup(this->tree_summary, tr("Pressure-dependent demand"));
    addSummaryValue(demand_group, tr("Maximum deficient nodes"),
                    QString::number(aggregate.deficient_nodes.value),
                    statisticTime(results, aggregate.deficient_nodes.result_index));
    addSummaryValue(demand_group, tr("Maximum demand reduction"),
                    QStringLiteral("%1 %").arg(formatDouble(aggregate.demand_reduction_percent.value)),
                    statisticTime(results, aggregate.demand_reduction_percent.result_index));

    QTreeWidgetItem *leakage_group = addSummaryGroup(this->tree_summary, tr("Leakage"));
    addSummaryValue(leakage_group, tr("Maximum leakage loss"),
                    QStringLiteral("%1 %").arg(formatDouble(aggregate.leakage_loss_percent.value)),
                    statisticTime(results, aggregate.leakage_loss_percent.result_index));

    for (int top_level_index = 0; top_level_index < this->tree_summary->topLevelItemCount(); ++top_level_index)
        this->tree_summary->topLevelItem(top_level_index)->setExpanded(true);
}

void SimulationStatisticsDialog::refreshTimeline()
{
    const QSignalBlocker blocker(this->table_timeline);
    refreshTimelineUnits();
    this->table_timeline->setRowCount(0);
    const std::optional<HydraulicSimulationResultTimeline> &timeline_optional = this->hydraulic_data->simulationResultTimeline();
    if (!this->hydraulic_data->hasSimulationResults() || !timeline_optional.has_value())
        return;

    const QList<HydraulicSimulationResult> &results = timeline_optional->results;
    this->table_timeline->setRowCount(results.size());
    for (int result_index = 0; result_index < results.size(); ++result_index)
    {
        const HydraulicSimulationResult &result = results.at(result_index);
        const HydraulicSimulationResultStatistics &statistics = result.statistics;
        this->table_timeline->setItem(result_index, 0, new QTableWidgetItem(formatElapsedTime(result.time_elapsed_s)));
        this->table_timeline->setItem(result_index, 1, numericTableItem(QString::number(statistics.hydraulic_iterations)));
        this->table_timeline->setItem(result_index, 2, numericTableItem(formatDouble(statistics.relative_error)));
        this->table_timeline->setItem(result_index, 3, measurementTableItem(statistics.maximum_head_error_m));
        this->table_timeline->setItem(result_index, 4, measurementTableItem(statistics.maximum_flow_change_m3_per_h));
        this->table_timeline->setItem(result_index, 5, numericTableItem(QString::number(statistics.deficient_nodes)));
        this->table_timeline->setItem(result_index, 6, numericTableItem(formatDouble(statistics.demand_reduction_percent)));
        this->table_timeline->setItem(result_index, 7, numericTableItem(formatDouble(statistics.leakage_loss_percent)));
    }
}

void SimulationStatisticsDialog::syncCurrentResultSelection(int result_index)
{
    const QSignalBlocker blocker(this->table_timeline);

    if (result_index < 0 || result_index >= this->table_timeline->rowCount())
    {
        this->table_timeline->clearSelection();
        return;
    }

    this->table_timeline->selectRow(result_index);
    this->table_timeline->scrollToItem(
        this->table_timeline->item(result_index, 0),
        QAbstractItemView::EnsureVisible);
}
