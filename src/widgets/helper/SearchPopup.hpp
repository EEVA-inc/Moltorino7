// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "ForwardDecl.hpp"
#include "widgets/BasePopup.hpp"

#include <QPointer>

#include <memory>

class QLineEdit;

namespace chatterino {

class Split;
class MessagePredicate;

class SearchPopup : public BasePopup
{
public:
    SearchPopup(QWidget *parent, Split *split = nullptr);

    virtual void addChannel(ChannelView &channel);
    void goToMessage(const MessagePtr &message);
    bool canReplyToMessage(const MessagePtr &message) const;
    bool replyToMessage(const MessagePtr &message);
    ChannelPtr sourceChannelForMessage(const MessagePtr &message) const;

    void goToMessageId(const QString &messageId);

protected:
    virtual void updateWindowTitle();
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;
    void themeChangedEvent() override;

private:
    void initLayout();
    void search();
    void addShortcuts() override;
    std::vector<MessagePtr> buildSnapshot();
    ChannelView *sourceViewForMessage(const MessagePtr &message) const;

    static ChannelPtr filter(const QString &text, const QString &channelName,
                             const std::vector<MessagePtr> &snapshot);

    static std::vector<std::unique_ptr<MessagePredicate>> parsePredicates(
        const QString &input);

    std::vector<MessagePtr> snapshot_;
    QLineEdit *searchInput_{};
    ChannelView *channelView_{};
    QString channelName_{};
    Split *split_ = nullptr;
    QList<QPointer<ChannelView>> searchChannels_;
};

}
