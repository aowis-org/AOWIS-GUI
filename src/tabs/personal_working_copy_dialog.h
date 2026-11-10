#pragma once

#include "config/personal_repository_storage.h"
#include "projects/personal_working_copy_client.h"

#include <QDialog>

#include <optional>
#include <functional>

class QLabel;
class QDialogButtonBox;
class QEvent;
class QListWidget;
class QPushButton;

// Open Network operation. Normally invisible; shows a chooser only when
// several private drafts exist for the selected branch.
class PersonalWorkingCopyDialog final : public QDialog
{
    Q_OBJECT
public:
    PersonalWorkingCopyDialog(const QString &project_id, const QString &branch_id,
                              const QString &branch_name, const QString &head,
                              const QString &source_repository_id,
                              const QString &source_endpoint, const QString &project_name,
                              QWidget *parent = nullptr,
                              std::function<void(const QString &, bool)> on_status = {});

    // Must start after the caller connects finished(), including for failures.
    void start();

    // Available only after a generation-checked canonical network load.
    // The caller opens the verified editor session, or previews the chosen
    // draft in Monitor when the conflict chooser requests read-only viewing.
    const std::optional<NetworkHydraulic> &selectedPreviewNetwork() const;
    QString selectedPreviewDraftId() const;
    QString selectedPreviewGeneration() const;
    QString selectedEditNetworkId() const;
    bool selectedEditRequested() const;
    QString targetRepositoryId() const;

protected:
    void changeEvent(QEvent *event) override;

private:
    void refreshStaticText();
    void refreshDraftDetailsText();
    void setOperationStatus(const QString &message);
    void setOperationStatusKey(const char *source_key, const QString &argument = QString());
    void refreshOperationStatus();
    void reportFailure(const QString &message);
    void refresh();
    void createDraft(const QString &base);
    void inspectDraft();
    void checkoutBranch();
    // A selected draft must be verified against the branch's personal HEAD,
    // including when chosen from the multiple-draft recovery dialog.
    void verifyDirectDraft(const QString &draft_id);
    void replaceStaleDraft(const PersonalWorkingCopyInfo &draft, const QString &personal_head);
    int queuedLocalEdits(const QString &draft_id) const;
    void initializeNetwork();
    void viewDraftNetwork();
    void editDraftNetwork();
    void setBusy(bool busy);
    void showDraft(const PersonalWorkingCopyInfo &draft);

    PersonalRepositoryTarget target;
    PersonalWorkingCopyClient client;
    QString project_id;
    QString branch_id;
    QString branch_name;
    QString head;
    QString source_repository_id;
    QString source_endpoint;
    QString project_name;
    QLabel *context_label = nullptr;
    QLabel *scope_label = nullptr;
    QDialogButtonBox *close_buttons = nullptr;
    QLabel *status = nullptr;
    // Retain application-owned status keys rather than already translated text.
    // A running DVMDB request must not restart when the interface language changes.
    const char *operation_status_key = nullptr;
    QString operation_status_argument;
    QLabel *details = nullptr;
    QListWidget *drafts = nullptr;
    QPushButton *refresh_button = nullptr;
    QPushButton *reopen_button = nullptr;
    QPushButton *view_button = nullptr;
    QPushButton *edit_button = nullptr;
    std::optional<PersonalWorkingCopyInfo> active_draft;
    std::optional<NetworkHydraulic> preview_network;
    QString preview_generation;
    QString preview_draft_id;
    QString accepted_draft_id;
    QString accepted_generation;
    QString accepted_network_id;
    bool accepted_edit = false;
    bool choosing_draft = false;
    std::function<void(const QString &, bool)> on_status;
    bool busy = false;
    // Source synchronization is attempted at most once per Open operation.
    bool source_pull_attempted = false;
    // A modified draft on an older base is opened only after confirmation.
    bool confirm_before_editing = false;
    QString stale_notice;
};
