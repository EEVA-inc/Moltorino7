#include "widgets/dialogs/VanityDialog.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Image.hpp"
#include "providers/bluzyrino/BluzyrinoBadges.hpp"
#include "providers/bttv/BttvBadges.hpp"
#include "providers/chatterino/ChatterinoBadges.hpp"
#include "providers/ffz/FfzBadges.hpp"
#include "providers/ffzap/FfzApBadges.hpp"
#include "providers/homies/HomiesBadges.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/seventv/SeventvBadges.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "providers/twitch/TwitchBadges.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/buttons/Button.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/helper/Line.hpp"

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QButtonGroup>
#include <QClipboard>
#include <QCryptographicHash>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHash>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>
#include <unordered_set>

namespace chatterino {

QPointer<VanityDialog> VanityDialog::activeDialog_;

namespace {

constexpr auto SEVENTV_GQL_ENDPOINT = "https://api.7tv.app/v4/gql";
constexpr auto TWITCH_EVENT_BADGES_ENDPOINT =
    "https://api.catquery.com/eventBadges";
constexpr auto VANITY_LAYOUT_ENDPOINT =
    "https://api.moltorino.com/v2/badges/me";
constexpr QSize DEFAULT_SIZE(412, 480);
constexpr int HEADER_SEPARATOR_HEIGHT = 5;
constexpr int MAX_SEVENTV_COSMETICS_PER_KIND = 2048;
constexpr int MAX_SEVENTV_ACCOUNT_CACHE_ENTRIES = 8;
constexpr int ORDER_KEY_ROLE = Qt::UserRole;
constexpr int ORDER_VISIBLE_ROLE = Qt::UserRole + 1;
constexpr int ORDER_SLOT_ROLE = Qt::UserRole + 2;

struct PreviewBadge {
    QString key;
    QString slot;
    QString name;
    ImagePtr image;
};

struct TwitchColorChoice {
    const char *name;
    const char *value;
    const char *hex;
};

constexpr TwitchColorChoice TWITCH_COLORS[] = {
    {"Blue", "blue", "#0000FF"},
    {"Blue Violet", "blue_violet", "#8A2BE2"},
    {"Cadet Blue", "cadet_blue", "#5F9EA0"},
    {"Chocolate", "chocolate", "#D2691E"},
    {"Coral", "coral", "#FF7F50"},
    {"Dodger Blue", "dodger_blue", "#1E90FF"},
    {"Firebrick", "firebrick", "#B22222"},
    {"Golden Rod", "golden_rod", "#DAA520"},
    {"Green", "green", "#008000"},
    {"Hot Pink", "hot_pink", "#FF69B4"},
    {"Orange Red", "orange_red", "#FF4500"},
    {"Red", "red", "#FF0000"},
    {"Sea Green", "sea_green", "#2E8B57"},
    {"Spring Green", "spring_green", "#00FF7F"},
    {"Yellow Green", "yellow_green", "#9ACD32"},
};

enum class VanityChoiceKind {
    Badge,
    Paint,
};

struct VanityChoice {
    QString key;
    QString name;
    QString imageUrl;
    QString shortName;
    QString description;
    bool owned = true;
};

struct SevenTVVanityCacheEntry {
    QByteArray tokenHash;
    QString userId;
    QString login;
    QString activeBadgeId;
    QString activePaintId;
    QVector<SevenTVVanityCosmetic> badges;
    QVector<SevenTVVanityCosmetic> paints;
};

QHash<QString, SevenTVVanityCacheEntry> &sevenTVVanityCache()
{
    static QHash<QString, SevenTVVanityCacheEntry> cache;
    return cache;
}

struct TwitchEventBadgeCache {
    QVector<TwitchEventBadge> badges;
    QDateTime fetchedAt;
    QDateTime lastAttempt;
    bool requestInFlight = false;
    QVector<std::function<void(const QVector<TwitchEventBadge> &, bool)>>
        waiters;
};

TwitchEventBadgeCache &twitchEventBadgeCache()
{
    static TwitchEventBadgeCache cache;
    return cache;
}

bool isSafeHttpsUrl(const QString &value)
{
    const QUrl url(value);
    return url.isValid() && url.scheme() == QStringLiteral("https") &&
           !url.host().isEmpty() && url.userInfo().isEmpty() &&
           url.port(443) == 443;
}

QDateTime parseIsoDate(const QString &value)
{
    auto parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!parsed.isValid())
    {
        parsed = QDateTime::fromString(value, Qt::ISODate);
    }
    return parsed.toUTC();
}

std::optional<QVector<TwitchEventBadge>> parseTwitchEventBadges(
    const QJsonObject &root)
{
    const auto badgesValue = root.value(QStringLiteral("badges"));
    if (!badgesValue.isArray())
    {
        return std::nullopt;
    }

    constexpr int MAX_EVENT_BADGES = 128;
    QVector<TwitchEventBadge> badges;
    QSet<QString> seen;
    for (const auto &entry : badgesValue.toArray())
    {
        if (badges.size() >= MAX_EVENT_BADGES)
        {
            break;
        }
        if (!entry.isObject())
        {
            continue;
        }
        const auto object = entry.toObject();
        auto id = object.value(QStringLiteral("id")).toString().trimmed();
        const auto name =
            object.value(QStringLiteral("name")).toString().trimmed();
        const auto imageUrl =
            object.value(QStringLiteral("imageUrl")).toString().trimmed();
        auto detailsUrl =
            object.value(QStringLiteral("streamdatabaseUrl"))
                .toString()
                .trimmed();
        const auto startAt = parseIsoDate(
            object.value(QStringLiteral("startAt")).toString());
        const auto endAt =
            parseIsoDate(object.value(QStringLiteral("endAt")).toString());
        id = id.toLower();
        if (id.isEmpty() || id.size() > 128 || name.isEmpty() ||
            name.size() > 160 || imageUrl.size() > 2048 ||
            detailsUrl.size() > 2048 || !isSafeHttpsUrl(imageUrl) ||
            !startAt.isValid() || !endAt.isValid() || startAt >= endAt ||
            seen.contains(id))
        {
            continue;
        }
        seen.insert(id);
        if (!isSafeHttpsUrl(detailsUrl))
        {
            detailsUrl.clear();
        }
        std::optional<bool> free;
        const auto freeValue = object.value(QStringLiteral("free"));
        if (freeValue.isBool())
        {
            free = freeValue.toBool();
        }
        badges.push_back({id, name, imageUrl, detailsUrl, startAt, endAt, free});
    }
    return badges;
}

bool isTwitchEventBadgeActive(const TwitchEventBadge &badge,
                              const QDateTime &now)
{
    return badge.startAt <= now && now < badge.endAt;
}

QString twitchEventBadgeDescription(const TwitchEventBadge &badge)
{
    const auto date = badge.endAt.toLocalTime().toString(
        QStringLiteral("MMM d, yyyy"));
    if (badge.free.value_or(false))
    {
        return QStringLiteral("Free for a limited time. Available until %1.")
            .arg(date);
    }
    return QStringLiteral("Available for a limited time, until %1.")
        .arg(date);
}

QByteArray tokenHash(const QString &token)
{
    return QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256);
}

QString graphQlError(const QJsonObject &root)
{
    const auto errors = root.value("errors").toArray();
    if (errors.isEmpty())
    {
        return {};
    }
    return errors.first().toObject().value("message").toString(
        QStringLiteral("7TV rejected the request"));
}

QString bestImageUrl(const QJsonArray &images)
{
    constexpr int MAX_IMAGE_CANDIDATES = 32;
    QString fallback;
    int bestDistance = std::numeric_limits<int>::max();
    int bestFormatRank = -1;
    QString best;
    const auto candidateCount =
        std::min<qsizetype>(images.size(), MAX_IMAGE_CANDIDATES);
    for (qsizetype index = 0; index < candidateCount; ++index)
    {
        const auto image = images.at(index).toObject();
        const auto url = image.value("url").toString().trimmed();
        if (url.isEmpty() || url.size() > 2048 || !isSafeHttpsUrl(url))
        {
            continue;
        }
        const auto width = image.value("width").toInt();
        if (width <= 0)
        {
            fallback = url;
            continue;
        }
        if (width > 256)
        {
            continue;
        }
        const auto distance = std::abs(width - 36);
        const auto path = QUrl(url).path().toLower();

        int formatRank = 0;
        if (!path.contains(u"_static"))
        {
            if (path.endsWith(u".webp") || path.endsWith(u".gif"))
            {
                formatRank = 2;
            }
            else if (path.endsWith(u".png"))
            {
                formatRank = 1;
            }
        }
        if (formatRank > bestFormatRank ||
            (formatRank == bestFormatRank && distance < bestDistance))
        {
            bestDistance = distance;
            bestFormatRank = formatRank;
            best = url;
        }
    }
    return best.isEmpty() ? fallback : best;
}

SevenTVVanityCosmetic cosmeticFromObject(const QJsonObject &object)
{
    auto id = object.value("id").toString().trimmed();
    if (id.isEmpty() || id.size() > 128)
    {
        return {};
    }
    auto name = object.value("name").toString().trimmed().left(160);
    if (name.isEmpty())
    {
        name = id;
    }
    return SevenTVVanityCosmetic{
        .id = std::move(id),
        .name = std::move(name),
        .imageUrl = bestImageUrl(object.value("images").toArray()),
    };
}

QString normalizedToken(QString token)
{
    constexpr qsizetype MAX_TOKEN_LENGTH = 4096;
    if (token.size() > MAX_TOKEN_LENGTH + 32)
    {
        return {};
    }
    token = token.trimmed();
    while (token.startsWith(QStringLiteral("Bearer "), Qt::CaseInsensitive))
    {
        token = token.mid(7).trimmed();
    }
    return token.size() <= MAX_TOKEN_LENGTH ? token : QString{};
}

QString tokenExpiry(const QString &token)
{
    const auto parts = token.split('.');
    if (parts.size() < 2)
    {
        return {};
    }
    const auto decoded = QByteArray::fromBase64(parts.at(1).toUtf8(),
                                                QByteArray::Base64UrlEncoding);
    const auto object = QJsonDocument::fromJson(decoded).object();
    const auto expiry = object.value("exp").toInteger();
    if (expiry <= 0)
    {
        return {};
    }
    return QDateTime::fromSecsSinceEpoch(expiry, Qt::UTC).toString(Qt::ISODate);
}

QString compactSevenTVBadgeName(const QString &name)
{
    static const QRegularExpression subscriber(
        QStringLiteral("^7TV Subscriber(?:\\s*(?:-|\\x{2013}|\\x{2014})\\s*)?"
                       "(.*)$"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = subscriber.match(name.trimmed());
    if (!match.hasMatch())
    {
        return name;
    }
    auto duration = match.captured(1).trimmed();
    if (duration.isEmpty())
    {
        return QStringLiteral("Subscriber");
    }
    duration.replace(QRegularExpression(QStringLiteral("\\s*years?")),
                     QStringLiteral("y"));
    duration.replace(QRegularExpression(QStringLiteral("\\s*months?")),
                     QStringLiteral("m"));
    duration.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
    return duration;
}

QString compactMoltorinoBadgeName(QString name)
{
    name = name.trimmed();
    static const QRegularExpression prefix(
        QStringLiteral("^Moltorino\\s+"),
        QRegularExpression::CaseInsensitiveOption);
    name.remove(prefix);
    return name.isEmpty() ? QStringLiteral("Badge") : name;
}

MoltorinoVanityLayout vanityLayoutFromObject(const QJsonObject &object)
{
    constexpr int MAX_KEYS = 64;

    auto parseKeys = [&](const QJsonValue &value) {
        std::vector<QString> keys;
        std::unordered_set<QString> seen;
        for (const auto &entry : value.toArray())
        {
            if (keys.size() >= MAX_KEYS)
            {
                break;
            }
            const auto key = entry.toString().trimmed().toLower();

            if (!key.isEmpty() && seen.insert(key).second)
            {
                keys.push_back(key);
            }
        }
        return keys;
    };

    MoltorinoVanityLayout layout;
    layout.order = parseKeys(object.contains("order") ? object.value("order")
                                                      : object.value("o"));
    for (auto key : parseKeys(object.contains("hidden") ? object.value("hidden")
                                                        : object.value("h")))
    {
        layout.hidden.insert(std::move(key));
    }
    layout.moltorinoBadge =
        object.value(object.contains("selectedBadge") ? "selectedBadge" : "m")
            .toString()
            .trimmed()
            .toLower();
    layout.moltorinoBadgeSelectionExplicit =
        object.contains("badgeSelectionExplicit")
            ? object.value("badgeSelectionExplicit").toBool(false)
        : object.contains("x") ? object.value("x").toBool(false)
                               : !layout.moltorinoBadge.isEmpty();
    return vanity::detail::normalizeLayout(std::move(layout));
}

QRect orderEyeRect(const QRect &row, qreal scale)
{
    const int width = qRound(34 * scale);
    return QRect(row.right() - width, row.top(), width, row.height());
}

}

bool vanity::detail::canSaveIndependentVanityChanges(bool hasAccount,
                                                     bool layoutLoaded,
                                                     bool layoutRequestInFlight,
                                                     bool saveInFlight)
{
    return hasAccount && layoutLoaded && !layoutRequestInFlight &&
           !saveInFlight;
}

std::vector<TwitchGqlAuth> vanity::detail::buildTwitchAuthCandidates(
    const QString &accountToken, const QString &accountClientId,
    const QString &savedToken, const QString &savedClientId)
{
    std::vector<TwitchGqlAuth> candidates;
    auto append = [&candidates](QString token, QString clientId) {
        token = normalizedToken(std::move(token));
        clientId = clientId.trimmed();
        if (token.isEmpty())
        {
            return;
        }
        const auto duplicate =
            std::ranges::any_of(candidates, [&](const auto &candidate) {
                return candidate.oauthToken == token &&
                       candidate.clientId.compare(clientId,
                                                  Qt::CaseInsensitive) == 0;
            });
        if (!duplicate)
        {
            candidates.push_back({std::move(token), std::move(clientId)});
        }
    };

    append(accountToken, accountClientId);
    append(savedToken, savedClientId);
    if (!normalizedToken(savedToken).isEmpty() &&
        savedClientId.trimmed().isEmpty())
    {
        append(savedToken, twitchgql::detail::tvClientId());
    }
    return candidates;
}

std::vector<TwitchBadge> vanity::detail::withSelectedTwitchVanityBadge(
    std::vector<TwitchBadge> badges, const QString &selectedSetId,
    const QString &selectedVersion)
{
    std::erase_if(badges, [](const auto &badge) {
        return badge.vanitySlotKey() == QStringLiteral("tv");
    });
    if (!selectedSetId.trimmed().isEmpty() &&
        !selectedVersion.trimmed().isEmpty())
    {
        badges.emplace_back(selectedSetId.trimmed().toLower(),
                            selectedVersion.trimmed());
    }
    return badges;
}

class VanityPreviewWidget final : public QWidget
{
public:
    explicit VanityPreviewWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        this->setFixedHeight(60);
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        this->repaintTimer_.setTimerType(Qt::PreciseTimer);
        this->repaintTimer_.setInterval(33);
        QObject::connect(&this->repaintTimer_, &QTimer::timeout, this, [this] {
            if (!this->needsContinuousRepaint())
            {
                this->repaintTimer_.stop();
                return;
            }
            this->update();
        });
    }

    void setPreview(QString displayName, QString login, QColor color,
                    QString paintId, bool useActivePaint,
                    QVector<PreviewBadge> badges)
    {
        this->displayName_ = std::move(displayName);
        this->login_ = std::move(login);
        this->color_ = color;
        this->paintId_ = std::move(paintId);
        this->useActivePaint_ = useActivePaint;
        this->badges_ = std::move(badges);
        this->paintPreviewCache_ = {};
        this->updateRepaintTimer();
        this->update();
    }

    void invalidatePaintPreview()
    {
        this->paintPreviewCache_ = {};
        this->updateRepaintTimer();
        this->update();
    }

    void setUiScale(float scale)
    {
        this->uiScale_ = scale;
        this->setFixedHeight(qRound(60 * scale));
        this->invalidatePaintPreview();
    }

protected:
    void hideEvent(QHideEvent *event) override
    {
        this->repaintTimer_.stop();
        QWidget::hideEvent(event);
    }

    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        this->updateRepaintTimer();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.fillRect(this->rect(), getTheme()->splits.input.background);

        const int baseBadgeSize = qRound(22 * this->uiScale_);
        const int baseSpacing = qRound(3 * this->uiScale_);
        const int baseGap = qRound(5 * this->uiScale_);
        const int baseFontSize = qRound(26 * this->uiScale_);

        auto paint =
            this->paintId_.isEmpty()
                ? std::shared_ptr<Paint>{}
                : getApp()->getSeventvPaints()->getPaintByID(this->paintId_);
        if (!paint && this->useActivePaint_)
        {
            paint = getApp()->getSeventvPaints()->getPaint(this->login_, false);
        }
        if (paint)
        {
            paint->ensureLoaded();
        }

        auto font = makeResolvedFont(this->font(), QFont::Bold);
        font.setPixelSize(baseFontSize);
        const QFontMetricsF baseMetrics(font);
        const qreal badgesWidth =
            this->badges_.empty()
                ? 0.0
                : this->badges_.size() * baseBadgeSize +
                      (this->badges_.size() - 1) * baseSpacing;
        const qreal nameWidth =
            baseMetrics.horizontalAdvance(this->displayName_);
        const qreal naturalWidth =
            badgesWidth + (this->badges_.empty() ? 0 : baseGap) + nameWidth;
        const qreal availableWidth = std::max(1, this->width() - 16);
        const qreal identityScale =
            naturalWidth > availableWidth ? availableWidth / naturalWidth : 1.0;
        const int badgeSize =
            std::max(1, qRound(baseBadgeSize * identityScale));
        const int spacing = std::max(0, qRound(baseSpacing * identityScale));
        const int gap = this->badges_.empty()
                            ? 0
                            : std::max(1, qRound(baseGap * identityScale));
        font.setPixelSize(std::max(6, qRound(baseFontSize * identityScale)));
        painter.setFont(font);
        const QFontMetricsF metrics(font);
        const auto paintScale = identityScale * this->uiScale_;
        const auto margins = paint && paint->loaded()
                                 ? paint->getShadowMargins(paintScale)
                                 : QMarginsF{};
        const qreal renderedBadgesWidth =
            this->badges_.empty() ? 0.0
                                  : this->badges_.size() * badgeSize +
                                        (this->badges_.size() - 1) * spacing;
        const qreal renderedNameWidth =
            metrics.horizontalAdvance(this->displayName_);
        const qreal renderedWidth =
            renderedBadgesWidth + gap + renderedNameWidth;
        qreal x = (this->width() - renderedWidth) / 2.0;
        const int top = (this->height() - badgeSize) / 2;
        for (int i = 0; i < this->badges_.size(); ++i)
        {
            const auto &badge = this->badges_.at(i);
            const auto pixmap = badge.image == nullptr
                                    ? std::optional<QPixmap>{}
                                    : badge.image->pixmapOrLoad();
            if (pixmap && !pixmap->isNull())
            {
                painter.drawPixmap(QRect{qRound(x), top, badgeSize, badgeSize},
                                   *pixmap, pixmap->rect());
            }
            else
            {
                auto placeholder = getTheme()->window.text;
                placeholder.setAlpha(32);
                painter.fillRect(QRect{qRound(x), top, badgeSize, badgeSize},
                                 placeholder);
            }
            x += badgeSize;
            if (i + 1 < this->badges_.size())
            {
                x += spacing;
            }
        }
        x += gap;

        const qreal textTop = this->badges_.empty()
                                  ? (this->height() - metrics.height()) / 2.0
                                  : top + (badgeSize - metrics.height()) / 2.0;
        const qreal baseline = textTop + metrics.ascent();
        if (paint && paint->loaded())
        {
            const QSizeF textSize(metrics.horizontalAdvance(this->displayName_),
                                  metrics.height());
            const QSizeF canvasSize(
                textSize.width() + margins.left() + margins.right(),
                textSize.height() + margins.top() + margins.bottom());
            const auto dpr = this->devicePixelRatioF();
            if (this->paintPreviewCache_.isNull() || paint->animated() ||
                !qFuzzyCompare(this->paintPreviewScale_, identityScale) ||
                !qFuzzyCompare(this->paintPreviewDpr_, dpr))
            {
                this->paintPreviewCache_ = paint->getPixmap(
                    this->displayName_, font, this->color_, canvasSize,
                    paintScale, dpr, margins, false);
                this->paintPreviewScale_ = identityScale;
                this->paintPreviewDpr_ = dpr;
            }
            painter.drawPixmap(
                QPointF{x - margins.left(), textTop - margins.top()},
                this->paintPreviewCache_);
        }
        else
        {
            painter.setPen(this->color_.isValid() ? this->color_
                                                  : QColor("#B0B0B0"));
            painter.drawText(QPointF{x, baseline}, this->displayName_);
        }
    }

private:
    bool needsContinuousRepaint() const
    {
        for (const auto &badge : this->badges_)
        {
            if (badge.image != nullptr &&
                ((!badge.image->loaded() && !badge.image->isEmpty()) ||
                 badge.image->animated()))
            {
                return true;
            }
        }

        auto paint =
            this->paintId_.isEmpty()
                ? std::shared_ptr<Paint>{}
                : getApp()->getSeventvPaints()->getPaintByID(this->paintId_);
        if (!paint && this->useActivePaint_)
        {
            paint = getApp()->getSeventvPaints()->getPaint(this->login_, false);
        }
        return paint != nullptr &&
               ((!paint->loaded() && !paint->failed()) || paint->animated());
    }

    void updateRepaintTimer()
    {
        if (this->isVisible() && this->needsContinuousRepaint())
        {
            if (!this->repaintTimer_.isActive())
            {
                this->repaintTimer_.start();
            }
        }
        else
        {
            this->repaintTimer_.stop();
        }
    }

    QString displayName_;
    QString login_;
    QColor color_;
    QString paintId_;
    bool useActivePaint_ = false;
    QVector<PreviewBadge> badges_;
    QPixmap paintPreviewCache_;
    qreal paintPreviewScale_ = 0.0;
    qreal paintPreviewDpr_ = 0.0;
    QTimer repaintTimer_{this};
    float uiScale_ = 1.F;
};

class VanityColorPreview final : public QWidget
{
public:
    explicit VanityColorPreview(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        this->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    void setColor(const QColor &color)
    {
        if (!color.isValid() || color == this->color_)
        {
            return;
        }
        this->color_ = color;
        this->update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(getTheme()->splits.header.border, 1));
        painter.setBrush(this->color_);
        painter.drawRoundedRect(QRectF(this->rect()).adjusted(0.5, 0.5, -0.5,
                                                              -0.5),
                                2, 2);
    }

private:
    QColor color_{Qt::black};
};

class VanityLinkLabel final : public QLabel
{
public:
    explicit VanityLinkLabel(const QString &text, QWidget *parent = nullptr)
        : QLabel(text, parent)
    {
        this->setOpenExternalLinks(false);
        this->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
        this->setMouseTracking(true);
        QObject::connect(this, &QLabel::linkActivated, this,
                         [](const QString &link) {
                             if (isSafeHttpsUrl(link))
                             {
                                 QDesktopServices::openUrl(QUrl(link));
                             }
                         });
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        QLabel::mousePressEvent(event);
        if (event->button() == Qt::LeftButton)
        {
            event->accept();
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        QLabel::mouseReleaseEvent(event);
        if (event->button() == Qt::LeftButton)
        {
            event->accept();
        }
    }
};

class VanityColorPicker final : public QWidget
{
public:
    explicit VanityColorPicker(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        this->setObjectName(QStringLiteral("VanityColorPicker"));
        this->setMinimumHeight(108);
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

        this->setMouseTracking(true);
        this->setCursor(Qt::CrossCursor);
        this->setColor(QColor(QStringLiteral("#FF7F50")));
    }

    void setColor(const QColor &color)
    {
        if (!color.isValid())
        {
            return;
        }
        const auto hue = color.hsvHueF();
        if (hue >= 0.0F)
        {
            this->hue_ = hue;
        }
        this->saturation_ = color.hsvSaturationF();
        this->value_ = color.valueF();
        this->update();
    }

    std::function<void(const QColor &)> colorChanged;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto field = this->fieldRect();
        const auto hueBar = this->hueRect();

        QLinearGradient saturation(field.topLeft(), field.topRight());
        saturation.setColorAt(0.0, Qt::white);
        saturation.setColorAt(1.0, QColor::fromHsvF(this->hue_, 1.0, 1.0));
        painter.fillRect(field, saturation);
        QLinearGradient value(field.topLeft(), field.bottomLeft());
        value.setColorAt(0.0, QColor(0, 0, 0, 0));
        value.setColorAt(1.0, Qt::black);
        painter.fillRect(field, value);

        QLinearGradient hues(hueBar.topLeft(), hueBar.topRight());
        for (int i = 0; i <= 6; ++i)
        {
            hues.setColorAt(i / 6.0, QColor::fromHsvF(i / 6.0, 1.0, 1.0));
        }
        painter.fillRect(hueBar, hues);

        auto border = getTheme()->splits.header.border;
        painter.setPen(border);
        painter.drawRect(field.adjusted(0, 0, -1, -1));
        painter.drawRect(hueBar.adjusted(0, 0, -1, -1));

        const QPointF fieldPoint{
            field.left() + this->saturation_ * (field.width() - 1),
            field.top() + (1.0 - this->value_) * (field.height() - 1)};
        painter.setPen(QPen(Qt::black, 3));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(fieldPoint, 5, 5);
        painter.setPen(QPen(Qt::white, 1));
        painter.drawEllipse(fieldPoint, 5, 5);

        const auto hueX = hueBar.left() + this->hue_ * (hueBar.width() - 1);
        painter.setPen(QPen(Qt::black, 3));
        painter.drawLine(QPointF{hueX, qreal(hueBar.top() - 1)},
                         QPointF{hueX, qreal(hueBar.bottom() + 1)});
        painter.setPen(QPen(Qt::white, 1));
        painter.drawLine(QPointF{hueX, qreal(hueBar.top() - 1)},
                         QPointF{hueX, qreal(hueBar.bottom() + 1)});
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton)
        {
            return;
        }
        const auto point = event->position().toPoint();
        const bool huePressed =
            this->hueRect().adjusted(0, -4, 0, 4).contains(point);
        if (!huePressed && !this->fieldRect().contains(point))
        {
            this->dragging_ = false;
            event->accept();
            return;
        }
        this->dragging_ = true;
        this->draggingHue_ = huePressed;
        this->updateFromPoint(point);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (this->dragging_ && event->buttons().testFlag(Qt::LeftButton))
        {
            this->updateFromPoint(event->position().toPoint());
            event->accept();
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
        {
            this->dragging_ = false;
            this->draggingHue_ = false;
            event->accept();
        }
    }

private:
    QRect fieldRect() const
    {
        return QRect{0, 0, this->width(), this->height() - 25};
    }

    QRect hueRect() const
    {
        return QRect{0, this->height() - 17, this->width(), 13};
    }

    void updateFromPoint(const QPoint &point)
    {
        if (this->draggingHue_)
        {
            const auto rect = this->hueRect();
            this->hue_ = std::clamp((point.x() - rect.left()) /
                                        double(std::max(1, rect.width() - 1)),
                                    0.0, 1.0);
        }
        else
        {
            const auto rect = this->fieldRect();
            this->saturation_ =
                std::clamp((point.x() - rect.left()) /
                               double(std::max(1, rect.width() - 1)),
                           0.0, 1.0);
            this->value_ =
                1.0 - std::clamp((point.y() - rect.top()) /
                                     double(std::max(1, rect.height() - 1)),
                                 0.0, 1.0);
        }
        const auto color =
            QColor::fromHsvF(this->hue_, this->saturation_, this->value_);
        this->update();
        if (this->colorChanged)
        {
            this->colorChanged(color);
        }
    }

    double hue_ = 0.0;
    double saturation_ = 1.0;
    double value_ = 1.0;
    bool dragging_ = false;
    bool draggingHue_ = false;
};

class VanityChoiceTile final : public QAbstractButton
{
public:
    VanityChoiceTile(VanityChoice choice, VanityChoiceKind kind,
                     QString previewText, QColor previewColor,
                     QWidget *parent = nullptr)
        : QAbstractButton(parent)
        , choice_(std::move(choice))
        , kind_(kind)
        , previewText_(std::move(previewText))
        , previewColor_(previewColor)
    {
        this->setCursor(Qt::PointingHandCursor);
        this->setMouseTracking(true);
        this->setFocusPolicy(Qt::StrongFocus);
        this->setAccessibleName(this->choice_.name);
        this->setAccessibleDescription(this->choice_.description);
        this->setCheckable(true);
        this->setToolTip(this->choice_.description.isEmpty()
                             ? this->choice_.name
                             : this->choice_.name + QChar('\n') +
                                   this->choice_.description);
        if (!this->choice_.imageUrl.isEmpty())
        {
            this->image_ =
                Image::fromUrl(Url{this->choice_.imageUrl}, 1, QSize{36, 36});
        }
        this->setFixedSize(kind == VanityChoiceKind::Paint ? QSize{96, 46}
                                                           : QSize{72, 58});
    }

    const QString &key() const
    {
        return this->choice_.key;
    }

    const VanityChoice &choice() const
    {
        return this->choice_;
    }

    void setUiScale(float scale)
    {
        this->uiScale_ = scale;
        this->invalidateVisual();
    }

    void setSelected(bool selected)
    {
        this->setChecked(selected);
    }

    void setTileSize(int width, int height)
    {
        if (this->width() == width && this->height() == height)
        {
            return;
        }
        this->paintPreviewCache_ = {};
        this->setFixedSize(width, height);
    }

    void setPaintPreview(const QString &text, const QColor &color)
    {
        if (this->previewText_ == text && this->previewColor_ == color)
        {
            return;
        }
        this->previewText_ = text;
        this->previewColor_ = color;
        this->paintPreviewCache_ = {};
        this->update();
    }

    void invalidateVisual()
    {
        this->paintPreviewCache_ = {};
        this->update();
    }

    bool needsContinuousRepaint() const
    {
        if (this->kind_ == VanityChoiceKind::Badge)
        {
            return this->image_ != nullptr &&
                   ((!this->image_->loaded() && !this->image_->isEmpty()) ||
                    this->image_->animated());
        }

        auto sampleFont = makeResolvedFont(this->font(), QFont::Bold);
        sampleFont.setPixelSize(14);
        if (QFontMetricsF(sampleFont).horizontalAdvance(this->previewText_) >
            std::max(0.0, this->width() / double(this->uiScale_) - 8))
        {
            return true;
        }
        if (this->choice_.key.isEmpty())
        {
            return false;
        }

        const auto *paints = getApp()->getSeventvPaints();
        const auto paint = paints->getPaintByID(this->choice_.key);
        if (paint)
        {
            return (!paint->loaded() && !paint->failed()) || paint->animated();
        }
        return paints->getPaintLoadStatus(this->choice_.key) ==
               SeventvPaintLoadStatus::Loading;
    }

    QString searchableText() const
    {
        return this->choice_.name + QChar(' ') + this->choice_.shortName;
    }

protected:
    void changeEvent(QEvent *event) override
    {
        QAbstractButton::changeEvent(event);
        if (event->type() == QEvent::FontChange)
        {
            this->invalidateVisual();
        }
    }

    void enterEvent(QEnterEvent *) override
    {
        this->hovered_ = true;
        this->update();
    }

    void leaveEvent(QEvent *) override
    {
        this->hovered_ = false;
        this->update();
    }

    void paintEvent(QPaintEvent *) override
    {
        this->paintPreviewFailed_ = false;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);

        painter.scale(this->uiScale_, this->uiScale_);
        const int width = qRound(this->width() / this->uiScale_);
        const int height = qRound(this->height() / this->uiScale_);

        auto background = getTheme()->splits.input.background;
        const auto border = getTheme()->splits.header.border;
        const auto accent = getTheme()->tabs.selected.backgrounds.regular;
        const auto text = getTheme()->window.text;
        auto muted = text;
        muted.setAlpha(155);
        if (this->isChecked())
        {
            background = accent;
            background.setAlpha(getTheme()->isLightTheme() ? 46 : 60);
        }
        else if (this->hovered_)
        {
            background = getTheme()->splits.header.background;
        }
        painter.fillRect(QRect(0, 0, width, height), background);
        QPen tileBorder(this->isChecked() ? accent : border,
                        this->isChecked() ? 2 : 1);
        if (!this->choice_.owned && !this->isChecked())
        {
            tileBorder.setStyle(Qt::DashLine);
        }
        painter.setPen(tileBorder);
        painter.drawRect(QRect(0, 0, width, height).adjusted(0, 0, -1, -1));

        if (this->kind_ == VanityChoiceKind::Badge)
        {
            const QRect imageRect{(width - 27) / 2, 4, 27, 27};
            if (this->choice_.key.isEmpty())
            {
                const QRectF symbolRect =
                    QRectF(imageRect).adjusted(5.5, 5.5, -5.5, -5.5);
                QPen symbolPen(text, 1.5);
                symbolPen.setCapStyle(Qt::RoundCap);
                painter.setPen(symbolPen);
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(symbolRect);
                painter.drawLine(symbolRect.topRight(),
                                 symbolRect.bottomLeft());
            }
            else if (this->image_)
            {
                const auto pixmap = this->image_->pixmapOrLoad();
                if (pixmap && !pixmap->isNull())
                {
                    if (!this->choice_.owned && !this->isChecked())
                    {
                        painter.setOpacity(0.62);
                    }
                    painter.drawPixmap(imageRect, *pixmap, pixmap->rect());
                    painter.setOpacity(1.0);
                }
            }

            if (!this->choice_.owned)
            {
                const QRectF lockBody{width - 13.0, 5.0, 8.0, 7.0};
                QPen lockPen(text, 1.1);
                lockPen.setCapStyle(Qt::RoundCap);
                painter.setPen(lockPen);
                painter.setBrush(background);
                painter.drawRoundedRect(lockBody, 1.0, 1.0);
                painter.drawArc(QRectF{width - 11.5, 1.5, 5.0, 7.0}, 0,
                                180 * 16);
            }

            auto labelFont = this->font();
            labelFont.setPixelSize(11);
            painter.setFont(labelFont);
            painter.setPen(this->isChecked() ? text : muted);
            const QFontMetrics metrics(labelFont);
            const auto source = this->choice_.shortName.isEmpty()
                                    ? this->choice_.name
                                    : this->choice_.shortName;
            const auto label =
                metrics.elidedText(source, Qt::ElideRight, width - 8);
            painter.drawText(QRect{4, 34, width - 8, 18},
                             Qt::AlignHCenter | Qt::AlignTop, label);
            return;
        }

        auto sampleFont = makeResolvedFont(this->font(), QFont::Bold);
        sampleFont.setPixelSize(14);
        painter.setFont(sampleFont);
        const QFontMetricsF sampleMetrics(sampleFont);
        const QRectF sampleRect{4.0, 2.0, width - 8.0, 23.0};
        const auto drawScrollingText = [&](qreal contentWidth,
                                           const auto &drawAt) {
            if (contentWidth <= sampleRect.width())
            {
                drawAt(sampleRect.left() +
                       (sampleRect.width() - contentWidth) / 2.0);
                return;
            }

            constexpr qreal SPEED_PIXELS_PER_SECOND = 28.0;
            const qreal gap = std::max<qreal>(14.0, sampleRect.width() / 4.0);
            const qreal cycleWidth = contentWidth + gap;
            const qreal elapsedSeconds =
                QDateTime::currentMSecsSinceEpoch() / 1000.0;
            const qreal offset =
                std::fmod(elapsedSeconds * SPEED_PIXELS_PER_SECOND, cycleWidth);

            painter.save();
            painter.setClipRect(sampleRect);
            qreal x = sampleRect.left() - offset;
            while (x + contentWidth < sampleRect.left())
            {
                x += cycleWidth;
            }
            drawAt(x);
            drawAt(x + cycleWidth);
            painter.restore();
        };

        if (this->choice_.key.isEmpty())
        {
            painter.setPen(this->previewColor_.isValid() ? this->previewColor_
                                                         : text);
            const qreal baseline =
                sampleRect.top() +
                (sampleRect.height() + sampleMetrics.ascent() -
                 sampleMetrics.descent()) /
                    2.0;
            const qreal contentWidth =
                sampleMetrics.horizontalAdvance(this->previewText_);
            drawScrollingText(contentWidth, [&](qreal x) {
                painter.drawText(QPointF{x, baseline}, this->previewText_);
            });
        }
        else
        {
            const auto paint =
                getApp()->getSeventvPaints()->getPaintByID(this->choice_.key);
            if (paint)
            {
                paint->ensureLoaded();
                this->paintPreviewFailed_ = paint->failed();
            }
            else
            {
                const auto status =
                    getApp()->getSeventvPaints()->getPaintLoadStatus(
                        this->choice_.key);
                this->paintPreviewFailed_ =
                    status == SeventvPaintLoadStatus::Failed ||
                    status == SeventvPaintLoadStatus::NotFound;
            }
            if (paint && paint->loaded())
            {
                const auto margins = paint->getShadowMargins(0.8F);
                const QSizeF textSize(
                    sampleMetrics.horizontalAdvance(this->previewText_),
                    sampleMetrics.height());
                const QSizeF canvasSize(
                    textSize.width() + margins.left() + margins.right(),
                    textSize.height() + margins.top() + margins.bottom());
                const auto dpr = this->devicePixelRatioF() * this->uiScale_;
                if (this->paintPreviewCache_.isNull() || paint->animated() ||
                    !qFuzzyCompare(this->paintPreviewDpr_, dpr))
                {
                    this->paintPreviewCache_ = paint->getPixmap(
                        this->previewText_, sampleFont, this->previewColor_,
                        canvasSize, 0.8F, dpr, margins, false);
                    this->paintPreviewDpr_ = dpr;
                }
                const qreal textTop =
                    sampleRect.top() +
                    (sampleRect.height() - textSize.height()) / 2.0;
                drawScrollingText(textSize.width(), [&](qreal x) {
                    painter.drawPixmap(
                        QPointF{x - margins.left(), textTop - margins.top()},
                        this->paintPreviewCache_);
                });
            }
            else
            {
                painter.setPen(muted);
                painter.drawText(sampleRect, Qt::AlignCenter,
                                 this->previewText_);
            }

            if (this->paintPreviewFailed_)
            {
                auto retryColor = text;
                retryColor.setAlpha(this->hovered_ ? 230 : 145);
                QPen retryPen(retryColor, 1.35);
                retryPen.setCapStyle(Qt::RoundCap);
                retryPen.setJoinStyle(Qt::RoundJoin);
                painter.setPen(retryPen);
                painter.setBrush(Qt::NoBrush);

                const QRectF retryRect(width - 14.0, 4.0, 8.0, 8.0);
                painter.drawArc(retryRect, 40 * 16, 285 * 16);
                const QPointF arrowTip(retryRect.right() - 0.4,
                                       retryRect.top() + 1.7);
                painter.drawLine(arrowTip, arrowTip + QPointF{-3.0, -0.2});
                painter.drawLine(arrowTip, arrowTip + QPointF{-0.4, 3.0});
            }
        }

        auto tooltip = this->choice_.name;
        if (!this->choice_.description.isEmpty())
        {
            tooltip += QChar('\n') + this->choice_.description;
        }
        if (this->paintPreviewFailed_)
        {
            tooltip += QStringLiteral(
                "\nPreview unavailable. Select it to try again.");
        }
        if (tooltip != this->toolTip())
        {
            this->setToolTip(tooltip);
        }

        auto labelFont = this->font();
        labelFont.setPixelSize(10);
        painter.setFont(labelFont);
        painter.setPen(muted);
        const QFontMetrics labelMetrics(labelFont);
        const auto source = this->choice_.shortName.isEmpty()
                                ? this->choice_.name
                                : this->choice_.shortName;
        const auto label =
            labelMetrics.elidedText(source, Qt::ElideRight, width - 8);
        painter.drawText(QRect{4, 28, width - 8, 15}, Qt::AlignCenter, label);
    }

private:
    VanityChoice choice_;
    VanityChoiceKind kind_;
    QString previewText_;
    QColor previewColor_;
    ImagePtr image_;
    QPixmap paintPreviewCache_;
    qreal paintPreviewDpr_ = 0.0;
    bool hovered_ = false;
    bool paintPreviewFailed_ = false;
    float uiScale_ = 1.F;
};

class VanityChoiceGrid final : public QScrollArea
{
public:
    VanityChoiceGrid(VanityChoiceKind kind, int columns,
                     QWidget *parent = nullptr)
        : QScrollArea(parent)
        , kind_(kind)
        , preferredColumns_(columns)
    {
        this->setObjectName(QStringLiteral("VanityChoiceGrid"));
        this->setFrameShape(QFrame::NoFrame);
        this->setWidgetResizable(true);
        this->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        this->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        this->content_ = new QWidget(this);
        this->content_->setObjectName(
            QStringLiteral("VanityChoiceGridContent"));
        this->grid_ = new QGridLayout(this->content_);
        this->grid_->setContentsMargins(2, 2, 2, 2);
        this->grid_->setSpacing(4);
        this->grid_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        this->setWidget(this->content_);
    }

    void setChoices(QVector<VanityChoice> choices, const QString &selected,
                    const QString &previewText, const QColor &previewColor)
    {
        while (auto *item = this->grid_->takeAt(0))
        {
            delete item;
        }
        qDeleteAll(this->tiles_);
        this->tiles_.clear();
        this->tilesByKey_.clear();
        this->selected_ = selected;
        for (auto &choice : choices)
        {
            auto *tile =
                new VanityChoiceTile(std::move(choice), this->kind_,
                                     previewText, previewColor, this->content_);
            tile->setUiScale(this->uiScale_);
            tile->setSelected(tile->key() == this->selected_);
            const auto visualKey =
                this->kind_ == VanityChoiceKind::Paint
                    ? SeventvPaints::normalizePaintID(tile->key())
                    : tile->key();
            this->tilesByKey_.insert(visualKey, tile);
            QObject::connect(tile, &QAbstractButton::clicked, this,
                             [this, tile] {
                                 this->setSelected(tile->key());
                                 if (this->selectionChanged)
                                 {
                                     this->selectionChanged(tile->choice());
                                 }
                             });
            this->tiles_.push_back(tile);
        }
        this->applyFilter();
        this->startVisualRefresh();
    }

    void setUiScale(float scale)
    {
        if (this->uiScale_ == scale)
        {
            return;
        }
        this->uiScale_ = scale;
        this->grid_->setSpacing(qRound(4 * scale));
        for (auto *tile : this->tiles_)
        {
            tile->setUiScale(scale);
        }
        this->applyFilter();
    }

    void setSelected(const QString &selected)
    {
        bool changed = this->selected_ != selected;
        this->selected_ = selected;
        for (auto *tile : this->tiles_)
        {
            const bool shouldSelect = tile->key() == selected;
            if (tile->isChecked() != shouldSelect)
            {
                tile->setSelected(shouldSelect);
                changed = true;
            }
        }
        if (changed)
        {
            this->startVisualRefresh();
        }
    }

    void clearSelectedDisplay()
    {
        for (auto *tile : this->tiles_)
        {
            tile->setSelected(false);
        }
    }

    void updateChoiceVisual(const QString &key, bool invalidate = false)
    {
        const auto visualKey = this->kind_ == VanityChoiceKind::Paint
                                   ? SeventvPaints::normalizePaintID(key)
                                   : key;
        auto *tile = this->tilesByKey_.value(visualKey, nullptr);
        if (tile == nullptr || !this->isTileVisible(tile))
        {
            return;
        }
        if (invalidate)
        {
            tile->invalidateVisual();
        }
        else
        {
            tile->update();
        }
    }

    void setPaintPreview(const QString &text, const QColor &color)
    {
        for (auto *tile : this->tiles_)
        {
            tile->setPaintPreview(text, color);
        }
    }

    void setFilter(QString filter)
    {
        this->filter_ = filter.trimmed();
        this->applyFilter();
        this->startVisualRefresh();
    }

    void updateVisibleTiles(bool continuousOnly = false,
                            bool invalidate = false)
    {
        for (auto *tile : this->tiles_)
        {
            if (this->isTileVisible(tile) &&
                (!continuousOnly || tile->needsContinuousRepaint()))
            {
                if (invalidate)
                {
                    tile->invalidateVisual();
                }
                else
                {
                    tile->update();
                }
            }
        }
    }

    bool hasVisibleContinuousTiles() const
    {
        return std::ranges::any_of(this->tiles_, [this](const auto *tile) {
            return this->isTileVisible(tile) && tile->needsContinuousRepaint();
        });
    }

    std::function<void(const VanityChoice &)> selectionChanged;
    std::function<void()> visualActivityStarted;

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QScrollArea::resizeEvent(event);
        this->applyFilter();
        this->startVisualRefresh();
    }

private:
    bool isTileVisible(const QWidget *tile) const
    {
        if (tile == nullptr || !tile->isVisibleTo(this->viewport()))
        {
            return false;
        }
        const QRect tileRect(tile->mapTo(this->viewport(), QPoint{}),
                             tile->size());
        return this->viewport()->rect().intersects(tileRect);
    }

    void applyFilter()
    {
        if (this->reflowing_)
        {
            return;
        }
        this->reflowing_ = true;
        while (auto *item = this->grid_->takeAt(0))
        {
            delete item;
        }
        const int spacing = this->grid_->horizontalSpacing();
        const int availableWidth = std::max(1, this->viewport()->width() - 4);
        const int minimumTileWidth =
            qRound((this->kind_ == VanityChoiceKind::Paint ? 86 : 64) *
                   this->uiScale_);
        const int fittingColumns = std::max(
            1, (availableWidth + spacing) / (minimumTileWidth + spacing));
        const int columns =
            std::max(1, std::min(this->preferredColumns_, fittingColumns));
        const int tileWidth =
            std::max(1, (availableWidth - spacing * (columns - 1)) / columns);
        const int tileHeight =
            qRound((this->kind_ == VanityChoiceKind::Paint ? 46 : 58) *
                   this->uiScale_);
        int visibleIndex = 0;
        for (auto *tile : this->tiles_)
        {
            const bool visible = this->filter_.isEmpty() ||
                                 tile->searchableText().contains(
                                     this->filter_, Qt::CaseInsensitive);
            tile->setVisible(visible);
            if (!visible)
            {
                continue;
            }
            tile->setTileSize(tileWidth, tileHeight);
            this->grid_->addWidget(tile, visibleIndex / columns,
                                   visibleIndex % columns);
            ++visibleIndex;
        }
        this->reflowing_ = false;
        QTimer::singleShot(0, this, [this] {
            this->updateVisibleTiles();
            this->startVisualRefresh();
        });
    }

    void startVisualRefresh()
    {
        if (this->visualActivityStarted)
        {
            this->visualActivityStarted();
        }
    }

    VanityChoiceKind kind_;
    int preferredColumns_;
    QWidget *content_{};
    QGridLayout *grid_{};
    QVector<VanityChoiceTile *> tiles_;
    QHash<QString, VanityChoiceTile *> tilesByKey_;
    QString selected_;
    QString filter_;
    bool reflowing_ = false;
    float uiScale_ = 1.F;
};

QLineEdit *addGallerySearch(QVBoxLayout *layout, VanityChoiceGrid *grid,
                            const QString &placeholder, QWidget *parent)
{
    auto *search = new QLineEdit(parent);
    search->setObjectName(QStringLiteral("VanitySearch"));
    search->setPlaceholderText(placeholder);
    search->setClearButtonEnabled(true);
    search->setFixedHeight(25);
    QObject::connect(search, &QLineEdit::textChanged, grid,
                     [grid](const QString &text) {
                         grid->setFilter(text);
                     });
    layout->addWidget(search);
    return search;
}

class VanityOrderDelegate final : public QStyledItemDelegate
{
public:
    explicit VanityOrderDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    std::function<void(const QString &, bool)> visibilityChanged;
    float uiScale = 1.F;

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &) const override
    {
        return QSize{option.rect.width(),
                     std::max(qRound(32 * this->uiScale),
                              option.fontMetrics.height() + 8)};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        auto background = getTheme()->splits.input.background;
        if (selected)
        {
            background = getTheme()->tabs.selected.backgrounds.regular;
        }
        else if (hovered)
        {
            background = getTheme()->splits.header.background;
        }
        painter->fillRect(option.rect, background);

        auto text =
            selected ? getTheme()->tabs.selected.text : getTheme()->window.text;
        auto muted = text;
        muted.setAlpha(155);

        painter->setPen(Qt::NoPen);
        painter->setBrush(muted);
        const int gripX = option.rect.left() + qRound(10 * this->uiScale);
        const int centerY = option.rect.center().y();
        for (int xOffset : {0, 5})
        {
            for (int yOffset : {-5, 0, 5})
            {
                painter->drawEllipse(QPoint{gripX + xOffset, centerY + yOffset},
                                     1, 1);
            }
        }

        painter->setPen(text);
        painter->setFont(option.font);
        const QRect textRect = option.rect.adjusted(
            qRound(30 * this->uiScale), 0, -qRound(42 * this->uiScale), 0);
        painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                          index.data(Qt::DisplayRole).toString());

        const bool visible = index.data(ORDER_VISIBLE_ROLE).toBool();
        const auto eyeRect =
            orderEyeRect(option.rect, this->uiScale)
                .adjusted(qRound(7 * this->uiScale), qRound(8 * this->uiScale),
                          -qRound(7 * this->uiScale),
                          -qRound(8 * this->uiScale));
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(visible ? text : muted, 1.5));
        QPainterPath eye;
        eye.moveTo(eyeRect.left(), eyeRect.center().y());
        eye.cubicTo(eyeRect.left() + 5, eyeRect.top(), eyeRect.right() - 5,
                    eyeRect.top(), eyeRect.right(), eyeRect.center().y());
        eye.cubicTo(eyeRect.right() - 5, eyeRect.bottom(), eyeRect.left() + 5,
                    eyeRect.bottom(), eyeRect.left(), eyeRect.center().y());
        painter->drawPath(eye);
        if (visible)
        {
            painter->setBrush(text);
            painter->drawEllipse(eyeRect.center(), 2, 2);
        }
        else
        {
            painter->drawLine(eyeRect.topLeft(), eyeRect.bottomRight());
        }
        painter->restore();
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *model,
                     const QStyleOptionViewItem &option,
                     const QModelIndex &index) override
    {
        if (event->type() != QEvent::MouseButtonRelease)
        {
            return false;
        }
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::LeftButton ||
            !orderEyeRect(option.rect, this->uiScale).contains(mouse->pos()))
        {
            return false;
        }
        const bool visible = index.data(ORDER_VISIBLE_ROLE).toBool();
        const auto key = index.data(ORDER_KEY_ROLE).toString();
        const bool changed =
            model->setData(index, !visible, ORDER_VISIBLE_ROLE);
        if (changed)
        {
            model->setData(index,
                           visible ? QStringLiteral("Hidden in chat")
                                   : QStringLiteral("Visible in chat"),
                           Qt::ToolTipRole);
            if (this->visibilityChanged)
            {
                this->visibilityChanged(key, !visible);
            }
        }
        return changed;
    }
};

VanityDialog::VanityDialog(std::shared_ptr<TwitchChannel> channel,
                           QWidget *parent)
    : DraggablePopup(true, parent)
    , channel_(std::move(channel))
{
    const auto account = getApp()->getAccounts()->twitch.getCurrent();
    this->accountUserId_ = account->getUserId();
    this->accountLogin_ = account->getUserName();
    this->accountDisplayName_ = this->accountLogin_;
    this->accountToken_ = account->getOAuthToken();
    this->accountClientId_ = account->getOAuthClient();
    this->refreshTwitchAuthCandidates();

    this->setObjectName(QStringLiteral("VanityDialog"));
    this->setWindowTitle(QStringLiteral("Vanity"));
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->enableResize(getSettings()->vanityPopupSize, DEFAULT_SIZE);
    this->buildUi();
    this->managedConnections_.managedConnect(
        getApp()->getAccounts()->twitch.currentUserChanged, [this] {
            this->accountChanged_ = true;
        });
    getSettings()->sevenTVVanityToken.connect(
        [this] {
            ++this->sevenTVAuthGeneration_;
        },
        this->managedConnections_, false);
    getSettings()->moltorinoAuthAccounts.connect(
        [this] {
            if (!this->isOriginalAccountCurrent())
            {
                this->accountChanged_ = true;
            }
        },
        this->managedConnections_, false);
    this->managedConnections_.managedConnect(
        getApp()->getSeventvPaints()->paintChanged,
        [this](const QString &login, bool kick) {
            if (!kick && !this->sevenTVLoaded_ &&
                login.compare(this->accountLogin_, Qt::CaseInsensitive) == 0)
            {
                this->preview_->invalidatePaintPreview();
            }
        });
    this->managedConnections_.managedConnect(
        getApp()->getSeventvPaints()->paintLoadStatusChanged,
        [this](const QString &paintID, SeventvPaintLoadStatus) {
            if (this->paintGrid_ != nullptr)
            {
                this->paintGrid_->updateChoiceVisual(paintID, true);
            }
            if (this->preview_ != nullptr &&
                SeventvPaints::normalizePaintID(this->selectedSevenTVPaint_) ==
                    paintID)
            {
                this->preview_->invalidatePaintPreview();
            }
            this->updateVisualRefreshTimer();
        });

    this->managedConnections_.managedConnect(
        getApp()->getWindows()->layoutRequested, [this](Channel *) {
            this->refreshLoadedImages();
        });
    if (auto *provider = getApp()->getMoltorinoSupporterBadges())
    {
        this->managedConnections_.managedConnect(
            provider->badgesUpdated, [this] {
                this->moltorinoBadgeChoicesBuilt_ = false;
                this->refreshMoltorinoBadgeAvailability();
                this->populateCurrentTab();
                this->refreshPreview();
            });
    }
    if (auto *provider = getApp()->getFfzApBadges())
    {
        this->managedConnections_.managedConnect(provider->badgesUpdated,
                                                  [this] {
                                                      this->refreshPreview();
                                                  });
    }
    if (auto *provider = getApp()->getBluzyrinoBadges())
    {
        this->managedConnections_.managedConnect(provider->badgesUpdated,
                                                 [this] {
                                                     this->refreshPreview();
                                                 });
    }
    this->refreshMoltorinoBadgeAvailability();

    this->originalColor_ = account->color();
    if (!this->originalColor_.isValid())
    {
        this->originalColor_ = QColor("#B0B0B0");
    }
    this->selectedColor_ = this->originalColor_;
    this->selectedColorValue_ = this->selectedColor_.name();
    for (int i = 0; i < int(std::size(TWITCH_COLORS)); ++i)
    {
        if (QColor(TWITCH_COLORS[i].hex) == this->selectedColor_)
        {
            this->selectedColorValue_ =
                QString::fromUtf8(TWITCH_COLORS[i].value);
            if (auto *button = this->colorButtons_->button(i))
            {
                button->setChecked(true);
            }
            break;
        }
    }
    this->refreshStyle();
    this->refreshHeaderIdentity();
    QTimer::singleShot(0, this, [this] {
        this->loadTwitchState();
        this->loadLayoutState();
        this->loadSevenTVState();
        this->populateCurrentTab();
        this->refreshPreview();
    });
}

void VanityDialog::showDialog(std::shared_ptr<TwitchChannel> channel,
                              QWidget *parent)
{
    if (channel == nullptr)
    {
        return;
    }

    if (activeDialog_)
    {
        const auto account = getApp()->getAccounts()->twitch.getCurrent();
        const bool sameContext =
            activeDialog_->isOriginalAccountCurrent() &&
            activeDialog_->accountUserId_ == account->getUserId() &&
            activeDialog_->channel_ != nullptr && channel != nullptr &&
            activeDialog_->channel_->getName().compare(
                channel->getName(), Qt::CaseInsensitive) == 0;
        if (sameContext || activeDialog_->saveInFlight_)
        {
            activeDialog_->show();
            activeDialog_->raise();
            activeDialog_->activateWindow();
            return;
        }
        activeDialog_->close();
    }

    auto *dialog = new VanityDialog(std::move(channel), parent);
    activeDialog_ = dialog;

    QPoint center = QCursor::pos();
    if (parent != nullptr && parent->window() != nullptr)
    {
        center = parent->window()->geometry().center();
    }

    dialog->show();
    const auto size = dialog->size();
    dialog->showAndMoveTo(center - QPoint(size.width() / 2, size.height() / 2),
                          widgets::BoundsChecking::DesiredPosition);
    dialog->raise();
    dialog->activateWindow();
}

void VanityDialog::buildUi()
{
    auto *container = this->getLayoutContainer();
    container->setObjectName(QStringLiteral("VanityDialogRoot"));
    this->mainLayout_ = new QVBoxLayout(container);

    this->headerWidget_ = new QWidget(container);
    this->headerWidget_->setObjectName(QStringLiteral("VanityDialogHeader"));
    auto *header = new QHBoxLayout(this->headerWidget_);
    this->headerTitleLabel_ =
        new QLabel(QStringLiteral("Vanity"), this->headerWidget_);
    this->headerTitleLabel_->setObjectName(QStringLiteral("VanityHeaderTitle"));
    this->headerTitleLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    header->addWidget(this->headerTitleLabel_);
    auto *headerDot = new QLabel(QStringLiteral("\u00b7"), this->headerWidget_);
    headerDot->setObjectName(QStringLiteral("VanityHeaderIdentity"));
    headerDot->setAttribute(Qt::WA_TransparentForMouseEvents);
    header->addWidget(headerDot);
    this->headerIdentityLabel_ = new QLabel(this->headerWidget_);
    this->headerIdentityLabel_->setObjectName(
        QStringLiteral("VanityHeaderIdentity"));
    this->headerIdentityLabel_->setAttribute(
        Qt::WA_TransparentForMouseEvents);
    header->addWidget(this->headerIdentityLabel_, 1);

    this->pinButton_ = this->createPinButton();
    this->pinButton_->setToolTip(QStringLiteral("Keep this popup open"));
    this->pinButton_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    header->addWidget(this->pinButton_);

    this->closeButton_ = new SvgButton(
        {
            .dark = ":/buttons/cancel.svg",
            .light = ":/buttons/cancelDark.svg",
        },
        this, QSize{3, 3});
    this->closeButton_->setScaleIndependentSize(18, 18);
    this->closeButton_->setToolTip(QStringLiteral("Close"));
    this->closeButton_->setCursor(Qt::PointingHandCursor);
    QObject::connect(this->closeButton_, &Button::leftClicked, this,
                     &QWidget::close);
    header->addWidget(this->closeButton_);
    this->mainLayout_->addWidget(this->headerWidget_);

    auto *separator = new Line(false);
    separator->setObjectName(QStringLiteral("VanityDialogSeparator"));
    separator->setFixedHeight(HEADER_SEPARATOR_HEIGHT);
    this->mainLayout_->addWidget(separator);

    this->previewFrame_ = new QWidget(container);
    this->previewFrame_->setObjectName(QStringLiteral("VanityPreviewFrame"));
    auto *previewLayout = new QVBoxLayout(this->previewFrame_);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    this->preview_ = new VanityPreviewWidget(this->previewFrame_);
    this->preview_->setToolTip(QStringLiteral("Your current preview"));
    previewLayout->addWidget(this->preview_);
    this->mainLayout_->addWidget(this->previewFrame_);

    this->tabs_ = new QTabWidget(container);
    this->tabs_->setObjectName(QStringLiteral("VanityTabs"));
    this->tabs_->tabBar()->setExpanding(false);
    this->tabs_->tabBar()->setUsesScrollButtons(false);
    this->tabs_->tabBar()->setElideMode(Qt::ElideNone);
    this->colorPage_ = this->buildColorTab();
    this->paintPage_ = this->buildSevenTVPaintTab();
    this->globalBadgePage_ = this->buildTwitchBadgeTab(false);
    this->channelBadgePage_ = this->buildTwitchBadgeTab(true);
    this->sevenTVBadgePage_ = this->buildSevenTVBadgeTab();
    this->moltorinoBadgePage_ = this->buildMoltorinoBadgeTab();
    this->layoutPage_ = this->buildLayoutTab();
    this->tabs_->addTab(this->colorPage_, QStringLiteral("Color"));
    this->tabs_->addTab(this->globalBadgePage_, QStringLiteral("Global"));
    this->tabs_->addTab(this->channelBadgePage_, QStringLiteral("Channel"));
    this->tabs_->addTab(this->sevenTVBadgePage_, QStringLiteral("7TV"));
    this->tabs_->addTab(this->paintPage_, QStringLiteral("Paints"));
    this->tabs_->addTab(this->moltorinoBadgePage_, QStringLiteral("Moltorino"));
    this->tabs_->addTab(this->layoutPage_, QStringLiteral("Order"));
    this->tabs_->setTabToolTip(1, QStringLiteral("Twitch global badge"));
    this->tabs_->setTabToolTip(2, QStringLiteral("Badge for this channel"));
    this->tabs_->setTabToolTip(3, QStringLiteral("7TV badge"));
    this->tabs_->setTabToolTip(4, QStringLiteral("7TV paint"));
    this->tabs_->setTabToolTip(5, QStringLiteral("Moltorino badge"));
    this->tabs_->setTabToolTip(6, QStringLiteral("Badge order and visibility"));
    this->mainLayout_->addWidget(this->tabs_, 1);
    QObject::connect(this->tabs_, &QTabWidget::currentChanged, this,
                     [this](int) {
                         this->populateCurrentTab();
                         this->scheduleResponsiveLayoutRefresh();
                         QTimer::singleShot(
                             0, this, &VanityDialog::updateVisualRefreshTimer);
                     });

    this->statusLabel_ = new QLabel(container);
    this->statusLabel_->setObjectName(QStringLiteral("VanityStatus"));
    this->statusLabel_->setWordWrap(true);

    auto statusPolicy = this->statusLabel_->sizePolicy();
    statusPolicy.setHorizontalPolicy(QSizePolicy::Ignored);
    this->statusLabel_->setSizePolicy(statusPolicy);
    this->statusLabel_->hide();
    this->mainLayout_->addWidget(this->statusLabel_);

    this->saveButton_ = new QPushButton(QStringLiteral("Save"), container);
    this->saveButton_->setObjectName(QStringLiteral("VanitySaveButton"));
    this->saveButton_->setSizePolicy(QSizePolicy::Expanding,
                                     QSizePolicy::Fixed);
    this->saveButton_->setEnabled(false);
    this->mainLayout_->addWidget(this->saveButton_);
    QObject::connect(this->saveButton_, &QPushButton::clicked, this,
                     &VanityDialog::save);

    this->visualRefreshTimer_ = new QTimer(this);
    this->visualRefreshTimer_->setTimerType(Qt::PreciseTimer);
    this->visualRefreshTimer_->setInterval(33);
    QObject::connect(this->visualRefreshTimer_, &QTimer::timeout, this, [this] {
        if (this->tabs_ == nullptr)
        {
            return;
        }
        auto *grid = this->currentChoiceGrid();
        if (grid != nullptr)
        {
            const bool settling = this->visualSettleTicks_ > 0;
            grid->updateVisibleTiles(!settling);
            if (settling)
            {
                --this->visualSettleTicks_;
            }
            if (!grid->hasVisibleContinuousTiles() &&
                this->visualSettleTicks_ <= 0)
            {
                grid->updateVisibleTiles();
                this->visualRefreshTimer_->stop();
            }
        }
        else
        {
            this->visualRefreshTimer_->stop();
        }
    });
    for (auto *grid :
         {this->paintGrid_, this->globalBadgeGrid_, this->channelBadgeGrid_,
          this->sevenTVBadgeGrid_, this->moltorinoBadgeGrid_})
    {
        if (grid != nullptr)
        {
            grid->visualActivityStarted = [this] {
                this->updateVisualRefreshTimer();
            };
            QObject::connect(grid->verticalScrollBar(),
                             &QScrollBar::valueChanged, this,
                             &VanityDialog::updateVisualRefreshTimer);
            QObject::connect(grid->verticalScrollBar(),
                             &QScrollBar::rangeChanged, this, [this] {
                                 QTimer::singleShot(
                                     0, this,
                                     &VanityDialog::updateVisualRefreshTimer);
                             });
        }
    }
}

QWidget *VanityDialog::buildColorTab()
{
    auto *page = new QWidget(this->tabs_);
    page->setObjectName(QStringLiteral("VanityTabPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(7, 7, 7, 7);
    layout->setSpacing(6);

    auto *titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(0, 0, 0, 0);
    auto *colorTitle = new QLabel(QStringLiteral("Twitch color"), page);
    colorTitle->setObjectName(QStringLiteral("VanitySectionTitle"));
    titleRow->addWidget(colorTitle);
    titleRow->addStretch(1);
    this->colorHexInput_ = new QLineEdit(page);
    this->colorHexInput_->setAccessibleName(QStringLiteral("Username color"));
    this->colorHexInput_->setObjectName(QStringLiteral("VanitySearch"));
    this->colorHexInput_->setMaxLength(7);
    this->colorHexInput_->setFixedWidth(82);
    this->colorHexInput_->setAlignment(Qt::AlignCenter);
    this->colorHexInput_->setToolTip(
        QStringLiteral("Enter a six digit hex color"));
    this->colorHexInput_->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("^#?[0-9A-Fa-f]{0,6}$")),
        this->colorHexInput_));
    this->colorPreviewChip_ = new VanityColorPreview(page);
    this->colorPreviewChip_->setObjectName(
        QStringLiteral("VanityColorPreviewChip"));
    this->colorPreviewChip_->setToolTip(
        QStringLiteral("Selected Twitch color"));
    titleRow->addWidget(this->colorPreviewChip_);
    titleRow->addWidget(this->colorHexInput_);
    layout->addLayout(titleRow);

    this->colorPicker_ = new VanityColorPicker(page);
    this->colorPicker_->colorChanged = [this](const QColor &color) {
        this->selectedColor_ = color;
        this->selectedColorValue_ = color.name();
        const QSignalBlocker blocker(this->colorButtons_);
        this->colorButtons_->setExclusive(false);
        for (auto *button : this->colorButtons_->buttons())
        {
            button->setChecked(false);
        }
        this->colorButtons_->setExclusive(true);
        this->refreshPreview();
    };
    layout->addWidget(this->colorPicker_, 1);

    this->colorSwatchLayout_ = new QGridLayout;
    this->colorSwatchLayout_->setContentsMargins(0, 0, 0, 0);
    this->colorSwatchLayout_->setHorizontalSpacing(6);
    this->colorSwatchLayout_->setVerticalSpacing(5);
    this->colorButtons_ = new QButtonGroup(this);
    this->colorButtons_->setExclusive(true);
    for (int i = 0; i < int(std::size(TWITCH_COLORS)); ++i)
    {
        const auto &choice = TWITCH_COLORS[i];
        auto *button = new QPushButton(page);
        button->setCheckable(true);
        button->setFixedSize(30, 22);
        button->setToolTip(QString::fromUtf8(choice.name));
        button->setProperty("vanityColor", QString::fromUtf8(choice.hex));
        this->colorButtons_->addButton(button, i);
        this->colorSwatchLayout_->addWidget(button, i / 8, i % 8);
    }
    this->colorSwatchLayout_->setColumnStretch(8, 1);
    layout->addLayout(this->colorSwatchLayout_);
    QObject::connect(
        this->colorButtons_, &QButtonGroup::idClicked, this, [this](int id) {
            const auto &choice = TWITCH_COLORS[id];
            this->selectedColor_ = QColor(choice.hex);
            this->selectedColorValue_ = QString::fromUtf8(choice.value);
            this->refreshPreview();
        });
    QObject::connect(this->colorHexInput_, &QLineEdit::editingFinished, this,
                     [this] {
                         auto value = this->colorHexInput_->text().trimmed();
                         if (!value.startsWith('#'))
                         {
                             value.prepend('#');
                         }
                         const QColor color(value);
                         if (!color.isValid() || value.size() != 7)
                         {
                             this->colorHexInput_->setText(
                                 this->selectedColor_.name().toUpper());
                             return;
                         }
                         this->selectedColor_ = color;
                         this->selectedColorValue_ = color.name();
                         this->colorButtons_->setExclusive(false);
                         for (auto *button : this->colorButtons_->buttons())
                         {
                             button->setChecked(false);
                         }
                         this->colorButtons_->setExclusive(true);
                         this->refreshPreview();
                     });
    return page;
}

QWidget *VanityDialog::buildSevenTVPaintTab()
{
    auto *page = new QWidget(this->tabs_);
    page->setObjectName(QStringLiteral("VanityTabPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(7, 7, 7, 7);
    layout->setSpacing(5);

    layout->addWidget(this->buildSevenTVConnectionHeader(
        page, QStringLiteral("7TV paint"),
        QStringLiteral("Connect 7TV to choose a paint.")));
    this->paintHint_ = new QLabel(QStringLiteral("Loading paints..."), page);
    this->paintHint_->setObjectName(QStringLiteral("VanityHint"));
    this->paintHint_->hide();
    layout->addWidget(this->paintHint_);
    this->paintGrid_ = new VanityChoiceGrid(VanityChoiceKind::Paint, 4, page);
    this->paintGrid_->selectionChanged = [this](const VanityChoice &choice) {
        this->selectedSevenTVPaint_ =
            SeventvPaints::normalizePaintID(choice.key);
        this->sevenTVPaintTouched_ =
            this->selectedSevenTVPaint_ != this->originalSevenTVPaint_;
        if (!this->selectedSevenTVPaint_.isEmpty())
        {
            auto *paints = getApp()->getSeventvPaints();
            if (const auto paint =
                    paints->getPaintByID(this->selectedSevenTVPaint_))
            {
                paint->ensureLoaded(true);
            }
            else
            {
                paints->loadPaintByID(this->selectedSevenTVPaint_, true);
            }
        }
        this->refreshPreview();
    };
    addGallerySearch(layout, this->paintGrid_, QStringLiteral("Search paints"),
                     page);
    layout->addWidget(this->paintGrid_, 1);
    return page;
}

QWidget *VanityDialog::buildTwitchBadgeTab(bool channelBadges)
{
    auto *page = new QWidget(this->tabs_);
    page->setObjectName(QStringLiteral("VanityTabPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(7, 7, 7, 7);
    layout->setSpacing(6);

    auto *titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(0, 0, 0, 0);
    auto *title =
        new QLabel(channelBadges ? QStringLiteral("Badge shown in #%1")
                                       .arg(this->channel_->getName())
                                 : QStringLiteral("Global Twitch badge"),
                   page);
    title->setObjectName(QStringLiteral("VanitySectionTitle"));
    titleRow->addWidget(title);
    titleRow->addStretch(1);
    auto *retry = new QPushButton(QStringLiteral("Try again"), page);
    retry->setToolTip(QStringLiteral("Reload Twitch badges"));
    retry->hide();
    QObject::connect(retry, &QPushButton::clicked, this,
                     &VanityDialog::retryTwitchState);
    this->twitchRetryButtons_.push_back(retry);
    titleRow->addWidget(retry);
    layout->addLayout(titleRow);

    auto *hint =
        new VanityLinkLabel(QStringLiteral("Loading Twitch badges..."), page);
    hint->setObjectName(QStringLiteral("VanityHint"));
    hint->setTextFormat(Qt::RichText);
    hint->setWordWrap(true);
    hint->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    hint->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(hint);

    auto *grid = new VanityChoiceGrid(VanityChoiceKind::Badge, 5, page);
    addGallerySearch(layout, grid, QStringLiteral("Search badges"), page);
    layout->addWidget(grid, 1);
    if (channelBadges)
    {
        this->channelBadgeHint_ = hint;
        this->channelBadgeGrid_ = grid;
        grid->selectionChanged = [this](const VanityChoice &choice) {
            if (!choice.owned)
            {
                this->previewTwitchEventBadge_ = choice.key;
                this->previewTwitchEventForChannel_ = true;
                this->refreshTwitchBadgeHint(false);
                this->refreshTwitchBadgeHint(true);
                this->rebuildLayoutRows();
                this->refreshPreview();
                return;
            }
            this->previewTwitchEventBadge_.clear();
            const auto &setId = choice.key;
            this->selectedChannelBadge_ = setId;
            if (this->globalBadgeGrid_ != nullptr)
            {
                if (setId.isEmpty())
                {
                    this->globalBadgeGrid_->setSelected(
                        this->selectedGlobalBadge_);
                }
                else
                {
                    this->globalBadgeGrid_->clearSelectedDisplay();
                }
            }
            this->refreshTwitchBadgeHint(false);
            this->refreshTwitchBadgeHint(true);
            this->rebuildLayoutRows();
            this->refreshPreview();
        };
    }
    else
    {
        this->globalBadgeHint_ = hint;
        this->globalBadgeGrid_ = grid;
        grid->selectionChanged = [this](const VanityChoice &choice) {
            if (!choice.owned)
            {
                this->previewTwitchEventBadge_ = choice.key;
                this->previewTwitchEventForChannel_ = false;
                this->refreshTwitchBadgeHint(false);
                this->refreshTwitchBadgeHint(true);
                this->rebuildLayoutRows();
                this->refreshPreview();
                return;
            }
            this->previewTwitchEventBadge_.clear();
            const auto &setId = choice.key;
            this->selectedGlobalBadge_ = setId;
            if (!setId.isEmpty())
            {
                this->selectedChannelBadge_.clear();
                if (this->channelBadgeGrid_ != nullptr)
                {
                    this->channelBadgeGrid_->setSelected({});
                }
            }
            this->refreshTwitchBadgeHint(false);
            this->refreshTwitchBadgeHint(true);
            this->rebuildLayoutRows();
            this->refreshPreview();
        };
    }
    return page;
}

QWidget *VanityDialog::buildSevenTVBadgeTab()
{
    auto *page = new QWidget(this->tabs_);
    page->setObjectName(QStringLiteral("VanityTabPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(7, 7, 7, 7);
    layout->setSpacing(6);

    layout->addWidget(this->buildSevenTVConnectionHeader(
        page, QStringLiteral("7TV badge"),
        QStringLiteral("Connect 7TV to choose a badge.")));
    this->sevenTVBadgeHint_ =
        new QLabel(QStringLiteral("Loading 7TV badges..."), page);
    this->sevenTVBadgeHint_->setObjectName(QStringLiteral("VanityHint"));
    this->sevenTVBadgeHint_->hide();
    layout->addWidget(this->sevenTVBadgeHint_);
    this->sevenTVBadgeGrid_ =
        new VanityChoiceGrid(VanityChoiceKind::Badge, 5, page);
    this->sevenTVBadgeGrid_->selectionChanged =
        [this](const VanityChoice &choice) {
            this->selectedSevenTVBadge_ = choice.key;
            this->sevenTVBadgeTouched_ =
                this->selectedSevenTVBadge_ != this->originalSevenTVBadge_;
            this->refreshPreview();
        };
    addGallerySearch(layout, this->sevenTVBadgeGrid_,
                     QStringLiteral("Search 7TV badges"), page);
    layout->addWidget(this->sevenTVBadgeGrid_, 1);
    return page;
}

QWidget *VanityDialog::buildMoltorinoBadgeTab()
{
    auto *page = new QWidget(this->tabs_);
    page->setObjectName(QStringLiteral("VanityTabPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(7, 7, 7, 7);
    layout->setSpacing(6);

    auto *title = new QLabel(QStringLiteral("Moltorino badge"), page);
    title->setObjectName(QStringLiteral("VanitySectionTitle"));
    layout->addWidget(title);
    this->moltorinoBadgeHint_ = new QLabel(page);
    this->moltorinoBadgeHint_->setObjectName(QStringLiteral("VanityHint"));
    layout->addWidget(this->moltorinoBadgeHint_);
    this->moltorinoBadgeGrid_ =
        new VanityChoiceGrid(VanityChoiceKind::Badge, 5, page);
    this->moltorinoBadgeGrid_->selectionChanged =
        [this](const VanityChoice &choice) {
            if (!choice.owned)
            {
                this->previewMoltorinoBadge_ = choice.key;
                this->setStatus(
                    QStringLiteral("Previewing %1. Unlock it before you can "
                                   "equip it.")
                        .arg(choice.name));
                this->refreshPreview();
                return;
            }
            this->previewMoltorinoBadge_.clear();
            this->setStatus({});
            this->selectedMoltorinoBadge_ = choice.key;
            this->workingLayout_.moltorinoBadge = choice.key;
            this->workingLayout_.moltorinoBadgeSelectionExplicit = true;
            this->updateLayoutDirtyState();
            this->refreshPreview();
        };
    addGallerySearch(layout, this->moltorinoBadgeGrid_,
                     QStringLiteral("Search Moltorino badges"), page);
    layout->addWidget(this->moltorinoBadgeGrid_, 1);
    return page;
}

QWidget *VanityDialog::buildLayoutTab()
{
    auto *page = new QWidget(this->tabs_);
    page->setObjectName(QStringLiteral("VanityTabPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(7, 7, 7, 7);
    layout->setSpacing(6);

    auto *stateRow = new QHBoxLayout;
    stateRow->setContentsMargins(0, 0, 0, 0);
    this->layoutHintLabel_ = new QLabel(
        QStringLiteral("Drag badges into place. Use the eye to hide one."),
        page);
    this->layoutHintLabel_->setObjectName(QStringLiteral("VanityHint"));
    this->layoutHintLabel_->setWordWrap(false);
    stateRow->addWidget(this->layoutHintLabel_, 1);
    this->layoutResetButton_ = new QPushButton(QStringLiteral("Reset"), page);
    this->layoutResetButton_->setEnabled(false);
    this->layoutResetButton_->setToolTip(
        QStringLiteral("Restore the default badge order and show every badge"));
    QObject::connect(this->layoutResetButton_, &QPushButton::clicked, this,
                     &VanityDialog::resetLayoutToDefault);
    stateRow->addWidget(this->layoutResetButton_);
    this->layoutRetryButton_ =
        new QPushButton(QStringLiteral("Try again"), page);
    this->layoutRetryButton_->hide();
    QObject::connect(this->layoutRetryButton_, &QPushButton::clicked, this,
                     &VanityDialog::retryLayoutSync);
    stateRow->addWidget(this->layoutRetryButton_);
    layout->addLayout(stateRow);

    this->layoutList_ = new QListWidget(page);
    this->layoutList_->setObjectName(QStringLiteral("VanityOrderList"));
    this->layoutList_->setDragDropMode(QAbstractItemView::InternalMove);
    this->layoutList_->setDefaultDropAction(Qt::MoveAction);
    this->layoutList_->setDragDropOverwriteMode(false);
    this->layoutList_->setMouseTracking(true);
    auto *delegate = new VanityOrderDelegate(this->layoutList_);
    delegate->visibilityChanged = [this](const QString &key, bool visible) {
        if (this->rebuildingLayoutRows_ || key.isEmpty())
        {
            return;
        }
        vanity::detail::setBadgeVisible(this->workingLayout_, key, visible);
        this->updateLayoutDirtyState();
        this->refreshPreview();
    };
    this->layoutList_->setItemDelegate(delegate);
    this->layoutList_->setEnabled(true);
    layout->addWidget(this->layoutList_, 1);
    QObject::connect(this->layoutList_->model(), &QAbstractItemModel::rowsMoved,
                     this, [this] {
                         if (this->rebuildingLayoutRows_)
                         {
                             return;
                         }
                         this->syncWorkingLayoutFromRows();
                         this->updateLayoutDirtyState();
                         this->refreshPreview();
                     });
    return page;
}

QWidget *VanityDialog::buildSevenTVConnectionHeader(
    QWidget *parent, const QString &title, const QString &disconnectedText)
{
    auto *rowWidget = new QWidget(parent);
    rowWidget->setObjectName(QStringLiteral("VanityConnectionRow"));
    auto *row = new QHBoxLayout(rowWidget);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    auto *textStack = new QVBoxLayout;
    textStack->setContentsMargins(0, 0, 0, 0);
    textStack->setSpacing(2);
    auto *titleLabel = new QLabel(title, rowWidget);
    titleLabel->setObjectName(QStringLiteral("VanitySectionTitle"));
    textStack->addWidget(titleLabel);
    auto *label = new QLabel(rowWidget);
    label->setObjectName(QStringLiteral("VanityHint"));
    label->setProperty("disconnectedText", disconnectedText);
    textStack->addWidget(label);
    row->addLayout(textStack, 1);
    auto *connectButton = new QPushButton(QStringLiteral("Connect"), rowWidget);
    connectButton->setToolTip(
        QStringLiteral("Your 7TV token stays on this device"));
    auto *disconnectButton =
        new QPushButton(QStringLiteral("Disconnect"), rowWidget);
    auto *refreshButton = new SvgButton(
        {
            .dark = QStringLiteral(":/buttons/reloadLight.svg"),
            .light = QStringLiteral(":/buttons/reloadDark.svg"),
        },
        this, QSize{3, 3});
    refreshButton->setScaleIndependentSize(18, 18);
    refreshButton->setCursor(Qt::PointingHandCursor);
    refreshButton->setToolTip(QStringLiteral("Refresh 7TV badges and paints"));
    row->addWidget(connectButton, 0, Qt::AlignTop);
    row->addWidget(refreshButton, 0, Qt::AlignTop);
    row->addWidget(disconnectButton, 0, Qt::AlignTop);
    this->sevenTVConnectionLabels_.push_back(label);
    this->sevenTVConnectButtons_.push_back(connectButton);
    this->sevenTVDisconnectButtons_.push_back(disconnectButton);
    this->sevenTVRefreshButtons_.push_back(refreshButton);
    QObject::connect(connectButton, &QPushButton::clicked, this,
                     &VanityDialog::connectSevenTV);
    QObject::connect(disconnectButton, &QPushButton::clicked, this,
                     &VanityDialog::disconnectSevenTV);
    QObject::connect(refreshButton, &Button::leftClicked, this, [this] {
        QStringList paintIDs;
        paintIDs.reserve(this->sevenTVPaints_.size());
        for (const auto &paint : this->sevenTVPaints_)
        {
            paintIDs.push_back(paint.id);
        }
        getApp()->getSeventvPaints()->loadPaintsByID(paintIDs, true);
        if (this->paintGrid_ != nullptr)
        {
            this->paintGrid_->updateVisibleTiles(false, true);
        }
        this->loadSevenTVState(false, true);
    });
    return rowWidget;
}

void VanityDialog::updateMinimumWidth()
{
    if (this->tabs_ == nullptr)
    {
        return;
    }

    auto *bar = this->tabs_->tabBar();
    bar->ensurePolished();
    const auto margins = this->mainLayout_->contentsMargins();

    this->tabs_->setMinimumWidth(bar->sizeHint().width() + 2);
    const auto frame = this->contentsMargins();
    this->setMinimumWidth(this->tabs_->minimumWidth() + margins.left() +
                          margins.right() + frame.left() + frame.right());
}

void VanityDialog::refreshColorPreviewChip()
{
    if (this->colorPreviewChip_ == nullptr || !this->selectedColor_.isValid())
    {
        return;
    }

    this->colorPreviewChip_->setColor(this->selectedColor_);
    this->colorPreviewChip_->setToolTip(
        QStringLiteral("Selected color: %1")
            .arg(this->selectedColor_.name().toUpper()));
}

void VanityDialog::refreshStyle()
{
    if (this->mainLayout_ == nullptr)
    {
        return;
    }

    const auto scale = this->scale();
    this->setFont(
        getApp()->getFonts()->getFont(FontStyle::UiMedium, scale));
    this->preview_->setUiScale(scale);
    this->closeButton_->setOverrideScale(scale);
    this->pinButton_->setOverrideScale(scale);
    for (auto *button : this->sevenTVRefreshButtons_)
    {
        button->setOverrideScale(scale);
    }
    const auto background = getTheme()->window.background;
    const auto text = getTheme()->window.text;
    auto muted = text;
    muted.setAlpha(160);
    const auto section = getTheme()->splits.header.background;
    const auto input = getTheme()->splits.input.background;
    const auto border = getTheme()->splits.header.border;
    const auto accent = getTheme()->tabs.selected.backgrounds.regular;
    const auto accentText = getTheme()->tabs.selected.text;

    this->headerTitleLabel_->setFont(
        getApp()->getFonts()->getFont(FontStyle::UiMediumBold, scale));
    this->headerIdentityLabel_->setFont(
        getApp()->getFonts()->getFont(FontStyle::UiMedium, scale));
    this->closeButton_->setColor(text);
    for (auto *button : this->sevenTVRefreshButtons_)
    {
        button->setColor(text);
    }

    this->mainLayout_->setContentsMargins(
        std::max(3, int(5 * scale)), std::max(3, int(5 * scale)),
        std::max(3, int(5 * scale)), std::max(3, int(5 * scale)));
    this->mainLayout_->setSpacing(std::max(3, int(5 * scale)));
    this->headerWidget_->layout()->setContentsMargins(
        std::max(2, int(4 * scale)), std::max(1, int(2 * scale)),
        std::max(2, int(4 * scale)), std::max(1, int(2 * scale)));
    this->headerWidget_->layout()->setSpacing(std::max(3, int(4 * scale)));
    if (auto *separator =
            this->findChild<QWidget *>(QStringLiteral("VanityDialogSeparator")))
    {
        separator->setFixedHeight(
            std::max(2, int(HEADER_SEPARATOR_HEIGHT * scale)));
    }
    this->saveButton_->setMinimumHeight(std::max(24, int(27 * scale)));
    this->colorHexInput_->setFixedWidth(std::max(62, qRound(82 * scale)));
    this->colorPicker_->setMinimumHeight(qRound(108 * scale));
    this->colorPreviewChip_->setFixedSize(std::max(18, qRound(24 * scale)),
                                          std::max(14, qRound(20 * scale)));
    const QSize swatchSize{std::max(16, qRound(30 * scale)),
                           std::max(12, qRound(22 * scale))};
    this->colorSwatchLayout_->setHorizontalSpacing(
        std::max(2, qRound(6 * scale)));
    this->colorSwatchLayout_->setVerticalSpacing(
        std::max(2, qRound(5 * scale)));
    this->tabs_->tabBar()->setUsesScrollButtons(false);
    const int tabVerticalPadding = std::max(2, qRound(4 * scale));
    const int tabHorizontalPadding = qRound(9 * scale);

    this->setStyleSheet(
        QStringLiteral(R"(
            QWidget#VanityDialogRoot {
                background: %1;
                color: %2;
            }
            QWidget#VanityDialogHeader,
            QWidget#VanityTabPage,
            QWidget#VanityConnectionRow,
            QWidget#VanityChoiceGridContent {
                background: transparent;
            }
            QLabel#VanityHeaderTitle,
            QLabel#VanitySectionTitle {
                color: %2;
            }
            QLabel#VanityHeaderIdentity,
            QLabel#VanityHint,
            QLabel#VanityStatus {
                color: %3;
            }
            QFrame#VanityDialogSeparator {
                background: %4;
            }
            QWidget#VanityPreviewFrame {
                background: %5;
                border: 1px solid %4;
                border-radius: 2px;
            }
            QTabWidget#VanityTabs::pane {
                background: %1;
                border: 1px solid %4;
                top: -1px;
            }
            QTabWidget#VanityTabs QTabBar::tab {
                background: %5;
                color: %3;
                border: 1px solid %4;
                padding: %9px %10px;
                margin-right: 1px;
            }
            QTabWidget#VanityTabs QTabBar::tab:selected {
                background: %6;
                color: %7;
                border-bottom: 2px solid %6;
            }
            QTabWidget#VanityTabs QTabBar::tab:hover:!selected {
                color: %2;
                background: %8;
            }
            QScrollArea#VanityChoiceGrid,
            QListWidget#VanityOrderList {
                background: %8;
                color: %2;
                border: 1px solid %4;
                selection-background-color: %6;
                selection-color: %7;
            }
            QLineEdit#VanitySearch {
                background: %8;
                color: %2;
                border: 1px solid %4;
                padding: 2px 5px;
                selection-background-color: %6;
                selection-color: %7;
            }
            QLineEdit#VanitySearch:focus {
                border-color: %6;
            }
            QScrollArea#VanityChoiceGrid QScrollBar:vertical {
                background: transparent;
                width: 7px;
                margin: 0;
            }
            QScrollArea#VanityChoiceGrid QScrollBar::handle:vertical {
                background: %3;
                min-height: 20px;
            }
            QScrollArea#VanityChoiceGrid QScrollBar::add-line:vertical,
            QScrollArea#VanityChoiceGrid QScrollBar::sub-line:vertical,
            QScrollArea#VanityChoiceGrid QScrollBar::add-page:vertical,
            QScrollArea#VanityChoiceGrid QScrollBar::sub-page:vertical {
                background: transparent;
                height: 0;
            }
            QPushButton {
                background: %5;
                color: %2;
                border: 1px solid %4;
                border-radius: 2px;
                padding: 3px 8px;
            }
            QPushButton:hover:enabled {
                background: %8;
                border-color: %6;
            }
            QPushButton:pressed:enabled {
                background: %4;
            }
            QPushButton#VanitySaveButton {
                background: %6;
                color: %7;
            }
            QPushButton#VanitySaveButton:hover:enabled {
                border-color: %2;
            }
            QPushButton:disabled,
            QListWidget:disabled {
                color: %3;
            }
        )")
            .arg(background.name(QColor::HexArgb), text.name(QColor::HexArgb),
                 muted.name(QColor::HexArgb), border.name(QColor::HexArgb),
                 section.name(QColor::HexArgb), accent.name(QColor::HexArgb),
                 accentText.name(QColor::HexArgb),
                 input.name(QColor::HexArgb))
            .arg(tabVerticalPadding)
            .arg(tabHorizontalPadding));

    const auto uiFont =
        getApp()->getFonts()->getFont(FontStyle::UiMedium, scale);
    const auto uiBoldFont =
        getApp()->getFonts()->getFont(FontStyle::UiMediumBold, scale);

    for (auto *widget : this->getLayoutContainer()->findChildren<QWidget *>())
    {
        widget->setFont(uiFont);
    }
    for (auto *label : this->findChildren<QLabel *>())
    {
        const auto objectName = label->objectName();
        if (objectName == QStringLiteral("VanityHeaderTitle") ||
            objectName == QStringLiteral("VanitySectionTitle"))
        {
            label->setFont(uiBoldFont);
        }
        else if (objectName == QStringLiteral("VanityHeaderIdentity") ||
                 objectName == QStringLiteral("VanityHint") ||
                 objectName == QStringLiteral("VanityStatus"))
        {
            label->setFont(uiFont);
            if (objectName == QStringLiteral("VanityHint"))
            {
                label->setWordWrap(true);
            }
        }
    }
    this->saveButton_->setFont(uiBoldFont);
    for (auto *search :
         this->findChildren<QLineEdit *>(QStringLiteral("VanitySearch")))
    {
        search->setFixedHeight(
            std::max(qRound(25 * scale), search->fontMetrics().height() + 8));
    }

    for (auto *button : this->colorButtons_->buttons())
    {
        button->setFixedSize(swatchSize);
        const auto color = button->property("vanityColor").toString();
        if (color.isEmpty())
        {
            continue;
        }
        button->setStyleSheet(
            QStringLiteral("QPushButton { background: %1; border: 1px solid "
                           "%2; padding: 0; } "
                           "QPushButton:checked { border: 3px solid %3; }")
                .arg(color, border.name(QColor::HexArgb),
                     accentText.name(QColor::HexArgb)));
    }
    this->refreshColorPreviewChip();
    for (auto *grid :
         {this->paintGrid_, this->globalBadgeGrid_, this->channelBadgeGrid_,
          this->sevenTVBadgeGrid_, this->moltorinoBadgeGrid_})
    {
        if (grid != nullptr)
        {
            grid->setUiScale(scale);
            grid->viewport()->update();
            grid->updateVisibleTiles();
        }
    }
    if (this->layoutList_ != nullptr)
    {
        static_cast<VanityOrderDelegate *>(this->layoutList_->itemDelegate())
            ->uiScale = scale;
        this->layoutList_->doItemsLayout();
        this->layoutList_->viewport()->update();
    }
    this->updateMinimumWidth();
    this->applyPopupSize(DEFAULT_SIZE * scale);
}

VanityChoiceGrid *VanityDialog::currentChoiceGrid() const
{
    const auto *page = this->tabs_->currentWidget();
    if (page == this->paintPage_)
    {
        return this->paintGrid_;
    }
    if (page == this->globalBadgePage_)
    {
        return this->globalBadgeGrid_;
    }
    if (page == this->channelBadgePage_)
    {
        return this->channelBadgeGrid_;
    }
    if (page == this->sevenTVBadgePage_)
    {
        return this->sevenTVBadgeGrid_;
    }
    if (page == this->moltorinoBadgePage_)
    {
        return this->moltorinoBadgeGrid_;
    }
    return nullptr;
}

void VanityDialog::updateVisualRefreshTimer()
{
    if (this->visualRefreshTimer_ == nullptr || this->tabs_ == nullptr)
    {
        return;
    }
    if (!this->isVisible())
    {
        this->visualRefreshTimer_->stop();
        return;
    }

    auto *grid = this->currentChoiceGrid();
    if (grid != nullptr)
    {
        this->visualSettleTicks_ = 3;
        grid->updateVisibleTiles();
        if (!this->visualRefreshTimer_->isActive())
        {
            this->visualRefreshTimer_->start();
        }
    }
    else
    {
        this->visualRefreshTimer_->stop();
    }
}

void VanityDialog::refreshLoadedImages()
{
    if (!this->isVisible() || this->tabs_ == nullptr)
    {
        return;
    }

    auto *grid = this->currentChoiceGrid();
    if (grid != nullptr)
    {
        grid->viewport()->update();
        grid->updateVisibleTiles();
    }
    if (this->preview_ != nullptr)
    {
        this->preview_->update();
    }
}

void VanityDialog::loadTwitchState()
{
    if (this->twitchRequestInFlight_)
    {
        return;
    }

    this->refreshTwitchAuthCandidates();
    this->twitchAuthCandidateIndex_ = 0;
    this->activeTwitchAuth_.reset();
    this->twitchLoaded_ = false;
    this->twitchLoadFinished_ = false;
    this->twitchBadgeHint_.clear();
    this->twitchBadgeError_.clear();
    this->globalBadgeChoicesBuilt_ = false;
    this->channelBadgeChoicesBuilt_ = false;
    for (auto *button : this->twitchRetryButtons_)
    {
        button->hide();
        button->setEnabled(false);
    }
    this->populateCurrentTab();

    if (this->accountUserId_.isEmpty() || this->twitchAuthCandidates_.empty() ||
        this->channel_ == nullptr)
    {
        this->setTwitchBadgesUnavailable(
            QStringLiteral("Reconnect Twitch to change Twitch badges."));
        return;
    }

    this->twitchRequestInFlight_ = true;
    this->tryLoadTwitchStateCandidate();
}

void VanityDialog::tryLoadTwitchStateCandidate()
{
    if (!this->twitchRequestInFlight_ || this->twitchAuthCandidateIndex_ < 0 ||
        this->twitchAuthCandidateIndex_ >=
            int(this->twitchAuthCandidates_.size()))
    {
        this->setTwitchBadgesUnavailable(
            QStringLiteral("Twitch badges need a fresh login."));
        return;
    }

    const auto auth =
        this->twitchAuthCandidates_[this->twitchAuthCandidateIndex_];

    TwitchGql::getVanityState(
        this->channel_->getName(), this->channel_->roomId(), auth,
        [self = QPointer<VanityDialog>(this), auth](GqlVanityState state) {
            if (!self)
            {
                return;
            }
            if (state.currentUserId != self->accountUserId_)
            {
                ++self->twitchAuthCandidateIndex_;
                if (self->twitchAuthCandidateIndex_ <
                    int(self->twitchAuthCandidates_.size()))
                {
                    self->tryLoadTwitchStateCandidate();
                    return;
                }
                self->setTwitchBadgesUnavailable(
                    QStringLiteral("The Twitch badge login belongs to another "
                                   "account."));
                return;
            }
            self->twitchState_ = std::move(state);
            self->activeTwitchAuth_ = auth;
            self->twitchRequestInFlight_ = false;
            self->accountDisplayName_ =
                self->twitchState_.currentUserDisplayName.trimmed();
            if (self->accountDisplayName_.isEmpty())
            {
                self->accountDisplayName_ = self->accountLogin_;
            }
            self->twitchLoaded_ = true;
            self->twitchLoadFinished_ = true;
            self->twitchBadgeHint_.clear();
            self->twitchBadgeError_.clear();
            for (auto *button : self->twitchRetryButtons_)
            {
                button->hide();
                button->setEnabled(true);
            }
            self->updateSaveButtonState();
            self->originalGlobalBadge_ =
                self->twitchState_.selectedGlobalBadge
                    ? self->twitchState_.selectedGlobalBadge->setId
                    : QString{};
            self->originalChannelBadge_ =
                self->twitchState_.selectedChannelBadge
                    ? self->twitchState_.selectedChannelBadge->setId
                    : QString{};
            self->selectedGlobalBadge_ = self->originalGlobalBadge_;
            self->selectedChannelBadge_ = self->originalChannelBadge_;
            self->globalBadgeChoicesBuilt_ = false;
            self->channelBadgeChoicesBuilt_ = false;
            self->refreshHeaderIdentity();
            self->populateCurrentTab();
            self->rebuildLayoutRows();
            self->refreshPreview();
        },
        [self = QPointer<VanityDialog>(this)](const QString &error) {
            if (self)
            {
                const bool authenticationFailed =
                    error.contains(QStringLiteral("400"),
                                   Qt::CaseInsensitive) ||
                    error.contains(QStringLiteral("401"),
                                   Qt::CaseInsensitive) ||
                    error.contains(QStringLiteral("403"),
                                   Qt::CaseInsensitive) ||
                    error.contains(QStringLiteral("auth"),
                                   Qt::CaseInsensitive) ||
                    error.contains(QStringLiteral("client"),
                                   Qt::CaseInsensitive) ||
                    error.contains(QStringLiteral("token"),
                                   Qt::CaseInsensitive);
                if (authenticationFailed &&
                    ++self->twitchAuthCandidateIndex_ <
                        int(self->twitchAuthCandidates_.size()))
                {
                    self->tryLoadTwitchStateCandidate();
                    return;
                }
                self->setTwitchBadgesUnavailable(
                    authenticationFailed
                        ? QStringLiteral("Twitch badges need a fresh login.")
                        : QStringLiteral(
                              "Twitch badges could not load. You can still "
                              "save changes from the other tabs."),
                    error);
            }
        });
}

void VanityDialog::retryTwitchState()
{
    if (this->saveInFlight_ || this->twitchRequestInFlight_)
    {
        return;
    }
    this->setStatus({});
    this->loadTwitchState();
}

void VanityDialog::loadTwitchEventBadges()
{
    if (this->twitchEventBadgesRequested_)
    {
        return;
    }
    this->twitchEventBadgesRequested_ = true;

    const auto applyResult =
        [self = QPointer<VanityDialog>(this)](
            const QVector<TwitchEventBadge> &badges, bool succeeded) {
            if (!self)
            {
                return;
            }
            if (!succeeded)
            {
                self->twitchEventBadgesRequested_ = false;
                self->scheduleTwitchEventBadgeRetry();
            }
            if (self->twitchEventBadges_ == badges)
            {
                return;
            }
            self->twitchEventBadges_ = badges;
            const auto *preview = self->findTwitchEventBadge(
                self->previewTwitchEventBadge_);
            if (!self->previewTwitchEventBadge_.isEmpty() &&
                (preview == nullptr ||
                 !isTwitchEventBadgeActive(
                     *preview, QDateTime::currentDateTimeUtc())))
            {
                self->previewTwitchEventBadge_.clear();
            }
            self->globalBadgeChoicesBuilt_ = false;
            self->channelBadgeChoicesBuilt_ = false;
            self->populateCurrentTab();
            self->refreshPreview();
        };

    auto &cache = twitchEventBadgeCache();
    const auto now = QDateTime::currentDateTimeUtc();
    const bool hasCachedResult = cache.fetchedAt.isValid();
    const auto cacheAge = hasCachedResult ? cache.fetchedAt.secsTo(now) : -1;
    const bool cacheIsFresh =
        hasCachedResult && cacheAge >= 0 && cacheAge < 30 * 60;
    if (hasCachedResult)
    {
        applyResult(cache.badges, true);
        if (cacheIsFresh)
        {
            return;
        }
    }
    if (cache.requestInFlight)
    {
        cache.waiters.push_back(applyResult);
        return;
    }
    const auto sinceLastAttempt =
        cache.lastAttempt.isValid() ? cache.lastAttempt.secsTo(now) : -1;
    if (sinceLastAttempt >= 0 && sinceLastAttempt < 60)
    {
        this->twitchEventBadgesRequested_ = false;
        this->scheduleTwitchEventBadgeRetry();
        return;
    }

    cache.requestInFlight = true;
    cache.lastAttempt = now;
    cache.waiters.push_back(applyResult);
    NetworkRequest(TWITCH_EVENT_BADGES_ENDPOINT)
        .header("Accept", "application/json")
        .timeout(10000)
        .maximumResponseSize(1024 * 1024)
        .onSuccess([](const NetworkResult &result) {
            auto &cache = twitchEventBadgeCache();
            const auto parsed =
                parseTwitchEventBadges(result.parseJson());
            const bool succeeded = parsed.has_value();
            if (succeeded)
            {
                cache.badges = *parsed;
                cache.fetchedAt = QDateTime::currentDateTimeUtc();
            }
            cache.requestInFlight = false;
            auto waiters = std::exchange(cache.waiters, {});
            for (auto &waiter : waiters)
            {
                waiter(cache.badges, succeeded);
            }
        })
        .onError([](const NetworkResult &) {
            auto &cache = twitchEventBadgeCache();
            cache.requestInFlight = false;
            auto waiters = std::exchange(cache.waiters, {});
            for (auto &waiter : waiters)
            {
                waiter(cache.badges, false);
            }
        })
        .execute();
}

void VanityDialog::scheduleTwitchEventBadgeRetry()
{
    if (this->twitchEventBadgeRetryScheduled_ ||
        this->twitchEventBadgeAutomaticRetryUsed_)
    {
        return;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    const auto &cache = twitchEventBadgeCache();
    const auto secondsSinceAttempt =
        cache.lastAttempt.isValid() ? cache.lastAttempt.secsTo(now) : 60;
    const auto secondsUntilRetry =
        std::clamp<qint64>(60 - secondsSinceAttempt, 1, 60);

    this->twitchEventBadgeRetryScheduled_ = true;
    this->twitchEventBadgeAutomaticRetryUsed_ = true;
    QTimer::singleShot(std::chrono::seconds(secondsUntilRetry), this, [this] {
        this->twitchEventBadgeRetryScheduled_ = false;
        if (this->twitchEventBadgesRequested_ || this->tabs_ == nullptr)
        {
            return;
        }

        const auto *page = this->tabs_->currentWidget();
        if (page == this->globalBadgePage_ || page == this->channelBadgePage_)
        {
            this->loadTwitchEventBadges();
        }
    });
}

void VanityDialog::clearTwitchEventBadgePreview()
{
    if (this->previewTwitchEventBadge_.isEmpty())
    {
        return;
    }
    this->previewTwitchEventBadge_.clear();
    if (this->globalBadgeGrid_ != nullptr)
    {
        if (this->selectedChannelBadge_.isEmpty())
        {
            this->globalBadgeGrid_->setSelected(this->selectedGlobalBadge_);
        }
        else
        {
            this->globalBadgeGrid_->clearSelectedDisplay();
        }
    }
    if (this->channelBadgeGrid_ != nullptr)
    {
        this->channelBadgeGrid_->setSelected(this->selectedChannelBadge_);
    }
    this->refreshTwitchBadgeHint(false);
    this->refreshTwitchBadgeHint(true);
}

void VanityDialog::refreshTwitchBadgeHint(bool channelBadges)
{
    auto *hint = channelBadges ? this->channelBadgeHint_
                              : this->globalBadgeHint_;
    if (hint == nullptr || !this->twitchLoaded_)
    {
        return;
    }

    const bool previewIsHere = !this->previewTwitchEventBadge_.isEmpty() &&
                               this->previewTwitchEventForChannel_ ==
                                   channelBadges;
    if (previewIsHere)
    {
        if (const auto *badge =
                this->findTwitchEventBadge(this->previewTwitchEventBadge_))
        {
            auto text = QStringLiteral("Previewing <b>%1</b>")
                            .arg(badge->name.toHtmlEscaped());
            if (!badge->detailsUrl.isEmpty())
            {
                text += QStringLiteral(
                            " · <a href=\"%1\">How to get it</a>")
                            .arg(badge->detailsUrl.toHtmlEscaped());
            }
            hint->setText(text);
            hint->show();
            return;
        }
    }

    QSet<QString> owned;
    for (const auto &badge : this->twitchState_.globalBadges)
    {
        owned.insert(badge.setId.toLower());
    }
    for (const auto &badge : this->twitchState_.channelBadges)
    {
        owned.insert(badge.setId.toLower());
    }
    const auto now = QDateTime::currentDateTimeUtc();
    const bool hasLockedBadges =
        std::ranges::any_of(this->twitchEventBadges_, [&](const auto &badge) {
            return isTwitchEventBadgeActive(badge, now) &&
                   !owned.contains(badge.id);
        });
    if (hasLockedBadges)
    {
        hint->setText(QStringLiteral(
            "Badges you can still earn appear locked. Select one to preview "
            "it."));
        hint->show();
    }
    else
    {
        hint->hide();
    }
}

void VanityDialog::refreshTwitchAuthCandidates()
{
    const auto saved = MoltorinoAuth::resolveSelectedUserToken();
    this->twitchAuthCandidates_ = vanity::detail::buildTwitchAuthCandidates(
        this->accountToken_, this->accountClientId_, saved.token,
        saved.clientId);
}

void VanityDialog::setTwitchBadgesUnavailable(const QString &hint,
                                              const QString &details)
{
    this->twitchRequestInFlight_ = false;
    this->activeTwitchAuth_.reset();
    this->twitchLoaded_ = false;
    this->twitchLoadFinished_ = true;
    this->twitchBadgeHint_ = hint;
    this->twitchBadgeError_ = details;
    this->globalBadgeChoicesBuilt_ = false;
    this->channelBadgeChoicesBuilt_ = false;
    for (auto *button : this->twitchRetryButtons_)
    {
        button->show();
        button->setEnabled(true);
    }
    this->updateSaveButtonState();
    this->populateCurrentTab();
}

void VanityDialog::loadLayoutState()
{
    if (this->layoutRequestInFlight_)
    {
        return;
    }

    this->layoutRequestInFlight_ = true;
    this->updateSaveButtonState();
    this->layoutHintLabel_->setText(QStringLiteral("Checking online sync..."));
    this->layoutHintLabel_->setToolTip({});
    this->layoutHintLabel_->setStyleSheet({});
    this->layoutRetryButton_->hide();

    std::optional<MoltorinoVanityLayout> localLayout;
    std::optional<MoltorinoVanityLayout> effectiveLayout;
    if (auto *provider = getApp()->getMoltorinoSupporterBadges())
    {
        localLayout = provider->getLocalVanityLayout(this->accountUserId_);
        if (localLayout)
        {
            effectiveLayout = localLayout;
        }
        else
        {
            effectiveLayout = provider->getVanityLayout(this->accountUserId_);
        }
    }
    if (!this->layoutLoaded_)
    {
        this->useLoadedLayout(
            effectiveLayout.value_or(MoltorinoVanityLayout{}));
        this->layoutNeedsSync_ = localLayout.has_value();
        this->layoutLoaded_ = true;
        this->layoutResetButton_->setEnabled(true);
    }
    this->rebuildLayoutRows();
    this->refreshPreview();

    QUrl profileUrl(VANITY_LAYOUT_ENDPOINT);
    QUrlQuery profileQuery;
    profileQuery.addQueryItem(QStringLiteral("userId"), this->accountUserId_);
    profileUrl.setQuery(profileQuery);
    NetworkRequest(profileUrl)
        .header("Accept", "application/json")
        .timeout(10000)
        .maximumResponseSize(1024 * 1024)
        .caller(this)
        .onSuccess([this](const NetworkResult &result) {
            this->layoutRequestInFlight_ = false;
            this->updateSaveButtonState();
            const auto response = result.parseJson();
            const auto profileObject = response.value("profile").toObject();
            auto *provider = getApp()->getMoltorinoSupporterBadges();
            auto local =
                provider == nullptr
                    ? std::optional<MoltorinoVanityLayout>{}
                    : provider->getLocalVanityLayout(this->accountUserId_);
            const auto returnedUserId = response.value("userId").toString();
            if (returnedUserId != this->accountUserId_ ||
                profileObject.isEmpty() ||
                profileObject.value("layoutSchemaVersion").toInt() != 1)
            {
                const bool hasLocalState = local.has_value() ||
                                           this->layoutTouched_ ||
                                           this->layoutNeedsSync_;
                this->layoutHintLabel_->setText(
                    hasLocalState
                        ? QStringLiteral("Badge order is saved on this device.")
                        : QStringLiteral("Online badge order is unavailable."));
                this->layoutHintLabel_->setToolTip(
                    !returnedUserId.isEmpty() &&
                            returnedUserId != this->accountUserId_
                        ? QStringLiteral(
                              "Online sync returned a different account.")
                        : QStringLiteral(
                              "Could not load this online badge setup."));
                this->layoutRetryButton_->setText(
                    hasLocalState ? QStringLiteral("Sync")
                                  : QStringLiteral("Try again"));
                this->layoutRetryButton_->setToolTip(
                    hasLocalState
                        ? QStringLiteral("This badge setup is only confirmed "
                                         "on this device. Sync to save it "
                                         "online.")
                        : QStringLiteral("Try loading your online badge setup "
                                         "again."));
                this->layoutRetryButton_->show();
                return;
            }

            auto remoteLayout = vanityLayoutFromObject(profileObject);
            bool remoteNeedsRepair = false;
            if (response.value("assignedBadges").isArray())
            {
                this->assignedMoltorinoBadgeIds_.clear();
                QSet<QString> assignedBadges;
                for (const auto &badge :
                     response.value("assignedBadges").toArray())
                {
                    const auto badgeId = badge.toString().trimmed().toLower();
                    if (!badgeId.isEmpty() &&
                        !assignedBadges.contains(badgeId) &&
                        this->assignedMoltorinoBadgeIds_.size() < 64)
                    {
                        assignedBadges.insert(badgeId);
                        this->assignedMoltorinoBadgeIds_.push_back(badgeId);
                    }
                }
                this->moltorinoAssignmentsAuthoritative_ = true;
                this->moltorinoBadgeChoicesBuilt_ = false;
                this->refreshMoltorinoBadgeAvailability();
                if (remoteLayout.moltorinoBadgeSelectionExplicit &&
                    !remoteLayout.moltorinoBadge.isEmpty() &&
                    !assignedBadges.contains(remoteLayout.moltorinoBadge))
                {
                    const auto unavailableBadge = remoteLayout.moltorinoBadge;
                    remoteLayout.moltorinoBadge.clear();
                    remoteLayout.moltorinoBadgeSelectionExplicit = false;
                    remoteNeedsRepair = true;
                    if (this->selectedMoltorinoBadge_ == unavailableBadge)
                    {
                        this->selectedMoltorinoBadge_.clear();
                        this->workingLayout_.moltorinoBadge.clear();
                        this->workingLayout_
                            .moltorinoBadgeSelectionExplicit = false;
                        this->syncEffectiveMoltorinoSelection();
                        this->updateLayoutDirtyState();
                    }
                    this->moltorinoBadgeChoicesBuilt_ = false;
                }
                if (local && local->moltorinoBadgeSelectionExplicit &&
                    !local->moltorinoBadge.isEmpty() &&
                    !assignedBadges.contains(local->moltorinoBadge))
                {
                    const auto unavailableBadge = local->moltorinoBadge;
                    local->moltorinoBadge.clear();
                    local->moltorinoBadgeSelectionExplicit = false;
                    if (provider != nullptr)
                    {
                        provider->setLocalVanityLayout(this->accountUserId_,
                                                       *local);
                    }
                    if (this->selectedMoltorinoBadge_ == unavailableBadge)
                    {
                        this->selectedMoltorinoBadge_.clear();
                        this->workingLayout_.moltorinoBadge.clear();
                        this->workingLayout_
                            .moltorinoBadgeSelectionExplicit = false;
                        this->syncEffectiveMoltorinoSelection();
                        this->updateLayoutDirtyState();
                    }
                    this->moltorinoBadgeChoicesBuilt_ = false;
                }
            }
            this->layoutRevision_ =
                std::max(0, profileObject.value("revision").toInt());
            if (!this->layoutTouched_)
            {
                this->useLoadedLayout(local.value_or(remoteLayout));
            }
            if (remoteNeedsRepair || (local && *local != remoteLayout))
            {
                this->layoutNeedsSync_ = true;
                this->layoutHintLabel_->setText(
                    QStringLiteral("Badge order is saved on this device."));
                this->layoutRetryButton_->setText(QStringLiteral("Sync"));
                this->layoutRetryButton_->setToolTip(QStringLiteral(
                    "This badge setup is only confirmed on this device. Sync "
                    "to save it online."));
                this->layoutRetryButton_->show();
            }
            else
            {
                this->layoutNeedsSync_ = false;
                this->layoutHintLabel_->setText(QStringLiteral(
                    "Drag badges into place. Use the eye to hide one."));
                this->layoutRetryButton_->setToolTip({});
                this->layoutRetryButton_->hide();
            }
            this->rebuildLayoutRows();
            this->refreshPreview();
        })
        .onError([this](const NetworkResult &result) {
            this->layoutRequestInFlight_ = false;
            this->updateSaveButtonState();
            auto *provider = getApp()->getMoltorinoSupporterBadges();
            const auto local =
                provider == nullptr
                    ? std::optional<MoltorinoVanityLayout>{}
                    : provider->getLocalVanityLayout(this->accountUserId_);
            const bool hasLocalState = local.has_value() ||
                                       this->layoutTouched_ ||
                                       this->layoutNeedsSync_;
            this->layoutHintLabel_->setText(
                hasLocalState ? QStringLiteral("Badge order is saved "
                                               "on this device.")
                              : QStringLiteral("Online badge order is "
                                               "unavailable."));
            this->layoutHintLabel_->setToolTip(result.formatError());
            this->layoutRetryButton_->setText(
                hasLocalState ? QStringLiteral("Sync")
                              : QStringLiteral("Try again"));
            this->layoutRetryButton_->setToolTip(
                hasLocalState
                    ? QStringLiteral("This badge setup is only confirmed on "
                                     "this device. Sync to save it online.")
                    : QStringLiteral("Try loading your online badge setup "
                                     "again."));
            this->layoutRetryButton_->show();
        })
        .execute();
}

void VanityDialog::retryLayoutSync()
{
    if (this->saveInFlight_ || this->layoutRequestInFlight_)
    {
        return;
    }

    auto *provider = getApp()->getMoltorinoSupporterBadges();
    const auto local =
        provider == nullptr
            ? std::optional<MoltorinoVanityLayout>{}
            : provider->getLocalVanityLayout(this->accountUserId_);
    if (!local && !this->layoutTouched_ && !this->layoutNeedsSync_)
    {
        this->loadLayoutState();
        return;
    }
    if (!this->isOriginalAccountCurrent())
    {
        this->stopForAccountChange();
        return;
    }
    this->syncWorkingLayoutFromRows();
    this->autoPinnedForSave_ = this->ensurePinned();
    this->setBusy(true);
    this->setStatus(QStringLiteral("Syncing badge order..."));
    this->saveLayout(true);
}

void VanityDialog::resetLayoutToDefault()
{
    if (!this->layoutLoaded_ || this->saveInFlight_)
    {
        return;
    }

    this->syncWorkingLayoutFromRows();
    const auto reset =
        vanity::detail::defaultLayoutPreservingBadge(this->workingLayout_);
    if (reset == this->workingLayout_)
    {
        this->setStatus(QStringLiteral(
            "Badges are already in the default order and all are visible."));
        return;
    }

    this->workingLayout_ = reset;
    this->previewMoltorinoBadge_.clear();
    this->syncEffectiveMoltorinoSelection();
    this->updateLayoutDirtyState();
    this->rebuildLayoutRows();
    this->refreshPreview();
    this->setStatus(QStringLiteral(
        "Default order restored and all badges shown. Save to apply."));
}

void VanityDialog::loadSevenTVState(bool connecting, bool forceRefresh)
{
    if (this->sevenTVRequestInFlight_)
    {
        return;
    }
    if (!this->isOriginalAccountCurrent())
    {
        this->stopForAccountChange();
        return;
    }
    const auto token =
        normalizedToken(this->pendingSevenTVToken_.isEmpty()
                            ? this->storedSevenTVTokenForCurrentAccount()
                            : this->pendingSevenTVToken_);
    if (token.isEmpty())
    {
        this->sevenTVLoaded_ = false;
        this->paintChoicesBuilt_ = false;
        this->sevenTVBadgeChoicesBuilt_ = false;
        this->refreshConnectionText();
        this->populateCurrentTab();
        this->refreshPreview();
        return;
    }

    const auto fingerprint = tokenHash(token);
    const auto expiry = QDateTime::fromString(
        getSettings()->sevenTVVanityTokenExpiry.getValue(), Qt::ISODate);
    const bool tokenExpired =
        expiry.isValid() && expiry <= QDateTime::currentDateTimeUtc();
    if (!connecting && !forceRefresh && !tokenExpired)
    {
        const auto cached =
            sevenTVVanityCache().constFind(this->accountUserId_);
        if (cached != sevenTVVanityCache().cend() &&
            cached->tokenHash == fingerprint)
        {
            this->sevenTVUserId_ = cached->userId;
            this->sevenTVLogin_ = cached->login;
            this->sevenTVToken_ = token;
            this->originalSevenTVBadge_ = cached->activeBadgeId;
            this->originalSevenTVPaint_ = cached->activePaintId;
            this->selectedSevenTVBadge_ = this->originalSevenTVBadge_;
            this->selectedSevenTVPaint_ = this->originalSevenTVPaint_;
            this->sevenTVBadgeTouched_ = false;
            this->sevenTVPaintTouched_ = false;
            this->sevenTVBadges_ = cached->badges;
            this->sevenTVPaints_ = cached->paints;
            this->sevenTVLoaded_ = true;
            this->paintChoicesBuilt_ = false;
            this->sevenTVBadgeChoicesBuilt_ = false;
            QStringList paintIds;
            paintIds.reserve(this->sevenTVPaints_.size());
            for (const auto &paint : this->sevenTVPaints_)
            {
                paintIds.push_back(paint.id);
            }
            getApp()->getSeventvPaints()->loadPaintsByID(paintIds);
            this->refreshConnectionText();
            this->populateCurrentTab();
            this->refreshPreview();
            return;
        }
    }

    this->sevenTVRequestInFlight_ = true;
    this->updateSaveButtonState();
    this->refreshConnectionText();
    this->populateCurrentTab();
    static constexpr auto QUERY = R"(
query MoltorinoVanityInventory {
  users {
    me {
      id
      connections { platform platformId platformDisplayName }
      style { activeBadgeId activePaintId }
      inventory(includeInaccessible: false) {
        badges { accessible to { badge { id name images { url width height scale } } } }
        paints { accessible to { paint { id name } } }
      }
    }
  }
}
)";
    QJsonObject payload{{"query", QString::fromUtf8(QUERY)},
                        {"variables", QJsonObject{}}};
    NetworkRequest(SEVENTV_GQL_ENDPOINT, NetworkRequestType::Post)
        .header("Authorization", QStringLiteral("Bearer ") + token)
        .header("Accept", "application/json")
        .json(payload)
        .timeout(10000)
        .maximumResponseSize(4 * 1024 * 1024)
        .caller(this)
        .onSuccess([this, connecting, forceRefresh, token,
                    fingerprint](const NetworkResult &result) {
            this->sevenTVRequestInFlight_ = false;
            this->updateSaveButtonState();
            if (!this->isOriginalAccountCurrent())
            {
                this->stopForAccountChange();
                return;
            }
            if (token != normalizedToken(this->pendingSevenTVToken_.isEmpty()
                         ? this->storedSevenTVTokenForCurrentAccount()
                         : this->pendingSevenTVToken_))
            {
                this->refreshConnectionText();
                return;
            }
            const bool preserveBadgeDraft = forceRefresh &&
                this->sevenTVLoaded_ && this->sevenTVBadgeTouched_;
            const bool preservePaintDraft = forceRefresh &&
                this->sevenTVLoaded_ && this->sevenTVPaintTouched_;
            const auto draftBadge = this->selectedSevenTVBadge_;
            const auto draftPaint = this->selectedSevenTVPaint_;
            const auto root = result.parseJson();
            const auto error = graphQlError(root);
            const auto me = root.value("data")
                                .toObject()
                                .value("users")
                                .toObject()
                                .value("me")
                                .toObject();
            if (!error.isEmpty() || me.isEmpty())
            {
                const bool authenticationError =
                    error.contains(QStringLiteral("auth"),
                                   Qt::CaseInsensitive) ||
                    error.contains(QStringLiteral("token"),
                                   Qt::CaseInsensitive);
                if (connecting)
                {
                    this->pendingSevenTVToken_.clear();
                }
                else if (authenticationError)
                {
                    this->clearSevenTVConnection();
                    this->sevenTVLoaded_ = false;
                }
                this->setStatus(
                    QStringLiteral("7TV: ") +
                        (error.isEmpty()
                             ? QStringLiteral(
                                   "This token is not connected to a user")
                             : error),
                    true);
                this->refreshConnectionText();
                this->populateCurrentTab();
                return;
            }

            QJsonObject twitchConnection;
            for (const auto &connectionValue :
                 me.value("connections").toArray())
            {
                const auto connection = connectionValue.toObject();
                if (connection.value("platform").toString() ==
                        QStringLiteral("TWITCH") &&
                    connection.value("platformId").toString() ==
                        this->accountUserId_)
                {
                    twitchConnection = connection;
                    break;
                }
            }
            if (twitchConnection.isEmpty())
            {
                if (connecting)
                {
                    this->pendingSevenTVToken_.clear();
                }
                else
                {
                    this->clearSevenTVConnection();
                }
                this->sevenTVLoaded_ = false;
                this->setStatus(QStringLiteral("This 7TV connection belongs to "
                                               "a different Twitch account."),
                                true);
                this->refreshConnectionText();
                this->populateCurrentTab();
                this->refreshPreview();
                return;
            }

            this->sevenTVUserId_ = me.value("id").toString();
            this->sevenTVLogin_ = twitchConnection.value("platformDisplayName")
                                      .toString(this->accountLogin_);
            const auto style = me.value("style").toObject();
            this->originalSevenTVBadge_ =
                style.value("activeBadgeId").toString();
            this->originalSevenTVPaint_ = SeventvPaints::normalizePaintID(
                style.value("activePaintId").toString());
            if (!preserveBadgeDraft)
            {
                this->selectedSevenTVBadge_ = this->originalSevenTVBadge_;
                this->sevenTVBadgeTouched_ = false;
            }
            if (!preservePaintDraft)
            {
                this->selectedSevenTVPaint_ = this->originalSevenTVPaint_;
                this->sevenTVPaintTouched_ = false;
            }
            this->sevenTVBadges_.clear();
            this->sevenTVPaints_.clear();
            const auto inventory = me.value("inventory").toObject();
            QSet<QString> seenBadges;
            for (const auto &value : inventory.value("badges").toArray())
            {
                if (this->sevenTVBadges_.size() >=
                    MAX_SEVENTV_COSMETICS_PER_KIND)
                {
                    break;
                }
                const auto entry = value.toObject();
                if (!entry.value("accessible").toBool())
                {
                    continue;
                }
                auto cosmetic = cosmeticFromObject(
                    entry.value("to").toObject().value("badge").toObject());
                if (!cosmetic.id.isEmpty() && !seenBadges.contains(cosmetic.id))
                {
                    seenBadges.insert(cosmetic.id);
                    this->sevenTVBadges_.push_back(std::move(cosmetic));
                }
            }
            QSet<QString> seenPaints;
            for (const auto &value : inventory.value("paints").toArray())
            {
                if (this->sevenTVPaints_.size() >=
                    MAX_SEVENTV_COSMETICS_PER_KIND)
                {
                    break;
                }
                const auto entry = value.toObject();
                if (!entry.value("accessible").toBool())
                {
                    continue;
                }
                auto cosmetic = cosmeticFromObject(
                    entry.value("to").toObject().value("paint").toObject());
                cosmetic.id = SeventvPaints::normalizePaintID(cosmetic.id);
                if (SeventvPaints::isValidPaintID(cosmetic.id) &&
                    !seenPaints.contains(cosmetic.id))
                {
                    seenPaints.insert(cosmetic.id);
                    this->sevenTVPaints_.push_back(std::move(cosmetic));
                }
            }
            const auto byName = [](const auto &left, const auto &right) {
                return left.name.compare(right.name, Qt::CaseInsensitive) < 0;
            };
            std::ranges::sort(this->sevenTVBadges_, byName);
            std::ranges::sort(this->sevenTVPaints_, byName);
            if (preserveBadgeDraft || preservePaintDraft)
            {
                const auto containsCosmetic = [](const auto &cosmetics,
                                                 const QString &id) {
                    return id.isEmpty() ||
                           std::ranges::any_of(cosmetics,
                                               [&](const auto &item) {
                                                   return item.id == id;
                                               });
                };
                if (preserveBadgeDraft)
                {
                    const bool draftStillAvailable =
                        containsCosmetic(this->sevenTVBadges_, draftBadge);
                    this->selectedSevenTVBadge_ =
                        draftStillAvailable ? draftBadge
                        : containsCosmetic(this->sevenTVBadges_,
                                           this->originalSevenTVBadge_)
                            ? this->originalSevenTVBadge_
                            : QString{};
                    this->sevenTVBadgeTouched_ =
                        draftStillAvailable &&
                        draftBadge != this->originalSevenTVBadge_;
                }
                if (preservePaintDraft)
                {
                    const bool draftStillAvailable =
                        containsCosmetic(this->sevenTVPaints_, draftPaint);
                    this->selectedSevenTVPaint_ =
                        draftStillAvailable ? draftPaint
                        : containsCosmetic(this->sevenTVPaints_,
                                           this->originalSevenTVPaint_)
                            ? this->originalSevenTVPaint_
                            : QString{};
                    this->sevenTVPaintTouched_ =
                        draftStillAvailable &&
                        draftPaint != this->originalSevenTVPaint_;
                }
            }
            this->sevenTVToken_ = token;
            this->sevenTVLoaded_ = true;
            this->paintChoicesBuilt_ = false;
            this->sevenTVBadgeChoicesBuilt_ = false;
            if (!this->pendingSevenTVToken_.isEmpty())
            {
                getSettings()->sevenTVVanityToken = this->pendingSevenTVToken_;
                this->pendingSevenTVToken_.clear();
            }
            getSettings()->sevenTVVanityUserId = this->sevenTVUserId_;
            getSettings()->sevenTVVanityLogin = this->sevenTVLogin_;
            getSettings()->sevenTVVanityTwitchUserId = this->accountUserId_;
            getSettings()->sevenTVVanityTokenExpiry =
                tokenExpiry(getSettings()->sevenTVVanityToken.getValue());
            QStringList paintIds;
            paintIds.reserve(this->sevenTVPaints_.size());
            for (const auto &paint : this->sevenTVPaints_)
            {
                paintIds.push_back(paint.id);
            }
            getApp()->getSeventvPaints()->loadPaintsByID(paintIds,
                                                         forceRefresh);
            auto &vanityCache = sevenTVVanityCache();
            if (!vanityCache.contains(this->accountUserId_) &&
                vanityCache.size() >= MAX_SEVENTV_ACCOUNT_CACHE_ENTRIES)
            {
                vanityCache.erase(vanityCache.begin());
            }
            vanityCache.insert(
                this->accountUserId_,
                SevenTVVanityCacheEntry{
                    .tokenHash = fingerprint,
                    .userId = this->sevenTVUserId_,
                    .login = this->sevenTVLogin_,
                    .activeBadgeId = this->originalSevenTVBadge_,
                    .activePaintId = this->originalSevenTVPaint_,
                    .badges = this->sevenTVBadges_,
                    .paints = this->sevenTVPaints_,
                });
            this->refreshConnectionText();
            this->populateCurrentTab();
            if (connecting)
            {
                this->setStatus(QStringLiteral("7TV connected."));
            }
            this->refreshPreview();
        })
        .onError([this, connecting, token](const NetworkResult &result) {
            this->sevenTVRequestInFlight_ = false;
            this->updateSaveButtonState();
            if (!this->isOriginalAccountCurrent())
            {
                this->stopForAccountChange();
                return;
            }
            if (token != normalizedToken(this->pendingSevenTVToken_.isEmpty()
                         ? this->storedSevenTVTokenForCurrentAccount()
                         : this->pendingSevenTVToken_))
            {
                this->refreshConnectionText();
                return;
            }
            if (connecting)
            {
                this->pendingSevenTVToken_.clear();
            }
            if (result.status() == 401 || result.status() == 403)
            {
                this->pendingSevenTVToken_.clear();
                if (!connecting)
                {
                    this->clearSevenTVConnection();
                    this->sevenTVLoaded_ = false;
                }
                this->setStatus(
                    connecting
                        ? QStringLiteral("7TV did not accept that token.")
                        : QStringLiteral(
                              "Your 7TV connection expired. Connect it again."),
                    true);
            }
            else
            {
                this->setStatus(
                    QStringLiteral(
                        "7TV could not be reached. Your connection was kept."),
                    true);
            }
            this->refreshConnectionText();
            this->populateCurrentTab();
            this->refreshPreview();
        })
        .execute();
}

void VanityDialog::connectSevenTV()
{
    if (!this->isOriginalAccountCurrent())
    {
        this->stopForAccountChange();
        return;
    }
    const bool autoPinned = this->ensurePinned();
    const QPointer<VanityDialog> self(this);
    QPointer<QDialog> dialog = new QDialog(this);
    dialog->setFont(this->font());
    dialog->setWindowTitle(QStringLiteral("Connect 7TV"));
    dialog->setModal(true);
    dialog->setMinimumWidth(qRound(390 * this->scale()));

    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(7);

    auto *intro = new QLabel(
        QStringLiteral("Open 7TV while signed in, then run the copied command "
                       "in its browser console."),
        dialog);
    intro->setObjectName(QStringLiteral("SevenTVConnectIntro"));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *actions = new QHBoxLayout;
    auto *openSevenTV = new QPushButton(QStringLiteral("Open 7TV"), dialog);
    auto *copyHelper =
        new QPushButton(QStringLiteral("Copy login command"), dialog);
    actions->addWidget(openSevenTV);
    actions->addWidget(copyHelper);
    actions->addStretch(1);
    layout->addLayout(actions);

    auto *helperStatus = new QLabel(
        QStringLiteral("The command copies your token without displaying it."),
        dialog);
    helperStatus->setObjectName(QStringLiteral("SevenTVConnectHint"));
    helperStatus->setWordWrap(true);
    layout->addWidget(helperStatus);

    auto *tokenLabel = new QLabel(QStringLiteral("7TV token"), dialog);
    tokenLabel->setObjectName(QStringLiteral("SevenTVConnectTokenLabel"));
    layout->addWidget(tokenLabel);
    auto *tokenInput = new QLineEdit(dialog);
    tokenInput->setEchoMode(QLineEdit::Password);
    tokenInput->setMaxLength(4096);
    tokenInput->setPlaceholderText(QStringLiteral("Paste token"));
    tokenInput->setClearButtonEnabled(true);
    layout->addWidget(tokenInput);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, dialog);
    auto *connectButton =
        buttons->addButton(QStringLiteral("Connect"),
                           QDialogButtonBox::AcceptRole);
    connectButton->setEnabled(false);
    layout->addWidget(buttons);

    static const auto LOGIN_COMMAND = QStringLiteral(
        "(()=>{copy(localStorage.getItem('7tv-token'));"
        "return '7TV token copied.'})()");

    QObject::connect(openSevenTV, &QPushButton::clicked, dialog.data(), [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://7tv.app")));
    });
    QObject::connect(copyHelper, &QPushButton::clicked, dialog.data(),
                     [helperStatus] {
                         QGuiApplication::clipboard()->setText(LOGIN_COMMAND);
                         helperStatus->setText(QStringLiteral(
                             "Command copied. Run it on 7TV, then "
                             "paste the copied token below."));
                     });
    QObject::connect(tokenInput, &QLineEdit::textChanged, dialog.data(),
                     [connectButton](const QString &text) {
                         connectButton->setEnabled(
                             !normalizedToken(text).isEmpty());
                     });
    QObject::connect(connectButton, &QPushButton::clicked, dialog.data(),
                     &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog.data(),
                     &QDialog::reject);

    const auto background = getTheme()->window.background;
    const auto surface = getTheme()->splits.header.background;
    const auto input = getTheme()->splits.input.background;
    const auto border = getTheme()->splits.header.border;
    const auto text = getTheme()->window.text;
    auto muted = text;
    muted.setAlpha(170);
    dialog->setStyleSheet(
        QStringLiteral(
            "QDialog { background: %1; color: %2; }"
            "QLabel { color: %2; }"
            "QLabel#SevenTVConnectHint { "
            "color: %3; }"
            "QLineEdit { background: %4; color: %2; border: 1px solid %5; "
            "padding: 5px; selection-background-color: %6; }"
            "QPushButton { background: %7; color: %2; border: 1px solid %5; "
            "padding: 4px 10px; }"
            "QPushButton:hover:enabled { border-color: %6; }"
            "QPushButton:focus:enabled { border-color: %6; }"
            "QPushButton:pressed:enabled { background: %6; color: %2; "
            "padding-top: 5px; padding-bottom: 3px; }"
            "QPushButton:disabled { color: %3; }")
            .arg(background.name(QColor::HexArgb), text.name(QColor::HexArgb),
                 muted.name(QColor::HexArgb), input.name(QColor::HexArgb),
                 border.name(QColor::HexArgb),
                 getTheme()->tabs.selected.backgrounds.regular.name(
                     QColor::HexArgb),
                 surface.name(QColor::HexArgb)));

    for (auto *widget : dialog->findChildren<QWidget *>())
    {
        widget->setFont(this->font());
    }
    tokenInput->setFocus();
    const auto result = dialog->exec();
    if (!self || !dialog)
    {
        return;
    }
    const auto token = normalizedToken(tokenInput->text());
    delete dialog;
    if (autoPinned)
    {
        this->togglePinned();
    }
    if (result != QDialog::Accepted)
    {
        return;
    }
    if (!this->isOriginalAccountCurrent())
    {
        this->stopForAccountChange();
        return;
    }
    if (token.isEmpty())
    {
        this->setStatus(QStringLiteral("Paste a valid 7TV token."), true);
        return;
    }
    this->pendingSevenTVToken_ = token;
    this->loadSevenTVState(true);
}

void VanityDialog::disconnectSevenTV()
{
    if (!this->isOriginalAccountCurrent())
    {
        this->stopForAccountChange();
        return;
    }
    this->pendingSevenTVToken_.clear();
    this->clearSevenTVConnection();
    this->sevenTVUserId_.clear();
    this->sevenTVLogin_.clear();
    this->sevenTVBadges_.clear();
    this->sevenTVPaints_.clear();
    this->sevenTVLoaded_ = false;
    this->paintChoicesBuilt_ = false;
    this->sevenTVBadgeChoicesBuilt_ = false;
    this->selectedSevenTVBadge_.clear();
    this->selectedSevenTVPaint_.clear();
    this->sevenTVBadgeTouched_ = false;
    this->sevenTVPaintTouched_ = false;
    this->paintGrid_->setChoices({}, {}, this->accountDisplayName_,
                                 this->selectedColor_);
    this->sevenTVBadgeGrid_->setChoices({}, {}, this->accountDisplayName_,
                                        this->selectedColor_);
    this->refreshConnectionText();
    this->populateCurrentTab();
    this->refreshPreview();
}

void VanityDialog::populateCurrentTab()
{
    if (this->tabs_ == nullptr)
    {
        return;
    }
    const auto *page = this->tabs_->currentWidget();
    if (page == this->globalBadgePage_)
    {
        if (!this->previewTwitchEventBadge_.isEmpty())
        {
            this->previewTwitchEventForChannel_ = false;
        }
        this->loadTwitchEventBadges();
        this->populateGlobalBadgeChoices();
    }
    else if (page == this->channelBadgePage_)
    {
        if (!this->previewTwitchEventBadge_.isEmpty())
        {
            this->previewTwitchEventForChannel_ = true;
        }
        this->loadTwitchEventBadges();
        this->populateChannelBadgeChoices();
    }
    else if (page == this->sevenTVBadgePage_)
    {
        this->populateSevenTVBadgeChoices();
    }
    else if (page == this->paintPage_)
    {
        this->populatePaintChoices();
    }
    else if (page == this->moltorinoBadgePage_)
    {
        this->populateMoltorinoBadgeChoices();
    }
    else if (page == this->layoutPage_)
    {
        this->refreshPreview();
        this->rebuildLayoutRows();
    }
    QTimer::singleShot(0, this, &VanityDialog::updateVisualRefreshTimer);
}

void VanityDialog::populatePaintChoices()
{
    if (this->paintChoicesBuilt_)
    {
        this->paintGrid_->setSelected(this->selectedSevenTVPaint_);
        return;
    }
    if (!this->sevenTVLoaded_)
    {
        this->paintHint_->hide();
        return;
    }

    QVector<VanityChoice> choices;
    QStringList paintIDs;
    choices.reserve(this->sevenTVPaints_.size() + 1);
    paintIDs.reserve(this->sevenTVPaints_.size() + 1);
    choices.push_back({{}, QStringLiteral("No paint"), {}});
    for (const auto &paint : this->sevenTVPaints_)
    {
        choices.push_back({paint.id, paint.name, {}});
        paintIDs.push_back(paint.id);
    }
    if (!this->selectedSevenTVPaint_.isEmpty() &&
        std::ranges::none_of(choices, [&](const auto &choice) {
            return choice.key == this->selectedSevenTVPaint_;
        }))
    {
        choices.push_back(
            {this->selectedSevenTVPaint_, QStringLiteral("Current paint"), {}});
        paintIDs.push_back(this->selectedSevenTVPaint_);
    }
    getApp()->getSeventvPaints()->loadPaintsByID(paintIDs);
    this->paintGrid_->setChoices(
        std::move(choices), this->selectedSevenTVPaint_,
        this->accountDisplayName_, this->selectedColor_);
    this->paintHint_->hide();
    this->paintChoicesBuilt_ = true;
}

void VanityDialog::populateGlobalBadgeChoices()
{
    if (this->globalBadgeChoicesBuilt_)
    {
        if (!this->previewTwitchEventBadge_.isEmpty() &&
            !this->previewTwitchEventForChannel_)
        {
            this->globalBadgeGrid_->setSelected(
                this->previewTwitchEventBadge_);
        }
        else if (this->selectedChannelBadge_.isEmpty())
        {
            this->globalBadgeGrid_->setSelected(this->selectedGlobalBadge_);
        }
        else
        {
            this->globalBadgeGrid_->clearSelectedDisplay();
        }
        return;
    }
    if (!this->twitchLoaded_)
    {
        this->globalBadgeHint_->setText(
            this->twitchLoadFinished_
                ? this->twitchBadgeHint_
                : QStringLiteral("Loading Twitch badges..."));
        this->globalBadgeHint_->setToolTip(this->twitchBadgeError_);
        this->globalBadgeHint_->show();
        return;
    }

    QVector<VanityChoice> choices;
    choices.reserve(this->twitchState_.globalBadges.size() +
                    this->twitchEventBadges_.size() + 1);
    choices.push_back({{}, QStringLiteral("No global badge"), {}});
    QSet<QString> owned;
    for (const auto &badge : this->twitchState_.globalBadges)
    {
        const auto id = badge.setId.toLower();
        const bool duplicate = owned.contains(id);
        owned.insert(id);
        if (duplicate || TwitchBadge::vanitySlotKeyForSet(badge.setId) !=
            QStringLiteral("tv"))
        {
            continue;
        }
        choices.push_back(
            {badge.setId, badge.title,
             badge.image2.isEmpty() ? badge.image1 : badge.image2});
    }
    for (const auto &badge : this->twitchState_.channelBadges)
    {
        owned.insert(badge.setId.toLower());
    }
    const auto now = QDateTime::currentDateTimeUtc();
    for (const auto &badge : this->twitchEventBadges_)
    {
        if (!isTwitchEventBadgeActive(badge, now) || owned.contains(badge.id))
        {
            continue;
        }
        choices.push_back({badge.id, badge.name, badge.imageUrl, {},
                           twitchEventBadgeDescription(badge), false});
    }
    const auto displayedSelection =
        !this->previewTwitchEventBadge_.isEmpty() &&
                !this->previewTwitchEventForChannel_
            ? this->previewTwitchEventBadge_
            : this->selectedGlobalBadge_;
    this->globalBadgeGrid_->setChoices(
        std::move(choices), displayedSelection, this->accountDisplayName_,
        this->selectedColor_);
    if (this->previewTwitchEventBadge_.isEmpty() &&
        !this->selectedChannelBadge_.isEmpty())
    {
        this->globalBadgeGrid_->clearSelectedDisplay();
    }
    this->refreshTwitchBadgeHint(false);
    this->globalBadgeChoicesBuilt_ = true;
}

void VanityDialog::populateChannelBadgeChoices()
{
    if (this->channelBadgeChoicesBuilt_)
    {
        this->channelBadgeGrid_->setSelected(
            !this->previewTwitchEventBadge_.isEmpty() &&
                    this->previewTwitchEventForChannel_
                ? this->previewTwitchEventBadge_
                : this->selectedChannelBadge_);
        return;
    }
    if (!this->twitchLoaded_)
    {
        this->channelBadgeHint_->setText(
            this->twitchLoadFinished_
                ? this->twitchBadgeHint_
                : QStringLiteral("Loading Twitch badges..."));
        this->channelBadgeHint_->setToolTip(this->twitchBadgeError_);
        this->channelBadgeHint_->show();
        return;
    }

    QVector<VanityChoice> choices;
    choices.reserve(this->twitchState_.globalBadges.size() +
                    this->twitchState_.channelBadges.size() +
                    this->twitchEventBadges_.size() + 1);
    choices.push_back({{}, QStringLiteral("Use Twitch default"), {}});
    QSet<QString> owned;
    const auto appendOwned = [&](const GqlVanityBadge &badge) {
        const auto id = badge.setId.toLower();
        if (owned.contains(id))
        {
            return;
        }
        owned.insert(id);
        if (TwitchBadge::vanitySlotKeyForSet(badge.setId) !=
            QStringLiteral("tv"))
        {
            return;
        }
        choices.push_back(
            {badge.setId, badge.title,
             badge.image2.isEmpty() ? badge.image1 : badge.image2});
    };

    for (const auto &badge : this->twitchState_.globalBadges)
    {
        appendOwned(badge);
    }
    for (const auto &badge : this->twitchState_.channelBadges)
    {
        appendOwned(badge);
    }
    const auto now = QDateTime::currentDateTimeUtc();
    for (const auto &badge : this->twitchEventBadges_)
    {
        if (!isTwitchEventBadgeActive(badge, now) || owned.contains(badge.id))
        {
            continue;
        }
        choices.push_back({badge.id, badge.name, badge.imageUrl, {},
                           twitchEventBadgeDescription(badge), false});
    }
    this->channelBadgeGrid_->setChoices(
        std::move(choices),
        !this->previewTwitchEventBadge_.isEmpty() &&
                this->previewTwitchEventForChannel_
            ? this->previewTwitchEventBadge_
            : this->selectedChannelBadge_,
        this->accountDisplayName_, this->selectedColor_);
    this->refreshTwitchBadgeHint(true);
    this->channelBadgeChoicesBuilt_ = true;
}

void VanityDialog::populateSevenTVBadgeChoices()
{
    if (this->sevenTVBadgeChoicesBuilt_)
    {
        this->sevenTVBadgeGrid_->setSelected(this->selectedSevenTVBadge_);
        return;
    }
    if (!this->sevenTVLoaded_)
    {
        this->sevenTVBadgeHint_->hide();
        return;
    }

    QVector<VanityChoice> choices;
    choices.reserve(this->sevenTVBadges_.size() + 1);
    choices.push_back({{}, QStringLiteral("No 7TV badge"), {}});
    for (const auto &badge : this->sevenTVBadges_)
    {
        choices.push_back({badge.id, badge.name, badge.imageUrl,
                           compactSevenTVBadgeName(badge.name)});
    }
    if (!this->selectedSevenTVBadge_.isEmpty() &&
        std::ranges::none_of(choices, [&](const auto &choice) {
            return choice.key == this->selectedSevenTVBadge_;
        }))
    {
        choices.push_back(
            {this->selectedSevenTVBadge_, QStringLiteral("Current badge"), {}});
    }
    this->sevenTVBadgeGrid_->setChoices(
        std::move(choices), this->selectedSevenTVBadge_,
        this->accountDisplayName_, this->selectedColor_);
    this->sevenTVBadgeHint_->hide();
    this->sevenTVBadgeChoicesBuilt_ = true;
}

void VanityDialog::populateMoltorinoBadgeChoices()
{
    this->syncEffectiveMoltorinoSelection();
    if (this->moltorinoBadgeChoicesBuilt_)
    {
        this->moltorinoBadgeGrid_->setSelected(
            this->previewMoltorinoBadge_.isEmpty()
                ? this->selectedMoltorinoBadge_
                : this->previewMoltorinoBadge_);
        return;
    }

    const auto catalog = this->moltorinoBadgeCatalog();
    const auto assigned = this->availableMoltorinoBadges();
    QSet<QString> ownedIds;
    for (const auto &badge : assigned)
    {
        ownedIds.insert(badge.categoryId);
    }
    QVector<VanityChoice> choices;
    choices.reserve(int(catalog.size()) + 1);
    choices.push_back({{}, QStringLiteral("No badge"), {}, {},
                       QStringLiteral("Do not show a Moltorino badge."), true});
    auto orderedCatalog = catalog;
    std::stable_partition(
        orderedCatalog.begin(), orderedCatalog.end(),
        [&](const auto &badge) { return ownedIds.contains(badge.categoryId); });
    for (const auto &badge : orderedCatalog)
    {
        const auto owned = ownedIds.contains(badge.categoryId);
        if (!vanity::detail::shouldShowMoltorinoBadge(
                badge.listedInVanity, owned))
        {
            continue;
        }
        if (!badge.emote)
        {
            continue;
        }
        const auto image = badge.emote->images.getImage2()->url().string;
        const auto fullName = badge.displayName.isEmpty()
                                  ? badge.emote->tooltip.string
                                  : badge.displayName;
        choices.push_back({badge.categoryId, fullName, image,
                           compactMoltorinoBadgeName(fullName),
                           badge.description, owned});
    }
    this->moltorinoBadgeGrid_->setChoices(
        std::move(choices),
        this->previewMoltorinoBadge_.isEmpty()
            ? this->selectedMoltorinoBadge_
            : this->previewMoltorinoBadge_,
        this->accountDisplayName_, this->selectedColor_);
    if (catalog.empty())
    {
        this->moltorinoBadgeHint_->setText(
            QStringLiteral("Loading the Moltorino badge collection..."));
        this->moltorinoBadgeHint_->show();
    }
    else if (assigned.empty())
    {
        this->moltorinoBadgeHint_->setText(
            QStringLiteral(
                "Preview any badge here. Unlock one before you can equip it."));
        this->moltorinoBadgeHint_->show();
    }
    else
    {
        this->moltorinoBadgeHint_->setText(
            QStringLiteral(
                "Choose one of your badges, or preview the rest."));
        this->moltorinoBadgeHint_->show();
    }
    this->moltorinoBadgeChoicesBuilt_ = true;
}

std::vector<MoltorinoSupporterBadge> VanityDialog::availableMoltorinoBadges()
    const
{
    auto *provider = getApp()->getMoltorinoSupporterBadges();
    if (provider == nullptr)
    {
        return {};
    }
    if (this->moltorinoAssignmentsAuthoritative_)
    {
        return provider->getBadgesByCategoryIds(
            this->assignedMoltorinoBadgeIds_);
    }
    return provider->getAssignedBadges(this->accountUserId_);
}

std::vector<MoltorinoSupporterBadge> VanityDialog::moltorinoBadgeCatalog()
    const
{
    auto *provider = getApp()->getMoltorinoSupporterBadges();
    return provider == nullptr ? std::vector<MoltorinoSupporterBadge>{}
                               : provider->getBadgeCatalog();
}

void VanityDialog::refreshMoltorinoBadgeAvailability()
{
    if (this->tabs_ == nullptr || this->moltorinoBadgePage_ == nullptr)
    {
        return;
    }

    const auto index = this->tabs_->indexOf(this->moltorinoBadgePage_);
    if (index < 0)
    {
        return;
    }
    this->tabs_->setTabVisible(index, true);
    this->syncEffectiveMoltorinoSelection();
}

void VanityDialog::syncEffectiveMoltorinoSelection()
{
    if (this->workingLayout_.moltorinoBadgeSelectionExplicit)
    {
        this->selectedMoltorinoBadge_ =
            this->workingLayout_.moltorinoBadge;
        return;
    }

    const auto assigned = this->availableMoltorinoBadges();
    this->selectedMoltorinoBadge_ =
        assigned.empty() ? QString{} : assigned.front().categoryId;
}

void VanityDialog::useLoadedLayout(const MoltorinoVanityLayout &layout)
{
    const auto normalized = vanity::detail::normalizeLayout(layout);
    this->originalLayout_ = normalized;
    this->workingLayout_ = normalized;
    this->previewMoltorinoBadge_.clear();
    this->syncEffectiveMoltorinoSelection();
    this->layoutTouched_ = false;
    this->moltorinoBadgeChoicesBuilt_ = false;
}

void VanityDialog::syncWorkingLayoutFromRows()
{
    if (this->layoutList_ == nullptr || this->rebuildingLayoutRows_)
    {
        return;
    }

    std::vector<QString> activeOrder;
    activeOrder.reserve(static_cast<size_t>(this->layoutList_->count()));
    for (int i = 0; i < this->layoutList_->count(); ++i)
    {
        const auto *item = this->layoutList_->item(i);
        const auto key = item->data(ORDER_KEY_ROLE).toString();
        const auto slot = item->data(ORDER_SLOT_ROLE).toString();
        if (key.isEmpty() || slot.isEmpty())
        {
            continue;
        }
        activeOrder.push_back(slot);
        vanity::detail::setBadgeVisible(
            this->workingLayout_, key, item->data(ORDER_VISIBLE_ROLE).toBool());
    }
    vanity::detail::replaceActiveBadgeOrder(this->workingLayout_, activeOrder);
    this->workingLayout_.moltorinoBadge =
        this->workingLayout_.moltorinoBadgeSelectionExplicit
            ? this->selectedMoltorinoBadge_
            : QString{};
}

void VanityDialog::updateLayoutDirtyState()
{
    this->workingLayout_.moltorinoBadge =
        this->workingLayout_.moltorinoBadgeSelectionExplicit
            ? this->selectedMoltorinoBadge_
            : QString{};
    this->layoutTouched_ = this->workingLayout_ != this->originalLayout_;
}

void VanityDialog::rebuildLayoutRows()
{
    if (this->layoutList_ == nullptr || !this->layoutLoaded_)
    {
        return;
    }

    QStringList availableSlots;
    for (const auto &row : this->activeBadgeRows_)
    {
        if (!row.slot.isEmpty() && !availableSlots.contains(row.slot))
        {
            availableSlots.push_back(row.slot);
        }
    }

    const std::vector<QString> available(availableSlots.begin(),
                                         availableSlots.end());
    vanity::detail::appendMissingBadgeKeys(this->workingLayout_, available);

    QHash<QString, int> ranks;
    for (int i = 0; i < int(this->workingLayout_.order.size()); ++i)
    {
        ranks.insert(this->workingLayout_.order.at(i), i);
    }
    auto rows = this->activeBadgeRows_;
    std::stable_sort(rows.begin(), rows.end(),
                     [&ranks](const auto &left, const auto &right) {
                         return ranks.value(left.slot, 1000) <
                                ranks.value(right.slot, 1000);
                     });

    const QScopedValueRollback rebuilding(this->rebuildingLayoutRows_, true);
    const QSignalBlocker viewBlocker(this->layoutList_);
    this->layoutList_->clear();
    for (const auto &row : rows)
    {
        const bool visible =
            vanity::detail::isBadgeVisible(this->workingLayout_, row.key);
        auto *item = new QListWidgetItem(
            row.name.isEmpty() ? this->layoutDisplayName(row.key) : row.name,
            this->layoutList_);
        item->setData(ORDER_KEY_ROLE, row.key);
        item->setData(ORDER_SLOT_ROLE, row.slot);
        item->setData(ORDER_VISIBLE_ROLE, visible);
        item->setToolTip(visible ? QStringLiteral("Visible in chat")
                                 : QStringLiteral("Hidden in chat"));
    }
}

void VanityDialog::refreshPreview()
{
    if (this->preview_ == nullptr)
    {
        return;
    }
    QVector<PreviewBadge> badges;
    const auto addBadge = [&badges](QString key, QString provider,
                                    const EmotePtr &emote) {
        if (emote && !emote->images.getImage1()->isEmpty())
        {
            const auto title = emote->tooltip.string.isEmpty()
                                   ? emote->name.string
                                   : emote->tooltip.string;
            const auto slot = vanity::detail::badgeOrderSlot(key);
            badges.push_back({std::move(key), slot,
                              provider + QStringLiteral(": ") + title,
                              emote->images.getImage1()});
        }
    };
    const auto addUrl = [&badges](QString key, QString name,
                                  const QString &url) {
        if (!url.isEmpty())
        {
            const auto slot = vanity::detail::badgeOrderSlot(key);
            badges.push_back({std::move(key), slot, std::move(name),
                              Image::fromUrl(Url{url}, 1, QSize{26, 26})});
        }
    };

    auto twitchBadges =
        this->channel_ == nullptr
            ? std::vector<TwitchBadge>{}
            : this->channel_->currentUserBadges(this->accountUserId_);
    if (twitchBadges.empty() && this->channel_ != nullptr)
    {
        if (this->channel_->isBroadcaster())
        {
            twitchBadges.emplace_back(QStringLiteral("broadcaster"),
                                      QStringLiteral("1"));
        }
        else if (this->channel_->isLeadMod())
        {
            twitchBadges.emplace_back(QStringLiteral("lead_moderator"),
                                      QStringLiteral("1"));
        }
        else if (this->channel_->isMod())
        {
            twitchBadges.emplace_back(QStringLiteral("moderator"),
                                      QStringLiteral("1"));
        }
        if (this->channel_->isVip())
        {
            twitchBadges.emplace_back(QStringLiteral("vip"),
                                      QStringLiteral("1"));
        }
        if (this->channel_->isStaff())
        {
            twitchBadges.emplace_back(QStringLiteral("staff"),
                                      QStringLiteral("1"));
        }
    }

    const auto effectiveGlobalBadge = this->selectedChannelBadge_.isEmpty()
                                          ? this->selectedGlobalBadge_
                                          : QString{};
    const auto *effectiveSelectedBadge =
        this->selectedChannelBadge_.isEmpty()
            ? this->findGlobalBadge(effectiveGlobalBadge)
            : this->findChannelBadge(this->selectedChannelBadge_);
    if (this->twitchLoaded_ || !this->previewTwitchEventBadge_.isEmpty())
    {
        twitchBadges = vanity::detail::withSelectedTwitchVanityBadge(
            std::move(twitchBadges),
            this->previewTwitchEventBadge_.isEmpty() &&
                    effectiveSelectedBadge != nullptr
                ? effectiveSelectedBadge->setId
                : QString{},
            this->previewTwitchEventBadge_.isEmpty() &&
                    effectiveSelectedBadge != nullptr
                ? effectiveSelectedBadge->version
                : QString{});
    }

    QSet<QString> addedTwitchKeys;
    for (const auto &badge : twitchBadges)
    {
        std::optional<EmotePtr> emote;
        if (this->channel_ != nullptr)
        {
            emote = this->channel_->twitchBadge(badge.key_, badge.value_);
        }
        if (!emote)
        {
            emote =
                getApp()->getTwitchBadges()->badge(badge.key_, badge.value_);
        }
        const auto key = QStringLiteral("t:") + badge.key_.toLower();
        if (emote)
        {
            addBadge(key, QStringLiteral("Twitch"), *emote);
            addedTwitchKeys.insert(key);
        }
    }
    if (this->previewTwitchEventBadge_.isEmpty() &&
        effectiveSelectedBadge != nullptr)
    {
        const auto key = QStringLiteral("t:") +
                         effectiveSelectedBadge->setId.toLower();
        if (!addedTwitchKeys.contains(key))
        {
            addUrl(key,
                   QStringLiteral("Twitch: ") +
                       effectiveSelectedBadge->title,
                   effectiveSelectedBadge->image2.isEmpty()
                       ? effectiveSelectedBadge->image1
                       : effectiveSelectedBadge->image2);
            addedTwitchKeys.insert(key);
        }
    }
    else if (const auto *preview =
                 this->findTwitchEventBadge(
                     this->previewTwitchEventBadge_))
    {
        const auto key = QStringLiteral("t:") + preview->id;
        if (!addedTwitchKeys.contains(key))
        {
            addUrl(key, QStringLiteral("Twitch: ") + preview->name,
                   preview->imageUrl);
            addedTwitchKeys.insert(key);
        }
    }
    if (auto badge =
            getApp()->getChatterinoBadges()->getBadge({this->accountUserId_}))
    {
        addBadge(QStringLiteral("c"), QStringLiteral("Chatterino"), *badge);
    }
    for (const auto &badge :
         getApp()->getFfzBadges()->getUserBadges({this->accountUserId_}))
    {
        addBadge(QStringLiteral("ff"), QStringLiteral("FrankerFaceZ"),
                 badge.emote);
    }
    if (auto *ffzAp = getApp()->getFfzApBadges())
    {
        if (auto badge = ffzAp->getBadge({this->accountUserId_}))
        {
            addBadge(QStringLiteral("fa"), QStringLiteral("FFZ:AP"),
                     badge->emote);
        }
    }
    if (this->channel_)
    {
        for (const auto &badge :
             this->channel_->ffzChannelBadges(this->accountUserId_))
        {
            addBadge(QStringLiteral("ff"), QStringLiteral("FrankerFaceZ"),
                     badge.emote);
        }
    }
    if (auto badge =
            getApp()->getBttvBadges()->getBadge({this->accountUserId_}))
    {
        addBadge(QStringLiteral("bt"), QStringLiteral("BetterTTV"), *badge);
    }
    if (auto *provider = getApp()->getBluzyrinoBadges())
    {
        for (const auto &badge : provider->getBadges({this->accountUserId_}))
        {
            addBadge(QStringLiteral("bl"), QStringLiteral("Bluzyrino"), badge);
        }
    }
    if (getApp()->getMoltorinoSupporterBadges() != nullptr)
    {
        const bool previewingLockedBadge =
            !this->previewMoltorinoBadge_.isEmpty();
        const auto moltorinoBadges = previewingLockedBadge
                                         ? this->moltorinoBadgeCatalog()
                                         : this->availableMoltorinoBadges();
        const auto selectedId = previewingLockedBadge
                                    ? this->previewMoltorinoBadge_
                                    : this->selectedMoltorinoBadge_;
        auto selected = std::ranges::find(
            moltorinoBadges, selectedId,
            &MoltorinoSupporterBadge::categoryId);
        if (selected != moltorinoBadges.end())
        {
            addBadge(QStringLiteral("m"), QStringLiteral("Moltorino"),
                     selected->emote);
        }
    }
    const auto selectedSevenTVBadge = this->selectedSevenTVBadgeId();
    if (const auto *badge = this->findSevenTVBadge(selectedSevenTVBadge))
    {
        addUrl(QStringLiteral("7"), QStringLiteral("7TV: ") + badge->name,
               badge->imageUrl);
    }
    else if (!this->sevenTVLoaded_ ||
             (!selectedSevenTVBadge.isEmpty() &&
              selectedSevenTVBadge == this->originalSevenTVBadge_))
    {
        if (auto activeSevenTVBadge =
                getApp()->getSeventvBadges()->getBadge({this->accountUserId_}))
        {
            addBadge(QStringLiteral("7"), QStringLiteral("7TV"),
                     *activeSevenTVBadge);
        }
    }
    if (auto *homies = getApp()->getHomiesBadges())
    {
        const auto homiesBadges = homies->getBadges(this->accountUserId_);
        for (int i = 0; i < int(homiesBadges.size()); ++i)
        {
            addBadge(i == 0 ? QStringLiteral("hc") : QStringLiteral("hs"),
                     QStringLiteral("Homies"), homiesBadges.at(i));
        }
    }

    QVector<ActiveBadgeRow> activeRows;
    QHash<QString, int> activeSlotRows;
    for (const auto &badge : badges)
    {
        const auto existing = activeSlotRows.constFind(badge.slot);
        if (existing == activeSlotRows.cend())
        {
            activeSlotRows.insert(badge.slot, activeRows.size());
            activeRows.push_back({badge.key, badge.slot, badge.name});
            continue;
        }

        auto &row = activeRows[*existing];
        row.key = badge.slot;
        row.name = this->layoutDisplayName(badge.slot);
    }
    if (activeRows != this->activeBadgeRows_ ||
        (this->layoutList_ != nullptr &&
         this->layoutList_->count() != activeRows.size()))
    {
        this->activeBadgeRows_ = std::move(activeRows);
        this->rebuildLayoutRows();
    }

    const auto layout = this->editedLayout();
    badges.erase(std::remove_if(badges.begin(), badges.end(),
                                [&layout](const auto &badge) {
                                    return !vanity::detail::isBadgeVisible(
                                        layout, badge.key);
                                }),
                 badges.end());
    QHash<QString, int> ranks;
    for (int i = 0; i < int(layout.order.size()); ++i)
    {
        ranks.insert(layout.order.at(i), i);
    }
    std::stable_sort(badges.begin(), badges.end(),
                     [&ranks](const auto &left, const auto &right) {
                         return ranks.value(left.slot, 1000) <
                                ranks.value(right.slot, 1000);
                     });

    const auto paintId = this->sevenTVLoaded_
                             ? this->selectedSevenTVPaintId()
                             : QString{};
    if (!paintId.isEmpty())
    {
        getApp()->getSeventvPaints()->loadPaintByID(paintId);
    }
    const bool useActivePaint = !this->sevenTVLoaded_;
    this->preview_->setPreview(this->accountDisplayName_, this->accountLogin_,
                               this->selectedColor_, paintId, useActivePaint,
                               std::move(badges));
    if (this->paintGrid_ != nullptr)
    {
        this->paintGrid_->setSelected(paintId);
        this->paintGrid_->setPaintPreview(this->accountDisplayName_,
                                          this->selectedColor_);
    }
    if (this->colorPicker_ != nullptr)
    {
        this->colorPicker_->setColor(this->selectedColor_);
    }
    this->refreshColorPreviewChip();
    if (this->colorHexInput_ != nullptr && !this->colorHexInput_->hasFocus())
    {
        this->colorHexInput_->setText(this->selectedColor_.name().toUpper());
    }
}

void VanityDialog::refreshHeaderIdentity()
{
    if (this->headerIdentityLabel_ == nullptr)
    {
        return;
    }
    this->headerIdentityLabel_->setText(
        QStringLiteral("%1  \u00b7  #%2")
            .arg(this->accountDisplayName_, this->channel_->getName()));
}

void VanityDialog::refreshConnectionText()
{
    const auto token =
        normalizedToken(this->storedSevenTVTokenForCurrentAccount());
    for (auto *label : this->sevenTVConnectionLabels_)
    {
        if (this->sevenTVRequestInFlight_)
        {
            label->setText(this->sevenTVLoaded_
                               ? QStringLiteral("Refreshing 7TV...")
                               : QStringLiteral("Loading 7TV..."));
        }
        else if (token.isEmpty())
        {
            label->setText(label->property("disconnectedText").toString());
        }
        else
        {
            label->setText(
                QStringLiteral("Connected as %1%2")
                    .arg(this->sevenTVLogin_.isEmpty()
                             ? getSettings()->sevenTVVanityLogin.getValue()
                             : this->sevenTVLogin_,
                         this->sevenTVTokenExpiryText()));
        }
    }
    for (auto *button : this->sevenTVConnectButtons_)
    {
        button->setVisible(token.isEmpty());
        button->setEnabled(!this->sevenTVRequestInFlight_);
    }
    for (auto *button : this->sevenTVDisconnectButtons_)
    {
        button->setVisible(!token.isEmpty());
        button->setEnabled(!this->sevenTVRequestInFlight_);
    }
    for (auto *button : this->sevenTVRefreshButtons_)
    {
        button->setVisible(!token.isEmpty());
        button->setEnabled(!this->sevenTVRequestInFlight_);
    }
}

void VanityDialog::save()
{
    if (this->saveInFlight_ || this->layoutRequestInFlight_ ||
        this->sevenTVRequestInFlight_ ||
        !this->layoutLoaded_ || this->accountUserId_.isEmpty())
    {
        return;
    }
    if (!this->isOriginalAccountCurrent())
    {
        this->stopForAccountChange();
        return;
    }

    bool restoredPreview = false;
    if (!this->previewTwitchEventBadge_.isEmpty())
    {
        this->clearTwitchEventBadgePreview();
        restoredPreview = true;
    }
    if (!this->previewMoltorinoBadge_.isEmpty())
    {
        this->previewMoltorinoBadge_.clear();
        if (this->moltorinoBadgeGrid_ != nullptr)
        {
            this->moltorinoBadgeGrid_->setSelected(
                this->selectedMoltorinoBadge_);
        }
        restoredPreview = true;
    }
    if (restoredPreview)
    {
        this->refreshPreview();
    }
    this->syncWorkingLayoutFromRows();
    this->updateLayoutDirtyState();
    this->savedChanges_.clear();
    this->saveSevenTVAuthGeneration_ = this->sevenTVAuthGeneration_;
    this->autoPinnedForSave_ = this->ensurePinned();
    this->setBusy(true);
    this->setStatus(QStringLiteral("Saving..."));
    this->runSaveStep(0);
}

void VanityDialog::runSaveStep(int step)
{
    if (!this->isOriginalAccountCurrent())
    {
        this->stopForAccountChange();
        return;
    }
    if (this->sevenTVLoaded_ &&
        (this->saveSevenTVAuthGeneration_ != this->sevenTVAuthGeneration_ ||
         this->sevenTVToken_ !=
             normalizedToken(this->storedSevenTVTokenForCurrentAccount())))
    {
        this->failSave(QStringLiteral("7TV changes"),
                       QStringLiteral("Your 7TV connection changed. Reopen "
                                      "/vanity before saving."));
        return;
    }
    const auto auth = this->activeTwitchAuth_.value_or(TwitchGqlAuth{});
    if (step == 0)
    {
        if (this->selectedColor_ == this->originalColor_)
        {
            return this->runSaveStep(1);
        }
        getHelix()->updateUserChatColor(
            this->accountUserId_, this->selectedColorValue_,
            [self = QPointer<VanityDialog>(this)] {
                if (!self)
                {
                    return;
                }
                self->originalColor_ = self->selectedColor_;
                self->recordSavedChange(QStringLiteral("Twitch color"));
                if (self->isOriginalAccountCurrent())
                {
                    getApp()->getAccounts()->twitch.getCurrent()->setColor(
                        self->selectedColor_);
                }
                self->runSaveStep(1);
            },
            [self = QPointer<VanityDialog>(this)](
                HelixUpdateUserChatColorError error, const QString &message) {
                if (!self)
                {
                    return;
                }
                QString text;
                switch (error)
                {
                    case HelixUpdateUserChatColorError::UserMissingScope:
                        text = QStringLiteral(
                            "Reconnect your Twitch account and try again.");
                        break;
                    case HelixUpdateUserChatColorError::InvalidColor:
                        text = QStringLiteral(
                            "Twitch did not accept that color. Custom colors "
                            "require Turbo or Prime.");
                        break;
                    case HelixUpdateUserChatColorError::Forwarded:
                        text = message;
                        break;
                    case HelixUpdateUserChatColorError::Unknown:
                    default:
                        text = QStringLiteral(
                            "Twitch could not change your color.");
                        break;
                }
                self->failSave(QStringLiteral("Twitch color"), text);
            });
        return;
    }
    if (step == 1)
    {
        if (!this->twitchLoaded_)
        {
            return this->runSaveStep(3);
        }
        if (!auth.isValid())
        {
            this->failSave(
                QStringLiteral("Twitch badge"),
                QStringLiteral("Reload Twitch badges and try again"));
            return;
        }

        if (!this->selectedGlobalBadge_.isEmpty() &&
            this->selectedGlobalBadge_.compare(this->originalGlobalBadge_,
                                               Qt::CaseInsensitive) != 0)
        {
            const auto *badge =
                this->findGlobalBadge(this->selectedGlobalBadge_);
            if (badge == nullptr)
            {
                this->failSave(
                    QStringLiteral("Twitch badge"),
                    QStringLiteral("That badge is no longer available"));
                return;
            }
            const auto selected = badge->setId;
            TwitchGql::selectGlobalBadge(
                badge->setId, badge->version, auth,
                [self = QPointer<VanityDialog>(this), selected] {
                    if (self)
                    {
                        self->originalGlobalBadge_ = selected;
                        self->recordSavedChange(QStringLiteral("Twitch badge"));
                        self->runSaveStep(1);
                    }
                },
                [self = QPointer<VanityDialog>(this)](const QString &error) {
                    if (self)
                    {
                        self->failSave(QStringLiteral("Twitch badge"), error);
                    }
                });
            return;
        }
        if (!this->selectedChannelBadge_.isEmpty() &&
            this->selectedChannelBadge_.compare(this->originalChannelBadge_,
                                                Qt::CaseInsensitive) != 0)
        {
            if (this->twitchState_.channelId.isEmpty())
            {
                this->failSave(
                    QStringLiteral("Channel badge"),
                    QStringLiteral("Twitch did not return this channel yet"));
                return;
            }
            const auto *badge =
                this->findChannelBadge(this->selectedChannelBadge_);
            if (badge == nullptr)
            {
                this->failSave(
                    QStringLiteral("Channel badge"),
                    QStringLiteral("That badge is no longer available"));
                return;
            }
            const auto selected = badge->setId;
            TwitchGql::selectChannelBadge(
                this->twitchState_.channelId, badge->setId, badge->version,
                auth,
                [self = QPointer<VanityDialog>(this), selected] {
                    if (self)
                    {
                        self->originalChannelBadge_ = selected;
                        self->recordSavedChange(
                            QStringLiteral("Channel badge"));
                        self->runSaveStep(1);
                    }
                },
                [self = QPointer<VanityDialog>(this)](const QString &error) {
                    if (self)
                    {
                        self->failSave(QStringLiteral("Channel badge"), error);
                    }
                });
            return;
        }
        return this->runSaveStep(2);
    }
    if (step == 2)
    {
        if (this->selectedGlobalBadge_.isEmpty() &&
            !this->originalGlobalBadge_.isEmpty())
        {
            TwitchGql::deselectGlobalBadge(
                auth,
                [self = QPointer<VanityDialog>(this)] {
                    if (self)
                    {
                        self->originalGlobalBadge_.clear();
                        self->recordSavedChange(QStringLiteral("Twitch badge"));
                        self->runSaveStep(2);
                    }
                },
                [self = QPointer<VanityDialog>(this)](const QString &error) {
                    if (self)
                    {
                        self->failSave(QStringLiteral("Twitch badge"), error);
                    }
                });
            return;
        }
        if (this->selectedChannelBadge_.isEmpty() &&
            !this->originalChannelBadge_.isEmpty())
        {
            if (this->twitchState_.channelId.isEmpty())
            {
                this->failSave(
                    QStringLiteral("Channel badge"),
                    QStringLiteral("Twitch did not return this channel yet"));
                return;
            }
            TwitchGql::deselectChannelBadge(
                this->twitchState_.channelId, auth,
                [self = QPointer<VanityDialog>(this)] {
                    if (self)
                    {
                        self->originalChannelBadge_.clear();
                        self->recordSavedChange(
                            QStringLiteral("Channel badge"));
                        self->runSaveStep(2);
                    }
                },
                [self = QPointer<VanityDialog>(this)](const QString &error) {
                    if (self)
                    {
                        self->failSave(QStringLiteral("Channel badge"), error);
                    }
                });
            return;
        }
        return this->runSaveStep(3);
    }
    if (step == 3)
    {
        const auto selected = this->selectedSevenTVBadgeId();
        if (!this->sevenTVLoaded_ || selected == this->originalSevenTVBadge_)
        {
            return this->runSaveStep(4);
        }
        QJsonObject variables{
            {"id", this->sevenTVUserId_},
            {"badgeId", selected.isEmpty() ? QJsonValue(QJsonValue::Null)
                                           : QJsonValue(selected)}};
        static constexpr auto MUTATION = R"(
mutation SetActiveBadge($id: Id!, $badgeId: Id) {
  users { user(id: $id) { activeBadge(badgeId: $badgeId) { id } } }
}
)";
        QJsonObject payload{{"query", QString::fromUtf8(MUTATION)},
                            {"variables", variables}};
        NetworkRequest(SEVENTV_GQL_ENDPOINT, NetworkRequestType::Post)
            .header("Authorization",
                    QStringLiteral("Bearer ") + this->sevenTVToken_)
            .json(payload)
            .timeout(10000)
            .maximumResponseSize(1024 * 1024)
            .caller(this)
            .onSuccess([this, selected](const NetworkResult &result) {
                const auto root = result.parseJson();
                const auto error = graphQlError(root);
                if (!error.isEmpty())
                {
                    return this->failSave(QStringLiteral("7TV badge"), error);
                }
                if (!root.value("data")
                         .toObject()
                         .value("users")
                         .toObject()
                         .value("user")
                         .isObject())
                {
                    return this->failSave(
                        QStringLiteral("7TV badge"),
                        QStringLiteral("7TV did not confirm the badge change"));
                }
                this->originalSevenTVBadge_ = selected;
                this->sevenTVBadgeTouched_ = false;
                this->recordSavedChange(QStringLiteral("7TV badge"));
                if (auto cached =
                        sevenTVVanityCache().find(this->accountUserId_);
                    cached != sevenTVVanityCache().end())
                {
                    cached->activeBadgeId = selected;
                }
                this->runSaveStep(4);
            })
            .onError([this](const NetworkResult &result) {
                this->failSave(QStringLiteral("7TV badge"),
                               result.formatError());
            })
            .execute();
        return;
    }
    if (step == 4)
    {
        const auto selected = this->selectedSevenTVPaintId();
        if (!this->sevenTVLoaded_ || selected == this->originalSevenTVPaint_)
        {
            return this->runSaveStep(5);
        }
        QJsonObject variables{
            {"id", this->sevenTVUserId_},
            {"paintId", selected.isEmpty() ? QJsonValue(QJsonValue::Null)
                                           : QJsonValue(selected)}};
        static constexpr auto MUTATION = R"(
mutation SetActivePaint($id: Id!, $paintId: Id) {
  users { user(id: $id) { activePaint(paintId: $paintId) { id } } }
}
)";
        QJsonObject payload{{"query", QString::fromUtf8(MUTATION)},
                            {"variables", variables}};
        NetworkRequest(SEVENTV_GQL_ENDPOINT, NetworkRequestType::Post)
            .header("Authorization",
                    QStringLiteral("Bearer ") + this->sevenTVToken_)
            .json(payload)
            .timeout(10000)
            .maximumResponseSize(1024 * 1024)
            .caller(this)
            .onSuccess([this, selected](const NetworkResult &result) {
                const auto root = result.parseJson();
                const auto error = graphQlError(root);
                if (!error.isEmpty())
                {
                    return this->failSave(QStringLiteral("7TV paint"), error);
                }
                if (!root.value("data")
                         .toObject()
                         .value("users")
                         .toObject()
                         .value("user")
                         .isObject())
                {
                    return this->failSave(
                        QStringLiteral("7TV paint"),
                        QStringLiteral("7TV did not confirm the paint change"));
                }
                this->originalSevenTVPaint_ = selected;
                this->sevenTVPaintTouched_ = false;
                this->recordSavedChange(QStringLiteral("7TV paint"));
                if (auto cached =
                        sevenTVVanityCache().find(this->accountUserId_);
                    cached != sevenTVVanityCache().end())
                {
                    cached->activePaintId = selected;
                }
                this->runSaveStep(5);
            })
            .onError([this](const NetworkResult &result) {
                this->failSave(QStringLiteral("7TV paint"),
                               result.formatError());
            })
            .execute();
        return;
    }
    this->saveLayout();
}

void VanityDialog::saveLayout(bool forceOnlineSync)
{
    if (!this->isOriginalAccountCurrent())
    {
        this->stopForAccountChange();
        return;
    }
    if (!this->layoutLoaded_)
    {
        this->finishSave();
        return;
    }

    this->syncWorkingLayoutFromRows();
    this->updateLayoutDirtyState();
    const auto layout = this->editedLayout();
    if (!vanity::detail::shouldSaveProfile(this->originalLayout_, layout,
                                           forceOnlineSync))
    {
        this->finishSave();
        return;
    }

    if (auto *provider = getApp()->getMoltorinoSupporterBadges())
    {
        provider->setLocalVanityLayout(this->accountUserId_, layout);
    }
    this->originalLayout_ = layout;
    this->workingLayout_ = layout;
    this->layoutTouched_ = false;
    this->layoutNeedsSync_ = true;

    QJsonArray order;
    for (const auto &key : layout.order)
    {
        order.append(key);
    }
    QJsonArray hidden;
    QStringList hiddenKeys;
    for (const auto &key : layout.hidden)
    {
        hiddenKeys.push_back(key);
    }
    hiddenKeys.sort(Qt::CaseInsensitive);
    for (const auto &key : hiddenKeys)
    {
        hidden.append(key);
    }
    // I know there's no authentication here yet
    // please be a good person and don't exploit it
    NetworkRequest(VANITY_LAYOUT_ENDPOINT, NetworkRequestType::Put)
        .json(QJsonObject{{"userId", this->accountUserId_},
                          {"revision", this->layoutRevision_},
                          {"layoutSchemaVersion", 1},
                          {"order", order},
                          {"hidden", hidden},
                          {"selectedBadge", layout.moltorinoBadge},
                          {"badgeSelectionExplicit",
                           layout.moltorinoBadgeSelectionExplicit}})
        .timeout(10000)
        .maximumResponseSize(1024 * 1024)
        .caller(this)
        .onSuccess([this, layout](const NetworkResult &result) {
            const auto response = result.parseJson();
            const auto profileObject = response.value("profile").toObject();
            const auto returnedLayout = vanityLayoutFromObject(profileObject);
            const auto revision = profileObject.value("revision").toInt(-1);
            const bool confirmed =
                profileObject.value("layoutSchemaVersion").toInt() == 1 &&
                returnedLayout == layout && revision >= 0;
            if (!confirmed)
            {
                this->layoutNeedsSync_ = true;
                this->layoutHintLabel_->setText(
                    QStringLiteral("Badge order is saved on this device."));
                this->layoutHintLabel_->setToolTip(QStringLiteral(
                    "Online sync did not confirm the same badge order."));
                this->layoutRetryButton_->setText(QStringLiteral("Sync"));
                this->layoutRetryButton_->setToolTip(QStringLiteral(
                    "This badge setup is only confirmed on this device. Sync "
                    "to save it online."));
                this->layoutRetryButton_->show();
                this->finishSave(QStringLiteral(
                    "Saved on this device. Online sync was not confirmed."));
                return;
            }
            if (auto *provider = getApp()->getMoltorinoSupporterBadges())
            {
                provider->setLocalVanityLayout(this->accountUserId_, layout);
            }
            this->layoutRevision_ = revision;
            this->layoutNeedsSync_ = false;
            this->layoutHintLabel_->setText(QStringLiteral(
                "Drag badges into place. Use the eye to hide one."));
            this->layoutHintLabel_->setToolTip({});
            this->layoutRetryButton_->setToolTip({});
            this->layoutRetryButton_->hide();
            this->finishSave();
        })
        .onError([this](const NetworkResult &result) {
            this->layoutNeedsSync_ = true;
            this->layoutHintLabel_->setText(
                QStringLiteral("Badge order is saved on this device."));
            const auto response = result.parseJson();
            const auto error = response.value("error").toObject();
            const auto errorCode = error.value("code").toString();
            if (result.status().value_or(0) == 422 &&
                errorCode == QStringLiteral("badge_not_owned") &&
                !this->selectedMoltorinoBadge_.isEmpty())
            {
                const auto unavailableBadge = this->selectedMoltorinoBadge_;
                std::erase(this->assignedMoltorinoBadgeIds_, unavailableBadge);
                this->moltorinoAssignmentsAuthoritative_ = true;
                this->selectedMoltorinoBadge_.clear();
                this->workingLayout_.moltorinoBadge.clear();
                this->workingLayout_.moltorinoBadgeSelectionExplicit = false;
                this->previewMoltorinoBadge_.clear();
                this->syncEffectiveMoltorinoSelection();
                this->moltorinoBadgeChoicesBuilt_ = false;
                this->refreshMoltorinoBadgeAvailability();
                this->populateCurrentTab();
                this->refreshPreview();

                this->saveLayout(true);
                return;
            }
            const auto currentProfile =
                error.value("currentProfile").toObject();
            const bool changedElsewhere =
                result.status().value_or(0) == 409 && !currentProfile.isEmpty();
            if (changedElsewhere)
            {
                this->layoutRevision_ =
                    std::max(0, currentProfile.value("revision").toInt());
            }
            const auto errorMessage =
                error.value("message").toString(result.formatError());
            this->layoutHintLabel_->setToolTip(errorMessage);
            this->layoutRetryButton_->setText(QStringLiteral("Sync"));
            this->layoutRetryButton_->setToolTip(QStringLiteral(
                "This badge setup is only confirmed on this device. Sync to "
                "save it online."));
            this->layoutRetryButton_->show();
            this->finishSave(
                changedElsewhere
                    ? QStringLiteral("Saved on this device. Sync again to "
                                     "replace the online version.")
                    : QStringLiteral("Saved on this device. Online badge sync "
                                     "is unavailable."));
        })
        .execute();
}

void VanityDialog::finishSave(const QString &message)
{
    this->setBusy(false);
    this->setStatus(message.isEmpty()
                        ? QStringLiteral("Saved. New messages use this look.")
                        : message);
    this->savedChanges_.clear();
    this->releaseAutomaticPin();
}

void VanityDialog::failSave(const QString &provider, const QString &error)
{
    this->setBusy(false);
    const auto saved =
        this->savedChanges_.isEmpty()
            ? QString{}
            : QStringLiteral("Saved %1. ").arg(this->savedChanges_.join(", "));
    this->setStatus(
        saved + QStringLiteral("%1 was not saved: %2").arg(provider, error),
        true);
    this->savedChanges_.clear();
    this->releaseAutomaticPin();
}

void VanityDialog::recordSavedChange(const QString &change)
{
    if (!change.isEmpty() && !this->savedChanges_.contains(change))
    {
        this->savedChanges_.push_back(change);
    }
}

bool VanityDialog::isOriginalAccountCurrent() const
{
    if (this->accountChanged_)
    {
        return false;
    }
    const auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (account->getUserId() != this->accountUserId_ ||
        account->getOAuthToken() != this->accountToken_)
    {
        return false;
    }

    const auto usesSavedToken = [this](const QString &token) {
        return !token.isEmpty() && token != this->accountToken_;
    };
    if (this->activeTwitchAuth_ &&
        usesSavedToken(this->activeTwitchAuth_->oauthToken))
    {
        const auto saved = MoltorinoAuth::resolveSelectedUserToken();
        if (saved.token != this->activeTwitchAuth_->oauthToken)
        {
            return false;
        }
    }
    return true;
}

void VanityDialog::stopForAccountChange()
{
    this->setBusy(false);
    this->setStatus(
        QStringLiteral(
            "Your Twitch account changed. Reopen /vanity before saving."),
        true);
    this->releaseAutomaticPin();
}

QString VanityDialog::storedSevenTVTokenForCurrentAccount() const
{
    const auto owner =
        getSettings()->sevenTVVanityTwitchUserId.getValue().trimmed();
    if (!owner.isEmpty() && owner != this->accountUserId_)
    {
        return {};
    }
    return getSettings()->sevenTVVanityToken.getValue();
}

void VanityDialog::clearSevenTVConnection()
{
    sevenTVVanityCache().remove(this->accountUserId_);
    this->sevenTVToken_.clear();
    const auto owner = getSettings()->sevenTVVanityTwitchUserId.getValue();
    if (!owner.isEmpty() && owner != this->accountUserId_)
    {
        return;
    }
    getSettings()->sevenTVVanityToken = QString{};
    getSettings()->sevenTVVanityTwitchUserId = QString{};
    getSettings()->sevenTVVanityUserId = QString{};
    getSettings()->sevenTVVanityLogin = QString{};
    getSettings()->sevenTVVanityTokenExpiry = QString{};
}

void VanityDialog::releaseAutomaticPin()
{
    if (!this->autoPinnedForSave_)
    {
        return;
    }
    this->autoPinnedForSave_ = false;
    this->togglePinned();
}

void VanityDialog::setBusy(bool busy)
{
    this->saveInFlight_ = busy;
    this->updateSaveButtonState();
    this->tabs_->setEnabled(!busy);
    this->closeButton_->setEnabled(!busy);
    this->pinButton_->setEnabled(!busy);
    this->layoutResetButton_->setEnabled(!busy && this->layoutLoaded_);
    this->saveButton_->setText(busy ? QStringLiteral("Saving...")
                                    : QStringLiteral("Save"));
}

void VanityDialog::updateSaveButtonState()
{
    const bool hasAccount = !this->accountUserId_.isEmpty();
    this->saveButton_->setEnabled(
        !this->sevenTVRequestInFlight_ &&
        vanity::detail::canSaveIndependentVanityChanges(
            hasAccount, this->layoutLoaded_, this->layoutRequestInFlight_,
            this->saveInFlight_));
}

void VanityDialog::setStatus(const QString &text, bool error)
{
    this->statusLabel_->setText(text);
    this->statusLabel_->setToolTip(text);
    this->statusLabel_->setVisible(!text.isEmpty());
    this->statusLabel_->setStyleSheet(error ? QStringLiteral("color: #ff6b6b;")
                                            : QString{});
}

QString VanityDialog::selectedSevenTVBadgeId() const
{
    return this->selectedSevenTVBadge_;
}

QString VanityDialog::selectedSevenTVPaintId() const
{
    return this->selectedSevenTVPaint_;
}

const GqlVanityBadge *VanityDialog::findGlobalBadge(const QString &key) const
{
    const auto setId = key.startsWith(QStringLiteral("t:")) ? key.mid(2) : key;
    const auto it = std::ranges::find_if(
        this->twitchState_.globalBadges, [&](const auto &badge) {
            return badge.setId.compare(setId, Qt::CaseInsensitive) == 0;
        });
    return it == this->twitchState_.globalBadges.end() ? nullptr : &*it;
}

const GqlVanityBadge *VanityDialog::findChannelBadge(const QString &key) const
{
    const auto setId = key.startsWith(QStringLiteral("t:")) ? key.mid(2) : key;
    const auto it = std::ranges::find_if(
        this->twitchState_.channelBadges, [&](const auto &badge) {
            return badge.setId.compare(setId, Qt::CaseInsensitive) == 0;
        });
    if (it != this->twitchState_.channelBadges.end())
    {
        return &*it;
    }

    return this->findGlobalBadge(setId);
}

const TwitchEventBadge *VanityDialog::findTwitchEventBadge(
    const QString &key) const
{
    const auto id = key.startsWith(QStringLiteral("t:")) ? key.mid(2) : key;
    const auto it = std::ranges::find_if(
        this->twitchEventBadges_, [&](const auto &badge) {
            return badge.id.compare(id, Qt::CaseInsensitive) == 0;
        });
    return it == this->twitchEventBadges_.end() ? nullptr : &*it;
}

const SevenTVVanityCosmetic *VanityDialog::findSevenTVBadge(
    const QString &id) const
{
    const auto it =
        std::ranges::find(this->sevenTVBadges_, id, &SevenTVVanityCosmetic::id);
    return it == this->sevenTVBadges_.end() ? nullptr : &*it;
}

MoltorinoVanityLayout VanityDialog::editedLayout() const
{
    auto layout = this->workingLayout_;
    layout.moltorinoBadge = layout.moltorinoBadgeSelectionExplicit
                                ? this->selectedMoltorinoBadge_
                                : QString{};
    return vanity::detail::normalizeLayout(std::move(layout));
}

QString VanityDialog::layoutDisplayName(const QString &key) const
{
    if (key == QStringLiteral("ta"))
    {
        return QStringLiteral("Twitch role badge");
    }
    if (key == QStringLiteral("ts"))
    {
        return QStringLiteral("Twitch subscriber badge");
    }
    if (key == QStringLiteral("tv"))
    {
        return QStringLiteral("Twitch selected badge");
    }
    if (key == QStringLiteral("tp"))
    {
        return QStringLiteral("Twitch prediction badge");
    }
    if (key.startsWith(QStringLiteral("t:")))
    {
        if (const auto *badge = this->findGlobalBadge(key))
        {
            return QStringLiteral("Twitch: ") + badge->title;
        }
        if (const auto *badge = this->findChannelBadge(key))
        {
            return QStringLiteral("Twitch: ") + badge->title;
        }
        auto name = key.mid(2);
        name.replace('_', ' ');
        name.replace('-', ' ');
        if (!name.isEmpty())
        {
            name[0] = name[0].toUpper();
        }
        return QStringLiteral("Twitch: ") + name;
    }
    if (key == QStringLiteral("c"))
    {
        return QStringLiteral("Chatterino");
    }
    if (key == QStringLiteral("ff"))
    {
        return QStringLiteral("FrankerFaceZ");
    }
    if (key == QStringLiteral("fa"))
    {
        return QStringLiteral("FFZ:AP");
    }
    if (key == QStringLiteral("bt"))
    {
        return QStringLiteral("BetterTTV");
    }
    if (key == QStringLiteral("m"))
    {
        return QStringLiteral("Moltorino");
    }
    if (key == QStringLiteral("bl"))
    {
        return QStringLiteral("Bluzyrino");
    }
    if (key == QStringLiteral("7"))
    {
        return QStringLiteral("7TV");
    }
    if (key == QStringLiteral("hc"))
    {
        return QStringLiteral("Homies custom");
    }
    if (key == QStringLiteral("hs"))
    {
        return QStringLiteral("Homies supporter");
    }
    return key;
}

QString VanityDialog::sevenTVTokenExpiryText() const
{
    const auto expiry = QDateTime::fromString(
        getSettings()->sevenTVVanityTokenExpiry.getValue(), Qt::ISODate);
    if (!expiry.isValid())
    {
        return {};
    }
    const auto days = QDateTime::currentDateTimeUtc().daysTo(expiry);
    if (days < 0)
    {
        return QStringLiteral(" \u00b7 expired");
    }
    if (days == 0)
    {
        return QStringLiteral(" \u00b7 expires today");
    }
    return QStringLiteral(" \u00b7 expires in %1 day%2")
        .arg(days)
        .arg(days == 1 ? QString{} : QStringLiteral("s"));
}

float VanityDialog::scale() const
{
    return std::max(0.75F, DraggablePopup::scale());
}

void VanityDialog::themeChangedEvent()
{
    DraggablePopup::themeChangedEvent();
    this->refreshStyle();
    if (this->preview_)
    {
        this->preview_->update();
    }
}

void VanityDialog::scaleChangedEvent(float scale)
{
    DraggablePopup::scaleChangedEvent(scale);
    this->refreshStyle();
    this->scheduleResponsiveLayoutRefresh();
}

void VanityDialog::resizeEvent(QResizeEvent *event)
{
    DraggablePopup::resizeEvent(event);
    this->scheduleResponsiveLayoutRefresh();
}

void VanityDialog::showEvent(QShowEvent *event)
{
    DraggablePopup::showEvent(event);
    this->updateVisualRefreshTimer();
}

void VanityDialog::hideEvent(QHideEvent *event)
{
    if (this->visualRefreshTimer_ != nullptr)
    {
        this->visualRefreshTimer_->stop();
    }
    DraggablePopup::hideEvent(event);
}

void VanityDialog::scheduleResponsiveLayoutRefresh()
{
    if (this->responsiveLayoutRefreshPending_ || this->tabs_ == nullptr)
    {
        return;
    }

    this->responsiveLayoutRefreshPending_ = true;
    QTimer::singleShot(0, this, [this] {
        this->responsiveLayoutRefreshPending_ = false;
        if (this->mainLayout_ == nullptr || this->tabs_ == nullptr)
        {
            return;
        }

        this->updateMinimumWidth();
        this->mainLayout_->invalidate();
        this->mainLayout_->activate();
        this->tabs_->updateGeometry();
        this->tabs_->update();
        this->tabs_->tabBar()->updateGeometry();
        this->tabs_->tabBar()->update();
        if (auto *currentPage = this->tabs_->currentWidget())
        {
            if (auto *pageLayout = currentPage->layout())
            {
                pageLayout->invalidate();
                pageLayout->activate();
            }
            currentPage->updateGeometry();
        }
        this->updateVisualRefreshTimer();
    });
}

}
