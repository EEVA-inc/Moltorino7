#pragma once

#include <QPersistentModelIndex>
#include <QStyledItemDelegate>
#include <QVector>

class QAbstractItemView;
class QLabel;

namespace chatterino {

struct DialogTextLink {
    qsizetype start{};
    qsizetype length{};
    QString target;
};

QVector<DialogTextLink> dialogTextLinks(const QString &text);
void openDialogTextLink(const QString &target);
void configureDialogLinkLabel(QLabel *label);
void setDialogLinkLabelText(QLabel *label, const QString &text);

class DialogLinkTextDelegate final : public QStyledItemDelegate
{
public:
    explicit DialogLinkTextDelegate(QAbstractItemView *view);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    bool editorEvent(QEvent *event, QAbstractItemModel *model,
                     const QStyleOptionViewItem &option,
                     const QModelIndex &index) override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QString linkAt(const QStyleOptionViewItem &option,
                   const QModelIndex &index, const QPointF &position) const;

    QAbstractItemView *view_{};
    QString pressedLink_;
    QPersistentModelIndex pressedIndex_;
};

}
