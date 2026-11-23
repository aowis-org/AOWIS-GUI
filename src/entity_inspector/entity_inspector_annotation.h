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
    bool isCreating() const;
    void publishDraftPreview();
    void moveDraft(const CoordinateWGS84 &position);
    // Synchronously publish a pending, debounced edit before changing
    // inspector selection or ending the verified private editing session.
    void flushPendingEdit();

protected:
    void changeEvent(QEvent *event) override;

signals:
    void annotationCreated(const QUuid &uuid);
    void annotationDeleted();
    void annotationSelected(const QUuid &uuid);
    // Local-only placeholder; it is never sent to MODEL/DVMDB while blank.
    void draftPreviewChanged(const MapAnnotation &annotation);
    void draftPreviewCleared();

private:
    void refreshPermission();
    void refreshFromModel();
    void refreshAnnotationChoices();
    void refreshAnchorChoices(const std::optional<MapAnnotation> &current);
    void retranslate();
    void commit();
    CoordinateWGS84 enteredPosition() const;

    HydraulicData *data_ = nullptr;
    QUuid annotation_uuid_;
    QUuid draft_uuid_ = QUuid::createUuid(); // Transient UI identity; never persisted.
    bool creating_ = false;
    bool draft_initialized_ = false;
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
    QComboBox *anchor_ = nullptr;
    // A node may move while text is being edited. Only rewrite offset fields
    // when the user actually changes the annotation's geographic position.
    bool position_dirty_ = false;
    QPushButton *delete_ = nullptr;
    QTimer commit_timer_;
};

#endif
