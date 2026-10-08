// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/UserInfoPopup.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/Literals.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/highlights/HighlightBlacklistUser.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/ignores/HiddenUser.hpp"
#include "controllers/ignores/HiddenUserController.hpp"
#include "controllers/userdata/UserDataController.hpp"
#include "messages/Emote.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "messages/Link.hpp"
#include "providers/bluzyrino/BluzyrinoBadges.hpp"
#include "providers/bttv/BttvBadges.hpp"
#include "providers/chatterino/ChatterinoBadges.hpp"
#include "providers/ffz/FfzBadges.hpp"
#include "providers/ffzap/FfzApBadges.hpp"
#include "providers/homies/HomiesBadges.hpp"
#include "providers/IvrApi.hpp"
#include "providers/jilchat/JilChatBadges.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickApi.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/moltorino/MoltorinoSupporterBadges.hpp"
#include "providers/pronouns/Pronouns.hpp"
#include "providers/seventv/SeventvBadges.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "providers/tiktok/TikTokAccount.hpp"
#include "providers/tiktok/TikTokBadge.hpp"
#include "providers/tiktok/TikTokChannel.hpp"
#include "providers/tiktok/TikTokChatServer.hpp"
#include "providers/tiktok/TikTokEmotes.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchBadges.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/twitch/TwitchNameHistory.hpp"
#include "providers/twitch/TwitchUsers.hpp"
#include "providers/youtube/YouTubeAccount.hpp"
#include "providers/youtube/YouTubeChannel.hpp"
#include "providers/youtube/YouTubeMessageBuilder.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Resources.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Clipboard.hpp"
#include "util/FormatTime.hpp"
#include "util/Helpers.hpp"
#include "util/IncognitoBrowser.hpp"
#include "util/IrcHelpers.hpp"
#include "util/LayoutCreator.hpp"
#include "util/PostToThread.hpp"
#include "util/Twitch.hpp"
#include "widgets/buttons/DrawnButton.hpp"
#include "widgets/buttons/LabelButton.hpp"
#include "widgets/buttons/PixmapButton.hpp"
#include "widgets/dialogs/EditUserNotesDialog.hpp"
#include "widgets/dialogs/ModeratorCommentsView.hpp"
#include "widgets/dialogs/UserLogsView.hpp"
#include "widgets/dialogs/UserRolesView.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/Line.hpp"
#include "widgets/helper/LiveIndicator.hpp"
#include "widgets/helper/ScalingSpacerItem.hpp"
#include "widgets/Label.hpp"
#include "widgets/layout/FlowLayout.hpp"
#include "widgets/MarkdownLabel.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/Scrollbar.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/Window.hpp"

#include <IrcMessage>
#include <QCache>
#include <QBuffer>
#include <QCryptographicHash>
#include <QImageReader>
#include <QCheckBox>
#include <QColor>
#include <QDate>
#include <QDesktopServices>
#include <QDialog>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMovie>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QSvgRenderer>
#include <QTimer>
#include <QTimeZone>
#include <QToolTip>
#include <QUrl>
#include <QUrlQuery>
#include <QShowEvent>
#include <QStringBuilder>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <limits>
#include <utility>

namespace {
constexpr QStringView TEXT_FOLLOWERS = u"Followers: %1";
constexpr QStringView TEXT_CREATED = u"Created: %1";
constexpr QStringView TEXT_TITLE = u"%1's Usercard - #%2";
constexpr QStringView TEXT_USER_ID = u"ID: ";
constexpr QStringView TEXT_UNAVAILABLE = u"(not available)";
constexpr QStringView TEXT_PRONOUNS = u"Pronouns: %1";
constexpr QStringView TEXT_UNSPECIFIED = u"(unspecified)";
constexpr QStringView TEXT_LOADING = u"(loading...)";

constexpr QStringView SEVENTV_TWITCH_USER_API =
    u"https://7tv.io/v3/users/twitch/%1";
constexpr QStringView SEVENTV_KICK_USER_API =
    u"https://7tv.io/v3/users/kick/%1";
constexpr QStringView SEVENTV_USER_PAGE = u"https://7tv.app/users/";

constexpr qint64 MAX_AVATAR_BYTES = 5 * 1024 * 1024;

using namespace chatterino;

enum class UsercardActionPlacement {
    Bar,
    Menu,
    Hidden,
};

UsercardActionPlacement usercardActionPlacement(const QString &value)
{
    if (value == u"menu")
    {
        return UsercardActionPlacement::Menu;
    }
    if (value == u"hidden")
    {
        return UsercardActionPlacement::Hidden;
    }
    return UsercardActionPlacement::Bar;
}

QString chatVaultTwitchChannelUrl(const QString &login)
{
    return QStringLiteral("https://chatvau.lt/channel/twitch/%1")
        .arg(QString::fromLatin1(QUrl::toPercentEncoding(login.toLower())));
}

QString sevenTVUserCacheKey(const QString &userID, bool isKick)
{
    return (isKick ? QStringLiteral("kick:") : QStringLiteral("twitch:")) +
           userID;
}

QCache<QString, QString> &sevenTVUserIDCache()
{
    static QCache<QString, QString> cache(1024);
    return cache;
}

QString joinTooltipOptions(QStringList options)
{
    if (options.size() < 2)
    {
        return options.value(0);
    }

    const auto last = options.takeLast();
    return options.join(", ") + " or " + last;
}

class PaintTooltipLabel final : public Label
{
protected:
    bool event(QEvent *event) override
    {
        if (event->type() != QEvent::ToolTip)
        {
            return Label::event(event);
        }

        const auto login = this->property("paint-login").toString().toLower();
        const bool isKick = this->property("paint-kick").toBool();
        auto *app = tryGetApp();
        if (!app || login.isEmpty() ||
            !getSettings()->displaySevenTVPaints)
        {
            return Label::event(event);
        }

        const auto paint = app->getSeventvPaints()->getPaint(login, isKick);
        const auto tooltip = paint ? paint->getTooltip() : QString{};
        if (tooltip.isEmpty())
        {
            return Label::event(event);
        }

        const auto *helpEvent = static_cast<QHelpEvent *>(event);
        QToolTip::showText(helpEvent->globalPos(), tooltip, this);
        return true;
    }
};

class UsercardBadgeStrip final : public BaseWidget
{
public:
    struct Badge {
        EmotePtr emote;
        QString tooltip;
        MessageElementFlags flags;
        std::shared_ptr<const TikTokBadgeArtwork> tiktok;
    };

    explicit UsercardBadgeStrip(QWidget *parent)
        : BaseWidget(parent)
    {
        this->setMouseTracking(true);
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        this->setMinimumHeight(this->rowHeight());
    }

    void setBadges(std::vector<Badge> badges)
    {
        this->badges_ = std::move(badges);
        this->updateGeometry();
        this->update();
    }

    bool hasAnimatedBadges() const
    {
        return std::ranges::any_of(this->badges_, [this](const Badge &badge) {
            return (badge.tiktok &&
                    (badge.tiktok->background->animated() ||
                     badge.tiktok->darkBackground->animated())) ||
                   badge.emote->images
                       .getImageOrLoadedNoLoad(
                           this->scale() * this->devicePixelRatioF(),
                           ImageSet::ScaleMode::Exact)
                       ->animated();
        });
    }

    bool empty() const
    {
        return this->badges_.empty();
    }

    QSize sizeHint() const override
    {
        if (this->badges_.empty())
        {
            return {0, this->rowHeight()};
        }

        const auto width = std::min(this->badgesWidth(),
                                    qRound(LOGICAL_PREFERRED_WIDTH *
                                           this->scale()));
        return {width, this->heightForWidth(width)};
    }

    bool hasHeightForWidth() const override
    {
        return true;
    }

    int heightForWidth(int width) const override
    {
        if (this->badges_.empty())
        {
            return this->rowHeight();
        }

        const auto available = std::max(1, width);
        auto rows = 1;
        auto rowWidth = 0;
        for (const auto &badge : this->badges_)
        {
            const auto nextWidth = this->badgeWidth(badge);
            if (rowWidth > 0 &&
                rowWidth + this->badgeSpacing() + nextWidth > available)
            {
                ++rows;
                rowWidth = 0;
            }
            if (rowWidth > 0)
            {
                rowWidth += this->badgeSpacing();
            }
            rowWidth += nextWidth;
        }
        return rows * this->rowHeight();
    }

protected:
    bool event(QEvent *event) override
    {
        if (event->type() != QEvent::ToolTip)
        {
            return BaseWidget::event(event);
        }

        const auto *helpEvent = static_cast<QHelpEvent *>(event);
        const auto index = this->badgeIndexAt(helpEvent->pos());
        if (!index || this->badges_[*index].tooltip.isEmpty())
        {
            return BaseWidget::event(event);
        }

        QToolTip::showText(helpEvent->globalPos(),
                           this->badges_[*index].tooltip, this);
        return true;
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto height = this->badgeSize();
        const auto spacing = this->badgeSpacing();
        auto y = (this->rowHeight() - height) / 2;
        auto x = 0;

        for (const auto &badge : this->badges_)
        {
            const auto width = this->badgeWidth(badge);
            if (x > 0 && x + width > this->width())
            {
                x = 0;
                y += this->rowHeight();
            }

            const auto image = badge.emote->images.getImageOrLoaded(
                this->scale() * this->devicePixelRatioF(),
                ImageSet::ScaleMode::Exact);
            if (badge.tiktok)
            {
                paintTikTokBadge(painter, QRectF(x, y, width, height),
                                 *badge.tiktok, image,
                                 getApp()->getThemes()->isLightTheme());
            }
            else if (const auto pixmap = image->pixmapOrLoad())
            {
                painter.setRenderHint(
                    QPainter::SmoothPixmapTransform,
                    badge.flags.has(MessageElementFlag::BadgeMoltorino));
                painter.drawPixmap(QRectF(x, y, width, height), *pixmap,
                                   pixmap->rect());
            }
            x += width + spacing;
        }
    }

    void scaleChangedEvent(float scale) override
    {
        BaseWidget::scaleChangedEvent(scale);
        this->setMinimumHeight(this->rowHeight());
        this->updateGeometry();
        this->update();
    }

private:
    static constexpr int LOGICAL_BADGE_SIZE = 18;
    static constexpr int LOGICAL_BADGE_SPACING = 3;
    static constexpr int LOGICAL_ROW_HEIGHT = 22;
    static constexpr int LOGICAL_PREFERRED_WIDTH = 420;

    int badgeSize() const
    {
        return std::max(1, qRound(LOGICAL_BADGE_SIZE * this->scale()));
    }

    int badgeSpacing() const
    {
        return std::max(1, qRound(LOGICAL_BADGE_SPACING * this->scale()));
    }

    int rowHeight() const
    {
        return std::max(1, qRound(LOGICAL_ROW_HEIGHT * this->scale()));
    }

    int badgesWidth() const
    {
        auto width = 0;
        for (const auto &badge : this->badges_)
        {
            if (width > 0)
            {
                width += this->badgeSpacing();
            }
            width += this->badgeWidth(badge);
        }
        return width;
    }

    int badgeWidth(const Badge &badge) const
    {
        const auto image = badge.emote->images.getImageOrLoadedNoLoad(
            this->scale() * this->devicePixelRatioF(),
            ImageSet::ScaleMode::Exact);
        if (badge.tiktok)
        {
            return qRound(tikTokBadgeSize(*badge.tiktok, image,
                                         this->badgeSize()).width());
        }
        const auto sourceSize = image->size();
        if (sourceSize.width() <= 0 || sourceSize.height() <= 0)
        {
            return this->badgeSize();
        }

        return std::max(
            1, qRound(this->badgeSize() * sourceSize.width() /
                      sourceSize.height()));
    }

    std::optional<size_t> badgeIndexAt(const QPoint &position) const
    {
        const auto height = this->badgeSize();
        const auto rowHeight = this->rowHeight();
        if (position.x() < 0 || position.y() < 0 ||
            position.y() >= this->height())
        {
            return std::nullopt;
        }

        auto y = (rowHeight - height) / 2;
        auto x = 0;
        for (size_t index = 0; index < this->badges_.size(); ++index)
        {
            const auto width = this->badgeWidth(this->badges_[index]);
            if (x > 0 && x + width > this->width())
            {
                x = 0;
                y += rowHeight;
            }
            if (position.x() >= x && position.x() < x + width &&
                position.y() >= y && position.y() < y + height)
            {
                return index;
            }
            x += width + this->badgeSpacing();
        }
        return std::nullopt;
    }

    std::vector<Badge> badges_;
};

class UsercardPaintButton final : public Button
{
public:
    explicit UsercardPaintButton(BaseWidget *parent)
        : Button(parent)
    {
        this->setCursor(Qt::PointingHandCursor);
        this->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        this->setMouseEffectColor(QColor(Qt::transparent));
        this->setScaleIndependentHeight(18);
        this->setMaximumWidth(qRound(LOGICAL_MAX_WIDTH * this->scale()));
    }

    void setPaint(std::shared_ptr<Paint> paint)
    {
        this->paint_ = std::move(paint);
        this->setToolTip(
            this->paint_ ? this->paint_->getTooltip() +
                               QStringLiteral("<br>Open on 7Database")
                         : QString{});
        this->refreshAppearance();
    }

    const std::shared_ptr<Paint> &paint() const
    {
        return this->paint_;
    }

    void setUserColor(const QColor &color)
    {
        if (this->userColor_ != color)
        {
            this->userColor_ = color;
            this->invalidateContent();
        }
    }

    void refreshAppearance()
    {
        this->paintLoaded_ = this->paint_ && this->paint_->loaded();
        this->setContentCacheEnabled(this->paint_ &&
                                     !this->paint_->animated());
        this->updateGeometry();
        this->invalidateContent();
    }

    void refreshAfterImageLayout()
    {
        if (!this->paint_)
        {
            return;
        }

        const bool loaded = this->paint_->loaded();
        const bool shouldCache = !this->paint_->animated();
        const bool cacheChanged = shouldCache != this->contentCacheEnabled();
        const bool finishedLoading = loaded && !this->paintLoaded_;
        this->paintLoaded_ = loaded;

        if (cacheChanged)
        {
            this->setContentCacheEnabled(shouldCache);
        }
        if (cacheChanged || finishedLoading)
        {
            this->invalidateContent();
        }
    }

    void refreshAnimationFrame()
    {
        if (!this->paint_ || !this->paint_->animated())
        {
            return;
        }

        this->paintLoaded_ = true;
        this->setContentCacheEnabled(false);
        this->invalidateContent();
    }

    QSize sizeHint() const override
    {
        if (!this->paint_)
        {
            return {0, this->height()};
        }

        const auto margins = this->contentMargins();
        const auto metrics = this->fontMetrics();
        const auto width = qCeil(this->leftPadding() +
                                 metrics.horizontalAdvance(
                                     QStringLiteral("7TV Paint: ")) +
                                 metrics.horizontalAdvance(
                                     this->paint_->getName()) +
                                 margins.right());
        return {std::min(width, this->maximumWidth()), this->height()};
    }

protected:
    void paintContent(QPainter &painter) override
    {
        if (!this->paint_)
        {
            return;
        }

        const auto margins = this->contentMargins();
        const auto metrics = this->fontMetrics();
        const auto prefix = QStringLiteral("7TV Paint: ");
        const auto prefixX = this->leftPadding();
        const auto prefixWidth = metrics.horizontalAdvance(prefix);
        const auto nameX = prefixX + prefixWidth;
        const auto availableNameWidth =
            std::max<qreal>(0, this->width() - nameX - margins.right());
        const auto displayName = metrics.elidedText(
            this->paint_->getName(), Qt::ElideRight,
            qFloor(availableNameWidth));

        const auto paintedHeight =
            metrics.height() + margins.top() + margins.bottom();

        const auto baseline =
            (this->height() - metrics.height()) / 2.0 + metrics.ascent();
        const auto paintedTop = baseline - margins.top() - metrics.ascent();

        painter.setFont(this->font());
        painter.setPen(this->theme->window.text);
        painter.drawText(QPointF(prefixX, baseline), prefix);

        const auto fallback = this->userColor_.isValid()
                                  ? this->userColor_
                                  : this->theme->window.text;
        const auto pixmapWidth =
            std::max<qreal>(1, this->width() - nameX + margins.left());
        const auto pixmap = this->paint_->getPixmap(
            displayName, this->font(), fallback,
            QSizeF(pixmapWidth, paintedHeight), this->scale(),
            this->devicePixelRatioF(), margins);

        this->paintLoaded_ = this->paint_->loaded();
        painter.drawPixmap(QPointF(nameX - margins.left(), paintedTop),
                           pixmap);
    }

    void scaleChangedEvent(float scale) override
    {
        Button::scaleChangedEvent(scale);
        this->setMaximumWidth(qRound(LOGICAL_MAX_WIDTH * this->scale()));
        this->updateGeometry();
        this->invalidateContent();
    }

    void themeChangedEvent() override
    {
        Button::themeChangedEvent();
        this->invalidateContent();
    }

private:
    static constexpr int LOGICAL_MAX_WIDTH = 360;

    qreal leftPadding() const
    {
        return 8 * this->scale();
    }

    QMarginsF contentMargins() const
    {
        const auto scale = this->scale();
        const auto shadow = this->paint_->getShadowMargins(scale);

        const qreal horizontalLimit = 8 * scale;
        const qreal verticalLimit = 4 * scale;
        return {
            std::clamp(shadow.left(), qreal(0), horizontalLimit),
            std::clamp(shadow.top(), qreal(0), verticalLimit),
            std::clamp(shadow.right(), qreal(0), horizontalLimit),
            std::clamp(shadow.bottom(), qreal(0), verticalLimit),
        };
    }

    std::shared_ptr<Paint> paint_;
    QColor userColor_;
    bool paintLoaded_ = false;
};

class NameHistoryMenuRow final : public QWidget
{
public:
    NameHistoryMenuRow(QString login, QString leftText, QString rightText,
                       QWidget *parent)
        : QWidget(parent)
        , login_(std::move(login))
    {
        this->setCursor(Qt::PointingHandCursor);
        this->setMouseTracking(true);
        this->setToolTip("Click to copy " + this->login_);

        const auto metrics = this->fontMetrics();
        const auto loginWidth =
            std::max(metrics.horizontalAdvance("koplayzenthraquiluxmorive") + 8,
                     132);
        const auto dateWidth =
            metrics.horizontalAdvance("Sep 30, 2026") + 8;
        const auto dashWidth = metrics.horizontalAdvance("-") + 8;

        auto *layout = new QGridLayout(this);
        layout->setContentsMargins(8, 2, 8, 2);
        layout->setHorizontalSpacing(4);
        layout->setVerticalSpacing(0);

        auto *loginLabel = new QLabel(
            metrics.elidedText(this->login_, Qt::ElideRight, loginWidth),
            this);
        loginLabel->setFixedWidth(loginWidth);
        loginLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        loginLabel->setToolTip(this->login_);
        layout->addWidget(loginLabel, 0, 0, Qt::AlignVCenter);

        auto *leftLabel = new QLabel(std::move(leftText), this);
        leftLabel->setFixedWidth(dateWidth);
        leftLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        leftLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(leftLabel, 0, 1, Qt::AlignVCenter);

        auto *dashLabel = new QLabel("-", this);
        dashLabel->setFixedWidth(dashWidth);
        dashLabel->setAlignment(Qt::AlignCenter);
        dashLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(dashLabel, 0, 2, Qt::AlignVCenter);

        auto *rightLabel = new QLabel(std::move(rightText), this);
        rightLabel->setFixedWidth(dateWidth);
        rightLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        rightLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(rightLabel, 0, 3, Qt::AlignVCenter);

        const auto height = std::max(metrics.height() + 6, 22);
        this->setFixedSize(loginWidth + dateWidth * 2 + dashWidth + 36,
                           height);
    }

protected:
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::Enter)
        {
            this->hovered_ = true;
            this->update();
        }
        else if (event->type() == QEvent::Leave)
        {
            this->hovered_ = false;
            this->update();
        }

        return QWidget::event(event);
    }

    void paintEvent(QPaintEvent *event) override
    {
        if (this->hovered_)
        {
            QPainter painter(this);
            auto highlight = this->palette().color(QPalette::Highlight);
            highlight.setAlpha(70);
            painter.fillRect(this->rect(), highlight);
        }

        QWidget::paintEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton)
        {
            QWidget::mouseReleaseEvent(event);
            return;
        }

        crossPlatformCopy(this->login_);
        QToolTip::showText(event->globalPosition().toPoint(),
                           QString("Copied %1").arg(this->login_), this);

        for (auto *widget = this->parentWidget(); widget != nullptr;
             widget = widget->parentWidget())
        {
            if (auto *menu = qobject_cast<QMenu *>(widget))
            {
                menu->close();
                break;
            }
        }
    }

private:
    QString login_;
    bool hovered_ = false;
};

class ClickableColorRow final : public Button
{
public:
    ClickableColorRow()
        : Button(nullptr)
        , layout_(this)
    {
        this->layout_.setContentsMargins(8, 0, 8, 0);
        this->layout_.setSpacing(5);
        this->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    }

    QHBoxLayout *layout()
    {
        return &this->layout_;
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        // Keep Button's reliable click handling without its hover/click wash.
    }

    void paintContent(QPainter & /*painter*/) override
    {
    }

private:
    QHBoxLayout layout_;
};

template <typename LabelType = Label>
LabelType *addCopyableLabel(LayoutCreator<QHBoxLayout> box,
                            const char *tooltip,
                            PixmapButton **copyButton = nullptr)
{
    auto label = box.emplace<LabelType>();
    auto button = box.emplace<PixmapButton>();
    if (copyButton != nullptr)
    {
        button.assign(copyButton);
    }
    button->setPixmap(getApp()->getThemes()->buttons.copy);
    button->setScaleIndependentSize(18, 18);
    button->setDim(DimButton::Dim::Lots);
    button->setToolTip(tooltip);
    QObject::connect(
        button.getElement(), &Button::leftClicked,
        [label = label.getElement()] {
            auto copyText = label->property("copy-text").toString();

            crossPlatformCopy(copyText.isEmpty() ? label->getText() : copyText);
        });

    return label.getElement();
};

void createUsercardStatusRow(LayoutCreator<QVBoxLayout> &vbox,
                             QWidget **rowOut, QLabel **iconOut,
                             Label **labelOut)
{
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);

    layout->setContentsMargins(4, 0, 8, 0);
    layout->setSpacing(4);

    auto *icon = new QLabel(row);
    icon->setVisible(false);
    layout->addWidget(icon, 0, Qt::AlignVCenter);

    auto *label = new Label("");
    label->setPadding({});
    layout->addWidget(label, 0, Qt::AlignVCenter);
    layout->addStretch(1);

    row->setVisible(false);
    vbox->addWidget(row);

    *rowOut = row;
    *iconOut = icon;
    *labelOut = label;
}

void createUsercardColorRow(LayoutCreator<QVBoxLayout> &vbox, QWidget **rowOut,
                            QWidget **swatchOut, Label **labelOut)
{
    auto *row = new ClickableColorRow;
    auto *layout = row->layout();
    row->setCursor(Qt::PointingHandCursor);
    row->setToolTip("Click to copy color");

    auto *swatch = new QFrame(row);
    swatch->setObjectName("UsercardColorSwatch");
    swatch->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(swatch, 0, Qt::AlignVCenter);

    auto *label = new Label("");
    label->setPadding({});
    label->setCursor(Qt::PointingHandCursor);
    label->setToolTip(row->toolTip());
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(label, 0, Qt::AlignVCenter);
    layout->addStretch(1);

    row->setVisible(false);
    vbox->addWidget(row);

    *rowOut = row;
    *swatchOut = swatch;
    *labelOut = label;
}

QPixmap renderUsercardStatusIcon(const QString &path, int size, qreal scale)
{
    static QCache<QString, QPixmap> cache(128);
    const auto key =
        QStringLiteral("%1:%2:%3").arg(path).arg(size).arg(scale);
    if (auto *cached = cache.object(key))
    {
        return *cached;
    }

    QPixmap pixmap(QSize(size, size) * scale);
    pixmap.setDevicePixelRatio(scale);
    pixmap.fill(Qt::transparent);

    QSvgRenderer renderer(path);
    QPainter painter(&pixmap);
    renderer.render(&painter, QRectF(0, 0, size, size));

    cache.insert(key, new QPixmap(pixmap));
    return pixmap;
}

QDateTime parseIvrTimestamp(const QString &isoTimestamp)
{
    auto timestamp = QDateTime::fromString(isoTimestamp, Qt::ISODateWithMs);
    if (!timestamp.isValid())
    {
        timestamp = QDateTime::fromString(isoTimestamp, Qt::ISODate);
    }
    if (!timestamp.isValid() && isoTimestamp.contains('.'))
    {
        auto trimmed = isoTimestamp;
        const auto dotIndex = trimmed.indexOf('.');
        const auto zoneIndex = trimmed.indexOf('Z', dotIndex);
        if (zoneIndex > dotIndex + 4)
        {
            trimmed = trimmed.left(dotIndex + 4) + trimmed.mid(zoneIndex);
            timestamp = QDateTime::fromString(trimmed, Qt::ISODateWithMs);
        }
    }

    return timestamp;
}

QString formatIvrDate(const QString &isoTimestamp)
{
    const auto timestamp = parseIvrTimestamp(isoTimestamp);
    if (!timestamp.isValid())
    {
        return {};
    }

    return timestamp.toLocalTime().date().toString(Qt::ISODate);
}

int completeCalendarMonthsBetween(const QDate &from, const QDate &to)
{
    if (!from.isValid() || !to.isValid() || from > to)
    {
        return 0;
    }

    auto months = (to.year() - from.year()) * 12 + (to.month() - from.month());
    if (to.day() < from.day())
    {
        --months;
    }

    return std::max(months, 0);
}

QString formatUsercardCount(int count, const QString &unit)
{
    return QStringLiteral("%1 %2%3")
        .arg(count)
        .arg(unit)
        .arg(count == 1 ? QString() : QStringLiteral("s"));
}

QString formatUsercardYearsMonths(int totalMonths)
{
    if (totalMonths < 12)
    {
        return {};
    }

    const auto years = totalMonths / 12;
    const auto months = totalMonths % 12;
    auto result = QStringLiteral("%1y").arg(years);
    if (months > 0)
    {
        result += QStringLiteral(" %1m").arg(months);
    }

    return QStringLiteral(" (%1)").arg(result);
}

QString formatUsercardFollowRelativeTime(const QDate &followedDate)
{
    const auto today = QDateTime::currentDateTimeUtc().date();
    if (!followedDate.isValid() || followedDate > today)
    {
        return {};
    }

    const auto months = completeCalendarMonthsBetween(followedDate, today);
    if (months >= 12)
    {
        return formatUsercardYearsMonths(months);
    }
    if (months >= 1)
    {
        return QStringLiteral(" (%1)")
            .arg(formatUsercardCount(months, QStringLiteral("month")));
    }

    const auto days = followedDate.daysTo(today);
    if (days >= 14)
    {
        return QStringLiteral(" (%1)")
            .arg(formatUsercardCount(days / 7, QStringLiteral("week")));
    }
    if (days > 0)
    {
        return QStringLiteral(" (%1)")
            .arg(formatUsercardCount(days, QStringLiteral("day")));
    }

    return QStringLiteral(" (today)");
}

QString formatUsercardStatus(const IvrUserProfile &profile)
{
    if (profile.isStaff)
    {
        return "Staff";
    }
    if (profile.isPartner)
    {
        return "Partner";
    }
    if (profile.isAffiliate)
    {
        return "Affiliate";
    }

    return "Non Affiliate";
}

bool checkUsercardMessage(const QString &userName, const QString &userID,
                          MessagePlatform platform, const MessagePtr &message)
{
    if (!message)
    {
        return false;
    }
    if (message->platform == MessagePlatform::TikTok &&
        platform != MessagePlatform::TikTok)
    {
        return false;
    }
    if (platform == MessagePlatform::YouTube ||
        platform == MessagePlatform::TikTok)
    {
        return message->platform == platform && !userID.isEmpty() &&
               message->userID == userID;
    }
    if (message->flags.has(MessageFlag::Whisper))
    {
        return false;
    }

    bool isSubscription = message->flags.has(MessageFlag::Subscription) &&
                          message->loginName.isEmpty() &&
                          message->messageText.split(" ").at(0).compare(
                              userName, Qt::CaseInsensitive) == 0;

    bool isModAction =
        message->timeoutUser.compare(userName, Qt::CaseInsensitive) == 0;
    bool isSelectedUser =
        message->loginName.compare(userName, Qt::CaseInsensitive) == 0;

    return (isSubscription || isModAction || isSelectedUser);
}

QDate usercardMessageDate(const MessagePtr &message)
{
    if (!message || message->flags.has(MessageFlag::System) ||
        !message->serverReceivedTime.isValid())
    {
        return {};
    }

    return message->serverReceivedTime.toLocalTime().date();
}

MessagePtr makeUsercardDateSeparator(const QDate &date)
{
    auto separator = std::const_pointer_cast<Message>(makeSystemMessage(
        QLocale().toString(date, QLocale::LongFormat), QTime(0, 0)));
    separator->serverReceivedTime =
        QDateTime(date, QTime(0, 0), QTimeZone::systemTimeZone());
    return separator;
}

std::vector<MessagePtr> withUsercardDateSeparators(
    const std::vector<MessagePtr> &messages,
    const QDate &connectedNewerDate = {})
{
    std::vector<MessagePtr> result;
    result.reserve(messages.size() + 4);
    QDate previousDate;

    for (const auto &message : messages)
    {
        const auto date = usercardMessageDate(message);
        if (date.isValid() && previousDate.isValid() && date != previousDate)
        {
            result.push_back(makeUsercardDateSeparator(date));
        }
        if (date.isValid())
        {
            previousDate = date;
        }
        result.push_back(message);
    }

    if (previousDate.isValid() && connectedNewerDate.isValid() &&
        previousDate != connectedNewerDate)
    {
        result.push_back(makeUsercardDateSeparator(connectedNewerDate));
    }

    return result;
}

QDate oldestUsercardMessageDate(const ChannelPtr &channel)
{
    if (!channel)
    {
        return {};
    }

    for (const auto &message : channel->getMessageSnapshot())
    {
        const auto date = usercardMessageDate(message);
        if (date.isValid())
        {
            return date;
        }
    }
    return {};
}

bool messageHasTwitchBadge(const Message &message, QStringView badge)
{
    const auto badgeName = badge.toString();
    for (const auto &twitchBadge : message.twitchBadges)
    {
        if (twitchBadge.key_.compare(badgeName, Qt::CaseInsensitive) == 0)
        {
            return true;
        }
    }

    return false;
}

ChannelPtr filterMessages(const QString &userName, const QString &userID,
                          MessagePlatform platform, const ChannelPtr &channel)
{
    std::vector<MessagePtr> snapshot = channel->getMessageSnapshot();

    ChannelPtr channelPtr;
    if (platform == MessagePlatform::YouTube ||
        platform == MessagePlatform::TikTok)
    {
        channelPtr = std::make_shared<Channel>(
            channel->getName(), platform == MessagePlatform::TikTok
                                    ? Channel::Type::TikTok
                                    : Channel::Type::YouTube);
    }
    else if (channel->isTwitchChannel())
    {
        channelPtr = std::make_shared<TwitchChannel>(channel->getName());
    }
    else
    {
        channelPtr =
            std::make_shared<Channel>(channel->getName(), Channel::Type::None);
    }

    std::vector<MessagePtr> matching;
    matching.reserve(snapshot.size());
    for (const auto &message : snapshot)
    {
        if (checkUsercardMessage(userName, userID, platform, message))
        {
            matching.push_back(message);
        }
    }

    for (const auto &message : withUsercardDateSeparators(matching))
    {
        channelPtr->addMessage(message, MessageContext::Repost);
    }

    return channelPtr;
};

QString escapeIrcTagValue(QString value)
{
    value.replace(QChar(u'\\'), QStringLiteral("\\\\"));
    value.replace(QChar(u';'), QStringLiteral("\\:"));
    value.replace(QChar(u' '), QStringLiteral("\\s"));
    value.replace(QChar(u'\r'), QStringLiteral("\\r"));
    value.replace(QChar(u'\n'), QStringLiteral("\\n"));
    return value;
}

QString cleanIrcMessageBody(QString value)
{
    value.replace(QChar(u'\r'), QChar(u' '));
    value.replace(QChar(u'\n'), QChar(u' '));
    return value;
}

MessagePtr makeUsercardModLogMessage(const GqlUsercardMessage &message,
                                      TwitchChannel *twitchChannel,
                                      const QString &channelName,
                                      const QString &fallbackUserId)
{
    auto sentAt = parseIvrTimestamp(message.sentAt);
    if (!sentAt.isValid())
    {
        sentAt = QDateTime::currentDateTime();
    }
    else
    {
        sentAt = sentAt.toLocalTime();
    }

    const auto userId =
        message.senderId.isEmpty() ? fallbackUserId : message.senderId;
    auto displayName = message.senderDisplayName.trimmed();
    if (displayName.isEmpty())
    {
        displayName = message.senderLogin;
    }
    const auto login =
        message.senderLogin.isEmpty() ? displayName : message.senderLogin;
    const auto body = cleanIrcMessageBody(message.text);

    if (twitchChannel != nullptr && !login.isEmpty())
    {
        QStringList tags;
        if (!message.id.isEmpty())
        {
            tags << QStringLiteral("id=") + escapeIrcTagValue(message.id);
        }
        if (!userId.isEmpty())
        {
            tags << QStringLiteral("user-id=") + escapeIrcTagValue(userId);
        }
        if (!message.senderColor.isEmpty())
        {
            tags << QStringLiteral("color=") +
                        escapeIrcTagValue(message.senderColor);
        }
        if (!message.senderBadges.isEmpty())
        {
            tags << QStringLiteral("badges=") +
                        escapeIrcTagValue(message.senderBadges);
        }
        if (!twitchChannel->roomId().isEmpty())
        {
            tags << QStringLiteral("room-id=") +
                        escapeIrcTagValue(twitchChannel->roomId());
        }
        if (!displayName.isEmpty())
        {
            tags << QStringLiteral("display-name=") +
                        escapeIrcTagValue(displayName);
        }
        tags << QStringLiteral("login=") + escapeIrcTagValue(login);
        if (sentAt.isValid())
        {
            tags << QStringLiteral("tmi-sent-ts=") +
                        QString::number(sentAt.toMSecsSinceEpoch());
        }
        tags << QStringLiteral("historical=1");

        const auto tagsText =
            tags.isEmpty() ? QString() : u"@" % tags.join(';') % u" ";
        const auto fakeIrcData =
            QStringLiteral("%1:%2!%2@%2.tmi.twitch.tv PRIVMSG #%3 :%4")
                .arg(tagsText, login, twitchChannel->getName(), body);

        auto *fakeMessage =
            Communi::IrcMessage::fromData(fakeIrcData.toUtf8(), nullptr);
        if (fakeMessage != nullptr && fakeMessage->command() == "PRIVMSG")
        {
            MessageParseArgs args;
            args.allowIgnore = false;
            auto result = MessageBuilder::makeIrcMessage(
                twitchChannel, fakeMessage, args, body, 0);
            auto builtMessage = std::move(result.first);
            fakeMessage->deleteLater();
            fakeMessage = nullptr;

            if (builtMessage)
            {
                builtMessage->flags.set(MessageFlag::DoNotLog,
                                        MessageFlag::DoNotTriggerNotification);
                if (message.isDeleted)
                {
                    builtMessage->flags.set(MessageFlag::Disabled,
                                            MessageFlag::InvalidReplyTarget);
                }
                return builtMessage;
            }
        }
        if (fakeMessage != nullptr)
        {
            fakeMessage->deleteLater();
        }
    }

    auto color = QColor(message.senderColor);
    const auto userColor = color.isValid() ? MessageColor(color)
                                           : MessageColor(MessageColor::Text);

    MessageBuilder builder;
    builder->id = message.id;
    builder->loginName = message.senderLogin;
    builder->displayName = displayName;
    builder->userID = userId;
    builder->messageText = body;
    builder->searchText = displayName + QStringLiteral(": ") + body;
    builder->channelName = channelName;
    builder->serverReceivedTime = sentAt;
    builder->usernameColor = color;
    builder->flags.set(MessageFlag::DoNotLog,
                       MessageFlag::DoNotTriggerNotification);
    if (message.isDeleted)
    {
        builder->flags.set(MessageFlag::Disabled,
                           MessageFlag::InvalidReplyTarget);
    }

    builder.emplace<TimestampElement>(sentAt.time());
    builder
        .emplace<TextElement>(displayName + QStringLiteral(":"),
                              MessageElementFlag::Username, userColor,
                              FontStyle::ChatMediumBold)
        ->setLink({Link::UserInfo, message.senderLogin});
    builder.appendOrEmplaceText(body, MessageColor::Text);

    return builder.release();
}

QDateTime oldestUsercardMessageTime(const ChannelPtr &channel)
{
    QDateTime oldest;
    if (!channel)
    {
        return oldest;
    }

    for (const auto &message : channel->getMessageSnapshot())
    {
        if (message == nullptr || message->flags.has(MessageFlag::System) ||
            !message->serverReceivedTime.isValid())
        {
            continue;
        }

        if (!oldest.isValid() || message->serverReceivedTime < oldest)
        {
            oldest = message->serverReceivedTime;
        }
    }

    return oldest;
}

qreal usercardMessagePreloadDistance(const Scrollbar &scrollbar)
{
    return std::clamp<qreal>(scrollbar.getPageSize() * 0.75, 8.0, 24.0);
}

const auto borderColor = QColor(255, 255, 255, 80);

int calculateTimeoutDuration(TimeoutButton timeout)
{
    static const QMap<QString, int> durations{
        {"s", 1}, {"m", 60}, {"h", 3600}, {"d", 86400}, {"w", 604800},
    };
    const auto seconds = qint64(timeout.second) * durations[timeout.first];
    return int(std::clamp<qint64>(seconds, 0,
                                  std::numeric_limits<int>::max()));
}

QString normalizeModerationReason(QString reason)
{
    reason.replace('\r', ' ');
    reason.replace('\n', ' ');
    return reason.trimmed();
}

QString appendModerationReason(QString command, const QString &reason)
{
    const auto cleanedReason = normalizeModerationReason(reason);
    if (cleanedReason.isEmpty())
    {
        return command;
    }

    return command + ' ' + cleanedReason;
}

QString timeoutButtonReason(int index)
{
    if (index < 0)
    {
        return {};
    }

    const auto reasons = getSettings()->timeoutButtonReasons.getValue();
    if (index >= static_cast<int>(reasons.size()))
    {
        return {};
    }

    return normalizeModerationReason(reasons[index]);
}

QString timeoutBanReason()
{
    return normalizeModerationReason(
        getSettings()->timeoutBanReason.getValue());
}

bool shouldPromptForModerationReason(Qt::MouseButton button)
{
    if (button == Qt::RightButton &&
        getSettings()->timeoutReasonPromptOnRightClick.getValue())
    {
        return true;
    }

    if (!getSettings()->timeoutReasonPromptOnModifier.getValue())
    {
        return false;
    }

    const auto configuredModifier =
        getSettings()->timeoutReasonPromptModifier.getValue();
    const auto modifiers = QGuiApplication::keyboardModifiers();
    if (configuredModifier == "Ctrl")
    {
        return modifiers.testFlag(Qt::ControlModifier);
    }
    if (configuredModifier == "Alt")
    {
        return modifiers.testFlag(Qt::AltModifier);
    }

    return modifiers.testFlag(Qt::ShiftModifier);
}

bool shouldHandleModerationButtonClick(Qt::MouseButton button)
{
    return button == Qt::LeftButton ||
           (button == Qt::RightButton &&
            getSettings()->timeoutReasonPromptOnRightClick.getValue());
}

class ModerationReasonPopup final : public DraggablePopup
{
public:
    ModerationReasonPopup(const QString &title, const QString &placeholder,
                          const QString &initialReason,
                          bool showSendButton,
                          std::function<void(QString)> onSend,
                          QWidget *parent = nullptr)
        : DraggablePopup(true, parent)
        , showSendButton_(showSendButton)
        , onSend_(std::move(onSend))
    {
        this->setWindowTitle(title);
        this->setAttribute(Qt::WA_DeleteOnClose);
        this->setScaleIndependentSize(showSendButton ? QSize(360, 42)
                                                     : QSize(290, 42));

        auto layout = LayoutCreator<QWidget>(this->getLayoutContainer())
                          .setLayoutType<QHBoxLayout>();
        this->layout_ = layout.getElement();

        this->input_ = layout.emplace<QLineEdit>().getElement();
        this->input_->setPlaceholderText(placeholder);
        this->input_->setText(initialReason);
        this->input_->setMouseTracking(true);
        this->input_->setSizePolicy(QSizePolicy::Expanding,
                                    QSizePolicy::Fixed);

        if (this->showSendButton_)
        {
            this->sendButton_ = layout.emplace<QPushButton>("Send").getElement();
            this->sendButton_->setSizePolicy(QSizePolicy::Fixed,
                                             QSizePolicy::Fixed);
            this->sendButton_->setCursor(Qt::PointingHandCursor);
            this->sendButton_->setMouseTracking(true);

            QObject::connect(this->sendButton_, &QPushButton::clicked, this,
                             [this] {
                                 this->send();
                             });
        }
        QObject::connect(this->input_, &QLineEdit::returnPressed, this,
                         [this] {
                             this->send();
                         });

        this->applyScaledLayout();
    }

    void showCenteredAt(const QPoint &center)
    {
        this->show();
        this->moveTo(center - QPoint(this->width() / 2, this->height() / 2),
                     widgets::BoundsChecking::DesiredPosition);
    }

protected:
    void scaleChangedEvent(float scale) override
    {
        DraggablePopup::scaleChangedEvent(scale);
        this->applyScaledLayout();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape)
        {
            QTimer::singleShot(0, this, &QWidget::close);
            return;
        }

        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            (!event->modifiers() ||
             event->modifiers().testFlag(Qt::KeypadModifier)))
        {
            this->send();
            return;
        }

        DraggablePopup::keyPressEvent(event);
    }

    void showEvent(QShowEvent *event) override
    {
        DraggablePopup::showEvent(event);

        this->input_->setFocus(Qt::PopupFocusReason);
        this->input_->selectAll();
    }

private:
    void applyScaledLayout()
    {
        const auto effectiveScale = std::max(0.75F, this->scale());
        const int marginX = std::max(4, int(6 * effectiveScale));
        const int marginY = std::max(2, int(4 * effectiveScale));
        const int spacing = std::max(4, int(6 * effectiveScale));
        const int controlHeight = std::max(22, int(24 * effectiveScale));

        this->layout_->setContentsMargins(marginX, marginY, marginX, marginY);
        this->layout_->setSpacing(spacing);

        const auto uiFont =
            getApp()->getFonts()->getFont(FontStyle::UiMedium, effectiveScale);
        const auto buttonFont = getApp()->getFonts()->getFont(
            FontStyle::UiMediumBold, effectiveScale);
        this->input_->setFont(uiFont);
        this->input_->setFixedHeight(controlHeight);
        if (this->sendButton_ != nullptr)
        {
            this->sendButton_->setFont(buttonFont);
            const QFontMetrics buttonMetrics(buttonFont);
            this->sendButton_->setFixedHeight(controlHeight);
            this->sendButton_->setMinimumWidth(
                buttonMetrics.horizontalAdvance("Send") +
                std::max(22, int(24 * effectiveScale)));
        }
    }

    void send()
    {
        if (this->sent_)
        {
            return;
        }
        this->sent_ = true;

        if (this->onSend_)
        {
            this->onSend_(this->input_->text());
        }
        QTimer::singleShot(0, this, &QWidget::close);
    }

    QHBoxLayout *layout_{};
    QLineEdit *input_{};
    QPushButton *sendButton_{};
    bool showSendButton_ = false;
    std::function<void(QString)> onSend_;
    bool sent_ = false;
};

QPixmap readUsercardAvatar(QByteArray data)
{
    QBuffer buffer(&data);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    const auto size = reader.size();
    if (!size.isValid() || size.width() > 2048 || size.height() > 2048)
    {
        return {};
    }
    return QPixmap::fromImage(reader.read());
}

QString hashUrl(const QString &url)
{
    QByteArray bytes;

    bytes.append(url.toUtf8());
    QByteArray hashBytes(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));

    return hashBytes.toHex();
}

}  // namespace

namespace chatterino {

using namespace literals;

static void openYouTubeUrl(const QUrl &url);

void UserInfoPopup::addShortcuts()
{
    const auto makeModeratorHotkey = [this](bool promptForReason) {
        return [this, promptForReason](
                   std::vector<QString> arguments) -> QString {
            if (!this->shouldShowModerationActions())
            {
                return "";
            }

            if (arguments.empty())
            {
                return "This shortcut does not have a usercard action.";
            }
            const auto &target = arguments.at(0);
            UsercardModerationRequest request;

            if (target == "ban")
            {
                request.action = UsercardModerationAction::Ban;
                request.reason = timeoutBanReason();
            }
            else if (target == "unban" && !promptForReason)
            {
                request.action = UsercardModerationAction::Unban;
            }
            else
            {
                bool ok = false;
                const auto buttonNum = target.toInt(&ok);
                const auto &timeoutButtons =
                    getSettings()->timeoutButtons.getValue();
                if (!ok || buttonNum <= 0 ||
                    buttonNum > static_cast<int>(timeoutButtons.size()))
                {
                    return QString("That usercard action is not available: %1")
                        .arg(target);
                }
                const auto &button = timeoutButtons.at(buttonNum - 1);
                request.action = UsercardModerationAction::Timeout;
                request.durationSeconds = calculateTimeoutDuration(button);
                request.reason = timeoutButtonReason(buttonNum - 1);
            }

            if (promptForReason && !this->isYouTube_ && !this->isTikTok_)
            {
                this->showUsercardModerationReasonPopup(request);
            }
            else
            {
                this->executeUsercardModerationAction(request);
            }
            return "";
        };
    };

    HotkeyController::HotkeyMap actions{
        {"delete",
         [this](std::vector<QString>) -> QString {
             this->deleteLater();
             return "";
         }},
        {"scrollPage",
         [this](std::vector<QString> arguments) -> QString {
             if (arguments.size() == 0)
             {
                 qCWarning(chatterinoHotkeys)
                     << "scrollPage hotkey called without arguments!";
                 return "scrollPage hotkey called without arguments!";
             }
             auto direction = arguments.at(0);

             auto &scrollbar = this->ui_.latestMessages->getScrollBar();
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
        {"execModeratorAction", makeModeratorHotkey(false)},
        {"execModeratorActionWithReason", makeModeratorHotkey(true)},
        {"pin",
         [this](std::vector<QString> /*arguments*/) -> QString {
             this->togglePinned();
             return "";
         }},
        {"openProfilePictureMenu",
         [this](std::vector<QString> /*arguments*/) -> QString {
             return this->showProfilePictureContextMenu();
         }},

        // these actions make no sense in the context of a usercard, so they aren't implemented
        {"reject", nullptr},
        {"accept", nullptr},
        {"openTab", nullptr},
        {"search", nullptr},
    };

    this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
        HotkeyCategory::PopupWindow, actions, this);
}

UserInfoPopup::UserInfoPopup(bool closeAutomatically, Split *split)
    : DraggablePopup(closeAutomatically, split)
    , split_(split)
    , closeAutomatically_(closeAutomatically)
{
    assert(split != nullptr &&
           "split being nullptr causes lots of bugs down the road");
    this->setWindowTitle("Usercard");

    this->addShortcuts();
    this->signalHolder_.managedConnect(
        getApp()->getHotkeys()->onItemsUpdated, [this] {
            this->clearShortcuts();
            this->addShortcuts();
            if (this->ui_.timeoutWidget != nullptr)
            {
                this->ui_.timeoutWidget->refreshActionTooltips();
            }
        });

    auto layers = LayoutCreator<QWidget>(this->getLayoutContainer())
                      .setLayoutType<QGridLayout>()
                      .withoutMargin();
    auto layout = layers.emplace<QVBoxLayout>();

    // first line
    auto head = layout.emplace<QHBoxLayout>().withoutMargin();
    {
        auto avatarBox = head.emplace<QVBoxLayout>().withoutMargin();
        avatarBox->setAlignment(Qt::AlignTop);
        avatarBox->setSpacing(4);
        // avatar
        auto *avatarFrame = new QWidget(this);
        auto *avatarLayout = new QGridLayout(avatarFrame);
        avatarLayout->setContentsMargins(0, 0, 0, 0);
        avatarLayout->setSpacing(0);
        avatarBox->addWidget(avatarFrame);

        auto *avatar = new PixmapButton(nullptr);
        this->ui_.avatarButton = avatar;
        avatar->setScaleIndependentSize(100, 100);
        avatar->setDim(DimButton::Dim::None);
        avatarLayout->addWidget(avatar, 0, 0);
        QObject::connect(
            avatar, &Button::clicked,
            [this](Qt::MouseButton button) {
                if (this->isTikTok_)
                {
                    if (button == Qt::LeftButton)
                    {
                        this->openPlatformUsercard();
                    }
                    else if (button == Qt::RightButton)
                    {
                        this->showProfilePictureContextMenu();
                    }
                    return;
                }
                if (this->isYouTube_)
                {
                    if (button == Qt::LeftButton)
                    {
                        openYouTubeUrl(
                            QUrl(youtubeChannelUrl(this->userId_)));
                    }
                    else if (button == Qt::RightButton)
                    {
                        this->showProfilePictureContextMenu();
                    }
                    return;
                }
                if (this->isKick_)
                {
                    this->onKickProfilePictureClick(button);
                    return;
                }

                QUrl channelURL("https://www.twitch.tv/" +
                                this->userName_.toLower());

                switch (button)
                {
                    case Qt::LeftButton: {
                        QDesktopServices::openUrl(channelURL);
                    }
                    break;

                    case Qt::RightButton: {
                        this->showProfilePictureContextMenu();
                    }
                    break;

                    default:;
                }
            });
        auto *bannedLabel = new QLabel("BANNED", avatarFrame);
        bannedLabel->setAlignment(Qt::AlignCenter);
        bannedLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        bannedLabel->setStyleSheet(
            "QLabel { background: rgba(185, 28, 28, 220); color: white; "
            "padding: 2px 6px; border-radius: 3px; }");
        bannedLabel->setFont(
            makeResolvedFont(bannedLabel->font(), QFont::Bold));
        bannedLabel->hide();
        avatarLayout->addWidget(bannedLabel, 0, 0, Qt::AlignHCenter | Qt::AlignBottom);
        this->ui_.bannedAvatarLabel = bannedLabel;

        auto *followButton = new QPushButton("Follow", this);
        this->ui_.followButton = followButton;
        followButton->setAutoDefault(false);
        followButton->setDefault(false);
        followButton->setFlat(true);
        followButton->setFocusPolicy(Qt::TabFocus);

        followButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        followButton->setToolTip("Checking follow status...");
        followButton->setEnabled(false);
        followButton->hide();
        this->updateFollowButtonAppearance();
        avatarBox->addWidget(followButton);
        QObject::connect(followButton, &QPushButton::clicked, this, [this] {
            this->runFollowAction();
        });

        auto switchAv =
            avatarBox.emplace<LabelButton>(QString{}, nullptr, QSize{2, 2})
                .assign(&this->ui_.switchAvatars);
        switchAv->hide();
        QObject::connect(
            switchAv.getElement(), &LabelButton::leftClicked, [this] {
                if (getApp()->getStreamerMode()->isEnabled() &&
                    getSettings()->streamerModeHideUsercardAvatars)
                {
                    return;
                }
                if (!this->seventvAvatar_)
                {
                    this->ui_.switchAvatars->hide();
                    return;
                }
                this->isTwitchAvatarShown_ = !this->isTwitchAvatarShown_;
                if (this->isTwitchAvatarShown_)
                {
                    this->seventvAvatar_->stop();
                    this->ui_.avatarButton->setPixmap(this->avatarPixmap_);
                    this->ui_.switchAvatars->setText("Show 7TV");
                }
                else
                {
                    this->ui_.avatarButton->setPixmap(
                        this->seventvAvatar_->currentPixmap());
                    this->seventvAvatar_->start();
                    this->ui_.switchAvatars->setText(u"Show " %
                                                     this->platformName());
                }
                this->updateAvatarUrl();
            });

        auto vbox = head.emplace<QVBoxLayout>();
        vbox->setAlignment(Qt::AlignTop);
        {
            // items on the right
            {
                auto box = vbox.emplace<QHBoxLayout>()
                               .withoutMargin()
                               .withoutSpacing();
                this->ui_.identityHeader = box.getElement();

                this->ui_.nameLabel =
                    addCopyableLabel<PaintTooltipLabel>(box, "Copy name");
                this->ui_.nameLabel->setFontStyle(FontStyle::UiMediumBold);
                this->ui_.nameLabel->setPadding(QMargins(8, 0, 1, 0));
                this->ui_.liveIndicator = new LiveIndicator;
                this->ui_.liveIndicator->hide();
                // addCopyableLabel adds the copy button last -> add the indicator before that
                box->insertWidget(box->count() - 1, this->ui_.liveIndicator);
                box->insertItem(box->count() - 1,
                                ScalingSpacerItem::horizontal(7));
                auto nameHistory =
                    box.emplace<LabelButton>("aka", this, QSize{4, 0})
                        .assign(&this->ui_.nameHistoryButton);
                nameHistory->setToolTip("Show name history");
                nameHistory->hide();
                QObject::connect(nameHistory.getElement(),
                                 &Button::leftClicked, [this] {
                                     this->showNameHistoryMenu();
                                 });
                box->addSpacing(5);
                box->addStretch(1);

                this->ui_.localizedNameIndex = box->count();
                this->ui_.localizedNameLabel =
                    addCopyableLabel(box, "Copy localized name",
                                     &this->ui_.localizedNameCopyButton);
                this->ui_.localizedNameLabel->setFontStyle(
                    FontStyle::UiMediumBold);
                box->addSpacing(5);
                box->addStretch(1);

                auto palette = QPalette();
                palette.setColor(QPalette::WindowText, QColor("#aaa"));
                this->ui_.userIDLabel = addCopyableLabel(box, "Copy ID");
                this->ui_.userIDLabel->setPalette(palette);

                this->ui_.localizedNameLabel->setVisible(false);
                this->ui_.localizedNameCopyButton->setVisible(false);

                auto *userActions = new DrawnButton(
                    DrawnButton::Symbol::Kebab, {}, this);
                userActions->setScaleIndependentSize(24, 24);
                userActions->setToolTip("More actions");
                this->ui_.userActions = userActions;
                box->addWidget(userActions);
                QObject::connect(
                    userActions, &Button::leftMousePress, this,
                    [this, userActions] {
                        userActions->setMenu(this->createUserActionsMenu());
                    });

                // button to pin the window (only if we close automatically)
                if (this->closeAutomatically_)
                {
                    box->addWidget(this->createPinButton());
                }

                QPointer<UserInfoPopup> self(this);
                this->currentUserChangedConnection_ =
                    getApp()->getAccounts()->twitch.currentUserChanged.connect(
                        [self] {
                            runInGuiThread([self] {
                                if (!self)
                                {
                                    return;
                                }

                                if (!self->isKick_ && !self->isYouTube_ &&
                                    !self->isTikTok_ &&
                                    self->underlyingChannel_)
                                {
                                    if (auto *twitchChannel =
                                            dynamic_cast<TwitchChannel *>(
                                                self->underlyingChannel_.get()))
                                    {
                                        twitchChannel->refreshLeadModStatus();
                                    }
                                }

                                if (!self->isKick_ && !self->isYouTube_ &&
                                    !self->isTikTok_ &&
                                    (!self->userName_.isEmpty() ||
                                     !self->userId_.isEmpty()))
                                {
                                    self->followStatusKnown_ = false;
                                    self->followStatusRequestInFlight_ = false;
                                    self->followMutationInFlight_ = false;
                                    self->resetUsercardInfoRows();
                                    self->updateUserData();
                                }
                                self->refreshFollowButton();
                                self->userStateChanged_.invoke();
                                if (self->ui_.commentsView != nullptr)
                                {
                                    self->ui_.commentsView
                                        ->authenticationChanged();
                                }
                            });
                        });
                this->kickCurrentUserChangedConnection_ =
                    getApp()->getAccounts()->kick.currentUserChanged.connect(
                        [self] {
                            runInGuiThread([self] {
                                if (self && self->isKick_)
                                {
                                    self->userStateChanged_.invoke();
                                }
                            });
                        });
                this->youtubeCurrentUserChangedConnection_ =
                    getApp()->getAccounts()->youtube.currentChanged.connect(
                        [self] {
                            runInGuiThread([self] {
                                if (self && self->isYouTube_)
                                {
                                    self->refreshLocalUserActions();
                                    self->userStateChanged_.invoke();
                                }
                            });
                        });
                this->tiktokCurrentUserChangedConnection_ =
                    getApp()->getAccounts()->tiktok.currentChanged.connect(
                        [self] {
                            runInGuiThread([self] {
                                if (self && self->isTikTok_)
                                {
                                    self->refreshLocalUserActions();
                                    self->userStateChanged_.invoke();
                                }
                            });
                        });
            }

            auto *handleRow = new BaseWidget(this);
            auto *handleLayout = new QHBoxLayout(handleRow);
            handleLayout->setContentsMargins(0, 0, 0, 0);
            handleLayout->setSpacing(0);
            handleLayout->addStretch(1);
            handleRow->hide();
            this->ui_.handleRow = handleRow;
            this->ui_.handleLayout = handleLayout;
            vbox->addWidget(handleRow);

            auto *badgeRow = new BaseWidget(this);
            badgeRow->setSizePolicy(QSizePolicy::Expanding,
                                    QSizePolicy::Preferred);
            badgeRow->hide();
            auto *badgeLayout = new QHBoxLayout(badgeRow);
            badgeLayout->setContentsMargins(8, 0, 0, 0);
            badgeLayout->setSpacing(0);
            this->ui_.identityBadgeRow = badgeRow;

            auto *badgeStrip = new UsercardBadgeStrip(badgeRow);
            this->ui_.identityBadges = badgeStrip;
            badgeLayout->addWidget(badgeStrip, 0, Qt::AlignVCenter);
            badgeLayout->addStretch(1);
            vbox->addWidget(badgeRow);

            vbox.emplace<Label>().assign(&this->ui_.bioLabel);
            this->ui_.bioLabel->setWordWrap(true);
            this->ui_.bioLabel->setPadding({8, 0, 8, 0});
            this->ui_.bioLabel->hide();

            auto *paintButton = new UsercardPaintButton(this);
            paintButton->hide();
            this->ui_.identityPaintRow = paintButton;
            this->ui_.identityPaint = paintButton;

            QObject::connect(
                paintButton, &Button::leftClicked, this, [paintButton] {
                    const auto &paint = paintButton->paint();
                    if (!paint || paint->id.isEmpty())
                    {
                        return;
                    }

                    const auto encodedID = QString::fromLatin1(
                        QUrl::toPercentEncoding(paint->id));
                    QDesktopServices::openUrl(QUrl(
                        QStringLiteral("https://7database.com/paint/") +
                        encodedID));
                });

            // items on the left
            if (getSettings()->showPronouns)
            {
                vbox.emplace<Label>(TEXT_PRONOUNS.arg(TEXT_LOADING))
                    .assign(&this->ui_.pronounsLabel);
            }
            vbox.emplace<Label>(TEXT_FOLLOWERS.arg(""))
                .assign(&this->ui_.followerCountLabel);
            vbox.emplace<Label>(TEXT_CREATED.arg(""))
                .assign(&this->ui_.createdDateLabel);
            vbox.emplace<Label>("").assign(&this->ui_.lastLiveLabel);
            vbox->addWidget(paintButton);
            createUsercardColorRow(vbox, &this->ui_.userColorRow,
                                   &this->ui_.userColorSwatch,
                                   &this->ui_.userColorLabel);
            if (auto *colorRow =
                    dynamic_cast<ClickableColorRow *>(this->ui_.userColorRow))
            {
                QObject::connect(colorRow, &Button::leftClicked, this, [this] {
                    const auto color =
                        this->ui_.userColorRow->property("copy-color")
                            .toString();
                    if (color.isEmpty())
                    {
                        return;
                    }

                    crossPlatformCopy(color);
                    const auto message =
                        QString("Copied user color %1").arg(color);
                    QToolTip::showText(QCursor::pos(), message, this);
                    if (this->channel_)
                    {
                        this->channel_->addSystemMessage(message);
                    }
                });
            }
            vbox.emplace<Label>("").assign(&this->ui_.statusLabel);
            vbox.emplace<Label>("").assign(&this->ui_.chatterCountLabel);
            createUsercardStatusRow(vbox, &this->ui_.followageRow,
                                    &this->ui_.followageIcon,
                                    &this->ui_.followageLabel);
            this->ui_.followageRow->hide();
            createUsercardStatusRow(vbox, &this->ui_.subageRow,
                                    &this->ui_.subageIcon,
                                    &this->ui_.subageLabel);
            this->ui_.subageRow->hide();
        }
    }

    layout.emplace<Line>(false);

    // second line
    auto user = layout.emplace<QHBoxLayout>().withoutMargin();
    {
        user->addStretch(1);
        auto usercard = user.emplace<LabelButton>("Usercard", this)
                            .assign(&this->ui_.usercardLabel);
        auto comments = user.emplace<LabelButton>("Comments", this)
                            .assign(&this->ui_.commentsLabel);
        comments->hide();
        auto userlogs = user.emplace<LabelButton>("Logs view", this)
                            .assign(&this->ui_.userlogsLabel);
        userlogs->hide();
        auto sevenTVUser = user.emplace<LabelButton>("7TV", this)
                               .assign(&this->ui_.sevenTVUserLabel);
        sevenTVUser->setToolTip("Checking 7TV profile...");
        sevenTVUser->setEnabled(false);
        sevenTVUser->hide();
        auto roles = user.emplace<LabelButton>("Roles", this)
                         .assign(&this->ui_.rolesLabel);
        roles->setToolTip("View roles and channels");
        roles->hide();
        auto notesAction = user.emplace<LabelButton>("Notes", this)
                               .assign(&this->ui_.notesActionLabel);
        notesAction->hide();
        auto blockAction = user.emplace<LabelButton>("Block", this)
                               .assign(&this->ui_.blockActionLabel);
        blockAction->hide();
        auto hideAction = user.emplace<LabelButton>("Hide", this)
                              .assign(&this->ui_.hideActionLabel);
        hideAction->hide();
        auto ignoreHighlightsAction =
            user.emplace<LabelButton>("Ignore", this)
                .assign(&this->ui_.ignoreHighlightsActionLabel);
        ignoreHighlightsAction->hide();
        auto crossBanAction = user.emplace<LabelButton>("Cross ban", this)
                                  .assign(&this->ui_.crossBanActionLabel);
        crossBanAction->hide();
        auto crossUnbanAction = user.emplace<LabelButton>("Cross unban", this)
                                    .assign(&this->ui_.crossUnbanActionLabel);
        crossUnbanAction->hide();
        auto mod = user.emplace<PixmapButton>(this);
        mod->setPixmap(getResources().buttons.mod);
        mod->setScaleIndependentSize(30, 30);
        auto unmod = user.emplace<PixmapButton>(this);
        unmod->setPixmap(getResources().buttons.unmod);
        unmod->setScaleIndependentSize(30, 30);
        auto vip = user.emplace<PixmapButton>(this);
        vip->setPixmap(getResources().buttons.vip);
        vip->setScaleIndependentSize(30, 30);
        auto unvip = user.emplace<PixmapButton>(this);
        unvip->setPixmap(getResources().buttons.unvip);
        unvip->setScaleIndependentSize(30, 30);

        user->addStretch(1);

        QObject::connect(usercard.getElement(), &Button::leftClicked,
                         this, [this] { this->openPlatformUsercard(); });
        this->registerMnemonicButton(this->ui_.usercardLabel, Qt::Key_U,
                                     [this] { this->openPlatformUsercard(); });

        QObject::connect(comments.getElement(), &Button::leftClicked, this,
                         [this] { this->toggleModeratorComments(); });

        auto openLogs = [this] { this->toggleUserLogs(); };
        QObject::connect(userlogs.getElement(), &Button::leftClicked, openLogs);
        this->registerMnemonicButton(this->ui_.userlogsLabel, Qt::Key_L,
                                     openLogs);

        auto openSevenTVUser = [this] { this->openSevenTVUser(); };
        QObject::connect(sevenTVUser.getElement(), &Button::leftClicked,
                         openSevenTVUser);
        this->registerMnemonicButton(this->ui_.sevenTVUserLabel, Qt::Key_V,
                                     openSevenTVUser);

        auto openRoles = [this] { this->toggleUserRoles(); };
        QObject::connect(roles.getElement(), &Button::leftClicked, openRoles);
        this->registerMnemonicButton(this->ui_.rolesLabel, Qt::Key_R,
                                     openRoles);

        QObject::connect(notesAction.getElement(), &Button::leftClicked, this,
                         [this] { this->openUserNotes(); });
        QObject::connect(blockAction.getElement(), &Button::leftClicked, this,
                         [this] {
                             this->setTargetBlocked(!this->targetBlocked_);
                         });
        QObject::connect(hideAction.getElement(), &Button::leftClicked, this,
                         [this] {
                             this->setTargetLocallyHidden(
                                 !this->targetLocallyHidden_);
                         });
        QObject::connect(ignoreHighlightsAction.getElement(),
                         &Button::leftClicked, this, [this] {
                             this->setTargetIgnoringHighlights(
                                 !this->targetIgnoringHighlights_);
                         });
        QObject::connect(crossBanAction.getElement(), &Button::leftClicked,
                         this, [this] {
                             this->runCrossAction(QStringLiteral("/crossban"));
                         });
        QObject::connect(crossUnbanAction.getElement(), &Button::leftClicked,
                         this, [this] {
                             this->runCrossAction(
                                 QStringLiteral("/crossunban"));
                         });

        QObject::connect(mod.getElement(), &Button::leftClicked, [this] {
            QString value = "/mod " + this->userName_;
            value = getApp()->getCommands()->execCommand(
                value, this->underlyingChannel_, false);
            this->underlyingChannel_->sendMessage(value);
        });
        QObject::connect(unmod.getElement(), &Button::leftClicked, [this] {
            QString value = "/unmod " + this->userName_;
            value = getApp()->getCommands()->execCommand(
                value, this->underlyingChannel_, false);
            this->underlyingChannel_->sendMessage(value);
        });
        QObject::connect(vip.getElement(), &Button::leftClicked, [this] {
            QString value = "/vip " + this->userName_;
            value = getApp()->getCommands()->execCommand(
                value, this->underlyingChannel_, false);
            this->underlyingChannel_->sendMessage(value);
        });
        QObject::connect(unvip.getElement(), &Button::leftClicked, [this] {
            QString value = "/unvip " + this->userName_;
            value = getApp()->getCommands()->execCommand(
                value, this->underlyingChannel_, false);
            this->underlyingChannel_->sendMessage(value);
        });

        // userstate
        // We can safely ignore this signal connection since this is a private signal, and
        // we only connect once
        std::ignore = this->userStateChanged_.connect([this, mod, unmod, vip,
                                                       unvip]() mutable {
            TwitchChannel *twitchChannel =
                dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get());

            bool visibilityModButtons = false;

            if (!this->isYouTube_ && !this->isTikTok_ && twitchChannel)
            {
                bool isMyself =
                    QString::compare(getApp()
                                         ->getAccounts()
                                         ->twitch.getCurrent()
                                         ->getUserName(),
                                     this->userName_, Qt::CaseInsensitive) == 0;

                const bool canManageRoles =
                    twitchChannel->isBroadcaster() ||
                    (getSettings()->showLeadModRoleButtons &&
                     twitchChannel->isLeadMod());

                visibilityModButtons =
                    canManageRoles && !isMyself && !this->isBroadcaster_;
            }
            mod->setVisible(visibilityModButtons);
            unmod->setVisible(visibilityModButtons);
            vip->setVisible(visibilityModButtons);
            unvip->setVisible(visibilityModButtons);
            this->updateUserRolesContext();
            this->refreshUsercardActionPlacements();
        });
    }

    this->customActions_ = layout.emplace<BaseWidget>(this).getElement();
    new FlowLayout(this->customActions_, {0, 4, 2});
    this->customActions_->hide();
    this->signalHolder_.managedConnect(
        getApp()->getCommands()->items.delayedItemsChanged, [this] {
            this->refreshCustomActions();
        });

    auto notesPreview = layout.emplace<MarkdownLabel>(this, QString())
                            .assign(&this->ui_.notesPreview);
    notesPreview->setVisible(false);
    notesPreview->setShouldElide(true);

    auto lineMod = layout.emplace<Line>(false);

    // third line
    auto moderation = layout.emplace<QHBoxLayout>().withoutMargin();
    {
        auto timeout = moderation.emplace<TimeoutWidget>().assign(
            &this->ui_.timeoutWidget);

        // We can safely ignore this signal connection since this is a private signal, and
        // we only connect once
        std::ignore = this->userStateChanged_.connect([this, lineMod,
                                                       timeout]() mutable {
            bool visible = this->shouldShowModerationActions();
            lineMod->setVisible(visible);
            timeout->setVisible(visible);
        });

        // We can safely ignore this signal connection since we own the button, and
        // the button will always be destroyed before the UserInfoPopup
        std::ignore = timeout->buttonClicked.connect(
            [this](const UsercardModerationRequest &request) {
                if (request.promptForReason && !this->isYouTube_ &&
                    !this->isTikTok_)
                {
                    this->showUsercardModerationReasonPopup(request);
                    return;
                }

                this->executeUsercardModerationAction(request);
            });
    }

    layout.emplace<Line>(false);

    auto *activityStack = new QStackedWidget(this);
    activityStack->setMinimumWidth(430);
    this->ui_.activityStack = activityStack;
    auto *messagesPage = new QWidget(activityStack);
    this->ui_.messagesPage = messagesPage;
    auto *logs = new QVBoxLayout(messagesPage);
    logs->setContentsMargins(0, 0, 0, 0);
    logs->setSpacing(0);
    {
        this->ui_.noMessagesLabel = new Label("No recent messages");
        this->ui_.noMessagesLabel->setVisible(false);
        this->ui_.noMessagesLabel->setSizePolicy(QSizePolicy::Expanding,
                                                 QSizePolicy::Expanding);

        this->ui_.latestMessages =
            new ChannelView(this, this->split_, ChannelView::Context::UserCard,
                            sanitizeScrollbackLimit(
                                getSettings()->scrollbackUsercardLimit.getValue()));
        this->ui_.latestMessages->setMinimumSize(430, 275);
        this->ui_.latestMessages->setSizePolicy(QSizePolicy::Expanding,
                                                QSizePolicy::Expanding);

        auto loadMore =
            new LabelButton("Load more messages", this, QSize{8, 2});
        loadMore->setVisible(false);
        loadMore->setToolTip("Load older messages from Twitch mod logs");
        this->ui_.loadMoreMessages = loadMore;

        QObject::connect(loadMore, &Button::leftClicked, this,
                         [this] { this->requestMoreUsercardMessages(true); });
        this->usercardScrollConnection_ =
            std::make_unique<pajlada::Signals::ScopedConnection>(
                this->ui_.latestMessages->getScrollBar()
                    .getDesiredValueChanged()
                    .connect([this] {
                        this->maybeLoadMoreUsercardMessagesFromScroll();
                    }));

        logs->addWidget(this->ui_.loadMoreMessages);
        logs->addWidget(this->ui_.noMessagesLabel);
        logs->addWidget(this->ui_.latestMessages);
        logs->setAlignment(this->ui_.noMessagesLabel, Qt::AlignHCenter);
        logs->setAlignment(this->ui_.loadMoreMessages, Qt::AlignHCenter);
    }
    activityStack->addWidget(messagesPage);
    this->ui_.commentsView = new ModeratorCommentsView(activityStack);
    this->ui_.commentsView->refreshStyle(this->scale());
    activityStack->addWidget(this->ui_.commentsView);
    this->ui_.logsView = new UserLogsView(activityStack);
    this->ui_.logsView->refreshStyle(this->scale());
    activityStack->addWidget(this->ui_.logsView);
    this->ui_.rolesView = new UserRolesView(activityStack);
    this->ui_.rolesView->setRoleManagementCallback(
        [this](QWidget *anchor) { this->showRoleManagementMenu(anchor); });
    this->ui_.rolesView->refreshStyle(this->scale());
    activityStack->addWidget(this->ui_.rolesView);
    activityStack->setCurrentWidget(messagesPage);
    layout->addWidget(activityStack);

    this->enableResize(getSettings()->usercardPopupSize, {}, false,
                       ResizeMode::Always);

    this->installEvents();
    this->updateUsercardStatusIcons();
    std::ignore = this->userStateChanged_.connect(
        [this] { this->updateLoadMoreMessagesButton(); });
    std::ignore = this->userStateChanged_.connect(
        [this] { this->updateModeratorCommentsAvailability(); });

    this->signalHolder_.managedConnect(
        getApp()->getSeventvPaints()->paintChanged,
        [this](const QString &login, bool isKick) {
            if (isKick == this->isKick_ &&
                login.compare(this->userName_, Qt::CaseInsensitive) == 0)
            {
                this->refreshIdentityPaint();
            }
        });
    this->signalHolder_.managedConnect(
        getApp()->getWindows()->wordFlagsChanged,
        [this] { this->refreshIdentityBadges(); });
    if (auto *provider = getApp()->getFfzApBadges())
    {
        this->signalHolder_.managedConnect(
            provider->badgesUpdated,
            [this] { this->refreshIdentityBadges(); });
    }
    if (auto *provider = getApp()->getBluzyrinoBadges())
    {
        this->signalHolder_.managedConnect(
            provider->badgesUpdated,
            [this] { this->refreshIdentityBadges(); });
    }
    if (auto *provider = getApp()->getJilChatBadges())
    {
        this->signalHolder_.managedConnect(
            provider->badgesUpdated,
            [this] { this->refreshIdentityBadges(); });
    }
    if (auto *provider = getApp()->getMoltorinoSupporterBadges())
    {
        this->signalHolder_.managedConnect(
            provider->badgesUpdated,
            [this] { this->refreshIdentityBadges(); });
    }
    this->signalHolder_.managedConnect(
        getApp()->getWindows()->layoutRequested, [this](Channel *channel) {
            if (!this->isVisible() ||
                (channel != nullptr && this->underlyingChannel_ &&
                 channel != this->underlyingChannel_.get()))
            {
                return;
            }
            this->ui_.identityBadges->updateGeometry();
            this->ui_.identityBadges->update();
            static_cast<UsercardPaintButton *>(this->ui_.identityPaint)
                ->refreshAfterImageLayout();
        });
    this->signalHolder_.managedConnect(
        getApp()->getWindows()->gifRepaintRequested, [this] {
            if (!this->isVisible())
            {
                return;
            }

            auto *badges = static_cast<UsercardBadgeStrip *>(
                this->ui_.identityBadges);
            if (badges->hasAnimatedBadges())
            {
                badges->update();
            }

            auto *paint = static_cast<UsercardPaintButton *>(
                this->ui_.identityPaint);
            paint->refreshAnimationFrame();
        });
    this->signalHolder_.managedConnect(
        getApp()->getFonts()->fontChanged, [this] {
            auto *paint = static_cast<UsercardPaintButton *>(
                this->ui_.identityPaint);
            paint->setFont(getApp()->getFonts()->getFont(
                FontStyle::UiMedium, this->scale()));
            paint->refreshAppearance();
        });
    getSettings()->displaySevenTVPaints.connect(
        [this](bool) {
            this->refreshIdentityBadges();
            this->refreshIdentityPaint();
        },
        this->signalHolder_);
    getSettings()->showUsercardBadges.connect(
        [this](bool) { this->refreshIdentityBadges(); }, this->signalHolder_);
    getSettings()->localVanityLayouts.connect(
        [this](const auto &) {
            this->refreshIdentityBadges();
        },
        this->signalHolder_, false);
    getSettings()->showUsercardSevenTVPaint.connect(
        [this](bool) {
            this->refreshIdentityBadges();
            this->refreshIdentityPaint();
        },
        this->signalHolder_);
    getSettings()->displaySevenTVPaintShadows.connect(
        [this](bool) {
            static_cast<UsercardPaintButton *>(this->ui_.identityPaint)
                ->refreshAppearance();
        },
        this->signalHolder_);
    getSettings()->largeSevenTVPaintShadows.connect(
        [this](bool) {
            static_cast<UsercardPaintButton *>(this->ui_.identityPaint)
                ->refreshAppearance();
        },
        this->signalHolder_);

    static_cast<UsercardPaintButton *>(this->ui_.identityPaint)
        ->setFont(getApp()->getFonts()->getFont(FontStyle::UiMedium,
                                                this->scale()));
    this->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Policy::Ignored);
}

static void openYouTubeUrl(const QUrl &url)
{
    if (!url.isValid() || url.isEmpty())
    {
        return;
    }
    if (getSettings()->openLinksIncognito && supportsIncognitoLinks())
    {
        openLinkIncognito(url.toString());
        return;
    }
    QDesktopServices::openUrl(url);
}

void UserInfoPopup::themeChangedEvent()
{
    BaseWindow::themeChangedEvent();

    for (auto &&child : this->findChildren<QCheckBox *>())
    {
        child->setFont(
            getApp()->getFonts()->getFont(FontStyle::UiMedium, this->scale()));
    }

    this->updateUsercardStatusIcons();
    this->updateFollowButtonAppearance();

    this->ui_.identityBadges->update();
    this->ui_.identityPaint->update();
    this->ui_.commentsView->refreshStyle(this->scale());
    this->ui_.logsView->refreshStyle(this->scale());
    this->ui_.rolesView->refreshStyle(this->scale());
}

void UserInfoPopup::scaleChangedEvent(float scale)
{
    DraggablePopup::scaleChangedEvent(scale);
    this->themeChangedEvent();

    auto *paint =
        static_cast<UsercardPaintButton *>(this->ui_.identityPaint);
    paint->setFont(getApp()->getFonts()->getFont(FontStyle::UiMedium,
                                                 this->scale()));
    paint->updateGeometry();

    QTimer::singleShot(20, this, [this] {
        this->applyPopupSize(this->minimumSizeHint());
    });
}

void UserInfoPopup::windowDeactivationEvent()
{
    const auto menus = this->findChildren<QMenu *>();
    const bool hasVisibleMenu =
        std::ranges::any_of(menus, [](const QMenu *menu) {
            return menu->isVisible();
        });
    const bool hasVisibleNotesDialog =
        !this->editUserNotesDialog_.isNull() &&
        this->editUserNotesDialog_->isVisible();
    const auto dialogs = this->findChildren<QDialog *>();
    const bool hasVisibleModalDialog =
        std::ranges::any_of(dialogs, [](const QDialog *dialog) {
            return dialog->isVisible() && dialog->isModal();
        });

    if (!hasVisibleNotesDialog && !hasVisibleMenu && !hasVisibleModalDialog)
    {
        BaseWindow::windowDeactivationEvent();
    }
}

void UserInfoPopup::registerMnemonicButton(LabelButton *button, int key,
                                           std::function<void()> action)
{
    if (button == nullptr)
    {
        return;
    }

    this->mnemonicActions_[key] = {
        std::move(action),
        [button] {
            return button->isVisible() && button->isEnabled();
        },
    };
}

void UserInfoPopup::keyPressEvent(QKeyEvent *event)
{
    const auto modifiers = event->modifiers() & ~Qt::KeypadModifier;
    if (modifiers == Qt::NoModifier || modifiers == Qt::AltModifier)
    {
        auto it = this->mnemonicActions_.find(event->key());
        if (it != this->mnemonicActions_.end())
        {
            const auto &[action, canRun] = it->second;
            if (!canRun || canRun())
            {
                action();
                event->accept();
                return;
            }
        }
    }

    DraggablePopup::keyPressEvent(event);
}

void UserInfoPopup::installEvents()
{
    this->mnemonicActions_[Qt::Key_N] = {
        [this] { this->openUserNotes(); },
        [this] { return this->canEditTargetNotes_; },
    };

    this->userDataUpdatedConnection_ =
        std::make_unique<pajlada::Signals::ScopedConnection>(
            getApp()->getUserData()->userDataUpdated().connect([this]() {
                this->updateNotes();
            }));
    QObject::connect(getApp()->getStreamerMode(), &IStreamerMode::changed, this,
                     [this]() {
                         this->updateNotes();
                         this->refreshAvatarVisibility();
                     });
    getSettings()->streamerModeHideUsercardAvatars.connect(
        [this](bool) { this->refreshAvatarVisibility(); },
        this->signalHolder_, false);

    getSettings()->hideModActionsOnModUsercards.connect(
        [this](bool enabled) {
            if (enabled &&
                getSettings()->showModActionsOnModUsercardsAsLeadMod)
            {
                if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(
                        this->underlyingChannel_.get()))
                {
                    twitchChannel->refreshLeadModStatus();
                }
            }
            this->userStateChanged_.invoke();
        },
        this->signalHolder_);
    getSettings()->showModActionsOnModUsercardsAsLeadMod.connect(
        [this](bool enabled) {
            if (enabled && getSettings()->hideModActionsOnModUsercards)
            {
                if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(
                        this->underlyingChannel_.get()))
                {
                    twitchChannel->refreshLeadModStatus();
                }
            }

            this->userStateChanged_.invoke();
        },
        this->signalHolder_);
    getSettings()->showLeadModRoleButtons.connect(
        [this](bool enabled) {
            if (enabled)
            {
                if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(
                        this->underlyingChannel_.get()))
                {
                    twitchChannel->refreshLeadModStatus(true);
                }
            }

            this->userStateChanged_.invoke();
        },
        this->signalHolder_);
    getSettings()->showUsercardRoleManagementMenu.connect(
        [this](bool) {
            this->userStateChanged_.invoke();
        },
        this->signalHolder_);
    getSettings()->showSevenTVUsercardButton.connect(
        [this](bool enabled) {
            if (this->isYouTube_ || this->isTikTok_)
            {
                this->refreshSevenTVUserButtonVisibility();
                return;
            }
            if (enabled && this->seventvUserID_.isEmpty() &&
                !this->seventvUserLookupInFlight_ &&
                !this->seventvUserLookupFinished_ &&
                !this->userId_.isEmpty())
            {
                auto userID = this->userId_;
                const QStringView kickPrefix = u"kick:";
                if (this->isKick_ && userID.startsWith(kickPrefix))
                {
                    userID = userID.mid(kickPrefix.size());
                }
                this->loadSevenTVAvatar(userID, this->isKick_, false);
                return;
            }

            this->refreshSevenTVUserButtonVisibility();
        },
        this->signalHolder_);
    getSettings()->showUsercardNameHistoryButton.connect(
        [this](bool) {
            this->updateNameHistoryButton();
        },
        this->signalHolder_);
    getSettings()->showUsercardLoadMoreMessagesButton.connect(
        [this](bool) {
            this->updateLoadMoreMessagesButton();
        },
        this->signalHolder_);
    getSettings()->showModeratorCommentsButton.connect(
        [this](bool) {
            this->updateModeratorCommentsAvailability();
        },
        this->signalHolder_);
    getSettings()->usercardUsercardActionPlacement.connect(
        [this](const QString &) {
            this->refreshUsercardActionPlacements();
        },
        this->signalHolder_);
    getSettings()->usercardCommentsActionPlacement.connect(
        [this](const QString &) {
            this->updateModeratorCommentsAvailability();
        },
        this->signalHolder_);
    getSettings()->usercardLogsActionPlacement.connect(
        [this](const QString &) {
            this->updateUserLogsContext();
        },
        this->signalHolder_);
    getSettings()->usercardRolesActionPlacement.connect(
        [this](const QString &) {
            this->updateUserRolesContext();
        },
        this->signalHolder_);
    getSettings()->usercardSevenTVActionPlacement.connect(
        [this](const QString &) {
            this->refreshSevenTVUserButtonVisibility();
        },
        this->signalHolder_);
    auto refreshActionPlacements = [this](const QString &) {
        this->refreshUsercardActionPlacements();
    };
    getSettings()->usercardNotesActionPlacement.connect(
        refreshActionPlacements, this->signalHolder_);
    getSettings()->usercardBlockActionPlacement.connect(
        refreshActionPlacements, this->signalHolder_);
    getSettings()->usercardHideActionPlacement.connect(
        refreshActionPlacements, this->signalHolder_);
    getSettings()->usercardIgnoreHighlightsActionPlacement.connect(
        refreshActionPlacements, this->signalHolder_);
    getSettings()->usercardCrossBanActionPlacement.connect(
        refreshActionPlacements, this->signalHolder_);
    getSettings()->usercardCrossUnbanActionPlacement.connect(
        refreshActionPlacements, this->signalHolder_);
    getSettings()->showCrossActionsInUnmoderatedChannels.connect(
        [this](bool) { this->refreshUsercardActionPlacements(); },
        this->signalHolder_);
    getSettings()->showFollowButtonInUsercard.connect(
        [this](bool) {
            this->refreshFollowButton();
        },
        this->signalHolder_);
    getSettings()->alwaysLoadMoreUsercardMessages.connect(
        [this](bool enabled) {
            if (!enabled)
            {
                this->usercardMessagesLazyLoadEnabled_ = false;
                this->updateLoadMoreMessagesButton();
                return;
            }

            this->maybeStartUsercardMessageAutoLoad();
        },
        this->signalHolder_);
    getSettings()->moltorinoAuthAccounts.connect(
        [this](const QString &, auto) {
            this->loadTwitchFollowage();
            this->refreshFollowButton();
            this->ui_.commentsView->authenticationChanged();
            this->userStateChanged_.invoke();
            this->updateLoadMoreMessagesButton();
            this->maybeStartUsercardMessageAutoLoad();
        },
        this->signalHolder_);
}

void UserInfoPopup::refreshTargetModerationStatus()
{
    if (this->userName_.isEmpty() || !this->underlyingChannel_ ||
        !this->underlyingChannel_->isTwitchChannel())
    {
        return;
    }

    this->targetModerationTime_ = {};
    this->isMod_ = false;
    if (const auto *channel =
            dynamic_cast<const TwitchChannel *>(this->underlyingChannel_.get()))
    {
        if (const auto status = channel->knownModeratorStatus(this->userName_))
        {
            this->isMod_ = status->moderator;
            this->targetModerationTime_ = status->changedAt;
        }
    }
    this->isBroadcaster_ =
        this->userName_.compare(this->underlyingChannel_->getName(),
                                Qt::CaseInsensitive) == 0;

    for (const auto &message : this->underlyingChannel_->getMessageSnapshot())
    {
        this->updateTargetModerationStatusFromMessage(message);
    }
}

bool UserInfoPopup::updateTargetModerationStatusFromMessage(
    const MessagePtr &message)
{
    if (message == nullptr || this->userName_.isEmpty() ||
        !this->underlyingChannel_ ||
        !this->underlyingChannel_->isTwitchChannel() ||
        message->platform != MessagePlatform::AnyOrTwitch ||
        message->flags.has(MessageFlag::System) ||
        message->flags.has(MessageFlag::ModerationAction) ||
        message->flags.has(MessageFlag::SharedMessage))
    {
        return false;
    }

    if (message->loginName.compare(this->userName_, Qt::CaseInsensitive) != 0)
    {
        return false;
    }

    if (this->targetModerationTime_.isValid() &&
        (!message->serverReceivedTime.isValid() ||
         message->serverReceivedTime <= this->targetModerationTime_))
    {
        return false;
    }
    this->targetModerationTime_ = message->serverReceivedTime;
    const bool moderator = messageHasTwitchBadge(*message, u"moderator") ||
                           messageHasTwitchBadge(*message, u"lead_moderator");
    bool changed = this->isMod_ != moderator;
    this->isMod_ = moderator;
    if (!this->isBroadcaster_ &&
        (messageHasTwitchBadge(*message, u"broadcaster") ||
         (this->underlyingChannel_ &&
          this->userName_.compare(this->underlyingChannel_->getName(),
                                  Qt::CaseInsensitive) == 0)))
    {
        this->isBroadcaster_ = true;
        changed = true;
    }

    return changed;
}

bool UserInfoPopup::shouldShowModerationActions() const
{
    if (this->isTikTok_ || this->userName_.isEmpty() ||
        !this->underlyingChannel_)
    {
        return false;
    }

    if (auto *youtubeChannel =
            dynamic_cast<YouTubeChannel *>(this->underlyingChannel_.get()))
    {
        return youtubeChannel->canModerateTarget(YouTubeAuthor{
            .channelId = this->userId_,
            .isOwner = this->youtubeTargetIsOwner_,
            .isModerator = this->youtubeTargetIsModerator_,
            .roleMetadataKnown = true,
        });
    }

    if (auto *twitchChannel =
            dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get()))
    {
        const bool isMyself =
            getApp()
                ->getAccounts()
                ->twitch.getCurrent()
                ->getUserName()
                .compare(this->userName_, Qt::CaseInsensitive) == 0;
        if (isMyself || !twitchChannel->hasModRights())
        {
            return false;
        }
        if (twitchChannel->isBroadcaster())
        {
            return true;
        }

        if (!getSettings()->hideModActionsOnModUsercards)
        {
            return true;
        }

        if (!this->isMod_ && !this->isBroadcaster_)
        {
            return true;
        }

        return getSettings()->showModActionsOnModUsercardsAsLeadMod &&
               twitchChannel->isLeadMod() && this->isMod_ &&
               !this->isBroadcaster_;
    }

    if (auto *kickChannel =
            dynamic_cast<KickChannel *>(this->underlyingChannel_.get()))
    {
        const bool isMyself =
            getApp()->getAccounts()->kick.current()->username().compare(
                this->userName_, Qt::CaseInsensitive) == 0;
        return kickChannel->hasModRights() && !isMyself;
    }

    return false;
}

void UserInfoPopup::setData(const QString &name, const ChannelPtr &channel)
{
    this->setData(name, channel, channel);
}

void UserInfoPopup::clearSevenTVAvatar()
{
    if (this->seventvAvatar_)
    {
        this->seventvAvatar_->stop();
        delete this->seventvAvatar_;
        this->seventvAvatar_ = nullptr;
    }
    this->seventvAvatarUrl_.clear();
    this->isTwitchAvatarShown_ = true;
    this->ui_.switchAvatars->hide();
}

void UserInfoPopup::refreshAvatarVisibility()
{
    const bool hidden = getApp()->getStreamerMode()->isEnabled() &&
                        getSettings()->streamerModeHideUsercardAvatars;
    this->ui_.switchAvatars->setVisible(!hidden && this->seventvAvatar_);
    if (hidden)
    {
        if (this->seventvAvatar_)
        {
            this->seventvAvatar_->stop();
        }
        this->ui_.avatarButton->setPixmap(getResources().streamerMode);
    }
    else if (this->seventvAvatar_ && !this->isTwitchAvatarShown_)
    {
        this->ui_.avatarButton->setPixmap(this->seventvAvatar_->currentPixmap());
        this->seventvAvatar_->start();
    }
    else if (!this->avatarPixmap_.isNull())
    {
        this->ui_.avatarButton->setPixmap(this->avatarPixmap_);
    }
    else if (!this->helixAvatarUrl_.isEmpty())
    {
        auto id = this->userId_;
        if (this->isKick_ && id.startsWith(u"kick:"))
        {
            id.remove(0, 5);
        }
        this->loadAvatar(id, this->helixAvatarUrl_, this->isKick_,
                         !this->isYouTube_ && !this->isTikTok_);
    }
    else
    {
        this->ui_.avatarButton->setPixmap({});
    }
}

void UserInfoPopup::resetTargetState()
{
    ++this->userDataRequestGeneration_;
    ++this->seventvUserRequestGeneration_;
    this->twitchUserStateConnection_.reset();
    this->twitchTargetRoleConnection_.reset();
    this->twitchRoomIdConnection_.reset();
    this->youtubeModerationStateConnection_.reset();
    this->resetNameHistory();
    this->platformRoles_.clear();
    this->identityMessageFallback_.reset();
    this->resetUsercardMessageLoader();
    this->clearSevenTVAvatar();
    this->seventvUserID_.clear();
    this->seventvUserLookupInFlight_ = false;
    this->seventvUserLookupFinished_ = false;
    this->avatarUrl_.clear();
    this->helixAvatarUrl_.clear();
    this->avatarPixmap_ = {};
    this->ui_.avatarButton->setPixmap({});
    this->ui_.liveIndicator->hide();
    this->ui_.localizedNameLabel->hide();
    this->ui_.localizedNameCopyButton->hide();
    this->updateIdentityLayout();
    this->ui_.bioLabel->setText({});
    this->ui_.bioLabel->hide();
    this->ui_.notesPreview->hide();
    this->ui_.timeoutWidget->setUnbanEnabled(true);
    if (this->ui_.pronounsLabel)
    {
        this->ui_.pronounsLabel->setText(TEXT_PRONOUNS.arg(TEXT_LOADING));
        this->ui_.pronounsLabel->setVisible(
            !this->isYouTube_ && !this->isTikTok_ && !this->isKick_ &&
            getSettings()->showPronouns);
    }
    this->ui_.usercardLabel->setText("Usercard");
    this->ui_.usercardLabel->setToolTip({});
    for (auto *menu : this->findChildren<QMenu *>())
    {
        menu->close();
    }
    if (this->moderationReasonPopup_)
    {
        this->moderationReasonPopup_->close();
    }
}

void UserInfoPopup::updateIdentityLayout()
{
    if (this->isTikTok_)
    {
        if (this->ui_.handleLayout->indexOf(this->ui_.localizedNameLabel) < 0)
        {
            this->ui_.handleLayout->insertWidget(0, this->ui_.localizedNameLabel);
            this->ui_.handleLayout->insertWidget(1, this->ui_.localizedNameCopyButton);
        }
        this->ui_.localizedNameLabel->setFontStyle(FontStyle::UiMedium);
        this->ui_.localizedNameCopyButton->setToolTip("Copy handle");
    }
    else
    {
        if (this->ui_.identityHeader->indexOf(this->ui_.localizedNameLabel) < 0)
        {
            this->ui_.identityHeader->insertWidget(this->ui_.localizedNameIndex,
                                                   this->ui_.localizedNameLabel);
            this->ui_.identityHeader->insertWidget(this->ui_.localizedNameIndex + 1,
                                                   this->ui_.localizedNameCopyButton);
        }
        this->ui_.localizedNameLabel->setFontStyle(FontStyle::UiMediumBold);
        this->ui_.localizedNameCopyButton->setToolTip("Copy localized name");
    }
    this->ui_.handleRow->hide();
}

std::function<void()> UserInfoPopup::actionForCurrentTarget(
    std::function<void()> action)
{
    return [self = QPointer<UserInfoPopup>(this),
            generation = this->userDataRequestGeneration_,
            action = std::move(action)] {
        if (self && generation == self->userDataRequestGeneration_)
        {
            action();
        }
    };
}

void UserInfoPopup::setData(const QString &name,
                            const ChannelPtr &contextChannel,
                            const ChannelPtr &openingChannel)
{
    this->showActivityPage(ActivityPage::Messages);
    this->isYouTube_ = false;
    this->isTikTok_ = false;
    this->ui_.timeoutWidget->setReasonPromptsEnabled(true);
    this->ui_.timeoutWidget->setMinTimeout(0);
    this->platformHandle_.clear();
    this->youtubeTargetIsOwner_ = false;
    this->youtubeTargetIsModerator_ = false;
    const QStringView idPrefix = u"id:";
    bool isId = name.startsWith(idPrefix);
    if (isId)
    {
        this->userId_ = name.mid(idPrefix.size());
        this->updateNotes();
        this->userName_ = "";
    }
    else
    {
        this->userId_.clear();
        this->userName_ = name;
        this->kickUserSlug_ = name;
    }

    this->channel_ = openingChannel;

    if (!contextChannel->isEmpty())
    {
        this->underlyingChannel_ = contextChannel;
    }
    else
    {
        this->underlyingChannel_ = openingChannel;
    }
    this->isKick_ = this->underlyingChannel_->getType() == Channel::Type::Kick;
    this->resetTargetState();
    if (auto *twitchChannel =
            dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get()))
    {
        QPointer<UserInfoPopup> self(this);
        const auto queueUserStateUpdate = [self] {
            QTimer::singleShot(0, self, [self] {
                if (!self)
                {
                    return;
                }

                self->loadTwitchFollowage(true);
                self->userStateChanged_.invoke();
            });
        };
        this->twitchUserStateConnection_ =
            std::make_unique<pajlada::Signals::ScopedConnection>(
                twitchChannel->userStateChanged.connect(queueUserStateUpdate));
        this->twitchTargetRoleConnection_ =
            std::make_unique<pajlada::Signals::ScopedConnection>(
                twitchChannel->targetModeratorChanged.connect(
                    [this](const QString &login) {
                        if (login.compare(this->userName_,
                                          Qt::CaseInsensitive) == 0)
                        {
                            this->refreshTargetModerationStatus();
                            this->userStateChanged_.invoke();
                        }
                    }));
        this->twitchRoomIdConnection_ =
            std::make_unique<pajlada::Signals::ScopedConnection>(
                twitchChannel->roomIdChanged.connect(queueUserStateUpdate));
    }

    this->setWindowTitle(
        TEXT_TITLE.arg(name, this->underlyingChannel_->getName()));
    this->targetBlocked_ = false;
    this->targetLocallyHidden_ = false;
    this->targetIgnoringHighlights_ = false;
    this->targetIgnoreMatchedByRegex_ = false;
    this->canChangeTargetBlock_ = false;
    this->canChangeTargetHighlightIgnore_ = false;
    this->canEditTargetNotes_ = false;
    this->targetBlockStateAccountName_.clear();
    this->followStatusKnown_ = false;
    this->following_ = false;
    this->followStatusRequestInFlight_ = false;
    this->followMutationInFlight_ = false;
    this->ui_.followButton->hide();
    this->identityUserColor_ = {};
    this->messageUserColor_ = {};
    this->apiUserColor_ = {};
    this->apiUserColorLookupFinished_ = false;
    this->ui_.nameLabel->setProperty("paint-login", this->userName_);
    this->ui_.nameLabel->setProperty("paint-kick", this->isKick_);
    if (this->isKick_)
    {
        this->ui_.timeoutWidget->setMinTimeout(60);
    }

    this->isMod_ = false;
    this->isBroadcaster_ = false;
    this->twitchUserLookupFinished_ = false;
    this->refreshSevenTVUserButtonVisibility();
    this->refreshIdentityBadges();
    this->refreshIdentityPaint();
    this->refreshTargetModerationStatus();
    this->updateModeratorCommentsAvailability();
    this->updateUserRolesContext();
    if (!this->isKick_)
    {
        if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(
                this->underlyingChannel_.get()))
        {
            twitchChannel->refreshLeadModStatus();
        }
    }

    this->ui_.nameLabel->setText(name);
    this->ui_.nameLabel->setProperty("copy-text", name);
    this->resetUsercardInfoRows();

    if (this->isKick_)
    {
        this->updateKickUserData();
    }
    else
    {
        this->updateUserData();
    }

    this->userStateChanged_.invoke();

    if (!isId)
    {
        this->updateLatestMessages();
    }
    // If we're opening by ID, this will be called as soon as we get the information from twitch

    this->refreshUsercardActionPlacements();
    this->updateUserLogsContext();
    this->updateUserRolesContext();
}

bool UserInfoPopup::usercardActionAvailable() const
{
    if (!this->underlyingChannel_ || this->userName_.isEmpty())
    {
        return false;
    }
    if (this->isTikTok_)
    {
        return !tikTokProfileUrl(this->platformHandle_).isEmpty();
    }
    if (this->isYouTube_)
    {
        return !youtubeChannelUrl(this->userId_).isEmpty();
    }
    if (this->isKick_ || !this->channel_)
    {
        return false;
    }

    const auto type = this->channel_->getType();
    return type != Channel::Type::TwitchLive &&
           type != Channel::Type::TwitchWhispers &&
           type != Channel::Type::Misc && type != Channel::Type::Kick;
}

void UserInfoPopup::openPlatformUsercard()
{
    if (!this->usercardActionAvailable())
    {
        return;
    }
    if (this->isTikTok_)
    {
        QDesktopServices::openUrl(tikTokProfileUrl(this->platformHandle_));
        return;
    }
    if (this->isYouTube_)
    {
        openYouTubeUrl(QUrl(youtubeChannelUrl(this->userId_)));
        return;
    }

    openTwitchUsercard(this->underlyingChannel_->getName(), this->userName_);
}

bool UserInfoPopup::moderatorCommentsActionAvailable() const
{
    const auto placement = usercardActionPlacement(
        getSettings()->usercardCommentsActionPlacement.getValue());
    return placement != UsercardActionPlacement::Hidden &&
           getSettings()->showModeratorCommentsButton && !this->isYouTube_ &&
           !this->isTikTok_ && !this->isKick_ &&
           dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get()) !=
               nullptr;
}

bool UserInfoPopup::userLogsActionAvailable() const
{
    const auto placement = usercardActionPlacement(
        getSettings()->usercardLogsActionPlacement.getValue());
    const auto *twitchChannel =
        dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get());
    return placement != UsercardActionPlacement::Hidden && !this->isYouTube_ &&
           !this->isTikTok_ && !this->isKick_ && twitchChannel != nullptr &&
           !twitchChannel->getName().isEmpty() && !this->userName_.isEmpty();
}

bool UserInfoPopup::userRolesActionAvailable() const
{
    const auto placement = usercardActionPlacement(
        getSettings()->usercardRolesActionPlacement.getValue());
    return placement != UsercardActionPlacement::Hidden && !this->isYouTube_ &&
           !this->isTikTok_ && !this->isKick_;
}

void UserInfoPopup::toggleModeratorComments()
{
    this->showActivityPage(this->ui_.activityStack->currentWidget() ==
                                   this->ui_.commentsView
                               ? ActivityPage::Messages
                               : ActivityPage::Comments);
}

void UserInfoPopup::toggleUserLogs()
{
    this->showActivityPage(this->ui_.activityStack->currentWidget() ==
                                   this->ui_.logsView
                               ? ActivityPage::Messages
                               : ActivityPage::Logs);
}

void UserInfoPopup::toggleUserRoles()
{
    this->showActivityPage(this->ui_.activityStack->currentWidget() ==
                                   this->ui_.rolesView
                               ? ActivityPage::Messages
                               : ActivityPage::Roles);
}

void UserInfoPopup::openSevenTVUser()
{
    if (this->seventvUserID_.isEmpty())
    {
        this->refreshSevenTVUserButtonVisibility();
        return;
    }

    QDesktopServices::openUrl(QUrl(SEVENTV_USER_PAGE % this->seventvUserID_));
}

bool UserInfoPopup::crossActionAvailable() const
{
    const auto *twitchChannel =
        dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get());
    if (this->isKick_ || this->isYouTube_ || this->isTikTok_ ||
        this->isTargetCurrentUser() || this->userName_.isEmpty() ||
        twitchChannel == nullptr || twitchChannel->roomId().isEmpty())
    {
        return false;
    }

    auto auth = MoltorinoAuth::resolveModerationToken(
        twitchChannel->roomId(), twitchChannel->getName());
    if (!auth.hasToken() &&
        getSettings()->showCrossActionsInUnmoderatedChannels)
    {
        auth = MoltorinoAuth::resolveCurrentUserToken();
    }
    return auth.hasToken() && !auth.legacy;
}

void UserInfoPopup::runCrossAction(const QString &command)
{
    if (!this->crossActionAvailable() || !this->underlyingChannel_)
    {
        return;
    }

    auto value = getApp()->getCommands()->execCommand(
        command + ' ' + this->userName_, this->underlyingChannel_, false);
    if (!value.isEmpty())
    {
        this->underlyingChannel_->sendMessage(value);
    }
}

void UserInfoPopup::runCustomAction(const Command &command)
{
    const auto channel = this->underlyingChannel_;
    if (!channel || channel->isEmpty() || this->userName_.isEmpty() ||
        this->isYouTube_ || this->isTikTok_)
    {
        return;
    }
    auto userId = this->userId_;
    if (this->isKick_)
    {
        userId = !this->userId_.isEmpty() && this->kickUserID_ != 0
                     ? QString::number(this->kickUserID_)
                     : QString{};
    }
    static const QRegularExpression requiredUserId(
        QStringLiteral(R"((^|[^{])({{)*{user\.id})"));
    if (userId.isEmpty() && requiredUserId.match(command.func).hasMatch())
    {
        channel->addSystemMessage(
            "The user ID is still loading. Try again in a moment.");
        return;
    }
    auto words = command.name.split(u' ', Qt::SkipEmptyParts);
    words.append(this->userName_);
    auto *commands = getApp()->getCommands();
    auto expanded = commands->execCustomCommand(
        words, command, false, channel, nullptr,
        {{"user.name", this->userName_}, {"user.id", userId}});
    if (expanded.isEmpty())
    {
        return;
    }
    auto result = commands->execCommand(expanded, channel, false);
    if (!result.isEmpty())
    {
        channel->sendMessage(result);
    }
}

void UserInfoPopup::refreshCustomActions()
{
    if (!this->customActions_)
    {
        return;
    }
    auto *layout = this->customActions_->layout();
    while (auto *item = layout->takeAt(0))
    {
        if (auto *widget = item->widget())
        {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    if (!this->isYouTube_ && !this->isTikTok_)
    {
        for (const auto &command : getApp()->getCommands()->items)
        {
            if (command.usercardInMenu ||
                command.usercardLabel.trimmed().isEmpty())
            {
                continue;
            }
            auto *button = new LabelButton(
                command.usercardLabel.trimmed().left(40), this->customActions_);
            button->setToolTip(command.func);
            QObject::connect(button, &LabelButton::leftClicked, this,
                             [this, command] {
                                 this->runCustomAction(command);
                             });
            layout->addWidget(button);
        }
    }
    this->customActions_->setVisible(layout->count() > 0);
}

void UserInfoPopup::refreshUsercardActionPlacements()
{
    this->refreshCustomActions();
    const auto inBar = [](const QStringSetting &setting) {
        return usercardActionPlacement(setting.getValue()) ==
               UsercardActionPlacement::Bar;
    };
    const bool isSelf = this->isTargetCurrentUser();

    this->ui_.usercardLabel->setVisible(
        this->usercardActionAvailable() &&
        inBar(getSettings()->usercardUsercardActionPlacement));

    this->ui_.notesActionLabel->setVisible(
        inBar(getSettings()->usercardNotesActionPlacement));
    this->ui_.notesActionLabel->setEnabled(this->canEditTargetNotes_);
    this->ui_.notesActionLabel->setToolTip(
        this->canEditTargetNotes_ ? QStringLiteral("Add or edit notes")
                                  : QStringLiteral("Notes are unavailable"));

    const bool blockAvailable =
        !this->isKick_ && !this->isYouTube_ && !this->isTikTok_ && !isSelf;
    this->ui_.blockActionLabel->setVisible(
        blockAvailable &&
        inBar(getSettings()->usercardBlockActionPlacement));
    this->ui_.blockActionLabel->setEnabled(this->canChangeTargetBlock_);
    this->ui_.blockActionLabel->setText(this->targetBlocked_ ? "Unblock"
                                                             : "Block");
    this->ui_.blockActionLabel->setToolTip(
        this->targetBlocked_ ? QStringLiteral("Unblock this user")
                             : QStringLiteral("Block this user"));

    const bool localActionAvailable = !isSelf;
    this->ui_.hideActionLabel->setVisible(
        localActionAvailable &&
        inBar(getSettings()->usercardHideActionPlacement));
    this->ui_.hideActionLabel->setText(this->targetLocallyHidden_ ? "Unhide"
                                                                 : "Hide");
    this->ui_.hideActionLabel->setToolTip(
        this->targetLocallyHidden_
            ? QStringLiteral("Show this user in Moltorino")
            : QStringLiteral("Hide this user in Moltorino"));

    this->ui_.ignoreHighlightsActionLabel->setVisible(
        localActionAvailable &&
        inBar(getSettings()->usercardIgnoreHighlightsActionPlacement));
    this->ui_.ignoreHighlightsActionLabel->setEnabled(
        this->canChangeTargetHighlightIgnore_);
    this->ui_.ignoreHighlightsActionLabel->setText(
        this->targetIgnoringHighlights_ ? "Stop ignoring" : "Ignore");
    this->ui_.ignoreHighlightsActionLabel->setToolTip(
        this->targetIgnoreMatchedByRegex_
            ? QStringLiteral("This user is ignored by a matching rule")
        : this->targetIgnoringHighlights_
            ? QStringLiteral("Stop ignoring this user's highlights")
            : QStringLiteral("Ignore this user's highlights"));

    const bool crossAvailable = this->crossActionAvailable();
    this->ui_.crossBanActionLabel->setVisible(
        crossAvailable &&
        inBar(getSettings()->usercardCrossBanActionPlacement));
    this->ui_.crossUnbanActionLabel->setVisible(
        crossAvailable &&
        inBar(getSettings()->usercardCrossUnbanActionPlacement));
    this->ui_.crossBanActionLabel->setToolTip(
        "Ban this user across your configured channels");
    this->ui_.crossUnbanActionLabel->setToolTip(
        "Unban this user across your configured channels");
}

void UserInfoPopup::updateModeratorCommentsAvailability()
{
    if (this->ui_.commentsLabel == nullptr || this->ui_.commentsView == nullptr)
    {
        return;
    }
    auto *twitchChannel =
        dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get());
    bool hasModerationAccess = false;
    if (twitchChannel != nullptr && !twitchChannel->roomId().isEmpty())
    {
        const auto auth = MoltorinoAuth::resolveModerationToken(
            twitchChannel->roomId(), twitchChannel->getName());
        hasModerationAccess = twitchChannel->hasModRights() ||
                              (auth.hasToken() && !auth.legacy);
    }
    const bool available = this->moderatorCommentsActionAvailable();
    const auto placement = usercardActionPlacement(
        getSettings()->usercardCommentsActionPlacement.getValue());
    this->ui_.commentsLabel->setVisible(
        available && placement == UsercardActionPlacement::Bar);
    if (!available)
    {
        this->ui_.commentsView->setContext({}, {}, {}, {}, false, false);
        if (this->ui_.activityStack->currentWidget() ==
            this->ui_.commentsView)
        {
            this->showActivityPage(ActivityPage::Messages);
        }
        return;
    }

    this->ui_.commentsView->setContext(twitchChannel->roomId(),
                                       twitchChannel->getName(), this->userId_,
                                       this->userName_, hasModerationAccess,
                                       this->twitchUserLookupFinished_);
}

void UserInfoPopup::updateUserLogsContext()
{
    if (this->ui_.logsView == nullptr || this->ui_.userlogsLabel == nullptr)
    {
        return;
    }
    const auto *twitchChannel =
        dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get());
    const bool available = this->userLogsActionAvailable();
    const auto placement = usercardActionPlacement(
        getSettings()->usercardLogsActionPlacement.getValue());
    this->ui_.userlogsLabel->setVisible(
        available && placement == UsercardActionPlacement::Bar);
    this->ui_.logsView->setContext(
        available ? twitchChannel->getName() : QString{},
        available ? this->userName_ : QString{});
    if (!available && this->ui_.activityStack->currentWidget() ==
                          this->ui_.logsView)
    {
        this->showActivityPage(ActivityPage::Messages);
    }
}

void UserInfoPopup::updateUserRolesContext()
{
    if (this->ui_.rolesView == nullptr || this->ui_.rolesLabel == nullptr)
    {
        return;
    }
    const bool available = this->userRolesActionAvailable();
    const auto placement = usercardActionPlacement(
        getSettings()->usercardRolesActionPlacement.getValue());
    this->ui_.rolesLabel->setVisible(
        available && placement == UsercardActionPlacement::Bar);
    this->ui_.rolesView->setContext(available ? this->userId_ : QString{},
                                    available ? this->userName_ : QString{},
                                    this->twitchUserLookupFinished_);
    this->ui_.rolesView->setRoleManagementAvailable(
        available && !this->userId_.isEmpty() &&
        !this->userName_.isEmpty() && this->canShowRoleManagementMenu());
    if (!available && this->ui_.activityStack->currentWidget() ==
                          this->ui_.rolesView)
    {
        this->showActivityPage(ActivityPage::Messages);
    }
}

void UserInfoPopup::showActivityPage(ActivityPage page)
{
    if (this->ui_.activityStack == nullptr ||
        this->ui_.commentsLabel == nullptr ||
        this->ui_.userlogsLabel == nullptr ||
        this->ui_.rolesLabel == nullptr)
    {
        return;
    }
    if (page == ActivityPage::Comments &&
        !this->moderatorCommentsActionAvailable())
    {
        page = ActivityPage::Messages;
    }
    if (page == ActivityPage::Logs && !this->userLogsActionAvailable())
    {
        page = ActivityPage::Messages;
    }
    if (page == ActivityPage::Roles && !this->userRolesActionAvailable())
    {
        page = ActivityPage::Messages;
    }

    QWidget *widget = this->ui_.messagesPage;
    if (page == ActivityPage::Comments)
    {
        widget = this->ui_.commentsView;
    }
    else if (page == ActivityPage::Logs)
    {
        widget = this->ui_.logsView;
    }
    else if (page == ActivityPage::Roles)
    {
        widget = this->ui_.rolesView;
    }
    this->ui_.activityStack->setCurrentWidget(widget);

    const bool commentsShown = page == ActivityPage::Comments;
    const bool logsShown = page == ActivityPage::Logs;
    const bool rolesShown = page == ActivityPage::Roles;
    this->ui_.commentsLabel->setText(commentsShown ? "Messages" : "Comments");
    this->ui_.commentsLabel->setToolTip(
        commentsShown ? "Return to recent messages"
                      : "View moderator comments");
    this->ui_.userlogsLabel->setText(logsShown ? "Messages" : "Logs view");
    this->ui_.userlogsLabel->setToolTip(
        logsShown ? "Return to recent messages" : "Open logs view");
    this->ui_.rolesLabel->setText(rolesShown ? "Messages" : "Roles");
    this->ui_.rolesLabel->setToolTip(
        rolesShown ? "Return to recent messages" : "View roles and channels");

    if (commentsShown)
    {
        this->ui_.commentsView->activate();
    }
    else
    {
        this->ui_.commentsView->deactivate();
    }
    if (logsShown)
    {
        this->ui_.logsView->activate();
    }
    else
    {
        this->ui_.logsView->deactivate();
    }
    if (rolesShown)
    {
        this->ui_.rolesView->activate();
    }
    else
    {
        this->ui_.rolesView->deactivate();
    }
}

void UserInfoPopup::updateLatestMessages()
{
    this->refreshIdentityBadges();
    this->refreshIdentityPaint();
    this->usercardMessagesChannel_ =
        filterMessages(this->userName_, this->userId_,
                       this->targetMessagePlatform(), this->underlyingChannel_);
    this->ui_.latestMessages->setChannel(this->usercardMessagesChannel_);
    this->ui_.latestMessages->setSourceChannel(this->underlyingChannel_);

    this->updateUsercardMessagesVisibility();
    this->maybeStartUsercardMessageAutoLoad();

    this->refreshConnection_ =
        std::make_unique<pajlada::Signals::ScopedConnection>(
            this->underlyingChannel_->messageAppended.connect(
                [this](auto message, auto) {
                    if (this->updateTargetModerationStatusFromMessage(message))
                    {
                        this->userStateChanged_.invoke();
                    }

                    if (this->isYouTube_ && message &&
                        !message->flags.has(MessageFlag::ModerationAction) &&
                        message->userID == this->userId_)
                    {
                        if (const auto author =
                                YouTubeMessageBuilder::cachedAuthorForMessage(
                                    message->id))
                        {
                            const bool wasProtected =
                                this->youtubeTargetIsOwner_ ||
                                this->youtubeTargetIsModerator_;
                            const auto *youtube =
                                dynamic_cast<const YouTubeChannel *>(
                                    this->underlyingChannel_.get());
                            this->youtubeTargetIsOwner_ =
                                author->isOwner ||
                                (youtube && this->userId_ ==
                                                youtube->channelID());
                            this->youtubeTargetIsModerator_ =
                                author->isModerator;
                            if (wasProtected !=
                                (this->youtubeTargetIsOwner_ ||
                                 this->youtubeTargetIsModerator_))
                            {
                                this->userStateChanged_.invoke();
                            }
                        }
                    }

                    if (!checkUsercardMessage(this->userName_, this->userId_,
                                              this->targetMessagePlatform(),
                                              message))
                    {
                        return;
                    }

                    this->refreshIdentityBadges();

                    if (this->usercardMessagesChannel_ &&
                        this->usercardMessagesChannel_->hasMessages())
                    {
                        this->usercardMessagesChannel_->addMessage(
                            message, MessageContext::Repost);
                        this->updateUsercardMessagesVisibility();
                    }
                    else
                    {
                        // The ChannelView is currently hidden, so manually refresh
                        // and display the latest messages
                        this->updateLatestMessages();
                    }
                }));

    this->usercardMessageReplacementConnection_ =
        std::make_unique<pajlada::Signals::ScopedConnection>(
            this->underlyingChannel_->messageReplaced.connect(
                [this](size_t, const MessagePtr &previous,
                       const MessagePtr &replacement) {
                    if (!this->usercardMessagesChannel_ || !previous ||
                        !replacement)
                    {
                        return;
                    }
                    const auto existing =
                        this->usercardMessagesChannel_->findMessageByID(
                            previous->id);
                    if (!existing)
                    {
                        return;
                    }
                    if (checkUsercardMessage(this->userName_, this->userId_,
                                             this->targetMessagePlatform(),
                                             replacement))
                    {
                        this->usercardMessagesChannel_->replaceMessage(
                            existing, replacement);
                    }
                    else
                    {
                        QTimer::singleShot(0, this,
                                           [this] { this->updateLatestMessages(); });
                    }
                }));
    this->usercardMessagesClearedConnection_ =
        std::make_unique<pajlada::Signals::ScopedConnection>(
            this->underlyingChannel_->messagesCleared.connect([this] {
                if (this->usercardMessagesChannel_)
                {
                    this->usercardMessagesChannel_->clearMessages();
                    this->updateUsercardMessagesVisibility();
                }
            }));
}

void UserInfoPopup::updateUsercardMessagesVisibility()
{
    const bool hasMessages = this->usercardMessagesChannel_ &&
                             this->usercardMessagesChannel_->hasMessages();
    const bool hadMessages = this->ui_.latestMessages->isVisible();
    const bool hadNoMessagesLabel = this->ui_.noMessagesLabel->isVisible();
    const bool hadLoadMoreButton =
        this->ui_.loadMoreMessages != nullptr &&
        this->ui_.loadMoreMessages->isVisible();
    const auto previousNoMessagesText = this->ui_.noMessagesLabel->getText();
    QString noMessagesText;
    if (this->usercardMessagesLoading_)
    {
        noMessagesText = QStringLiteral("Loading messages...");
    }
    else if (this->usercardMessagesAuthRequired_)
    {
        noMessagesText =
            QStringLiteral("Connect a moderator account for older messages");
    }
    else if (!this->usercardMessagesError_.isEmpty())
    {
        noMessagesText = QStringLiteral("Older messages couldn't be loaded");
    }
    else
    {
        noMessagesText = QStringLiteral("No recent messages");
    }

    this->ui_.latestMessages->setVisible(hasMessages);
    this->ui_.noMessagesLabel->setText(noMessagesText);
    this->ui_.noMessagesLabel->setToolTip(this->usercardMessagesError_);
    this->ui_.noMessagesLabel->setVisible(!hasMessages);
    this->updateLoadMoreMessagesButton();

    const bool hasLoadMoreButton =
        this->ui_.loadMoreMessages != nullptr &&
        this->ui_.loadMoreMessages->isVisible();
    if (hadMessages != hasMessages || hadNoMessagesLabel != !hasMessages ||
        hadLoadMoreButton != hasLoadMoreButton ||
        previousNoMessagesText != noMessagesText)
    {
        this->applyPopupSize(this->sizeHint());
    }
}

void UserInfoPopup::resetUsercardMessageLoader()
{
    ++this->usercardMessagesRequestGeneration_;
    this->usercardMessagesCursor_.clear();
    this->usercardMessagesError_.clear();
    this->usercardMessagesLoading_ = false;
    this->usercardMessagesHasNextPage_ = true;
    this->usercardMessagesLazyLoadEnabled_ =
        getSettings()->alwaysLoadMoreUsercardMessages;
    this->usercardMessagesAuthRequired_ = false;
    this->usercardMessagesChannel_.reset();
    this->refreshConnection_.reset();
    this->usercardMessageReplacementConnection_.reset();
    this->usercardMessagesClearedConnection_.reset();
    this->ui_.latestMessages->setChannel(Channel::getEmpty());
    this->updateUsercardMessagesVisibility();
    this->updateLoadMoreMessagesButton();
}

bool UserInfoPopup::canLoadMoreUsercardMessages() const
{
    if (this->isKick_ || this->isYouTube_ || this->isTikTok_ ||
        this->userName_.isEmpty() || this->userId_.isEmpty() ||
        !this->underlyingChannel_)
    {
        return false;
    }

    auto *twitchChannel =
        dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get());
    if (twitchChannel == nullptr || twitchChannel->roomId().isEmpty())
    {
        return false;
    }

    return true;
}

void UserInfoPopup::updateLoadMoreMessagesButton()
{
    auto *button = this->ui_.loadMoreMessages;
    if (button == nullptr)
    {
        return;
    }

    const bool canLoad = getSettings()->showUsercardLoadMoreMessagesButton &&
                         this->canLoadMoreUsercardMessages() &&
                         this->usercardMessagesHasNextPage_ &&
                         !this->usercardMessagesLazyLoadEnabled_;
    button->setVisible(canLoad);
    button->setEnabled(canLoad && !this->usercardMessagesLoading_);

    if (this->usercardMessagesLoading_)
    {
        button->setText("Loading messages...");
        button->setToolTip("Loading older messages from Twitch mod logs");
    }
    else
    {
        button->setText(this->usercardMessagesError_.isEmpty()
                            ? "Load more messages"
                            : "Try again");
        button->setToolTip(this->usercardMessagesError_.isEmpty()
                               ? "Load older messages from Twitch mod logs"
                               : this->usercardMessagesError_);
    }
}

void UserInfoPopup::maybeStartUsercardMessageAutoLoad()
{
    if (!getSettings()->alwaysLoadMoreUsercardMessages ||
        !this->canLoadMoreUsercardMessages() ||
        !this->usercardMessagesHasNextPage_)
    {
        return;
    }

    this->usercardMessagesLazyLoadEnabled_ = true;
    this->updateLoadMoreMessagesButton();

    const bool hasMessages = this->usercardMessagesChannel_ &&
                             this->usercardMessagesChannel_->hasMessages();
    if (!hasMessages)
    {
        this->requestMoreUsercardMessages(false);
        return;
    }

    this->maybeLoadMoreUsercardMessagesFromScroll();
}

void UserInfoPopup::requestMoreUsercardMessages(bool enableLazyLoadOnSuccess)
{
    if (this->usercardMessagesLoading_ ||
        !this->canLoadMoreUsercardMessages() ||
        !this->usercardMessagesHasNextPage_)
    {
        return;
    }

    this->usercardMessagesError_.clear();
    this->usercardMessagesLoading_ = true;
    this->updateUsercardMessagesVisibility();
    this->fetchMoreUsercardMessages(2, enableLazyLoadOnSuccess);
}

void UserInfoPopup::maybeLoadMoreUsercardMessagesFromScroll()
{
    if (!this->usercardMessagesLazyLoadEnabled_ ||
        this->usercardMessagesLoading_ ||
        !this->usercardMessagesHasNextPage_)
    {
        return;
    }

    auto &scrollbar = this->ui_.latestMessages->getScrollBar();
    if (scrollbar.getDesiredValue() >
        scrollbar.getMinimum() + usercardMessagePreloadDistance(scrollbar))
    {
        return;
    }

    this->requestMoreUsercardMessages(false);
}

void UserInfoPopup::fetchMoreUsercardMessages(int emptyPageSkipsLeft,
                                              bool enableLazyLoadOnSuccess)
{
    auto *twitchChannel =
        dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get());
    if (twitchChannel == nullptr)
    {
        this->usercardMessagesLazyLoadEnabled_ = false;
        this->usercardMessagesAuthRequired_ = true;
        this->usercardMessagesLoading_ = false;
        this->updateUsercardMessagesVisibility();
        return;
    }

    QString authError;
    const auto auth = MoltorinoAuth::resolveModerationToken(
        twitchChannel->roomId(), twitchChannel->getName(), &authError);
    if (!auth.hasToken() || (auth.legacy && !twitchChannel->hasModRights()))
    {
        this->usercardMessagesError_ =
            authError.isEmpty()
                ? MoltorinoAuth::authRequiredMessage(
                      "loading older messages")
                : authError;
        this->usercardMessagesAuthRequired_ = true;
        this->usercardMessagesLazyLoadEnabled_ = false;
        this->usercardMessagesLoading_ = false;
        this->updateUsercardMessagesVisibility();
        return;
    }

    const auto generation = this->usercardMessagesRequestGeneration_;
    const auto channelId = twitchChannel->roomId();
    const auto channelName = twitchChannel->getName();
    const auto targetUserId = this->userId_;
    const auto cursor = this->usercardMessagesCursor_;
    const auto oldestLoadedMessage =
        oldestUsercardMessageTime(this->usercardMessagesChannel_);
    const QPointer<UserInfoPopup> self(this);

    TwitchGql::getUsercardMessagesBySender(
        channelId, targetUserId, cursor, auth.token,
        [self, generation, targetUserId, channelName, emptyPageSkipsLeft,
         oldestLoadedMessage, enableLazyLoadOnSuccess, cursor](
            GqlUsercardMessagePage page) mutable {
            if (!self || generation != self->usercardMessagesRequestGeneration_ ||
                self->userId_ != targetUserId)
            {
                return;
            }

            self->usercardMessagesCursor_ = page.nextCursor;
            self->usercardMessagesHasNextPage_ =
                page.hasNextPage && !page.nextCursor.isEmpty() &&
                page.nextCursor != cursor;

            if (!self->usercardMessagesChannel_)
            {
                self->usercardMessagesChannel_ =
                    std::make_shared<TwitchChannel>(channelName);
                self->ui_.latestMessages->setChannel(
                    self->usercardMessagesChannel_);
                self->ui_.latestMessages->setSourceChannel(
                    self->underlyingChannel_);
            }

            std::vector<MessagePtr> messages;
            messages.reserve(static_cast<size_t>(page.messages.size()));
            auto *renderChannel = dynamic_cast<TwitchChannel *>(
                self->underlyingChannel_.get());
            for (auto it = page.messages.crbegin(); it != page.messages.crend();
                 ++it)
            {
                const auto sentAt = parseIvrTimestamp(it->sentAt);
                if (oldestLoadedMessage.isValid() && sentAt.isValid() &&
                    sentAt >= oldestLoadedMessage)
                {
                    continue;
                }

                if (self->usercardMessagesChannel_->findMessageByID(it->id))
                {
                    continue;
                }

                messages.push_back(makeUsercardModLogMessage(
                    *it, renderChannel, channelName, targetUserId));
            }

            if (!messages.empty())
            {
                const auto newerDate =
                    oldestUsercardMessageDate(self->usercardMessagesChannel_);
                self->usercardMessagesChannel_->addMessagesAtStart(
                    withUsercardDateSeparators(messages, newerDate));
                if (enableLazyLoadOnSuccess)
                {
                    self->usercardMessagesLazyLoadEnabled_ = true;
                }
                self->usercardMessagesLoading_ = false;
                self->usercardMessagesError_.clear();
                self->usercardMessagesAuthRequired_ = false;
                self->updateUsercardMessagesVisibility();
                QTimer::singleShot(0, self.data(), [self] {
                    if (self)
                    {
                        self->maybeLoadMoreUsercardMessagesFromScroll();
                    }
                });
                return;
            }

            if (self->usercardMessagesHasNextPage_ &&
                emptyPageSkipsLeft > 0)
            {
                self->fetchMoreUsercardMessages(emptyPageSkipsLeft - 1,
                                                enableLazyLoadOnSuccess);
                return;
            }

            self->usercardMessagesLoading_ = false;
            self->updateUsercardMessagesVisibility();
        },
        [self, generation](const QString &error) {
            if (!self || generation != self->usercardMessagesRequestGeneration_)
            {
                return;
            }

            qCWarning(chatterinoWidget)
                << "Failed to load usercard messages:" << error;
            const auto normalized = MoltorinoAuth::normalizeAuthError(
                "loading older messages", error);
            self->usercardMessagesError_ = normalized;
            self->usercardMessagesAuthRequired_ = normalized != error;
            self->usercardMessagesLazyLoadEnabled_ = false;
            self->usercardMessagesLoading_ = false;
            self->updateUsercardMessagesVisibility();
        });
}

void UserInfoPopup::updateUserData()
{
    if (this->isTikTok_ || this->isYouTube_)
    {
        return;
    }
    std::weak_ptr<bool> hack = this->lifetimeHack_;
    const auto requestGeneration = ++this->userDataRequestGeneration_;
    const auto isCurrentRequest = [this, hack, requestGeneration] {
        return hack.lock() &&
               requestGeneration == this->userDataRequestGeneration_;
    };
    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();

    const auto onUserFetchFailed = [this, isCurrentRequest] {
        if (!isCurrentRequest())
        {
            return;
        }

        // this can occur when the account doesn't exist.
        if (getSettings()->showUsercardFollowerCount)
        {
            this->ui_.followerCountLabel->setText(
                TEXT_FOLLOWERS.arg(TEXT_UNAVAILABLE));
            this->ui_.followerCountLabel->setVisible(true);
        }
        if (getSettings()->showUsercardCreatedDate)
        {
            this->ui_.createdDateLabel->setText(
                TEXT_CREATED.arg(TEXT_UNAVAILABLE));
            this->ui_.createdDateLabel->setVisible(true);
        }

        this->ui_.nameLabel->setText(this->userName_);

        this->ui_.userIDLabel->setText(u"ID " % TEXT_UNAVAILABLE);
        this->ui_.userIDLabel->setProperty("copy-text",
                                           TEXT_UNAVAILABLE.toString());

        if (getSettings()->showUsercardFollowage)
        {
            this->ui_.followageLabel->setText({});
        }
        if (getSettings()->showUsercardSubage)
        {
            this->ui_.subageLabel->setText({});
        }
        if (getSettings()->showUsercardChatterCount)
        {
            this->ui_.chatterCountLabel->setText("Chatters: " %
                                                 TEXT_UNAVAILABLE);
        }
        if (getSettings()->showUsercardLastLive)
        {
            this->ui_.lastLiveLabel->setText("Last live: " %
                                             TEXT_UNAVAILABLE);
        }
        this->apiUserColor_ = {};
        this->apiUserColorLookupFinished_ = true;
        this->updateUsercardColor();
        if (getSettings()->showUsercardStatus)
        {
            this->ui_.statusLabel->setText("Status: " % TEXT_UNAVAILABLE);
        }

        this->seventvUserRequestGeneration_++;
        this->seventvUserID_.clear();
        this->seventvUserLookupInFlight_ = false;
        this->seventvUserLookupFinished_ = true;
        this->refreshSevenTVUserButtonVisibility();
        this->twitchUserLookupFinished_ = true;
        this->updateModeratorCommentsAvailability();
        this->updateUserRolesContext();
    };
    const auto onUserFetched = [this, isCurrentRequest,
                                currentUser](const HelixUser &user) {
        if (!isCurrentRequest())
        {
            return;
        }

        this->twitchUserLookupFinished_ = true;
        this->userId_ = user.id;

        // Correct for when being opened with ID
        if (this->userName_.isEmpty())
        {
            this->userName_ = user.login;
            this->ui_.nameLabel->setProperty("paint-login", this->userName_);
            this->ui_.nameLabel->setText(user.login);

            this->refreshTargetModerationStatus();
            this->userStateChanged_.invoke();

            // Ensure recent messages are shown
            this->updateLatestMessages();
        }
        else
        {
            this->refreshIdentityBadges();
        }

        this->updateModeratorCommentsAvailability();
        this->updateUserLogsContext();
        this->updateUserRolesContext();

        this->resetNameHistory();
        this->refreshFollowButton();
        this->updateLoadMoreMessagesButton();
        this->maybeStartUsercardMessageAutoLoad();
        this->helixAvatarUrl_ = user.profileImageUrl;
        this->updateAvatarUrl();
        this->updateNotes();

        // copyable button for login name of users with a localized username
        if (user.displayName.toLower() != user.login)
        {
            this->ui_.localizedNameLabel->setText(user.displayName);
            this->ui_.localizedNameLabel->setProperty("copy-text",
                                                      user.displayName);
            this->ui_.localizedNameLabel->setVisible(true);
            this->ui_.localizedNameCopyButton->setVisible(true);
        }
        else
        {
            this->ui_.nameLabel->setText(user.displayName);
            this->ui_.nameLabel->setProperty("copy-text", user.displayName);
        }

        this->setWindowTitle(TEXT_TITLE.arg(
            user.displayName, this->underlyingChannel_->getName()));
        if (getSettings()->showUsercardCreatedDate)
        {
            this->ui_.createdDateLabel->setText(
                TEXT_CREATED.arg(user.createdAt.section("T", 0, 0)));
            this->ui_.createdDateLabel->setToolTip(
                formatLongFriendlyDuration(
                    QDateTime::fromString(user.createdAt, Qt::ISODateWithMs),
                    QDateTime::currentDateTimeUtc()) +
                u" ago"_s);
            this->ui_.createdDateLabel->setMouseTracking(true);
            this->ui_.createdDateLabel->setVisible(true);
        }
        this->ui_.userIDLabel->setText(TEXT_USER_ID % user.id);
        this->ui_.userIDLabel->setProperty("copy-text", user.id);

        if (getApp()->getStreamerMode()->isEnabled() &&
            getSettings()->streamerModeHideUsercardAvatars)
        {
            this->ui_.avatarButton->setPixmap(getResources().streamerMode);
            if (getSettings()->showSevenTVUsercardButton)
            {
                this->loadSevenTVAvatar(user.id, false, false);
            }
        }
        else
        {
            this->loadAvatar(user.id, user.profileImageUrl, false);
        }

        if (getSettings()->showUsercardFollowerCount)
        {
            getHelix()->getChannelFollowers(
                user.id,
                [this, isCurrentRequest](const auto &followers) {
                    if (!isCurrentRequest() ||
                        !getSettings()->showUsercardFollowerCount)
                    {
                        return;
                    }
                    this->ui_.followerCountLabel->setText(
                        TEXT_FOLLOWERS.arg(localizeNumbers(followers.total)));
                    this->ui_.followerCountLabel->setVisible(true);
                },
                [](const auto &errorMessage) {
                    qCWarning(chatterinoTwitch)
                        << "Error getting followers:" << errorMessage;
                });
        }
        getHelix()->getStreamById(
            user.id,
            [this, isCurrentRequest](bool isLive, const auto &stream) {
                if (!isCurrentRequest())
                {
                    return;
                }

                if (isLive)
                {
                    this->ui_.liveIndicator->setViewers(stream.viewerCount);
                    this->ui_.liveIndicator->show();
                }
                else
                {
                    this->ui_.liveIndicator->hide();
                }
            },
            [id{user.id}]() {
                qCWarning(chatterinoWidget)
                    << "Failed to get stream for user ID" << id;
            },
            []() {});

        // get ignore state
        bool isIgnoring = currentUser->blockedUserIds().contains(user.id);

        this->targetBlocked_ = isIgnoring;
        this->targetBlockStateAccountName_ = currentUser->getUserName();
        this->canChangeTargetBlock_ =
            !currentUser->isAnon() && !this->isTargetCurrentUser();
        this->refreshLocalUserActions();

        auto type = this->underlyingChannel_->getType();

        if (type == Channel::Type::Twitch)
        {
            this->loadTwitchFollowage();
            if (getSettings()->showUsercardSubage)
            {
                getIvr()->getSubage(
                    this->userName_, this->underlyingChannel_->getName(),
                    [this, isCurrentRequest](const IvrSubage &subageInfo) {
                        if (!isCurrentRequest())
                        {
                            return;
                        }

                        if (!getSettings()->showUsercardSubage)
                        {
                            return;
                        }

                        if (subageInfo.isSubHidden)
                        {
                            this->ui_.subageLabel->setText(
                                "Subscription status hidden");
                            this->updateUsercardStatusIcons();
                            this->ui_.subageRow->setVisible(true);
                            this->ui_.subageIcon->setVisible(false);
                        }
                        else if (subageInfo.isSubbed)
                        {
                            auto subageText =
                                QString("Tier %1 - Subscribed for %2 months")
                                    .arg(subageInfo.subTier)
                                    .arg(subageInfo.totalSubMonths);
                            if (getSettings()->showUsercardSubageRelativeTime)
                            {
                                subageText += formatUsercardYearsMonths(
                                    subageInfo.totalSubMonths);
                            }
                            if (getSettings()->showUsercardSubGiftSource &&
                                subageInfo.isGifted)
                            {
                                if (subageInfo.giftIsAnonymous ||
                                    subageInfo.giftSource.isEmpty())
                                {
                                    subageText +=
                                        QStringLiteral(" · Gifted anonymously");
                                }
                                else
                                {
                                    subageText += QStringLiteral(" · Gifted by %1")
                                                      .arg(subageInfo.giftSource);
                                }
                            }
                            this->ui_.subageLabel->setText(subageText);
                            this->updateUsercardStatusIcons();
                            this->ui_.subageRow->setVisible(true);
                            this->ui_.subageIcon->setVisible(true);
                        }
                        else if (subageInfo.totalSubMonths)
                        {
                            auto subageText =
                                QString("Previously subscribed for %1 months")
                                    .arg(subageInfo.totalSubMonths);
                            if (getSettings()->showUsercardSubageRelativeTime)
                            {
                                subageText += formatUsercardYearsMonths(
                                    subageInfo.totalSubMonths);
                            }
                            this->ui_.subageLabel->setText(subageText);
                            this->updateUsercardStatusIcons();
                            this->ui_.subageRow->setVisible(true);
                            this->ui_.subageIcon->setVisible(true);
                        }
                        else
                        {
                            this->ui_.subageLabel->setText({});
                            this->ui_.subageRow->hide();
                            this->ui_.subageIcon->setVisible(false);
                        }
                    },
                    [this, isCurrentRequest] {
                        if (!isCurrentRequest())
                        {
                            return;
                        }

                        if (getSettings()->showUsercardSubage)
                        {
                            this->ui_.subageLabel->setText({});
                            this->ui_.subageRow->hide();
                            this->ui_.subageIcon->setVisible(false);
                        }
                    });
            }

            getIvr()->getUser(
                user.login,
                [this, isCurrentRequest](const IvrUserProfile &profile) {
                    if (!isCurrentRequest())
                    {
                        return;
                    }

                    this->applyIvrUserProfile(profile);
                },
                [this, isCurrentRequest] {
                    if (!isCurrentRequest())
                    {
                        return;
                    }

                    if (getSettings()->showUsercardChatterCount)
                    {
                        this->ui_.chatterCountLabel->setText(
                            "Chatters: " % TEXT_UNAVAILABLE);
                    }
                    if (getSettings()->showUsercardLastLive)
                    {
                        this->ui_.lastLiveLabel->setText("Last live: " %
                                                         TEXT_UNAVAILABLE);
                    }
                    this->apiUserColor_ = {};
                    this->apiUserColorLookupFinished_ = true;
                    this->updateUsercardColor();
                    if (getSettings()->showUsercardStatus)
                    {
                        this->ui_.statusLabel->setText("Status: " %
                                                       TEXT_UNAVAILABLE);
                    }
                });
        }
        else
        {
            this->apiUserColor_ = {};
            this->apiUserColorLookupFinished_ = true;
            this->updateUsercardColor();
        }

        // get pronouns
        if (getSettings()->showPronouns)
        {
            getApp()->getPronouns()->getUserPronoun(
                user.login,
                [this, isCurrentRequest](const auto userPronoun) {
                    runInGuiThread([this, isCurrentRequest,
                                    userPronoun = std::move(userPronoun)]() {
                        if (!isCurrentRequest() ||
                            this->ui_.pronounsLabel == nullptr)
                        {
                            return;
                        }
                        if (!userPronoun.isUnspecified())
                        {
                            this->ui_.pronounsLabel->setText(
                                TEXT_PRONOUNS.arg(userPronoun.format()));
                        }
                        else
                        {
                            this->ui_.pronounsLabel->setText(
                                TEXT_PRONOUNS.arg(TEXT_UNSPECIFIED));
                        }
                    });
                },
                [this, isCurrentRequest]() {
                    runInGuiThread([this, isCurrentRequest]() {
                        qCWarning(chatterinoTwitch) << "Error getting pronouns";
                        if (!isCurrentRequest())
                        {
                            return;
                        }
                        this->ui_.pronounsLabel->setText(
                            TEXT_PRONOUNS.arg(TEXT_UNSPECIFIED));
                    });
                });
        }
    };

    if (!this->userId_.isEmpty())
    {
        getHelix()->getUserById(this->userId_, onUserFetched,
                                onUserFetchFailed);
    }
    else
    {
        getHelix()->getUserByName(this->userName_, onUserFetched,
                                  onUserFetchFailed);
    }

    this->canChangeTargetBlock_ = false;
    this->canChangeTargetHighlightIgnore_ = false;
    this->canEditTargetNotes_ = false;
    this->targetBlockStateAccountName_.clear();
    this->refreshUsercardActionPlacements();
}

void UserInfoPopup::loadAvatar(const QString &userID, const QString &pictureURL,
                               bool isKick, bool allowSevenTVLookup)
{
    const QUrl avatarUrl(pictureURL);
    if (!avatarUrl.isValid() || avatarUrl.scheme() != QStringLiteral("https") ||
        avatarUrl.host().isEmpty() || !avatarUrl.userInfo().isEmpty() ||
        avatarUrl.port(443) != 443)
    {
        return;
    }
    this->helixAvatarUrl_ = pictureURL;
    this->updateAvatarUrl();
    const auto generation = this->userDataRequestGeneration_;
    const auto applyAvatar = [this](QPixmap avatar) {
        if (this->isTikTok_)
        {
            const auto side = std::min(avatar.width(), avatar.height());
            QPixmap square(side, side);
            square.fill(QColor(22, 24, 35));
            QPainter painter(&square);
            painter.drawPixmap(0, 0, avatar, (avatar.width() - side) / 2,
                               (avatar.height() - side) / 2, side, side);
            painter.end();
            avatar = std::move(square);
        }
        this->avatarPixmap_ = std::move(avatar);
        if (this->isTwitchAvatarShown_ &&
            !(getApp()->getStreamerMode()->isEnabled() &&
              getSettings()->streamerModeHideUsercardAvatars))
        {
            this->ui_.avatarButton->setPixmap(this->avatarPixmap_);
        }
    };
    auto filename =
        getApp()->getPaths().cacheDirectory() + "/" + hashUrl(pictureURL);
    QFile cacheFile(filename);
    QPixmap cachedAvatar;
    if (cacheFile.size() <= MAX_AVATAR_BYTES &&
        cacheFile.open(QIODevice::ReadOnly))
    {
        cachedAvatar = readUsercardAvatar(cacheFile.readAll());
    }
    if (!cachedAvatar.isNull())
    {
        applyAvatar(std::move(cachedAvatar));
    }
    else if (!pictureURL.isEmpty())
    {
        NetworkRequest(pictureURL)
            .caller(this)
            .timeout(10000)
            .maximumResponseSize(MAX_AVATAR_BYTES)
            .onSuccess([this, generation, pictureURL, filename,
                        applyAvatar](const NetworkResult &result) {
                if (generation != this->userDataRequestGeneration_ ||
                    pictureURL != this->helixAvatarUrl_)
                {
                    return;
                }
                auto avatar = readUsercardAvatar(result.getData());
                if (!avatar.isNull())
                {
                    this->saveCacheAvatar(result.getData(), filename);
                    applyAvatar(std::move(avatar));
                }
            })
            .execute();
    }

    if (allowSevenTVLookup &&
        (getSettings()->displaySevenTVAnimatedProfile ||
         getSettings()->showSevenTVUsercardButton))
    {
        this->loadSevenTVAvatar(userID, isKick);
    }
}

void UserInfoPopup::loadSevenTVAvatar(const QString &userID, bool isKick,
                                      bool allowAvatarDownload)
{
    const auto generation = ++this->seventvUserRequestGeneration_;
    if (userID.isEmpty())
    {
        this->seventvUserID_.clear();
        this->seventvUserLookupInFlight_ = false;
        this->seventvUserLookupFinished_ = true;
        this->refreshSevenTVUserButtonVisibility();
        return;
    }

    auto fmt = isKick ? SEVENTV_KICK_USER_API : SEVENTV_TWITCH_USER_API;
    const auto cacheKey = sevenTVUserCacheKey(userID, isKick);
    const auto *cachedUserID = sevenTVUserIDCache().object(cacheKey);
    const bool hadCachedUserID =
        cachedUserID != nullptr && !cachedUserID->isEmpty();
    const bool needsAvatar =
        allowAvatarDownload && getSettings()->displaySevenTVAnimatedProfile;

    if (cachedUserID != nullptr)
    {
        this->seventvUserID_ = *cachedUserID;
        this->seventvUserLookupInFlight_ = false;
        this->seventvUserLookupFinished_ = true;
        this->refreshSevenTVUserButtonVisibility();

        if (!needsAvatar || this->seventvUserID_.isEmpty())
        {
            return;
        }
    }
    else
    {
        this->seventvUserID_.clear();
        this->seventvUserLookupInFlight_ = true;
        this->seventvUserLookupFinished_ = false;
        this->refreshSevenTVUserButtonVisibility();
    }

    NetworkRequest(fmt.arg(userID))
        .caller(this)
        .timeout(20000)
        .maximumResponseSize(2 * 1024 * 1024)
        .onSuccess([this, hack = std::weak_ptr<bool>(this->lifetimeHack_),
                    generation, cacheKey,
                    allowAvatarDownload](const NetworkResult &result) {
            if (!hack.lock() ||
                generation != this->seventvUserRequestGeneration_)
            {
                return;
            }

            const auto root = result.parseJson();
            const auto userObj = root["user"].toObject();
            this->seventvUserID_ = userObj["id"].toString();
            if (!this->seventvUserID_.isEmpty())
            {
                sevenTVUserIDCache().insert(
                    cacheKey, new QString(this->seventvUserID_));
            }
            this->seventvUserLookupInFlight_ = false;
            this->seventvUserLookupFinished_ = true;
            this->refreshSevenTVUserButtonVisibility();

            if (!allowAvatarDownload ||
                !getSettings()->displaySevenTVAnimatedProfile)
            {
                return;
            }

            auto url = userObj["avatar_url"].toString();

            if (url.isEmpty())
            {
                return;
            }
            if (!url.startsWith(u"https:"))
            {
                url.prepend(u"https:");
            }
            const QUrl avatarUrl(url);
            if (!avatarUrl.isValid() || avatarUrl.scheme() != QStringLiteral("https") ||
                avatarUrl.host().isEmpty() || !avatarUrl.userInfo().isEmpty() ||
                avatarUrl.port(443) != 443)
            {
                return;
            }
            this->seventvAvatarUrl_ = url;
            if (this->helixAvatarUrl_ == this->seventvAvatarUrl_)
            {
                return;
            }

            auto dotIdx = url.lastIndexOf('.') + 1;
            QByteArray format;
            if (dotIdx > 0)
            {
                auto end = url.size();
                auto queryIdx = url.lastIndexOf('?');
                if (queryIdx > dotIdx)
                {
                    end = queryIdx;
                }
                format = QStringView(url).sliced(dotIdx, end - dotIdx).toUtf8();
            }

            // We're implementing custom caching here,
            // because we need the cached file path.
            auto hash = hashUrl(url);
            auto filename = getApp()->getPaths().cacheDirectory() + "/" + hash;

            QFile cacheFile(filename);
            if (cacheFile.exists())
            {
                this->setSevenTVAvatar(filename, format);
                return;
            }

            NetworkRequest(url)
                .caller(this)
                .timeout(10000)
                .maximumResponseSize(MAX_AVATAR_BYTES)
                .onSuccess([this, generation, filename,
                            format](const NetworkResult &result) {
                    if (generation != this->seventvUserRequestGeneration_ ||
                        !getSettings()->displaySevenTVAnimatedProfile)
                    {
                        return;
                    }
                    this->saveCacheAvatar(result.getData(), filename);
                    this->setSevenTVAvatar(filename, format);
                })
                .execute();

            return;
        })
        .onError([this, hack = std::weak_ptr<bool>(this->lifetimeHack_),
                  generation, cacheKey,
                  hadCachedUserID](const NetworkResult &result) {
            if (!hack.lock() ||
                generation != this->seventvUserRequestGeneration_)
            {
                return;
            }

            const auto status = result.status();
            if (status && *status == 404)
            {
                sevenTVUserIDCache().insert(cacheKey, new QString);
            }
            else if (hadCachedUserID)
            {
                return;
            }

            this->seventvUserID_.clear();
            this->seventvUserLookupInFlight_ = false;
            this->seventvUserLookupFinished_ = true;
            this->refreshSevenTVUserButtonVisibility();
        })
        .execute();
}

void UserInfoPopup::setSevenTVAvatar(const QString &filename,
                                     const QByteArray &format)
{
    if (QFileInfo(filename).size() > MAX_AVATAR_BYTES)
    {
        return;
    }
    QImageReader reader(filename, format);
    const auto size = reader.size();
    if (!size.isValid() || size.width() > 2048 || size.height() > 2048)
    {
        return;
    }
    auto *movie = new QMovie(filename, format, this);
    movie->setScaledSize(QSize(100, 100) * this->devicePixelRatioF());
    if (!movie->isValid())
    {
        qCWarning(chatterinoSeventv)
            << "Error reading Profile Picture, " << movie->lastErrorString();
        delete movie;
        return;
    }

    const auto avatarUrl = this->seventvAvatarUrl_;
    this->clearSevenTVAvatar();
    this->seventvAvatarUrl_ = avatarUrl;
    this->seventvAvatar_ = movie;

    QObject::connect(movie, &QMovie::frameChanged, this, [this, movie] {
        if (!this->isTwitchAvatarShown_ &&
            !(getApp()->getStreamerMode()->isEnabled() &&
              getSettings()->streamerModeHideUsercardAvatars))
        {
            this->ui_.avatarButton->setPixmap(movie->currentPixmap());
        }
    });

    this->ui_.switchAvatars->setText(u"Show " % this->platformName());
    this->isTwitchAvatarShown_ = false;
    this->updateAvatarUrl();
    this->refreshAvatarVisibility();
}

void UserInfoPopup::saveCacheAvatar(const QByteArray &avatar,
                                    const QString &filename) const
{
    QFile outfile(filename);
    if (outfile.open(QIODevice::WriteOnly))
    {
        if (outfile.write(avatar) == -1)
        {
            qCWarning(chatterinoImage) << "Error writing to cache" << filename;
            this->ui_.avatarButton->setPixmap(QPixmap());
        }
    }
    else
    {
        qCWarning(chatterinoImage) << "Error writing to cache" << filename;
        this->ui_.avatarButton->setPixmap(QPixmap());
    }
}

void UserInfoPopup::updateNotes()
{
    static QRegularExpression onlySpaceRegex{"^\\s*$"};

    auto userData = getApp()->getUserData()->getUser(this->notesUserKey());
    if (!userData.has_value() ||
        onlySpaceRegex.match(userData->notes).hasMatch())
    {
        this->ui_.notesPreview->setText("");
        this->ui_.notesPreview->setVisible(false);
        return;
    }

    if (getApp()->getStreamerMode()->isEnabled() &&
        getSettings()->streamerModeHideUserNotes)
    {
        this->ui_.notesPreview->setText("Notes hidden in streamer mode.");
        this->ui_.notesPreview->setVisible(true);
        return;
    }

    this->ui_.notesPreview->setText(userData->notes);
    this->ui_.notesPreview->setVisible(true);
}

void UserInfoPopup::updateKickUserData()
{
    assert(this->isKick_);

    auto onChannelFetchFailed = [](UserInfoPopup *self) {
        // this can occur when the account doesn't exist.
        if (getSettings()->showUsercardFollowerCount)
        {
            self->ui_.followerCountLabel->setText(
                TEXT_FOLLOWERS.arg(TEXT_UNAVAILABLE));
            self->ui_.followerCountLabel->setVisible(true);
        }
        if (getSettings()->showUsercardCreatedDate)
        {
            self->ui_.createdDateLabel->setText(
                TEXT_CREATED.arg(TEXT_UNAVAILABLE));
            self->ui_.createdDateLabel->setVisible(true);
        }

        self->ui_.nameLabel->setText(
            self->userName_.isEmpty()
                ? QStringLiteral("User ID %1").arg(self->userId_)
                : self->userName_);

        self->ui_.userIDLabel->setText(u"ID " % TEXT_UNAVAILABLE);
        self->ui_.userIDLabel->setProperty("copy-text",
                                           TEXT_UNAVAILABLE.toString());

        self->seventvUserRequestGeneration_++;
        self->seventvUserID_.clear();
        self->seventvUserLookupInFlight_ = false;
        self->seventvUserLookupFinished_ = true;
        self->refreshSevenTVUserButtonVisibility();
    };
    auto onChannelFetched = [](UserInfoPopup *self,
                               const KickPrivateChannelInfo &channel) {
        self->kickUserSlug_ = channel.slug;

        if (self->userName_ != channel.user.username)
        {
            self->userName_ = channel.user.username;
            self->ui_.nameLabel->setText(channel.user.username);

            // Ensure recent messages are shown
            self->updateLatestMessages();
        }

        self->kickUserID_ = channel.user.userID;
        auto userIDStr = QString::number(self->kickUserID_);
        self->userId_ = u"kick:" % userIDStr;
        self->helixAvatarUrl_ = channel.user.profilePictureURL.value_or(
            u"https://kick.com/img/default-profile-pictures/default-avatar-2.webp"_s);
        self->updateAvatarUrl();
        self->updateNotes();

        self->ui_.nameLabel->setText(channel.user.username);
        self->ui_.nameLabel->setProperty("copy-text", channel.user.username);
        self->ui_.nameLabel->setProperty("paint-login",
                                         channel.user.username);

        self->setWindowTitle(TEXT_TITLE.arg(
            channel.user.username, self->underlyingChannel_->getName()));
        if (getSettings()->showUsercardCreatedDate)
        {
            self->ui_.createdDateLabel->setText(TEXT_CREATED.arg(
                channel.chatroom.createdAt.date().toString(Qt::ISODate)));
            self->ui_.createdDateLabel->setToolTip(
                formatLongFriendlyDuration(channel.chatroom.createdAt,
                                           QDateTime::currentDateTimeUtc()) +
                u" ago"_s);
            self->ui_.createdDateLabel->setMouseTracking(true);
            self->ui_.createdDateLabel->setVisible(true);
        }
        self->ui_.userIDLabel->setText(TEXT_USER_ID % userIDStr);
        self->ui_.userIDLabel->setProperty("copy-text", userIDStr);

        if (getApp()->getStreamerMode()->isEnabled() &&
            getSettings()->streamerModeHideUsercardAvatars)
        {
            self->ui_.avatarButton->setPixmap(getResources().streamerMode);
            if (getSettings()->showSevenTVUsercardButton)
            {
                self->loadSevenTVAvatar(userIDStr, true, false);
            }
        }
        else
        {
            self->loadAvatar(userIDStr, self->helixAvatarUrl_, true);
        }

        if (!channel.user.profilePictureURL)
        {
            const auto generation = self->userDataRequestGeneration_;
            KickApi::privateChannelInfoSmall(
                channel.slug,
                [weak = QPointer(self), generation](const auto &res) {
                    if (!weak || !res || !weak->isKick_ ||
                        weak->userDataRequestGeneration_ != generation ||
                        !res->user.profilePictureURL)
                    {
                        return;
                    }
                    weak->helixAvatarUrl_ = *res->user.profilePictureURL;
                    weak->avatarPixmap_ = {};
                    weak->updateAvatarUrl();
                    if (!(getApp()->getStreamerMode()->isEnabled() &&
                          getSettings()->streamerModeHideUsercardAvatars))
                    {
                        weak->loadAvatar(QString::number(weak->kickUserID_),
                                         weak->helixAvatarUrl_, true, false);
                    }
                });
        }

        if (getSettings()->showUsercardFollowerCount)
        {
            self->ui_.followerCountLabel->setText(
                TEXT_FOLLOWERS.arg(localizeNumbers(channel.followersCount)));
            self->ui_.followerCountLabel->setVisible(true);
        }

        self->targetBlocked_ = false;
        self->targetBlockStateAccountName_.clear();
        self->canChangeTargetBlock_ = false;
        self->refreshLocalUserActions();
    };

    const auto requestGeneration = this->userDataRequestGeneration_;
    auto fetchChannelInfo =
        [self = QPointer(this), requestGeneration, onChannelFetched,
         onChannelFetchFailed](const QString &userName) {
            KickApi::privateChannelInfo(
                userName, [self, requestGeneration, onChannelFetched,
                           onChannelFetchFailed](const auto &res) {
                    if (!self)
                    {
                        return;
                    }
                    if (self->userDataRequestGeneration_ != requestGeneration ||
                        !self->isKick_)
                    {
                        return;
                    }
                    if (res)
                    {
                        onChannelFetched(self.get(), *res);
                    }
                    else
                    {
                        qCDebug(chatterinoKick)
                            << "Channel fetch failed" << res.error();
                        onChannelFetchFailed(self.get());
                    }
                });
    };
    auto fetchUserInChannelInfo =
        [self = QPointer(this), requestGeneration,
         channelName = this->underlyingChannel_->getName()](
            const QString &userName) {
            KickApi::privateUserInChannelInfo(
                userName, channelName,
                [self, requestGeneration](const auto &res) {
                    if (!self || !res ||
                        self->userDataRequestGeneration_ != requestGeneration ||
                        !self->isKick_)
                    {
                        return;
                    }

                    self->setFollowage(
                        res->followingSince.value_or(QDateTime{}));

                    if (getSettings()->showUsercardSubage &&
                        res->subscriptionMonths)
                    {
                        auto subageText = QString("Subscribed for %1 months")
                                              .arg(*res->subscriptionMonths);
                        if (getSettings()->showUsercardSubageRelativeTime)
                        {
                            subageText += formatUsercardYearsMonths(
                                *res->subscriptionMonths);
                        }
                        self->ui_.subageLabel->setText(subageText);
                        self->updateUsercardStatusIcons();
                        self->ui_.subageRow->setVisible(true);
                        self->ui_.subageIcon->setVisible(true);
                    }
                    else if (getSettings()->showUsercardSubage)
                    {
                        self->ui_.subageLabel->setText({});
                        self->ui_.subageRow->hide();
                        self->ui_.subageIcon->setVisible(false);
                    }
                });
    };

    if (!this->userId_.isEmpty() && this->userName_.isEmpty())
    {
        bool ok = false;
        const auto userID = this->userId_.toULongLong(&ok);
        if (!ok || userID == 0)
        {
            onChannelFetchFailed(this);
        }
        else
        {
            std::array<uint64_t, 1> userIDs{userID};
            getKickApi()->getChannels(
                userIDs,
                [self = QPointer(this), requestGeneration,
                 onChannelFetchFailed, fetchChannelInfo,
                 fetchUserInChannelInfo](const auto &res) {
                    if (!self ||
                        self->userDataRequestGeneration_ != requestGeneration ||
                        !self->isKick_)
                    {
                        return;
                    }
                    if (!res || res->size() != 1 ||
                        res->front().slug.isEmpty())
                    {
                        onChannelFetchFailed(self.get());
                        return;
                    }
                    fetchChannelInfo(res->front().slug);
                    fetchUserInChannelInfo(res->front().slug);
                });
        }
    }
    else
    {
        fetchChannelInfo(this->userName_);
        fetchUserInChannelInfo(this->userName_);
    }

    this->canChangeTargetBlock_ = false;
    this->canChangeTargetHighlightIgnore_ = false;
    this->canEditTargetNotes_ = false;
    this->targetBlockStateAccountName_.clear();
    this->refreshUsercardActionPlacements();
}

void UserInfoPopup::onKickProfilePictureClick(Qt::MouseButton button)
{
    assert(this->isKick_);
    auto channelURL = QUrl("https://kick.com/" + this->kickUserSlug_);

    switch (button)
    {
        case Qt::LeftButton: {
            QDesktopServices::openUrl(channelURL);
        }
        break;

        // largely the same as on Twitch
        case Qt::RightButton: {
            if (this->avatarUrl_.isEmpty())
            {
                return;
            }

            auto *menu = new QMenu(this);
            menu->setAttribute(Qt::WA_DeleteOnClose);

            auto avatarUrl = this->avatarUrl_;

            // add context menu actions
            menu->addAction("Open &avatar in browser", this, [avatarUrl] {
                QDesktopServices::openUrl(QUrl(avatarUrl));
            });

            menu->addAction("Copy a&vatar link", this, [avatarUrl] {
                crossPlatformCopy(avatarUrl);
            });

            // we need to assign login name for msvc compilation
            auto username = this->userName_.toLower();
            menu->addAction(
                "Open channel in a new &popup window", this, [username] {
                    auto *app = getApp();
                    auto *split = app->getWindows()
                                      ->createWindow(WindowType::Popup, true)
                                      .getNotebook()
                                      .getOrAddSelectedPage()
                                      ->appendNewSplit(false);
                    split->setChannel(
                        app->getKickChatServer()->getOrCreate(username));
                });

            menu->addAction("Open channel in a new &tab", this, [username] {
                SplitContainer *container = getApp()
                                                ->getWindows()
                                                ->getMainWindow()
                                                .getNotebook()
                                                .addPage(true);
                auto *split = new Split(container);
                split->setChannel(
                    getApp()->getKickChatServer()->getOrCreate(username));
                container->insertSplit(split);
            });

            menu->addAction("Open channel in &browser", this, [channelURL] {
                QDesktopServices::openUrl(channelURL);
            });

            this->appendCommonProfileActions(menu);

            menu->popup(QCursor::pos());
            menu->raise();
        }
        break;

        default:
            break;
    }
}

QString UserInfoPopup::showProfilePictureContextMenu()
{
    if (this->avatarUrl_.isEmpty())
    {
        return "No avatar is available for this user.";
    }

    if (this->isKick_)
    {
        this->onKickProfilePictureClick(Qt::RightButton);
        return {};
    }

    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    auto avatarUrl = this->avatarUrl_;
    if (this->isTikTok_)
    {
        if (isTikTokImageUrl(QUrl(avatarUrl)))
        {
            menu->addAction("Open &avatar in browser", this, [avatarUrl] {
                QDesktopServices::openUrl(QUrl(avatarUrl));
            });
            menu->addAction("Copy a&vatar link", this, [avatarUrl] {
                crossPlatformCopy(avatarUrl);
            });
        }
        if (this->usercardActionAvailable())
        {
            if (!menu->isEmpty())
            {
                menu->addSeparator();
            }
            menu->addAction("Open chat in a new &tab", this,
                            this->actionForCurrentTarget([this] {
                                this->openUserChannelInNewTab();
                            }));
            menu->addAction("Open TikTok &profile", this,
                            this->actionForCurrentTarget([this] {
                                this->openPlatformUsercard();
                            }));
        }
        if (menu->isEmpty())
        {
            menu->deleteLater();
            return {};
        }
        menu->popup(QCursor::pos());
        menu->raise();
        return {};
    }
    if (this->isYouTube_)
    {
        menu->addAction("Open &avatar in browser", this, [avatarUrl] {
            openYouTubeUrl(QUrl(avatarUrl));
        });
        menu->addAction("Copy a&vatar link", this, [avatarUrl] {
            crossPlatformCopy(avatarUrl);
        });
        const auto channelUrl = youtubeChannelUrl(this->userId_);
        menu->addSeparator();
        menu->addAction("Open YouTube channel in &browser", this,
                        [channelUrl] { openYouTubeUrl(QUrl(channelUrl)); });
        menu->popup(QCursor::pos());
        menu->raise();
        return {};
    }

    auto channelURL = QUrl("https://www.twitch.tv/" + this->userName_.toLower());

    menu->addAction("Open &avatar in browser", this, [avatarUrl] {
        QDesktopServices::openUrl(QUrl(avatarUrl));
    });

    menu->addAction("Copy a&vatar link", this, [avatarUrl] {
        crossPlatformCopy(avatarUrl);
    });

    auto loginName = this->userName_.toLower();
    menu->addAction("Open channel in a new &popup window", this, [loginName] {
        auto *app = getApp();
        auto &window =
            app->getWindows()->createWindow(WindowType::Popup, true);
        auto *split =
            window.getNotebook().getOrAddSelectedPage()->appendNewSplit(false);
        split->setChannel(app->getTwitch()->getOrAddChannel(loginName));
    });

    menu->addAction("Open channel in a new &tab", this, [loginName] {
        ChannelPtr channel = getApp()->getTwitch()->getOrAddChannel(loginName);
        auto &notebook = getApp()->getWindows()->getMainWindow().getNotebook();
        SplitContainer *container = notebook.addPage(true);
        Split *split = new Split(container);
        split->setChannel(channel);
        container->insertSplit(split);
    });

    menu->addAction("Open channel in &browser", this, [channelURL] {
        QDesktopServices::openUrl(channelURL);
    });

    this->appendCommonProfileActions(menu);

    menu->popup(QCursor::pos());
    menu->raise();
    return {};
}

bool UserInfoPopup::canShowRoleManagementMenu() const
{
    if (!getSettings()->showUsercardRoleManagementMenu || this->isKick_ ||
        this->isYouTube_ || this->isTikTok_ || this->userName_.isEmpty() ||
        !this->underlyingChannel_)
    {
        return false;
    }

    auto *twitchChannel =
        dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get());
    if (twitchChannel == nullptr || twitchChannel->roomId().isEmpty())
    {
        return false;
    }

    const bool isMyself =
        getApp()
            ->getAccounts()
            ->twitch.getCurrent()
            ->getUserName()
            .compare(this->userName_, Qt::CaseInsensitive) == 0;
    const bool isChannelOwner =
        this->userName_.compare(twitchChannel->getName(),
                                Qt::CaseInsensitive) == 0;
    if (isMyself || isChannelOwner || this->isBroadcaster_)
    {
        return false;
    }

    const auto auth = MoltorinoAuth::resolveSavedBroadcasterToken(
        twitchChannel->roomId(), twitchChannel->getName());
    return auth.hasToken();
}

void UserInfoPopup::showRoleManagementMenu(QWidget *anchor)
{
    if (!this->canShowRoleManagementMenu())
    {
        return;
    }
    if (anchor == nullptr)
    {
        anchor = this->ui_.rolesLabel;
    }
    if (anchor == nullptr)
    {
        return;
    }

    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    const auto addAction = [this, menu](const QString &title,
                                        const QString &command,
                                        const QString &actionText) {
        menu->addAction(title, this, this->actionForCurrentTarget(
            [this, command, actionText] {
                this->runRoleManagementCommand(command, actionText);
            }));
    };

    addAction("Add lead moderator", "/leadmod", "add lead moderator to");
    addAction("Remove lead moderator", "/unleadmod",
              "remove lead moderator from");
    menu->addSeparator();
    addAction("Add editor", "/editor", "add editor to");
    addAction("Remove editor", "/uneditor", "remove editor from");

    menu->popup(anchor->mapToGlobal(QPoint(0, anchor->height())));
    menu->raise();
}

void UserInfoPopup::runRoleManagementCommand(const QString &command,
                                             const QString &actionText)
{
    if (!this->underlyingChannel_ || this->userName_.isEmpty())
    {
        return;
    }

    const auto self = QPointer<UserInfoPopup>(this);
    const auto generation = this->userDataRequestGeneration_;
    const bool wasPinned = this->ensurePinned();
    auto reply = QMessageBox::warning(
        this, "Confirm role change",
        QString("Are you sure you want to %1 %2 in #%3?")
            .arg(actionText, this->userName_,
                 this->underlyingChannel_->getName()),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (!self)
    {
        return;
    }
    if (wasPinned)
    {
        this->togglePinned();
    }
    if (reply != QMessageBox::Yes ||
        generation != this->userDataRequestGeneration_)
    {
        return;
    }

    auto value = command + ' ' + this->userName_;
    value = getApp()->getCommands()->execCommand(value,
                                                 this->underlyingChannel_,
                                                 false);
    if (!value.isEmpty())
    {
        this->underlyingChannel_->sendMessage(value);
    }
}

void UserInfoPopup::updateUsercardStatusIcons()
{
    const auto boxSize = std::max(1, qRound(15 * this->scale()));
    const auto iconSize = std::max(1, qRound(14 * this->scale()));
    const bool isLight = getApp()->getThemes()->isLightTheme();
    const auto iconScale = this->devicePixelRatioF();

    auto updateIcon = [boxSize, iconScale](QLabel *label, const QString &path,
                                           int iconSize) {
        if (label == nullptr)
        {
            return;
        }

        label->setFixedSize(boxSize, boxSize);
        label->setAlignment(Qt::AlignCenter);
        label->setPixmap(renderUsercardStatusIcon(path, iconSize, iconScale));
    };

    auto updateColorSwatch = [this] {
        if (this->ui_.userColorSwatch == nullptr ||
            this->ui_.userColorRow == nullptr)
        {
            return;
        }

        auto colorText =
            this->ui_.userColorRow->property("copy-color").toString();
        if (colorText.isEmpty())
        {
            colorText =
                this->ui_.userColorRow->property("swatch-color").toString();
        }

        const QColor color(colorText);
        const auto swatchSize = std::max(1, qRound(8 * this->scale()));
        this->ui_.userColorSwatch->setFixedSize(swatchSize, swatchSize);
        if (color.isValid())
        {
            this->ui_.userColorSwatch->setStyleSheet(
                QString("QFrame#UsercardColorSwatch { background: %1; "
                        "border-radius: %2px; }")
                    .arg(color.name(QColor::HexRgb))
                    .arg(std::max(1, qRound(2 * this->scale()))));
        }
        else
        {
            this->ui_.userColorSwatch->setStyleSheet({});
        }
    };

    updateIcon(this->ui_.followageIcon,
               isLight ? ":/buttons/usercardFollow-lightMode.svg"
                       : ":/buttons/usercardFollow-darkMode.svg",
               iconSize);
    updateIcon(this->ui_.subageIcon,
               isLight ? ":/buttons/usercardSub-lightMode.svg"
                       : ":/buttons/usercardSub-darkMode.svg",
               iconSize);
    updateColorSwatch();
}

void UserInfoPopup::setFollowage(const QDateTime &followedAt)
{
    const bool visible = getSettings()->showUsercardFollowage &&
                         followedAt.isValid();
    const bool changed = visible != !this->ui_.followageRow->isHidden();
    this->ui_.followageLabel->setText({});
    this->ui_.followageLabel->setToolTip({});
    if (visible)
    {
        const auto date = followedAt.date();
        const auto relative = getSettings()->showUsercardFollowageRelativeTime
                                  ? formatUsercardFollowRelativeTime(date)
                                  : QString{};
        this->ui_.followageLabel->setText(
            "Following since " + date.toString(Qt::ISODate) + relative);
        this->ui_.followageLabel->setToolTip(
            formatLongFriendlyDuration(followedAt,
                                       QDateTime::currentDateTimeUtc()) +
            u" ago"_s);
        this->ui_.followageLabel->setMouseTracking(true);
        this->updateUsercardStatusIcons();
    }
    this->ui_.followageRow->setVisible(visible);
    this->ui_.followageIcon->setVisible(visible);
    if (changed)
    {
        this->applyPopupSize(this->sizeHint());
    }
}

void UserInfoPopup::loadTwitchFollowage(bool onlyIfChanged)
{
    if (this->isKick_ || this->isYouTube_ || this->isTikTok_)
    {
        return;
    }
    const auto clear = [this] {
        ++this->followageRequestGeneration_;
        this->followageRequestKey_.clear();
        this->setFollowage({});
    };
    auto *channel = dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get());
    if (!getSettings()->showUsercardFollowage || !channel ||
        channel->roomId().isEmpty() || this->userId_.isEmpty())
    {
        clear();
        return;
    }
    auto auth = MoltorinoAuth::resolveModerationToken(channel->roomId(),
                                                    channel->getName());

    if (!auth.hasToken() || auth.clientId.isEmpty())
    {
        const auto current = getApp()->getAccounts()->twitch.getCurrent();
        if (!current || current->isAnon() || !channel->hasModRights())
        {
            clear();
            return;
        }
        auth.token = current->getOAuthToken();
        auth.clientId = current->getOAuthClient();
    }

    const QStringList key{channel->roomId(), this->userId_, auth.clientId,
                          auth.token};
    if (onlyIfChanged && this->followageRequestKey_ == key)
    {
        return;
    }
    this->followageRequestKey_ = key;
    const auto generation = ++this->followageRequestGeneration_;
    this->setFollowage({});
    const auto self = QPointer<UserInfoPopup>(this);
    const auto dataGeneration = this->userDataRequestGeneration_;
    getHelix()->getChannelFollowDate(
        channel->roomId(), this->userId_, auth.clientId, auth.token, this,
        [self, generation, dataGeneration](const QDateTime &date) {
            if (self && self->followageRequestGeneration_ == generation &&
                self->userDataRequestGeneration_ == dataGeneration)
            {
                self->setFollowage(date);
            }
        },
        [] {});
}

void UserInfoPopup::resetUsercardInfoRows()
{
    ++this->followageRequestGeneration_;
    this->followageRequestKey_.clear();
    if (this->isYouTube_ || this->isTikTok_)
    {
        this->ui_.followerCountLabel->hide();
        this->ui_.createdDateLabel->hide();
        this->ui_.followageRow->hide();
        this->ui_.subageRow->hide();
        this->ui_.chatterCountLabel->hide();
        this->ui_.lastLiveLabel->hide();
        this->ui_.userColorRow->hide();
        this->ui_.statusLabel->setText(this->platformRoles_);
        this->ui_.statusLabel->setVisible(this->isTikTok_ &&
                                          !this->platformRoles_.isEmpty());
        this->ui_.bannedAvatarLabel->hide();
        return;
    }

    auto *settings = getSettings();
    const bool showTwitchProfileRows = !this->isKick_;

    this->ui_.followerCountLabel->setText(TEXT_FOLLOWERS.arg(""));
    this->ui_.followerCountLabel->setVisible(
        settings->showUsercardFollowerCount);

    this->ui_.createdDateLabel->setText(TEXT_CREATED.arg(""));
    this->ui_.createdDateLabel->setToolTip({});
    this->ui_.createdDateLabel->setVisible(settings->showUsercardCreatedDate);

    this->ui_.followageLabel->setText({});
    this->ui_.followageLabel->setToolTip({});
    this->ui_.followageRow->hide();
    this->ui_.followageIcon->setVisible(false);

    this->ui_.subageLabel->setText({});
    this->ui_.subageRow->hide();
    this->ui_.subageIcon->setVisible(false);

    this->ui_.chatterCountLabel->setText("Chatters: ...");
    this->ui_.chatterCountLabel->setVisible(
        showTwitchProfileRows && settings->showUsercardChatterCount);
    this->ui_.lastLiveLabel->setText("Last live: 0000-00-00");
    this->ui_.lastLiveLabel->setToolTip({});
    this->ui_.lastLiveLabel->setVisible(
        showTwitchProfileRows && settings->showUsercardLastLive);
    this->ui_.statusLabel->setText("Status: ...");
    this->ui_.statusLabel->setVisible(showTwitchProfileRows &&
                                      settings->showUsercardStatus);
    this->ui_.bannedAvatarLabel->hide();

    this->updateUsercardColor();
}

void UserInfoPopup::updateUsercardColor()
{
    const auto color = this->apiUserColor_.isValid()
                           ? this->apiUserColor_
                           : this->messageUserColor_;
    this->identityUserColor_ = color;
    static_cast<UsercardPaintButton *>(this->ui_.identityPaint)
        ->setUserColor(color);

    const bool visible = !this->isYouTube_ && !this->isTikTok_ &&
                         !this->isKick_ && getSettings()->showUsercardColor;
    this->ui_.userColorRow->setVisible(visible);
    if (visible && color.isValid())
    {
        const auto colorHex = color.name(QColor::HexRgb).toUpper();
        this->ui_.userColorRow->setProperty("copy-color", colorHex);
        this->ui_.userColorRow->setProperty("swatch-color", colorHex);
        this->ui_.userColorLabel->setText("Color: " + colorHex);
    }
    else if (visible)
    {
        this->ui_.userColorRow->setProperty("copy-color", {});
        this->ui_.userColorRow->setProperty("swatch-color", {});
        this->ui_.userColorLabel->setText(
            "Color: " % (this->apiUserColorLookupFinished_ ? TEXT_UNAVAILABLE
                                                           : TEXT_LOADING));
    }
    this->updateUsercardStatusIcons();
}

void UserInfoPopup::applyIvrUserProfile(const IvrUserProfile &profile)
{
    auto *settings = getSettings();

    this->ui_.bannedAvatarLabel->setVisible(profile.banned);

    this->apiUserColor_ = QColor(profile.chatColor);
    this->apiUserColorLookupFinished_ = true;
    this->updateUsercardColor();

    if (settings->showUsercardChatterCount)
    {
        this->ui_.chatterCountLabel->setText(profile.chatterCount
                                                 ? "Chatters: " +
                                                       localizeNumbers(
                                                           *profile.chatterCount)
                                                 : "Chatters: " %
                                                       TEXT_UNAVAILABLE);
        this->ui_.chatterCountLabel->setVisible(true);
    }

    if (settings->showUsercardLastLive)
    {
        const auto lastLive = formatIvrDate(profile.lastBroadcastStartedAt);
        if (lastLive.isEmpty())
        {
            this->ui_.lastLiveLabel->setText("Last live: " %
                                             TEXT_UNAVAILABLE);
            this->ui_.lastLiveLabel->setToolTip({});
        }
        else
        {
            this->ui_.lastLiveLabel->setText("Last live: " + lastLive);
            if (!profile.lastBroadcastTitle.isEmpty())
            {
                this->ui_.lastLiveLabel->setToolTip(
                    profile.lastBroadcastTitle);
                this->ui_.lastLiveLabel->setMouseTracking(true);
            }
        }
        this->ui_.lastLiveLabel->setVisible(true);
    }

    if (settings->showUsercardStatus)
    {
        this->ui_.statusLabel->setText("Status: " +
                                       formatUsercardStatus(profile));
        this->ui_.statusLabel->setVisible(true);
    }
}

void UserInfoPopup::refreshSevenTVUserButtonVisibility()
{
    if (this->ui_.sevenTVUserLabel == nullptr)
    {
        return;
    }
    if (this->isYouTube_ || this->isTikTok_)
    {
        this->ui_.sevenTVUserLabel->hide();
        return;
    }

    const bool settingEnabled = getSettings()->showSevenTVUsercardButton;
    const auto placement = usercardActionPlacement(
        getSettings()->usercardSevenTVActionPlacement.getValue());
    const bool hasSevenTVUser = !this->seventvUserID_.isEmpty();
    const bool lookupPending =
        !this->seventvUserLookupFinished_ && !hasSevenTVUser;
    const bool shouldShow =
        settingEnabled && placement == UsercardActionPlacement::Bar &&
        (lookupPending || hasSevenTVUser);

    this->ui_.sevenTVUserLabel->setVisible(shouldShow);
    this->ui_.sevenTVUserLabel->setEnabled(settingEnabled && hasSevenTVUser);

    if (hasSevenTVUser)
    {
        this->ui_.sevenTVUserLabel->setToolTip("Open 7TV profile");
    }
    else if (this->seventvUserLookupInFlight_)
    {
        this->ui_.sevenTVUserLabel->setToolTip("Checking 7TV profile...");
    }
    else if (this->seventvUserLookupFinished_)
    {
        this->ui_.sevenTVUserLabel->setToolTip("No 7TV profile found");
    }
    else
    {
        this->ui_.sevenTVUserLabel->setToolTip("7TV profile not loaded yet");
    }
}

void UserInfoPopup::resetNameHistory()
{
    ++this->nameHistoryRequestGeneration_;
    if (this->nameHistoryMenu_ != nullptr)
    {
        this->nameHistoryMenu_->close();
        this->nameHistoryMenu_ = nullptr;
    }
    this->nameHistoryLogin_.clear();
    this->nameHistoryEntries_.clear();
    this->nameHistoryLoading_ = false;
    this->nameHistoryLoaded_ = false;
    this->applyCachedNameHistory();
    this->updateNameHistoryButton();
}

bool UserInfoPopup::applyCachedNameHistory()
{
    if (this->userName_.isEmpty() || this->userId_.isEmpty() || this->isKick_ ||
        this->isYouTube_ || this->isTikTok_)
    {
        return false;
    }

    const auto login = normalizeTwitchNameHistoryLogin(this->userName_);
    if (login.isEmpty())
    {
        return false;
    }

    auto cached = getCachedTwitchNameHistory(this->userId_, login);
    if (!cached)
    {
        return false;
    }

    this->nameHistoryLogin_ = login;
    this->nameHistoryEntries_ = cached->entries;
    this->nameHistoryLoading_ = false;
    this->nameHistoryLoaded_ = true;
    return true;
}

void UserInfoPopup::updateNameHistoryButton()
{
    if (this->ui_.nameHistoryButton == nullptr)
    {
        return;
    }

    const bool canShow = getSettings()->showUsercardNameHistoryButton &&
                         !this->isKick_ && !this->isYouTube_ &&
                         !this->isTikTok_ && !this->userName_.isEmpty();
    this->ui_.nameHistoryButton->setVisible(canShow);
    this->ui_.nameHistoryButton->setEnabled(canShow &&
                                            !this->userId_.isEmpty());
    this->ui_.nameHistoryButton->setText(this->nameHistoryLoading_ ? "..."
                                                                    : "aka");

    if (!canShow)
    {
        if (this->nameHistoryMenu_ != nullptr)
        {
            this->nameHistoryMenu_->close();
            this->nameHistoryMenu_ = nullptr;
        }
        this->ui_.nameHistoryButton->setToolTip({});
        return;
    }
    if (this->userId_.isEmpty())
    {
        this->ui_.nameHistoryButton->setToolTip(
            "Name history loads after Twitch profile data.");
        return;
    }
    if (this->nameHistoryLoading_)
    {
        this->ui_.nameHistoryButton->setToolTip("Fetching name history...");
        return;
    }
    const auto login = normalizeTwitchNameHistoryLogin(this->userName_);
    if (this->nameHistoryLoaded_ && this->nameHistoryLogin_ == login &&
        this->nameHistoryEntries_.empty())
    {
        this->ui_.nameHistoryButton->setToolTip("No name history found");
        return;
    }

    this->ui_.nameHistoryButton->setToolTip("Show name history");
}

void UserInfoPopup::showNameHistoryMenu()
{
    if (this->ui_.nameHistoryButton == nullptr || this->userName_.isEmpty() ||
        this->userId_.isEmpty() || this->isKick_ || this->isYouTube_ ||
        this->isTikTok_)
    {
        return;
    }
    if (this->nameHistoryLoading_)
    {
        this->openNameHistoryMenu("Fetching name history...");
        return;
    }

    if (this->applyCachedNameHistory())
    {
        this->updateNameHistoryButton();
        this->openNameHistoryMenu();
        return;
    }

    const auto login = normalizeTwitchNameHistoryLogin(this->userName_);
    if (this->nameHistoryLoaded_ && this->nameHistoryLogin_ == login)
    {
        this->openNameHistoryMenu();
        return;
    }

    this->requestNameHistory();
}

void UserInfoPopup::openNameHistoryMenu(const QString &statusText)
{
    auto *button = this->ui_.nameHistoryButton;
    if (button == nullptr || !button->isVisible())
    {
        return;
    }

    if (this->nameHistoryMenu_ != nullptr)
    {
        this->nameHistoryMenu_->close();
    }

    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    this->nameHistoryMenu_ = menu;

    if (!statusText.isEmpty())
    {
        auto *status = menu->addAction(statusText);
        status->setEnabled(false);
    }
    else if (this->nameHistoryEntries_.empty())
    {
        auto *empty = menu->addAction("No name history found");
        empty->setEnabled(false);
    }
    else
    {
        for (const auto &entry : this->nameHistoryEntries_)
        {
            auto *action = new QWidgetAction(menu);
            action->setDefaultWidget(new NameHistoryMenuRow(
                entry.login, entry.leftText, entry.rightText, menu));
            menu->addAction(action);
        }

        if (static_cast<int>(this->nameHistoryEntries_.size()) >=
            TWITCH_NAME_HISTORY_LIMIT)
        {
            menu->addSeparator();
            auto *limited =
                menu->addAction(QString("Showing latest %1 names")
                                    .arg(TWITCH_NAME_HISTORY_LIMIT));
            limited->setEnabled(false);
        }
    }

    menu->popup(button->mapToGlobal(QPoint(0, button->height())));
}

void UserInfoPopup::requestNameHistory()
{
    if (this->userName_.isEmpty() || this->userId_.isEmpty() || this->isKick_ ||
        this->isYouTube_ || this->isTikTok_)
    {
        return;
    }

    const auto login = normalizeTwitchNameHistoryLogin(this->userName_);
    if (login.isEmpty())
    {
        return;
    }

    const auto generation = ++this->nameHistoryRequestGeneration_;
    const auto userId = this->userId_;
    this->nameHistoryLogin_ = login;
    this->nameHistoryEntries_.clear();
    this->nameHistoryLoading_ = true;
    this->nameHistoryLoaded_ = false;
    this->updateNameHistoryButton();
    this->openNameHistoryMenu("Fetching name history...");

    const QPointer<UserInfoPopup> self(this);

    fetchTwitchNameHistoryByUserId(
        userId, login,
        [self, generation, userId, login](TwitchNameHistory history) mutable {
            if (!self ||
                generation != self->nameHistoryRequestGeneration_ ||
                self->userId_ != userId ||
                normalizeTwitchNameHistoryLogin(self->userName_) != login)
            {
                return;
            }

            self->nameHistoryLogin_ = login;
            self->nameHistoryEntries_ = std::move(history.entries);
            self->nameHistoryLoading_ = false;
            self->nameHistoryLoaded_ = true;
            self->updateNameHistoryButton();
            self->openNameHistoryMenu();
        },
        [self, generation, userId, login](const QString &error) {
            if (!self ||
                generation != self->nameHistoryRequestGeneration_ ||
                self->userId_ != userId ||
                normalizeTwitchNameHistoryLogin(self->userName_) != login)
            {
                return;
            }

            qCWarning(chatterinoWidget)
                << "Failed to fetch name history:" << error;
            self->nameHistoryEntries_.clear();
            self->nameHistoryLoading_ = false;
            self->nameHistoryLoaded_ = false;
            self->updateNameHistoryButton();
            self->openNameHistoryMenu("Name history unavailable");
        });
}

QStringView UserInfoPopup::platformName() const
{
    if (this->isTikTok_)
    {
        return u"TikTok";
    }
    if (this->isYouTube_)
    {
        return u"YouTube";
    }
    if (this->isKick_)
    {
        return u"Kick";
    }
    return u"Twitch";
}

void UserInfoPopup::updateIdentityStripVisibility()
{
    if (!this->ui_.identityBadgeRow || !this->ui_.identityBadges ||
        !this->ui_.identityPaintRow || !this->ui_.identityPaint)
    {
        return;
    }

    const auto *badges =
        static_cast<UsercardBadgeStrip *>(this->ui_.identityBadges);
    const auto *paint =
        static_cast<UsercardPaintButton *>(this->ui_.identityPaint);
    const bool badgesVisible =
        getSettings()->showUsercardBadges && !badges->empty();
    const bool paintVisible = !this->isYouTube_ && !this->isTikTok_ &&
                              getSettings()->showUsercardSevenTVPaint &&
                              paint->paint() != nullptr;
    this->ui_.identityBadgeRow->setVisible(badgesVisible);
    this->ui_.identityPaintRow->setVisible(paintVisible);
    this->ui_.identityBadgeRow->updateGeometry();
    this->ui_.identityPaintRow->updateGeometry();
}

void UserInfoPopup::refreshIdentityBadges()
{
    auto *strip = static_cast<UsercardBadgeStrip *>(this->ui_.identityBadges);
    std::vector<UsercardBadgeStrip::Badge> badges;
    const bool collectBadges = getSettings()->showUsercardBadges;
    const bool needsPaintColor = !this->isYouTube_ && !this->isTikTok_ &&
                                 getSettings()->showUsercardSevenTVPaint &&
                                 getSettings()->displaySevenTVPaints;
    const bool needsMessageColor = !this->isYouTube_ && !this->isTikTok_ &&
                                   !this->isKick_ &&
                                   getSettings()->showUsercardColor;

    MessagePtr identityMessage = this->identityMessageFallback_;
    if ((collectBadges || needsPaintColor || needsMessageColor) &&
        this->underlyingChannel_ && !this->userName_.isEmpty())
    {
        const auto snapshot = this->underlyingChannel_->getMessageSnapshot();
        for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it)
        {
            const auto &message = *it;
            bool matchesTarget = false;
            if (message)
            {
                const bool matchingLogin =
                    !message->loginName.isEmpty() &&
                    message->loginName.compare(this->userName_,
                                               Qt::CaseInsensitive) == 0;
                if (this->isYouTube_ || this->isTikTok_)
                {
                    matchesTarget =
                        message->platform == this->targetMessagePlatform() &&
                        !this->userId_.isEmpty() &&
                        message->userID == this->userId_;
                }
                else if (this->isKick_ || this->userId_.isEmpty() ||
                         message->userID.isEmpty())
                {
                    matchesTarget = matchingLogin;
                }
                else
                {
                    matchesTarget = message->userID == this->userId_;
                }
            }
            if (!matchesTarget ||
                message->flags.hasAny(
                    {MessageFlag::System, MessageFlag::Timeout,
                     MessageFlag::Subscription, MessageFlag::Whisper,
                     MessageFlag::ModerationAction, MessageFlag::ClearChat}))
            {
                continue;
            }

            if (message->usernameColor.isValid())
            {
                this->messageUserColor_ = message->usernameColor;
            }
            identityMessage = message;
            break;
        }
    }

    if (collectBadges && identityMessage)
    {
        const auto wordFlags = getApp()->getWindows()->getWordFlags();
        const auto appendBadge = [&badges, &wordFlags](
                                     const EmotePtr &emote,
                                     MessageElementFlag flag,
                                     QString tooltip = {}) {
            const MessageElementFlags flags{flag};
            if (!emote || !wordFlags.hasAny(flags))
            {
                return;
            }
            if (tooltip.isEmpty())
            {
                tooltip = emote->tooltip.string;
            }
            badges.push_back({emote, std::move(tooltip), flags});
        };

        if (!this->isYouTube_ && !this->isTikTok_ && !this->isKick_)
        {
            const auto *twitchChannel =
                dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get());
            if (twitchChannel != nullptr)
            {
                QHash<QString, QString> badgeInfos;
                for (const auto &[key, value] :
                     identityMessage->twitchBadgeInfos)
                {
                    badgeInfos.insert(key, value);
                }

                const auto appendTwitchBadge =
                    [this, &appendBadge, &badgeInfos, twitchChannel](
                        const TwitchBadge &badge, const QString &sourceName) {
                    EmotePtr emote;
                    if (badge.key_ == u"moderator" &&
                        getSettings()->useCustomFfzModeratorBadges)
                    {
                        if (const auto custom =
                                twitchChannel->ffzCustomModBadge())
                        {
                            emote = *custom;
                        }
                    }
                    else if (badge.key_ == u"vip" &&
                             getSettings()->useCustomFfzVipBadges)
                    {
                        if (const auto custom =
                                twitchChannel->ffzCustomVipBadge())
                        {
                            emote = *custom;
                        }
                    }

                    if (!emote)
                    {
                        if (const auto channelBadge = twitchChannel->twitchBadge(
                                badge.key_, badge.value_))
                        {
                            emote = *channelBadge;
                        }
                        else if (const auto globalBadge =
                                     getApp()->getTwitchBadges()->badge(
                                         badge.key_, badge.value_))
                        {
                            emote = *globalBadge;
                        }
                    }
                    if (!emote)
                    {
                        return;
                    }

                    auto tooltip = emote->tooltip.string;
                    if (badge.key_ == u"bits")
                    {
                        tooltip = QString("Twitch cheer %1").arg(badge.value_);
                    }
                    else if (badge.flag_ ==
                             MessageElementFlag::BadgeSubscription)
                    {
                        const auto info = badgeInfos.value(badge.key_);
                        if (!info.isEmpty())
                        {
                            const auto tier = badge.value_.length() > 3
                                                  ? badge.value_.at(0)
                                                  : QChar('1');
                            tooltip += QString(" (%1%2 months)")
                                           .arg(tier != QChar('1')
                                                    ? QString("Tier %1, ")
                                                          .arg(tier)
                                                    : QString{})
                                           .arg(info);
                        }
                    }
                    else if (badge.flag_ ==
                             MessageElementFlag::BadgePredictions)
                    {
                        const auto info = badgeInfos.value(badge.key_);
                        if (!info.isEmpty())
                        {
                            tooltip = QString("Predicted %1")
                                          .arg(parseTagString(info).replace(
                                              QChar(0x2E1D), QChar(',')));
                        }
                    }
                    if (!sourceName.isEmpty())
                    {
                        tooltip = QStringLiteral("%1 (%2)")
                                      .arg(tooltip, sourceName);
                    }
                    appendBadge(emote, badge.flag_, std::move(tooltip));
                };

                if (identityMessage->sharedChatSourceBadges)
                {
                    QString sourceName;
                    if (!identityMessage->sharedChatSourceId.isEmpty())
                    {
                        const auto sourceUser =
                            getApp()->getTwitchUsers()->resolveID(
                                {identityMessage->sharedChatSourceId});
                        sourceName = sourceUser->displayName;
                        if (sourceName.isEmpty())
                        {
                            sourceName = sourceUser->name;
                        }
                    }
                    for (const auto &badge :
                         *identityMessage->sharedChatSourceBadges)
                    {
                        appendTwitchBadge(badge, sourceName);
                    }
                }
                for (const auto &badge : identityMessage->twitchBadges)
                {
                    appendTwitchBadge(badge, {});
                }

                const auto userID =
                    !this->userId_.isEmpty() ? this->userId_
                                             : identityMessage->userID;
                if (!userID.isEmpty())
                {
                    if (const auto badge =
                            getApp()->getChatterinoBadges()->getBadge({userID}))
                    {
                        appendBadge(*badge,
                                    MessageElementFlag::BadgeChatterino);
                    }
                    for (const auto &badge :
                         getApp()->getFfzBadges()->getUserBadges({userID}))
                    {
                        appendBadge(badge.emote, MessageElementFlag::BadgeFfz);
                    }
                    for (const auto &badge :
                         twitchChannel->ffzChannelBadges(userID))
                    {
                        appendBadge(badge.emote, MessageElementFlag::BadgeFfz);
                    }
                    if (auto *provider = getApp()->getFfzApBadges())
                    {
                        if (const auto badge = provider->getBadge({userID}))
                        {
                            appendBadge(badge->emote,
                                        MessageElementFlag::BadgeFfzAp);
                        }
                    }
                    if (const auto badge =
                            getApp()->getBttvBadges()->getBadge({userID}))
                    {
                        appendBadge(*badge, MessageElementFlag::BadgeBttv);
                    }
                    if (auto *provider = getApp()->getBluzyrinoBadges())
                    {
                        for (const auto &badge : provider->getBadges({userID}))
                        {
                            appendBadge(badge,
                                        MessageElementFlag::BadgeBluzyrino);
                        }
                    }
                    if (auto *provider =
                            getApp()->getMoltorinoSupporterBadges())
                    {
                        for (const auto &badge : provider->getBadges(userID))
                        {
                            if (!badge.emote)
                            {
                                continue;
                            }
                            appendBadge(badge.emote,
                                        MessageElementFlag::BadgeMoltorino);

                            break;
                        }
                    }
                    if (const auto badge =
                            getApp()->getSeventvBadges()->getBadge({userID}))
                    {
                        appendBadge(*badge, MessageElementFlag::BadgeSevenTV);
                    }
                    if (auto *provider = getApp()->getHomiesBadges())
                    {
                        const auto assigned = provider->getBadges(userID);
                        for (size_t index = 0; index < assigned.size(); ++index)
                        {
                            appendBadge(
                                assigned[index],
                                index == 0
                                    ? MessageElementFlag::BadgeHomiesCustom
                                    : MessageElementFlag::BadgeHomiesSupporter);
                        }
                    }
                    if (auto *provider = getApp()->getJilChatBadges())
                    {
                        for (const auto &badge : provider->getBadges({userID}))
                        {
                            appendBadge(badge,
                                        MessageElementFlag::BadgeJilChat);
                        }
                    }
                }
            }
        }
        else
        {
            for (const auto &element : identityMessage->elements)
            {
                const auto *badge =
                    dynamic_cast<const BadgeElement *>(element.get());
                if (!badge || !badge->getEmote())
                {
                    continue;
                }

                const auto flags = badge->getFlags();
                if (!flags.hasAny(MessageElementFlag::Badges) ||
                    flags.has(MessageElementFlag::BadgeSharedChannel) ||
                    !wordFlags.hasAny(flags))
                {
                    continue;
                }
                const auto *tiktokBadge =
                    dynamic_cast<const TikTokBadgeElement *>(badge);
                badges.push_back({badge->getEmote(), badge->getTooltip(), flags,
                                  tiktokBadge ? tiktokBadge->artwork() : nullptr});
            }
        }
    }

    strip->setBadges(std::move(badges));
    this->updateUsercardColor();
    this->updateIdentityStripVisibility();
}

void UserInfoPopup::refreshIdentityPaint()
{
    auto *button =
        static_cast<UsercardPaintButton *>(this->ui_.identityPaint);
    std::shared_ptr<Paint> paint;
    if (!this->isYouTube_ && !this->isTikTok_ &&
        getSettings()->showUsercardSevenTVPaint &&
        getSettings()->displaySevenTVPaints && !this->userName_.isEmpty())
    {
        paint = getApp()->getSeventvPaints()->getPaint(
            this->userName_.toLower(), this->isKick_);
    }

    const bool visible =
        paint && !paint->getName().isEmpty() && !paint->id.isEmpty();
    button->setPaint(visible ? std::move(paint) : nullptr);
    button->setUserColor(this->identityUserColor_);
    button->setVisible(visible);
    this->updateIdentityStripVisibility();
}

void UserInfoPopup::updateFollowButtonAppearance()
{
    auto *button = this->ui_.followButton;
    if (button == nullptr)
    {
        return;
    }

    if (this->ui_.avatarButton != nullptr &&
        this->ui_.avatarButton->scaleIndependentWidth() > 0)
    {
        const auto avatarWidth = qRound(
            this->ui_.avatarButton->scaleIndependentWidth() * this->scale());
        button->setFixedWidth(std::max(1, avatarWidth));
    }

    const auto hoverColor =
        this->following_ ? QColor("#d95757") : QColor("#9146ff");
    const auto rgba = [](const QColor &color, int alpha) {
        return QStringLiteral("rgba(%1, %2, %3, %4)")
            .arg(color.red())
            .arg(color.green())
            .arg(color.blue())
            .arg(alpha);
    };

    button->setFixedHeight(std::max(1, qRound(24 * this->scale())));
    button->setFont(getApp()->getFonts()->getFont(FontStyle::UiMedium,
                                                  this->scale()));
    button->setStyleSheet(
        QStringLiteral(
            "QPushButton { background-color: transparent; color: %1; "
            "border: 1px solid transparent; padding: 0; }"
            "QPushButton:hover { background-color: %2; color: %3; }"
            "QPushButton:pressed { background-color: %4; }"
            "QPushButton:focus { border-color: %5; }"
            "QPushButton:disabled { background-color: transparent; color: %6; "
            "border-color: transparent; }")
            .arg(this->theme->window.text.name(QColor::HexRgb),
                 rgba(hoverColor, 32), hoverColor.name(QColor::HexRgb),
                 rgba(hoverColor, 52), rgba(hoverColor, 150),
                 this->theme->messages.disabled.name(QColor::HexRgb)));
}

void UserInfoPopup::refreshFollowButton()
{
    auto *button = this->ui_.followButton;
    if (button == nullptr)
    {
        return;
    }

    if (this->isTikTok_ || this->isYouTube_)
    {
        button->hide();
        return;
    }

    const auto account = getApp()->getAccounts()->twitch.getCurrent();
    const auto actionAuth = MoltorinoAuth::resolveSelectedUserToken();
    const bool available =
        getSettings()->showFollowButtonInUsercard && !this->isKick_ &&
        account &&
        !account->isAnon() && !this->userId_.isEmpty() &&
        !this->userName_.isEmpty() && account->getUserId() != this->userId_ &&
        actionAuth.hasToken();
    button->setVisible(available);
    if (!available)
    {
        this->followStatusKnown_ = false;
        this->followStatusRequestInFlight_ = false;
        this->followMutationInFlight_ = false;
        return;
    }

    if (const auto cached = detail::cachedFollowingStatus(
            account->getUserId(), this->userId_))
    {
        this->followStatusKnown_ = true;
        this->following_ = *cached;
    }

    button->setText(this->following_ ? "Unfollow" : "Follow");
    this->updateFollowButtonAppearance();
    button->setEnabled(this->followStatusKnown_ &&
                       !this->followMutationInFlight_);
    if (this->followMutationInFlight_)
    {
        button->setToolTip(QStringLiteral("Updating follow status..."));
    }
    else if (!this->followStatusKnown_)
    {
        button->setToolTip(QStringLiteral("Checking follow status..."));
    }
    else
    {
        button->setToolTip((this->following_ ? QStringLiteral("Unfollow %1")
                                            : QStringLiteral("Follow %1"))
                              .arg(this->userName_));
    }

    if (this->followStatusKnown_ || this->followStatusRequestInFlight_)
    {
        return;
    }

    this->followStatusRequestInFlight_ = true;
    const auto accountID = account->getUserId();
    const auto targetID = this->userId_;
    const auto self = QPointer<UserInfoPopup>(this);
    getHelix()->getFollowedChannel(
        accountID, targetID, this,
        [self, accountID, targetID](const auto &follow) {
            if (!self || self->isKick_ || self->isYouTube_ || self->isTikTok_ ||
                self->userId_ != targetID)
            {
                return;
            }
            const auto current = getApp()->getAccounts()->twitch.getCurrent();
            if (!current || current->getUserId() != accountID)
            {
                return;
            }

            self->followStatusRequestInFlight_ = false;
            self->followStatusKnown_ = true;
            self->following_ = follow.has_value();
            detail::rememberFollowingStatus(accountID, targetID,
                                            self->following_);
            self->refreshFollowButton();
        },
        [self, accountID, targetID](const QString &) {
            if (!self || self->userId_ != targetID)
            {
                return;
            }
            const auto current = getApp()->getAccounts()->twitch.getCurrent();
            if (!current || current->getUserId() != accountID)
            {
                return;
            }
            self->followStatusRequestInFlight_ = false;
            self->ui_.followButton->setText("Follow");
            self->following_ = false;
            self->updateFollowButtonAppearance();
            self->ui_.followButton->setEnabled(false);
            self->ui_.followButton->setToolTip(
                "Couldn't check follow status. Reopen the usercard to retry.");
        });
}

void UserInfoPopup::runFollowAction()
{
    if (!this->followStatusKnown_ || this->followMutationInFlight_ ||
        this->userId_.isEmpty() || this->isKick_ || this->isYouTube_ ||
        this->isTikTok_)
    {
        return;
    }

    QString authError;
    const auto auth = MoltorinoAuth::resolveSelectedUserToken(&authError);
    if (!auth.hasToken())
    {
        if (this->channel_)
        {
            this->channel_->addSystemMessage(
                authError.isEmpty()
                    ? MoltorinoAuth::authRequiredMessage("following users")
                    : authError);
        }
        return;
    }

    const bool unfollow = this->following_;
    const auto targetID = this->userId_;
    const auto targetName = this->userName_;
    const auto accountID = auth.userId;
    const auto self = QPointer<UserInfoPopup>(this);
    const auto generation = this->userDataRequestGeneration_;

    if (unfollow && getSettings()->confirmUnfollowFromSplitHeader)
    {
        const bool wasPinned = this->ensurePinned();
        const auto response = QMessageBox::question(
            this, "Unfollow user?",
            QString("Are you sure you want to unfollow %1?").arg(targetName),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (!self)
        {
            return;
        }
        if (wasPinned)
        {
            this->togglePinned();
        }
        if (response != QMessageBox::Yes ||
            generation != this->userDataRequestGeneration_)
        {
            return;
        }
    }

    this->followMutationInFlight_ = true;
    this->refreshFollowButton();

    auto success = [self, accountID, targetID, targetName, unfollow, generation] {
        detail::rememberFollowingStatus(accountID, targetID, !unfollow);
        if (!self || self->userId_ != targetID ||
            generation != self->userDataRequestGeneration_)
        {
            return;
        }
        const auto current = getApp()->getAccounts()->twitch.getCurrent();
        if (!current || current->getUserId() != accountID)
        {
            return;
        }

        self->followMutationInFlight_ = false;
        self->followStatusKnown_ = true;
        self->following_ = !unfollow;
        self->refreshFollowButton();
        if (self->channel_)
        {
            self->channel_->addSystemMessage(
                unfollow ? QString("You unfollowed %1.").arg(targetName)
                         : QString("You followed %1.").arg(targetName));
        }
    };
    auto failure = [self, accountID, targetID, targetName,
                    unfollow, generation](const QString &error) {
        if (!self || self->userId_ != targetID ||
            generation != self->userDataRequestGeneration_)
        {
            return;
        }
        const auto current = getApp()->getAccounts()->twitch.getCurrent();
        if (!current || current->getUserId() != accountID)
        {
            return;
        }

        self->followMutationInFlight_ = false;
        self->refreshFollowButton();
        if (self->channel_)
        {
            self->channel_->addSystemMessage(
                QString("Failed to %1 %2: %3")
                    .arg(unfollow ? "unfollow" : "follow", targetName,
                         MoltorinoAuth::normalizeAuthError(
                             unfollow ? "unfollowing users" : "following users",
                             error)));
        }
    };

    if (unfollow)
    {
        TwitchGql::unfollowUser(targetID, auth.token, std::move(success),
                                std::move(failure));
    }
    else
    {
        TwitchGql::followUser(targetID, auth.token, std::move(success),
                              std::move(failure));
    }
}

void UserInfoPopup::openUserChannelInNewTab()
{
    if (this->userName_.isEmpty() || this->isYouTube_ ||
        (this->isTikTok_ && this->platformHandle_.isEmpty()))
    {
        return;
    }

    ChannelPtr channel;
    if (this->isTikTok_)
    {
        channel = getApp()->getTikTokChatServer()->getOrCreate(
            this->platformHandle_);
    }
    else if (this->isKick_)
    {
        channel = getApp()->getKickChatServer()->getOrCreate(
            this->userName_.toLower());
    }
    else
    {
        channel = getApp()->getTwitch()->getOrAddChannel(
            this->userName_.toLower());
    }
    auto &notebook = getApp()->getWindows()->getMainWindow().getNotebook();
    auto *container = notebook.addPage(true);
    auto *split = new Split(container);
    split->setChannel(channel);
    container->insertSplit(split);
}

void UserInfoPopup::appendPlacedUsercardActions(QMenu *menu)
{
    if (menu == nullptr)
    {
        return;
    }

    bool addedAction = false;
    if (!this->isYouTube_ && !this->isTikTok_)
    {
        for (const auto &command : getApp()->getCommands()->items)
        {
            if (command.usercardInMenu &&
                !command.usercardLabel.trimmed().isEmpty())
            {
                auto *action =
                    menu->addAction(command.usercardLabel.trimmed().left(40));
                QObject::connect(action, &QAction::triggered, this,
                                 this->actionForCurrentTarget([this, command] {
                                     this->runCustomAction(command);
                                 }));
                addedAction = true;
            }
        }
    }
    auto addPageAction = [this, menu, &addedAction](
                             const QString &text, QWidget *page,
                             const std::function<void()> &action) {
        auto *menuAction = menu->addAction(text, this,
                                           this->actionForCurrentTarget(action));
        menuAction->setCheckable(true);
        menuAction->setChecked(this->ui_.activityStack->currentWidget() ==
                               page);
        addedAction = true;
    };

    if (usercardActionPlacement(
            getSettings()->usercardCommentsActionPlacement.getValue()) ==
            UsercardActionPlacement::Menu &&
        this->moderatorCommentsActionAvailable())
    {
        addPageAction("Moderator &comments", this->ui_.commentsView, [this] {
            this->toggleModeratorComments();
        });
    }
    if (usercardActionPlacement(
            getSettings()->usercardLogsActionPlacement.getValue()) ==
            UsercardActionPlacement::Menu &&
        this->userLogsActionAvailable())
    {
        addPageAction("&Logs view", this->ui_.logsView, [this] {
            this->toggleUserLogs();
        });
    }
    if (usercardActionPlacement(
            getSettings()->usercardRolesActionPlacement.getValue()) ==
            UsercardActionPlacement::Menu &&
        this->userRolesActionAvailable())
    {
        addPageAction("&Roles", this->ui_.rolesView, [this] {
            this->toggleUserRoles();
        });
    }
    if (usercardActionPlacement(
            getSettings()->usercardSevenTVActionPlacement.getValue()) ==
            UsercardActionPlacement::Menu &&
        getSettings()->showSevenTVUsercardButton &&
        !this->seventvUserID_.isEmpty())
    {
        menu->addAction("Open &7TV profile", this,
                        this->actionForCurrentTarget([this] {
                            this->openSevenTVUser();
                        }));
        addedAction = true;
    }

    if (addedAction)
    {
        menu->addSeparator();
    }
}

std::unique_ptr<QMenu> UserInfoPopup::createUserActionsMenu()
{
    this->refreshLocalUserActions();
    auto menu = std::make_unique<QMenu>(this);
    this->appendPlacedUsercardActions(menu.get());
    const auto inMenu = [](const QStringSetting &setting) {
        return usercardActionPlacement(setting.getValue()) ==
               UsercardActionPlacement::Menu;
    };
    if (this->isYouTube_ || this->isTikTok_)
    {
        menu->addAction("Copy display &name", this, [name = this->userName_] {
            crossPlatformCopy(name);
        });
        if (!this->platformHandle_.isEmpty())
        {
            menu->addAction(this->isTikTok_ ? "Copy TikTok &handle"
                                            : "Copy YouTube &handle",
                            this, [handle = this->platformHandle_] {
                                crossPlatformCopy(u'@' + handle);
                            });
        }
        if (!this->userId_.isEmpty())
        {
            menu->addAction(
                this->isTikTok_ ? "Copy user &ID" : "Copy channel &ID", this,
                [id = this->userId_] {
                    crossPlatformCopy(id);
                });
        }
        if (this->isTikTok_ && !this->platformHandle_.isEmpty())
        {
            menu->addAction("Open chat in a new &tab", this,
                            this->actionForCurrentTarget([this] {
                                this->openUserChannelInNewTab();
                            }));
        }
        menu->addSeparator();
    }
    else if (!this->userName_.isEmpty())
    {
        menu->addAction("Open chat in a new &tab", this,
                        this->actionForCurrentTarget([this] {
                            this->openUserChannelInNewTab();
                        }));
        const auto profileURL =
            QUrl((this->isKick_ ? QStringLiteral("https://kick.com/")
                                : QStringLiteral("https://www.twitch.tv/")) +
                 this->userName_.toLower());
        menu->addAction("Open &profile in browser", this, [profileURL] {
            QDesktopServices::openUrl(profileURL);
        });
        menu->addSeparator();
    }

    bool addedLocalAction = false;
    if (inMenu(getSettings()->usercardNotesActionPlacement))
    {
        auto *notes = menu->addAction(
            "Add or edit &notes", this, this->actionForCurrentTarget([this] {
                this->openUserNotes();
            }));
        notes->setEnabled(this->canEditTargetNotes_);
        addedLocalAction = true;
    }
    if (inMenu(getSettings()->usercardBlockActionPlacement) && !this->isKick_ &&
        !this->isYouTube_ && !this->isTikTok_ && !this->isTargetCurrentUser())
    {
        auto *block = menu->addAction("&Block user");
        block->setCheckable(true);
        block->setChecked(this->targetBlocked_);
        block->setEnabled(this->canChangeTargetBlock_);
        QObject::connect(block, &QAction::triggered, this,
                         this->actionForCurrentTarget([this, block] {
                             this->setTargetBlocked(block->isChecked());
                         }));
        addedLocalAction = true;
    }
    if (!this->isTargetCurrentUser())
    {
        if (inMenu(getSettings()->usercardHideActionPlacement))
        {
            auto *hide = menu->addAction("Hide user in &Moltorino");
            hide->setCheckable(true);
            hide->setChecked(this->targetLocallyHidden_);
            hide->setToolTip("Hide this user and conversations involving them "
                             "in Moltorino.");
            menu->setToolTipsVisible(true);
            QObject::connect(hide, &QAction::triggered, this,
                             this->actionForCurrentTarget([this, hide] {
                                 this->setTargetLocallyHidden(hide->isChecked());
                             }));
            addedLocalAction = true;
        }
        if (inMenu(getSettings()->usercardIgnoreHighlightsActionPlacement))
        {
            auto *ignore = menu->addAction("Ignore &highlights");
            ignore->setCheckable(true);
            ignore->setChecked(this->targetIgnoringHighlights_ ||
                               this->targetIgnoreMatchedByRegex_);
            ignore->setEnabled(this->canChangeTargetHighlightIgnore_);
            if (this->targetIgnoreMatchedByRegex_)
            {
                ignore->setText("Ignore highlights (matched by regex)");
                ignore->setToolTip(
                    "This username is ignored by a matching regex rule.");
                menu->setToolTipsVisible(true);
            }
            QObject::connect(ignore, &QAction::triggered, this,
                             this->actionForCurrentTarget([this, ignore] {
                                 this->setTargetIgnoringHighlights(
                                     ignore->isChecked());
                             }));
            addedLocalAction = true;
        }
    }

    const bool showCrossBan =
        inMenu(getSettings()->usercardCrossBanActionPlacement);
    const bool showCrossUnban =
        inMenu(getSettings()->usercardCrossUnbanActionPlacement);
    if (this->crossActionAvailable() && (showCrossBan || showCrossUnban))
    {
        if (addedLocalAction)
        {
            menu->addSeparator();
        }
        if (showCrossBan)
        {
            menu->addAction("Cross &ban", this,
                            this->actionForCurrentTarget([this] {
                                this->runCrossAction(QStringLiteral("/crossban"));
                            }));
        }
        if (showCrossUnban)
        {
            menu->addAction("Cross &unban", this,
                            this->actionForCurrentTarget([this] {
                                this->runCrossAction(QStringLiteral("/crossunban"));
                            }));
        }
    }

    if (usercardActionPlacement(
            getSettings()->usercardUsercardActionPlacement.getValue()) ==
            UsercardActionPlacement::Menu &&
        this->usercardActionAvailable())
    {
        const auto actions = menu->actions();
        if (!actions.empty() && !actions.back()->isSeparator())
        {
            menu->addSeparator();
        }
        menu->addAction(this->isTikTok_    ? "Open &TikTok profile"
                        : this->isYouTube_ ? "Open &YouTube channel"
                                           : "Open Twitch &usercard",
                        this, this->actionForCurrentTarget([this] {
                            this->openPlatformUsercard();
                        }));
    }

    return menu;
}

void UserInfoPopup::setTargetLocallyHidden(bool hidden)
{
    if (this->isTargetCurrentUser())
    {
        return;
    }

    const auto platform = this->isTikTok_    ? HiddenUserPlatform::TikTok
                          : this->isYouTube_ ? HiddenUserPlatform::YouTube
                          : this->isKick_    ? HiddenUserPlatform::Kick
                                             : HiddenUserPlatform::Twitch;
    auto userID = this->userId_.trimmed();
    if (this->isKick_ &&
        userID.startsWith(QStringLiteral("kick:"), Qt::CaseInsensitive))
    {
        userID.remove(0, 5);
    }
    const auto login = (this->isTikTok_ || (this->isYouTube_ &&
                                            !this->platformHandle_.isEmpty())
                            ? this->platformHandle_
                            : this->userName_)
                           .trimmed();
    const auto displayName = this->userName_.trimmed();
    if (userID.isEmpty() && login.isEmpty() && displayName.isEmpty())
    {
        return;
    }

    if (auto *hiddenUsers = getApp()->getHiddenUsers())
    {
        hiddenUsers->setHidden(platform, userID, login, displayName, hidden);
        this->targetLocallyHidden_ = hidden;
        this->refreshUsercardActionPlacements();
    }
}

void UserInfoPopup::setTargetBlocked(bool blocked)
{
    if (this->isTikTok_ || this->isYouTube_ || this->isKick_ ||
        !this->canChangeTargetBlock_ || this->userId_.isEmpty() ||
        blocked == this->targetBlocked_)
    {
        return;
    }

    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (account->isAnon() ||
        account->getUserName().compare(this->targetBlockStateAccountName_,
                                       Qt::CaseInsensitive) != 0)
    {
        this->canChangeTargetBlock_ = false;
        this->refreshUsercardActionPlacements();
        this->updateUserData();
        return;
    }

    if (blocked)
    {
        const auto self = QPointer<UserInfoPopup>(this);
        const auto generation = this->userDataRequestGeneration_;
        const bool wasPinned = this->ensurePinned();
        const auto response = QMessageBox::warning(
            this, u"Blocking " % this->userName_,
            u"Blocking %1 can cause unintended side-effects like unfollowing.\n\n"_s
                .arg(this->userName_) +
                u"Are you sure you want to block %1?"_s.arg(this->userName_),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (!self)
        {
            return;
        }
        if (wasPinned)
        {
            this->togglePinned();
        }
        if (response != QMessageBox::Yes ||
            generation != this->userDataRequestGeneration_ ||
            getApp()->getAccounts()->twitch.getCurrent() != account)
        {
            return;
        }
    }

    this->canChangeTargetBlock_ = false;
    this->refreshUsercardActionPlacements();
    const auto userID = this->userId_;
    const auto userName = this->userName_;
    const auto accountName = account->getUserName();
    const auto self = QPointer<UserInfoPopup>(this);
    const auto finish = [self, userID, userName, accountName,
                         blocked](bool success) {
        if (!self || self->userId_ != userID ||
            self->userName_.compare(userName, Qt::CaseInsensitive) != 0)
        {
            return;
        }
        const auto active = getApp()->getAccounts()->twitch.getCurrent();
        if (active->getUserName().compare(accountName,
                                          Qt::CaseInsensitive) != 0)
        {
            return;
        }

        if (success)
        {
            self->targetBlocked_ = blocked;
        }
        self->targetBlockStateAccountName_ = active->getUserName();
        self->canChangeTargetBlock_ =
            !active->isAnon() && !self->isTargetCurrentUser();
        self->refreshUsercardActionPlacements();
        if (self->channel_)
        {
            self->channel_->addSystemMessage(
                success
                    ? QString("You successfully %1 user %2")
                          .arg(blocked ? "blocked" : "unblocked",
                               self->userName_)
                    : QString("User %1 couldn't be %2, an unknown error "
                              "occurred!")
                          .arg(self->userName_,
                               blocked ? "blocked" : "unblocked"));
        }
    };

    if (blocked)
    {
        account->blockUser(userID, userName, this,
                           [finish] { finish(true); },
                           [finish] { finish(false); });
    }
    else
    {
        account->unblockUser(userID, userName, this,
                             [finish] { finish(true); },
                             [finish] { finish(false); });
    }
}

void UserInfoPopup::setYouTubeData(const MessagePtr &message,
                                   const YouTubeAuthor &selectedAuthor,
                                   const ChannelPtr &contextChannel,
                                   const ChannelPtr &openingChannel)
{
    this->showActivityPage(ActivityPage::Messages);
    auto youtubeChannel =
        std::dynamic_pointer_cast<YouTubeChannel>(contextChannel);
    if (!message || message->platform != MessagePlatform::YouTube ||
        !youtubeChannel || selectedAuthor.channelId.isEmpty())
    {
        this->deleteLater();
        return;
    }

    this->isYouTube_ = true;
    this->isTikTok_ = false;
    this->isKick_ = false;
    this->identityUserColor_ = {};
    this->messageUserColor_ = {};
    this->apiUserColor_ = {};
    this->apiUserColorLookupFinished_ = true;
    this->twitchUserLookupFinished_ = true;
    this->followStatusKnown_ = false;
    this->following_ = false;
    this->followStatusRequestInFlight_ = false;
    this->followMutationInFlight_ = false;
    this->ui_.followButton->hide();
    this->channel_ = openingChannel ? openingChannel : contextChannel;
    this->underlyingChannel_ = std::move(contextChannel);
    this->userId_ = selectedAuthor.channelId;
    this->resetTargetState();

    auto author = selectedAuthor;
    author.channelId = this->userId_;
    this->userName_ = visibleYouTubeName(author.displayName);
    if (this->userName_.isEmpty() && message->userID == this->userId_)
    {
        this->userName_ = visibleYouTubeName(message->displayName);
        if (this->userName_.isEmpty())
        {
            this->userName_ = visibleYouTubeName(message->loginName);
        }
    }
    if (this->userName_.isEmpty())
    {
        this->userName_ = this->userId_;
    }
    this->platformHandle_ = visibleYouTubeName(author.handle);
    this->youtubeTargetIsOwner_ =
        author.isOwner || this->userId_ == youtubeChannel->channelID();
    this->youtubeTargetIsModerator_ = author.isModerator;
    this->isBroadcaster_ = this->youtubeTargetIsOwner_;
    this->isMod_ = this->youtubeTargetIsModerator_;
    this->avatarUrl_ = author.avatarUrl;
    this->helixAvatarUrl_ = author.avatarUrl;

    this->setWindowTitle(
        QStringLiteral("%1's YouTube usercard - #%2")
            .arg(this->userName_, youtubeChannel->getDisplayName()));
    this->ui_.nameLabel->setText(this->userName_);
    this->ui_.nameLabel->setProperty("copy-text", this->userName_);
    this->ui_.nameLabel->setProperty("paint-login", {});

    const bool showHandle = !this->platformHandle_.isEmpty() &&
                            this->platformHandle_.compare(
                                this->userName_, Qt::CaseInsensitive) != 0;
    this->ui_.localizedNameLabel->setText(
        showHandle ? u'@' + this->platformHandle_ : QString{});
    this->ui_.localizedNameLabel->setProperty(
        "copy-text", showHandle ? u'@' + this->platformHandle_ : QString{});
    this->ui_.localizedNameLabel->setVisible(showHandle);
    this->ui_.localizedNameCopyButton->setVisible(showHandle);
    this->ui_.userIDLabel->setText(TEXT_USER_ID % this->userId_);
    this->ui_.userIDLabel->setProperty("copy-text", this->userId_);

    this->ui_.nameHistoryButton->hide();
    if (this->ui_.pronounsLabel)
    {
        this->ui_.pronounsLabel->hide();
    }
    this->ui_.followerCountLabel->hide();
    this->ui_.createdDateLabel->hide();
    this->ui_.lastLiveLabel->hide();
    this->ui_.userColorRow->hide();
    this->ui_.statusLabel->hide();
    this->ui_.chatterCountLabel->hide();
    this->ui_.followageRow->hide();
    this->ui_.subageRow->hide();
    this->ui_.identityPaintRow->hide();
    this->ui_.notesPreview->hide();
    this->ui_.switchAvatars->hide();
    this->updateUserLogsContext();
    this->ui_.sevenTVUserLabel->hide();
    this->updateUserRolesContext();
    this->ui_.usercardLabel->setText("YouTube");
    this->ui_.usercardLabel->setToolTip("Open YouTube channel");
    this->refreshUsercardActionPlacements();

    if (author.avatarUrl.isEmpty())
    {
        this->avatarPixmap_ =
            QPixmap(QStringLiteral(":/badges/platform-youtube-36.webp"));
    }
    if (getApp()->getStreamerMode()->isEnabled() &&
        getSettings()->streamerModeHideUsercardAvatars)
    {
        this->ui_.avatarButton->setPixmap(getResources().streamerMode);
    }
    else if (!author.avatarUrl.isEmpty())
    {
        this->loadAvatar(this->userId_, author.avatarUrl, false, false);
    }
    else
    {
        this->ui_.avatarButton->setPixmap(this->avatarPixmap_);
    }

    this->updateLatestMessages();

    const auto self = QPointer<UserInfoPopup>(this);
    this->twitchUserStateConnection_ =
        std::make_unique<pajlada::Signals::ScopedConnection>(
            youtubeChannel->userStateChanged.connect([self] {
                if (self)
                {
                    self->userStateChanged_.invoke();
                }
            }));
    this->youtubeModerationStateConnection_ =
        std::make_unique<pajlada::Signals::ScopedConnection>(
            youtubeChannel->moderationStateChanged.connect([self, youtubeChannel] {
                if (!self)
                {
                    return;
                }
                self->ui_.timeoutWidget->setUnbanEnabled(
                    youtubeChannel->canUnbanUser(self->userId_));
                self->userStateChanged_.invoke();
            }));

    this->ui_.timeoutWidget->setMinTimeout(1);
    this->ui_.timeoutWidget->setReasonPromptsEnabled(false);
    this->ui_.timeoutWidget->setUnbanEnabled(
        youtubeChannel->canUnbanUser(this->userId_));
    this->refreshLocalUserActions();
    this->userStateChanged_.invoke();
    this->applyPopupSize(this->sizeHint());
}

void UserInfoPopup::setTikTokData(const TikTokAuthor &author,
                                  const ChannelPtr &contextChannel,
                                  const ChannelPtr &openingChannel)
{
    this->showActivityPage(ActivityPage::Messages);
    this->isTikTok_ = true;
    this->isYouTube_ = false;
    this->isKick_ = false;
    this->youtubeTargetIsOwner_ = false;
    this->youtubeTargetIsModerator_ = false;
    this->identityUserColor_ = {};
    this->messageUserColor_ = {};
    this->apiUserColor_ = {};
    this->apiUserColorLookupFinished_ = true;
    this->twitchUserLookupFinished_ = true;
    this->followStatusKnown_ = false;
    this->following_ = false;
    this->followStatusRequestInFlight_ = false;
    this->followMutationInFlight_ = false;
    this->canChangeTargetBlock_ = false;
    this->targetBlocked_ = false;
    this->ui_.followButton->hide();

    this->underlyingChannel_ =
        contextChannel && contextChannel->getType() == Channel::Type::TikTok
            ? contextChannel
            : std::make_shared<Channel>(QString{}, Channel::Type::TikTok);
    this->channel_ = openingChannel ? openingChannel : this->underlyingChannel_;
    this->userId_ = author.id;
    this->resetTargetState();
    this->platformHandle_ =
        normalizeTikTokHandle(author.handle).value_or(QString{});
    this->userName_ = author.displayName;
    if (this->userName_.isEmpty())
    {
        this->userName_ = this->platformHandle_;
    }
    if (this->userName_.isEmpty())
    {
        this->userName_ = u"TikTok user"_s;
    }
    this->isBroadcaster_ = author.broadcaster;
    this->isMod_ = author.moderator;

    this->setWindowTitle(u"%1's TikTok usercard"_s.arg(this->userName_));
    this->ui_.nameLabel->setText(this->userName_);
    this->ui_.nameLabel->setProperty("copy-text", this->userName_);
    this->ui_.nameLabel->setProperty("paint-login", {});
    const bool showHandle = !this->platformHandle_.isEmpty();
    const auto handleText =
        showHandle ? u'@' + this->platformHandle_ : QString{};
    this->ui_.localizedNameLabel->setText(handleText);
    this->ui_.localizedNameLabel->setProperty("copy-text", handleText);
    this->ui_.localizedNameLabel->setVisible(showHandle);
    this->ui_.localizedNameCopyButton->setVisible(showHandle);
    this->ui_.handleRow->setVisible(showHandle);
    this->ui_.userIDLabel->setText(
        this->userId_.isEmpty() ? QString{} : TEXT_USER_ID % this->userId_);
    this->ui_.userIDLabel->setProperty("copy-text", this->userId_);
    this->ui_.nameHistoryButton->hide();
    this->ui_.bioLabel->setText(author.bio);
    this->ui_.bioLabel->setVisible(!author.bio.isEmpty());

    QStringList roles;
    if (author.broadcaster)
    {
        roles << u"Host"_s;
    }
    if (author.moderator)
    {
        roles << u"Moderator"_s;
    }
    if (author.subscriber)
    {
        roles << u"Subscriber"_s;
    }
    if (author.verified)
    {
        roles << u"Verified"_s;
    }
    this->platformRoles_ = roles.join(u" · "_s);
    this->resetUsercardInfoRows();
    MessageBuilder identity;
    appendTikTokBadges(identity, author);
    this->identityMessageFallback_ = identity.release();
    this->ui_.identityPaintRow->hide();
    this->ui_.switchAvatars->hide();
    this->ui_.sevenTVUserLabel->hide();
    this->ui_.usercardLabel->setText("TikTok");
    this->ui_.usercardLabel->setToolTip("Open TikTok profile");
    this->updateUserLogsContext();
    this->updateUserRolesContext();
    this->ui_.timeoutWidget->hide();

    auto avatarUrl = author.avatarUrl;
    if (avatarUrl.isEmpty())
    {
        const auto account = getApp()->getAccounts()->tiktok.current();
        if (account && account->userID() == this->userId_)
        {
            avatarUrl = account->avatarUrl();
        }
    }
    if (isTikTokImageUrl(QUrl(avatarUrl)))
    {
        this->avatarUrl_ = this->helixAvatarUrl_ = avatarUrl;
    }
    else
    {
        this->avatarPixmap_ = QPixmap(128, 128);
        this->avatarPixmap_.fill(QColor(22, 24, 35));
        QPainter painter(&this->avatarPixmap_);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.drawPixmap(
            8, 8, QIcon(u":/badges/platform-tiktok.svg"_s).pixmap(112, 112));
    }
    this->refreshAvatarVisibility();
    this->updateLatestMessages();
    this->refreshLocalUserActions();
    this->userStateChanged_.invoke();
    this->applyPopupSize(this->sizeHint());
}

QString UserInfoPopup::highlightIdentity() const
{
    if (this->isTikTok_)
    {
        return this->platformHandle_;
    }
    if (this->isYouTube_ && !this->platformHandle_.isEmpty())
    {
        return this->platformHandle_;
    }
    return this->userName_;
}

QString UserInfoPopup::notesUserKey() const
{
    return this->isTikTok_ && !this->userId_.isEmpty()
               ? u"tiktok:"_s + this->userId_
               : this->userId_;
}

MessagePlatform UserInfoPopup::targetMessagePlatform() const
{
    return this->isTikTok_    ? MessagePlatform::TikTok
           : this->isYouTube_ ? MessagePlatform::YouTube
           : this->isKick_    ? MessagePlatform::Kick
                              : MessagePlatform::AnyOrTwitch;
}

void UserInfoPopup::refreshLocalUserActions()
{
    const auto identity = this->highlightIdentity();
    bool hasExactIgnore = false;
    for (const auto &ignoredUser : getSettings()->blacklistedUsers.raw())
    {
        if (ignoredUser.isLiteralMatch(identity))
        {
            hasExactIgnore = true;
            break;
        }
    }

    this->targetIgnoringHighlights_ = hasExactIgnore;
    this->targetIgnoreMatchedByRegex_ =
        !identity.isEmpty() && getSettings()->isBlacklistedUser(identity) &&
        !hasExactIgnore;
    this->canChangeTargetHighlightIgnore_ =
        !identity.isEmpty() && !this->isTargetCurrentUser() &&
        !this->targetIgnoreMatchedByRegex_;
    this->canEditTargetNotes_ = !this->userId_.isEmpty();
    const auto platform = this->targetMessagePlatform();
    auto localUserID = this->userId_;
    if (this->isKick_ &&
        localUserID.startsWith(QStringLiteral("kick:"),
                               Qt::CaseInsensitive))
    {
        localUserID.remove(0, 5);
    }
    const auto localLogin =
        this->isTikTok_ ||
                (this->isYouTube_ && !this->platformHandle_.isEmpty())
            ? this->platformHandle_
            : this->userName_;
    this->targetLocallyHidden_ =
        getApp()->getHiddenUsers() != nullptr &&
        getApp()->getHiddenUsers()->isHidden(platform, localUserID,
                                             localLogin, this->userName_);

    this->updateNotes();
    this->refreshUsercardActionPlacements();
}

void UserInfoPopup::setTargetIgnoringHighlights(bool ignored)
{
    const auto identity = this->highlightIdentity();
    if (!this->canChangeTargetHighlightIgnore_ ||
        identity.isEmpty() ||
        ignored == this->targetIgnoringHighlights_)
    {
        return;
    }

    if (ignored)
    {
        getSettings()->blacklistedUsers.insert(
            HighlightBlacklistUser{identity, false});
    }
    else
    {
        const auto &users = getSettings()->blacklistedUsers.raw();
        for (int i = 0; i < static_cast<int>(users.size()); ++i)
        {
            if (users[static_cast<size_t>(i)].isLiteralMatch(identity))
            {
                getSettings()->blacklistedUsers.removeAt(i--);
            }
        }
    }
    this->targetIgnoringHighlights_ = ignored;
    this->targetIgnoreMatchedByRegex_ =
        getSettings()->isBlacklistedUser(identity) && !ignored;
    this->canChangeTargetHighlightIgnore_ =
        !this->isTargetCurrentUser() && !this->targetIgnoreMatchedByRegex_;
    this->refreshUsercardActionPlacements();
}

void UserInfoPopup::openUserNotes()
{
    if (!this->canEditTargetNotes_ || this->userId_.isEmpty())
    {
        return;
    }
    if (this->editUserNotesDialog_.isNull())
    {
        this->editUserNotesDialog_ = new EditUserNotesDialog(this);
        const auto self = QPointer<UserInfoPopup>(this);
        std::ignore = this->editUserNotesDialog_->onOk.connect(
            [self](const QString &notes) {
                if (self && !self->editUserNotesTargetId_.isEmpty())
                {
                    getApp()->getUserData()->setUserNotes(
                        self->editUserNotesTargetId_, notes);
                }
            });
    }

    this->editUserNotesTargetId_ = this->notesUserKey();
    const auto data = getApp()->getUserData()->getUser(this->notesUserKey());
    this->editUserNotesDialog_->setNotes(data ? data->notes : QString{});
    this->editUserNotesDialog_->updateWindowTitle(this->userName_);
    this->editUserNotesDialog_->show();
}

bool UserInfoPopup::isTargetCurrentUser() const
{
    if (this->isTikTok_)
    {
        const auto account = getApp()->getAccounts()->tiktok.current();
        return !account->isAnonymous() && !this->userId_.isEmpty() &&
               account->userID() == this->userId_;
    }
    if (this->isYouTube_)
    {
        const auto account = getApp()->getAccounts()->youtube.current();
        return !account->isAnonymous() && !this->userId_.isEmpty() &&
               account->channelID() == this->userId_;
    }
    if (this->userName_.isEmpty())
    {
        return false;
    }
    const auto currentName =
        this->isKick_
            ? getApp()->getAccounts()->kick.current()->username()
            : getApp()->getAccounts()->twitch.getCurrent()->getUserName();
    return currentName.compare(this->userName_, Qt::CaseInsensitive) == 0;
}

void UserInfoPopup::appendCommonProfileActions(QMenu *menu)
{
    if (!this->isKick_ && !this->isYouTube_ && !this->isTikTok_ &&
        !this->userName_.isEmpty())
    {
        menu->addAction(
            "Open profile in &ChatVault", this,
            [url = chatVaultTwitchChannelUrl(this->userName_)] {
                QDesktopServices::openUrl(QUrl(url));
            });
    }

    if (!this->seventvUserID_.isEmpty())
    {
        menu->addAction(
            "Open &7TV user in browser", this, [id = this->seventvUserID_] {
                QDesktopServices::openUrl(QUrl(SEVENTV_USER_PAGE % id));
            });
    }
}

void UserInfoPopup::executeUsercardModerationAction(
    const UsercardModerationRequest &request)
{
    if (!this->underlyingChannel_ || !this->shouldShowModerationActions())
    {
        return;
    }

    if (auto *youtubeChannel =
            dynamic_cast<YouTubeChannel *>(this->underlyingChannel_.get()))
    {
        if (this->userId_.isEmpty())
        {
            return;
        }
        switch (request.action)
        {
            case UsercardModerationAction::Ban:
                youtubeChannel->moderateUser(this->userId_, std::nullopt);
                return;
            case UsercardModerationAction::Unban:
                youtubeChannel->unbanUser(this->userId_);
                return;
            case UsercardModerationAction::Timeout:
                if (request.durationSeconds > 0)
                {
                    youtubeChannel->moderateUser(
                        this->userId_,
                        std::chrono::seconds{request.durationSeconds});
                }
                return;
        }
    }

    QString value;
    switch (request.action)
    {
        case UsercardModerationAction::Ban: {
            value = appendModerationReason("/ban " + this->userName_,
                                           request.reason);
        }
        break;

        case UsercardModerationAction::Unban: {
            value = "/unban " + this->userName_;
        }
        break;

        case UsercardModerationAction::Timeout: {
            if (request.durationSeconds <= 0)
            {
                return;
            }

            value = appendModerationReason(
                "/timeout " + this->userName_ + " " +
                    QString::number(request.durationSeconds) + 's',
                request.reason);
        }
        break;
    }

    value = getApp()->getCommands()->execCommand(value,
                                                 this->underlyingChannel_,
                                                 false);
    this->underlyingChannel_->sendMessage(value);
}

void UserInfoPopup::showUsercardModerationReasonPopup(
    const UsercardModerationRequest &request)
{
    if (request.action == UsercardModerationAction::Unban)
    {
        this->executeUsercardModerationAction(request);
        return;
    }

    if (this->moderationReasonPopup_)
    {
        this->moderationReasonPopup_->close();
    }

    const bool wasPinned = this->ensurePinned();

    const auto isBan = request.action == UsercardModerationAction::Ban;
    const auto initialReason =
        getSettings()->timeoutReasonPromptPrefillSavedReason.getValue()
            ? request.reason
            : QString();
    auto *popup = new ModerationReasonPopup(
        isBan ? "Ban reason" : "Timeout reason", "optional reason",
        initialReason,
        getSettings()->timeoutReasonPromptShowSendButton.getValue(),
        [self = QPointer<UserInfoPopup>(this),
         generation = this->userDataRequestGeneration_,
         request](QString reason) mutable {
            if (!self || generation != self->userDataRequestGeneration_)
            {
                return;
            }

            auto updatedRequest = request;
            updatedRequest.reason = reason;
            updatedRequest.promptForReason = false;
            self->executeUsercardModerationAction(updatedRequest);
        },
        this);

    popup->setAttribute(Qt::WA_DeleteOnClose);
    this->moderationReasonPopup_ = popup;
    if (wasPinned)
    {
        auto didRestorePin = std::make_shared<bool>(false);
        auto restorePin = [self = QPointer<UserInfoPopup>(this),
                           didRestorePin] {
            if (*didRestorePin)
            {
                return;
            }
            *didRestorePin = true;

            if (self)
            {
                self->togglePinned();
            }
        };
        std::ignore = popup->closing.connect(restorePin);
        QObject::connect(popup, &QObject::destroyed, this,
                         std::move(restorePin));
    }
    popup->showCenteredAt(QCursor::pos());
    popup->activateWindow();
    popup->raise();
}

//
// TimeoutWidget
//
UserInfoPopup::TimeoutWidget::TimeoutWidget()
    : BaseWidget(nullptr)
{
    this->rootLayout_ = new QHBoxLayout(this);
    this->rootLayout_->setContentsMargins(0, 0, 0, 0);
    this->rootLayout_->setSpacing(0);
    this->rebuildActions();

    getSettings()->timeoutButtons.connect([this](const auto &) {
        this->rebuildActions();
    }, this->signalHolder_);
    getSettings()->timeoutButtonReasons.connect([this](const auto &) {
        this->refreshActionTooltips();
    }, this->signalHolder_);
    getSettings()->timeoutBanReason.connect([this](const auto &) {
        this->refreshActionTooltips();
    }, this->signalHolder_);
    getSettings()->timeoutReasonPromptOnRightClick.connect(
        [this](const auto &) {
            this->refreshActionTooltips();
        },
        this->signalHolder_);
    getSettings()->timeoutReasonPromptOnModifier.connect(
        [this](const auto &) {
            this->refreshActionTooltips();
        },
        this->signalHolder_);
    getSettings()->timeoutReasonPromptModifier.connect(
        [this](const auto &) {
            this->refreshActionTooltips();
        },
        this->signalHolder_);
}

void UserInfoPopup::TimeoutWidget::rebuildActions()
{
    delete this->content_;
    this->content_ = new QWidget(this);
    this->rootLayout_->addWidget(this->content_);

    this->timeoutButtons.clear();
    this->actionWidgets_.clear();
    this->unbanButton_ = nullptr;

    auto layout = LayoutCreator<QWidget>(this->content_)
                      .setLayoutType<QHBoxLayout>()
                      .withoutMargin();

    int buttonWidth = 40;
    int buttonHeight = 32;

    layout->setSpacing(16);

    const auto addLayout = [&](const QString &text) {
        auto vbox = layout.emplace<QVBoxLayout>().withoutMargin();
        auto title = vbox.emplace<QHBoxLayout>().withoutMargin();
        title->addStretch(1);
        auto label = title.emplace<Label>(text);
        label->setStyleSheet("color: #BBB");
        label->setPadding(QMargins{});
        title->addStretch(1);

        auto hbox = vbox.emplace<QHBoxLayout>().withoutMargin();
        hbox->setSpacing(0);
        return hbox;
    };

    const auto addButton = [&](UsercardModerationAction action,
                               const QString &title, const QPixmap &pixmap,
                               const QString &target) {
        auto button = addLayout(title).emplace<PixmapButton>(nullptr);
        button->setPixmap(pixmap);
        button->setScaleIndependentSize(buttonHeight, buttonHeight);
        button->setBorderColor(QColor(255, 255, 255, 127));
        if (action == UsercardModerationAction::Unban)
        {
            this->unbanButton_ = button.getElement();
        }
        const bool supportsPrompt = action != UsercardModerationAction::Unban;
        this->actionWidgets_.push_back(
            {button.getElement(), target, supportsPrompt});

        QObject::connect(
            button.getElement(), &Button::clicked,
            [this, action](Qt::MouseButton mouseButton) {
                if (!this->reasonPromptsEnabled_ &&
                    mouseButton != Qt::LeftButton)
                {
                    return;
                }
                if (!shouldHandleModerationButtonClick(mouseButton))
                {
                    return;
                }

                UsercardModerationRequest request;
                request.action = action;
                if (this->reasonPromptsEnabled_ &&
                    action == UsercardModerationAction::Ban)
                {
                    request.reason = timeoutBanReason();
                }
                request.promptForReason =
                    this->reasonPromptsEnabled_ &&
                    action != UsercardModerationAction::Unban &&
                    shouldPromptForModerationReason(mouseButton);

                this->buttonClicked.invoke(request);
            });
    };

    auto addTimeouts = [&](const QString &title) {
        auto hbox = addLayout(title);

        int index = 0;
        for (const auto &item : getSettings()->timeoutButtons.getValue())
        {
            auto a = hbox.emplace<LabelButton>();
            a->setPadding({0, 0});
            a->setText(QString::number(item.second) + item.first);

            a->setScaleIndependentSize(buttonWidth, buttonHeight);
            a->setBorderColor(borderColor);

            const auto duration = calculateTimeoutDuration(item);
            const auto buttonIndex = index;
            const auto target = QString::number(index + 1);
            this->timeoutButtons.emplace_back(a.getElement(), duration);
            this->actionWidgets_.push_back(
                {a.getElement(), target, true});

            QObject::connect(a.getElement(), &LabelButton::clicked,
                             [this, duration,
                              buttonIndex](Qt::MouseButton button) {
                                 if (!this->reasonPromptsEnabled_ &&
                                     button != Qt::LeftButton)
                                 {
                                     return;
                                 }
                                 if (!shouldHandleModerationButtonClick(
                                         button))
                                 {
                                     return;
                                 }

                                 UsercardModerationRequest request;
                                 request.action =
                                     UsercardModerationAction::Timeout;
                                 request.durationSeconds = duration;
                                 request.reason =
                                     this->reasonPromptsEnabled_
                                         ? timeoutButtonReason(buttonIndex)
                                         : QString{};
                                 request.promptForReason =
                                     this->reasonPromptsEnabled_ &&
                                     shouldPromptForModerationReason(
                                         button);

                                 this->buttonClicked.invoke(request);
                             });
            ++index;
        }
    };

    addButton(UsercardModerationAction::Unban, "Unban",
              getResources().buttons.unban, "unban");
    addTimeouts("Timeouts");
    addButton(UsercardModerationAction::Ban, "Ban",
              getResources().buttons.ban, "ban");

    this->setMinTimeout(this->minTimeoutSeconds_);
    this->setUnbanEnabled(this->unbanEnabled_);
}

void UserInfoPopup::TimeoutWidget::paintEvent(QPaintEvent *)
{
    //    QPainter painter(this);

    //    painter.setPen(QColor(255, 255, 255, 63));

    //    painter.drawLine(0, this->height() / 2, this->width(), this->height()
    //    / 2);
}

void UserInfoPopup::TimeoutWidget::setMinTimeout(int minSecs)
{
    this->minTimeoutSeconds_ = minSecs;
    for (auto &[widget, dur] : this->timeoutButtons)
    {
        widget->setVisible(dur >= minSecs);
    }
}

void UserInfoPopup::TimeoutWidget::setUnbanEnabled(bool enabled)
{
    this->unbanEnabled_ = enabled;
    if (this->unbanButton_)
    {
        this->unbanButton_->setEnabled(enabled);
    }
    this->refreshActionTooltips();
}

void UserInfoPopup::TimeoutWidget::setReasonPromptsEnabled(bool enabled)
{
    this->reasonPromptsEnabled_ = enabled;
    this->refreshActionTooltips();
}

void UserInfoPopup::TimeoutWidget::refreshActionTooltips()
{
    for (const auto &action : this->actionWidgets_)
    {
        if (action.widget == nullptr)
        {
            continue;
        }

        const auto immediate = getApp()->getHotkeys()->getDisplaySequence(
            HotkeyCategory::PopupWindow, "execModeratorAction",
            std::vector<QString>{action.target});
        const auto prompted = getApp()->getHotkeys()->getDisplaySequence(
            HotkeyCategory::PopupWindow, "execModeratorActionWithReason",
            std::vector<QString>{action.target});

        QStringList lines;
        QStringList shortcuts;
        if (!immediate.isEmpty())
        {
            shortcuts.push_back(
                immediate.toString(QKeySequence::NativeText));
        }
        if (!this->reasonPromptsEnabled_ && !prompted.isEmpty())
        {
            shortcuts.push_back(prompted.toString(QKeySequence::NativeText));
        }
        if (!shortcuts.isEmpty())
        {
            lines.push_back("Shortcut: " + joinTooltipOptions(shortcuts));
        }

        QStringList reviewOptions;
        if (action.supportsReasonPrompt && this->reasonPromptsEnabled_)
        {
            if (!prompted.isEmpty())
            {
                reviewOptions.push_back(
                    prompted.toString(QKeySequence::NativeText));
            }
            if (getSettings()->timeoutReasonPromptOnRightClick.getValue())
            {
                reviewOptions.push_back("right click");
            }
            if (getSettings()->timeoutReasonPromptOnModifier.getValue())
            {
                reviewOptions.push_back(
                    "hold " +
                    getSettings()->timeoutReasonPromptModifier.getValue());
            }
        }
        if (!reviewOptions.isEmpty())
        {
            lines.push_back("Review: " +
                            joinTooltipOptions(reviewOptions));
        }
        if (action.target == "unban" && !this->unbanEnabled_)
        {
            lines.push_back(
                "Only bans made during this Moltorino session can be "
                "undone here.");
        }
        action.widget->setToolTip(lines.join('\n'));
    }
}

void UserInfoPopup::updateAvatarUrl()
{
    if (this->isTwitchAvatarShown_)
    {
        this->avatarUrl_ = this->helixAvatarUrl_;
    }
    else
    {
        this->avatarUrl_ = this->seventvAvatarUrl_;
    }
}

}  // namespace chatterino
