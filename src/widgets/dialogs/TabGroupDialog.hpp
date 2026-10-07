#pragma once

#include <QDialog>
#include <QList>
#include <QPointer>
#include <QString>

#include <vector>

class QComboBox;
class QEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QWidget;

namespace chatterino {

struct TabGroupDialogEntry {
    QWidget *page{};
    QString title;
    QString channelTitle;
    QString otherGroupName;
    bool selected{false};
};

class TabGroupDialog final : public QDialog
{
public:
    TabGroupDialog(QString groupName,
                   const std::vector<TabGroupDialogEntry> &entries,
                   bool creating, QString icon, QString customIconPath,
                   QWidget *parent = nullptr);

    [[nodiscard]] QString groupName() const;
    [[nodiscard]] QString groupIcon() const;
    [[nodiscard]] QString customIconPath() const;
    [[nodiscard]] QList<QWidget *> selectedPages() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Row {
        QListWidgetItem *item{};
        QPointer<QWidget> page{};
        QString searchText;
        QString otherGroupName;
    };

    void filterTabs(const QString &query);
    void setAllVisibleChecked(bool checked);
    void toggleItem(QListWidgetItem *item);
    void updateItemTransferState(QListWidgetItem *item, bool moving);
    void updateSelectionState();

    QLineEdit *name_{};
    QComboBox *icon_{};
    QPushButton *chooseIcon_{};
    QLineEdit *search_{};
    QListWidget *tabs_{};
    QLabel *selectionSummary_{};
    QPushButton *acceptButton_{};
    QString customIconPath_;
    bool creating_{};
    std::vector<Row> rows_;
};

}
