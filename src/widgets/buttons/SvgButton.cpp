// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/buttons/SvgButton.hpp"

#include "singletons/Theme.hpp"

#include <QFile>
#include <QHash>
#include <QSvgRenderer>

#include <utility>

namespace chatterino {
namespace {
std::shared_ptr<QSvgRenderer> makeSvgRenderer(const QString &path,
                                              const QColor &accent)
{
    auto renderer = std::make_shared<QSvgRenderer>();
    if (accent.isValid())
    {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly))
        {
            auto svg = file.readAll();
            const auto root = svg.indexOf("<svg");
            const auto end = root < 0 ? -1 : svg.indexOf('>', root);
            if (end >= 0)
            {
                svg.insert(end + 1,
                           QString("<style>.accent { fill: %1; stroke: %2; "
                                   "opacity: %3; }</style>")
                               .arg(accent.name(), accent.darker(185).name())
                               .arg(accent.alphaF())
                               .toUtf8());
            }
            renderer->load(svg);
        }
    }
    else
    {
        renderer->load(path);
    }
    renderer->setAspectRatioMode(Qt::KeepAspectRatio);
    return renderer;
}

std::shared_ptr<QSvgRenderer> rendererForPath(const QString &path,
                                              const QColor &accent)
{
    if (!path.startsWith(QStringLiteral(":/")))
    {
        return makeSvgRenderer(path, accent);
    }

    static QHash<QString, std::pair<QColor, std::weak_ptr<QSvgRenderer>>> cache;
    if (auto it = cache.constFind(path); it != cache.cend())
    {
        if (auto renderer = it.value().second.lock();
            renderer && it.value().first == accent)
        {
            return renderer;
        }
    }

    auto renderer = makeSvgRenderer(path, accent);
    if (renderer->isValid() && !renderer->animated())
    {
        cache.insert(path, {accent, std::weak_ptr<QSvgRenderer>{renderer}});
    }
    return renderer;
}
}

SvgButton::SvgButton(Src source, BaseWidget *parent, QSize padding)
    : Button(parent)
    , source_(std::move(source))
    , svg_(rendererForPath(this->currentSvgPath(), this->source_.useAccent
                                                       ? this->theme->accent
                                                       : QColor{}))
    , padding_(padding)
{
    this->setContentCacheEnabled(true);
}

void SvgButton::setSource(Src source)
{

    this->source_ = std::move(source);
    this->loadSource();
    this->invalidateContent();
}

void SvgButton::setColor(std::optional<QColor> color)
{
    this->color_ = color;
    this->invalidateContent();
}

void SvgButton::setPadding(QSize padding)
{
    if (this->padding_ == padding)
    {
        return;
    }

    this->padding_ = padding;
    this->invalidateContent();
}

void SvgButton::themeChangedEvent()
{
    Button::themeChangedEvent();

    if (this->source_.dark == this->source_.light && !this->source_.useAccent)
    {
        return;
    }
    this->loadSource();
    this->invalidateContent();
}

void SvgButton::scaleChangedEvent(float scale)
{
    Button::scaleChangedEvent(scale);

    this->invalidateContent();
}

void SvgButton::resizeEvent(QResizeEvent *e)
{
    Button::resizeEvent(e);

    this->invalidateContent();
}

void SvgButton::paintContent(QPainter &painter)
{
    QSize actualPadding = this->scale() * this->padding_;
    QPoint topLeft{actualPadding.width(), actualPadding.height()};
    QSize contentSize = this->size() - 2 * actualPadding;
    auto bounds = QRectF{topLeft, contentSize};
    this->svg_->render(&painter, bounds);

    if (this->color_.has_value())
    {
        painter.save();

        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(bounds, *this->color_);

        painter.restore();
    }
}

QString SvgButton::currentSvgPath() const
{
    if (this->theme->isLightTheme())
    {
        return this->source_.light;
    }
    return this->source_.dark;
}

void SvgButton::loadSource()
{
    this->svg_ = rendererForPath(
        this->currentSvgPath(),
        this->source_.useAccent ? this->theme->accent : QColor{});
}

}
