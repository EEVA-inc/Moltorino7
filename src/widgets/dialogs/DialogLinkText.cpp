#include "widgets/dialogs/DialogLinkText.hpp"

#include "Application.hpp"
#include "common/LinkParser.hpp"
#include "singletons/Settings.hpp"
#include "util/IncognitoBrowser.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QDesktopServices>
#include <QFontMetricsF>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace chatterino {
namespace {

QString linkifiedText(const QString &text,
                      const QVector<DialogTextLink> &links)
{
    if (links.isEmpty())
    {
        return text.toHtmlEscaped().replace(u'\n', QStringLiteral("<br>"));
    }

    QString html;
    html.reserve(text.size() + links.size() * 48);
    qsizetype position = 0;
    for (const auto &link : links)
    {
        html += text.sliced(position, link.start - position)
                    .toHtmlEscaped()
                    .replace(u'\n', QStringLiteral("<br>"));
        html += QStringLiteral("<a href=\"%1\">%2</a>")
                    .arg(link.target.toHtmlEscaped(),
                         text.sliced(link.start, link.length).toHtmlEscaped());
        position = link.start + link.length;
    }
    html += text.sliced(position)
                .toHtmlEscaped()
                .replace(u'\n', QStringLiteral("<br>"));
    return html;
}

QTextLine prepareTextLayout(QTextLayout &layout, int width)
{
    QTextOption textOption;
    textOption.setWrapMode(QTextOption::NoWrap);
    layout.setTextOption(textOption);
    layout.beginLayout();
    auto line = layout.createLine();
    if (line.isValid())
    {
        line.setLineWidth(std::max(1, width));
        line.setPosition({0, 0});
    }
    layout.endLayout();
    return line;
}

struct ElidedDialogText {
    QString text;
    QVector<qsizetype> sourceIndexes;
};

ElidedDialogText elideDialogText(QString text, const QFont &font,
                                 Qt::TextElideMode mode, int width)
{
    text.replace(u'\n', u' ');
    const auto elided =
        QFontMetricsF(font).elidedText(text, mode, std::max(1, width));

    ElidedDialogText result{elided, QVector<qsizetype>(elided.size(), -1)};
    if (elided == text)
    {
        for (qsizetype position = 0; position < text.size(); ++position)
        {
            result.sourceIndexes[position] = position;
        }
        return result;
    }

    qsizetype prefix = 0;
    if (mode != Qt::ElideLeft)
    {
        while (prefix < text.size() && prefix < elided.size() &&
               text.at(prefix) == elided.at(prefix))
        {
            result.sourceIndexes[prefix] = prefix;
            ++prefix;
        }
    }

    if (mode != Qt::ElideRight)
    {
        qsizetype suffix = 0;
        while (suffix < text.size() - prefix &&
               suffix < elided.size() - prefix &&
               text.at(text.size() - suffix - 1) ==
                   elided.at(elided.size() - suffix - 1))
        {
            result.sourceIndexes[elided.size() - suffix - 1] =
                text.size() - suffix - 1;
            ++suffix;
        }
    }
    return result;
}

QRect itemTextRect(const QStyleOptionViewItem &option)
{
    const auto *style = option.widget != nullptr ? option.widget->style()
                                                 : QApplication::style();
    return style->subElementRect(QStyle::SE_ItemViewItemText, &option,
                                 option.widget);
}

}

QVector<DialogTextLink> dialogTextLinks(const QString &text)
{
    QVector<DialogTextLink> links;
    const QStringView view(text);
    qsizetype position = 0;
    while (position < view.size())
    {
        while (position < view.size() && view.at(position).isSpace())
        {
            ++position;
        }
        const auto tokenStart = position;
        while (position < view.size() && !view.at(position).isSpace())
        {
            ++position;
        }
        if (tokenStart == position)
        {
            continue;
        }

        const auto token = view.sliced(tokenStart, position - tokenStart);
        const auto parsed = linkparser::parse(token);
        if (!parsed)
        {
            continue;
        }

        const auto start = parsed->link.begin() - view.begin();
        auto target = parsed->link.toString();
        if (parsed->protocol.isEmpty())
        {
            target.prepend(QStringLiteral("https://"));
        }
        const QUrl url(target);
        if (!url.isValid() ||
            (url.scheme() != u"http" && url.scheme() != u"https"))
        {
            continue;
        }
        links.push_back({start, parsed->link.size(), url.toString()});
    }
    return links;
}

void openDialogTextLink(const QString &target)
{
    const QUrl url(target);
    if (!url.isValid() ||
        (url.scheme() != u"http" && url.scheme() != u"https"))
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

void configureDialogLinkLabel(QLabel *label)
{
    label->setTextFormat(Qt::RichText);
    label->setOpenExternalLinks(false);
    QObject::connect(label, &QLabel::linkActivated, label,
                     [](const QString &target) {
                         openDialogTextLink(target);
                     });
}

void setDialogLinkLabelText(QLabel *label, const QString &text)
{
    const auto links = dialogTextLinks(text);
    label->setTextInteractionFlags(
        links.isEmpty()
            ? Qt::TextSelectableByMouse
            : Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse |
                  Qt::LinksAccessibleByKeyboard);
    label->setFocusPolicy(links.isEmpty() ? Qt::NoFocus : Qt::StrongFocus);
    label->setText(linkifiedText(text, links));
}

DialogLinkTextDelegate::DialogLinkTextDelegate(QAbstractItemView *view)
    : QStyledItemDelegate(view)
    , view_(view)
{
    this->view_->setMouseTracking(true);
    this->view_->viewport()->installEventFilter(this);
}

void DialogLinkTextDelegate::paint(QPainter *painter,
                                   const QStyleOptionViewItem &option,
                                   const QModelIndex &index) const
{
    const auto text = index.data(Qt::DisplayRole).toString();
    const auto links = dialogTextLinks(text);
    if (links.isEmpty())
    {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    QStyleOptionViewItem background(option);
    this->initStyleOption(&background, index);
    const auto textRect = itemTextRect(background);
    background.text.clear();
    auto *style = background.widget != nullptr ? background.widget->style()
                                               : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &background, painter,
                       background.widget);

    const auto display = elideDialogText(
        text, background.font, background.textElideMode, textRect.width());
    QTextLayout layout(display.text, background.font);
    const auto linkColor =
        background.state & QStyle::State_Selected
            ? background.palette.color(QPalette::HighlightedText)
            : background.palette.color(QPalette::Link);
    QVector<QTextLayout::FormatRange> formats;
    formats.reserve(links.size());
    for (const auto &link : links)
    {
        QTextCharFormat format;
        format.setForeground(linkColor);
        format.setFontUnderline(true);
        const auto linkEnd = link.start + link.length;
        qsizetype runStart = -1;
        for (qsizetype position = 0; position <= display.text.size();
             ++position)
        {
            const bool inLink =
                position < display.sourceIndexes.size() &&
                display.sourceIndexes.at(position) >= link.start &&
                display.sourceIndexes.at(position) < linkEnd;
            if (inLink && runStart < 0)
            {
                runStart = position;
            }
            else if (!inLink && runStart >= 0)
            {
                formats.push_back({static_cast<int>(runStart),
                                   static_cast<int>(position - runStart),
                                   format});
                runStart = -1;
            }
        }
    }
    layout.setFormats(formats);
    const auto line = prepareTextLayout(layout, textRect.width());
    const auto lineHeight = line.isValid()
                                ? line.height()
                                : QFontMetricsF(background.font).height();
    const auto top = textRect.top() +
                     std::max<qreal>(0, (textRect.height() - lineHeight) / 2);

    painter->save();
    painter->setClipRect(textRect);
    painter->setPen(background.state & QStyle::State_Selected
                        ? background.palette.color(QPalette::HighlightedText)
                        : background.palette.color(QPalette::Text));
    layout.draw(painter, QPointF(textRect.left(), top));
    painter->restore();
}

bool DialogLinkTextDelegate::editorEvent(
    QEvent *event, QAbstractItemModel *model,
    const QStyleOptionViewItem &option, const QModelIndex &index)
{
    if (event->type() == QEvent::MouseMove)
    {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        this->view_->viewport()->setCursor(
            this->linkAt(option, index, mouse->position()).isEmpty()
                ? Qt::ArrowCursor
                : Qt::PointingHandCursor);
    }
    else if (event->type() == QEvent::MouseButtonPress)
    {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton)
        {
            this->pressedLink_ =
                this->linkAt(option, index, mouse->position());
            this->pressedIndex_ = index;
            if (!this->pressedLink_.isEmpty())
            {
                return true;
            }
        }
    }
    else if (event->type() == QEvent::MouseButtonRelease)
    {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton &&
            !this->pressedLink_.isEmpty())
        {
            const auto pressed = std::exchange(this->pressedLink_, {});
            const auto pressedIndex = std::exchange(
                this->pressedIndex_, QPersistentModelIndex{});
            const auto released = this->linkAt(option, index, mouse->position());
            if (pressedIndex == index && released == pressed)
            {
                openDialogTextLink(released);
            }
            return true;
        }
    }
    return QStyledItemDelegate::editorEvent(event, model, option, index);
}

bool DialogLinkTextDelegate::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != this->view_->viewport())
    {
        return QStyledItemDelegate::eventFilter(watched, event);
    }
    if (event->type() == QEvent::Leave)
    {
        this->view_->viewport()->unsetCursor();
        this->pressedLink_.clear();
        this->pressedIndex_ = QPersistentModelIndex{};
    }
    else if (event->type() == QEvent::MouseMove ||
             event->type() == QEvent::MouseButtonPress ||
             event->type() == QEvent::MouseButtonRelease)
    {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        const auto index = this->view_->indexAt(mouse->position().toPoint());
        if (!index.isValid() ||
            this->view_->itemDelegateForIndex(index) != this)
        {
            this->view_->viewport()->unsetCursor();
            if (event->type() != QEvent::MouseMove)
            {
                this->pressedLink_.clear();
                this->pressedIndex_ = QPersistentModelIndex{};
            }
        }
    }
    return QStyledItemDelegate::eventFilter(watched, event);
}

QString DialogLinkTextDelegate::linkAt(
    const QStyleOptionViewItem &option, const QModelIndex &index,
    const QPointF &position) const
{
    const auto text = index.data(Qt::DisplayRole).toString();
    const auto links = dialogTextLinks(text);
    if (links.isEmpty())
    {
        return {};
    }
    QStyleOptionViewItem styled(option);
    this->initStyleOption(&styled, index);
    const auto textRect = itemTextRect(styled);
    if (!textRect.contains(position.toPoint()))
    {
        return {};
    }
    const auto display = elideDialogText(
        text, styled.font, styled.textElideMode, textRect.width());
    QTextLayout layout(display.text, styled.font);
    const auto line = prepareTextLayout(layout, textRect.width());
    if (!line.isValid())
    {
        return {};
    }
    const auto top = textRect.top() +
                     std::max<qreal>(0,
                                     (textRect.height() - line.height()) / 2);
    const auto local = position - QPointF(textRect.left(), top);
    if (local.x() < 0 || local.y() < 0 ||
        local.y() >= line.height() || local.x() > line.naturalTextWidth())
    {
        return {};
    }
    const auto cursor = line.xToCursor(local.x());
    if (cursor < 0 || cursor >= display.sourceIndexes.size())
    {
        return {};
    }
    const auto sourceIndex = display.sourceIndexes.at(cursor);
    if (sourceIndex < 0)
    {
        return {};
    }
    for (const auto &link : links)
    {
        if (sourceIndex >= link.start &&
            sourceIndex < link.start + link.length)
        {
            return link.target;
        }
    }
    return {};
}

}
