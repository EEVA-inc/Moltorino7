// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/layouts/MessageLayoutContext.hpp"

#include "messages/Message.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"

#include <algorithm>

namespace chatterino {

namespace {

QString effectiveClientDetectionDisplayMode(const QString &value,
                                            bool legacyHighlights)
{
    const auto mode = value.trimmed().toLower();
    if (mode == QLatin1String("highlight") || mode == QLatin1String("icon") ||
        mode == QLatin1String("both") || mode == QLatin1String("off"))
    {
        return mode;
    }

    return legacyHighlights ? QStringLiteral("highlight")
                            : QStringLiteral("off");
}

}

void MessageLayoutContext::resetMessageTextCursor() const
{
    this->messageTextCursor_ = 0;
}

std::optional<MessageLayoutContext::MessageTextRange>
    MessageLayoutContext::claimMessageTextRange(QStringView renderedText) const
{
    if (renderedText.isEmpty() || this->message.messageText.isEmpty())
    {
        return std::nullopt;
    }

    auto start = this->message.messageText.indexOf(
        renderedText, this->messageTextCursor_, Qt::CaseSensitive);
    if (start < 0)
    {
        start = this->message.messageText.indexOf(
            renderedText, this->messageTextCursor_, Qt::CaseInsensitive);
    }
    if (start < 0)
    {
        return std::nullopt;
    }

    this->messageTextCursor_ = start + renderedText.size();
    return MessageTextRange{start, renderedText.size()};
}

void MessageColors::applyTheme(Theme *theme, bool isOverlay,
                               int backgroundOpacity)
{
    auto applyColors = [this](const auto &src) {
        this->regularBg = src.backgrounds.regular;
        this->alternateBg = src.backgrounds.alternate;

        this->disabled = src.disabled;
        this->selection = src.selection;

        this->regularText = src.textColors.regular;
        this->linkText = src.textColors.link;
        this->systemText = src.textColors.system;
        this->timestampText = src.textColors.timestamp.isValid()
                                  ? src.textColors.timestamp
                                  : this->systemText;
    };

    if (isOverlay)
    {
        this->channelBackground = theme->overlayMessages.background;
        this->channelBackground.setAlpha(std::clamp(backgroundOpacity, 0, 255));
        applyColors(theme->overlayMessages);
    }
    else
    {
        this->channelBackground = theme->splits.background;
        applyColors(theme->messages);
    }

    this->messageSeperator = theme->splits.messageSeperator;

    this->focusedLastMessageLine = theme->tabs.selected.backgrounds.regular;
    this->unfocusedLastMessageLine = theme->tabs.selected.backgrounds.unfocused;

    this->hasTransparency =
        this->regularBg.alpha() != 255 || this->alternateBg.alpha() != 255;
}

void MessagePreferences::connectSettings(Settings *settings,
                                         pajlada::Signals::SignalHolder &holder)
{
    settings->enableRedeemedHighlight.connect(
        [this](const auto &newValue) {
            this->enableRedeemedHighlight = newValue;
        },
        holder);

    settings->enableElevatedMessageHighlight.connect(
        [this](const auto &newValue) {
            this->enableElevatedMessageHighlight = newValue;
        },
        holder);

    settings->enableFirstMessageHighlight.connect(
        [this](const auto &newValue) {
            this->enableFirstMessageHighlight = newValue;
        },
        holder);

    settings->enableSubHighlight.connect(
        [this](const auto &newValue) {
            this->enableSubHighlight = newValue;
        },
        holder);

    settings->enableWatchStreakHighlight.connect(
        [this](const auto &newValue) {
            this->enableWatchStreakHighlight = newValue;
        },
        holder);

    settings->enableAutomodHighlight.connect(
        [this](const auto &newValue) {
            this->enableAutomodHighlight = newValue;
        },
        holder);

    const auto applyClientDetectionMode = [this,
                                           settings](const QString &value) {
        const auto mode = effectiveClientDetectionDisplayMode(
            value, settings->showClientDetectionHighlights.getValue());
        this->enableClientDetectionHighlight =
            mode == QLatin1String("highlight") || mode == QLatin1String("both");
        this->enableClientDetectionIcon =
            mode == QLatin1String("icon") || mode == QLatin1String("both");
    };

    settings->clientDetectionDisplayMode.connect(
        [applyClientDetectionMode](const auto &newValue) {
            applyClientDetectionMode(newValue);
        },
        holder);

    settings->showClientDetectionHighlights.connect(
        [settings, applyClientDetectionMode](const auto &) {
            applyClientDetectionMode(
                settings->clientDetectionDisplayMode.getValue());
        },
        holder);
    applyClientDetectionMode(settings->clientDetectionDisplayMode.getValue());

    settings->showAbnormalClientDetectionHighlights.connect(
        [this](const auto &newValue) {
            this->enableAbnormalClientDetectionHighlight = newValue;
        },
        holder);

    settings->clientDetectionWebColor.connect(
        [this](const auto &newValue) {
            this->clientDetectionWebColor = QColor(newValue);
        },
        holder);

    settings->clientDetectionAndroidColor.connect(
        [this](const auto &newValue) {
            this->clientDetectionAndroidColor = QColor(newValue);
        },
        holder);

    settings->clientDetectionIosColor.connect(
        [this](const auto &newValue) {
            this->clientDetectionIosColor = QColor(newValue);
        },
        holder);

    settings->clientDetectionAbnormalColor.connect(
        [this](const auto &newValue) {
            this->clientDetectionAbnormalColor = QColor(newValue);
        },
        holder);

    settings->enableAnnouncementHighlight.connect(
        [this](const auto &newValue) {
            this->enableAnnouncementHighlight = newValue;
        },
        holder);
    settings->enableColoredAnnouncementHighlight.connect(
        [this](const auto &newValue) {
            this->enableColoredAnnouncementHighlight = newValue;
        },
        holder);

    settings->alternateMessages.connect(
        [this](const auto &newValue) {
            this->alternateMessages = newValue;
        },
        holder);

    settings->separateMessages.connect(
        [this](const auto &newValue) {
            this->separateMessages = newValue;
        },
        holder);

    settings->lastMessageColor.connect(
        [this](const auto &newValue) {
            if (newValue.isEmpty())
            {
                this->lastMessageColor = QColor();
            }
            else
            {
                this->lastMessageColor = QColor(newValue);
            }
        },
        holder);

    settings->lastMessagePattern.connect(
        [this](const auto &newValue) {
            this->lastMessagePattern = static_cast<Qt::BrushStyle>(newValue);
        },
        holder);

    settings->fadeMessageHistory.connect(
        [this](const auto &newValue) {
            this->fadeMessageHistory = newValue;
        },
        holder);
}

}
