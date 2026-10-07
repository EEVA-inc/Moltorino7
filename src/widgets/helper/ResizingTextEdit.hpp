// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QCompleter>
#include <QKeyEvent>
#include <QTextEdit>

namespace chatterino {

class TabCompletionModel;

class ResizingTextEdit : public QTextEdit
{
public:
    ResizingTextEdit();
    ~ResizingTextEdit() override;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    bool hasHeightForWidth() const override;
    bool isFirstWord() const;

    pajlada::Signals::Signal<QKeyEvent *> keyPressed;
    pajlada::Signals::NoArgSignal focused;
    pajlada::Signals::NoArgSignal focusLost;
    pajlada::Signals::Signal<const QMimeData *> imagePasted;
    pajlada::Signals::Signal<QMenu *, QPoint> contextMenuRequested;
    pajlada::Signals::Signal<TabCompletionModel *, int> tabCompletionChanged;
    pajlada::Signals::NoArgSignal tabCompletionHidden;

    void setCompleter(QCompleter *c);

    void resetCompletion();
    bool selectCompletionRow(int row);
    void setGhostText(QString text);
    const QString &ghostText() const;

protected:
    int heightForWidth(int) const override;
    void keyPressEvent(QKeyEvent *event) override;
    void changeEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

    bool canInsertFromMimeData(const QMimeData *source) const override;
    void insertFromMimeData(const QMimeData *source) override;

    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    qreal documentHeightForWidth(int width) const;
    void invalidateAncestorLayouts();
    void finishCompletion();

    QString textUnderCursor(bool *hadSpace = nullptr) const;

    QCompleter *completer_ = nullptr;
    pajlada::Signals::SignalHolder connections_;

    bool completionInProgress_ = false;
    QString ghostText_;

    bool eventFilter(QObject *obj, QEvent *event) override;

private Q_SLOTS:
    void insertCompletion(const QString &completion);
};

}
