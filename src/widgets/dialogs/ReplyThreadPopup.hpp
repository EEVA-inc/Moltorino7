// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "ForwardDecl.hpp"
#include "widgets/DraggablePopup.hpp"

#include <boost/signals2/connection.hpp>
#include <pajlada/signals/scoped-connection.hpp>
#include <pajlada/signals/signal.hpp>

class QCheckBox;

namespace chatterino {

class MessageThread;
class Split;
class SplitInput;
class Channel;

class ReplyThreadPopup final : public DraggablePopup
{
    Q_OBJECT

public:
    /**
     * @param closeAutomatically Decides whether the popup should close when it loses focus
     * @param split Will be used as the popup's parent. Must not be null
     */
    explicit ReplyThreadPopup(bool closeAutomatically, Split *split);

    void setThread(std::shared_ptr<MessageThread> thread,
                   std::weak_ptr<Channel> channel);
    void showAt(QPoint position);
    void giveFocus(Qt::FocusReason reason);

protected:
    bool event(QEvent *event) override;
    void scaleChangedEvent(float scale) override;
    void focusInEvent(QFocusEvent *event) override;

private:
    void addMessagesFromThread();
    void updateInputUI();
    void updateMinimumSize();
    int minimumChatHeight() const;
    void queueFitToScreen();
    void fitToScreen();
    bool fittingToScreen_ = false;
    bool fitQueued_ = false;

    // The message reply thread
    std::shared_ptr<MessageThread> thread_;
    // The channel that the reply thread is in
    ChannelPtr channel_;
    // The channel for the `threadView`
    ChannelPtr virtualChannel_;
    Split *split_;

    struct {
        ChannelView *threadView = nullptr;
        SplitInput *replyInput = nullptr;

        QCheckBox *notificationCheckbox = nullptr;
    } ui_;

    std::unique_ptr<pajlada::Signals::ScopedConnection> messageConnection_;
    pajlada::Signals::ScopedConnection currentUserConnection_;
    pajlada::Signals::ScopedConnection kickCurrentUserConnection_;
    boost::signals2::scoped_connection replySubscriptionSignal_;
};

}  // namespace chatterino
