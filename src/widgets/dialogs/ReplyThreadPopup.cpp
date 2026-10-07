// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/ReplyThreadPopup.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "messages/Message.hpp"
#include "messages/MessageThread.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/buttons/Button.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/Scrollbar.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitInput.hpp"

#include <QCheckBox>
#include <QGuiApplication>
#include <QScopedValueRollback>
#include <QScreen>
#include <QTimer>

#include <algorithm>

const QString TEXT_TITLE("Reply Thread - @%1 in #%2");

namespace chatterino {

ReplyThreadPopup::ReplyThreadPopup(bool closeAutomatically, Split *split)
    : DraggablePopup(closeAutomatically, split)
    , split_(split)
{
    assert(split != nullptr);

    this->setWindowTitle(QStringLiteral("Reply Thread"));

    HotkeyController::HotkeyMap actions{
        {"delete",
         [this](std::vector<QString>) -> QString {
             this->deleteLater();
             return "";
         }},
        {"scrollPage",
         [this](std::vector<QString> arguments) -> QString {
             if (arguments.empty())
             {
                 qCWarning(chatterinoHotkeys)
                     << "scrollPage hotkey called without arguments!";
                 return "scrollPage hotkey called without arguments!";
             }
             auto direction = arguments.at(0);

             auto &scrollbar = this->ui_.threadView->getScrollBar();
             if (direction == "up")
             {
                 scrollbar.offset(-scrollbar.getPageSize());
             }
             else if (direction == "down")
             {
                 scrollbar.offset(scrollbar.getPageSize());
             }
             else
             {
                 qCWarning(chatterinoHotkeys) << "Unknown scroll direction";
             }
             return "";
         }},
        {"pin",
         [this](std::vector<QString> /*arguments*/) -> QString {
             this->togglePinned();
             return "";
         }},

        // these actions make no sense in the context of a reply thread, so they aren't implemented
        {"execModeratorAction", nullptr},
        {"openProfilePictureMenu", nullptr},
        {"reject", nullptr},
        {"accept", nullptr},
        {"openTab", nullptr},
        {"search", nullptr},
    };

    this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
        HotkeyCategory::PopupWindow, actions, this);

    // initialize UI
    this->ui_.threadView =
        new ChannelView(this, this->split_, ChannelView::Context::ReplyThread);
    this->updateMinimumSize();
    this->ui_.threadView->setSizePolicy(QSizePolicy::Expanding,
                                        QSizePolicy::Expanding);
    std::ignore = this->ui_.threadView->layoutChanged.connect([this] {
        this->queueFitToScreen();
    });
    // We can safely ignore this signal's connection since threadView will always be deleted before
    // the ReplyThreadPopup
    std::ignore =
        this->ui_.threadView->mouseDown.connect([this](QMouseEvent *) {
            this->giveFocus(Qt::MouseFocusReason);
        });

    // Create SplitInput with inline replying disabled
    this->ui_.replyInput =
        new SplitInput(this, this->split_, this->ui_.threadView, false);

    this->currentUserConnection_ =
        getApp()->getAccounts()->twitch.currentUserChanged.connect([this] {
            this->updateInputUI();
        });
    this->kickCurrentUserConnection_ =
        getApp()->getAccounts()->kick.currentUserChanged.connect([this] {
            this->updateInputUI();
        });

    // We can safely ignore this signal's connection since threadView will always be deleted before
    // the ReplyThreadPopup
    std::ignore = this->ui_.threadView->selectionChanged.connect([this]() {
        // clear SplitInput selection when selecting in ChannelView
        if (this->ui_.replyInput->hasSelection())
        {
            this->ui_.replyInput->clearSelection();
        }
    });

    // clear ChannelView selection when selecting in SplitInput
    // We can safely ignore this signal's connection since replyInput will always be deleted before
    // the ReplyThreadPopup
    std::ignore = this->ui_.replyInput->selectionChanged.connect([this]() {
        if (this->ui_.threadView->hasSelection())
        {
            this->ui_.threadView->clearSelection();
        }
    });

    auto layers = LayoutCreator<QWidget>(this->getLayoutContainer())
                      .setLayoutType<QGridLayout>()
                      .withoutMargin();
    auto layout = layers.emplace<QVBoxLayout>();

    layout->setSpacing(0);
    // provide draggable margin if frameless
    auto marginPx = closeAutomatically ? 15 : 1;
    layout->setContentsMargins(marginPx, marginPx, marginPx, marginPx);

    // Top Row
    bool addCheckbox = getSettings()->enableThreadHighlight;
    if (addCheckbox || closeAutomatically)
    {
        auto *hbox = new QHBoxLayout();

        if (addCheckbox)
        {
            this->ui_.notificationCheckbox =
                new QCheckBox("Subscribe to thread", this);
            QObject::connect(this->ui_.notificationCheckbox,
                             &QCheckBox::toggled, [this](bool checked) {
                                 if (!this->thread_ ||
                                     this->thread_->subscribed() == checked)
                                 {
                                     return;
                                 }

                                 if (checked)
                                 {
                                     this->thread_->markSubscribed();
                                 }
                                 else
                                 {
                                     this->thread_->markUnsubscribed();
                                 }
                             });
            hbox->addWidget(this->ui_.notificationCheckbox);
            hbox->addStretch(1);
            this->ui_.notificationCheckbox->setFocusPolicy(Qt::NoFocus);
        }

        if (closeAutomatically)
        {
            hbox->addWidget(this->createPinButton(), 0, Qt::AlignRight);
            hbox->setContentsMargins(0, 0, 0, 5);
        }
        else
        {
            hbox->setContentsMargins(10, 0, 0, 4);
        }

        layout->addLayout(hbox);
    }

    layout->addWidget(this->ui_.threadView, 1);
    layout->addWidget(this->ui_.replyInput);

    this->enableResize(getSettings()->threadPopupSize);

    this->applyPopupSize(
        QSize(qRound(440 * this->scale()), this->minimumChatHeight()));
}

int ReplyThreadPopup::minimumChatHeight() const
{
    const auto metrics = getApp()->getFonts()->getFontMetrics(
        FontStyle::ChatMedium, this->scale());
    const auto lineHeight = qCeil(metrics.height());
    return 2 * (lineHeight + qRound(8 * this->scale())) + 8;
}

void ReplyThreadPopup::updateMinimumSize()
{
    if (this->ui_.threadView)
    {
        this->ui_.threadView->setMinimumSize(
            std::max(320, qRound(400 * this->scale())),
            this->minimumChatHeight());
    }
}

void ReplyThreadPopup::scaleChangedEvent(float scale)
{
    DraggablePopup::scaleChangedEvent(scale);
    if (this->isVisible())
    {
        this->fitToScreen();
    }
    else
    {
        this->updateMinimumSize();
    }
}

void ReplyThreadPopup::showAt(QPoint position)
{
    if (auto *target = QGuiApplication::screenAt(position))
    {
        this->setScreen(target);
    }
    const QPoint offset(qRound(150 * this->scale()),
                        qRound(70 * this->scale()));
    this->showAndMoveTo(position - offset, widgets::BoundsChecking::Off);
    this->fitToScreen();
}

bool ReplyThreadPopup::event(QEvent *event)
{
    if (event->type() == QEvent::DeferredDelete)
    {
        return DraggablePopup::event(event);
    }
    const bool result = DraggablePopup::event(event);
    if (this->isVisible() && (event->type() == QEvent::Show ||
                              event->type() == QEvent::LayoutRequest ||
                              event->type() == QEvent::ScreenChangeInternal ||
                              event->type() == QEvent::DevicePixelRatioChange))
    {
        this->fitToScreen();
    }
    return result;
}

void ReplyThreadPopup::queueFitToScreen()
{
    if (this->fitQueued_ || this->fittingToScreen_ || !this->isVisible())
    {
        return;
    }
    this->fitQueued_ = true;
    QTimer::singleShot(0, this, [this] {
        this->fitQueued_ = false;
        this->fitToScreen();
    });
}

void ReplyThreadPopup::fitToScreen()
{
    auto *screen = this->screen();
    if (this->fittingToScreen_ || !screen || !this->layout() ||
        !this->ui_.threadView || !this->ui_.replyInput || this->isMaximized() ||
        this->isFullScreen() || this->isMinimized())
    {
        return;
    }
    const QScopedValueRollback guard(this->fittingToScreen_, true);
    const auto available = screen->availableGeometry().adjusted(8, 8, -8, -8);
    if (available.isEmpty())
    {
        return;
    }

    const auto frameExtra =
        (this->frameGeometry().size() - this->size()).expandedTo(QSize(0, 0));
    const auto maximum =
        (available.size() - frameExtra).expandedTo(QSize(1, 1));


    const auto chrome =
        (this->minimumSizeHint() - this->ui_.threadView->minimumSize())
            .expandedTo(QSize(0, 0));
    const int otherWidth = chrome.width();
    const int otherHeight = chrome.height();
    const int maximumChatHeight =
        std::min(std::max(180, qRound(240 * this->scale())),
                 std::max(1, maximum.height() - otherHeight));
    const int chatHeight = std::clamp(
        this->ui_.threadView->contentHeight(maximumChatHeight),
        std::min(this->minimumChatHeight(), maximumChatHeight),
        maximumChatHeight);
    const QSize minimum(std::min(std::max(320, qRound(400 * this->scale())),
                                 std::max(1, maximum.width() - otherWidth)),
                        std::min(this->minimumChatHeight(), maximumChatHeight));
    if (this->ui_.threadView->minimumSize() != minimum)
    {
        this->ui_.threadView->setMinimumSize(minimum);
        for (auto *widget = this->ui_.threadView->parentWidget(); widget;
             widget = widget->parentWidget())
        {
            if (widget->layout())
            {
                widget->layout()->activate();
            }
            if (widget == this)
            {
                break;
            }
        }
    }
    this->applyPopupSize(
        QSize(qRound(440 * this->scale()), otherHeight + chatHeight)
            .expandedTo(this->minimumSizeHint())
            .boundedTo(maximum));

    const auto frame = this->frameGeometry();
    const QPoint position(
        std::clamp(
            frame.left(), available.left(),
            std::max(available.left(), available.right() - frame.width() + 1)),
        std::clamp(frame.top(), available.top(),
                   std::max(available.top(),
                            available.bottom() - frame.height() + 1)));
    if (position != frame.topLeft())
    {
        this->move(this->pos() + position - frame.topLeft());
    }
}

void ReplyThreadPopup::setThread(std::shared_ptr<MessageThread> thread,
                                 std::weak_ptr<Channel> channel)
{
    this->thread_ = std::move(thread);
    this->channel_ = channel.lock();
    this->ui_.replyInput->setReply(
        this->thread_ ? this->thread_->root() : nullptr, std::move(channel));
    this->addMessagesFromThread();
    this->updateInputUI();

    if (!this->thread_) [[unlikely]]
    {
        this->replySubscriptionSignal_.disconnect();
        return;
    }

    auto updateCheckbox = [this]() {
        if (this->ui_.notificationCheckbox)
        {
            this->ui_.notificationCheckbox->setChecked(
                this->thread_->subscribed());
        }
    };
    updateCheckbox();

    this->replySubscriptionSignal_ =
        this->thread_->subscriptionUpdated.connect(updateCheckbox);
}

void ReplyThreadPopup::addMessagesFromThread()
{
    this->messageConnection_.reset();
    this->ui_.threadView->clearMessages();
    if (!this->thread_)
    {
        return;
    }

    const auto sourceChannel = this->channel_ ? this->channel_
                                              : this->split_->getSelectedChannel();
    if (!sourceChannel)
    {
        return;
    }
    this->setWindowTitle(TEXT_TITLE.arg(this->thread_->root()->loginName,
                                        sourceChannel->getName()));

    if (sourceChannel->isTwitchChannel())
    {
        this->virtualChannel_ =
            std::make_shared<TwitchChannel>(sourceChannel->getName());
    }
    else
    {
        this->virtualChannel_ = std::make_shared<Channel>(
            sourceChannel->getName(), Channel::Type::None);
    }

    auto rootOverrideFlags =
        std::optional<MessageFlags>(this->thread_->root()->flags);
    rootOverrideFlags->set(MessageFlag::DoNotLog);

    this->virtualChannel_->addMessage(
        this->thread_->root(), MessageContext::Repost, rootOverrideFlags);
    for (const auto &msgRef : this->thread_->replies())
    {
        if (auto msg = msgRef.lock())
        {
            auto overrideFlags = std::optional<MessageFlags>(msg->flags);
            overrideFlags->set(MessageFlag::DoNotLog);

            this->virtualChannel_->addMessage(msg, MessageContext::Repost,
                                              overrideFlags);
        }
    }

    this->ui_.threadView->setChannel(this->virtualChannel_);
    this->ui_.threadView->setSourceChannel(sourceChannel);

    this->messageConnection_ =
        std::make_unique<pajlada::Signals::ScopedConnection>(
            sourceChannel->messageAppended.connect(
                [this](MessagePtr &message, auto) {
                    if (message->replyThread == this->thread_)
                    {
                        auto overrideFlags =
                            std::optional<MessageFlags>(message->flags);
                        overrideFlags->set(MessageFlag::DoNotLog);

                        // same reply thread, add message
                        this->virtualChannel_->addMessage(
                            message, MessageContext::Repost, overrideFlags);
                    }
                }));
}

void ReplyThreadPopup::updateInputUI()
{
    auto channel = this->channel_ ? this->channel_
                                  : this->split_->getSelectedChannel();
    // Bail out if not a twitch channel.
    // Special twitch channels will hide their reply input box.
    if (!channel || !channel->isTwitchOrKickChannel())
    {
        this->ui_.replyInput->setVisible(false);
        return;
    }

    this->ui_.replyInput->setVisible(channel->isWritable());

    QString name;
    if (channel->isTwitchChannel())
    {
        auto user = getApp()->getAccounts()->twitch.getCurrent();
        if (!user->isAnon())
        {
            name = user->getUserName();
        }
    }
    else
    {
        auto user = getApp()->getAccounts()->kick.current();
        if (!user->isAnonymous())
        {
            name = user->username();
        }
    }
    QString placeholderText;
    if (name.isEmpty())
    {
        placeholderText = QStringLiteral("Log in to send messages...");
    }
    else
    {
        placeholderText = QStringLiteral("Reply as %1...").arg(name);
    }

    this->ui_.replyInput->setPlaceholderText(placeholderText);
}

void ReplyThreadPopup::giveFocus(Qt::FocusReason reason)
{
    this->ui_.replyInput->giveFocus(reason);
}

void ReplyThreadPopup::focusInEvent(QFocusEvent *event)
{
    this->giveFocus(event->reason());
}

}  // namespace chatterino
