#ifndef ENTITY_INSPECTOR_ANNOTATION_H
#define ENTITY_INSPECTOR_ANNOTATION_H

#include "network/hydraulic_data.h"

#include <QWidget>
#include <QTimer>
#include <QUuid>
#include <optional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QFormLayout;
class QEvent;
class QPlainTextEdit;
class QPushButton;

// Editor for the revisioned, domain-neutral annotation model. This is a
// separate inspector page, not a hydraulic entity and not an ephemeral note.
class EntityInspectorAnnotation final : public QWidget
{
    Q_OBJECT
public:
    explicit EntityInspectorAnnotation(HydraulicData *data,
                                       const std::optional<MapAnnotation> &existing,
                                       const CoordinateWGS84 &new_position = {},
                                       QWidget *parent = nullptr);
    ~EntityInspectorAnnotation() override;
    void focusText();

protected:
    void changeEvent(QEvent *event) override;

signals:
    void annotationCreated(const QUuid &uuid);
    void annotationDeleted();
    void annotationSelected(const QUuid &uuid);

private:
    void refreshPermission();
    void refreshFromModel();
    void refreshAnnotationChoices();
    void retranslate();
    void commit();

    HydraulicData *data_ = nullptr;
    QUuid annotation_uuid_;
    bool creating_ = false;
    bool updating_ui_ = false;
    CoordinateWGS84 initial_position_;
    QLabel *heading_ = nullptr;
    QFormLayout *form_ = nullptr;
    QComboBox *browse_ = nullptr;
    QPlainTextEdit *text_ = nullptr;
    QDoubleSpinBox *latitude_ = nullptr;
    QDoubleSpinBox *longitude_ = nullptr;
    QComboBox *size_ = nullptr;
    QCheckBox *background_ = nullptr;
    QCheckBox *visible_ = nullptr;
    QLabel *message_ = nullptr;
    QLabel *anchor_ = nullptr;
    QPushButton *create_ = nullptr;
    QPushButton *delete_ = nullptr;
    QTimer commit_timer_;
};

#endif
