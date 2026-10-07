#include "widgets/dialogs/ModeratorCommentsView.hpp"

#include "Application.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "util/IncognitoBrowser.hpp"
#include "widgets/dialogs/DialogLinkText.hpp"
#include "widgets/dialogs/MoltorinoDialogTheme.hpp"
#include "widgets/dialogs/SettingsDialog.hpp"

#include <QAbstractListModel>
#include <QCheckBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QStyledItemDelegate>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace chatterino {
namespace {

constexpr int MAX_RENDERED_COMMENTS = 500;

void openCommentLink(const QString &target)
{
    if (getSettings()->openLinksIncognito && supportsIncognitoLinks())
    {
        openLinkIncognito(target);
        return;
    }
    QDesktopServices::openUrl(QUrl(target));
}

QDateTime commentDate(const QString &value)
{
    auto date = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!date.isValid())
    {
        date = QDateTime::fromString(value, Qt::ISODate);
    }
    return date;
}

QString commentDateText(const QString &value)
{
    const auto date = commentDate(value);
    return date.isValid()
               ? QLocale().toString(date.toLocalTime(), QLocale::ShortFormat)
               : QStringLiteral("Unknown time");
}

}

class ModeratorCommentsModel final : public QAbstractListModel
{
public:
    enum Role {
        CommentText = Qt::UserRole + 1,
        Author,
        Added,
        Shared,
        Shareable,
        SourceChannel,
    };

    explicit ModeratorCommentsModel(
        const QVector<GqlModeratorComment> *comments, QObject *parent)
        : QAbstractListModel(parent)
        , comments_(comments)
    {
    }

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() || this->comments_ == nullptr
                   ? 0
                   : this->comments_->size();
    }

    QVariant data(const QModelIndex &index,
                  int role = Qt::DisplayRole) const override
    {
        if (!index.isValid() || this->comments_ == nullptr ||
            index.row() < 0 || index.row() >= this->comments_->size())
        {
            return {};
        }

        const auto &comment = this->comments_->at(index.row());
        switch (role)
        {
            case Qt::DisplayRole:
            case CommentText:
                return comment.text;
            case Author: {
                auto author = comment.authorDisplayName.trimmed();
                return author.isEmpty() ? comment.authorLogin : author;
            }
            case Added:
                return commentDateText(comment.timestamp);
            case Shared:
                return comment.shared;
            case Shareable:
                return comment.shareable;
            case SourceChannel:
                return comment.channelLogin;
            case Qt::ToolTipRole:
                if (comment.shared && !comment.channelLogin.isEmpty())
                {
                    return QStringLiteral("Shared from #%1")
                        .arg(comment.channelLogin);
                }
                return {};
            default:
                return {};
        }
    }

    void refresh()
    {
        this->beginResetModel();
        this->endResetModel();
    }

private:
    const QVector<GqlModeratorComment> *comments_{};
};

namespace {

qreal prepareTextLayout(QTextLayout &layout, qreal width)
{
    layout.beginLayout();
    qreal height = 0;
    while (true)
    {
        auto line = layout.createLine();
        if (!line.isValid())
        {
            break;
        }
        line.setLineWidth(std::max<qreal>(1, width));
        line.setPosition(QPointF(0, height));
        height += line.height();
    }
    layout.endLayout();
    return height;
}

qreal wrappedTextHeight(
    const QString &source, const QFont &font, qreal width,
    const QVector<QTextLayout::FormatRange> &formats = {},
    QPainter *painter = nullptr, const QPointF &origin = {})
{
    auto text = source;
    text.replace(u'\n', QChar::LineSeparator);
    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.setFormats(formats);
    const auto height = prepareTextLayout(layout, width);
    if (painter != nullptr)
    {
        layout.draw(painter, origin);
    }
    return std::max<qreal>(height, QFontMetricsF(font).height());
}

QString linkAtPosition(const QString &source, const QFont &font, qreal width,
                       const QPointF &position,
                       const QVector<DialogTextLink> &links)
{
    if (position.x() < 0 || position.y() < 0)
    {
        return {};
    }

    auto text = source;
    text.replace(u'\n', QChar::LineSeparator);
    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    prepareTextLayout(layout, width);

    for (int lineIndex = 0; lineIndex < layout.lineCount(); ++lineIndex)
    {
        const auto line = layout.lineAt(lineIndex);
        if (position.y() < line.y() ||
            position.y() >= line.y() + line.height())
        {
            continue;
        }
        if (position.x() > line.naturalTextWidth())
        {
            return {};
        }
        const auto cursor = line.xToCursor(position.x());
        for (const auto &link : links)
        {
            if (cursor >= link.start && cursor < link.start + link.length)
            {
                return link.target;
            }
        }
        return {};
    }
    return {};
}

class ModeratorCommentsListView final : public QListView
{
public:
    using QListView::QListView;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
        {
            this->pressedLink_ = this->linkAt(event->position());
            if (!this->pressedLink_.isEmpty())
            {
                event->accept();
                return;
            }
        }
        QListView::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        this->viewport()->setCursor(this->linkAt(event->position()).isEmpty()
                                        ? Qt::ArrowCursor
                                        : Qt::PointingHandCursor);
        QListView::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton &&
            !this->pressedLink_.isEmpty())
        {
            const auto pressed = std::exchange(this->pressedLink_, {});
            const auto released = this->linkAt(event->position());
            if (!released.isEmpty() && released == pressed)
            {
                openCommentLink(released);
            }
            event->accept();
            return;
        }
        QListView::mouseReleaseEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        this->viewport()->unsetCursor();
        QListView::leaveEvent(event);
    }

private:
    QString linkAt(const QPointF &position) const
    {
        const auto index = this->indexAt(position.toPoint());
        if (!index.isValid())
        {
            return {};
        }

        const auto rect = this->visualRect(index);
        const auto contentLeft = rect.left() + 8;
        const auto contentTop = rect.top() + 7;
        const auto contentWidth = std::max(
            80, rect.right() - contentLeft - 8);
        const auto comment =
            index.data(ModeratorCommentsModel::CommentText).toString();
        return linkAtPosition(
            comment, this->font(), contentWidth,
            position - QPointF(contentLeft, contentTop),
            dialogTextLinks(comment));
    }

    QString pressedLink_;
};

class ModeratorCommentDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void setColors(const QColor &background, const QColor &hover,
                   const QColor &selected, const QColor &text,
                   const QColor &muted, const QColor &link,
                   const QColor &separator)
    {
        this->background_ = background;
        this->hover_ = hover;
        this->selected_ = selected;
        this->text_ = text;
        this->muted_ = muted;
        this->link_ = link;
        this->separator_ = separator;
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        auto width = option.rect.width();
        if (width <= 0 && option.widget != nullptr)
        {
            width = option.widget->width();
        }
        const auto contentWidth = std::max(80, width - 16);
        const auto bodyHeight = wrappedTextHeight(
            index.data(ModeratorCommentsModel::CommentText).toString(),
            option.font, contentWidth);
        auto metadataFont = option.font;
        metadataFont.setPointSizeF(
            std::max<qreal>(7, metadataFont.pointSizeF() - 1));
        const auto metadataHeight = QFontMetricsF(metadataFont).height();
        return {width,
                std::max(40, qCeil(7 + bodyHeight + 3 + metadataHeight + 7))};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        painter->setClipRect(option.rect);

        const auto background = option.state & QStyle::State_Selected
                                    ? this->selected_
                                : option.state & QStyle::State_MouseOver
                                    ? this->hover_
                                    : this->background_;
        painter->fillRect(option.rect, background);

        const auto contentLeft = option.rect.left() + 8;
        const auto contentWidth = std::max(80, option.rect.right() -
                                                  contentLeft - 8);
        const auto contentTop = option.rect.top() + 7;
        painter->setFont(option.font);
        painter->setPen(this->text_);
        const auto comment =
            index.data(ModeratorCommentsModel::CommentText).toString();
        const auto links = dialogTextLinks(comment);
        QVector<QTextLayout::FormatRange> formats;
        formats.reserve(links.size());
        for (const auto &link : links)
        {
            QTextCharFormat format;
            format.setForeground(this->link_);
            format.setFontUnderline(true);
            formats.push_back({static_cast<int>(link.start),
                               static_cast<int>(link.length), format});
        }
        const auto bodyHeight = wrappedTextHeight(
            comment, option.font, contentWidth, formats, painter,
            QPointF(contentLeft, contentTop));

        auto metadataFont = option.font;
        metadataFont.setPointSizeF(
            std::max<qreal>(7, metadataFont.pointSizeF() - 1));
        painter->setFont(metadataFont);
        const QFontMetricsF metadataMetrics(metadataFont);
        const auto metadataY = contentTop + bodyHeight + 3;
        const auto author =
            index.data(ModeratorCommentsModel::Author).toString();
        auto authorColor = this->text_;
        authorColor.setAlpha(205);
        painter->setPen(authorColor);
        painter->drawText(QPointF(contentLeft,
                                  metadataY + metadataMetrics.ascent()),
                          author);

        const auto authorWidth = metadataMetrics.horizontalAdvance(author);
        auto details = QStringLiteral(" · %1").arg(
            index.data(ModeratorCommentsModel::Added).toString());
        const bool shared =
            index.data(ModeratorCommentsModel::Shared).toBool();
        const bool shareable =
            index.data(ModeratorCommentsModel::Shareable).toBool();
        const auto source =
            index.data(ModeratorCommentsModel::SourceChannel).toString();
        if (shared && !source.isEmpty())
        {
            details += QStringLiteral(" · Shared from #%1").arg(source);
        }
        else if (shared || shareable)
        {
            details += QStringLiteral(" · Shared");
        }
        details = metadataMetrics.elidedText(
            details, Qt::ElideRight,
            std::max<qreal>(0, contentWidth - authorWidth));
        painter->setPen(this->muted_);
        painter->drawText(
            QPointF(contentLeft + authorWidth,
                    metadataY + metadataMetrics.ascent()),
            details);

        painter->setPen(this->separator_);
        painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());

        painter->restore();
    }

private:
    QColor background_;
    QColor hover_;
    QColor selected_;
    QColor text_;
    QColor muted_;
    QColor link_;
    QColor separator_;
};

}

ModeratorCommentsView::ModeratorCommentsView(QWidget *parent)
    : QWidget(parent)
{
    this->setAttribute(Qt::WA_NoMousePropagation);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);

    auto *statusRow = new QHBoxLayout;
    statusRow->setContentsMargins(7, 0, 7, 0);
    this->statusLabel_ = new QLabel(this);
    this->statusLabel_->setSizePolicy(QSizePolicy::Expanding,
                                      QSizePolicy::Preferred);
    this->statusLabel_->setWordWrap(true);
    statusRow->addWidget(this->statusLabel_, 1);
    this->stateButton_ = new QPushButton(this);
    this->stateButton_->setAutoDefault(false);
    this->stateButton_->hide();
    statusRow->addWidget(this->stateButton_);
    layout->addLayout(statusRow);

    this->list_ = new ModeratorCommentsListView(this);
    this->model_ = new ModeratorCommentsModel(&this->comments_, this);
    this->list_->setModel(this->model_);
    this->list_->setItemDelegate(new ModeratorCommentDelegate(this->list_));
    this->list_->setFrameShape(QFrame::NoFrame);
    this->list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    this->list_->setSelectionMode(QAbstractItemView::SingleSelection);
    this->list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    this->list_->setMouseTracking(true);
    this->list_->setContextMenuPolicy(Qt::CustomContextMenu);
    this->list_->setResizeMode(QListView::Adjust);
    layout->addWidget(this->list_, 1);

    auto *composer = new QHBoxLayout;
    composer->setContentsMargins(0, 0, 0, 0);
    composer->setSpacing(6);
    this->commentInput_ = new QLineEdit(this);
    this->commentInput_->setPlaceholderText("Write a moderator comment");
    this->commentInput_->setMaxLength(500);
    composer->addWidget(this->commentInput_, 1);
    this->addButton_ = new QPushButton("Add comment", this);
    this->addButton_->setAutoDefault(false);
    composer->addWidget(this->addButton_);
    layout->addLayout(composer);

    auto *options = new QHBoxLayout;
    options->setContentsMargins(7, 0, 7, 0);
    this->shareComment_ = new QCheckBox("Share this comment", this);
    this->shareComment_->setToolTip("Shared comments are visible to moderators "
                                    "in channels that share ban information.");
    options->addWidget(this->shareComment_);
    options->addStretch(1);
    this->characterCount_ = new QLabel("0/500", this);
    options->addWidget(this->characterCount_);
    layout->addLayout(options);

    QObject::connect(this->stateButton_, &QPushButton::clicked, this, [this] {
        if (this->authToken_.isEmpty())
        {
            SettingsDialog::showDialog(
                SettingsDialogPreference::MoltorinoAccounts);
            return;
        }
        this->retry();
    });
    QObject::connect(this->commentInput_, &QLineEdit::textChanged, this,
                     [this] {
                         this->updateComposer();
                     });
    QObject::connect(this->commentInput_, &QLineEdit::returnPressed, this,
                     [this] {
                         this->submitComment();
                     });
    QObject::connect(this->addButton_, &QPushButton::clicked, this, [this] {
        this->submitComment();
    });
    QObject::connect(this->list_, &QListView::customContextMenuRequested,
                     this, [this](const QPoint &position) {
                         this->showCommentMenu(position);
                     });
    QObject::connect(this->list_->verticalScrollBar(),
                     &QScrollBar::valueChanged, this, [this](int value) {
                         auto *bar = this->list_->verticalScrollBar();
                         if (bar->maximum() - value <= 80)
                         {
                             this->loadOlder();
                         }
                     });

    this->setMinimumSize(400, 275);
    this->updateState();
    getSettings()->customPinAuthToken.connect(
        [this] {
            this->authenticationChanged();
        },
        this->authConnections_, false);
}

void ModeratorCommentsView::setContext(const QString &channelId,
                                       const QString &channelLogin,
                                       const QString &targetId,
                                       const QString &targetLogin,
                                       bool hasModerationAccess,
                                       bool targetLookupFinished)
{
    if (this->channelId_ == channelId && this->channelLogin_ == channelLogin &&
        this->targetId_ == targetId && this->targetLogin_ == targetLogin &&
        this->hasModerationAccess_ == hasModerationAccess &&
        this->targetLookupFinished_ == targetLookupFinished)
    {
        return;
    }

    this->channelId_ = channelId;
    this->channelLogin_ = channelLogin;
    this->targetId_ = targetId;
    this->targetLogin_ = targetLogin;
    this->hasModerationAccess_ = hasModerationAccess;
    this->targetLookupFinished_ = targetLookupFinished;
    this->reset();
    if (this->activated_)
    {
        this->loadInitial();
    }
}

void ModeratorCommentsView::activate()
{
    this->activated_ = true;
    if (!this->loaded_ && !this->localLoading_ && !this->sharedLoading_)
    {
        this->loadInitial();
    }
}

void ModeratorCommentsView::deactivate()
{
    this->activated_ = false;
}

void ModeratorCommentsView::authenticationChanged()
{
    this->reset();
    if (this->activated_)
    {
        this->loadInitial();
    }
}

void ModeratorCommentsView::refreshStyle(float scale)
{
    const auto uiFont =
        getApp()->getFonts()->getFont(FontStyle::UiMedium, scale);
    const auto *theme = getTheme();

    auto palette = this->palette();
    palette.setColor(QPalette::Window, theme->window.background);
    palette.setColor(QPalette::WindowText, theme->window.text);
    palette.setColor(QPalette::Base,
                     theme->tabs.regular.backgrounds.regular);
    palette.setColor(QPalette::AlternateBase,
                     theme->tabs.regular.backgrounds.hover);
    palette.setColor(QPalette::Text, theme->window.text);
    palette.setColor(QPalette::Button,
                     theme->tabs.selected.backgrounds.regular);
    palette.setColor(QPalette::ButtonText, theme->window.text);
    palette.setColor(QPalette::Highlight,
                     theme->tabs.selected.backgrounds.regular);
    palette.setColor(QPalette::HighlightedText, theme->window.text);
    palette.setColor(QPalette::PlaceholderText,
                     theme->messages.textColors.chatPlaceholder);
    palette.setColor(QPalette::Link, theme->accent);
    palette.setColor(QPalette::Mid, theme->tabs.dividerLine);
    palette.setColor(QPalette::ToolTipBase,
                     theme->tabs.selected.backgrounds.regular);
    palette.setColor(QPalette::ToolTipText, theme->window.text);
    palette.setColor(QPalette::Disabled, QPalette::WindowText,
                     theme->tabs.regular.text);
    palette.setColor(QPalette::Disabled, QPalette::Text,
                     theme->tabs.regular.text);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText,
                     theme->tabs.regular.text);
    palette.setColor(QPalette::Disabled, QPalette::Button,
                     theme->tabs.regular.backgrounds.unfocused);

    this->setPalette(palette);
    this->statusLabel_->setPalette(palette);
    this->stateButton_->setPalette(palette);
    this->list_->setPalette(palette);

    auto inputPalette = palette;
    inputPalette.setColor(QPalette::Base, theme->splits.input.background);
    inputPalette.setColor(QPalette::Text, theme->splits.input.text);
    inputPalette.setColor(QPalette::Disabled, QPalette::Base,
                          theme->splits.input.background);
    this->commentInput_->setPalette(inputPalette);
    this->addButton_->setPalette(palette);
    this->shareComment_->setPalette(palette);
    this->characterCount_->setPalette(palette);

    auto *delegate =
        static_cast<ModeratorCommentDelegate *>(this->list_->itemDelegate());
    delegate->setColors(theme->tabs.regular.backgrounds.regular,
                        theme->tabs.regular.backgrounds.hover,
                        theme->tabs.selected.backgrounds.regular,
                        theme->window.text, theme->tabs.regular.text,
                        theme->accent,
                        theme->tabs.dividerLine);

    this->setFont(uiFont);
    this->list_->setFont(uiFont);
    this->list_->doItemsLayout();
    this->list_->viewport()->update();
}

void ModeratorCommentsView::reset()
{
    ++this->generation_;
    this->authToken_.clear();
    this->localCursor_.clear();
    this->sharedCursor_.clear();
    this->localLoadError_.clear();
    this->sharedLoadError_.clear();
    this->error_.clear();
    this->errorMessage_.clear();
    this->localComments_.clear();
    this->sharedComments_.clear();
    this->comments_.clear();
    this->loaded_ = false;
    this->localLoading_ = false;
    this->sharedLoading_ = false;
    this->localHasNext_ = true;
    this->sharedHasNext_ = true;
    this->localLoadFailed_ = false;
    this->sharedLoadFailed_ = false;
    this->listTruncated_ = false;
    this->sharingSettingLoading_ = false;
    this->sharingDisabled_ = true;
    this->sharingUnavailable_ = false;
    this->submitting_ = false;
    this->deleting_ = false;
    this->commentInput_->clear();
    this->model_->refresh();
    this->list_->scrollToTop();
    this->updateState();
}

void ModeratorCommentsView::retry()
{
    if (this->localLoadFailed_ || this->sharedLoadFailed_)
    {
        this->error_.clear();
        this->errorMessage_.clear();
        if (this->localLoadFailed_)
        {
            this->localLoadFailed_ = false;
            this->localLoadError_.clear();
            this->localHasNext_ = true;
            this->loadLocalPage(this->localComments_.isEmpty() &&
                                this->localCursor_.isEmpty());
        }
        if (this->sharedLoadFailed_)
        {
            this->sharedLoadFailed_ = false;
            this->sharedLoadError_.clear();
            this->sharedHasNext_ = true;
            this->loadSharedPage(this->sharedComments_.isEmpty() &&
                                 this->sharedCursor_.isEmpty());
        }
        return;
    }

    const auto draft = this->commentInput_->text();
    const bool shareable = this->shareComment_->isChecked();
    this->reset();
    this->commentInput_->setText(draft);
    this->shareComment_->setChecked(shareable);
    this->loadInitial();
}

void ModeratorCommentsView::loadInitial()
{
    if (this->channelId_.isEmpty() || this->channelLogin_.isEmpty() ||
        this->targetId_.isEmpty())
    {
        this->updateState();
        return;
    }

    QString authError;
    const auto auth = MoltorinoAuth::resolveModerationToken(
        this->channelId_, this->channelLogin_, &authError);
    if (!auth.hasToken() || !this->hasModerationAccess_)
    {
        this->authToken_.clear();
        this->error_ = authError;
        this->updateState();
        return;
    }

    this->authToken_ = auth.token;
    this->error_.clear();
    this->errorMessage_.clear();
    this->loadLocalPage(true);
    this->loadSharedPage(true);
    this->loadSharingSetting();
}

void ModeratorCommentsView::loadOlder()
{
    if (this->authToken_.isEmpty())
    {
        return;
    }
    if (this->localHasNext_ && !this->localLoading_ &&
        !this->localLoadFailed_ &&
        this->localComments_.size() < MAX_RENDERED_COMMENTS)
    {
        this->loadLocalPage(false);
    }
    if (this->sharedHasNext_ && !this->sharedLoading_ &&
        !this->sharedLoadFailed_ &&
        this->sharedComments_.size() < MAX_RENDERED_COMMENTS)
    {
        this->loadSharedPage(false);
    }
}

void ModeratorCommentsView::loadLocalPage(bool initial)
{
    if (this->localLoading_ || (!initial && !this->localHasNext_))
    {
        return;
    }
    this->localLoading_ = true;
    const auto generation = this->generation_;
    const auto cursor = initial ? QString() : this->localCursor_;
    const QPointer<ModeratorCommentsView> self(this);
    this->updateState();
    TwitchGql::getModeratorComments(
        this->channelId_, this->targetId_, cursor, this->authToken_,
        [self, generation, cursor](GqlModeratorCommentPage page) mutable {
            if (!self || generation != self->generation_)
            {
                return;
            }
            self->localLoading_ = false;
            self->loaded_ = true;
            self->localLoadFailed_ = false;
            self->localLoadError_.clear();
            const bool cursorAdvanced =
                !page.nextCursor.isEmpty() && page.nextCursor != cursor;
            self->localCursor_ = cursorAdvanced ? page.nextCursor : QString();
            self->localHasNext_ = page.hasNextPage && cursorAdvanced;
            self->appendComments(std::move(page.comments), false);
            self->updateState();
        },
        [self, generation](const QString &error) {
            if (!self || generation != self->generation_)
            {
                return;
            }
            self->localLoading_ = false;
            self->loaded_ = true;
            self->localHasNext_ = false;
            self->localLoadFailed_ = true;
            const auto normalized = MoltorinoAuth::normalizeAuthError(
                "loading moderator comments", error);
            self->localLoadError_ = normalized;
            if (normalized != error)
            {
                self->authToken_.clear();
                self->error_ = normalized;
            }
            self->updateState();
        });
}

void ModeratorCommentsView::loadSharedPage(bool initial)
{
    if (this->sharedLoading_ || (!initial && !this->sharedHasNext_))
    {
        return;
    }
    this->sharedLoading_ = true;
    const auto generation = this->generation_;
    const auto cursor = initial ? QString() : this->sharedCursor_;
    const QPointer<ModeratorCommentsView> self(this);
    this->updateState();
    TwitchGql::getSharedModeratorComments(
        this->channelId_, this->targetId_, cursor, this->authToken_,
        [self, generation, cursor](GqlModeratorCommentPage page) mutable {
            if (!self || generation != self->generation_)
            {
                return;
            }
            self->sharedLoading_ = false;
            self->loaded_ = true;
            self->sharedLoadFailed_ = false;
            self->sharedLoadError_.clear();
            const bool cursorAdvanced =
                !page.nextCursor.isEmpty() && page.nextCursor != cursor;
            self->sharedCursor_ = cursorAdvanced ? page.nextCursor : QString();
            self->sharedHasNext_ = page.hasNextPage && cursorAdvanced;
            self->appendComments(std::move(page.comments), true);
            self->updateState();
        },
        [self, generation](const QString &error) {
            if (!self || generation != self->generation_)
            {
                return;
            }
            self->sharedLoading_ = false;
            self->loaded_ = true;
            self->sharedHasNext_ = false;
            self->sharedLoadFailed_ = true;
            const auto normalized = MoltorinoAuth::normalizeAuthError(
                "loading moderator comments", error);
            self->sharedLoadError_ = normalized;
            if (normalized != error)
            {
                self->authToken_.clear();
                self->error_ = normalized;
            }
            self->updateState();
        });
}

void ModeratorCommentsView::loadSharingSetting()
{
    this->sharingSettingLoading_ = true;
    this->sharingDisabled_ = true;
    this->sharingUnavailable_ = false;
    const auto generation = this->generation_;
    const QPointer<ModeratorCommentsView> self(this);
    this->updateComposer();
    TwitchGql::getModeratorCommentSharingSetting(
        this->channelLogin_, this->channelId_, this->authToken_,
        [self, generation](bool disabled) {
            if (!self || generation != self->generation_)
            {
                return;
            }
            self->sharingSettingLoading_ = false;
            self->sharingDisabled_ = disabled;
            self->sharingUnavailable_ = false;
            if (disabled)
            {
                self->shareComment_->setChecked(false);
            }
            self->updateComposer();
        },
        [self, generation](const QString &error) {
            if (!self || generation != self->generation_)
            {
                return;
            }
            self->sharingSettingLoading_ = false;
            self->sharingDisabled_ = true;
            self->sharingUnavailable_ = true;
            self->shareComment_->setChecked(false);
            const auto normalized = MoltorinoAuth::normalizeAuthError(
                "checking moderator comment sharing", error);
            if (normalized != error)
            {
                self->authToken_.clear();
                self->error_ = normalized;
                self->updateState();
                return;
            }
            self->updateComposer();
        });
}

void ModeratorCommentsView::appendComments(
    QVector<GqlModeratorComment> comments, bool sharedSource)
{
    auto &destination = sharedSource ? this->sharedComments_
                                     : this->localComments_;
    QSet<QString> knownIds;
    knownIds.reserve(destination.size() + comments.size());
    for (const auto &comment : destination)
    {
        knownIds.insert(comment.id);
    }
    for (auto &comment : comments)
    {
        if (comment.id.isEmpty() || knownIds.contains(comment.id))
        {
            continue;
        }
        knownIds.insert(comment.id);
        destination.push_back(std::move(comment));
    }
    std::stable_sort(destination.begin(), destination.end(),
                     [](const auto &left, const auto &right) {
                         return commentDate(left.timestamp) >
                                commentDate(right.timestamp);
                     });
    if (destination.size() > MAX_RENDERED_COMMENTS)
    {
        destination.resize(MAX_RENDERED_COMMENTS);
    }
    this->rebuildList(true);
}

void ModeratorCommentsView::rebuildList(bool preserveScroll)
{
    const auto oldValue = this->list_->verticalScrollBar()->value();
    const auto generation = this->generation_;
    QVector<GqlModeratorComment> merged;
    merged.reserve(this->localComments_.size() +
                   this->sharedComments_.size());
    QSet<QString> knownIds;
    knownIds.reserve(this->localComments_.size() +
                     this->sharedComments_.size());
    for (const auto &comment : this->localComments_)
    {
        if (!comment.id.isEmpty() && !knownIds.contains(comment.id))
        {
            knownIds.insert(comment.id);
            merged.push_back(comment);
        }
    }
    for (const auto &comment : this->sharedComments_)
    {
        if (!comment.id.isEmpty() && !knownIds.contains(comment.id))
        {
            knownIds.insert(comment.id);
            merged.push_back(comment);
        }
    }
    std::stable_sort(merged.begin(), merged.end(),
                     [](const auto &left, const auto &right) {
                         return commentDate(left.timestamp) >
                                commentDate(right.timestamp);
                     });
    this->listTruncated_ =
        merged.size() > MAX_RENDERED_COMMENTS ||
        (this->localComments_.size() >= MAX_RENDERED_COMMENTS &&
         this->localHasNext_) ||
        (this->sharedComments_.size() >= MAX_RENDERED_COMMENTS &&
         this->sharedHasNext_);
    if (merged.size() > MAX_RENDERED_COMMENTS)
    {
        merged.resize(MAX_RENDERED_COMMENTS);
    }
    this->comments_ = std::move(merged);
    this->model_->refresh();
    if (preserveScroll)
    {
        this->list_->verticalScrollBar()->setValue(oldValue);
        QTimer::singleShot(0, this, [this, oldValue, generation] {
            if (generation != this->generation_)
            {
                return;
            }
            this->list_->verticalScrollBar()->setValue(oldValue);
        });
    }
}

void ModeratorCommentsView::updateState()
{
    const bool loading = this->localLoading_ || this->sharedLoading_;
    const bool sourceFailure =
        this->localLoadFailed_ || this->sharedLoadFailed_;
    this->statusLabel_->setToolTip({});
    this->stateButton_->setToolTip({});
    this->stateButton_->hide();
    if (this->activated_ &&
        (this->channelId_.isEmpty() || this->channelLogin_.isEmpty()))
    {
        this->statusLabel_->setText("Loading channel details...");
    }
    else if (this->activated_ && this->targetId_.isEmpty())
    {
        this->statusLabel_->setText(
            this->targetLookupFinished_
                ? "Moderator comments are unavailable for this user."
                : "Loading user details...");
    }
    else if (this->authToken_.isEmpty() && this->activated_)
    {
        this->statusLabel_->setText(
            this->error_.isEmpty()
                ? QStringLiteral(
                      "Connect a moderator account for #%1 to view comments.")
                      .arg(this->channelLogin_)
                : this->error_);
        this->statusLabel_->setToolTip(this->error_);
        this->stateButton_->setText("Open settings");
        this->stateButton_->show();
    }
    else if (loading && this->comments_.isEmpty())
    {
        this->statusLabel_->setText("Loading moderator comments...");
    }
    else if (!this->error_.isEmpty())
    {
        this->statusLabel_->setText(this->errorMessage_.isEmpty()
                                        ? "Something went wrong."
                                        : this->errorMessage_);
        this->statusLabel_->setToolTip(this->error_);
    }
    else if (this->loaded_ && this->comments_.isEmpty() && !loading &&
             !sourceFailure)
    {
        this->statusLabel_->setText("No moderator comments yet.");
    }
    else
    {
        const auto text = QStringLiteral("%1 comment%2")
                              .arg(this->comments_.size())
                              .arg(this->comments_.size() == 1
                                       ? QString()
                                       : QStringLiteral("s"));
        if (this->listTruncated_)
        {
            this->statusLabel_->setToolTip(
                "Showing the latest 500 moderator comments.");
        }
        this->statusLabel_->setText(text);
    }
    if (sourceFailure && !loading && this->error_.isEmpty())
    {
        QStringList errors;
        if (!this->localLoadError_.isEmpty())
        {
            errors.push_back(this->localLoadError_);
        }
        if (!this->sharedLoadError_.isEmpty())
        {
            errors.push_back(this->sharedLoadError_);
        }
        this->statusLabel_->setToolTip(errors.join(u'\n'));
        if (this->comments_.isEmpty())
        {
            this->statusLabel_->setText("Comments couldn't be loaded.");
        }
        this->stateButton_->setText("Try again");
        this->stateButton_->show();
    }
    this->updateComposer();
}

void ModeratorCommentsView::updateComposer()
{
    const auto length = this->commentInput_->text().size();
    const bool authenticated = !this->authToken_.isEmpty();
    const bool valid = this->commentInput_->text().trimmed().size() >= 2;
    this->characterCount_->setText(QStringLiteral("%1/500").arg(length));
    this->commentInput_->setEnabled(authenticated && !this->submitting_);
    this->addButton_->setEnabled(authenticated && valid && !this->submitting_);
    this->addButton_->setText(this->submitting_ ? "Adding..." : "Add comment");
    this->shareComment_->setEnabled(authenticated && !this->submitting_ &&
                                    !this->sharingSettingLoading_ &&
                                    !this->sharingDisabled_);
    if (this->sharingSettingLoading_)
    {
        this->shareComment_->setToolTip("Checking comment sharing...");
    }
    else if (this->sharingUnavailable_)
    {
        this->shareComment_->setToolTip(
            "Shared comments are unavailable right now.");
    }
    else if (this->sharingDisabled_)
    {
        this->shareComment_->setToolTip(
            "The channel owner has turned off shared moderator comments.");
    }
    else
    {
        this->shareComment_->setToolTip(
            "Visible to moderators in channels that share ban information.");
    }
}

void ModeratorCommentsView::submitComment()
{
    const auto text = this->commentInput_->text().trimmed();
    if (this->submitting_ || this->authToken_.isEmpty() || text.size() < 2 ||
        text.size() > 500)
    {
        return;
    }

    const bool shareable =
        this->shareComment_->isEnabled() && this->shareComment_->isChecked();
    this->submitting_ = true;
    this->error_.clear();
    this->errorMessage_.clear();
    this->updateState();
    const auto generation = this->generation_;
    const QPointer<ModeratorCommentsView> self(this);
    TwitchGql::createModeratorComment(
        this->channelId_, this->targetId_, text, shareable, this->authToken_,
        [self, generation](GqlModeratorComment comment) mutable {
            if (!self || generation != self->generation_)
            {
                return;
            }
            self->submitting_ = false;
            self->commentInput_->clear();
            self->appendComments({std::move(comment)}, false);
            self->updateState();
        },
        [self, generation](const QString &error) {
            if (!self || generation != self->generation_)
            {
                return;
            }
            self->submitting_ = false;
            const auto normalized = MoltorinoAuth::normalizeAuthError(
                "adding a moderator comment", error);
            self->error_ = normalized;
            if (normalized != error)
            {
                self->authToken_.clear();
                self->errorMessage_.clear();
            }
            else
            {
                self->errorMessage_ = "Comment couldn't be added.";
            }
            self->updateState();
        });
}

void ModeratorCommentsView::deleteComment(const QString &commentId)
{
    const auto comment = std::find_if(
        this->comments_.cbegin(), this->comments_.cend(),
        [&commentId](const auto &candidate) {
            return candidate.id == commentId && !candidate.shared;
        });
    if (comment == this->comments_.cend() || this->deleting_ ||
        this->authToken_.isEmpty())
    {
        return;
    }
    const auto generation = this->generation_;
    const QPointer<ModeratorCommentsView> self(this);
    QPointer<QMessageBox> confirmation = new QMessageBox(
        QMessageBox::Question, "Delete comment",
        "Delete this moderator comment?", QMessageBox::Cancel, this);
    auto *deleteButton = confirmation->addButton(
        "Delete comment", QMessageBox::DestructiveRole);
    confirmation->setDefaultButton(QMessageBox::Cancel);
    installMoltorinoDialogTheme(confirmation);
    confirmation->exec();
    if (!self || !confirmation)
    {
        return;
    }
    const bool confirmed = confirmation->clickedButton() == deleteButton;
    delete confirmation;
    if (!confirmed ||
        generation != this->generation_ || this->authToken_.isEmpty() ||
        this->deleting_)
    {
        return;
    }

    this->deleting_ = true;
    this->error_.clear();
    this->errorMessage_.clear();
    this->updateState();
    TwitchGql::deleteModeratorComment(
        commentId, this->channelId_, this->authToken_,
        [self, generation, commentId] {
            if (!self || generation != self->generation_)
            {
                return;
            }
            self->deleting_ = false;
            self->error_.clear();
            self->errorMessage_.clear();
            const auto removeComment = [&commentId](auto &comments) {
                comments.removeIf([&commentId](const auto &comment) {
                    return comment.id == commentId;
                });
            };
            removeComment(self->localComments_);
            removeComment(self->sharedComments_);
            self->rebuildList(true);
            self->updateState();
        },
        [self, generation](const QString &error) {
            if (!self || generation != self->generation_)
            {
                return;
            }
            self->deleting_ = false;
            const auto normalized = MoltorinoAuth::normalizeAuthError(
                "deleting a moderator comment", error);
            self->error_ = normalized;
            if (normalized != error)
            {
                self->authToken_.clear();
                self->errorMessage_.clear();
            }
            else
            {
                self->errorMessage_ = "Comment couldn't be deleted.";
            }
            self->updateState();
        });
}

void ModeratorCommentsView::showCommentMenu(const QPoint &position)
{
    const auto index = this->list_->indexAt(position);
    const auto row = index.row();
    if (row < 0 || row >= this->comments_.size() ||
        this->comments_.at(row).shared)
    {
        return;
    }
    this->list_->setCurrentIndex(index);
    auto *menu = new QMenu(this);
    const auto commentId = this->comments_.at(row).id;
    const auto generation = this->generation_;
    menu->addAction("Delete comment", this, [this, commentId, generation] {
        if (generation == this->generation_)
        {
            this->deleteComment(commentId);
        }
    });
    QObject::connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->popup(this->list_->viewport()->mapToGlobal(position));
}

}
