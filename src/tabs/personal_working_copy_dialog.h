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

// First VC-04 Projects-tab surface: create/reopen a durable private draft.
// Editing and Commit are intentionally not presented as available yet.
class PersonalWorkingCopyDialog final : public QDialog
{
    Q_OBJECT
public:
    PersonalWorkingCopyDialog(const QString &project_id, const QString &branch_id,
                              const QString &branch_name, const QString &head,
                              const QString &source_repository_id,
                              const QString &source_endpoint, const QString &project_name,
                              QWidget *parent = nullptr, bool open_editor_directly = false,
                              std::function<void(const QString &, bool)> on_status = {});

    // Available only after a generation-checked canonical network load.
    // The caller may display it, but editing is NOT bound to autosave yet.
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
    void reportFailure(const QString &message);
    void refresh();
    void createDraft(const QString &base);
    void inspectDraft();
    void checkoutBranch();
    // Direct edit of the branch's single draft: make sure it is based on the
    // branch's current personal HEAD before it is opened (see .cpp).
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
    QLabel *repository_warning_label = nullptr;
    QLabel *scope_label = nullptr;
    QDialogButtonBox *close_buttons = nullptr;
    QLabel *status = nullptr;
    QLabel *details = nullptr;
    QListWidget *drafts = nullptr;
    QPushButton *refresh_button = nullptr;
    QPushButton *create_button = nullptr;
    QPushButton *checkout_button = nullptr;
    QPushButton *reopen_button = nullptr;
    QPushButton *initialize_button = nullptr;
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
    bool direct_edit = false;
    std::function<void(const QString &, bool)> on_status;
    bool busy = false;
    bool draft_list_empty_notice = false;
    // Direct edit pulls newer source commits at most once per dialog.
    bool source_pull_attempted = false;
    // A modified draft on an older base is opened only after confirmation.
    bool confirm_before_editing = false;
    QString stale_notice;
};
