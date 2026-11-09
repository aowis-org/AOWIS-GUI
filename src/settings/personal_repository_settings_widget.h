#pragma once

#include "services/personal_repository_monitor.h"

#include <QCoreApplication>
#include <QWidget>

class QEvent;
class QLabel;
class QLineEdit;
class QPushButton;

// Explicit, per-user private DVMDB configuration. Source database selection
// and project navigation do not modify this setting.
class PersonalRepositorySettingsWidget final : public QWidget
{
    Q_DECLARE_TR_FUNCTIONS(PersonalRepositorySettingsWidget)

public:
    explicit PersonalRepositorySettingsWidget(QWidget *parent = nullptr);
    PersonalRepositoryMonitor::State verificationState() const;

protected:
    void changeEvent(QEvent *event) override;

private:
    void refreshLocalizedText();
    void restore();
    void save();
    void clear();
    void verify();
    void updateStatus(PersonalRepositoryMonitor::State state, const QString &detail);

    QLineEdit *endpoint = nullptr;
    QLineEdit *repository_id = nullptr;
    QLabel *status = nullptr;
    QPushButton *save_button = nullptr;
    enum class LocalStatusDetail { None, SaveFailed, RemoveFailed };
    PersonalRepositoryMonitor monitor;
    PersonalRepositoryMonitor::State displayed_status_state = PersonalRepositoryMonitor::State::Unconfigured;
    QString displayed_status_detail;
    LocalStatusDetail local_status_detail = LocalStatusDetail::None;
};
