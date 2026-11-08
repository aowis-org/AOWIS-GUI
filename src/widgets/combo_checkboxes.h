#ifndef COMBO_CHECKBOXES_H
#define COMBO_CHECKBOXES_H

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>
#include <QWidget>

class QCheckBox;
class QMenu;
class QPushButton;
class QWidgetAction;
#ifdef Q_OS_WASM
class WasmPopupMenu;
#endif

class ComboCheckboxes : public QWidget
{
    Q_OBJECT
    
public:
    explicit ComboCheckboxes(QWidget *parent = nullptr);
    
    void addItem(const QString &text,
                 const QVariant &data = QVariant(),
                 bool checked = false);
    void addItem(const QString &text,
                 const QVariant &data = QVariant(),
                 bool checked = false,
                 QString tooltip = "");
    void insertItem(int index,
                    const QString &text,
                    const QVariant &data = QVariant(),
                    bool checked = false,
                    QString tooltip = "");
    void clear();
    
    int count() const;
    
    QString itemText(int index) const;
    void setItemText(int index, const QString &text);
    void setItemToolTip(int index, const QString &tooltip);
    
    QVariant itemData(int index) const;
    void setItemData(int index, const QVariant &data);
    
    bool isItemChecked(int index) const;
    void setItemChecked(int index, bool checked);
    
    QStringList checkedTexts() const;
    QList<QVariant> checkedData() const;
    QList<int> checkedIndexes() const;
    
    QString placeholderText() const;
    void setPlaceholderText(const QString &text);
    
    int summaryLimit() const;
    void setSummaryLimit(int limit);
    
    QSize sizeHint() const override;
    
signals:
    void itemCheckedChanged(int index, bool checked);
    void checkedItemsChanged();
    
private:
    struct Item
    {
        QString text;
        QVariant data;
        QCheckBox *check_box = nullptr;
        QWidgetAction *action = nullptr;
    };
    
    QPushButton *button = nullptr;
#ifdef Q_OS_WASM
    WasmPopupMenu *wasm_menu = nullptr;
#else
    QMenu *menu = nullptr;
#endif
    QVector<Item> items;
    
    QString placeholder_text = QStringLiteral("None Selected");
    int summary_limit = 3;
    
    bool hasValidIndex(int index) const;
    int indexOfCheckBox(const QCheckBox *check_box) const;
    void updateSummaryText();
};

#endif // COMBO_CHECKBOXES_H
