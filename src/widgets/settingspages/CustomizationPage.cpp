#include "widgets/settingspages/CustomizationPage.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/Version.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "providers/twitch/TwitchBadges.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/ThemeVideo.hpp"
#include "singletons/ThemeVideoDecoder.hpp"
#include "singletons/ThemeVideoImport.hpp"
#include "singletons/ThemeWallpaper.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Clipboard.hpp"
#include "widgets/dialogs/ColorPickerDialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QImageReader>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLinearGradient>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSaveFile>
#include <QSet>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTabWidget>
#include <QtConcurrent/QtConcurrentRun>
#include <QTextLayout>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <vector>

using namespace Qt::StringLiterals;

namespace chatterino {
namespace {

constexpr auto APPEARANCE_FORMAT = "moltorino-appearance";
constexpr int APPEARANCE_VERSION = 1;

struct StoredWallpaper {
    ThemeWallpaperData data;
    QString source;
};

StoredWallpaper storePreparedWallpaper(ThemeWallpaperData data,
                                       const QString &directory,
                                       std::stop_token cancellation)
{
    QString source;
    if (cancellation.stop_requested())
    {
        data.error = u"Background preparation cancelled."_s;
    }
    else
    {
        source = storeThemeWallpaper(data, directory, &data.error);
    }
    return {std::move(data), std::move(source)};
}

std::optional<QString> promptForThemeName(QWidget *parent, const QString &title,
                                          const QString &label,
                                          const QString &currentName)
{
    QInputDialog dialog(parent);
    dialog.setWindowTitle(title);
    dialog.setLabelText(label);
    dialog.setTextValue(currentName);
    dialog.setTextEchoMode(QLineEdit::Normal);
    dialog.setMinimumWidth(360);
    if (auto *input = dialog.findChild<QLineEdit *>())
    {
        input->setMinimumWidth(320);
        input->selectAll();
    }
    dialog.resize(360, dialog.sizeHint().height());

    if (dialog.exec() != QDialog::Accepted)
    {
        return std::nullopt;
    }
    return dialog.textValue();
}

QString cleanCode(QString input)
{
    input = input.trimmed();
    const QRegularExpression url(
        uR"((?:https?://)?h\.moltorino\.com/([A-Za-z0-9]+)(?:/raw)?)"_s,
        QRegularExpression::CaseInsensitiveOption);
    if (const auto match = url.match(input); match.hasMatch())
    {
        input = match.captured(1);
    }
    return QRegularExpression(u"^[A-Za-z0-9]{4,20}$"_s).match(input).hasMatch()
               ? input
               : QString();
}

QString safeFileBase(QString name)
{
    name = name.trimmed().toLower();
    name.replace(QRegularExpression(u"[^a-z0-9]+"_s), u"-"_s);
    name.remove(QRegularExpression(u"(^-+|-+$)"_s));
    return name.isEmpty() ? u"custom-theme"_s : name.left(48);
}

QString resolvedLocalPath(const QString &path)
{
    const QFileInfo info(path);
    auto resolved = info.canonicalFilePath();
    if (resolved.isEmpty())
    {
        resolved = info.absoluteFilePath();
    }
    return QDir::fromNativeSeparators(QDir::cleanPath(resolved));
}

QString normalizedLocalPath(const QString &path)
{
    auto normalized = resolvedLocalPath(path);
#ifdef Q_OS_WIN
    normalized = normalized.toCaseFolded();
#endif
    return normalized;
}

QString importedThemeName(const QString &path)
{
    auto name = QFileInfo(path).completeBaseName();
    if (name.endsWith(u".moltheme"_s, Qt::CaseInsensitive))
    {
        name.chop(9);
    }
    name.replace(QRegularExpression(u"[_-]+"_s), u" "_s);
    name = name.simplified().left(80);
    if (name.isEmpty())
    {
        return u"Imported theme"_s;
    }
    name[0] = name[0].toUpper();
    return name;
}

QString wallpaperDisplayName(const QString &source)
{
    if (source.isEmpty())
    {
        return {};
    }
    auto name = QFileInfo(source).completeBaseName();
    name.replace(u'-', u' ');
    auto parts = name.split(u' ', Qt::SkipEmptyParts);
    for (auto &part : parts)
    {
        part[0] = part[0].toUpper();
    }
    const auto label = parts.join(u' ');
    return source.startsWith(u":/"_s)
               ? QStringLiteral("Bundled wallpaper: %1").arg(label)
               : QFileInfo(source).fileName();
}

class PositionSlider final : public QSlider
{
public:
    explicit PositionSlider(QWidget *parent = nullptr)
        : QSlider(Qt::Horizontal, parent)
    {
        this->setTracking(true);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton)
        {
            QSlider::mousePressEvent(event);
            return;
        }
        this->setSliderDown(true);
        this->setFromPosition(event->position().x());
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!this->isSliderDown())
        {
            QSlider::mouseMoveEvent(event);
            return;
        }
        this->setFromPosition(event->position().x());
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && this->isSliderDown())
        {
            this->setFromPosition(event->position().x());
            this->setSliderDown(false);
            event->accept();
            return;
        }
        QSlider::mouseReleaseEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
        {
            this->setFromPosition(event->position().x());
            event->accept();
            return;
        }
        QSlider::mouseDoubleClickEvent(event);
    }

private:
    void setFromPosition(qreal x)
    {
        QStyleOptionSlider option;
        this->initStyleOption(&option);
        const auto groove = this->style()->subControlRect(
            QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
        const auto handle = this->style()->subControlRect(
            QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
        const int span = std::max(1, groove.width() - handle.width());
        const int position =
            std::clamp(qRound(x) - groove.left() - handle.width() / 2, 0, span);
        this->setValue(
            QStyle::sliderValueFromPosition(this->minimum(), this->maximum(),
                                            position, span, option.upsideDown));
    }
};

QWidget *makeSlider(int minimum, int maximum, QSlider **slider,
                    const QString &suffix = {})
{
    auto *container = new QWidget;
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    *slider = new PositionSlider(container);
    (*slider)->setRange(minimum, maximum);
    auto *value = new QLabel(container);
    value->setMinimumWidth(42);
    value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    value->setText(QString::number(minimum) + suffix);
    QObject::connect(*slider, &QSlider::valueChanged, value,
                     [value, suffix](int amount) {
                         value->setText(QString::number(amount) + suffix);
                     });
    layout->addWidget(*slider, 1);
    layout->addWidget(value);
    return container;
}

QWidget *makeOpacityAdjustmentSlider(QSlider **slider)
{
    auto *container = new QWidget;
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto *less = new QLabel(u"Fainter"_s, container);
    auto *more = new QLabel(u"Stronger"_s, container);
    auto *value = new QLabel(u"Original"_s, container);
    value->setMinimumWidth(58);
    value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    *slider = new PositionSlider(container);
    (*slider)->setRange(-100, 100);
    (*slider)->setSingleStep(1);
    (*slider)->setPageStep(10);
    (*slider)->setValue(0);
    QObject::connect(
        *slider, &QSlider::valueChanged, value, [value](int amount) {
            value->setText(amount == 0 ? u"Original"_s
                                       : QStringLiteral("%1%2%").arg(
                                             amount > 0 ? u"+"_s : QString(),
                                             QString::number(amount)));
        });

    layout->addWidget(less);
    layout->addWidget(*slider, 1);
    layout->addWidget(more);
    layout->addWidget(value);
    return container;
}

QStringList cleanFontFamilies()
{
    const auto installed = QFontDatabase::families();
    QSet<QString> seen;
    QStringList result;
    const QRegularExpression opticalSize(
        uR"(\s+\d+\s*pt(?:\s+.*)?$)"_s,
        QRegularExpression::CaseInsensitiveOption);

    for (auto family : installed)
    {
        family.remove(opticalSize);
        family = family.trimmed();
        const auto key = family.toCaseFolded();
        if (!family.isEmpty() && !seen.contains(key))
        {
            seen.insert(key);
            result.append(family);
        }
    }
    result.sort(Qt::CaseInsensitive);
    return result;
}

int canonicalFontWeight(const QString &family)
{
    struct NamedWeight {
        QStringView suffix;
        int weight;
    };
    static constexpr NamedWeight namedWeights[] = {
        {u"Thin", QFont::Thin},           {u"ExtraLight", QFont::ExtraLight},
        {u"Light", QFont::Light},         {u"Medium", QFont::Medium},
        {u"SemiBold", QFont::DemiBold},   {u"DemiBold", QFont::DemiBold},
        {u"ExtraBold", QFont::ExtraBold}, {u"Bold", QFont::Bold},
        {u"Black", QFont::Black},
    };
    for (const auto &named : namedWeights)
    {
        if (family.endsWith(named.suffix, Qt::CaseInsensitive))
        {
            return named.weight;
        }
    }

    const auto styles = QFontDatabase::styles(family);
    int closestWeight = -1;
    int closestDistance = std::numeric_limits<int>::max();
    for (const auto &style : styles)
    {
        const auto weight = QFontDatabase::weight(family, style);
        const auto distance = std::abs(weight - int(QFont::Normal));
        if (distance < closestDistance)
        {
            closestDistance = distance;
            closestWeight = weight;
        }
    }
    return closestWeight;
}

QColor nestedColor(const QJsonObject &root, std::initializer_list<QString> path,
                   const QColor &fallback)
{
    QJsonValue value(root);
    for (const auto &part : path)
    {
        value = value.toObject().value(part);
    }
    const QColor color(value.toString());
    return color.isValid() ? color : fallback;
}

ThemeCustomizationProfile profileFromTheme(const ThemeDescriptor &descriptor,
                                           const QJsonObject &json)
{
    if (auto profile = customizationProfileFromTheme(json))
    {
        return *profile;
    }
    ThemeCustomizationProfile profile;
    profile.name = descriptor.name;
    profile.useThemeFonts = false;
    profile.useThemeFontSizes = false;
    profile.baseTheme = descriptor.key;
    profile.background = nestedColor(
        json, {u"colors"_s, u"window"_s, u"background"_s}, profile.background);
    profile.chatBackground =
        nestedColor(json, {u"colors"_s, u"splits"_s, u"background"_s},
                    profile.chatBackground);
    profile.surface = nestedColor(
        json,
        {u"colors"_s, u"tabs"_s, u"regular"_s, u"backgrounds"_s, u"regular"_s},
        profile.surface);
    profile.raisedSurface = nestedColor(
        json,
        {u"colors"_s, u"tabs"_s, u"selected"_s, u"backgrounds"_s, u"regular"_s},
        profile.raisedSurface);
    profile.text =
        nestedColor(json, {u"colors"_s, u"window"_s, u"text"_s}, profile.text);
    profile.chatText = nestedColor(
        json, {u"colors"_s, u"messages"_s, u"textColors"_s, u"regular"_s},
        profile.text);
    profile.separateChatText = profile.chatText != profile.text;
    const auto messageSystem = nestedColor(
        json, {u"colors"_s, u"messages"_s, u"textColors"_s, u"system"_s},
        profile.mutedText);
    profile.mutedText = nestedColor(
        json, {u"colors"_s, u"tabs"_s, u"regular"_s, u"text"_s}, messageSystem);
    profile.systemText = nestedColor(
        json, {u"colors"_s, u"messages"_s, u"textColors"_s, u"system"_s},
        messageSystem);
    profile.timestampText = nestedColor(
        json, {u"colors"_s, u"messages"_s, u"textColors"_s, u"timestamp"_s},
        profile.systemText);
    profile.accent =
        nestedColor(json, {u"colors"_s, u"accent"_s}, profile.accent);
    profile.foundation = ThemeFoundation::ChatterinoClassic;
    profile.useThemeMessageRows = false;
    profile.cornerStyle = ThemeCornerStyle::Classic;
    profile.tabCornerRadius = 0;
    profile.chatCornerRadius = 0;
    profile.roundChat = false;
    return profile;
}

QJsonObject appearanceEnvelope(const QJsonObject &theme)
{
    return {
        {u"format"_s, u"moltorino-appearance"_s},
        {u"version"_s, APPEARANCE_VERSION},
        {u"theme"_s, theme},
    };
}

struct ThemeImport {
    ThemeCustomizationProfile profile;
    bool adapted = false;
    bool localWallpaperRemoved = false;
};

std::optional<ThemeImport> themeFromImport(const QByteArray &bytes,
                                           const QString &fallbackName,
                                           QString *error)
{
    const auto document = QJsonDocument::fromJson(bytes);
    if (!document.isObject())
    {
        *error = u"This is not a supported theme file."_s;
        return std::nullopt;
    }
    auto root = document.object();

    if (!root.contains(u"format"_s) && root.value(u"colors"_s).isObject() &&
        customizationProfileFromTheme(root))
    {
        root = appearanceEnvelope(root);
    }
    const auto format = root.value(u"format"_s).toString();
    if (format == QString::fromLatin1(APPEARANCE_FORMAT))
    {
        if (root.value(u"version"_s).toInt() != APPEARANCE_VERSION)
        {
            *error = u"This theme file uses an unsupported version."_s;
            return std::nullopt;
        }
        if (!root.value(u"theme"_s).isObject())
        {
            *error = u"This file does not contain a valid Moltorino theme."_s;
            return std::nullopt;
        }
        bool localWallpaperRemoved = false;
        const auto theme = makeShareableCustomizedTheme(
            root.value(u"theme"_s).toObject(), &localWallpaperRemoved);
        const auto profile = customizationProfileFromTheme(theme);
        if (!profile)
        {
            *error = u"This file does not contain a valid Moltorino theme."_s;
            return std::nullopt;
        }
        return ThemeImport{*profile, false, localWallpaperRemoved};
    }

    if (format == u"bluzyrino-theme"_s)
    {
        auto name = root.value(u"name"_s).toString().trimmed();
        if (name.isEmpty())
        {
            name = fallbackName;
        }
        const auto profile = customizationProfileFromBluzyrinoTheme(root, name);
        if (!profile)
        {
            *error =
                root.value(u"version"_s).toInt() > 2
                    ? u"This Bluzyrino theme needs a newer version of Moltorino."_s
                    : u"This Bluzyrino theme is missing a valid color palette."_s;
            return std::nullopt;
        }
        return ThemeImport{*profile, true, false};
    }

    *error = u"This is not a supported theme file."_s;
    return std::nullopt;
}

QFont previewTimestampFont(QFont chatFont)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    chatFont.setFeature(QFont::Tag("tnum"), 1);
#endif
    return chatFont;
}

template <typename PaintMask>
QImage renderPreviewShadow(const QWidget &widget, const QPainterPath &clip,
                           const QColor &color, int blurRadius,
                           PaintMask &&paintMask)
{
    const qreal dpr = widget.devicePixelRatioF();
    QImage image(
        QSize(qCeil(widget.width() * dpr), qCeil(widget.height() * dpr)),
        QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);

    QPainter shadowPainter(&image);
    shadowPainter.setRenderHint(QPainter::Antialiasing, true);
    shadowPainter.setRenderHint(QPainter::TextAntialiasing, true);
    shadowPainter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    shadowPainter.setClipPath(clip);
    paintMask(shadowPainter);
    shadowPainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    shadowPainter.fillRect(widget.rect(), color);
    shadowPainter.end();

    return blurThemeShadow(std::move(image), qRound(blurRadius * dpr));
}

}

class WallpaperFocusWidget final : public QWidget
{
public:
    explicit WallpaperFocusWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        this->setFixedHeight(94);
        this->setMinimumWidth(180);
        this->setCursor(Qt::CrossCursor);
        this->setToolTip(u"Click or drag to set the focus point."_s);
    }

    void setProfile(const ThemeCustomizationProfile &profile)
    {
        const QFileInfo info(profile.wallpaperSource);
        const auto key =
            QStringLiteral("%1:%2")
                .arg(profile.wallpaperSource)
                .arg(info.exists() ? info.lastModified().toMSecsSinceEpoch()
                                   : 0);
        this->profile_ = profile;
        if (key != this->sourceKey_)
        {
            this->sourceKey_ = key;
            this->image_ = this->isVisible() ? loadThemeWallpaper(
                                                   profile.wallpaperSource, 256)
                                             : QImage();
        }
        this->setEnabled(profile.hasWallpaper() &&
                         profile.wallpaperMode != ThemeWallpaperMode::Tile &&
                         (profile.wallpaperMode == ThemeWallpaperMode::Fill ||
                          profile.wallpaperZoom > 100));
        this->update();
    }

    std::function<void(int, int)> focusChanged;

protected:
    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        this->image_ = loadThemeWallpaper(this->profile_.wallpaperSource, 256);
    }

    void hideEvent(QHideEvent *event) override
    {
        this->image_ = {};
        QWidget::hideEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(this->rect(), this->profile_.chatBackground);
        const QRectF bounds = this->rect().adjusted(1, 1, -1, -1);
        if (this->image_.isNull())
        {
            painter.setPen(this->profile_.mutedText);
            painter.drawText(bounds, Qt::AlignCenter,
                             u"Choose a wallpaper to set its focus"_s);
            painter.setPen(this->profile_.raisedSurface);
            painter.drawRect(bounds);
            this->imageRect_ = {};
            return;
        }

        const auto size = QSizeF(this->image_.size())
                              .scaled(bounds.size(), Qt::KeepAspectRatio);
        this->imageRect_ = QRectF(QPointF(), size);
        this->imageRect_.moveCenter(bounds.center());
        painter.drawImage(this->imageRect_, this->image_);
        painter.fillRect(this->imageRect_, QColor(0, 0, 0, 42));

        const QPointF focus(this->imageRect_.left() +
                                this->imageRect_.width() *
                                    this->profile_.wallpaperFocalX / 100.0,
                            this->imageRect_.top() +
                                this->imageRect_.height() *
                                    this->profile_.wallpaperFocalY / 100.0);
        painter.setPen(QPen(Qt::black, 4));
        painter.drawEllipse(focus, 7, 7);
        painter.setPen(QPen(this->profile_.accent, 2));
        painter.drawEllipse(focus, 7, 7);
        painter.drawLine(focus + QPointF(-11, 0), focus + QPointF(-5, 0));
        painter.drawLine(focus + QPointF(5, 0), focus + QPointF(11, 0));
        painter.drawLine(focus + QPointF(0, -11), focus + QPointF(0, -5));
        painter.drawLine(focus + QPointF(0, 5), focus + QPointF(0, 11));
        painter.setPen(this->profile_.raisedSurface);
        painter.drawRect(bounds);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
        {
            this->setFocusFrom(event->position());
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (event->buttons().testFlag(Qt::LeftButton))
        {
            this->setFocusFrom(event->position());
        }
    }

private:
    void setFocusFrom(const QPointF &position)
    {
        if (!this->isEnabled() || this->imageRect_.isEmpty())
        {
            return;
        }
        const int x =
            qRound(std::clamp((position.x() - this->imageRect_.left()) /
                                  this->imageRect_.width(),
                              0.0, 1.0) *
                   100);
        const int y =
            qRound(std::clamp((position.y() - this->imageRect_.top()) /
                                  this->imageRect_.height(),
                              0.0, 1.0) *
                   100);
        if (this->focusChanged)
        {
            this->focusChanged(x, y);
        }
    }

    ThemeCustomizationProfile profile_;
    QImage image_;
    QRectF imageRect_;
    QString sourceKey_;
};

class ShadowPositionWidget final : public QWidget
{
public:
    explicit ShadowPositionWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        this->setFixedSize(118, 76);
        this->setCursor(Qt::CrossCursor);
        this->setToolTip(u"Drag to position the shadow. Center to reset."_s);
    }

    void setOffset(QPoint offset)
    {
        offset.setX(std::clamp(offset.x(), -8, 8));
        offset.setY(std::clamp(offset.y(), -8, 8));
        if (this->offset_ == offset)
        {
            return;
        }
        this->offset_ = offset;
        this->update();
    }

    QPoint offset() const
    {
        return this->offset_;
    }

    void setShadowColor(QColor color)
    {
        if (!color.isValid() || this->shadowColor_ == color)
        {
            return;
        }
        this->shadowColor_ = color;
        this->update();
    }

    std::function<void(QPoint)> positionChanged;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const auto bounds = QRectF(this->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.fillRect(bounds, this->palette().color(QPalette::Base));
        painter.setPen(this->palette().color(QPalette::Mid));
        painter.drawRect(bounds);
        painter.setPen(
            QPen(this->palette().color(QPalette::Mid), 1, Qt::DashLine));
        painter.drawLine(QPointF(bounds.center().x(), bounds.top() + 7),
                         QPointF(bounds.center().x(), bounds.bottom() - 7));
        painter.drawLine(QPointF(bounds.left() + 9, bounds.center().y()),
                         QPointF(bounds.right() - 9, bounds.center().y()));

        auto sample = makeResolvedFont(this->font(), QFont::Bold);
        sample.setPointSizeF(sample.pointSizeF() + 2);
        painter.setFont(sample);
        const QPointF center = bounds.center() + QPointF(-10, 5);
        const QPointF shadowOffset(this->offset_.x() * 1.8,
                                   this->offset_.y() * 1.8);
        auto shadow = this->shadowColor_;
        shadow.setAlpha(160);
        painter.setPen(shadow);
        painter.drawText(center + shadowOffset, u"Aa"_s);
        painter.setPen(this->palette().color(QPalette::Text));
        painter.drawText(center, u"Aa"_s);

        painter.setPen(Qt::NoPen);
        painter.setBrush(this->palette().color(QPalette::Highlight));
        painter.drawEllipse(this->pointForOffset(this->offset_), 3.5, 3.5);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
        {
            this->setFromPoint(event->position());
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (event->buttons().testFlag(Qt::LeftButton))
        {
            this->setFromPoint(event->position());
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

private:
    QRectF controlRect() const
    {
        return QRectF(this->rect()).adjusted(10, 9, -10, -9);
    }

    QPointF pointForOffset(QPoint offset) const
    {
        const auto area = this->controlRect();
        return {area.center().x() + area.width() * offset.x() / 16.0,
                area.center().y() + area.height() * offset.y() / 16.0};
    }

    void setFromPoint(QPointF point)
    {
        const auto area = this->controlRect();
        const QPoint next{
            std::clamp(
                qRound((point.x() - area.center().x()) * 16.0 / area.width()),
                -8, 8),
            std::clamp(
                qRound((point.y() - area.center().y()) * 16.0 / area.height()),
                -8, 8),
        };
        if (next == this->offset_)
        {
            return;
        }
        this->offset_ = next;
        this->update();
        if (this->positionChanged)
        {
            this->positionChanged(next);
        }
    }

    QPoint offset_{1, 1};
    QColor shadowColor_{Qt::black};
};

class ThemePreviewWidget final : public QWidget
{
public:
    QImage wallpaperFrame() const
    {
        return this->video_
                   ? this->video_->currentFrame()
                   : loadThemeWallpaper(this->profile_.wallpaperSource);
    }

    ~ThemePreviewWidget() override
    {
        this->releaseVideo();
    }

    explicit ThemePreviewWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        this->setMinimumSize(290, 420);
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        this->setToolTip(u"Live theme preview."_s);
        this->blurTimer_.setSingleShot(true);
        this->blurTimer_.setInterval(90);
        QObject::connect(&this->blurTimer_, &QTimer::timeout, this, [this] {
            this->rebuildWallpaper();
        });

        this->badgeRefreshTimer_.setInterval(120);
        QObject::connect(
            &this->badgeRefreshTimer_, &QTimer::timeout, this, [this] {
                ++this->badgeRefreshAttempts_;
                if (!this->refreshBadgeAssets() ||
                    this->badgeRefreshAttempts_ >= MaxBadgeRefreshAttempts)
                {
                    this->badgeRefreshTimer_.stop();
                }
            });
        if (this->refreshBadgeAssets())
        {
            this->badgeRefreshTimer_.start();
        }
    }

    void setProfile(ThemeCustomizationProfile profile)
    {
        const QFileInfo info(profile.wallpaperSource);
        const auto sourceKey =
            QStringLiteral("%1:%2")
                .arg(profile.wallpaperSource)
                .arg(info.exists() ? info.lastModified().toMSecsSinceEpoch()
                                   : 0);
        profile.clearDisabledFontOverrides();
        this->profile_ = std::move(profile);
        this->generatedTheme_ = buildCustomizedTheme(this->profile_);
        this->shadowPreview_ = {};
        if (this->wallpaperSourceKey_ != sourceKey)
        {
            this->wallpaperSourceKey_ = sourceKey;
            this->wallpaperRenderKey_.clear();
            this->sourceWallpaper_ = {};
            this->wallpaper_ = {};
            if (this->isVisible() && this->profile_.hasWallpaper() &&
                !isThemeVideo(this->profile_.wallpaperSource))
            {
                const auto image =
                    loadThemeWallpaper(this->profile_.wallpaperSource, 768);
                if (!image.isNull())
                {
                    this->sourceWallpaper_ = image;
                    const auto size =
                        QImageReader(this->profile_.wallpaperSource).size();
                    const int fullDimension =
                        std::min(1536, std::max(size.width(), size.height()));
                    this->wallpaperScale_ =
                        fullDimension > 0
                            ? qreal(std::max(image.width(), image.height())) /
                                  fullDimension
                            : 1.0;
                }
            }
        }
        if (isThemeVideo(this->profile_.wallpaperSource) &&
            this->profile_.wallpaperOpacity > 0 && this->isVisible())
        {
            if (!this->video_ ||
                this->videoSource_ != this->profile_.wallpaperSource)
            {
                this->releaseVideo();
                this->videoSource_ = this->profile_.wallpaperSource;
                this->video_ = ThemeVideo::acquire(this->videoSource_, this);
                this->videoConnection_ =
                    QObject::connect(this->video_.get(),
                                     &ThemeVideo::frameChanged, this, [this] {
                                         this->update();
                                     });
            }
        }
        else
        {
            this->releaseVideo();
        }
        const auto renderKey = QStringLiteral("%1:%2").arg(sourceKey).arg(
            this->profile_.wallpaperBlur);
        if (this->wallpaperRenderKey_ != renderKey)
        {
            this->wallpaperRenderKey_ = renderKey;
            this->blurTimer_.start();
        }
        if (this->refreshBadgeAssets() &&
            !this->badgeRefreshTimer_.isActive() &&
            this->badgeRefreshAttempts_ < MaxBadgeRefreshAttempts)
        {
            this->badgeRefreshTimer_.start();
        }
        this->update();
    }

protected:
    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);

        this->wallpaperSourceKey_.clear();
        this->setProfile(this->profile_);
        if (this->refreshBadgeAssets() && !this->badgeRefreshTimer_.isActive())
        {
            this->badgeRefreshAttempts_ = 0;
            this->badgeRefreshTimer_.start();
        }
    }

    void resizeEvent(QResizeEvent *event) override
    {
        this->shadowPreview_ = {};
        QWidget::resizeEvent(event);
    }

    void hideEvent(QHideEvent *event) override
    {
        this->blurTimer_.stop();
        this->releaseVideo();
        this->badgeRefreshTimer_.stop();
        this->sourceWallpaper_ = {};
        this->wallpaper_ = {};
        this->shadowPreview_ = {};
        this->wallpaperSourceKey_.clear();
        this->wallpaperRenderKey_.clear();
        QWidget::hideEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF bounds = this->rect().adjusted(1, 1, -1, -1);
        const auto color = [this](std::initializer_list<QString> path,
                                  const QColor &fallback) {
            return nestedColor(this->generatedTheme_, path, fallback);
        };
        const auto windowBackground =
            color({u"colors"_s, u"window"_s, u"background"_s},
                  this->profile_.background);
        const auto regularTab = color({u"colors"_s, u"tabs"_s, u"regular"_s,
                                       u"backgrounds"_s, u"regular"_s},
                                      this->profile_.surface);
        const auto selectedTab = color({u"colors"_s, u"tabs"_s, u"selected"_s,
                                        u"backgrounds"_s, u"regular"_s},
                                       this->profile_.raisedSurface);
        const auto selectedTabLine = color(
            {u"colors"_s, u"tabs"_s, u"selected"_s, u"line"_s, u"regular"_s},
            this->profile_.accent);
        const auto regularTabLine = color(
            {u"colors"_s, u"tabs"_s, u"regular"_s, u"line"_s, u"regular"_s},
            this->profile_.raisedSurface);
        const auto tabDivider =
            color({u"colors"_s, u"tabs"_s, u"dividerLine"_s},
                  this->profile_.raisedSurface);
        const auto headerBackground =
            color({u"colors"_s, u"splits"_s, u"header"_s, u"background"_s},
                  this->profile_.surface);
        const auto focusedHeaderBackground = color(
            {u"colors"_s, u"splits"_s, u"header"_s, u"focusedBackground"_s},
            headerBackground);
        const auto headerBorder = color(
            {u"colors"_s, u"splits"_s, u"header"_s, u"border"_s}, tabDivider);
        const auto focusedHeaderBorder =
            color({u"colors"_s, u"splits"_s, u"header"_s, u"focusedBorder"_s},
                  headerBorder);
        const auto inputBackground =
            color({u"colors"_s, u"splits"_s, u"input"_s, u"background"_s},
                  regularTab);
        const auto messageRegular =
            color({u"colors"_s, u"messages"_s, u"backgrounds"_s, u"regular"_s},
                  this->profile_.chatBackground);
        const auto messageAlternate = color(
            {u"colors"_s, u"messages"_s, u"backgrounds"_s, u"alternate"_s},
            this->profile_.surface);
        const auto interfaceText =
            color({u"colors"_s, u"window"_s, u"text"_s}, this->profile_.text);
        const auto chatText =
            color({u"colors"_s, u"messages"_s, u"textColors"_s, u"regular"_s},
                  this->profile_.effectiveChatText());
        const auto mutedText = this->profile_.mutedText;
        const auto systemText = color(
            {u"colors"_s, u"messages"_s, u"textColors"_s, u"system"_s},
            this->profile_.systemText.isValid() ? this->profile_.systemText
                                                : this->profile_.mutedText);
        const auto linkText =
            color({u"colors"_s, u"messages"_s, u"textColors"_s, u"link"_s},
                  this->profile_.accent);
        const auto timestampText =
            color({u"colors"_s, u"messages"_s, u"textColors"_s, u"timestamp"_s},
                  this->profile_.timestampText.isValid()
                      ? this->profile_.timestampText
                      : systemText);
        const auto accent =
            color({u"colors"_s, u"accent"_s}, this->profile_.accent);

        painter.fillRect(bounds, windowBackground);

        const int radius = this->profile_.cornerRadius();
        const int tabRadius = this->profile_.tabCornerRadius;
        const auto interfaceFont =
            makeResolvedFont(this->profile_.interfaceFontFamily.isEmpty()
                                 ? QStringLiteral(DEFAULT_FONT_FAMILY)
                                 : this->profile_.interfaceFontFamily,
                             this->profile_.interfaceFontSize > 0
                                 ? this->profile_.interfaceFontSize
                                 : 9,
                             QFont::Normal);
        painter.setFont(interfaceFont);

        const QRectF titleBar(bounds.left(), bounds.top(), bounds.width(), 25);
        painter.setPen(interfaceText);
        painter.drawText(titleBar.adjusted(8, 0, -8, 0), Qt::AlignVCenter,
                         Version::instance().fullVersion());

        constexpr qreal TAB_HEIGHT = 28;
        constexpr qreal TAB_SPACER = 1;
        const TabStyle tabStyle = getSettings()->tabStyle;
        const int tabLineWidth = getSettings()->thinTabLines ? 1 : 2;
        const bool compactTabs = tabStyle == TabStyle::Compact;
        const bool showTabCloseButton =
            getSettings()->showTabCloseButton.getValue();
        const QRectF tabStrip(bounds.left(), titleBar.bottom(), bounds.width(),
                              TAB_HEIGHT + 2);
        painter.fillRect(tabStrip, windowBackground);
        const auto drawTab = [&](const QRectF &rect, const QString &text,
                                 bool selected, bool roundStart,
                                 bool roundEnd) {
            auto backgroundRect = rect;
            backgroundRect.setTop(backgroundRect.top() +
                                  (selected ? 0 : tabLineWidth));
            auto shapedRect = backgroundRect;
            if (this->profile_.tabShape == ThemeTabShape::Individual)
            {
                const qreal gap = this->profile_.tabSpacing;
                shapedRect.adjust(gap / 2, 0, -gap / 2, 0);
                roundStart = true;
                roundEnd = true;
            }
            const auto path =
                themeTopTabPath(shapedRect, tabRadius, roundStart, roundEnd);
            painter.fillPath(path, selected ? selectedTab : regularTab);
            painter.setPen(selected ? interfaceText : mutedText);
            const bool showClose = selected && showTabCloseButton;
            const qreal compactDivider = compactTabs ? 1.5 : 1.0;
            auto labelRect =
                rect.adjusted(4 / compactDivider, 0, -4 / compactDivider, 0);
            labelRect.translate(0, selected ? 1 : 2);
            if (showClose)
            {
                labelRect.setRight(labelRect.right() - rect.height() / 2);
            }
            painter.drawText(labelRect, Qt::AlignCenter,
                             QFontMetrics(painter.font())
                                 .elidedText(text, Qt::ElideRight,
                                             qRound(labelRect.width())));
            if (showClose)
            {
                painter.setRenderHint(QPainter::Antialiasing, false);
                const auto closeX = rect.right() - 12;
                const auto closeY = rect.center().y();
                painter.setPen(QPen(interfaceText, 1));
                painter.drawLine(QPointF(closeX - 3, closeY - 3),
                                 QPointF(closeX + 3, closeY + 3));
                painter.drawLine(QPointF(closeX + 3, closeY - 3),
                                 QPointF(closeX - 3, closeY + 3));
                painter.setRenderHint(QPainter::Antialiasing, true);
            }
            if (selected)
            {
                painter.save();
                painter.setClipPath(path);
                painter.fillRect(
                    QRectF(backgroundRect.left(), backgroundRect.top(),
                           backgroundRect.width(), tabLineWidth + 1),
                    selectedTabLine);
                painter.restore();
            }
            else
            {
                painter.save();
                painter.setClipPath(path);
                painter.fillRect(
                    QRectF(backgroundRect.left(), backgroundRect.top(),
                           backgroundRect.width(), tabLineWidth),
                    regularTabLine);
                painter.restore();
            }
        };
        constexpr int TAB_COUNT = 4;
        constexpr int SELECTED_TAB = 0;
        const qreal tabStart = tabStrip.left() + 2;
        const qreal tabAreaWidth = std::max<qreal>(
            0, tabStrip.width() - 4 - TAB_SPACER * (TAB_COUNT - 1));
        const std::array tabNames{u"yusuf7n"_s, u"itsbr0dyy"_s, u"wyydogg"_s,
                                  u"randomkid"_s};
        std::array<qreal, TAB_COUNT> tabWidths{};
        qreal naturalWidth = 0;
        const QFontMetricsF tabMetrics(interfaceFont);
        const qreal compactDivider = compactTabs ? 1.5 : 1.0;
        const qreal tabPadding =
            showTabCloseButton ? 32 / compactDivider : 16 / compactDivider;
        for (int index = 0; index < TAB_COUNT; ++index)
        {
            tabWidths[index] = std::clamp<qreal>(
                tabMetrics.horizontalAdvance(tabNames[index]) + tabPadding,
                TAB_HEIGHT, 150);
            naturalWidth += tabWidths[index];
        }
        if (naturalWidth > tabAreaWidth && naturalWidth > 0)
        {
            const auto scale = tabAreaWidth / naturalWidth;
            for (auto &width : tabWidths)
            {
                width = std::max<qreal>(38, width * scale);
            }

            const qreal selectedTarget =
                std::min(tabMetrics.horizontalAdvance(tabNames[SELECTED_TAB]) +
                             tabPadding,
                         tabAreaWidth - 38 * (TAB_COUNT - 1));
            if (tabWidths[SELECTED_TAB] < selectedTarget)
            {
                const qreal otherWidth =
                    naturalWidth -
                    (tabMetrics.horizontalAdvance(tabNames[SELECTED_TAB]) +
                     tabPadding);
                const qreal remaining = tabAreaWidth - selectedTarget;
                const qreal otherScale =
                    otherWidth > 0 ? remaining / otherWidth : 1;
                for (int index = 0; index < TAB_COUNT; ++index)
                {
                    if (index != SELECTED_TAB)
                    {
                        tabWidths[index] = std::max<qreal>(
                            38, tabWidths[index] * otherScale / scale);
                    }
                }
                tabWidths[SELECTED_TAB] = selectedTarget;
            }
        }
        qreal tabX = tabStart;
        for (int index = 0; index < TAB_COUNT; ++index)
        {
            drawTab(QRectF(tabX, tabStrip.top(), tabWidths[index], TAB_HEIGHT),
                    tabNames[index], index == SELECTED_TAB, index == 0,
                    index == TAB_COUNT - 1);
            tabX += tabWidths[index] + TAB_SPACER;
        }

        painter.fillRect(QRectF(tabStrip.left(), tabStrip.top() + TAB_HEIGHT,
                                tabStrip.width(), 2),
                         tabDivider);

        const QRectF split(bounds.left(), tabStrip.bottom(), bounds.width(),
                           bounds.bottom() - tabStrip.bottom());
        const QRectF channelHeader(split.left(), split.top(), split.width(),
                                   25);
        const QRectF input(split.left(), split.bottom() - 27, split.width(),
                           27);
        const QRectF messages(split.left(), channelHeader.bottom(),
                              split.width(),
                              input.top() - channelHeader.bottom());

        const bool polished =
            this->profile_.foundation == ThemeFoundation::MoltorinoPolished;
        painter.fillRect(channelHeader,
                         polished ? headerBackground : focusedHeaderBackground);
        if (!polished)
        {
            painter.setPen(focusedHeaderBorder);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(channelHeader.adjusted(0.5, 0.5, -0.5, -1.5));
        }
        painter.fillRect(input, inputBackground);
        if (!polished)
        {
            const QColor inputBorder = windowBackground.lightnessF() > 0.5
                                           ? QColor(u"#cccccc"_s)
                                           : QColor(u"#333333"_s);
            painter.setPen(inputBorder);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(input.adjusted(0.5, 0.5, -0.5, -0.5));
        }

        QPainterPath messageClip;
        if (this->profile_.roundChat && radius > 0)
        {
            messageClip.addRoundedRect(messages, radius, radius);
        }
        else
        {
            messageClip.addRect(messages);
        }
        painter.save();
        painter.setClipPath(messageClip);
        painter.fillRect(messages, this->profile_.chatBackground);
        if (this->video_ || !this->wallpaper_.isNull())
        {
            painter.setOpacity(this->profile_.wallpaperOpacity / 100.0);
            const QSizeF imageSize =
                QSizeF(this->wallpaper_.size()) / this->wallpaperScale_;
            if (this->video_)
            {
                this->video_->paint(painter, messages, this->profile_);
            }
            else if (this->profile_.wallpaperMode == ThemeWallpaperMode::Tile)
            {
                painter.save();
                painter.translate(messages.topLeft());
                const auto zoom =
                    std::clamp(this->profile_.wallpaperZoom, 100, 300) / 100.0;
                painter.scale(zoom, zoom);
                painter.scale(1.0 / this->wallpaperScale_,
                              1.0 / this->wallpaperScale_);
                painter.drawTiledPixmap(
                    QRectF(QPointF(),
                           messages.size() * this->wallpaperScale_ / zoom),
                    this->wallpaper_);
                painter.restore();
            }
            else
            {
                const auto layout = layoutThemeWallpaper(
                    imageSize, messages, this->profile_.wallpaperMode,
                    this->profile_.wallpaperFocalX,
                    this->profile_.wallpaperFocalY,
                    this->profile_.wallpaperZoom);
                painter.drawPixmap(
                    layout.destination, this->wallpaper_,
                    QRectF(layout.source.topLeft() * this->wallpaperScale_,
                           layout.source.size() * this->wallpaperScale_));
            }
            painter.setOpacity(1.0);
            if (this->profile_.wallpaperOpacity > 0)
            {
                auto overlay = this->profile_.wallpaperOverlayColor;
                overlay.setAlphaF(overlay.alphaF() *
                                  this->profile_.wallpaperOverlayOpacity /
                                  100.0);
                painter.fillRect(messages, overlay);
            }
        }

        const auto chatFamily = this->profile_.chatFontFamily.isEmpty()
                                    ? getSettings()->chatFontFamily.getValue()
                                    : this->profile_.chatFontFamily;
        const auto chatFont =
            makeResolvedFont(chatFamily,
                             this->profile_.chatFontSize > 0
                                 ? this->profile_.chatFontSize
                                 : getSettings()->chatFontSize.getValue(),
                             this->profile_.chatFontWeight > 0
                                 ? this->profile_.chatFontWeight
                                 : getSettings()->chatFontWeight.getValue());
        const auto usernameFont =
            makeResolvedFont(this->profile_.usernameFontFamily.isEmpty()
                                 ? chatFamily
                                 : this->profile_.usernameFontFamily,
                             this->profile_.usernameFontSize > 0
                                 ? this->profile_.usernameFontSize
                                 : chatFont.pointSizeF(),
                             this->profile_.usernameFontWeight > 0
                                 ? this->profile_.usernameFontWeight
                                 : getUsernameBoldness());
        const QFontMetricsF chatMetrics(chatFont);
        const QFontMetricsF usernameMetrics(usernameFont);
        const auto timestampFont = previewTimestampFont(chatFont);

        const auto systemFont = chatFont;
        const qreal timestampWidth =
            QFontMetricsF(timestampFont).horizontalAdvance(u"12:34"_s) + 5;
        const qreal lineHeight = std::ceil(std::max(chatMetrics.height(),
                                                    usernameMetrics.height())) +
                                 2;
        const qreal textAscent =
            std::max(chatMetrics.ascent(), usernameMetrics.ascent());
        const qreal badgeSize = std::clamp(lineHeight - 3, 12.0, 18.0);
        const qreal contentLeft = messages.left() + 6;
        const qreal contentRight = messages.right() - 5;
        const qreal wordSpacing = chatMetrics.horizontalAdvance(u" "_s);
        const bool showAlternates =
            this->profile_.useThemeMessageRows
                ? this->profile_.alternateMessageRows
                : getSettings()->alternateMessages.getValue();

        struct PreviewMessage {
            QString username;
            QString message;
            QColor usernameColor;
            int paint = 0;
            std::array<ImagePtr, 3> badges{};
            int badgeCount = 0;
            bool highlighted = false;
            QString linkWord;
            bool system = false;
        };
        const std::array<PreviewMessage, 7> chatMessages{
            PreviewMessage{
                u"MoltoBenne_"_s,
                u"nah i finally got this theme right"_s,
                linkText,
                1,
                {this->moderatorBadge_, this->subscriberBadge_,
                 this->partnerBadge_},
                3,
            },
            PreviewMessage{
                u"itsbr0dyy"_s,
                u"lemme see what you did"_s,
                QColor(u"#63e6be"_s),
                2,
                {this->subscriberBadge_, this->vipBadge_, nullptr},
                2,
            },
            PreviewMessage{
                u"randomkid"_s,
                u"wait where yall get moltorino"_s,
                QColor(u"#b998ff"_s),
                3,
                {this->vipBadge_, nullptr, nullptr},
                1,
            },
            PreviewMessage{
                u"wyydogg"_s,
                u"right here https://moltorino.com"_s,
                QColor(u"#66d9ef"_s),
                0,
                {this->subGifterBadge_, nullptr, nullptr},
                1,
                false,
                u"https://moltorino.com"_s,
            },
            PreviewMessage{
                u"yusuf7n"_s,
                u"okay wait the pink kinda hard"_s,
                QColor(u"#f2b45b"_s),
                0,
                {this->broadcasterBadge_, this->partnerBadge_, nullptr},
                2,
            },
            PreviewMessage{
                u"itsbr0dyy"_s,
                u"@MoltoBenne_ send the code when you done"_s,
                QColor(u"#63e6be"_s),
                2,
                {this->subscriberBadge_, this->vipBadge_, nullptr},
                2,
                true,
            },
            PreviewMessage{
                {},
                u"Theme saved. New messages use this look."_s,
                systemText,
                0,
                {},
                0,
                false,
                {},
                true,
            },
        };

        struct PreviewPiece {
            enum class Kind : uint8_t {
                Text,
                Image,
            };

            Kind kind = Kind::Text;
            QPointF baseline;
            QRectF imageRect;
            QString text;
            QFont font;
            QBrush brush;
            ImagePtr image;
        };
        struct PreviewRow {
            QRectF rect;
            int sourceIndex = 0;
            bool highlighted = false;
            std::vector<PreviewPiece> pieces;
        };

        std::vector<PreviewRow> rows;
        rows.reserve(chatMessages.size());
        qreal rowTop = messages.top() + 3;
        for (int index = 0; index < int(chatMessages.size()); ++index)
        {
            if (rowTop >= messages.bottom())
            {
                break;
            }

            const auto &message = chatMessages[index];
            PreviewRow row;
            row.sourceIndex = index;
            row.highlighted = message.highlighted;
            row.pieces.reserve(size_t(message.badgeCount) + 12);

            int line = 0;
            qreal x = contentLeft;
            const auto lineTop = [&] {
                return rowTop + line * lineHeight;
            };
            const auto baseline = [&] {
                return QPointF(x, lineTop() + textAscent);
            };
            const auto nextLine = [&] {
                ++line;
                x = contentLeft;
            };

            PreviewPiece timestampPiece;
            timestampPiece.baseline = baseline();
            timestampPiece.text = index < 3 ? u"12:34"_s : u"12:35"_s;
            timestampPiece.font = timestampFont;
            timestampPiece.brush = QBrush(timestampText);
            row.pieces.push_back(std::move(timestampPiece));
            x += timestampWidth;

            for (int badgeIndex = 0;
                 !message.system && badgeIndex < message.badgeCount;
                 ++badgeIndex)
            {
                const auto &badge = message.badges[badgeIndex];
                if (!badge || badge->isEmpty())
                {
                    continue;
                }
                PreviewPiece piece;
                piece.kind = PreviewPiece::Kind::Image;
                piece.image = badge;
                piece.imageRect =
                    QRectF(x, lineTop() + (lineHeight - badgeSize) / 2,
                           badgeSize, badgeSize);
                row.pieces.push_back(std::move(piece));
                x += badgeSize + 2;
            }

            if (!message.system)
            {
                const auto username = message.username + u":"_s;
                const qreal usernameWidth =
                    usernameMetrics.horizontalAdvance(username);
                QBrush usernameBrush(message.usernameColor);
                if (message.paint != 0)
                {
                    QLinearGradient paint(x, 0, x + usernameWidth, 0);
                    if (message.paint == 1)
                    {
                        paint.setColorAt(0, QColor(u"#ffd86f"_s));
                        paint.setColorAt(0.52, linkText);
                        paint.setColorAt(1, QColor(u"#ff8c42"_s));
                    }
                    else if (message.paint == 2)
                    {
                        paint.setColorAt(0, QColor(u"#62d8ff"_s));
                        paint.setColorAt(0.5, QColor(u"#d56dff"_s));
                        paint.setColorAt(1, QColor(u"#ff6b91"_s));
                    }
                    else
                    {
                        paint.setColorAt(0, QColor(u"#8effa0"_s));
                        paint.setColorAt(0.5, QColor(u"#54d8ff"_s));
                        paint.setColorAt(1, QColor(u"#b998ff"_s));
                    }
                    usernameBrush = QBrush(paint);
                }
                PreviewPiece usernamePiece;
                usernamePiece.baseline = baseline();
                usernamePiece.text = username;
                usernamePiece.font = usernameFont;
                usernamePiece.brush = usernameBrush;
                row.pieces.push_back(std::move(usernamePiece));
                x += usernameWidth + wordSpacing;
            }

            const auto words = message.message.split(u' ', Qt::SkipEmptyParts);
            for (int wordIndex = 0; wordIndex < words.size(); ++wordIndex)
            {
                const auto &word = words[wordIndex];
                const qreal width = chatMetrics.horizontalAdvance(word);
                if (x + width > contentRight && x > contentLeft)
                {
                    nextLine();
                }
                PreviewPiece wordPiece;
                wordPiece.baseline = baseline();
                wordPiece.text = word;
                wordPiece.font = message.system ? systemFont : chatFont;
                auto wordColor = chatText;
                if (message.system)
                {
                    wordColor = systemText;
                }
                else if (word == message.linkWord)
                {
                    wordColor = linkText;
                }
                wordPiece.brush = QBrush(wordColor);
                row.pieces.push_back(std::move(wordPiece));
                x += width;
                if (wordIndex + 1 < words.size())
                {
                    x += wordSpacing;
                }
            }

            const qreal rowHeight = (line + 1) * lineHeight + 2;
            row.rect = QRectF(messages.left(), rowTop - 1, messages.width(),
                              rowHeight);
            rows.push_back(std::move(row));
            rowTop += rowHeight;
        }

        enum class RowPass : uint8_t {
            Background,
            Shadow,
            Content,
        };
        const auto paintRows = [&](QPainter &target, RowPass pass) {
            target.setRenderHint(QPainter::SmoothPixmapTransform, true);
            for (const auto &row : rows)
            {
                if (pass == RowPass::Background)
                {
                    auto rowBackground =
                        showAlternates && row.sourceIndex % 2 == 1
                            ? messageAlternate
                            : messageRegular;
                    if (row.highlighted)
                    {
                        rowBackground = blendThemeHighlight(
                            rowBackground,
                            *ColorProvider::instance().color(
                                ColorType::SelfHighlight),
                            this->profile_.highlightOpacityAdjustment);
                    }
                    target.fillRect(row.rect, rowBackground);
                    continue;
                }

                const QPointF offset =
                    pass == RowPass::Shadow
                        ? this->profile_.messageShadowOffset()
                        : QPointF();
                for (const auto &piece : row.pieces)
                {
                    if (piece.kind == PreviewPiece::Kind::Image)
                    {
                        if (const auto pixmap = piece.image->pixmapOrLoad())
                        {
                            target.drawPixmap(
                                piece.imageRect.translated(offset), *pixmap,
                                pixmap->rect());
                        }
                        continue;
                    }
                    target.setFont(piece.font);
                    target.setPen(pass == RowPass::Shadow
                                      ? QPen(Qt::black)
                                      : QPen(piece.brush, 1));
                    target.drawText(piece.baseline + offset, piece.text);
                }
            }
        };

        paintRows(painter, RowPass::Background);
        if (this->profile_.messageShadow &&
            this->profile_.messageShadowOpacity > 0)
        {
            const qreal dpr = this->devicePixelRatioF();
            const QSize shadowSize(qCeil(this->width() * dpr),
                                   qCeil(this->height() * dpr));
            if (this->shadowPreview_.size() != shadowSize ||
                !qFuzzyCompare(this->shadowPreview_.devicePixelRatio(), dpr))
            {
                this->shadowPreview_ = renderPreviewShadow(
                    *this, messageClip, this->profile_.messageShadowColor,
                    this->profile_.messageShadowBlur,
                    [&paintRows](QPainter &shadowPainter) {
                        paintRows(shadowPainter, RowPass::Shadow);
                    });
            }
            painter.save();
            painter.setOpacity(this->profile_.messageShadowOpacity / 100.0);
            painter.drawImage(QPointF(), this->shadowPreview_);
            painter.restore();
        }
        paintRows(painter, RowPass::Content);
        painter.restore();

        if (this->profile_.roundChat && radius > 0)
        {
            paintThemeChatFrame(
                painter, messages, radius,
                this->profile_.chatBorder
                    ? std::clamp(this->profile_.chatBorderWidth, 1, 4)
                    : 0,
                polished ? headerBackground : focusedHeaderBackground,
                inputBackground, this->profile_.chatBorderColor,
                this->profile_.chatBorder ? this->profile_.chatBorderOpacity
                                          : 0);
        }

        painter.setFont(interfaceFont);
        painter.setPen(interfaceText);
        painter.drawText(channelHeader.adjusted(8, 0, -8, 0),
                         Qt::AlignVCenter | Qt::AlignHCenter,
                         u"yusuf7n (live)"_s);
        painter.setPen(mutedText);
        painter.drawText(input.adjusted(8, 0, -8, 0), Qt::AlignVCenter,
                         u"Send message as moltobenne_..."_s);

        painter.setPen(QPen(tabDivider));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(bounds);
    }

private:
    bool refreshBadgeAssets()
    {
        bool pending = false;
        bool visualChanged = false;
        size_t badgeIndex = 0;
        const auto load = [&](ImagePtr &target, const QString &set,
                              const QString &version) {
            if (!target)
            {
                if (const auto badge =
                        getApp()->getTwitchBadges()->badge(set, version))
                {
                    target = (*badge)->images.getImage1();
                }
            }
            qint64 pixmapKey = 0;
            if (target)
            {
                if (const auto pixmap = target->pixmapOrLoad())
                {
                    pixmapKey = pixmap->cacheKey();
                }
                else
                {
                    pending = true;
                }
            }
            else
            {
                pending = true;
            }
            if (this->badgePixmapKeys_[badgeIndex] != pixmapKey)
            {
                this->badgePixmapKeys_[badgeIndex] = pixmapKey;
                visualChanged = true;
            }
            ++badgeIndex;
        };
        load(this->moderatorBadge_, u"moderator"_s, u"1"_s);
        load(this->subscriberBadge_, u"subscriber"_s, u"0"_s);
        load(this->vipBadge_, u"vip"_s, u"1"_s);
        load(this->partnerBadge_, u"partner"_s, u"1"_s);
        load(this->broadcasterBadge_, u"broadcaster"_s, u"1"_s);
        load(this->subGifterBadge_, u"sub-gifter"_s, u"1"_s);
        if (visualChanged)
        {
            this->shadowPreview_ = {};
            this->update();
        }
        return pending;
    }

    void rebuildWallpaper()
    {
        this->wallpaper_ = {};
        if (!this->sourceWallpaper_.isNull())
        {
            this->wallpaper_ = QPixmap::fromImage(blurThemeWallpaper(
                this->sourceWallpaper_,
                qRound(this->profile_.wallpaperBlur * this->wallpaperScale_)));
        }
        this->update();
    }

    ThemeCustomizationProfile profile_;
    QJsonObject generatedTheme_;
    QImage sourceWallpaper_;
    qreal wallpaperScale_ = 1.0;
    QImage shadowPreview_;
    QPixmap wallpaper_;
    std::shared_ptr<ThemeVideo> video_;
    QString videoSource_;
    QMetaObject::Connection videoConnection_;

    void releaseVideo()
    {
        QObject::disconnect(this->videoConnection_);
        if (this->video_)
        {
            this->video_->detach(this);
        }
        this->video_.reset();
    }
    QString wallpaperSourceKey_;
    QString wallpaperRenderKey_;
    QTimer blurTimer_;
    QTimer badgeRefreshTimer_;
    static constexpr int MaxBadgeRefreshAttempts = 50;
    int badgeRefreshAttempts_ = 0;
    std::array<qint64, 6> badgePixmapKeys_{};
    ImagePtr moderatorBadge_;
    ImagePtr subscriberBadge_;
    ImagePtr vipBadge_;
    ImagePtr partnerBadge_;
    ImagePtr broadcasterBadge_;
    ImagePtr subGifterBadge_;
};

class TypographyPreviewWidget final : public QWidget
{
public:
    explicit TypographyPreviewWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        this->setMinimumHeight(68);
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        this->setToolTip(u"Preview the fonts and shadow."_s);
    }

    void setProfile(ThemeCustomizationProfile profile)
    {
        profile.clearDisabledFontOverrides();
        this->profile_ = std::move(profile);
        this->generatedTheme_ = buildCustomizedTheme(this->profile_);
        this->shadowPreview_ = {};
        this->updateGeometry();
        this->update();
    }

    QSize sizeHint() const override
    {
        const auto chatFont = this->chatPreviewFont();
        const auto usernameFont = this->usernamePreviewFont(chatFont);
        const QFontMetricsF chatMetrics(chatFont);
        const QFontMetricsF usernameMetrics(usernameFont);
        const int contentHeight =
            qCeil(std::max(chatMetrics.height(), usernameMetrics.height())) +
            24;
        return {300, std::max(68, contentHeight)};
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        this->shadowPreview_ = {};
        QWidget::resizeEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);

        const auto color = [this](std::initializer_list<QString> path,
                                  const QColor &fallback) {
            return nestedColor(this->generatedTheme_, path, fallback);
        };
        const auto background =
            color({u"colors"_s, u"messages"_s, u"backgrounds"_s, u"regular"_s},
                  this->profile_.chatBackground);
        const auto text =
            color({u"colors"_s, u"messages"_s, u"textColors"_s, u"regular"_s},
                  this->profile_.effectiveChatText());
        const auto usernameColor =
            color({u"colors"_s, u"messages"_s, u"textColors"_s, u"link"_s},
                  this->profile_.effectiveChatText());
        const auto frame =
            color({u"colors"_s, u"splits"_s, u"input"_s, u"border"_s},
                  this->profile_.raisedSurface);

        const QRectF bounds = this->rect().adjusted(1, 1, -1, -1);
        const int radius =
            this->profile_.roundChat ? this->profile_.cornerRadius() : 0;
        QPainterPath clip;
        if (radius > 0)
        {
            clip.addRoundedRect(bounds, radius, radius);
        }
        else
        {
            clip.addRect(bounds);
        }
        painter.setClipPath(clip);
        painter.fillRect(bounds, background);

        const auto chatFont = this->chatPreviewFont();
        const auto usernameFont = this->usernamePreviewFont(chatFont);
        const QFontMetricsF chatMetrics(chatFont);
        const QFontMetricsF usernameMetrics(usernameFont);

        const qreal horizontalPadding = 14;
        const qreal contentWidth =
            std::max<qreal>(1, bounds.width() - horizontalPadding * 2);
        const QString username = u"MoltoBenne_:"_s;
        const QString message = u"this theme feels like mine"_s;
        const qreal usernameWidth = usernameMetrics.horizontalAdvance(username);
        const qreal gap = chatMetrics.horizontalAdvance(u" "_s);
        const qreal inlineOffset = usernameWidth + gap;
        const qreal messageWidth = chatMetrics.horizontalAdvance(message);
        const qreal inlineWidth = contentWidth - inlineOffset;
        const bool startMessageOnNextLine = inlineWidth < messageWidth;
        const qreal rowWidth = inlineOffset + messageWidth;
        const qreal rowLeft =
            !startMessageOnNextLine
                ? bounds.left() +
                      std::max<qreal>(horizontalPadding,
                                      (bounds.width() - rowWidth) / 2)
                : bounds.left() + horizontalPadding;
        const qreal messageX =
            rowLeft + (startMessageOnNextLine ? 0 : inlineOffset);

        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        QTextLayout messageLayout(message, chatFont);
        messageLayout.setTextOption(option);
        messageLayout.beginLayout();
        qreal messageHeight = 0;
        qreal firstLineAscent = chatMetrics.ascent();
        int lineIndex = 0;
        while (true)
        {
            auto line = messageLayout.createLine();
            if (!line.isValid())
            {
                break;
            }
            const bool firstLine = lineIndex++ == 0;
            const qreal width = firstLine && !startMessageOnNextLine
                                    ? std::max<qreal>(1, inlineWidth)
                                    : contentWidth;
            line.setLineWidth(width);
            line.setPosition(
                QPointF(firstLine || startMessageOnNextLine ? 0 : -inlineOffset,
                        messageHeight));
            if (firstLine)
            {
                firstLineAscent = line.ascent();
            }
            messageHeight += line.height();
        }
        messageLayout.endLayout();

        const qreal usernameTop =
            startMessageOnNextLine ? usernameMetrics.height() + 3 : 0;
        const qreal aboveBaseline =
            std::max(usernameMetrics.ascent(),
                     startMessageOnNextLine ? 0.0 : firstLineAscent);
        const qreal belowBaseline = std::max(
            usernameMetrics.descent(),
            startMessageOnNextLine
                ? usernameTop + messageHeight - usernameMetrics.ascent()
                : messageHeight - firstLineAscent);
        const qreal baseline =
            bounds.center().y() + (aboveBaseline - belowBaseline) / 2;
        const QPointF usernamePosition(rowLeft, baseline);
        const qreal messageTop = startMessageOnNextLine
                                     ? baseline + usernameMetrics.descent() + 3
                                     : baseline - firstLineAscent;
        const QPointF messagePosition(messageX, messageTop);

        const auto paintSample = [&](QPainter &target, bool shadow) {
            target.setFont(usernameFont);
            target.setPen(shadow ? QColor(Qt::black) : usernameColor);
            target.drawText(usernamePosition, username);
            target.setFont(chatFont);
            target.setPen(shadow ? QColor(Qt::black) : text);
            messageLayout.draw(&target, messagePosition);
        };

        if (this->profile_.messageShadow &&
            this->profile_.messageShadowOpacity > 0)
        {
            const qreal dpr = this->devicePixelRatioF();
            const QSize shadowSize(qCeil(this->width() * dpr),
                                   qCeil(this->height() * dpr));
            if (this->shadowPreview_.size() != shadowSize ||
                !qFuzzyCompare(this->shadowPreview_.devicePixelRatio(), dpr))
            {
                this->shadowPreview_ = renderPreviewShadow(
                    *this, clip, this->profile_.messageShadowColor,
                    this->profile_.messageShadowBlur,
                    [this, &paintSample](QPainter &shadowPainter) {
                        shadowPainter.translate(
                            this->profile_.messageShadowOffset());
                        paintSample(shadowPainter, true);
                    });
            }
            painter.save();
            painter.setOpacity(this->profile_.messageShadowOpacity / 100.0);
            painter.drawImage(QPointF(), this->shadowPreview_);
            painter.restore();
        }
        paintSample(painter, false);

        painter.setClipping(false);
        painter.setPen(QPen(frame, 1));
        painter.setBrush(Qt::NoBrush);
        if (radius > 0)
        {
            painter.drawRoundedRect(bounds, radius, radius);
        }
        else
        {
            painter.drawRect(bounds);
        }
    }

private:
    QFont chatPreviewFont() const
    {
        const auto family = this->profile_.chatFontFamily.isEmpty()
                                ? getSettings()->chatFontFamily.getValue()
                                : this->profile_.chatFontFamily;
        const auto weight = this->profile_.chatFontWeight > 0
                                ? this->profile_.chatFontWeight
                                : getSettings()->chatFontWeight.getValue();

        return makeResolvedFont(family, 14, weight);
    }

    QFont usernamePreviewFont(const QFont &chatFont) const
    {
        const auto family = this->profile_.usernameFontFamily.isEmpty()
                                ? chatFont.family()
                                : this->profile_.usernameFontFamily;
        const auto weight = this->profile_.usernameFontWeight > 0
                                ? this->profile_.usernameFontWeight
                                : getUsernameBoldness();
        return makeResolvedFont(family, chatFont.pointSizeF(), weight);
    }

    ThemeCustomizationProfile profile_;
    QJsonObject generatedTheme_;
    QImage shadowPreview_;
};

CustomizationPage::CustomizationPage()
{
    this->buildUi();
    this->reloadProfiles(getTheme()->themeName.getValue());
    this->managedConnections_.managedConnect(getTheme()->updated, [this] {
        if (!this->isVisible())
        {
            return;
        }
        if (this->selectedCustom_ && this->profile_ != this->savedProfile_)
        {
            this->updateControlState();
            return;
        }
        this->reloadProfiles(getTheme()->themeName.getValue());
    });
}

CustomizationPage::~CustomizationPage()
{
    this->transferCancellation_.request_stop();
    this->wallpaperCancellation_.request_stop();
}

void CustomizationPage::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(8);

    auto *profileRow = new QHBoxLayout;
    profileRow->setSpacing(6);
    profileRow->addWidget(new QLabel(u"Theme:"_s));
    this->profileBox_ = new QComboBox;
    this->profileBox_->setSizePolicy(QSizePolicy::Expanding,
                                     QSizePolicy::Preferred);
    profileRow->addWidget(this->profileBox_, 1);
    this->profileKind_ = new QLabel;
    this->profileKind_->setMinimumWidth(56);
    profileRow->addWidget(this->profileKind_);
    this->useButton_ = new QPushButton(u"Apply theme"_s);

    this->duplicateButton_ = new QPushButton(u"Customize"_s);
    this->duplicateButton_->setToolTip(
        u"Create an editable copy of this theme."_s);
    this->renameButton_ = new QPushButton(u"Rename"_s);
    this->deleteButton_ = new QPushButton(u"Delete"_s);
    profileRow->addWidget(this->useButton_);
    profileRow->addWidget(this->duplicateButton_);
    profileRow->addWidget(this->renameButton_);
    profileRow->addWidget(this->deleteButton_);
    outer->addLayout(profileRow);

    auto *workspace = new QSplitter(Qt::Horizontal, this);
    workspace->setChildrenCollapsible(false);
    workspace->setHandleWidth(7);
    this->preview_ = new ThemePreviewWidget;
    this->preview_->setMinimumWidth(290);
    workspace->addWidget(this->preview_);
    getSettings()->thinTabLines.connect(
        [this] {
            this->preview_->update();
        },
        this->managedConnections_, false);

    auto *tabs = new QTabWidget;
    tabs->setMinimumWidth(470);
    tabs->addTab(this->buildColorsTab(), u"Colors"_s);
    tabs->addTab(this->buildChatTab(), u"Chat"_s);
    tabs->addTab(this->buildInterfaceTab(), u"Interface"_s);
    tabs->addTab(this->buildTypographyTab(), u"Text"_s);
    tabs->addTab(this->buildShareTab(), u"Share and import"_s);
    workspace->addWidget(tabs);
    workspace->setStretchFactor(0, 2);
    workspace->setStretchFactor(1, 3);
    workspace->setSizes({340, 620});
    outer->addWidget(workspace, 1);

    auto *footer = new QHBoxLayout;
    this->status_ = new QLabel;
    this->status_->setTextFormat(Qt::PlainText);
    this->status_->setWordWrap(false);
    this->statusAnimation_ = new QTimer(this);
    this->statusAnimation_->setInterval(400);
    QObject::connect(this->statusAnimation_, &QTimer::timeout, this, [this] {
        this->statusDots_ = this->statusDots_ % 3 + 1;
        this->status_->setText(this->statusText_ +
                               QString(this->statusDots_, u'.'));
    });
    footer->addWidget(this->status_, 1);
    this->cancelWallpaperButton_ = new QPushButton(u"Cancel"_s);
    this->cancelWallpaperButton_->hide();
    footer->addWidget(this->cancelWallpaperButton_);
    QObject::connect(this->cancelWallpaperButton_, &QPushButton::clicked, this,
                     [this] {
                         this->wallpaperCancellation_.request_stop();
                         ++this->wallpaperGeneration_;
                         this->wallpaperPreparing_ = false;
                         this->setTransferBusy(false);
                         this->setStatus(u"Background preparation cancelled"_s);
                     });
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        this->wallpaperCancellation_.request_stop();
        this->transferCancellation_.request_stop();
    });
    this->resetButton_ = new QPushButton(u"Revert changes"_s);
    this->saveButton_ = new QPushButton(u"Save and apply"_s);
    footer->addWidget(this->resetButton_);
    footer->addWidget(this->saveButton_);
    outer->addLayout(footer);

    QObject::connect(
        this->profileBox_, QOverload<int>::of(&QComboBox::activated), this,
        [this] {
            const auto requestedKey = this->profileBox_->currentData();
            if (!this->confirmProfileChange())
            {
                this->profileBox_->blockSignals(true);
                this->profileBox_->setCurrentIndex(
                    this->profileBox_->findData(this->selectedKey_));
                this->profileBox_->blockSignals(false);
                return;
            }

            this->profileBox_->setCurrentIndex(
                this->profileBox_->findData(requestedKey));
            this->loadSelectedProfile();
        });
    QObject::connect(this->useButton_, &QPushButton::clicked, this,
                     &CustomizationPage::applySelectedTheme);
    QObject::connect(this->duplicateButton_, &QPushButton::clicked, this,
                     &CustomizationPage::duplicateProfile);
    QObject::connect(this->renameButton_, &QPushButton::clicked, this,
                     &CustomizationPage::renameProfile);
    QObject::connect(this->deleteButton_, &QPushButton::clicked, this,
                     &CustomizationPage::deleteProfile);
    QObject::connect(this->saveButton_, &QPushButton::clicked, this, [this] {
        this->saveCustomProfile(true);
    });
    QObject::connect(this->resetButton_, &QPushButton::clicked, this,
                     &CustomizationPage::resetDraft);
}

QWidget *CustomizationPage::buildColorsTab()
{
    auto *page = new QWidget;
    this->colorsEditor_ = page;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);
    auto *description = new QLabel(u"Choose your app colors."_s);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto *surfaces = new QGroupBox(u"Surfaces"_s);
    auto *surfaceGrid = new QGridLayout(surfaces);
    surfaceGrid->setHorizontalSpacing(14);
    surfaceGrid->setVerticalSpacing(6);
    auto *details = new QGroupBox(u"Text colors"_s);
    auto *detailGrid = new QGridLayout(details);
    detailGrid->setHorizontalSpacing(14);
    detailGrid->setVerticalSpacing(6);
    const struct Definition {
        const char *role;
        const char *label;
        const char *description;
        QColor ThemeCustomizationProfile::*member;
    } definitions[] = {
        {"background", "Background",
         "Window background behind tabs and panels.",
         &ThemeCustomizationProfile::background},
        {"chatBackground", "Chat background",
         "Background behind chat messages.",
         &ThemeCustomizationProfile::chatBackground},
        {"surface", "Surface", "Tabs, menus, and controls.",
         &ThemeCustomizationProfile::surface},
        {"raisedSurface", "Raised surface",
         "Selected tabs, inputs, and raised panels.",
         &ThemeCustomizationProfile::raisedSurface},
        {"text", "Text", "Main interface text.",
         &ThemeCustomizationProfile::text},
        {"chatText", "Chat text", "Regular chat messages.",
         &ThemeCustomizationProfile::chatText},
        {"mutedText", "Muted text", "Secondary interface text.",
         &ThemeCustomizationProfile::mutedText},
        {"systemText", "System messages", "Connection and moderation notices.",
         &ThemeCustomizationProfile::systemText},
        {"timestampText", "Timestamps", "Message timestamps.",
         &ThemeCustomizationProfile::timestampText},
        {"accent", "Accent", "Active tabs, links, and focused controls.",
         &ThemeCustomizationProfile::accent},
    };
    for (int index = 0; index < int(std::size(definitions)); ++index)
    {
        const auto &definition = definitions[index];
        const bool surfaceColor = index < 4;
        const int localIndex = surfaceColor ? index : index - 4;
        const int column = localIndex % 2;
        const int row = localIndex / 2;
        auto *container = new QWidget;
        auto *rowLayout = new QHBoxLayout(container);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(5);
        auto *label = new QLabel(QString::fromLatin1(definition.label));
        label->setMinimumWidth(102);
        auto *button = new QPushButton;
        button->setFixedWidth(34);
        auto *input = new QLineEdit;
        input->setMaxLength(9);
        const auto tooltip = QString::fromLatin1(definition.description);
        container->setToolTip(tooltip);
        button->setToolTip(tooltip);
        input->setToolTip(tooltip);
        rowLayout->addWidget(label);
        rowLayout->addWidget(button);
        rowLayout->addWidget(input, 1);
        (surfaceColor ? surfaceGrid : detailGrid)
            ->addWidget(container, row, column);
        this->colorControls_.append({button, input, definition.member});
        QObject::connect(
            button, &QPushButton::clicked, this,
            [this, member = definition.member,
             role = QString::fromLatin1(definition.role)] {
                auto *picker =
                    new ColorPickerDialog(this->profile_.*member, this);
                QObject::connect(
                    picker, &ColorPickerDialog::colorConfirmed, this,
                    [this, member, role, generation = this->profileGeneration_](
                        const QColor color) {
                        if (generation != this->profileGeneration_)
                        {
                            return;
                        }
                        this->profile_.*member = color;
                        this->markColorEdited(role);
                        this->loadProfileIntoControls();
                    });
                picker->show();
            });
        QObject::connect(
            input, &QLineEdit::editingFinished, this,
            [this, input, role = QString::fromLatin1(definition.role)] {
                if (QColor(input->text().trimmed()).isValid())
                {
                    this->markColorEdited(role);
                }
                this->readControlsIntoProfile();
                this->loadProfileIntoControls();
            });
    }
    this->chatTextFollowsText_ = new QCheckBox(u"Use Text for chat messages"_s);
    this->chatTextFollowsText_->setToolTip(
        u"Use the same color for chat and interface text."_s);
    detailGrid->addWidget(this->chatTextFollowsText_, 3, 0, 1, 2);
    QObject::connect(this->chatTextFollowsText_, &QCheckBox::toggled, this,
                     [this](bool linked) {
                         if (this->loadingControls_)
                         {
                             return;
                         }
                         this->profile_.separateChatText = !linked;
                         if (linked)
                         {
                             this->profile_.chatText = this->profile_.text;
                             this->editedColorRoles_.remove(u"chatText"_s);
                         }
                         this->loadProfileIntoControls();
                     });
    layout->addWidget(surfaces);
    layout->addWidget(details);

    auto *paletteHelper = new QGroupBox(u"Palette helper"_s);
    auto *paletteLayout = new QHBoxLayout(paletteHelper);
    paletteLayout->setContentsMargins(8, 5, 8, 5);
    auto *paletteHint = new QLabel(u"Keep some colors and rebuild the rest."_s);
    paletteHint->setWordWrap(true);
    paletteLayout->addWidget(paletteHint, 1);
    auto *buildPaletteButton = new QPushButton(u"Build palette…"_s);
    buildPaletteButton->setToolTip(u"Build around your chosen colors."_s);
    paletteLayout->addWidget(buildPaletteButton);
    layout->addWidget(paletteHelper);

    auto *behavior = new QGroupBox(u"Color balance"_s);
    auto *behaviorForm = new QFormLayout(behavior);
    behaviorForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto *panelContrastRow = makeSlider(0, 100, &this->panelContrast_, u"%"_s);
    this->panelContrast_->setToolTip(u"Adjusts contrast between panels."_s);
    panelContrastRow->setToolTip(this->panelContrast_->toolTip());
    this->accentStrength_ = new QComboBox;
    this->accentStrength_->addItems({u"Subtle"_s, u"Balanced"_s, u"Bold"_s});
    this->accentStrength_->setToolTip(
        u"How strongly controls use the accent color."_s);
    behaviorForm->addRow(u"Panel contrast:"_s, panelContrastRow);
    behaviorForm->addRow(u"Accent use:"_s, this->accentStrength_);
    layout->addWidget(behavior);

    this->readability_ = new QLabel;
    this->readability_->setWordWrap(true);
    layout->addWidget(this->readability_);
    layout->addStretch(1);

    const auto update = [this] {
        if (!this->loadingControls_)
        {
            this->readControlsIntoProfile();
        }
    };
    QObject::connect(this->panelContrast_, &QSlider::valueChanged, this,
                     update);
    QObject::connect(this->accentStrength_, &QComboBox::currentIndexChanged,
                     this, update);
    QObject::connect(buildPaletteButton, &QPushButton::clicked, this,
                     &CustomizationPage::buildPalette);
    return page;
}

QWidget *CustomizationPage::buildChatTab()
{
    auto *page = new QWidget;
    this->chatEditor_ = page;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(7);

    auto *wallpaper = new QGroupBox(u"Wallpaper"_s);
    auto *wallpaperLayout = new QVBoxLayout(wallpaper);
    wallpaperLayout->setSpacing(6);
    auto *form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setVerticalSpacing(6);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto *pathRow = new QWidget;
    auto *pathLayout = new QHBoxLayout(pathRow);
    pathLayout->setContentsMargins(0, 0, 0, 0);
    pathLayout->setSpacing(5);
    this->wallpaperPath_ = new QLineEdit;
    this->wallpaperPath_->setReadOnly(true);
    this->wallpaperPath_->setPlaceholderText(u"No image or video"_s);
    auto *choose = new QPushButton(u"Choose image or video"_s);
    choose->setToolTip(u"Images or videos. Videos use more resources."_s);
    auto *clear = new QPushButton(u"Remove"_s);
    pathLayout->addWidget(this->wallpaperPath_, 1);
    pathLayout->addWidget(choose);
    pathLayout->addWidget(clear);
    form->addRow(u"Background:"_s, pathRow);
    this->wallpaperMode_ = new QComboBox;
    this->wallpaperMode_->addItems({u"Fill chat"_s, u"Fit whole background"_s,
                                    u"Original size"_s, u"Tile"_s,
                                    u"Stretch"_s});
    form->addRow(u"Placement:"_s, this->wallpaperMode_);
    form->addRow(u"Zoom:"_s,
                 makeSlider(100, 300, &this->wallpaperZoom_, u"%"_s));
    this->wallpaperZoom_->setToolTip(u"Zoom around the focus point."_s);
    form->addRow(u"Wallpaper opacity:"_s,
                 makeSlider(0, 100, &this->wallpaperOpacity_, u"%"_s));

    auto *overlayColor = new QWidget;
    auto *overlayColorLayout = new QHBoxLayout(overlayColor);
    overlayColorLayout->setContentsMargins(0, 0, 0, 0);
    overlayColorLayout->setSpacing(5);
    this->wallpaperOverlayColorButton_ = new QPushButton;
    this->wallpaperOverlayColorButton_->setFixedWidth(34);
    this->wallpaperOverlayColorInput_ = new QLineEdit;
    this->wallpaperOverlayColorInput_->setMaxLength(9);
    overlayColorLayout->addWidget(this->wallpaperOverlayColorButton_);
    overlayColorLayout->addWidget(this->wallpaperOverlayColorInput_, 1);
    form->addRow(u"Overlay color:"_s, overlayColor);
    this->colorControls_.append(
        {this->wallpaperOverlayColorButton_,
         this->wallpaperOverlayColorInput_,
         &ThemeCustomizationProfile::wallpaperOverlayColor});
    form->addRow(u"Overlay opacity:"_s,
                 makeSlider(0, 100, &this->wallpaperOverlayOpacity_, u"%"_s));
    this->wallpaperOverlayOpacity_->setToolTip(
        u"Blends the overlay color over the wallpaper."_s);
    form->addRow(u"Blur:"_s, makeSlider(0, 30, &this->wallpaperBlur_));
    this->wallpaperFocus_ = new WallpaperFocusWidget;
    form->addRow(u"Focus:"_s, this->wallpaperFocus_);

    auto *focusHint = new QLabel(u"Keep this part in view."_s);
    focusHint->setObjectName(u"description"_s);
    focusHint->setWordWrap(true);
    auto *focusHelp = new QWidget;
    auto *focusHelpLayout = new QHBoxLayout(focusHelp);
    focusHelpLayout->setContentsMargins(0, 0, 0, 0);
    focusHelpLayout->setSpacing(6);
    focusHelpLayout->addWidget(focusHint, 1);
    this->wallpaperCenterFocus_ = new QPushButton(u"Center"_s);
    this->wallpaperCenterFocus_->setToolTip(u"Center the focus point."_s);
    focusHelpLayout->addWidget(this->wallpaperCenterFocus_);
    form->addRow(QString(), focusHelp);
    auto *palette = new QPushButton(u"Use colors from wallpaper"_s);
    form->addRow(QString(), palette);
    wallpaperLayout->addLayout(form);
    layout->addWidget(wallpaper);

    auto *rows = new QGroupBox(u"Alternating message rows"_s);
    auto *rowsForm = new QFormLayout(rows);
    this->alternateMessageRows_ =
        new QCheckBox(u"Alternate message backgrounds"_s);
    this->alternateMessageRows_->setToolTip(
        u"Overrides the global alternating rows setting."_s);
    rowsForm->addRow(QString(), this->alternateMessageRows_);
    rowsForm->addRow(
        u"Difference:"_s,
        makeSlider(0, 100, &this->alternateMessageContrast_, u"%"_s));
    this->alternateMessageContrast_->setToolTip(
        u"Contrast between alternating rows."_s);
    rowsForm->addRow(
        u"Strength:"_s,
        makeSlider(0, 100, &this->alternateMessageOpacity_, u"%"_s));
    this->alternateMessageOpacity_->setToolTip(
        u"Opacity of the alternating row color."_s);
    layout->addWidget(rows);

    auto *highlights = new QGroupBox(u"Highlighted messages"_s);
    auto *highlightsForm = new QFormLayout(highlights);
    highlightsForm->addRow(
        u"Opacity:"_s,
        makeOpacityAdjustmentSlider(&this->highlightOpacityAdjustment_));
    this->highlightOpacityAdjustment_->setToolTip(
        u"Original keeps each highlight's own opacity."_s);
    highlights->setToolTip(this->highlightOpacityAdjustment_->toolTip());
    layout->addWidget(highlights);

    layout->addStretch(1);

    QObject::connect(choose, &QPushButton::clicked, this,
                     &CustomizationPage::chooseWallpaper);
    QObject::connect(clear, &QPushButton::clicked, this,
                     &CustomizationPage::clearWallpaper);
    QObject::connect(palette, &QPushButton::clicked, this,
                     &CustomizationPage::suggestWallpaperPalette);
    QObject::connect(this->wallpaperCenterFocus_, &QPushButton::clicked, this,
                     [this] {
                         this->profile_.wallpaperFocalX = 50;
                         this->profile_.wallpaperFocalY = 50;
                         this->wallpaperFocus_->setProfile(this->profile_);
                         this->updatePreview();
                     });
    QObject::connect(this->wallpaperOverlayColorButton_, &QPushButton::clicked,
                     this, [this] {
                         auto *picker = new ColorPickerDialog(
                             this->profile_.wallpaperOverlayColor, this);
                         QObject::connect(
                             picker, &ColorPickerDialog::colorConfirmed, this,
                             [this, generation = this->profileGeneration_](
                                 const QColor color) {
                                 if (generation != this->profileGeneration_)
                                 {
                                     return;
                                 }
                                 this->profile_.wallpaperOverlayColor = color;
                                 this->loadProfileIntoControls();
                             });
                         picker->show();
                     });
    QObject::connect(this->wallpaperOverlayColorInput_,
                     &QLineEdit::editingFinished, this, [this] {
                         this->readControlsIntoProfile();
                         this->loadProfileIntoControls();
                     });
    this->wallpaperFocus_->focusChanged = [this](int x, int y) {
        if (this->loadingControls_)
        {
            return;
        }
        this->profile_.wallpaperFocalX = x;
        this->profile_.wallpaperFocalY = y;
        this->updatePreview();
    };
    const auto update = [this] {
        if (!this->loadingControls_)
        {
            this->readControlsIntoProfile();
        }
    };
    QObject::connect(this->wallpaperMode_, &QComboBox::currentIndexChanged,
                     this, update);
    for (auto *slider :
         {this->wallpaperOpacity_, this->wallpaperZoom_,
          this->wallpaperOverlayOpacity_, this->wallpaperBlur_,
          this->alternateMessageOpacity_, this->alternateMessageContrast_,
          this->highlightOpacityAdjustment_})
    {
        QObject::connect(slider, &QSlider::valueChanged, this, update);
    }
    QObject::connect(this->alternateMessageRows_, &QCheckBox::toggled, this,
                     update);
    return page;
}

QWidget *CustomizationPage::buildInterfaceTab()
{
    auto *page = new QWidget;
    this->interfaceEditor_ = page;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto *tabsAndBars = new QGroupBox(u"Tabs and channel bar"_s);
    auto *shapeForm = new QFormLayout(tabsAndBars);
    shapeForm->setVerticalSpacing(6);
    shapeForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    this->foundation_ = new QComboBox;
    this->foundation_->addItems(
        {u"Chatterino classic"_s, u"Moltorino polished"_s});
    this->foundation_->setToolTip(u"Polished uses simpler bars and borders."_s);
    this->tabShape_ = new QComboBox;
    this->tabShape_->addItems({u"Connected"_s, u"Individual"_s});
    this->tabShape_->setToolTip(
        u"Connected tabs join together. Individual tabs allow gaps."_s);
    shapeForm->addRow(u"Window style:"_s, this->foundation_);
    shapeForm->addRow(u"Tab shape:"_s, this->tabShape_);
    shapeForm->addRow(u"Tab rounding:"_s,
                      makeSlider(0, 10, &this->tabCornerRadius_));
    shapeForm->addRow(u"Space between tabs:"_s,
                      makeSlider(0, 4, &this->tabSpacing_));
    shapeForm->addRow(u"Inactive tabs:"_s,
                      makeSlider(0, 100, &this->inactiveTabContrast_, u"%"_s));
    this->inactiveTabContrast_->setToolTip(
        u"Contrast between inactive tabs and the window."_s);
    shapeForm->addRow(u"Channel bar:"_s,
                      makeSlider(0, 100, &this->channelBarContrast_, u"%"_s));
    this->channelBarContrast_->setToolTip(
        u"Contrast between the channel bar and chat."_s);
    layout->addWidget(tabsAndBars);

    auto *frame = new QGroupBox(u"Message area"_s);
    auto *frameForm = new QFormLayout(frame);
    frameForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    this->roundChat_ = new QCheckBox(u"Round the message area"_s);
    this->roundChat_->setToolTip(u"Also rounds the area below banners."_s);
    frameForm->addRow(QString(), this->roundChat_);
    frameForm->addRow(u"Corner rounding:"_s,
                      makeSlider(0, 16, &this->chatCornerRadius_));
    this->chatCornerRadius_->setToolTip(u"Rounding of the message area."_s);
    this->chatBorder_ = new QCheckBox(u"Draw an inner border"_s);
    this->chatBorder_->setToolTip(u"A border inside the message area."_s);
    frameForm->addRow(QString(), this->chatBorder_);

    auto *borderColor = new QWidget;
    auto *borderColorLayout = new QHBoxLayout(borderColor);
    borderColorLayout->setContentsMargins(0, 0, 0, 0);
    borderColorLayout->setSpacing(5);
    this->chatBorderColorButton_ = new QPushButton;
    this->chatBorderColorButton_->setFixedWidth(34);
    this->chatBorderColorInput_ = new QLineEdit;
    this->chatBorderColorInput_->setMaxLength(9);
    borderColorLayout->addWidget(this->chatBorderColorButton_);
    borderColorLayout->addWidget(this->chatBorderColorInput_, 1);
    frameForm->addRow(u"Border color:"_s, borderColor);
    this->colorControls_.append({this->chatBorderColorButton_,
                                this->chatBorderColorInput_,
                                &ThemeCustomizationProfile::chatBorderColor});
    frameForm->addRow(u"Border width:"_s,
                      makeSlider(1, 4, &this->chatBorderWidth_, u" px"_s));
    frameForm->addRow(u"Border strength:"_s,
                      makeSlider(0, 100, &this->chatBorderOpacity_, u"%"_s));
    this->chatBorderOpacity_->setToolTip(
        u"Border opacity. Set to 0% to hide it."_s);
    layout->addWidget(frame);

    layout->addStretch(1);

    const auto update = [this] {
        if (!this->loadingControls_)
        {
            this->readControlsIntoProfile();
        }
    };
    QObject::connect(this->foundation_, &QComboBox::currentIndexChanged, this,
                     update);
    QObject::connect(this->tabShape_, &QComboBox::currentIndexChanged, this,
                     update);
    QObject::connect(this->tabCornerRadius_, &QSlider::valueChanged, this,
                     update);
    QObject::connect(this->tabSpacing_, &QSlider::valueChanged, this, update);
    QObject::connect(this->inactiveTabContrast_, &QSlider::valueChanged, this,
                     update);
    QObject::connect(this->channelBarContrast_, &QSlider::valueChanged, this,
                     update);
    QObject::connect(this->roundChat_, &QCheckBox::toggled, this, update);
    QObject::connect(this->chatCornerRadius_, &QSlider::valueChanged, this,
                     update);
    QObject::connect(this->chatBorder_, &QCheckBox::toggled, this, update);
    QObject::connect(this->chatBorderWidth_, &QSlider::valueChanged, this,
                     update);
    QObject::connect(this->chatBorderOpacity_, &QSlider::valueChanged, this,
                     update);
    QObject::connect(
        this->chatBorderColorButton_, &QPushButton::clicked, this, [this] {
            auto *picker =
                new ColorPickerDialog(this->profile_.chatBorderColor, this);
            QObject::connect(picker, &ColorPickerDialog::colorConfirmed, this,
                             [this, generation = this->profileGeneration_](
                                 const QColor color) {
                                 if (generation != this->profileGeneration_)
                                 {
                                     return;
                                 }
                                 this->profile_.chatBorderColor = color;
                                 this->loadProfileIntoControls();
                             });
            picker->show();
        });
    QObject::connect(this->chatBorderColorInput_, &QLineEdit::editingFinished,
                     this, [this] {
                         this->readControlsIntoProfile();
                         this->loadProfileIntoControls();
                     });
    return page;
}

QWidget *CustomizationPage::buildTypographyTab()
{
    auto *page = new QWidget;
    this->typographyEditor_ = page;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto *description =
        new QLabel(u"Choose theme fonts or keep your usual settings."_s);
    description->setWordWrap(true);
    layout->addWidget(description);
    this->useThemeFonts_ = new QCheckBox(u"Use theme fonts"_s);
    this->useThemeFonts_->setToolTip(
        u"Off uses your General chat fonts and the default UI font."_s);
    this->useThemeFontSizes_ = new QCheckBox(u"Use theme font sizes"_s);
    this->useThemeFontSizes_->setToolTip(
        u"Off uses your usual sizes. UI scaling still applies."_s);
    auto *fontChoices = new QHBoxLayout;
    fontChoices->addWidget(this->useThemeFonts_);
    fontChoices->addWidget(this->useThemeFontSizes_);
    fontChoices->addStretch(1);
    layout->addLayout(fontChoices);
    auto *optionsGrid = new QGridLayout;
    optionsGrid->setContentsMargins(0, 0, 0, 0);
    optionsGrid->setHorizontalSpacing(8);
    optionsGrid->setVerticalSpacing(8);
    optionsGrid->setColumnStretch(0, 1);
    optionsGrid->setColumnStretch(1, 1);

    const auto fontFamilies = cleanFontFamilies();
    const auto makeFontBox = [&fontFamilies](const QString &defaultLabel) {
        auto *box = new QComboBox;
        box->addItem(defaultLabel, QString());
        for (const auto &family : fontFamilies)
        {
            box->addItem(family, family);
        }
        box->setMaxVisibleItems(20);
        return box;
    };

    auto *chat = new QGroupBox(u"Messages"_s);
    auto *chatForm = new QFormLayout(chat);
    chatForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    this->chatFont_ = makeFontBox(u"Use chat font from General"_s);
    chatForm->addRow(u"Font:"_s, this->chatFont_);
    chatForm->addRow(u"Size:"_s,
                     makeSlider(8, 32, &this->chatFontSize_, u" pt"_s));
    chatForm->addRow(u"Weight:"_s,
                     makeSlider(100, 900, &this->chatFontWeight_));
    this->chatFontWeight_->setSingleStep(50);
    this->chatFontWeight_->setPageStep(100);

    auto *usernames = new QGroupBox(u"Usernames"_s);
    auto *usernameForm = new QFormLayout(usernames);
    usernameForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    this->usernameFont_ = makeFontBox(u"Use the message font"_s);
    usernameForm->addRow(u"Font:"_s, this->usernameFont_);
    usernameForm->addRow(u"Size:"_s,
                         makeSlider(8, 32, &this->usernameFontSize_, u" pt"_s));
    usernameForm->addRow(u"Weight:"_s,
                         makeSlider(100, 900, &this->usernameFontWeight_));
    this->usernameFontWeight_->setSingleStep(50);
    this->usernameFontWeight_->setPageStep(100);
    this->usernameFontWeight_->setToolTip(u"Username weight only."_s);

    auto *interface = new QGroupBox(u"Interface"_s);
    auto *interfaceForm = new QFormLayout(interface);
    interfaceForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    this->interfaceFont_ = makeFontBox(u"Use Moltorino interface font"_s);
    interfaceForm->addRow(u"Font:"_s, this->interfaceFont_);
    interfaceForm->addRow(
        u"Size:"_s, makeSlider(8, 18, &this->interfaceFontSize_, u" pt"_s));

    auto *shadow = new QGroupBox(u"Readability shadow"_s);
    auto *shadowForm = new QFormLayout(shadow);
    this->messageShadow_ = new QCheckBox(u"Add a shadow behind chat content"_s);
    this->messageShadow_->setToolTip(
        u"Improves readability over busy backgrounds."_s);
    shadowForm->addRow(QString(), this->messageShadow_);
    this->messageShadowEmotes_ = new QCheckBox(u"Include emotes and emoji"_s);
    shadowForm->addRow(QString(), this->messageShadowEmotes_);

    auto *shadowColor = new QWidget;
    auto *shadowColorLayout = new QHBoxLayout(shadowColor);
    shadowColorLayout->setContentsMargins(0, 0, 0, 0);
    shadowColorLayout->setSpacing(5);
    this->messageShadowColorButton_ = new QPushButton;
    this->messageShadowColorButton_->setFixedWidth(34);
    this->messageShadowColorInput_ = new QLineEdit;
    this->messageShadowColorInput_->setMaxLength(9);
    this->messageShadowColorButton_->setToolTip(u"Shadow color."_s);
    this->messageShadowColorInput_->setToolTip(
        this->messageShadowColorButton_->toolTip());
    shadowColorLayout->addWidget(this->messageShadowColorButton_);
    shadowColorLayout->addWidget(this->messageShadowColorInput_, 1);
    shadowForm->addRow(u"Color:"_s, shadowColor);
    this->colorControls_.append(
        {this->messageShadowColorButton_,
         this->messageShadowColorInput_,
         &ThemeCustomizationProfile::messageShadowColor});
    this->messageShadowPosition_ = new ShadowPositionWidget;

    auto *positionRow = new QWidget;
    auto *positionLayout = new QHBoxLayout(positionRow);
    positionLayout->setContentsMargins(0, 0, 0, 0);
    positionLayout->setSpacing(8);
    auto *centerShadow = new QPushButton(u"Center shadow"_s);
    centerShadow->setToolTip(u"Reset the shadow position."_s);
    positionLayout->addWidget(this->messageShadowPosition_);
    positionLayout->addWidget(centerShadow, 0, Qt::AlignBottom);
    positionLayout->addStretch(1);
    shadowForm->addRow(u"Position:"_s, positionRow);
    shadowForm->addRow(u"Blur:"_s, makeSlider(0, 8, &this->messageShadowBlur_));
    shadowForm->addRow(
        u"Strength:"_s,
        makeSlider(0, 100, &this->messageShadowOpacity_, u"%"_s));
    optionsGrid->addWidget(chat, 0, 0);
    optionsGrid->addWidget(usernames, 0, 1);
    optionsGrid->addWidget(interface, 1, 0, 1, 2);
    optionsGrid->addWidget(shadow, 2, 0, 1, 2);
    layout->addLayout(optionsGrid);

    this->typographyPreview_ = new TypographyPreviewWidget;
    layout->addWidget(this->typographyPreview_);
    layout->addStretch(1);

    const auto update = [this] {
        if (!this->loadingControls_)
        {
            this->readControlsIntoProfile();
        }
    };
    const auto syncWeightForSelectedFont =
        [this](QComboBox *fontBox, QSlider *weightSlider, int index) {
            if (this->loadingControls_)
            {
                return;
            }
            const auto family = fontBox->itemData(index).toString();
            if (!family.isEmpty())
            {
                const auto weight = canonicalFontWeight(family);
                if (weight > 0)
                {
                    weightSlider->setValue(weight);
                }
            }
            this->readControlsIntoProfile();
        };
    QObject::connect(this->chatFont_, &QComboBox::currentIndexChanged, this,
                     [syncWeightForSelectedFont, this](int index) {
                         syncWeightForSelectedFont(
                             this->chatFont_, this->chatFontWeight_, index);
                     });
    QObject::connect(this->usernameFont_, &QComboBox::currentIndexChanged, this,
                     [syncWeightForSelectedFont, this](int index) {
                         syncWeightForSelectedFont(this->usernameFont_,
                                                   this->usernameFontWeight_,
                                                   index);
                     });
    QObject::connect(this->interfaceFont_, &QComboBox::currentIndexChanged,
                     this, update);
    QObject::connect(this->useThemeFonts_, &QCheckBox::toggled, this, update);
    QObject::connect(this->useThemeFontSizes_, &QCheckBox::toggled, this,
                     update);
    for (auto *slider :
         {this->chatFontSize_, this->chatFontWeight_, this->usernameFontSize_,
          this->usernameFontWeight_, this->messageShadowBlur_,
          this->messageShadowOpacity_, this->interfaceFontSize_})
    {
        QObject::connect(slider, &QSlider::valueChanged, this, update);
    }
    QObject::connect(this->messageShadow_, &QCheckBox::toggled, this, update);
    QObject::connect(this->messageShadowEmotes_, &QCheckBox::toggled, this,
                     update);
    QObject::connect(
        this->messageShadowColorButton_, &QPushButton::clicked, this, [this] {
            auto *picker =
                new ColorPickerDialog(this->profile_.messageShadowColor, this);
            QObject::connect(picker, &ColorPickerDialog::colorConfirmed, this,
                             [this, generation = this->profileGeneration_](
                                 const QColor color) {
                                 if (generation != this->profileGeneration_)
                                 {
                                     return;
                                 }
                                 this->profile_.messageShadowColor = color;
                                 this->loadProfileIntoControls();
                             });
            picker->show();
        });
    QObject::connect(this->messageShadowColorInput_,
                     &QLineEdit::editingFinished, this, [this] {
                         this->readControlsIntoProfile();
                         this->loadProfileIntoControls();
                     });
    this->messageShadowPosition_->positionChanged = [this](QPoint offset) {
        if (this->loadingControls_)
        {
            return;
        }
        this->profile_.messageShadowOffsetX = offset.x();
        this->profile_.messageShadowOffsetY = offset.y();
        this->updatePreview();
    };
    QObject::connect(centerShadow, &QPushButton::clicked, this, [this] {
        this->messageShadowPosition_->setOffset({0, 0});
        this->profile_.messageShadowOffsetX = 0;
        this->profile_.messageShadowOffsetY = 0;
        this->updatePreview();
    });
    return page;
}

QWidget *CustomizationPage::buildShareTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);
    auto *description =
        new QLabel(u"Import Moltorino or Bluzyrino themes, or share yours."_s);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto *share = new QGroupBox(u"Share this theme"_s);
    auto *shareForm = new QFormLayout(share);
    this->shareWallpaper_ = new QCheckBox(u"Include background"_s);
    this->shareWallpaper_->setToolTip(
        u"Uploads the background to kappa.lol for sharing."_s);
    shareForm->addRow(this->shareWallpaper_);
    this->shareExpiry_ = new QComboBox;
    this->shareExpiry_->addItem(u"Never"_s, QVariant());
    this->shareExpiry_->addItem(u"1 day"_s, 1);
    this->shareExpiry_->addItem(u"7 days"_s, 7);
    this->shareExpiry_->addItem(u"30 days"_s, 30);
    this->createCodeButton_ = new QPushButton(u"Create share code"_s);
    auto *expiryRow = new QWidget;
    auto *expiryLayout = new QHBoxLayout(expiryRow);
    expiryLayout->setContentsMargins(0, 0, 0, 0);
    expiryLayout->setSpacing(6);
    expiryLayout->addWidget(this->shareExpiry_);
    expiryLayout->addWidget(this->createCodeButton_);
    expiryLayout->addStretch(1);
    shareForm->addRow(u"Expires after:"_s, expiryRow);
    this->shareCode_ = new QLineEdit;
    this->shareCode_->setReadOnly(true);
    this->shareCode_->setPlaceholderText(u"Your share code"_s);
    auto *codeRow = new QWidget;
    auto *codeLayout = new QHBoxLayout(codeRow);
    codeLayout->setContentsMargins(0, 0, 0, 0);
    auto *copy = new QPushButton(u"Copy"_s);
    codeLayout->addWidget(this->shareCode_, 1);
    codeLayout->addWidget(copy);
    shareForm->addRow(u"Share code:"_s, codeRow);
    auto *exportFile = new QPushButton(u"Save theme file"_s);
    this->exportFileButton_ = exportFile;
    auto *fileRow = new QWidget;
    auto *fileLayout = new QHBoxLayout(fileRow);
    fileLayout->setContentsMargins(0, 0, 0, 0);
    fileLayout->addWidget(exportFile);
    fileLayout->addStretch(1);
    shareForm->addRow(u"Theme file:"_s, fileRow);
    layout->addWidget(share);

    auto *import = new QGroupBox(u"Import a theme"_s);
    auto *importForm = new QFormLayout(import);
    this->importCode_ = new QLineEdit;
    this->importCode_->setPlaceholderText(u"Paste a share code"_s);
    auto *importRow = new QWidget;
    auto *importLayout = new QHBoxLayout(importRow);
    importLayout->setContentsMargins(0, 0, 0, 0);
    this->importCodeButton_ = new QPushButton(u"Import code"_s);
    importLayout->addWidget(this->importCode_, 1);
    importLayout->addWidget(this->importCodeButton_);
    importForm->addRow(u"Share code:"_s, importRow);
    auto *importFile = new QPushButton(u"Open theme file"_s);
    this->importFileButton_ = importFile;
    auto *importFileRow = new QWidget;
    auto *importFileLayout = new QHBoxLayout(importFileRow);
    importFileLayout->setContentsMargins(0, 0, 0, 0);
    importFileLayout->addWidget(importFile);
    importFileLayout->addStretch(1);
    importForm->addRow(u"Theme file:"_s, importFileRow);
    layout->addWidget(import);
    layout->addStretch(1);

    QObject::connect(this->createCodeButton_, &QPushButton::clicked, this,
                     &CustomizationPage::createThemeCode);
    QObject::connect(this->importCodeButton_, &QPushButton::clicked, this,
                     &CustomizationPage::importThemeCode);
    QObject::connect(exportFile, &QPushButton::clicked, this,
                     &CustomizationPage::exportThemeFile);
    QObject::connect(importFile, &QPushButton::clicked, this,
                     &CustomizationPage::importThemeFile);
    QObject::connect(copy, &QPushButton::clicked, this, [this] {
        if (!this->shareCode_->text().isEmpty())
        {
            crossPlatformCopy(this->shareCode_->text());
        }
    });
    return page;
}

void CustomizationPage::onShow()
{
    if (this->selectedCustom_ && this->profile_ != this->savedProfile_)
    {
        return;
    }
    this->reloadProfiles(this->selectedKey_.isEmpty()
                             ? getTheme()->themeName.getValue()
                             : this->selectedKey_);
}

bool CustomizationPage::filterElements(const QString &query)
{
    return query.isEmpty() ||
           QStringLiteral(
               "customization themes colors palette wallpaper chat interface "
               "text typography fonts username weight tabs corners rounding "
               "shadow rows contrast timestamps system messages share code "
               "import export bluzyrino")
               .contains(query, Qt::CaseInsensitive) ||
           SettingsPage::filterElements(query);
}

void CustomizationPage::reloadProfiles(const QString &preferredKey)
{
    getTheme()->reloadAvailableThemes();
    this->profileBox_->blockSignals(true);
    this->profileBox_->clear();
    int selected = 0;
    int index = 0;
    for (const auto &descriptor : getTheme()->availableThemeDescriptors())
    {
        this->profileBox_->addItem(descriptor.name, descriptor.key);
        if (descriptor.key == preferredKey)
        {
            selected = index;
        }
        ++index;
    }
    this->profileBox_->setCurrentIndex(selected);
    this->profileBox_->blockSignals(false);
    this->loadSelectedProfile();
}

bool CustomizationPage::confirmProfileChange()
{
    if (!this->selectedCustom_ || this->profile_ == this->savedProfile_)
    {
        return true;
    }

    QMessageBox prompt(this);
    prompt.setIcon(QMessageBox::Question);
    prompt.setWindowTitle(u"Unsaved theme"_s);
    prompt.setText(
        QStringLiteral("Save your changes to %1?").arg(this->profile_.name));
    prompt.setStandardButtons(QMessageBox::Save | QMessageBox::Discard |
                              QMessageBox::Cancel);
    prompt.setDefaultButton(QMessageBox::Save);
    const auto generation = this->profileGeneration_;
    const auto choice = prompt.exec();
    if (choice == QMessageBox::Cancel || generation != this->profileGeneration_)
    {
        return false;
    }
    if (choice == QMessageBox::Save)
    {
        return this->saveCustomProfile(false);
    }
    return true;
}

void CustomizationPage::loadSelectedProfile()
{
    this->wallpaperPreparing_ = false;
    this->transferCancellation_.request_stop();
    this->transferCancellation_ = std::stop_source();
    this->wallpaperCancellation_.request_stop();
    this->setTransferBusy(false);
    this->setStatus({});
    const auto key = this->profileBox_->currentData().toString();
    for (const auto &descriptor : getTheme()->availableThemeDescriptors())
    {
        if (descriptor.key != key)
        {
            continue;
        }
        const auto json = getTheme()->themeJson(key);
        if (!json)
        {
            return;
        }
        ++this->profileGeneration_;
        this->shareCode_->clear();
        this->selectedKey_ = key;
        this->selectedPath_ = descriptor.path;
        this->selectedCustom_ = descriptor.custom;
        this->editedColorRoles_.clear();
        this->profile_ = profileFromTheme(descriptor, *json);
        this->shareWallpaper_->setChecked(false);
        this->savedProfile_ = this->profile_;
        this->loadProfileIntoControls();
        this->setStatus(this->selectedCustom_
                            ? QString()
                            : u"Choose Customize to edit a copy."_s);
        return;
    }
}

void CustomizationPage::loadProfileIntoControls()
{
    if (!this->profile_.separateChatText || !this->profile_.chatText.isValid())
    {
        this->profile_.chatText = this->profile_.text;
    }
    if (!this->profile_.systemText.isValid())
    {
        this->profile_.systemText = this->profile_.mutedText;
    }
    if (!this->profile_.timestampText.isValid())
    {
        this->profile_.timestampText = this->profile_.systemText;
    }
    this->loadingControls_ = true;
    this->chatTextFollowsText_->setChecked(!this->profile_.separateChatText);
    for (auto &control : this->colorControls_)
    {
        const auto color = this->profile_.*control.member;
        control.input->setText(color.name(
            color.alpha() == 255 ? QColor::HexRgb : QColor::HexArgb));
        control.button->setStyleSheet(
            QStringLiteral("background:%1; border:1px solid %2;")
                .arg(color.name(QColor::HexRgb),
                     this->profile_.mutedText.name(QColor::HexRgb)));
    }
    this->wallpaperPath_->setText(
        wallpaperDisplayName(this->profile_.wallpaperSource));
    this->wallpaperPath_->setToolTip(this->profile_.wallpaperSource);
    this->wallpaperMode_->setCurrentIndex(int(this->profile_.wallpaperMode));
    this->wallpaperOpacity_->setValue(this->profile_.wallpaperOpacity);
    this->wallpaperOverlayOpacity_->setValue(
        this->profile_.wallpaperOverlayOpacity);
    this->wallpaperBlur_->setValue(this->profile_.wallpaperBlur);
    this->wallpaperZoom_->setValue(this->profile_.wallpaperZoom);
    this->wallpaperFocus_->setProfile(this->profile_);
    this->alternateMessageRows_->setChecked(
        this->profile_.alternateMessageRows);
    this->alternateMessageOpacity_->setValue(
        this->profile_.alternateMessageOpacity);
    this->alternateMessageContrast_->setValue(
        this->profile_.alternateMessageContrast);
    this->highlightOpacityAdjustment_->setValue(
        this->profile_.highlightOpacityAdjustment);
    this->roundChat_->setChecked(this->profile_.roundChat);
    this->chatCornerRadius_->setValue(this->profile_.chatCornerRadius);
    this->chatBorder_->setChecked(this->profile_.chatBorder);
    this->chatBorderWidth_->setValue(this->profile_.chatBorderWidth);
    this->chatBorderOpacity_->setValue(this->profile_.chatBorderOpacity);
    this->messageShadow_->setChecked(this->profile_.messageShadow);
    this->messageShadowEmotes_->setChecked(this->profile_.messageShadowEmotes);
    this->messageShadowOpacity_->setValue(this->profile_.messageShadowOpacity);
    this->messageShadowPosition_->setOffset(
        this->profile_.messageShadowOffset());
    this->messageShadowPosition_->setShadowColor(
        this->profile_.messageShadowColor);
    this->messageShadowBlur_->setValue(this->profile_.messageShadowBlur);
    this->foundation_->setCurrentIndex(int(this->profile_.foundation));
    this->panelContrast_->setValue(this->profile_.panelContrast);
    this->accentStrength_->setCurrentIndex(int(this->profile_.accentStrength));
    this->tabShape_->setCurrentIndex(int(this->profile_.tabShape));
    this->tabCornerRadius_->setValue(this->profile_.tabCornerRadius);
    this->tabSpacing_->setValue(this->profile_.tabSpacing);
    this->inactiveTabContrast_->setValue(this->profile_.inactiveTabContrast);
    this->channelBarContrast_->setValue(this->profile_.channelBarContrast);
    const auto selectFont = [](QComboBox *box, const QString &family) {
        auto index = family.isEmpty() ? 0 : box->findData(family);
        if (index < 0)
        {
            box->addItem(family, family);
            index = box->count() - 1;
        }
        box->setCurrentIndex(index);
    };
    this->useThemeFonts_->setChecked(this->profile_.useThemeFonts);
    this->useThemeFontSizes_->setChecked(this->profile_.useThemeFontSizes);
    selectFont(this->chatFont_, this->profile_.chatFontFamily);
    this->chatFontSize_->setValue(this->profile_.chatFontSize > 0
                                      ? this->profile_.chatFontSize
                                      : getSettings()->chatFontSize.getValue());
    this->chatFontWeight_->setValue(
        this->profile_.chatFontWeight > 0
            ? this->profile_.chatFontWeight
            : getSettings()->chatFontWeight.getValue());
    selectFont(this->usernameFont_, this->profile_.usernameFontFamily);
    this->usernameFontSize_->setValue(
        this->profile_.usernameFontSize > 0
            ? this->profile_.usernameFontSize
            : (this->profile_.chatFontSize > 0
                   ? this->profile_.chatFontSize
                   : getSettings()->chatFontSize.getValue()));
    this->usernameFontWeight_->setValue(this->profile_.usernameFontWeight > 0
                                            ? this->profile_.usernameFontWeight
                                            : getUsernameBoldness());
    selectFont(this->interfaceFont_, this->profile_.interfaceFontFamily);
    this->interfaceFontSize_->setValue(this->profile_.interfaceFontSize > 0
                                           ? this->profile_.interfaceFontSize
                                           : 9);
    this->loadingControls_ = false;
    this->updatePreview();
}

void CustomizationPage::readControlsIntoProfile()
{
    if (this->loadingControls_)
    {
        return;
    }
    for (auto &control : this->colorControls_)
    {
        const QColor color(control.input->text().trimmed());
        if (color.isValid())
        {
            this->profile_.*control.member = color;
        }
    }
    this->profile_.separateChatText = !this->chatTextFollowsText_->isChecked();
    if (!this->profile_.separateChatText)
    {
        this->profile_.chatText = this->profile_.text;
    }
    this->profile_.wallpaperMode =
        ThemeWallpaperMode(this->wallpaperMode_->currentIndex());
    this->profile_.wallpaperOpacity = this->wallpaperOpacity_->value();
    this->profile_.wallpaperOverlayOpacity =
        this->wallpaperOverlayOpacity_->value();
    this->profile_.wallpaperBlur = this->wallpaperBlur_->value();
    this->profile_.wallpaperZoom = this->wallpaperZoom_->value();
    this->profile_.useThemeMessageRows = true;
    this->profile_.alternateMessageRows =
        this->alternateMessageRows_->isChecked();
    this->profile_.alternateMessageOpacity =
        this->alternateMessageOpacity_->value();
    this->profile_.alternateMessageContrast =
        this->alternateMessageContrast_->value();
    this->profile_.highlightOpacityAdjustment =
        this->highlightOpacityAdjustment_->value();
    this->profile_.roundChat = this->roundChat_->isChecked();
    this->profile_.chatCornerRadius = this->chatCornerRadius_->value();
    this->profile_.chatBorder = this->chatBorder_->isChecked();
    this->profile_.chatBorderWidth = this->chatBorderWidth_->value();
    this->profile_.chatBorderOpacity = this->chatBorderOpacity_->value();
    this->profile_.messageShadow = this->messageShadow_->isChecked();
    this->profile_.messageShadowEmotes =
        this->messageShadowEmotes_->isChecked();
    this->profile_.messageShadowOpacity = this->messageShadowOpacity_->value();
    const auto shadowOffset = this->messageShadowPosition_->offset();
    this->profile_.messageShadowOffsetX = shadowOffset.x();
    this->profile_.messageShadowOffsetY = shadowOffset.y();
    this->profile_.messageShadowBlur = this->messageShadowBlur_->value();
    this->profile_.foundation =
        ThemeFoundation(this->foundation_->currentIndex());
    this->profile_.panelContrast = this->panelContrast_->value();
    this->profile_.surfaceDepth =
        this->profile_.panelContrast < 34   ? ThemeSurfaceDepth::Flat
        : this->profile_.panelContrast > 66 ? ThemeSurfaceDepth::Layered
                                            : ThemeSurfaceDepth::Balanced;
    this->profile_.accentStrength =
        ThemeAccentStrength(this->accentStrength_->currentIndex());
    this->profile_.tabShape = ThemeTabShape(this->tabShape_->currentIndex());
    this->profile_.tabCornerRadius = this->tabCornerRadius_->value();
    this->profile_.tabSpacing = this->tabSpacing_->value();
    this->profile_.inactiveTabContrast = this->inactiveTabContrast_->value();
    this->profile_.channelBarContrast = this->channelBarContrast_->value();
    this->profile_.useThemeFonts = this->useThemeFonts_->isChecked();
    this->profile_.useThemeFontSizes = this->useThemeFontSizes_->isChecked();
    this->profile_.chatFontFamily = this->chatFont_->currentData().toString();
    this->profile_.chatFontSize = this->chatFontSize_->value();
    this->profile_.chatFontWeight = this->chatFontWeight_->value();
    this->profile_.usernameFontFamily =
        this->usernameFont_->currentData().toString();
    this->profile_.usernameFontSize = this->usernameFontSize_->value();
    this->profile_.usernameFontWeight = this->usernameFontWeight_->value();
    this->profile_.interfaceFontFamily =
        this->interfaceFont_->currentData().toString();
    this->profile_.interfaceFontSize = this->interfaceFontSize_->value();
    this->updatePreview();
}

void CustomizationPage::updatePreview()
{
    this->preview_->setProfile(this->profile_);
    this->wallpaperFocus_->setProfile(this->profile_);
    this->typographyPreview_->setProfile(this->profile_);
    this->updateReadability();
    this->updateControlState();
}

void CustomizationPage::updateReadability()
{
    const double chat = colorContrastRatio(this->profile_.effectiveChatText(),
                                           this->profile_.chatBackground);
    const double surface =
        colorContrastRatio(this->profile_.text, this->profile_.surface);
    const double raised =
        colorContrastRatio(this->profile_.text, this->profile_.raisedSurface);
    const double background =
        colorContrastRatio(this->profile_.text, this->profile_.background);
    const double interfaceContrast = std::min({background, surface, raised});
    const double accentContrast = std::min(
        {colorContrastRatio(this->profile_.accent, this->profile_.background),
         colorContrastRatio(this->profile_.accent, this->profile_.surface),
         colorContrastRatio(this->profile_.accent,
                            this->profile_.raisedSurface)});
    if (chat < 3.0 || interfaceContrast < 3.0)
    {
        this->readability_->setText(
            chat < 3.0 && interfaceContrast < 3.0
                ? u"Low text contrast in chat and the interface."_s
            : chat < 3.0 ? u"Low chat text contrast."_s
                         : u"Low interface text contrast."_s);
        this->readability_->setStyleSheet(u"color:#ff9b63;"_s);
    }
    else if (accentContrast < 3.0)
    {
        this->readability_->setText(u"Low accent contrast."_s);
        this->readability_->setStyleSheet(u"color:#ff9b63;"_s);
    }
    else if (chat < 4.5 || interfaceContrast < 4.5)
    {
        this->readability_->setText(
            chat < 4.5 && interfaceContrast < 4.5
                ? u"Text contrast could be higher."_s
            : chat < 4.5 ? u"Chat text could use more contrast."_s
                         : u"Interface text could use more contrast."_s);
        this->readability_->setStyleSheet(u"color:#e1be64;"_s);
    }
    else
    {
        this->readability_->setText(u"Text contrast looks good."_s);
        this->readability_->setStyleSheet(u"color:#73ca92;"_s);
    }
}

void CustomizationPage::updateControlState()
{
    this->cancelWallpaperButton_->setVisible(this->wallpaperPreparing_);
    this->profileKind_->setText(this->selectedCustom_ ? u"Custom"_s
                                                      : u"Included"_s);
    this->duplicateButton_->setText(this->selectedCustom_ ? u"Duplicate"_s
                                                          : u"Customize"_s);
    this->duplicateButton_->setToolTip(this->selectedCustom_
                                           ? u"Make a copy of this theme."_s
                                           : u"Make an editable copy."_s);
    this->duplicateButton_->setEnabled(!this->transferBusy_);
    this->renameButton_->setEnabled(this->selectedCustom_ &&
                                    !this->transferBusy_);
    this->deleteButton_->setEnabled(this->selectedCustom_ &&
                                    !this->transferBusy_);
    const bool dirty =
        this->selectedCustom_ && this->profile_ != this->savedProfile_;
    this->saveButton_->setEnabled(dirty && !this->transferBusy_);
    this->resetButton_->setEnabled(dirty && !this->transferBusy_);
    this->useButton_->setEnabled(!this->transferBusy_ &&
                                 this->selectedKey_ !=
                                     getTheme()->themeName.getValue());
    const bool editable = this->selectedCustom_ && !this->transferBusy_;
    this->colorsEditor_->setEnabled(editable);
    this->chatEditor_->setEnabled(editable);
    this->interfaceEditor_->setEnabled(editable);
    this->typographyEditor_->setEnabled(editable);
    for (auto &control : this->colorControls_)
    {
        control.button->setEnabled(editable);
        control.input->setEnabled(editable);
    }
    this->chatTextFollowsText_->setEnabled(editable);
    for (auto *widget : {static_cast<QWidget *>(this->chatFont_),
                         static_cast<QWidget *>(this->usernameFont_),
                         static_cast<QWidget *>(this->interfaceFont_),
                         static_cast<QWidget *>(this->chatFontWeight_),
                         static_cast<QWidget *>(this->usernameFontWeight_)})
    {
        widget->setEnabled(editable && this->profile_.useThemeFonts);
    }
    for (auto *slider : {this->chatFontSize_, this->usernameFontSize_,
                         this->interfaceFontSize_})
    {
        slider->setEnabled(editable && this->profile_.useThemeFontSizes);
    }
    const bool wallpaperEditable = editable && this->profile_.hasWallpaper();
    for (auto *widget :
         {static_cast<QWidget *>(this->wallpaperMode_),
          static_cast<QWidget *>(this->wallpaperZoom_),
          static_cast<QWidget *>(this->wallpaperOpacity_),
          static_cast<QWidget *>(this->wallpaperOverlayColorButton_),
          static_cast<QWidget *>(this->wallpaperOverlayColorInput_),
          static_cast<QWidget *>(this->wallpaperOverlayOpacity_),
          static_cast<QWidget *>(this->wallpaperBlur_)})
    {
        widget->setEnabled(wallpaperEditable);
    }
    const bool focusEditable =
        wallpaperEditable &&
        this->profile_.wallpaperMode != ThemeWallpaperMode::Tile &&
        (this->profile_.wallpaperMode == ThemeWallpaperMode::Fill ||
         this->profile_.wallpaperZoom > 100);
    this->wallpaperFocus_->setEnabled(focusEditable);
    this->wallpaperCenterFocus_->setEnabled(focusEditable);
    this->messageShadowOpacity_->setEnabled(editable &&
                                            this->profile_.messageShadow);
    this->messageShadowEmotes_->setEnabled(editable &&
                                           this->profile_.messageShadow);
    for (auto *widget :
         {static_cast<QWidget *>(this->messageShadowColorButton_),
          static_cast<QWidget *>(this->messageShadowColorInput_),
          static_cast<QWidget *>(this->messageShadowPosition_),
          static_cast<QWidget *>(this->messageShadowBlur_)})
    {
        widget->setEnabled(editable && this->profile_.messageShadow);
    }
    this->chatCornerRadius_->setEnabled(editable && this->profile_.roundChat);
    const bool borderEditable =
        editable && this->profile_.roundChat && this->profile_.chatBorder;
    this->chatBorder_->setEnabled(editable && this->profile_.roundChat);
    this->chatBorderColorButton_->setEnabled(borderEditable);
    this->chatBorderColorInput_->setEnabled(borderEditable);
    this->chatBorderWidth_->setEnabled(borderEditable);
    this->chatBorderOpacity_->setEnabled(borderEditable);
    this->alternateMessageOpacity_->setEnabled(
        editable && this->profile_.alternateMessageRows);
    this->alternateMessageContrast_->setEnabled(
        editable && this->profile_.alternateMessageRows);
    this->tabSpacing_->setEnabled(editable && this->profile_.tabShape ==
                                                  ThemeTabShape::Individual);
}

void CustomizationPage::applySelectedTheme()
{
    getTheme()->selectTheme(this->selectedKey_);
    getApp()->getWindows()->forceLayoutChannelViews();
    this->updateControlState();
    this->setStatus(u"Theme applied."_s);
}

QString CustomizationPage::uniqueThemePath(const QString &name) const
{
    const auto base = safeFileBase(name);
    QDir directory(getTheme()->themesDirectory());
    for (int suffix = 0; suffix < 1000; ++suffix)
    {
        const auto file =
            suffix == 0
                ? base + u".json"_s
                : QStringLiteral("%1-%2.json").arg(base).arg(suffix + 1);
        const auto path = directory.filePath(file);
        if (!QFileInfo::exists(path))
        {
            return path;
        }
    }
    QString path;
    do
    {
        path = directory.filePath(
            base + u'-' + QUuid::createUuid().toString(QUuid::WithoutBraces) +
            u".json"_s);
    } while (QFileInfo::exists(path));
    return path;
}

QString CustomizationPage::managedWallpaperSource(const QString &source,
                                                  QString *error) const
{
    if (source.isEmpty() || source.startsWith(u":/"_s))
    {
        return source;
    }

    const QFileInfo sourceInfo(source);
    if (!sourceInfo.isFile() || sourceInfo.size() > 50LL * 1024 * 1024)
    {
        *error = u"Choose a background file up to 50 MiB."_s;
        return {};
    }

    QDir themes(getTheme()->themesDirectory());
    const auto directoryPath = themes.filePath(u"Wallpapers"_s);
    if (!QDir().mkpath(directoryPath))
    {
        *error = u"Moltorino couldn't create its wallpaper folder."_s;
        return {};
    }
    const auto managedRoot = normalizedLocalPath(directoryPath) + u'/';
    const auto absoluteSource = QDir::fromNativeSeparators(
        QDir::cleanPath(sourceInfo.absoluteFilePath()));
    if (loadThemeWallpaper(absoluteSource, 256).isNull())
    {
        *error = u"Moltorino couldn't read that wallpaper."_s;
        return {};
    }
    const auto resolvedSource = resolvedLocalPath(absoluteSource);
    if (normalizedLocalPath(resolvedSource).startsWith(managedRoot))
    {
        return resolvedSource;
    }

    const auto fingerprint =
        QStringLiteral("%1\n%2\n%3")
            .arg(absoluteSource)
            .arg(sourceInfo.size())
            .arg(sourceInfo.lastModified().toMSecsSinceEpoch())
            .toUtf8();
    const auto digest =
        QCryptographicHash::hash(fingerprint, QCryptographicHash::Sha256)
            .toHex()
            .left(16);
    auto suffix = sourceInfo.suffix().toLower();
    if (suffix.isEmpty())
    {
        suffix = u"png"_s;
    }
    const auto destination =
        QDir(directoryPath)
            .filePath(QStringLiteral("wallpaper-%1.%2")
                          .arg(QString::fromLatin1(digest), suffix));
    if (!QFileInfo::exists(destination) &&
        !QFile::copy(absoluteSource, destination))
    {
        *error = u"Moltorino couldn't keep a copy of that wallpaper."_s;
        return {};
    }
    return destination;
}

void CustomizationPage::cleanupManagedWallpapers() const
{
    const QDir themes(getTheme()->themesDirectory());
    QDir wallpapers(themes.filePath(u"Wallpapers"_s));
    if (!wallpapers.exists())
    {
        return;
    }

    QSet<QString> referenced;
    const auto themeFiles = themes.entryInfoList(
        {u"*.json"_s}, QDir::Files | QDir::Hidden, QDir::Name);
    for (const auto &themeInfo : themeFiles)
    {
        QFile file(themeInfo.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly))
        {
            return;
        }
        const auto bytes = file.read(32 * 1024 * 1024 + 1);
        if (bytes.size() > 32 * 1024 * 1024 || !file.atEnd() ||
            file.error() != QFileDevice::NoError)
        {
            return;
        }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(bytes, &parseError);
        if (parseError.error != QJsonParseError::NoError ||
            !document.isObject())
        {
            return;
        }
        const auto root = document.object();
        const auto source = root.value(u"metadata"_s)
                                .toObject()
                                .value(u"moltorino"_s)
                                .toObject()
                                .value(u"wallpaper"_s)
                                .toObject()
                                .value(u"source"_s)
                                .toString();
        if (!source.isEmpty() && !source.startsWith(u":/"_s))
        {
            referenced.insert(normalizedLocalPath(source));
        }
    }

    if (!this->profile_.wallpaperSource.isEmpty() &&
        !this->profile_.wallpaperSource.startsWith(u":/"_s))
    {
        referenced.insert(normalizedLocalPath(this->profile_.wallpaperSource));
    }
    const QRegularExpression managedName(
        uR"(^wallpaper-[0-9a-f]{16}\.[a-z0-9]+$)"_s,
        QRegularExpression::CaseInsensitiveOption);
    const auto files =
        wallpapers.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name);
    for (const auto &file : files)
    {
        if (managedName.match(file.fileName()).hasMatch() &&
            !referenced.contains(normalizedLocalPath(file.absoluteFilePath())))
        {
            QFile::remove(file.absoluteFilePath());
        }
    }
}

void CustomizationPage::duplicateProfile()
{
    const auto generation = this->profileGeneration_;
    const auto name = promptForThemeName(
        this,
        this->selectedCustom_ ? u"Duplicate theme"_s : u"Customize theme"_s,
        u"New theme name:"_s, this->profile_.name + u" Copy"_s);
    if (!name || name->trimmed().isEmpty() ||
        generation != this->profileGeneration_)
    {
        return;
    }
    auto copy = this->profile_;
    copy.name = name->trimmed();
    if (!copy.useThemeMessageRows)
    {
        copy.useThemeMessageRows = true;
        copy.alternateMessageRows = getSettings()->alternateMessages.getValue();
    }
    if (copy.chatFontFamily.isEmpty())
    {
        copy.chatFontFamily = getSettings()->chatFontFamily.getValue();
    }
    if (copy.chatFontSize <= 0)
    {
        copy.chatFontSize = getSettings()->chatFontSize.getValue();
    }
    if (copy.chatFontWeight <= 0)
    {
        copy.chatFontWeight = getSettings()->chatFontWeight.getValue();
    }
    if (copy.usernameFontFamily.isEmpty())
    {
        copy.usernameFontFamily = copy.chatFontFamily;
    }
    if (copy.usernameFontSize <= 0)
    {
        copy.usernameFontSize = copy.chatFontSize;
    }
    if (copy.usernameFontWeight <= 0)
    {
        copy.usernameFontWeight = getUsernameBoldness();
    }
    if (copy.interfaceFontFamily.isEmpty())
    {
        copy.interfaceFontFamily = QStringLiteral(DEFAULT_FONT_FAMILY);
    }
    if (copy.interfaceFontSize <= 0)
    {
        copy.interfaceFontSize = 9;
    }
    const auto path = this->uniqueThemePath(copy.name);
    QSaveFile file(path);
    const auto contents = QJsonDocument(buildCustomizedTheme(copy))
                              .toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(contents) != contents.size() || !file.commit())
    {
        QMessageBox::warning(this, u"Couldn't create theme"_s,
                             u"Moltorino couldn't save the new theme file."_s);
        return;
    }
    this->reloadProfiles(QFileInfo(path).fileName());
    this->setStatus(u"Your editable theme is ready."_s);
}

void CustomizationPage::renameProfile()
{
    if (!this->selectedCustom_)
    {
        return;
    }
    const auto generation = this->profileGeneration_;
    const auto name = promptForThemeName(this, u"Rename theme"_s, u"Name:"_s,
                                         this->profile_.name);
    if (!name || name->trimmed().isEmpty() ||
        generation != this->profileGeneration_)
    {
        return;
    }
    this->profile_.name = name->trimmed();
    if (this->saveCustomProfile(false))
    {
        this->reloadProfiles(this->selectedKey_);
    }
}

void CustomizationPage::deleteProfile()
{
    if (!this->selectedCustom_)
    {
        return;
    }
    const auto generation = this->profileGeneration_;
    if (QMessageBox::question(
            this, u"Delete theme"_s,
            QStringLiteral("Delete %1? This cannot be undone.")
                .arg(this->profile_.name)) != QMessageBox::Yes ||
        generation != this->profileGeneration_)
    {
        return;
    }
    const bool active = getTheme()->themeName.getValue() == this->selectedKey_;
    if (!QFile::remove(this->selectedPath_))
    {
        QMessageBox::warning(this, u"Couldn't delete theme"_s,
                             u"Moltorino couldn't remove that theme file."_s);
        return;
    }
    if (active)
    {
        getTheme()->themeName.setValue(u"Moltorino Midnight"_s);
    }
    this->reloadProfiles(getTheme()->themeName.getValue());
    this->cleanupManagedWallpapers();
    this->setStatus(u"Theme deleted."_s);
}

bool CustomizationPage::saveCustomProfile(bool apply)
{
    if (this->wallpaperPreparing_)
    {
        return false;
    }
    if (!this->selectedCustom_)
    {
        return false;
    }
    this->readControlsIntoProfile();
    QString wallpaperError;
    const auto managedWallpaper = this->managedWallpaperSource(
        this->profile_.wallpaperSource, &wallpaperError);
    if (!wallpaperError.isEmpty())
    {
        QMessageBox::warning(this, u"Couldn't save theme"_s, wallpaperError);
        return false;
    }
    this->profile_.wallpaperSource = managedWallpaper;
    QSaveFile file(this->selectedPath_);
    const auto contents = QJsonDocument(buildCustomizedTheme(this->profile_))
                              .toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(contents) != contents.size() || !file.commit())
    {
        QMessageBox::warning(this, u"Couldn't save theme"_s,
                             u"Moltorino couldn't write that theme file."_s);
        return false;
    }
    this->savedProfile_ = this->profile_;
    this->cleanupManagedWallpapers();
    this->loadProfileIntoControls();
    getTheme()->reloadAvailableThemes();
    const bool active = getTheme()->themeName.getValue() == this->selectedKey_;
    if (active)
    {
        getTheme()->update();
    }
    if (apply)
    {
        if (!active)
        {
            getTheme()->themeName.setValue(this->selectedKey_);
        }
        getApp()->getWindows()->forceLayoutChannelViews();
        this->setStatus(u"Theme saved and applied."_s);
    }
    else
    {
        this->setStatus(u"Theme saved."_s);
    }
    return true;
}

void CustomizationPage::resetDraft()
{
    ++this->profileGeneration_;
    this->profile_ = this->savedProfile_;
    this->editedColorRoles_.clear();
    this->loadProfileIntoControls();
    this->setStatus(u"Changes reverted."_s);
}

void CustomizationPage::markColorEdited(const QString &role)
{
    if (role.isEmpty())
    {
        return;
    }
    if (role == u"chatText"_s)
    {
        this->profile_.separateChatText = true;
        const QSignalBlocker blocker(this->chatTextFollowsText_);
        this->chatTextFollowsText_->setChecked(false);
    }
    this->editedColorRoles_.insert(role);
}

void CustomizationPage::buildPalette()
{
    if (!this->selectedCustom_)
    {
        return;
    }

    this->readControlsIntoProfile();

    const auto generation = this->profileGeneration_;
    const auto originalProfile = this->profile_;
    QDialog dialog(this);
    dialog.setWindowTitle(u"Build a color palette"_s);
    dialog.setModal(true);
    dialog.setMinimumWidth(430);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *intro =
        new QLabel(u"Keep the checked colors and rebuild the rest."_s, &dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *styleRow = new QHBoxLayout;
    styleRow->addWidget(new QLabel(u"Palette style:"_s, &dialog));
    auto *style = new QComboBox(&dialog);
    style->addItem(u"Dark"_s, int(ThemePaletteMode::Dark));
    style->addItem(u"Light"_s, int(ThemePaletteMode::Light));
    const bool preferLight =
        colorContrastRatio(Qt::black, this->profile_.background) >=
        colorContrastRatio(Qt::white, this->profile_.background);
    style->setCurrentIndex(preferLight ? 1 : 0);
    style->setToolTip(
        u"Style for the new colors. Kept colors stay unchanged."_s);
    styleRow->addWidget(style, 1);
    layout->addLayout(styleRow);

    auto *keptColors = new QGroupBox(u"Keep these colors"_s, &dialog);
    auto *keptGrid = new QGridLayout(keptColors);
    keptGrid->setHorizontalSpacing(18);
    keptGrid->setVerticalSpacing(5);
    struct KeepChoice {
        QString role;
        QCheckBox *box;
        bool ThemePaletteSelection::*member;
    };
    QVector<KeepChoice> choices;
    const struct KeepDefinition {
        const char *role;
        const char *label;
        bool ThemePaletteSelection::*member;
    } definitions[] = {
        {"background", "Background", &ThemePaletteSelection::background},
        {"chatBackground", "Chat background",
         &ThemePaletteSelection::chatBackground},
        {"surface", "Surface", &ThemePaletteSelection::surface},
        {"raisedSurface", "Raised surface",
         &ThemePaletteSelection::raisedSurface},
        {"accent", "Accent", &ThemePaletteSelection::accent},
    };
    bool hasEditedColors = false;
    for (const auto &definition : definitions)
    {
        if (this->editedColorRoles_.contains(
                QString::fromLatin1(definition.role)))
        {
            hasEditedColors = true;
            break;
        }
    }
    for (int index = 0; index < int(std::size(definitions)); ++index)
    {
        const auto &definition = definitions[index];
        const auto role = QString::fromLatin1(definition.role);
        auto *box =
            new QCheckBox(QString::fromLatin1(definition.label), keptColors);
        box->setChecked(hasEditedColors ? this->editedColorRoles_.contains(role)
                                        : role == u"background"_s);
        keptGrid->addWidget(box, index / 2, index % 2);
        choices.append({role, box, definition.member});
    }
    layout->addWidget(keptColors);

    auto *note = new QLabel(u"Preview first, then save or revert."_s, &dialog);
    note->setObjectName(u"description"_s);
    note->setWordWrap(true);
    layout->addWidget(note);

    auto *problem = new QLabel(&dialog);
    problem->setWordWrap(true);
    problem->setStyleSheet(u"color:#ff9b63;"_s);
    problem->hide();
    layout->addWidget(problem);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto *cancel = new QPushButton(u"Cancel"_s, &dialog);
    auto *build = new QPushButton(u"Build palette"_s, &dialog);
    build->setDefault(true);
    buttons->addWidget(cancel);
    buttons->addWidget(build);
    layout->addLayout(buttons);
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    std::optional<ThemeCustomizationProfile> builtPalette;
    QSet<QString> keptRoles;
    QObject::connect(build, &QPushButton::clicked, &dialog, [&] {
        ThemePaletteSelection selection;
        keptRoles.clear();
        for (const auto &choice : choices)
        {
            if (!choice.box->isChecked())
            {
                continue;
            }
            selection.*choice.member = true;
            keptRoles.insert(choice.role);
        }
        if (!selection.any())
        {
            problem->setText(u"Choose at least one color to keep."_s);
            problem->show();
            return;
        }
        const auto mode = ThemePaletteMode(style->currentData().toInt());
        builtPalette = buildThemePalette(originalProfile, selection, mode);
        if (!builtPalette)
        {
            problem->setText(u"Choose at least one valid color to keep."_s);
            problem->show();
            return;
        }
        dialog.accept();
    });

    if (dialog.exec() != QDialog::Accepted ||
        generation != this->profileGeneration_)
    {
        return;
    }

    this->profile_ = *builtPalette;
    this->editedColorRoles_ = keptRoles;
    this->loadProfileIntoControls();
    this->setStatus(keptRoles.size() == 1
                        ? u"Palette rebuilt. Kept one color."_s
                        : QStringLiteral("Palette rebuilt. Kept %1 colors.")
                              .arg(keptRoles.size()));
}

void CustomizationPage::chooseWallpaper()
{
    const auto generation = this->profileGeneration_;
    const auto path = QFileDialog::getOpenFileName(
        this, u"Choose an image or video"_s, {}, themeWallpaperFileFilter());
    if (!path.isEmpty() && generation == this->profileGeneration_)
    {
        this->prepareWallpaper(path);
    }
}

std::optional<int> CustomizationPage::chooseVideoLength(double duration)
{
    if (duration <= 60)
    {
        return 0;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(u"Background length"_s);
    auto *layout = new QVBoxLayout(&dialog);
    auto *description =
        new QLabel(u"Shorter loops prepare faster and use less storage."_s);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *length = new QComboBox;
    length->addItem(u"First 15 seconds"_s, 15);
    length->addItem(u"First 30 seconds"_s, 30);
    length->addItem(u"First 45 seconds"_s, 45);
    length->addItem(u"First minute"_s, 60);
    length->addItem(u"Full clip"_s, 0);
    length->setCurrentIndex(1);
    auto *form = new QFormLayout;
    form->addRow(u"Use:"_s, length);
    layout->addLayout(form);
    if (duration > THEME_VIDEO_DURATION_MS / 1000)
    {
        if (auto *model = qobject_cast<QStandardItemModel *>(length->model()))
        {
            model->item(4)->setEnabled(false);
        }
        auto *limit =
            new QLabel(u"Over 2 minutes long. Choose a shorter loop."_s);
        limit->setWordWrap(true);
        layout->addWidget(limit);
    }
    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(u"Use background"_s);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog,
                     &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog,
                     &QDialog::reject);
    layout->addWidget(buttons);
    dialog.setMinimumWidth(380);
    if (dialog.exec() != QDialog::Accepted)
    {
        return std::nullopt;
    }
    return length->currentData().toInt();
}

void CustomizationPage::prepareWallpaper(const QString &path)
{
    const auto generation = this->profileGeneration_;
    const auto wallpaperGeneration = ++this->wallpaperGeneration_;
    this->wallpaperCancellation_.request_stop();
    this->wallpaperCancellation_ = std::stop_source();
    const auto cancellation = this->wallpaperCancellation_.get_token();
    this->wallpaperPreparing_ = true;
    this->setTransferBusy(true);
    const auto directory = getTheme()->themesDirectory();
    const auto current = [this, generation, wallpaperGeneration, cancellation] {
        return !cancellation.stop_requested() &&
               generation == this->profileGeneration_ &&
               wallpaperGeneration == this->wallpaperGeneration_;
    };
    const auto prepare = [this, path, directory, cancellation,
                          current](int seconds) {
        this->setStatus(u"Preparing background"_s, false, true);
        (void)QtConcurrent::run([path, cancellation, directory, seconds] {
            auto data = seconds > 0
                            ? prepareThemeVideo(path, cancellation, seconds)
                            : prepareThemeWallpaper(path, cancellation);
            return storePreparedWallpaper(std::move(data), directory,
                                          cancellation);
        }).then(this, [this, current](StoredWallpaper prepared) {
            if (!current())
            {
                return;
            }
            this->wallpaperPreparing_ = false;
            this->setTransferBusy(false);
            const auto &source = prepared.source;
            if (source.isEmpty())
            {
                this->setStatus(prepared.data.error, true);
                return;
            }
            this->profile_.wallpaperSource = source;
            this->profile_.wallpaperId.clear();
            this->wallpaperPath_->setText(wallpaperDisplayName(source));
            this->wallpaperPath_->setToolTip(source);
            this->updatePreview();
            this->setStatus({});
        });
    };
    if (!isThemeVideoInput(path))
    {
        prepare(0);
        return;
    }
    this->setStatus(u"Reading background"_s, false, true);
    (void)QtConcurrent::run([path, cancellation] {
        return inspectThemeVideo(path, cancellation);
    }).then(this, [this, current, prepare](ThemeVideoInfo info) {
        if (!current())
        {
            return;
        }
        if (!info.error.isEmpty())
        {
            this->wallpaperPreparing_ = false;
            this->setTransferBusy(false);
            this->setStatus(info.error, true);
            return;
        }
        this->setStatus({});
        const auto seconds = this->chooseVideoLength(info.duration);

        if (!current())
        {
            return;
        }
        if (!seconds)
        {
            this->wallpaperPreparing_ = false;
            this->setTransferBusy(false);
            this->setStatus(u"Background preparation cancelled"_s);
            return;
        }
        prepare(*seconds);
    });
}

void CustomizationPage::clearWallpaper()
{
    this->wallpaperCancellation_.request_stop();
    ++this->wallpaperGeneration_;
    if (this->wallpaperPreparing_)
    {
        this->wallpaperPreparing_ = false;
        this->setTransferBusy(false);
    }
    this->profile_.wallpaperSource.clear();
    this->profile_.wallpaperId.clear();
    this->wallpaperPath_->clear();
    this->wallpaperPath_->setToolTip({});
    this->setStatus({});
    this->updatePreview();
}

void CustomizationPage::suggestWallpaperPalette()
{
    if (!this->profile_.hasWallpaper())
    {
        this->setStatus(u"Choose a wallpaper first."_s, true);
        return;
    }
    QImage image = this->preview_->wallpaperFrame();
    if (image.isNull())
    {
        this->setStatus(u"Moltorino couldn't sample that background."_s, true);
        return;
    }
    image = image.scaled(80, 80, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    qint64 red = 0, green = 0, blue = 0, count = 0;
    QColor accent = this->profile_.accent;
    qreal bestAccent = -1;
    for (int y = 0; y < image.height(); ++y)
    {
        for (int x = 0; x < image.width(); ++x)
        {
            const QColor color = image.pixelColor(x, y);
            red += color.red();
            green += color.green();
            blue += color.blue();
            ++count;
            const qreal score = color.hsvSaturationF() *
                                (0.35 + color.valueF()) *
                                (1.0 - qAbs(color.valueF() - 0.62));
            if (score > bestAccent)
            {
                bestAccent = score;
                accent = color;
            }
        }
    }
    if (count == 0)
    {
        return;
    }
    QColor average(int(red / count), int(green / count), int(blue / count));
    if (average.lightnessF() > 0.35)
    {
        average = average.darker(250);
    }
    this->profile_.background = average.darker(145);
    this->profile_.chatBackground = average.darker(125);
    this->profile_.surface = average.lighter(135);
    this->profile_.raisedSurface = average.lighter(165);
    this->profile_.accent =
        accent.lightnessF() < 0.48 ? accent.lighter(150) : accent;
    this->profile_.text = Qt::white;
    this->profile_.chatText = this->profile_.text;
    this->profile_.separateChatText = false;
    this->profile_.mutedText = QColor(u"#b9b9c2"_s);
    this->profile_.chatBorderColor = this->profile_.mutedText;
    this->profile_.wallpaperOverlayColor = average.darker(160);
    this->loadProfileIntoControls();
    this->setStatus(
        u"Wallpaper colors applied. You can fine tune them in the Colors tab."_s);
}

void CustomizationPage::setTransferBusy(bool busy)
{
    this->transferBusy_ = busy;
    for (auto *button : {this->createCodeButton_, this->exportFileButton_,
                         this->importCodeButton_, this->importFileButton_})
    {
        button->setEnabled(!busy);
    }
    this->shareWallpaper_->setEnabled(!busy);
    this->updateControlState();
}

void CustomizationPage::prepareSharedTheme(
    std::function<void(QJsonObject, bool)> ready)
{
    if (this->selectedCustom_)
    {
        this->readControlsIntoProfile();
    }
    this->setTransferBusy(true);
    auto profile = this->profile_;
    if (!profile.useThemeMessageRows)
    {
        profile.useThemeMessageRows = true;
        profile.alternateMessageRows =
            getSettings()->alternateMessages.getValue();
    }
    const auto finish = [ready](const ThemeCustomizationProfile &snapshot,
                                bool include) {
        bool removed = false;
        auto safe = makeShareableCustomizedTheme(buildCustomizedTheme(snapshot),
                                                 &removed, include);
        ready(std::move(safe), removed);
    };
    if (!this->shareWallpaper_->isChecked() || !profile.hasWallpaper() ||
        profile.wallpaperSource.startsWith(u":/themes/wallpapers/"_s))
    {
        finish(profile, this->shareWallpaper_->isChecked());
        return;
    }
    this->setStatus(u"Preparing background"_s, false, true);
    const auto generation = this->profileGeneration_;
    const auto cancellation = this->transferCancellation_.get_token();
    const auto directory = getTheme()->themesDirectory();
    (void)QtConcurrent::run(
        [source = profile.wallpaperSource, cancellation, directory] {
            return storePreparedWallpaper(
                prepareThemeWallpaper(source, cancellation), directory,
                cancellation);
        })
        .then(this, [this, profile, generation, cancellation,
                     finish](StoredWallpaper prepared) mutable {
            if (cancellation.stop_requested() ||
                generation != this->profileGeneration_)
            {
                return;
            }
            const auto &path = prepared.source;
            const auto &data = prepared.data;
            if (path.isEmpty())
            {
                this->setTransferBusy(false);
                this->setStatus(data.error, true);
                return;
            }
            if (const auto id = themeWallpaperUploadId(path); !id.isEmpty())
            {
                profile.wallpaperId = id;
                finish(profile, true);
                return;
            }
            auto *payload = new QHttpMultiPart(QHttpMultiPart::FormDataType);
            QHttpPart part;
            part.setHeader(QNetworkRequest::ContentTypeHeader,
                           data.extension == u"webm"_s ? u"video/webm"_s
                                                       : u"image/webp"_s);
            part.setHeader(
                QNetworkRequest::ContentDispositionHeader,
                QStringLiteral(
                    "form-data; name=\"file\"; filename=\"background.%1\"")
                    .arg(data.extension));
            part.setBody(data.bytes);
            payload->append(part);
            this->setStatus(u"Uploading background"_s, false, true);
            NetworkRequest(QUrl(u"https://kappa.lol/api/upload"_s),
                           NetworkRequestType::Post)
                .multiPart(payload)
                .hideRequestBody()
                .followRedirects(false)
                .timeout(60000)
                .maximumResponseSize(64 * 1024)
                .cancelWith(cancellation)
                .caller(this)
                .onSuccess([this, profile, path, cancellation,
                            finish](const NetworkResult &result) mutable {
                    if (cancellation.stop_requested())
                    {
                        return;
                    }
                    const auto id =
                        result.parseJson().value(u"id"_s).toString();
                    if (!isThemeWallpaperId(id) ||
                        !saveThemeWallpaperUploadId(path, id))
                    {
                        this->setTransferBusy(false);
                        this->setStatus(
                            u"Couldn't save the upload details. Try again."_s,
                            true);
                        return;
                    }
                    profile.wallpaperId = id;
                    finish(profile, true);
                })
                .onError([this, cancellation](const NetworkResult &) {
                    if (!cancellation.stop_requested())
                    {
                        this->setTransferBusy(false);
                        this->setStatus(
                            u"Couldn't upload the background. Try again."_s,
                            true);
                    }
                })
                .execute();
        });
}

void CustomizationPage::exportThemeFile()
{
    const auto path = QFileDialog::getSaveFileName(
        this, u"Save Moltorino theme"_s,
        safeFileBase(this->profile_.name) + u".moltheme.json"_s,
        u"Moltorino themes (*.moltheme.json *.json)"_s);
    if (path.isEmpty())
    {
        return;
    }
    this->prepareSharedTheme([this, path](QJsonObject safe, bool removed) {
        QSaveFile file(path);
        const auto contents = QJsonDocument(appearanceEnvelope(safe))
                                  .toJson(QJsonDocument::Indented);
        this->setTransferBusy(false);
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(contents) != contents.size() || !file.commit())
        {
            this->setStatus(u"Moltorino couldn't save that theme file."_s,
                            true);
            return;
        }
        this->setStatus(removed ? u"Theme saved without its background."_s
                                : u"Theme file saved."_s);
    });
}

bool CustomizationPage::installImportedTheme(ThemeCustomizationProfile profile,
                                             bool adapted,
                                             bool localWallpaperRemoved,
                                             QString *error)
{
    const auto generation = this->profileGeneration_;
    QMessageBox prompt(this);
    prompt.setIcon(QMessageBox::Question);
    prompt.setWindowTitle(u"Import theme"_s);
    prompt.setText(QStringLiteral("Import %1?").arg(profile.name));
    auto details =
        QStringLiteral("Accent: %1\nWallpaper: %2%3")
            .arg(profile.accent.name(QColor::HexRgb).toUpper(),
                 profile.hasWallpaper() ? u"Included"_s : u"Not included"_s,
                 localWallpaperRemoved
                     ? u"\nThe creator's local wallpaper was left out."_s
                     : QString());
    if (adapted)
    {
        details.prepend(u"Adapted from a Bluzyrino palette.\n"_s);
    }
    prompt.setInformativeText(details);
    auto *add = prompt.addButton(u"Import"_s, QMessageBox::AcceptRole);
    prompt.addButton(QMessageBox::Cancel);
    prompt.exec();
    if (prompt.clickedButton() != add ||
        generation != this->profileGeneration_ || !this->confirmProfileChange())
    {
        error->clear();
        return false;
    }

    if (!profile.wallpaperId.isEmpty() && profile.wallpaperSource.isEmpty())
    {
        profile.wallpaperSource = findThemeWallpaper(
            profile.wallpaperId, getTheme()->themesDirectory());
        if (!profile.wallpaperSource.isEmpty())
        {
            return this->writeImportedTheme(std::move(profile), adapted, error);
        }
        this->setTransferBusy(true);
        this->setStatus(u"Downloading background"_s, false, true);
        const auto cancellation = this->transferCancellation_.get_token();
        const auto directory = getTheme()->themesDirectory();
        NetworkRequest(QUrl(u"https://kappa.lol/"_s + profile.wallpaperId))
            .followRedirects(false)
            .timeout(60000)
            .maximumResponseSize(50 * 1024 * 1024)
            .cancelWith(cancellation)
            .caller(this)
            .onSuccess([this, profile, adapted, cancellation,
                        directory](const NetworkResult &result) {
                if (cancellation.stop_requested())
                {
                    return;
                }
                (void)QtConcurrent::run(
                    [bytes = result.getData(), cancellation, directory] {
                        return storePreparedWallpaper(
                            prepareThemeWallpaper(bytes, cancellation),
                            directory, cancellation);
                    })
                    .then(this, [this, profile, adapted, cancellation](
                                    StoredWallpaper prepared) mutable {
                        if (cancellation.stop_requested())
                        {
                            return;
                        }
                        QString error = prepared.data.error;
                        profile.wallpaperSource = prepared.source;
                        this->setTransferBusy(false);
                        if (profile.wallpaperSource.isEmpty())
                        {
                            this->setStatus(error, true);
                            return;
                        }
                        saveThemeWallpaperUploadId(profile.wallpaperSource,
                                                   profile.wallpaperId);
                        if (!this->writeImportedTheme(std::move(profile),
                                                      adapted, &error))
                        {
                            this->setStatus(error, true);
                        }
                    });
            })
            .onError([this, cancellation](const NetworkResult &) {
                if (!cancellation.stop_requested())
                {
                    this->setTransferBusy(false);
                    this->setStatus(
                        u"Couldn't download the background. Try importing again."_s,
                        true);
                }
            })
            .execute();
        return true;
    }
    return this->writeImportedTheme(std::move(profile), adapted, error);
}

bool CustomizationPage::writeImportedTheme(ThemeCustomizationProfile profile,
                                           bool adapted, QString *error)
{
    this->setTransferBusy(false);
    profile.name = profile.name.trimmed() + u" (Imported)"_s;
    const auto path = this->uniqueThemePath(profile.name);
    QSaveFile file(path);
    const auto contents = QJsonDocument(buildCustomizedTheme(profile))
                              .toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(contents) != contents.size() || !file.commit())
    {
        *error = u"Moltorino couldn't save the imported theme."_s;
        return false;
    }
    this->reloadProfiles(QFileInfo(path).fileName());
    this->setStatus(
        adapted ? u"Theme adapted and imported. Choose Apply theme to use it."_s
                : u"Theme imported. Choose Apply theme to use it."_s);
    return true;
}

void CustomizationPage::importThemeFile()
{
    const auto path =
        QFileDialog::getOpenFileName(this, u"Open theme file"_s, {},
                                     u"Theme files (*.moltheme.json *.json)"_s);
    if (path.isEmpty())
    {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 2 * 1024 * 1024)
    {
        this->setStatus(u"Moltorino couldn't read that theme file."_s, true);
        return;
    }
    const auto bytes = file.read(2 * 1024 * 1024 + 1);
    if (bytes.size() > 2 * 1024 * 1024 || !file.atEnd() ||
        file.error() != QFileDevice::NoError)
    {
        this->setStatus(u"Moltorino couldn't read that theme file."_s, true);
        return;
    }
    QString error;
    const auto imported =
        themeFromImport(bytes, importedThemeName(path), &error);
    if (!imported)
    {
        this->setStatus(error, true);
        return;
    }
    if (!this->installImportedTheme(imported->profile, imported->adapted,
                                    imported->localWallpaperRemoved, &error))
    {
        this->setStatus(error.isEmpty() ? u"Theme wasn't imported."_s : error,
                        !error.isEmpty());
        return;
    }
}

void CustomizationPage::createThemeCode()
{
    const auto expiry = this->shareExpiry_->currentData();
    const auto generation = this->profileGeneration_;
    const auto cancellation = this->transferCancellation_.get_token();
    this->shareCode_->clear();
    this->prepareSharedTheme([this, expiry, generation, cancellation](
                                 QJsonObject safe, bool removed) {
        QJsonObject payload{
            {u"source"_s, u"appearance"_s},
            {u"title"_s, safe.value(u"metadata"_s).toObject().value(u"name"_s)},
            {u"language"_s, u"json"_s},
            {u"content"_s,
             QString::fromUtf8(QJsonDocument(appearanceEnvelope(safe))
                                   .toJson(QJsonDocument::Compact))},
            {u"expiresInDays"_s, expiry.isValid()
                                     ? QJsonValue(expiry.toInt())
                                     : QJsonValue(QJsonValue::Null)},
        };
        this->setStatus(u"Creating share code"_s, false, true);
        NetworkRequest(QUrl(u"https://h.moltorino.com/api/paste"_s),
                       NetworkRequestType::Post)
            .timeout(15000)
            .maximumResponseSize(64 * 1024)
            .hideRequestBody()
            .followRedirects(false)
            .json(payload)
            .cancelWith(cancellation)
            .caller(this)
            .onSuccess([this, removed, expiry,
                        generation](const NetworkResult &result) {
                if (generation != this->profileGeneration_)
                {
                    return;
                }
                const auto json = result.parseJson();
                const auto code = json.value(u"slug"_s).toString();
                if (cleanCode(code) != code || code.isEmpty())
                {
                    this->setStatus(u"The server returned an invalid code."_s,
                                    true);
                    return;
                }
                if (!expiry.isValid() && !json.value(u"expiresAt"_s).isNull())
                {
                    this->setStatus(
                        u"Could not create a link that never expires. "
                        u"Choose an expiry and try again."_s,
                        true);
                    return;
                }
                this->shareCode_->setText(code);
                crossPlatformCopy(code);
                this->setStatus(
                    removed ? u"Share code copied without the background."_s
                            : u"Share code copied."_s);
            })
            .onError([this, generation](const NetworkResult &) {
                if (generation == this->profileGeneration_)
                {
                    this->setStatus(
                        u"Couldn't create a share code. Try again."_s, true);
                }
            })
            .finally([this, generation] {
                if (generation == this->profileGeneration_)
                {
                    this->setTransferBusy(false);
                }
            })
            .execute();
    });
}

void CustomizationPage::importThemeCode()
{
    const auto code = cleanCode(this->importCode_->text());
    if (code.isEmpty())
    {
        this->setStatus(u"Enter a valid share code."_s, true);
        return;
    }
    const auto generation = ++this->shareGeneration_;
    const auto cancellation = this->transferCancellation_.get_token();
    this->setTransferBusy(true);
    this->setStatus(u"Loading theme"_s, false, true);
    NetworkRequest(
        QUrl(QStringLiteral("https://h.moltorino.com/%1/raw").arg(code)))
        .timeout(15000)
        .maximumResponseSize(2 * 1024 * 1024)
        .followRedirects(false)
        .cancelWith(cancellation)
        .caller(this)
        .onSuccess([this, generation,
                    cancellation](const NetworkResult &result) {
            if (cancellation.stop_requested() ||
                generation != this->shareGeneration_)
            {
                return;
            }
            QString error;
            const auto imported =
                themeFromImport(result.getData(), u"Shared theme"_s, &error);
            if (!imported)
            {
                this->setTransferBusy(false);
                this->setStatus(error, true);
                return;
            }
            if (!this->installImportedTheme(
                    imported->profile, imported->adapted,
                    imported->localWallpaperRemoved, &error))
            {
                this->setTransferBusy(false);
                this->setStatus(
                    error.isEmpty() ? u"Theme wasn't imported."_s : error,
                    !error.isEmpty());
                return;
            }
        })
        .onError([this, generation, cancellation](const NetworkResult &) {
            if (!cancellation.stop_requested() &&
                generation == this->shareGeneration_)
            {
                this->setTransferBusy(false);
                this->setStatus(u"Couldn't load that share code."_s, true);
            }
        })
        .execute();
}

void CustomizationPage::setStatus(const QString &text, bool error, bool busy)
{
    this->statusAnimation_->stop();
    this->statusText_ = text;
    this->statusDots_ = 1;
    this->status_->setText(busy ? text + u'.' : text);
    this->status_->setStyleSheet(error ? u"color:#ff7777;"_s : QString());
    if (busy && !text.isEmpty())
    {
        this->statusAnimation_->start();
    }
}

}
