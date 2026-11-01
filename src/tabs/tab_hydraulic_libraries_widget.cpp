#include "tabs/tab_hydraulic_libraries_widget.h"
#include "network/hydraulic_data.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QVariant>

#include <cmath>
#include <optional>

namespace
{
QString displayNumber(double value)
{
    return QString::number(value, 'g', 17);
}

bool parseFinite(const QString &value, double *result)
{
    bool ok = false;
    const double parsed = value.toDouble(&ok);
    if (!ok || !std::isfinite(parsed))
        return false;
    *result = parsed;
    return true;
}

QStringList cells(const QString &line)
{
    return line.split(QRegularExpression(QStringLiteral("[,;\\s]+")),
                      Qt::SkipEmptyParts);
}

QStringList dataLines(const QString &content)
{
    QStringList result;
    for (const QString &line : content.split('\n'))
    {
        if (!line.trimmed().isEmpty())
            result.append(line.trimmed());
    }
    return result;
}

template<typename Point>
QString pointText(const QList<Point> &points, double Point::*first, double Point::*second)
{
    QStringList lines;
    for (const Point &point : points)
        lines.append(displayNumber(point.*first) + QStringLiteral(", ")
                     + displayNumber(point.*second));
    return lines.join('\n');
}

template<typename Point>
bool readPoints(const QString &content, QList<Point> *points,
                double Point::*first, double Point::*second, QString *error)
{
    QList<Point> parsed;
    int line_number = 0;
    for (const QString &line : dataLines(content))
    {
        ++line_number;
        const QStringList values = cells(line);
        Point point;
        if (values.size() != 2 || !parseFinite(values.at(0), &(point.*first))
            || !parseFinite(values.at(1), &(point.*second)))
        {
            *error = QStringLiteral("Point row %1 needs two finite numbers.").arg(line_number);
            return false;
        }
        parsed.append(point);
    }
    *points = parsed;
    return true;
}

QString materialText(const HydraulicPipeMaterial &material)
{
    QStringList lines;
    for (const HydraulicPipeMaterialRoughnessAtAge &roughness : material.roughness_by_age)
    {
        const QString hw = roughness.roughness_hazen_williams.has_value()
            ? displayNumber(roughness.roughness_hazen_williams.value()) : QStringLiteral("-");
        const QString dw = roughness.roughness_darcy_weisbach_mm.has_value()
            ? displayNumber(roughness.roughness_darcy_weisbach_mm.value()) : QStringLiteral("-");
        const QString cm = roughness.roughness_chezy_manning.has_value()
            ? displayNumber(roughness.roughness_chezy_manning.value()) : QStringLiteral("-");
        lines.append(QString::number(roughness.age_years) + QStringLiteral(", ") + hw
                     + QStringLiteral(", ") + dw + QStringLiteral(", ") + cm);
    }
    return lines.join('\n');
}

bool readMaterial(const QString &content, HydraulicPipeMaterial *material, QString *error)
{
    QList<HydraulicPipeMaterialRoughnessAtAge> parsed;
    int line_number = 0;
    for (const QString &line : dataLines(content))
    {
        ++line_number;
        const QStringList values = cells(line);
        HydraulicPipeMaterialRoughnessAtAge row;
        bool age_ok = false;
        if (values.size() != 4)
        {
            *error = QStringLiteral("Material row %1 needs age, HW, DW, CM (use - for unset).").arg(line_number);
            return false;
        }
        row.age_years = values.at(0).toInt(&age_ok);
        if (!age_ok || row.age_years < 0)
        {
            *error = QStringLiteral("Invalid non-negative age in row %1.").arg(line_number);
            return false;
        }
        std::optional<double> *columns[3] = {
            &row.roughness_hazen_williams, &row.roughness_darcy_weisbach_mm,
            &row.roughness_chezy_manning
        };
        for (int col = 0; col < 3; ++col)
        {
            if (values.at(col + 1) == QStringLiteral("-"))
                continue;
            double number = 0.0;
            if (!parseFinite(values.at(col + 1), &number) || number < 0.0)
            {
                *error = QStringLiteral("Invalid roughness in row %1, column %2.")
                    .arg(line_number).arg(col + 2);
                return false;
            }
            *columns[col] = number;
        }
        parsed.append(row);
    }
    material->roughness_by_age = parsed;
    return true;
}

template<typename T>
const T *findEntity(const QList<T> &entities, const QUuid &uuid)
{
    for (const T &entity : entities)
        if (entity.uuid == uuid)
            return &entity;
    return nullptr;
}
}

HydraulicLibrariesWidget::HydraulicLibrariesWidget(HydraulicData *hydraulic_data, QWidget *parent)
    : QWidget(parent), hydraulic_data(hydraulic_data)
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    QLabel *description = new QLabel(
        QStringLiteral("Hydraulic library items are saved through the current network's "
                       "editing transaction. Delete is blocked while an item is referenced."), this);
    description->setWordWrap(true);
    layout->addWidget(description);

    QHBoxLayout *selector = new QHBoxLayout();
    this->category = new QComboBox(this);
    this->category->addItem(QStringLiteral("Pipe materials"));
    this->category->addItem(QStringLiteral("Time patterns"));
    this->category->addItem(QStringLiteral("Tank volume curves"));
    this->category->addItem(QStringLiteral("Pump head curves"));
    this->category->addItem(QStringLiteral("Pump efficiency curves"));
    this->category->addItem(QStringLiteral("Valve headloss curves"));
    this->category->addItem(QStringLiteral("Valve characteristic curves"));
    this->category->addItem(QStringLiteral("Generic curves"));
    selector->addWidget(new QLabel(QStringLiteral("Collection:"), this));
    selector->addWidget(this->category, 1);
    QPushButton *add_button = new QPushButton(QStringLiteral("+ Add"), this);
    this->edit_button = new QPushButton(QStringLiteral("Edit"), this);
    this->delete_button = new QPushButton(QStringLiteral("Delete"), this);
    selector->addWidget(add_button);
    selector->addWidget(this->edit_button);
    selector->addWidget(this->delete_button);
    layout->addLayout(selector);
    this->entries = new QListWidget(this);
    this->entries->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(this->entries, 1);

    connect(this->category, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { refresh(); });
    connect(this->entries, &QListWidget::itemSelectionChanged,
            this, [this]() { updateButtons(); });
    connect(this->entries, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem *) { editEntry(false); });
    connect(add_button, &QPushButton::clicked,
            this, [this]() { editEntry(true); });
    connect(this->edit_button, &QPushButton::clicked,
            this, [this]() { editEntry(false); });
    connect(this->delete_button, &QPushButton::clicked,
            this, [this]() { deleteEntry(); });
    connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded,
            this, [this]() { refresh(); });
    refresh();
}

void HydraulicLibrariesWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    refresh(selectedUuid());
}

QUuid HydraulicLibrariesWidget::selectedUuid() const
{
    const QListWidgetItem *item = this->entries->currentItem();
    return item == nullptr ? QUuid() : item->data(Qt::UserRole).toUuid();
}

void HydraulicLibrariesWidget::updateButtons()
{
    const bool has_selection = !selectedUuid().isNull();
    this->edit_button->setEnabled(has_selection);
    this->delete_button->setEnabled(has_selection);
}

void HydraulicLibrariesWidget::refresh(const QUuid &select_uuid)
{
    const QUuid uuid = select_uuid.isNull() ? selectedUuid() : select_uuid;
    this->entries->clear();
    const NetworkHydraulic &network = this->hydraulic_data->networkHydraulic();
    switch (this->category->currentIndex())
    {
    case 0:
        for (const HydraulicPipeMaterial &item : network.pipe_materials)
        {
            QListWidgetItem *row = new QListWidgetItem(item.id, this->entries);
            row->setData(Qt::UserRole, item.uuid);
            row->setToolTip(item.uuid.toString(QUuid::WithoutBraces));
        }
        break;
    case 1:
        for (const HydraulicPatternTime &item : network.patterns_time)
        {
            QListWidgetItem *row = new QListWidgetItem(item.id, this->entries);
            row->setData(Qt::UserRole, item.uuid);
            row->setToolTip(item.uuid.toString(QUuid::WithoutBraces));
        }
        break;
    case 2:
        for (const HydraulicCurveTankVolume &item : network.curves_tank_volume)
        {
            QListWidgetItem *row = new QListWidgetItem(item.id, this->entries);
            row->setData(Qt::UserRole, item.uuid);
            row->setToolTip(item.uuid.toString(QUuid::WithoutBraces));
        }
        break;
    case 3:
        for (const HydraulicCurvePumpHead &item : network.curves_pump_head)
        {
            QListWidgetItem *row = new QListWidgetItem(item.id, this->entries);
            row->setData(Qt::UserRole, item.uuid);
            row->setToolTip(item.uuid.toString(QUuid::WithoutBraces));
        }
        break;
    case 4:
        for (const HydraulicCurvePumpEfficiency &item : network.curves_pump_efficiency)
        {
            QListWidgetItem *row = new QListWidgetItem(item.id, this->entries);
            row->setData(Qt::UserRole, item.uuid);
            row->setToolTip(item.uuid.toString(QUuid::WithoutBraces));
        }
        break;
    case 5:
        for (const HydraulicCurveValveHeadloss &item : network.curves_valve_headloss)
        {
            QListWidgetItem *row = new QListWidgetItem(item.id, this->entries);
            row->setData(Qt::UserRole, item.uuid);
            row->setToolTip(item.uuid.toString(QUuid::WithoutBraces));
        }
        break;
    case 6:
        for (const HydraulicCurveValveCharacteristic &item : network.curves_valve_characteristic)
        {
            QListWidgetItem *row = new QListWidgetItem(item.id, this->entries);
            row->setData(Qt::UserRole, item.uuid);
            row->setToolTip(item.uuid.toString(QUuid::WithoutBraces));
        }
        break;
    case 7:
        for (const HydraulicCurveGeneric &item : network.curves_generic)
        {
            QListWidgetItem *row = new QListWidgetItem(item.id, this->entries);
            row->setData(Qt::UserRole, item.uuid);
            row->setToolTip(item.uuid.toString(QUuid::WithoutBraces));
        }
        break;
    default:
        break;
    }
    for (int index = 0; index < this->entries->count(); ++index)
    {
        if (this->entries->item(index)->data(Qt::UserRole).toUuid() == uuid)
        {
            this->entries->setCurrentRow(index);
            break;
        }
    }
    updateButtons();
}

void HydraulicLibrariesWidget::editEntry(bool creating)
{
    const int collection = this->category->currentIndex();
    const QUuid uuid = creating ? QUuid() : selectedUuid();
    if (!creating && uuid.isNull())
        return;
    const NetworkHydraulic &network = this->hydraulic_data->networkHydraulic();
    QString id;
    QString comment;
    QString data;
    switch (collection)
    {
    case 0:
        if (!creating)
        {
            const HydraulicPipeMaterial *item = findEntity(network.pipe_materials, uuid);
            if (item == nullptr) return;
            id = item->id;
            comment = item->description;
            data = materialText(*item);
        }
        break;
    case 1:
        if (!creating)
        {
            const HydraulicPatternTime *item = findEntity(network.patterns_time, uuid);
            if (item == nullptr) return;
            id = item->id;
            comment = item->comment;
            for (double multiplier : item->multipliers)
                data += (data.isEmpty() ? QString() : QStringLiteral("\n")) + displayNumber(multiplier);
        }
        break;
    case 2:
        if (!creating)
        {
            const HydraulicCurveTankVolume *item = findEntity(network.curves_tank_volume, uuid);
            if (item == nullptr) return;
            id = item->id;
            comment = item->comment;
            data = pointText(item->points, &HydraulicCurveTankVolumePoint::water_level_m, &HydraulicCurveTankVolumePoint::volume_m3);
        }
        break;
    case 3:
        if (!creating)
        {
            const HydraulicCurvePumpHead *item = findEntity(network.curves_pump_head, uuid);
            if (item == nullptr) return;
            id = item->id;
            comment = item->comment;
            data = pointText(item->points, &HydraulicCurvePumpHeadPoint::flow_m3_per_h, &HydraulicCurvePumpHeadPoint::head_gain_m);
        }
        break;
    case 4:
        if (!creating)
        {
            const HydraulicCurvePumpEfficiency *item = findEntity(network.curves_pump_efficiency, uuid);
            if (item == nullptr) return;
            id = item->id;
            comment = item->comment;
            data = pointText(item->points, &HydraulicCurvePumpEfficiencyPoint::flow_m3_per_h, &HydraulicCurvePumpEfficiencyPoint::efficiency_percent);
        }
        break;
    case 5:
        if (!creating)
        {
            const HydraulicCurveValveHeadloss *item = findEntity(network.curves_valve_headloss, uuid);
            if (item == nullptr) return;
            id = item->id;
            comment = item->comment;
            data = pointText(item->points, &HydraulicCurveValveHeadlossPoint::flow_m3_per_h, &HydraulicCurveValveHeadlossPoint::head_loss_m);
        }
        break;
    case 6:
        if (!creating)
        {
            const HydraulicCurveValveCharacteristic *item = findEntity(network.curves_valve_characteristic, uuid);
            if (item == nullptr) return;
            id = item->id;
            comment = item->comment;
            data = pointText(item->points, &HydraulicCurveValveCharacteristicPoint::position_percent, &HydraulicCurveValveCharacteristicPoint::relative_flow_percent);
        }
        break;
    case 7:
        if (!creating)
        {
            const HydraulicCurveGeneric *item = findEntity(network.curves_generic, uuid);
            if (item == nullptr) return;
            id = item->id;
            comment = item->comment;
            data = pointText(item->points, &HydraulicCurveGenericPoint::x, &HydraulicCurveGenericPoint::y);
        }
        break;
    default:
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(creating ? QStringLiteral("Add hydraulic library item")
                                   : QStringLiteral("Edit hydraulic library item"));
    QVBoxLayout *dialog_layout = new QVBoxLayout(&dialog);
    QFormLayout *fields = new QFormLayout();
    QLineEdit *id_edit = new QLineEdit(id, &dialog);
    QPlainTextEdit *comment_edit = new QPlainTextEdit(comment, &dialog);
    QPlainTextEdit *data_edit = new QPlainTextEdit(data, &dialog);
    comment_edit->setMaximumHeight(80);
    data_edit->setMinimumHeight(180);
    fields->addRow(QStringLiteral("ID:"), id_edit);
    fields->addRow(collection == 0 ? QStringLiteral("Description:") : QStringLiteral("Comment:"), comment_edit);
    const QString values_label = collection == 0
        ? QStringLiteral("Age, HW, DW mm, CM (use - for missing):")
        : collection == 1 ? QStringLiteral("Multipliers (one value per line):")
                          : QStringLiteral("Curve points (x, y per line):");
    fields->addRow(values_label, data_edit);
    dialog_layout->addLayout(fields);
    QLabel *hint = new QLabel(QStringLiteral("Numerical values use a dot decimal separator. "
                                               "New items receive UUIDv7 identifiers."), &dialog);
    hint->setWordWrap(true);
    dialog_layout->addWidget(hint);
    QDialogButtonBox *buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    dialog_layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [this, &dialog, id_edit,
                                                             comment_edit, data_edit,
                                                             collection, creating, uuid]()
    {
        const QString name = id_edit->text().trimmed();
        if (name.isEmpty())
        {
            QMessageBox::warning(&dialog, QStringLiteral("Invalid ID"),
                QStringLiteral("The library item ID cannot be empty."));
            return;
        }
        const NetworkHydraulic &current = this->hydraulic_data->networkHydraulic();
        const QString description = comment_edit->toPlainText();
        const QString content = data_edit->toPlainText();
        QString error;
        QUuid result;
        bool successful = false;
        switch (collection)
        {
        case 0:
        {
            HydraulicPipeMaterial entity;
            if (!creating)
            {
                const HydraulicPipeMaterial *existing = findEntity(current.pipe_materials, uuid);
                if (existing == nullptr) break;
                entity = *existing;
            }
            entity.id = name;
            entity.description = description;
            if (!readMaterial(content, &entity, &error)) break;
            result = creating ? this->hydraulic_data->addPipeMaterial(entity) : uuid;
            successful = creating ? !result.isNull() : this->hydraulic_data->updatePipeMaterial(entity);
            break;
        }
        case 1:
        {
            HydraulicPatternTime entity;
            if (!creating)
            {
                const HydraulicPatternTime *existing = findEntity(current.patterns_time, uuid);
                if (existing == nullptr) break;
                entity = *existing;
            }
            entity.id = name;
            entity.comment = description;
            entity.multipliers.clear();
            int row = 0;
            for (const QString &line : dataLines(content))
            {
                ++row;
                double number = 0.0;
                if (!parseFinite(line, &number))
                {
                    error = QStringLiteral("Invalid multiplier on row %1.").arg(row);
                    break;
                }
                entity.multipliers.append(number);
            }
            if (!error.isEmpty()) break;
            result = creating ? this->hydraulic_data->addPatternTime(entity) : uuid;
            successful = creating ? !result.isNull() : this->hydraulic_data->updatePatternTime(entity);
            break;
        }
        case 2:
        {
            HydraulicCurveTankVolume entity;
            if (!creating)
            {
                const HydraulicCurveTankVolume *existing = findEntity(current.curves_tank_volume, uuid);
                if (existing == nullptr) break;
                entity = *existing;
            }
            entity.id = name;
            entity.comment = description;
            if (!readPoints(content, &entity.points, &HydraulicCurveTankVolumePoint::water_level_m,
                            &HydraulicCurveTankVolumePoint::volume_m3, &error)) break;
            result = creating ? this->hydraulic_data->addCurveTankVolume(entity) : uuid;
            successful = creating ? !result.isNull() : this->hydraulic_data->updateCurveTankVolume(entity);
            break;
        }
        case 3:
        {
            HydraulicCurvePumpHead entity;
            if (!creating)
            {
                const HydraulicCurvePumpHead *existing = findEntity(current.curves_pump_head, uuid);
                if (existing == nullptr) break;
                entity = *existing;
            }
            entity.id = name;
            entity.comment = description;
            if (!readPoints(content, &entity.points, &HydraulicCurvePumpHeadPoint::flow_m3_per_h,
                            &HydraulicCurvePumpHeadPoint::head_gain_m, &error)) break;
            result = creating ? this->hydraulic_data->addCurvePumpHead(entity) : uuid;
            successful = creating ? !result.isNull() : this->hydraulic_data->updateCurvePumpHead(entity);
            break;
        }
        case 4:
        {
            HydraulicCurvePumpEfficiency entity;
            if (!creating)
            {
                const HydraulicCurvePumpEfficiency *existing = findEntity(current.curves_pump_efficiency, uuid);
                if (existing == nullptr) break;
                entity = *existing;
            }
            entity.id = name;
            entity.comment = description;
            if (!readPoints(content, &entity.points, &HydraulicCurvePumpEfficiencyPoint::flow_m3_per_h,
                            &HydraulicCurvePumpEfficiencyPoint::efficiency_percent, &error)) break;
            result = creating ? this->hydraulic_data->addCurvePumpEfficiency(entity) : uuid;
            successful = creating ? !result.isNull() : this->hydraulic_data->updateCurvePumpEfficiency(entity);
            break;
        }
        case 5:
        {
            HydraulicCurveValveHeadloss entity;
            if (!creating)
            {
                const HydraulicCurveValveHeadloss *existing = findEntity(current.curves_valve_headloss, uuid);
                if (existing == nullptr) break;
                entity = *existing;
            }
            entity.id = name;
            entity.comment = description;
            if (!readPoints(content, &entity.points, &HydraulicCurveValveHeadlossPoint::flow_m3_per_h,
                            &HydraulicCurveValveHeadlossPoint::head_loss_m, &error)) break;
            result = creating ? this->hydraulic_data->addCurveValveHeadloss(entity) : uuid;
            successful = creating ? !result.isNull() : this->hydraulic_data->updateCurveValveHeadloss(entity);
            break;
        }
        case 6:
        {
            HydraulicCurveValveCharacteristic entity;
            if (!creating)
            {
                const HydraulicCurveValveCharacteristic *existing = findEntity(current.curves_valve_characteristic, uuid);
                if (existing == nullptr) break;
                entity = *existing;
            }
            entity.id = name;
            entity.comment = description;
            if (!readPoints(content, &entity.points, &HydraulicCurveValveCharacteristicPoint::position_percent,
                            &HydraulicCurveValveCharacteristicPoint::relative_flow_percent, &error)) break;
            result = creating ? this->hydraulic_data->addCurveValveCharacteristic(entity) : uuid;
            successful = creating ? !result.isNull() : this->hydraulic_data->updateCurveValveCharacteristic(entity);
            break;
        }
        case 7:
        {
            HydraulicCurveGeneric entity;
            if (!creating)
            {
                const HydraulicCurveGeneric *existing = findEntity(current.curves_generic, uuid);
                if (existing == nullptr) break;
                entity = *existing;
            }
            entity.id = name;
            entity.comment = description;
            if (!readPoints(content, &entity.points, &HydraulicCurveGenericPoint::x,
                            &HydraulicCurveGenericPoint::y, &error)) break;
            result = creating ? this->hydraulic_data->addCurveGeneric(entity) : uuid;
            successful = creating ? !result.isNull() : this->hydraulic_data->updateCurveGeneric(entity);
            break;
        }
        default:
            break;
        }
        if (!successful)
        {
            QMessageBox::warning(&dialog, QStringLiteral("Cannot save library item"),
                error.isEmpty() ? QStringLiteral("The network rejected this edit.") : error);
            return;
        }
        dialog.setProperty("saved_uuid", result);
        dialog.accept();
    });
    if (dialog.exec() == QDialog::Accepted)
        refresh(dialog.property("saved_uuid").toUuid());
}

void HydraulicLibrariesWidget::deleteEntry()
{
    const QUuid uuid = selectedUuid();
    if (uuid.isNull())
        return;
    if (QMessageBox::question(this, QStringLiteral("Delete library item"),
            QStringLiteral("Delete the selected library entry? Referenced entries cannot be deleted."))
        != QMessageBox::Yes)
        return;
    bool removed = false;
    switch (this->category->currentIndex())
    {
    case 0: removed = this->hydraulic_data->removePipeMaterial(uuid); break;
    case 1: removed = this->hydraulic_data->removePatternTime(uuid); break;
    case 2: removed = this->hydraulic_data->removeCurveTankVolume(uuid); break;
    case 3: removed = this->hydraulic_data->removeCurvePumpHead(uuid); break;
    case 4: removed = this->hydraulic_data->removeCurvePumpEfficiency(uuid); break;
    case 5: removed = this->hydraulic_data->removeCurveValveHeadloss(uuid); break;
    case 6: removed = this->hydraulic_data->removeCurveValveCharacteristic(uuid); break;
    case 7: removed = this->hydraulic_data->removeCurveGeneric(uuid); break;
    default: break;
    }
    if (!removed)
    {
        QMessageBox::warning(this, QStringLiteral("Cannot delete library item"),
            QStringLiteral("The entry is still referenced or its UUID is invalid."));
        return;
    }
    refresh();
}
