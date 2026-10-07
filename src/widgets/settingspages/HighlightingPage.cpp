// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/HighlightingPage.hpp"

#include "Application.hpp"
#include "controllers/highlights/BadgeHighlightModel.hpp"
#include "controllers/highlights/ChattyHighlightImport.hpp"
#include "controllers/highlights/HighlightBadge.hpp"
#include "controllers/highlights/HighlightBlacklistModel.hpp"
#include "controllers/highlights/HighlightBlacklistUser.hpp"
#include "controllers/highlights/HighlightController.hpp"
#include "controllers/highlights/HighlightModel.hpp"
#include "controllers/highlights/HighlightPhrase.hpp"
#include "controllers/highlights/HighlightWordList.hpp"
#include "controllers/highlights/UserHighlightModel.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "singletons/Settings.hpp"
#include "util/Helpers.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/dialogs/BadgePickerDialog.hpp"
#include "widgets/dialogs/ColorPickerDialog.hpp"
#include "widgets/helper/color/Checkerboard.hpp"
#include "widgets/helper/color/ColorItemDelegate.hpp"
#include "widgets/helper/EditableModelView.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTabWidget>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextEdit>
#include <QTimer>
#include <QTreeWidget>

#include <algorithm>
#include <initializer_list>
#include <optional>

namespace chatterino {

namespace {

QList<DisplayBadge> availableBadges = {
    {"Broadcaster", "broadcaster"},
    {"Admin", "admin"},
    {"Staff", "staff"},
    {"Moderator", "moderator"},
    {"Lead Moderator", "lead_moderator"},
    {"Verified", "partner"},
    {"VIP", "vip"},
    {"Founder", "founder"},
    {"Subscriber", "subscriber"},
    {"Predicted Blue", "predictions/blue-1,predictions/blue-2"},
    {"Predicted Pink", "predictions/pink-2,predictions/pink-1"},
};

constexpr int HIGHLIGHT_PATTERN_MIN_WIDTH = 75;
constexpr int HIGHLIGHT_MATCH_APPEARANCE_WIDTH = 118;
constexpr int HIGHLIGHT_APPLIES_IN_WIDTH = 92;
constexpr int HIGHLIGHT_PAGE_MIN_WIDTH = 835;

void alignFormLabels(QFormLayout *layout, const QWidget *reference,
                     std::initializer_list<QWidget *> fields)
{
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    layout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->setHorizontalSpacing(10);
    layout->setVerticalSpacing(8);

    const auto labelWidth =
        reference->fontMetrics().horizontalAdvance("Message background:") + 8;
    for (auto *field : fields)
    {
        if (auto *label = layout->labelForField(field))
        {
            label->setFixedWidth(labelWidth);
        }
    }
}

class MatchAppearanceItemDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem cleanOption(option);
        this->initStyleOption(&cleanOption, index);
        cleanOption.features.setFlag(QStyleOptionViewItem::HasCheckIndicator,
                                     false);
        cleanOption.checkState = Qt::Unchecked;

        const auto colorData = index.data(Qt::DecorationRole);
        if (colorData.typeId() != QMetaType::QColor)
        {
            QStyledItemDelegate::paint(painter, cleanOption, index);
            return;
        }

        QStyleOptionViewItem background(cleanOption);
        background.text.clear();
        background.icon = {};
        const auto *widget = option.widget;
        auto *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &background, painter,
                           widget);

        const auto markerStyle = static_cast<HighlightMatchStyle>(
            index.data(HighlightModel::MatchStyleRole).toInt());
        const bool hasPaint =
            !index.data(HighlightModel::MatchPaintIDRole).toString().isEmpty();

        painter->save();
        auto textLeft = option.rect.left() + 5;
        if (markerStyle != HighlightMatchStyle::None)
        {
            const auto swatchSize =
                std::clamp(option.rect.height() - 8, 10, 18);
            QRect swatchRect{textLeft,
                             option.rect.center().y() - swatchSize / 2,
                             swatchSize, swatchSize};
            const auto color = colorData.value<QColor>();
            if (color.alpha() != 255)
            {
                drawCheckerboard(*painter, swatchRect,
                                 std::max(2, swatchSize / 3));
            }
            painter->fillRect(swatchRect, color);
            painter->setPen(option.palette.color(QPalette::Mid));
            painter->drawRect(swatchRect.adjusted(0, 0, -1, -1));
            textLeft += swatchSize + 5;
        }

        const auto textRight = option.rect.right() - 4;
        const QRect textRect{textLeft, option.rect.top(),
                             std::max(0, textRight - textLeft + 1),
                             option.rect.height()};
        const auto selected = option.state.testFlag(QStyle::State_Selected);
        painter->setPen(selected
                            ? option.palette.color(QPalette::HighlightedText)
                            : option.palette.color(QPalette::Text));
        auto styleName = markerStyle == HighlightMatchStyle::None && hasPaint
                             ? QStringLiteral("7TV")
                             : highlightMatchStyleName(markerStyle);
        if (hasPaint && markerStyle != HighlightMatchStyle::None)
        {
            styleName += QStringLiteral(", 7TV");
        }
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                          option.fontMetrics.elidedText(
                              styleName, Qt::ElideRight, textRect.width()));
        painter->restore();
    }
};

struct ParsedWordListTerms {
    std::vector<QString> terms;
    qsizetype blanks{};
    qsizetype duplicates{};
    qsizetype oversized{};
    qsizetype overLimit{};
    qsizetype overCharacterBudget{};
    qsizetype invalidRegex{};
    QString firstRegexError;
};

ParsedWordListTerms parseWordListTerms(const QString &text, bool caseSensitive,
                                       bool isRegex)
{
    ParsedWordListTerms parsed;
    if (text.size() > CHATTY_HIGHLIGHT_IMPORT_MAX_SOURCE_CHARACTERS)
    {
        parsed.overCharacterBudget = 1;
        return parsed;
    }
    QSet<QString> seen;
    const auto maxTerms =
        static_cast<qsizetype>(isRegex ? HIGHLIGHT_WORD_LIST_MAX_REGEX_RULES
                                       : HIGHLIGHT_WORD_LIST_MAX_LITERAL_RULES);
    const auto maxLength = static_cast<qsizetype>(
        isRegex ? HIGHLIGHT_WORD_LIST_MAX_REGEX_RULE_LENGTH
                : HIGHLIGHT_WORD_LIST_MAX_LITERAL_RULE_LENGTH);
    const auto regexOptions =
        QRegularExpression::UseUnicodePropertiesOption |
        (caseSensitive ? QRegularExpression::NoPatternOption
                       : QRegularExpression::CaseInsensitiveOption);
    qsizetype lineNumber = 0;
    qsizetype acceptedCharacters = 0;
    for (auto line : text.split(u'\n', Qt::KeepEmptyParts))
    {
        ++lineNumber;
        if (line.endsWith(u'\r'))
        {
            line.chop(1);
        }
        line = line.trimmed();
        if (!line.isEmpty() && line.front() == QChar::ByteOrderMark)
        {
            line.remove(0, 1);
        }
        if (line.isEmpty())
        {
            ++parsed.blanks;
            continue;
        }
        if (line.size() > maxLength)
        {
            ++parsed.oversized;
            continue;
        }
        const auto key = isRegex || caseSensitive ? line : line.toCaseFolded();
        if (seen.contains(key))
        {
            ++parsed.duplicates;
            continue;
        }
        if (static_cast<qsizetype>(parsed.terms.size()) >= maxTerms)
        {
            ++parsed.overLimit;
            continue;
        }
        if (acceptedCharacters + line.size() >
            static_cast<qsizetype>(HIGHLIGHT_WORD_LIST_MAX_RULE_CHARACTERS))
        {
            ++parsed.overCharacterBudget;
            continue;
        }
        if (isRegex)
        {
            const QRegularExpression regex(line, regexOptions);
            if (!regex.isValid())
            {
                ++parsed.invalidRegex;
                if (parsed.firstRegexError.isEmpty())
                {
                    parsed.firstRegexError = QString("Line %1: %2")
                                                 .arg(lineNumber)
                                                 .arg(regex.errorString());
                }
                continue;
            }
        }
        seen.insert(key);
        acceptedCharacters += line.size();
        parsed.terms.emplace_back(std::move(line));
    }
    return parsed;
}

QString normalizeImportedWordListText(const QString &text,
                                      bool commasSeparateRules)
{
    if (!commasSeparateRules)
    {
        return text;
    }

    QStringList terms;
    QString current;
    bool quoted = false;
    const auto flush = [&] {
        auto term = current.trimmed();
        if (!term.isEmpty())
        {
            terms.append(std::move(term));
        }
        current.clear();
    };
    for (qsizetype index = 0; index < text.size(); ++index)
    {
        const auto ch = text.at(index);
        if (ch == u'"')
        {
            if (quoted && index + 1 < text.size() && text.at(index + 1) == u'"')
            {
                current.append(u'"');
                ++index;
            }
            else
            {
                quoted = !quoted;
            }
        }
        else if (!quoted && (ch == u',' || ch == u'\n' || ch == u'\r'))
        {
            flush();
        }
        else if (quoted && (ch == u'\n' || ch == u'\r'))
        {
            if (!current.isEmpty() && !current.endsWith(u' '))
            {
                current.append(u' ');
            }
        }
        else
        {
            current.append(ch);
        }
    }
    flush();
    return terms.join(u'\n');
}

struct ImportedWordListFile {
    QString text;
    QString suggestedName;
    bool chattySyntax{};
    bool suggestedRegex{};
};

std::optional<ImportedWordListFile> openWordListFile(QWidget *parent,
                                                     const QString &title)
{
    const QPointer<QWidget> guardedParent(parent);
    const auto fileName = QFileDialog::getOpenFileName(
        parent, title, QString(),
        "Moderation list files (*.txt *.list *.csv);;Text files (*.txt "
        "*.list);;CSV files (*.csv);;All files (*)");
    if (!guardedParent || fileName.isEmpty())
    {
        return std::nullopt;
    }

    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly))
    {
        QMessageBox::warning(parent, "Could not import list",
                             "Moltorino could not open the selected file.");
        return std::nullopt;
    }
    if (file.size() > 4 * 1024 * 1024)
    {
        QMessageBox::warning(parent, "Moderation list is too large",
                             "The maximum import size is 4 MiB.");
        return std::nullopt;
    }

    const auto rawText = QString::fromUtf8(file.readAll());
    const bool chattySyntax = looksLikeChattyHighlightRules(rawText);
    const bool suggestedRegex =
        !chattySyntax && looksLikeRegularExpressionList(rawText);
    auto commasSeparateRules =
        !chattySyntax && !suggestedRegex &&
        QFileInfo(fileName).suffix().compare("csv", Qt::CaseInsensitive) == 0;
    if (!chattySyntax && !commasSeparateRules && rawText.contains(u','))
    {
        const auto choice = QMessageBox::question(
            parent, "Choose import format",
            "Use commas to separate rules?\n\nChoose No to keep each line "
            "as one rule.",
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
            QMessageBox::No);
        if (choice == QMessageBox::Cancel)
        {
            return std::nullopt;
        }
        commasSeparateRules = choice == QMessageBox::Yes;
    }

    if (!guardedParent)
    {
        return std::nullopt;
    }
    return ImportedWordListFile{
        chattySyntax
            ? rawText
            : normalizeImportedWordListText(rawText, commasSeparateRules),
        QFileInfo(fileName).completeBaseName().trimmed(),
        chattySyntax,
        suggestedRegex,
    };
}

std::vector<QString> parseChannels(const QString &text)
{
    std::vector<QString> channels;
    QSet<QString> seen;
    for (auto channel :
         text.split(QRegularExpression(QStringLiteral("[,\\r\\n]")),
                    Qt::SkipEmptyParts))
    {
        auto target = makeHighlightChannelTarget(
            HighlightChannelTargetPlatform::Any, std::move(channel));
        if (!target)
        {
            continue;
        }

        auto key = normalizeHighlightChannelName(target->channel);
        if (seen.contains(key))
        {
            continue;
        }
        seen.insert(std::move(key));
        channels.emplace_back(std::move(target->channel));
    }
    return channels;
}

class ExactMatchPreview final : public QWidget
{
public:
    explicit ExactMatchPreview(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        this->setMinimumHeight(42);
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setAppearance(QColor color, HighlightMatchStyle style,
                       std::shared_ptr<Paint> paint)
    {
        this->color_ = std::move(color);
        this->style_ = style;
        this->paint_ = std::move(paint);
        this->update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(this->rect(), this->palette().base());

        const QString prefix = "Example message with ";
        const QString matched = "matched text";
        const auto metrics = this->fontMetrics();
        const auto baseline =
            (this->height() + metrics.ascent() - metrics.descent()) / 2;
        const auto left = 10;
        painter.setFont(this->font());
        painter.setPen(this->palette().text().color());
        painter.drawText(left, baseline, prefix);

        QRectF markerRect(left + metrics.horizontalAdvance(prefix) - 2,
                          (this->height() - metrics.height()) / 2.0,
                          metrics.horizontalAdvance(matched) + 4,
                          metrics.height());
        FragmentHighlight highlight{
            HighlightMatch{.color = this->color_, .style = this->style_}, {}};
        paintFragmentHighlightBackground(painter, markerRect, highlight);

        const auto textRect = markerRect.adjusted(2, 0, -2, 0);
        if (this->paint_ && getSettings()->displaySevenTVPaints)
        {
            const auto pixmap = this->paint_->getPixmap(
                matched, this->font(), this->palette().text().color(),
                textRect.size(), 1.F, this->devicePixelRatioF(), {}, false);
            painter.drawPixmap(textRect, pixmap, QRectF());
        }
        else
        {
            painter.setPen(this->palette().text().color());
            painter.drawText(textRect.left(), baseline, matched);
        }
        paintFragmentHighlightForeground(painter, markerRect, highlight);
    }

private:
    QColor color_{Qt::red};
    HighlightMatchStyle style_{HighlightMatchStyle::Outline};
    std::shared_ptr<Paint> paint_;
};

class MatchAppearanceControls final : public QWidget
{
public:
    MatchAppearanceControls(QColor color, HighlightMatchStyle style,
                            QString paintID, QWidget *parent = nullptr,
                            bool loadExistingPaint = true,
                            bool allowPaint = true)
        : QWidget(parent)
        , color_(std::move(color))
        , allowPaint_(allowPaint)
    {
        auto *layout = new QFormLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);

        this->style_ = new QComboBox(this);
        for (const auto candidate :
             {HighlightMatchStyle::None, HighlightMatchStyle::Outline,
              HighlightMatchStyle::Fill, HighlightMatchStyle::OutlineAndFill,
              HighlightMatchStyle::Underline})
        {
            this->style_->addItem(highlightMatchStyleName(candidate),
                                  static_cast<int>(candidate));
        }
        const auto styleIndex = this->style_->findData(static_cast<int>(style));
        this->style_->setCurrentIndex(styleIndex >= 0 ? styleIndex : 1);
        layout->addRow("Marker style:", this->style_);

        this->colorButton_ = new QPushButton(this);
        layout->addRow("Marker color:", this->colorButton_);

        auto *paintRow = new QWidget(this);
        auto *paintLayout = new QHBoxLayout(paintRow);
        paintLayout->setContentsMargins(0, 0, 0, 0);
        this->paintID_ = new QLineEdit(
            this->allowPaint_
                ? SeventvPaints::normalizePaintID(std::move(paintID))
                : QString{},
            paintRow);
        this->paintID_->setPlaceholderText("Paste a 7TV paint ID");
        this->paintID_->setClearButtonEnabled(true);
        this->paintID_->setToolTip(
            "Use a 24 or 26 character 7TV paint ID. Paint only affects the "
            "matched text.");
        this->checkPaint_ = new QPushButton("Check", paintRow);
        paintLayout->addWidget(this->paintID_, 1);
        paintLayout->addWidget(this->checkPaint_);
        layout->addRow("7TV text paint:", paintRow);
        paintRow->setVisible(this->allowPaint_);
        if (auto *paintLabel = layout->labelForField(paintRow))
        {
            paintLabel->setVisible(this->allowPaint_);
        }

        this->paintStatus_ = new QLabel(this);
        this->paintStatus_->setWordWrap(true);
        layout->addRow(QString(), this->paintStatus_);

        this->preview_ = new ExactMatchPreview(this);
        layout->addRow("Preview:", this->preview_);
        alignFormLabels(layout, this,
                        {this->style_, this->colorButton_, paintRow,
                         this->paintStatus_, this->preview_});

        QObject::connect(this->style_, &QComboBox::currentIndexChanged, this,
                         [this] {
                             this->updateState();
                         });
        QObject::connect(
            this->colorButton_, &QPushButton::clicked, this, [this] {
                auto *picker = new ColorPickerDialog(this->color_, this);
                QObject::connect(picker, &ColorPickerDialog::colorConfirmed,
                                 this, [this](const QColor &color) {
                                     if (color.isValid())
                                     {
                                         this->color_ = color;
                                         this->updateState();
                                     }
                                 });
                picker->show();
            });
        QObject::connect(this->paintID_, &QLineEdit::textChanged, this, [this] {
            this->updateState();
        });
        QObject::connect(this->checkPaint_, &QPushButton::clicked, this,
                         [this] {
                             this->requestPaint(true);
                         });
        this->paintConnections_.managedConnect(
            getApp()->getSeventvPaints()->paintLoadStatusChanged,
            [this](const QString &paintID, SeventvPaintLoadStatus) {
                if (paintID == this->paintID())
                {
                    this->updateState();
                }
            });

        this->updateState();
        if (this->allowPaint_ && loadExistingPaint &&
            getSettings()->displaySevenTVPaints && this->hasValidPaintID() &&
            !this->paintID().isEmpty())
        {
            this->requestPaint(false);
        }
    }

    QColor color() const
    {
        return this->color_;
    }

    HighlightMatchStyle style() const
    {
        return static_cast<HighlightMatchStyle>(
            this->style_->currentData().toInt());
    }

    QString paintID() const
    {
        if (!this->allowPaint_)
        {
            return {};
        }
        return SeventvPaints::normalizePaintID(this->paintID_->text());
    }

    bool hasValidPaintID() const
    {
        if (!this->allowPaint_)
        {
            return true;
        }
        return this->paintID_->text().trimmed().isEmpty() ||
               SeventvPaints::isValidPaintID(this->paintID_->text());
    }

private:
    void requestPaint(bool retry)
    {
        if (!this->hasValidPaintID() || this->paintID().isEmpty())
        {
            this->updateState();
            return;
        }
        {
            const QSignalBlocker blocker(this->paintID_);
            this->paintID_->setText(this->paintID());
        }
        getApp()->getSeventvPaints()->loadPaintByID(this->paintID(), retry);
        this->updateState();
    }

    void updateState()
    {
        this->colorButton_->setText(this->color_.name(QColor::HexArgb));
        QPixmap swatch(16, 16);
        swatch.fill(this->color_);
        this->colorButton_->setIcon(QIcon(swatch));

        const bool markerEnabled = this->style() != HighlightMatchStyle::None;
        this->colorButton_->setEnabled(markerEnabled);
        if (!this->allowPaint_)
        {
            this->paintID_->setEnabled(false);
            this->checkPaint_->setEnabled(false);
            this->paintStatus_->clear();
            this->paintStatus_->setVisible(false);
            this->preview_->setAppearance(this->color_, this->style(), {});
            return;
        }
        this->paintID_->setEnabled(true);
        this->checkPaint_->setEnabled(
            SeventvPaints::isValidPaintID(this->paintID_->text()));

        QString status;
        std::shared_ptr<Paint> paint;
        if (this->paintID_->text().trimmed().isEmpty())
        {
            status = markerEnabled
                         ? QString()
                         : "Add a paint ID to highlight without a marker.";
        }
        else if (!this->hasValidPaintID())
        {
            status = "Enter a valid 24 or 26 character 7TV paint ID.";
        }
        else
        {
            auto *paints = getApp()->getSeventvPaints();
            const auto paintID = this->paintID();
            paint = paints->getPaintByID(paintID);
            switch (paints->getPaintLoadStatus(paintID))
            {
                case SeventvPaintLoadStatus::Unknown:
                    status = "Press Check to load this paint.";
                    break;
                case SeventvPaintLoadStatus::Loading:
                    status = "Loading paint from 7TV...";
                    break;
                case SeventvPaintLoadStatus::Ready:
                    status = paint && !paint->getName().isEmpty()
                                 ? "Paint: " + paint->getName()
                                 : "Paint loaded.";
                    if (!getSettings()->displaySevenTVPaints)
                    {
                        status += " Hidden by your 7TV paint setting.";
                    }
                    break;
                case SeventvPaintLoadStatus::NotFound:
                    status = "7TV did not find a paint with this ID.";
                    break;
                case SeventvPaintLoadStatus::Failed:
                    status = "Paint lookup failed. Press Check to retry.";
                    break;
            }
        }
        this->paintStatus_->setText(status);
        this->paintStatus_->setVisible(!status.isEmpty());
        this->preview_->setAppearance(this->color_, this->style(),
                                      std::move(paint));
    }

    QComboBox *style_{};
    QPushButton *colorButton_{};
    QLineEdit *paintID_{};
    QPushButton *checkPaint_{};
    QLabel *paintStatus_{};
    ExactMatchPreview *preview_{};
    QColor color_;
    bool allowPaint_{};
    pajlada::Signals::SignalHolder paintConnections_;
};

class ChattyHighlightImportDialog final : public QDialog
{
public:
    ChattyHighlightImportDialog(QWidget *parent, QString source = {},
                                QString suggestedName = {})
        : QDialog(parent)
        , analysisTimer_(this)
    {
        this->setWindowTitle("Import Chatty highlights");
        this->setMinimumSize(760, 680);

        auto *layout = new QVBoxLayout(this);
        layout->setSpacing(10);

        auto *details = new QGroupBox("Import details", this);
        auto *detailsForm = new QFormLayout(details);
        this->name_ = new QLineEdit(details);
        this->name_->setPlaceholderText("Example: Twitch moderation rules");
        this->name_->setText(std::move(suggestedName));
        detailsForm->addRow("List name:", this->name_);
        alignFormLabels(detailsForm, this, {this->name_});
        layout->addWidget(details);

        auto *sourceGroup = new QGroupBox("Chatty rules", this);
        auto *sourceLayout = new QVBoxLayout(sourceGroup);
        this->source_ = new QPlainTextEdit(sourceGroup);
        this->source_->setPlaceholderText(
            "config:any,!s regi:example\\s+rule\n"
            "chan:somechannel regwi:another(?:rule)?");
        this->source_->setPlainText(std::move(source));
        sourceLayout->addWidget(this->source_, 1);
        auto *sourceFooter = new QHBoxLayout();
        this->summary_ = new QLabel(sourceGroup);
        this->summary_->setWordWrap(true);
        auto *load = new QPushButton("Load from file", sourceGroup);
        load->setMinimumWidth(std::max(115, load->sizeHint().width()));
        sourceFooter->addWidget(this->summary_, 1);
        sourceFooter->addWidget(load);
        sourceLayout->addLayout(sourceFooter);
        layout->addWidget(sourceGroup, 1);

        this->issues_ = new QPlainTextEdit(this);
        this->issues_->setReadOnly(true);
        this->issues_->setMaximumHeight(120);
        this->issues_->setPlaceholderText(
            "Rules that need review appear here.");
        layout->addWidget(this->issues_);

        auto *behavior = new QGroupBox("List settings", this);
        auto *behaviorLayout = new QVBoxLayout(behavior);
        auto *actionsForm = new QFormLayout();
        auto *actionsWidget = new QWidget(behavior);
        auto *actions = new QHBoxLayout(actionsWidget);
        actions->setContentsMargins(0, 0, 0, 0);
        actions->setSpacing(18);
        this->showInMentions_ =
            new QCheckBox("Show in Mentions", actionsWidget);
        this->alert_ = new QCheckBox("Flash taskbar", actionsWidget);
        this->sound_ = new QCheckBox("Play sound", actionsWidget);
        actions->addWidget(this->showInMentions_);
        actions->addWidget(this->alert_);
        actions->addWidget(this->sound_);
        actions->addStretch(1);
        actionsForm->addRow("Actions:", actionsWidget);

        this->color_ =
            *ColorProvider::instance().color(ColorType::SelfHighlight);
        this->colorButton_ = new QPushButton(behavior);
        actionsForm->addRow("Message background:", this->colorButton_);
        alignFormLabels(actionsForm, this, {actionsWidget, this->colorButton_});
        behaviorLayout->addLayout(actionsForm);
        this->matchAppearance_ = new MatchAppearanceControls(
            defaultNewHighlightMatchColor(), HighlightMatchStyle::Outline, {},
            behavior);
        behaviorLayout->addWidget(this->matchAppearance_);
        layout->addWidget(behavior);

        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        this->importButton_ = buttons->button(QDialogButtonBox::Save);
        this->importButton_->setText("Import");
        layout->addWidget(buttons);

        this->analysisTimer_.setSingleShot(true);
        this->analysisTimer_.setInterval(250);
        QObject::connect(&this->analysisTimer_, &QTimer::timeout, this, [this] {
            this->analyze();
        });
        QObject::connect(this->source_, &QPlainTextEdit::textChanged, this,
                         [this] {
                             this->analysisTimer_.start();
                         });
        QObject::connect(this->name_, &QLineEdit::textChanged, this, [this] {
            this->updateImportButton();
        });
        QObject::connect(load, &QPushButton::clicked, this, [this] {
            const QPointer<ChattyHighlightImportDialog> self(this);
            const auto fileName = QFileDialog::getOpenFileName(
                this, "Load Chatty highlights", QString(),
                "Chatty and text files (*.ini *.txt *.list);;All files (*)");
            if (!self || fileName.isEmpty())
            {
                return;
            }
            QFile file(fileName);
            if (!file.open(QIODevice::ReadOnly))
            {
                QMessageBox::warning(
                    this, "Could not load Chatty highlights",
                    "Moltorino could not open the selected file.");
                return;
            }
            if (file.size() > 4 * 1024 * 1024)
            {
                QMessageBox::warning(this, "Chatty highlight file is too large",
                                     "The maximum import size is 4 MiB.");
                return;
            }
            this->source_->setPlainText(QString::fromUtf8(file.readAll()));
            if (this->name_->text().trimmed().isEmpty())
            {
                this->name_->setText(
                    QFileInfo(fileName).completeBaseName().trimmed());
            }
        });
        QObject::connect(
            this->colorButton_, &QPushButton::clicked, this, [this] {
                auto *picker = new ColorPickerDialog(this->color_, this);
                QObject::connect(picker, &ColorPickerDialog::colorConfirmed,
                                 this, [this](const QColor &color) {
                                     if (color.isValid())
                                     {
                                         this->color_ = color;
                                         this->updateColorButton();
                                     }
                                 });
                picker->show();
            });
        QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                         &QDialog::reject);
        QObject::connect(buttons, &QDialogButtonBox::accepted, this, [this] {
            const QPointer<ChattyHighlightImportDialog> self(this);
            this->analysisTimer_.stop();
            this->analyze();
            if (this->name_->text().trimmed().isEmpty())
            {
                QMessageBox::warning(this, "Missing name",
                                     "Give the imported lists a name.");
                return;
            }
            if (this->parsed_.importedRuleCount() == 0)
            {
                QMessageBox::warning(this, "No compatible rules",
                                     "No supported Chatty rules were found.");
                return;
            }
            if (!this->matchAppearance_->hasValidPaintID())
            {
                QMessageBox::warning(
                    this, "Invalid 7TV paint ID",
                    "Enter a valid 24 or 26 character paint ID, or clear the "
                    "field.");
                return;
            }

            bool large = false;
            for (const auto &group : this->parsed_.groups)
            {
                QStringList lines;
                for (const auto &regex : group.regexes)
                {
                    lines.append(regex);
                }
                const auto checked =
                    parseWordListTerms(lines.join(u'\n'), true, true);
                if (checked.overLimit > 0 || checked.overCharacterBudget > 0 ||
                    checked.oversized > 0)
                {
                    QMessageBox::warning(
                        this, "Converted list is too large",
                        "A generated list exceeds the limit: 2,000 rules, "
                        "512 characters per rule, or 500,000 characters "
                        "total. Split the source list and try again.");
                    return;
                }
                large = large || group.regexes.size() >
                                     HIGHLIGHT_WORD_LIST_REGEX_WARNING_RULES;
            }
            const auto skipped =
                this->parsed_.invalidRules + this->parsed_.unsupportedRules;
            if (skipped > 0 &&
                QMessageBox::warning(
                    this, "Some rules cannot be imported",
                    QString("%1 %2 review and will be skipped. Import the "
                            "other %3?")
                        .arg(skipped)
                        .arg(skipped == 1 ? "rule needs" : "rules need")
                        .arg(this->parsed_.importedRuleCount()),
                    QMessageBox::Yes | QMessageBox::Cancel,
                    QMessageBox::Cancel) != QMessageBox::Yes)
            {
                return;
            }
            if (!self)
            {
                return;
            }
            if (large &&
                QMessageBox::warning(
                    this, "Large regular expression list",
                    "One or more lists contain over 250 regular expressions. "
                    "Complex rules may slow busy chats. Continue?",
                    QMessageBox::Yes | QMessageBox::Cancel,
                    QMessageBox::Cancel) != QMessageBox::Yes)
            {
                return;
            }
            if (self)
            {
                this->accept();
            }
        });

        this->analyze();
        this->updateColorButton();
    }

    std::vector<HighlightWordList> values() const
    {
        std::vector<HighlightWordList> lists;
        lists.reserve(this->parsed_.groups.size());
        const auto baseName = this->name_->text().trimmed();
        const bool multiple = this->parsed_.groups.size() > 1;
        for (const auto &group : this->parsed_.groups)
        {
            auto name = baseName;
            if (multiple)
            {
                if (group.channels.empty())
                {
                    name += QStringLiteral(" (all channels)");
                }
                else if (group.channels.size() == 1)
                {
                    name += QStringLiteral(" (%1)").arg(group.channels[0]);
                }
                else
                {
                    name += QStringLiteral(" (%1 channels)")
                                .arg(group.channels.size());
                }
            }
            lists.emplace_back(
                std::move(name), true, group.regexes, group.channels, true,
                this->showInMentions_->isChecked(), this->alert_->isChecked(),
                this->sound_->isChecked(), QString(), this->color_, true,
                this->matchAppearance_->color(),
                this->matchAppearance_->style(),
                this->matchAppearance_->paintID(),
                HighlightWordListPlatform::Twitch);
        }
        return lists;
    }

private:
    void analyze()
    {
        this->parsed_ =
            importChattyHighlightRules(this->source_->toPlainText());
        QString summary =
            QString("Found %1 %2 across %3 Twitch %4. %5 copied, %6 "
                    "adjusted.")
                .arg(this->parsed_.importedRuleCount())
                .arg(this->parsed_.importedRuleCount() == 1 ? "rule" : "rules")
                .arg(this->parsed_.groups.size())
                .arg(this->parsed_.groups.size() == 1 ? "list" : "lists")
                .arg(this->parsed_.exactRules)
                .arg(this->parsed_.adjustedRules);
        if (this->parsed_.duplicateRules > 0)
        {
            summary +=
                QString(" %1 %2 ignored.")
                    .arg(this->parsed_.duplicateRules)
                    .arg(this->parsed_.duplicateRules == 1 ? "duplicate was"
                                                           : "duplicates were");
        }
        if (this->parsed_.hasSkippedRules())
        {
            const auto skipped =
                this->parsed_.invalidRules + this->parsed_.unsupportedRules;
            summary += QString(" %1 %2 review.")
                           .arg(skipped)
                           .arg(skipped == 1 ? "needs" : "need");
            if (skipped > static_cast<qsizetype>(this->parsed_.issues.size()))
            {
                summary += QString(" Showing the first %1.")
                               .arg(this->parsed_.issues.size());
            }
        }
        this->summary_->setText(summary);

        QStringList details;
        for (const auto &issue : this->parsed_.issues)
        {
            if (issue.line > 0)
            {
                details.append(QString("Line %1: %2\n%3")
                                   .arg(issue.line)
                                   .arg(issue.reason, issue.source));
            }
            else
            {
                details.append(issue.reason);
            }
        }
        this->issues_->setPlainText(details.join(QStringLiteral("\n\n")));
        this->issues_->setVisible(!details.isEmpty());
        this->updateImportButton();
    }

    void updateImportButton()
    {
        this->importButton_->setEnabled(
            !this->name_->text().trimmed().isEmpty() &&
            this->parsed_.importedRuleCount() > 0);
    }

    void updateColorButton()
    {
        this->colorButton_->setText(this->color_.name(QColor::HexArgb));
        QPixmap swatch(16, 16);
        swatch.fill(this->color_);
        this->colorButton_->setIcon(QIcon(swatch));
    }

    QLineEdit *name_{};
    QPlainTextEdit *source_{};
    QLabel *summary_{};
    QPlainTextEdit *issues_{};
    QCheckBox *showInMentions_{};
    QCheckBox *alert_{};
    QCheckBox *sound_{};
    QPushButton *colorButton_{};
    MatchAppearanceControls *matchAppearance_{};
    QPushButton *importButton_{};
    QColor color_;
    QTimer analysisTimer_;
    ChattyHighlightImportResult parsed_;
};

class MatchAppearanceDialog final : public QDialog
{
public:
    MatchAppearanceDialog(QWidget *parent, QColor color,
                          HighlightMatchStyle style, QString paintID,
                          bool allowPaint)
        : QDialog(parent)
    {
        this->setWindowTitle(allowPaint ? "Matched text appearance"
                                        : "AutoMod flagged text");
        this->setMinimumWidth(500);
        auto *layout = new QVBoxLayout(this);
        auto *description = new QLabel(
            allowPaint
                ? "Select None to use only a 7TV paint."
                : "Choose how the flagged part of an AutoMod message appears.",
            this);
        description->setWordWrap(true);
        layout->addWidget(description);
        this->controls_ = new MatchAppearanceControls(std::move(color), style,
                                                      std::move(paintID), this,
                                                      true, allowPaint);
        layout->addWidget(this->controls_);
        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::accepted, this, [this] {
            if (!this->controls_->hasValidPaintID())
            {
                QMessageBox::warning(
                    this, "Invalid 7TV paint ID",
                    "Enter a valid 24 or 26 character paint ID, or clear the "
                    "field.");
                return;
            }
            this->accept();
        });
        QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                         &QDialog::reject);
    }

    QColor color() const
    {
        return this->controls_->color();
    }
    HighlightMatchStyle style() const
    {
        return this->controls_->style();
    }
    QString paintID() const
    {
        return this->controls_->paintID();
    }

private:
    MatchAppearanceControls *controls_{};
};

class ChannelScopeDialog final : public QDialog
{
public:
    ChannelScopeDialog(QWidget *parent, QString ruleName,
                       const HighlightChannelScope &existing)
        : QDialog(parent)
    {
        this->setWindowTitle("Where this highlight applies");
        this->setMinimumSize(620, 440);

        auto *layout = new QVBoxLayout(this);
        layout->setSpacing(10);
        auto *rule = new QLabel(QStringLiteral("Rule: %1").arg(ruleName), this);
        rule->setWordWrap(true);
        rule->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(rule);

        auto *modeForm = new QFormLayout();
        this->mode_ = new QComboBox(this);
        for (const auto mode : {HighlightChannelScopeMode::Everywhere,
                                HighlightChannelScopeMode::OnlySelected,
                                HighlightChannelScopeMode::ExcludeSelected})
        {
            this->mode_->addItem(highlightChannelScopeModeName(mode),
                                 static_cast<int>(mode));
        }
        const auto modeIndex =
            this->mode_->findData(static_cast<int>(existing.mode()));
        this->mode_->setCurrentIndex(std::max(0, modeIndex));
        modeForm->addRow("Applies in:", this->mode_);
        alignFormLabels(modeForm, this, {this->mode_});
        layout->addLayout(modeForm);

        this->channelsGroup_ = new QGroupBox("Channels", this);
        auto *channelsLayout = new QVBoxLayout(this->channelsGroup_);

        auto *entryLayout = new QHBoxLayout();
        this->targetPlatform_ = new QComboBox(this->channelsGroup_);
        for (const auto platform : {HighlightChannelTargetPlatform::Any,
                                    HighlightChannelTargetPlatform::Twitch,
                                    HighlightChannelTargetPlatform::YouTube,
                                    HighlightChannelTargetPlatform::Kick,
                                    HighlightChannelTargetPlatform::TikTok})
        {
            this->targetPlatform_->addItem(
                highlightChannelTargetPlatformName(platform),
                static_cast<int>(platform));
        }
        this->targetPlatform_->setMinimumWidth(125);
        this->targetPlatform_->setToolTip(
            "Use Any platform to match the channel on every service.");
        this->channelInput_ = new QLineEdit(this->channelsGroup_);
        this->channelInput_->setPlaceholderText(
            "Channel name, handle, ID, or link");
        this->channelInput_->setToolTip(
            "Separate multiple channels with commas.");
        auto *add = new QPushButton("Add channels", this->channelsGroup_);
        add->setMinimumWidth(std::max(105, add->sizeHint().width()));
        entryLayout->addWidget(this->targetPlatform_);
        entryLayout->addWidget(this->channelInput_, 1);
        entryLayout->addWidget(add);
        channelsLayout->addLayout(entryLayout);

        this->channels_ = new QTreeWidget(this->channelsGroup_);
        this->channels_->setHeaderLabels({"Platform", "Channel"});
        this->channels_->setRootIsDecorated(false);
        this->channels_->setAlternatingRowColors(true);
        this->channels_->setUniformRowHeights(true);
        this->channels_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        this->channels_->setSelectionBehavior(QAbstractItemView::SelectRows);
        this->channels_->header()->setSectionResizeMode(
            0, QHeaderView::ResizeToContents);
        this->channels_->header()->setSectionResizeMode(1,
                                                        QHeaderView::Stretch);
        channelsLayout->addWidget(this->channels_, 1);

        auto *channelButtons = new QHBoxLayout();
        this->remove_ =
            new QPushButton("Remove selected", this->channelsGroup_);
        this->remove_->setEnabled(false);
        this->summary_ = new QLabel(this->channelsGroup_);
        channelButtons->addWidget(this->remove_);
        channelButtons->addStretch(1);
        channelButtons->addWidget(this->summary_);
        channelsLayout->addLayout(channelButtons);
        layout->addWidget(this->channelsGroup_, 1);

        for (const auto &target : existing.targets())
        {
            this->addTarget(target);
        }

        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        layout->addWidget(buttons);

        QObject::connect(add, &QPushButton::clicked, this, [this] {
            this->addInputChannels();
        });
        QObject::connect(this->channelInput_, &QLineEdit::returnPressed, this,
                         [this] {
                             this->addInputChannels();
                         });
        QObject::connect(this->remove_, &QPushButton::clicked, this, [this] {
            auto selected = this->channels_->selectedItems();
            for (auto *item : selected)
            {
                delete item;
            }
            this->updateState();
        });
        QObject::connect(this->channels_, &QTreeWidget::itemSelectionChanged,
                         this, [this] {
                             this->remove_->setEnabled(
                                 !this->channels_->selectedItems().isEmpty());
                         });
        QObject::connect(this->mode_, &QComboBox::currentIndexChanged, this,
                         [this] {
                             this->updateState();
                         });
        QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                         &QDialog::reject);
        QObject::connect(buttons, &QDialogButtonBox::accepted, this, [this] {
            if (this->mode() != HighlightChannelScopeMode::Everywhere &&
                this->channels_->topLevelItemCount() == 0)
            {
                QMessageBox::warning(this, "Choose at least one channel",
                                     "Add a channel, or choose All channels.");
                return;
            }
            this->accept();
        });
        this->updateState();
    }

    HighlightChannelScope scope() const
    {
        std::vector<HighlightChannelTarget> targets;
        targets.reserve(this->channels_->topLevelItemCount());
        for (int index = 0; index < this->channels_->topLevelItemCount();
             ++index)
        {
            const auto encoded = this->channels_->topLevelItem(index)
                                     ->data(0, Qt::UserRole)
                                     .toString();
            if (auto target = decodeHighlightChannelTarget(encoded))
            {
                targets.emplace_back(std::move(*target));
            }
        }
        return {this->mode(), std::move(targets)};
    }

private:
    HighlightChannelScopeMode mode() const
    {
        return static_cast<HighlightChannelScopeMode>(
            this->mode_->currentData().toInt());
    }

    bool addTarget(const HighlightChannelTarget &target)
    {
        const auto encoded = encodeHighlightChannelTarget(target);
        const auto normalizedChannel =
            normalizeHighlightChannelName(target.channel);
        for (int index = 0; index < this->channels_->topLevelItemCount();
             ++index)
        {
            const auto existing = decodeHighlightChannelTarget(
                this->channels_->topLevelItem(index)
                    ->data(0, Qt::UserRole)
                    .toString());
            if (existing && existing->platform == target.platform &&
                normalizeHighlightChannelName(existing->channel) ==
                    normalizedChannel)
            {
                return false;
            }
        }

        auto *item = new QTreeWidgetItem(
            {highlightChannelTargetPlatformName(target.platform),
             target.channel});
        item->setData(0, Qt::UserRole, encoded);
        item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        this->channels_->addTopLevelItem(item);
        return true;
    }

    void addInputChannels()
    {
        const auto platform = static_cast<HighlightChannelTargetPlatform>(
            this->targetPlatform_->currentData().toInt());
        bool added = false;
        for (auto channel : this->channelInput_->text().split(
                 QRegularExpression(QStringLiteral("[,\\r\\n]")),
                 Qt::SkipEmptyParts))
        {
            if (auto target =
                    makeHighlightChannelTarget(platform, std::move(channel)))
            {
                added = this->addTarget(*target) || added;
            }
        }
        if (added)
        {
            this->channelInput_->clear();
        }
        this->channelInput_->setFocus();
        this->updateState();
    }

    void updateState()
    {
        const bool scoped =
            this->mode() != HighlightChannelScopeMode::Everywhere;
        this->channelsGroup_->setEnabled(scoped);
        const auto count = this->channels_->topLevelItemCount();
        this->summary_->setText(QStringLiteral("%1 selected %2")
                                    .arg(count)
                                    .arg(count == 1
                                             ? QStringLiteral("channel")
                                             : QStringLiteral("channels")));
        this->remove_->setEnabled(scoped &&
                                  !this->channels_->selectedItems().isEmpty());
    }

    QComboBox *mode_{};
    QGroupBox *channelsGroup_{};
    QComboBox *targetPlatform_{};
    QLineEdit *channelInput_{};
    QTreeWidget *channels_{};
    QPushButton *remove_{};
    QLabel *summary_{};
};

class WordListEditorDialog final : public QDialog
{
public:
    WordListEditorDialog(QWidget *parent,
                         const std::optional<HighlightWordList> &existing,
                         QString importedText = {}, QString importedName = {},
                         bool importedRegex = false)
        : QDialog(parent)
    {
        this->setWindowTitle(existing ? "Edit moderation list"
                                      : "Add moderation list");
        this->setMinimumSize(720, 680);

        auto *layout = new QVBoxLayout(this);
        layout->setSpacing(10);
        auto *details = new QGroupBox("List details", this);
        auto *form = new QFormLayout(details);
        this->name_ = new QLineEdit(this);
        this->name_->setPlaceholderText("Example: Channel moderation terms");
        form->addRow("Name:", this->name_);

        this->channels_ = new QLineEdit(this);
        this->channels_->setPlaceholderText(
            "All channels (or channel names separated by commas)");
        this->channels_->setToolTip("Leave empty for every channel.");
        form->addRow("Applies in:", this->channels_);

        this->platform_ = new QComboBox(this);
        this->platform_->addItem(
            "All platforms", static_cast<int>(HighlightWordListPlatform::All));
        this->platform_->addItem(
            "Twitch", static_cast<int>(HighlightWordListPlatform::Twitch));
        this->platform_->addItem(
            "YouTube", static_cast<int>(HighlightWordListPlatform::YouTube));
        this->platform_->addItem(
            "Kick", static_cast<int>(HighlightWordListPlatform::Kick));
        this->platform_->addItem(
            "TikTok", static_cast<int>(HighlightWordListPlatform::TikTok));
        this->platform_->setToolTip("Limit this list to one platform.");
        form->addRow("Platform:", this->platform_);

        this->enabled_ = new QCheckBox("Enabled", this);
        this->enabled_->setChecked(true);
        form->addRow("Status:", this->enabled_);
        alignFormLabels(
            form, this,
            {this->name_, this->channels_, this->platform_, this->enabled_});
        layout->addWidget(details);

        auto *rules = new QGroupBox("Rules", this);
        auto *rulesLayout = new QVBoxLayout(rules);
        auto *ruleOptionsWidget = new QWidget(rules);
        auto *ruleOptions = new QHBoxLayout(ruleOptionsWidget);
        ruleOptions->setContentsMargins(0, 0, 0, 0);
        this->regex_ = new QCheckBox("Enable regex", this);
        this->regex_->setToolTip("Treat each line as a regular expression.");
        this->caseSensitive_ = new QCheckBox("Case sensitive", this);
        ruleOptions->addWidget(this->regex_);
        ruleOptions->addWidget(this->caseSensitive_);
        ruleOptions->addStretch(1);
        auto *ruleOptionsForm = new QFormLayout();
        ruleOptionsForm->addRow("Match options:", ruleOptionsWidget);
        alignFormLabels(ruleOptionsForm, this, {ruleOptionsWidget});
        rulesLayout->addLayout(ruleOptionsForm);
        this->intro_ = new QLabel(this);
        this->intro_->setWordWrap(true);
        rulesLayout->addWidget(this->intro_);
        this->terms_ = new QPlainTextEdit(this);
        this->terms_->setPlaceholderText("bad phrase\nanother phrase");
        rulesLayout->addWidget(this->terms_, 1);
        this->summary_ = new QLabel(this);
        this->summary_->setWordWrap(true);
        auto *rulesFooter = new QHBoxLayout();
        rulesFooter->setContentsMargins(0, 0, 0, 0);
        rulesFooter->addWidget(this->summary_, 1);
        auto *importRules = new QPushButton("Import rules from file", rules);
        importRules->setMinimumWidth(
            std::max(150, importRules->sizeHint().width()));
        importRules->setToolTip("Import rules from a TXT, LIST, or CSV file.");
        rulesFooter->addWidget(importRules);
        rulesLayout->addLayout(rulesFooter);
        layout->addWidget(rules, 1);

        auto *alerts = new QGroupBox("Notifications", this);
        auto *alertsLayout = new QFormLayout(alerts);
        auto *alertOptionsWidget = new QWidget(alerts);
        auto *alertOptions = new QGridLayout(alertOptionsWidget);
        alertOptions->setContentsMargins(0, 0, 0, 0);
        alertOptions->setHorizontalSpacing(16);
        this->showInMentions_ = new QCheckBox("Show in Mentions", this);
        this->alert_ = new QCheckBox("Flash taskbar", this);
        this->sound_ = new QCheckBox("Play sound", this);
        alertOptions->addWidget(this->showInMentions_, 0, 0);
        alertOptions->addWidget(this->alert_, 0, 1);
        alertOptions->addWidget(this->sound_, 0, 2);
        alertOptions->setColumnStretch(0, 1);
        alertOptions->setColumnStretch(1, 1);
        alertOptions->setColumnStretch(2, 1);
        alertsLayout->addRow("Actions:", alertOptionsWidget);

        auto *soundWidget = new QWidget(alerts);
        auto *soundLayout = new QHBoxLayout(soundWidget);
        soundLayout->setContentsMargins(0, 0, 0, 0);
        soundLayout->setSpacing(8);
        this->customSoundButton_ = new QPushButton(this);
        this->clearSoundButton_ = new QPushButton("Clear", this);
        soundLayout->addWidget(this->customSoundButton_, 1);
        soundLayout->addWidget(this->clearSoundButton_);
        alertsLayout->addRow("Sound:", soundWidget);
        alignFormLabels(alertsLayout, this, {alertOptionsWidget, soundWidget});
        layout->addWidget(alerts);

        auto *appearance = new QGroupBox("Appearance", this);
        auto *appearanceLayout = new QVBoxLayout(appearance);
        auto *colorRow = new QFormLayout();
        this->colorButton_ = new QPushButton(this);
        colorRow->addRow("Message background:", this->colorButton_);
        alignFormLabels(colorRow, this, {this->colorButton_});
        appearanceLayout->addLayout(colorRow);

        this->color_ =
            *ColorProvider::instance().color(ColorType::SelfHighlight);
        auto matchColor = defaultNewHighlightMatchColor();
        auto matchStyle = HighlightMatchStyle::Outline;
        QString matchPaintID;
        if (existing)
        {
            this->name_->setText(existing->name());
            QStringList channels;
            for (const auto &channel : existing->channels())
            {
                channels.append(channel);
            }
            this->channels_->setText(channels.join(", "));
            const auto platformIndex = this->platform_->findData(
                static_cast<int>(existing->platform()));
            this->platform_->setCurrentIndex(std::max(platformIndex, 0));
            QStringList terms;
            for (const auto &term : existing->terms())
            {
                terms.append(term);
            }
            this->terms_->setPlainText(terms.join(u'\n'));
            this->enabled_->setChecked(existing->enabled());
            this->caseSensitive_->setChecked(existing->caseSensitive());
            this->showInMentions_->setChecked(existing->showInMentions());
            this->alert_->setChecked(existing->hasAlert());
            this->sound_->setChecked(existing->hasSound());
            this->soundUrl_ = existing->soundUrl().toString();
            this->color_ = *existing->color();
            this->regex_->setChecked(existing->isRegex());
            matchColor = *existing->matchColor();
            matchStyle = existing->matchStyle();
            matchPaintID = existing->matchPaintID();
        }
        else
        {
            this->name_->setText(std::move(importedName));
            this->terms_->setPlainText(std::move(importedText));
            this->regex_->setChecked(importedRegex);
            if (importedRegex)
            {
                this->caseSensitive_->setChecked(true);
            }
        }

        this->matchAppearance_ = new MatchAppearanceControls(
            matchColor, matchStyle, matchPaintID, appearance,
            !existing || existing->enabled());
        appearanceLayout->addWidget(this->matchAppearance_);
        layout->addWidget(appearance);

        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        layout->addWidget(buttons);
        this->updateSummary();

        auto *analysisTimer = new QTimer(this);
        analysisTimer->setSingleShot(true);
        analysisTimer->setInterval(250);
        QObject::connect(analysisTimer, &QTimer::timeout, this, [this] {
            this->updateSummary();
        });
        QObject::connect(this->terms_, &QPlainTextEdit::textChanged, this,
                         [analysisTimer] {
                             analysisTimer->start();
                         });
        QObject::connect(this->caseSensitive_, &QCheckBox::toggled, this,
                         [this] {
                             this->updateSummary();
                         });
        QObject::connect(this->regex_, &QCheckBox::toggled, this, [this] {
            this->updateSummary();
        });
        QObject::connect(importRules, &QPushButton::clicked, this, [this] {
            const QPointer<WordListEditorDialog> self(this);
            auto imported =
                openWordListFile(this, "Import rules into moderation list");
            if (!self || !imported)
            {
                return;
            }
            if (imported->chattySyntax)
            {
                QMessageBox::information(
                    this, "Chatty highlight settings detected",
                    "Use Import > Chatty highlight settings from the "
                    "Moderator Word Lists page to keep channel limits.");
                return;
            }
            if (imported->text.trimmed().isEmpty())
            {
                QMessageBox::warning(
                    this, "No rules found",
                    "The selected file does not contain any usable text.");
                return;
            }

            const auto currentText = this->terms_->toPlainText();
            bool append = false;
            if (!currentText.trimmed().isEmpty())
            {
                QPointer<QMessageBox> choice = new QMessageBox(
                    QMessageBox::Question, "Import rules",
                    "This list already contains rules. Replace them or add "
                    "the imported rules to the existing list?",
                    QMessageBox::Cancel, this);
                const auto cleanup = qScopeGuard([choice] {
                    delete choice;
                });
                auto *replaceButton =
                    choice->addButton("Replace rules", QMessageBox::AcceptRole);
                auto *appendButton = choice->addButton("Add to existing rules",
                                                       QMessageBox::ActionRole);
                choice->exec();
                if (!self || !choice)
                {
                    return;
                }
                if (choice->clickedButton() ==
                    choice->button(QMessageBox::Cancel))
                {
                    return;
                }
                append = choice->clickedButton() == appendButton;
                if (choice->clickedButton() != replaceButton && !append)
                {
                    return;
                }
            }

            if (append && imported->suggestedRegex != this->isRegex())
            {
                const auto message = imported->suggestedRegex
                                         ? "This file looks like a regular "
                                           "expression list, but this list "
                                           "uses words and phrases. The new "
                                           "rules would be treated as plain "
                                           "text. Add them anyway?"
                                         : "This file looks like a word and "
                                           "phrase list, but this list uses "
                                           "regular expressions. The new "
                                           "rules would be interpreted as "
                                           "expressions. Add them anyway?";
                if (QMessageBox::warning(this, "Different rule format", message,
                                         QMessageBox::Yes | QMessageBox::Cancel,
                                         QMessageBox::Cancel) !=
                    QMessageBox::Yes)
                {
                    return;
                }
            }

            if (!self)
            {
                return;
            }
            if (append)
            {
                auto combined = currentText;
                if (!combined.endsWith(u'\n'))
                {
                    combined.append(u'\n');
                }
                combined.append(imported->text);
                this->terms_->setPlainText(std::move(combined));
            }
            else
            {
                this->terms_->setPlainText(imported->text);
            }
            if (this->name_->text().trimmed().isEmpty())
            {
                this->name_->setText(imported->suggestedName);
            }
            if (!append && imported->suggestedRegex)
            {
                this->regex_->setChecked(true);
                this->caseSensitive_->setChecked(true);
            }
        });
        QObject::connect(this->customSoundButton_, &QPushButton::clicked, this,
                         [this] {
                             const QPointer<WordListEditorDialog> self(this);
                             const auto sound = QFileDialog::getOpenFileUrl(
                                 this, "Choose moderation list sound", QUrl(),
                                 "Audio files (*.mp3 *.wav)");
                             if (self && !sound.isEmpty())
                             {
                                 this->soundUrl_ = sound.toString();
                                 this->updateSoundButtons();
                             }
                         });
        QObject::connect(this->clearSoundButton_, &QPushButton::clicked, this,
                         [this] {
                             this->soundUrl_.clear();
                             this->updateSoundButtons();
                         });
        QObject::connect(
            this->colorButton_, &QPushButton::clicked, this, [this] {
                auto *picker = new ColorPickerDialog(this->color_, this);
                QObject::connect(picker, &ColorPickerDialog::colorConfirmed,
                                 this, [this](const QColor &color) {
                                     if (color.isValid())
                                     {
                                         this->color_ = color;
                                         this->updateColorButton();
                                     }
                                 });
                picker->show();
            });
        QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                         &QDialog::reject);
        QObject::connect(buttons, &QDialogButtonBox::accepted, this, [this] {
            const QPointer<WordListEditorDialog> self(this);
            const auto parsed = parseWordListTerms(
                this->terms_->toPlainText(), this->caseSensitive_->isChecked(),
                this->isRegex());
            if (this->name_->text().trimmed().isEmpty())
            {
                QMessageBox::warning(this, "Missing name",
                                     "Give this word list a name.");
                return;
            }
            if (parsed.invalidRegex > 0)
            {
                QMessageBox::warning(
                    this, "Invalid regular expression",
                    QString("Fix the invalid expression before saving. %1")
                        .arg(parsed.firstRegexError));
                return;
            }
            if (parsed.overLimit > 0 || parsed.overCharacterBudget > 0 ||
                parsed.oversized > 0)
            {
                if (this->isRegex())
                {
                    QMessageBox::warning(
                        this, "Regular expression list is too large",
                        "Use up to 512 characters per expression, 2,000 rules, "
                        "and 500,000 characters total.");
                }
                else
                {
                    QMessageBox::warning(
                        this, "Word and phrase list is too large",
                        "Use up to 256 characters per entry, 10,000 rules, "
                        "and 500,000 characters total.");
                }
                return;
            }
            if (parsed.terms.empty())
            {
                QMessageBox::warning(
                    this, "No usable rules",
                    this->isRegex()
                        ? "Add at least one valid regular expression of 512 "
                          "characters or fewer."
                        : "Add at least one word or phrase of 256 characters "
                          "or fewer.");
                return;
            }
            if (!this->matchAppearance_->hasValidPaintID())
            {
                QMessageBox::warning(
                    this, "Invalid 7TV paint ID",
                    "Enter a valid 24 or 26 character paint ID, or clear the "
                    "field.");
                return;
            }
            if (this->isRegex() &&
                parsed.terms.size() > HIGHLIGHT_WORD_LIST_REGEX_WARNING_RULES &&
                QMessageBox::warning(
                    this, "Large regular expression list",
                    QString("This list has %1 regular expressions. Complex "
                            "rules may slow busy chats. Save anyway?")
                        .arg(parsed.terms.size()),
                    QMessageBox::Save | QMessageBox::Cancel,
                    QMessageBox::Cancel) != QMessageBox::Save)
            {
                return;
            }
            if (self)
            {
                this->accept();
            }
        });
    }

    HighlightWordList value() const
    {
        auto parsed = parseWordListTerms(this->terms_->toPlainText(),
                                         this->caseSensitive_->isChecked(),
                                         this->isRegex());
        return {
            this->name_->text().trimmed(),
            this->enabled_->isChecked(),
            std::move(parsed.terms),
            parseChannels(this->channels_->text()),
            this->caseSensitive_->isChecked(),
            this->showInMentions_->isChecked(),
            this->alert_->isChecked(),
            this->sound_->isChecked(),
            this->soundUrl_,
            this->color_,
            this->isRegex(),
            this->matchAppearance_->color(),
            this->matchAppearance_->style(),
            this->matchAppearance_->paintID(),
            static_cast<HighlightWordListPlatform>(
                this->platform_->currentData().toInt()),
        };
    }

private:
    bool isRegex() const
    {
        return this->regex_->isChecked();
    }

    void updateColorButton()
    {
        this->colorButton_->setText(this->color_.name(QColor::HexArgb));
        QPixmap swatch(16, 16);
        swatch.fill(this->color_);
        this->colorButton_->setIcon(QIcon(swatch));
    }

    void updateSoundButtons()
    {
        const auto sound = QUrl(this->soundUrl_);
        this->customSoundButton_->setText(
            sound.isEmpty()
                ? "Custom sound: use highlight default"
                : "Custom sound: " + shortenString(sound.fileName(), 50));
        this->customSoundButton_->setToolTip(
            sound.isEmpty() ? QString() : sound.fileName());
        this->clearSoundButton_->setVisible(!sound.isEmpty());
    }

    void updateSummary()
    {
        const auto parsed = parseWordListTerms(
            this->terms_->toPlainText(), this->caseSensitive_->isChecked(),
            this->isRegex());
        if (this->isRegex())
        {
            this->intro_->setText("One regular expression per line.");
            this->terms_->setPlaceholderText(
                "\\b(?:badword|bad phrase)\\b\nspam\\d{2,}");
        }
        else
        {
            this->intro_->setText(
                "One word or phrase per line. A word will not match inside a "
                "longer word.");
            this->terms_->setPlaceholderText("bad phrase\nanother phrase");
        }
        QStringList ignored;
        if (parsed.blanks > 0)
        {
            ignored.append(QString::number(parsed.blanks) + " blank");
        }
        if (parsed.duplicates > 0)
        {
            ignored.append(QString::number(parsed.duplicates) + " duplicate");
        }
        if (parsed.oversized > 0)
        {
            ignored.append(QString::number(parsed.oversized) +
                           (this->isRegex() ? " over 512 characters"
                                            : " over 256 characters"));
        }
        if (parsed.overLimit > 0)
        {
            ignored.append(QString::number(parsed.overLimit) +
                           (this->isRegex() ? " past the 2,000 rule limit"
                                            : " past the 10,000 rule limit"));
        }
        if (parsed.overCharacterBudget > 0)
        {
            ignored.append(QString::number(parsed.overCharacterBudget) +
                           " past the 500,000 character list limit");
        }
        QString summary = QString("%1 usable %2")
                              .arg(parsed.terms.size())
                              .arg(parsed.terms.size() == 1 ? "rule" : "rules");
        if (!ignored.isEmpty())
        {
            summary += ". Ignored: " + ignored.join(", ") + ".";
        }
        if (parsed.invalidRegex > 0)
        {
            summary += QString(" Fix %1 invalid %2. %3")
                           .arg(parsed.invalidRegex)
                           .arg(parsed.invalidRegex == 1 ? "expression"
                                                         : "expressions")
                           .arg(parsed.firstRegexError);
        }
        if (this->isRegex() &&
            parsed.terms.size() > HIGHLIGHT_WORD_LIST_REGEX_WARNING_RULES &&
            parsed.overLimit == 0)
        {
            summary += " Large list: complex expressions may slow busy chats.";
        }
        this->summary_->setText(summary);
        this->updateSoundButtons();
        this->updateColorButton();
    }

    QLineEdit *name_{};
    QLineEdit *channels_{};
    QComboBox *platform_{};
    QCheckBox *regex_{};
    QLabel *intro_{};
    QPlainTextEdit *terms_{};
    QCheckBox *enabled_{};
    QCheckBox *caseSensitive_{};
    QCheckBox *showInMentions_{};
    QCheckBox *alert_{};
    QCheckBox *sound_{};
    QPushButton *customSoundButton_{};
    QPushButton *clearSoundButton_{};
    QPushButton *colorButton_{};
    QLabel *summary_{};
    MatchAppearanceControls *matchAppearance_{};
    QColor color_;
    QString soundUrl_;
};

class HighlightTesterDialog final : public QDialog
{
public:
    explicit HighlightTesterDialog(QWidget *parent)
        : QDialog(parent)
    {
        this->setWindowTitle("Test highlight rules");
        this->setMinimumSize(650, 520);
        auto *layout = new QVBoxLayout(this);

        auto *form = new QFormLayout();
        this->sender_ = new QLineEdit("moltobenne_", this);
        this->channel_ = new QLineEdit(this);
        this->channel_->setPlaceholderText("Optional channel name");
        this->platform_ = new QComboBox(this);
        this->platform_->addItem("Twitch",
                                static_cast<int>(MessagePlatform::AnyOrTwitch));
        this->platform_->addItem("YouTube",
                                static_cast<int>(MessagePlatform::YouTube));
        this->platform_->addItem("Kick", static_cast<int>(MessagePlatform::Kick));
        this->platform_->addItem("TikTok",
                                static_cast<int>(MessagePlatform::TikTok));
        form->addRow("Sender:", this->sender_);
        form->addRow("Channel:", this->channel_);
        form->addRow("Platform:", this->platform_);
        layout->addLayout(form);

        this->message_ = new QPlainTextEdit(this);
        this->message_->setPlaceholderText("Type or paste a test chat message");
        layout->addWidget(this->message_, 1);
        layout->addWidget(new QLabel("Preview:", this));
        this->preview_ = new QTextEdit(this);
        this->preview_->setReadOnly(true);
        this->preview_->setMaximumHeight(90);
        layout->addWidget(this->preview_);
        layout->addWidget(new QLabel("Match details:", this));
        this->explanation_ = new QPlainTextEdit(this);
        this->explanation_->setReadOnly(true);
        layout->addWidget(this->explanation_, 1);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                         &QDialog::reject);
        QObject::connect(buttons, &QDialogButtonBox::accepted, this,
                         &QDialog::accept);

        const auto update = [this] {
            this->updateResult();
        };
        QObject::connect(this->message_, &QPlainTextEdit::textChanged, this,
                         update);
        QObject::connect(this->sender_, &QLineEdit::textChanged, this, update);
        QObject::connect(this->channel_, &QLineEdit::textChanged, this, update);
        QObject::connect(this->platform_, &QComboBox::currentIndexChanged, this,
                         update);
        this->updateResult();
        this->resize(this->minimumSize());
    }

private:
    void updateResult()
    {
        const auto text = this->message_->toPlainText();
        this->preview_->setPlainText(text);
        if (text.isEmpty())
        {
            this->explanation_->setPlainText(
                "Enter a message to see which rules match and why.");
            return;
        }
        if (getSettings()->isBlacklistedUser(this->sender_->text()))
        {
            this->explanation_->setPlainText(
                "No highlight: this sender is in Blacklisted Users.");
            return;
        }

        const auto platform = static_cast<MessagePlatform>(
            this->platform_->currentData().toInt());
        QStringList scopeRejections;
        const auto describeRejection = [this](const QString &kind,
                                              const HighlightPhrase &rule) {
            const auto channel = this->channel_->text().trimmed();
            const auto context =
                channel.isEmpty()
                    ? QStringLiteral("no channel was entered")
                    : QStringLiteral("%1 channel \"%2\"")
                          .arg(this->platform_->currentText(), channel);
            return QStringLiteral(
                       "%1 rule \"%2\" matched, but where it applies "
                       "does not include this test because %3. Applies "
                       "in: %4.")
                .arg(kind, rule.getPattern(), context,
                     highlightChannelScopeSummary(
                         rule.getChannelScope().mode(),
                         rule.getChannelScope().targets().size()));
        };
        const auto phrases = getSettings()->highlightedMessages.readOnly();
        for (const auto &rule : *phrases)
        {
            if (rule.isMatch(text) && !rule.getChannelScope().appliesTo(
                                          platform, this->channel_->text()))
            {
                scopeRejections.append(
                    describeRejection("Message highlight", rule));
            }
        }
        const auto users = getSettings()->highlightedUsers.readOnly();
        for (const auto &rule : *users)
        {
            if (rule.isMatch(this->sender_->text()) &&
                !rule.getChannelScope().appliesTo(platform,
                                                  this->channel_->text()))
            {
                scopeRejections.append(
                    describeRejection("User highlight", rule));
            }
        }
        const auto wordLists = getSettings()->highlightWordLists.readOnly();
        for (const auto &list : *wordLists)
        {
            if (!list.enabled() || !list.matchSubject(text).matched ||
                (list.appliesToPlatform(platform) &&
                 list.appliesToChannel(this->channel_->text())))
            {
                continue;
            }

            const auto channel = this->channel_->text().trimmed();
            const auto channelDescription =
                list.channels().empty()
                    ? QStringLiteral("every channel")
                    : QStringLiteral("%1 selected %2")
                          .arg(list.channels().size())
                          .arg(list.channels().size() == 1
                                   ? QStringLiteral("channel")
                                   : QStringLiteral("channels"));
            scopeRejections.append(
                QStringLiteral(
                    "Moderation list \"%1\" matched, but it applies to %2 "
                    "on %3. This test uses %4%5.")
                    .arg(list.name(), channelDescription,
                         highlightWordListPlatformName(list.platform()),
                         this->platform_->currentText(),
                         channel.isEmpty()
                             ? QStringLiteral(" with no channel entered")
                             : QStringLiteral(" channel \"%1\"").arg(channel)));
        }
        const auto [highlighted, result] = getApp()->getHighlights()->check(
            MessageParseArgs{}, {}, this->sender_->text(), text, MessageFlags{},
            platform, {}, this->channel_->text(), true);

        QStringList explanation;
        if (!highlighted)
        {
            explanation.append(
                scopeRejections.isEmpty()
                    ? QStringLiteral("No rules matched.")
                    : QStringLiteral("No highlight was applied."));
        }
        else if (result.matches.empty())
        {
            explanation.append(
                "The message is highlighted by a whole message rule or a "
                "zero width regular expression, so there is no text "
                "fragment to mark.");
        }

        auto *document = this->preview_->document();
        for (const auto &match : result.matches)
        {
            QTextCursor cursor(document);
            cursor.setPosition(static_cast<int>(match.start));
            cursor.setPosition(static_cast<int>(match.start + match.length),
                               QTextCursor::KeepAnchor);
            if (match.style != HighlightMatchStyle::None)
            {
                QTextCharFormat format;
                format.setBackground(match.color);
                cursor.mergeCharFormat(format);
            }

            const auto matchedText =
                text.mid(match.start, match.length).replace(u'\n', u' ');
            if (match.source == HighlightMatchSource::WordList)
            {
                explanation.append(
                    QString("Moderation list \"%1\": rule \"%2\" matched "
                            "\"%3\" at characters %4 through %5.")
                        .arg(match.ruleName, match.pattern, matchedText)
                        .arg(match.start + 1)
                        .arg(match.start + match.length));
            }
            else if (match.source == HighlightMatchSource::AutoMod)
            {
                auto reason = match.ruleName;
                reason.replace(u'\n', u' ');
                explanation.append(
                    QString("Twitch AutoMod rule \"%1\" matched \"%2\" at "
                            "characters %3 through %4.")
                        .arg(reason, matchedText)
                        .arg(match.start + 1)
                        .arg(match.start + match.length));
            }
            else
            {
                QString mode = "phrase";
                if (const auto found = std::ranges::find_if(
                        *phrases,
                        [&](const auto &phrase) {
                            return phrase.getPattern() == match.pattern;
                        });
                    found != phrases->end())
                {
                    mode = found->isRegex() ? "regular expression"
                                            : "literal phrase";
                    mode += found->isCaseSensitive() ? ", case sensitive"
                                                     : ", case insensitive";
                }
                explanation.append(
                    QString("Highlight %1 \"%2\" matched \"%3\" at "
                            "characters %4 through %5.")
                        .arg(mode, match.pattern, matchedText)
                        .arg(match.start + 1)
                        .arg(match.start + match.length));
            }
        }
        explanation.append(scopeRejections);
        this->explanation_->setPlainText(explanation.join(u'\n'));
    }

    QLineEdit *sender_{};
    QLineEdit *channel_{};
    QComboBox *platform_{};
    QPlainTextEdit *message_{};
    QTextEdit *preview_{};
    QPlainTextEdit *explanation_{};
};

}

HighlightingPage::HighlightingPage()
{
    this->setMinimumWidth(HIGHLIGHT_PAGE_MIN_WIDTH);

    LayoutCreator<HighlightingPage> layoutCreator(this);

    auto layout = layoutCreator.emplace<QVBoxLayout>().withoutMargin();
    {

        auto tabs = layout.emplace<QTabWidget>();
        {

            auto highlights = tabs.appendTab(new QVBoxLayout, "Messages");
            {
                highlights.emplace<QLabel>(
                    "Play notification sounds and highlight messages based on "
                    "certain patterns.\n"
                    "Message highlights are prioritized over badge highlights "
                    "and user highlights.");

                auto *view =
                    highlights
                        .emplace<EditableModelView>(
                            (new HighlightModel(nullptr))
                                ->initialized(
                                    &getSettings()->highlightedMessages))
                        .getElement();
                this->addChannelScopeButton(view);
                view->addRegexHelpLink();
                view->setTitles({"Pattern", "Show in\nMentions",
                                 "Flash\ntaskbar", "Enable\nregex",
                                 "Case-\nsensitive", "Play\nsound",
                                 "Custom\nsound", "Message\ncolor",
                                 "Matched\ntext", "Applies\nin"});
                view->getTableView()->horizontalHeader()->setSectionResizeMode(
                    QHeaderView::Fixed);
                view->getTableView()->horizontalHeader()->setSectionResizeMode(
                    0, QHeaderView::Stretch);
                view->getTableView()->horizontalHeader()->setSectionResizeMode(
                    HighlightModel::Column::MatchAppearance,
                    QHeaderView::Fixed);
                view->getTableView()->horizontalHeader()->setSectionResizeMode(
                    HighlightModel::Column::ChannelScope, QHeaderView::Fixed);
                view->getTableView()->setItemDelegateForColumn(
                    HighlightModel::Column::Color, new ColorItemDelegate(view));
                view->getTableView()->setItemDelegateForColumn(
                    HighlightModel::Column::MatchAppearance,
                    new MatchAppearanceItemDelegate(view));

                QTimer::singleShot(1, view, [view] {
                    view->getTableView()->resizeColumnsToContents();
                    view->getTableView()->setColumnWidth(
                        0, HIGHLIGHT_PATTERN_MIN_WIDTH);
                    view->getTableView()->setColumnWidth(
                        HighlightModel::Column::SoundPath, 110);
                    view->getTableView()->setColumnWidth(
                        HighlightModel::Column::MatchAppearance,
                        HIGHLIGHT_MATCH_APPEARANCE_WIDTH);
                    view->getTableView()->setColumnWidth(
                        HighlightModel::Column::ChannelScope,
                        HIGHLIGHT_APPLIES_IN_WIDTH);
                });

                std::ignore = view->addButtonPressed.connect([] {
                    getSettings()->highlightedMessages.append(HighlightPhrase{
                        "my phrase", true, true, false, false, false, "",
                        *ColorProvider::instance().color(
                            ColorType::SelfHighlight)});
                });

                QObject::connect(view->getTableView(), &QTableView::clicked,
                                 [this, view](const QModelIndex &clicked) {
                                     this->tableCellClicked(
                                         clicked, view, HighlightTab::Messages);
                                 });

                highlights.append(this->createCheckBox(
                    "Mark the exact part of a message that triggered a "
                    "highlight",
                    getSettings()->highlightMatchedFragments));
                auto tools = highlights.emplace<QHBoxLayout>().withoutMargin();
                auto tester = tools.emplace<QPushButton>("Test highlights...");
                tools->addStretch(1);
                QObject::connect(
                    tester.getElement(), &QPushButton::clicked, this, [this] {
                        auto *dialog = new HighlightTesterDialog(this);
                        dialog->setAttribute(Qt::WA_DeleteOnClose);
                        dialog->open();
                    });
            }

            auto wordLists =
                tabs.appendTab(new QVBoxLayout, "Moderator Word Lists");
            {
                auto description = wordLists.emplace<QLabel>(
                    "Highlight messages with reusable word, phrase, or regex "
                    "lists. Limit a list to specific platforms or channels "
                    "when needed.");
                description->setWordWrap(true);

                this->wordLists_ =
                    wordLists.emplace<QTreeWidget>().getElement();
                this->wordLists_->setHeaderLabels({"Name", "List type", "Rules",
                                                   "Platform", "Applies in",
                                                   "Notifications"});
                this->wordLists_->setRootIsDecorated(false);
                this->wordLists_->setAlternatingRowColors(true);
                this->wordLists_->setUniformRowHeights(true);
                this->wordLists_->setSelectionMode(
                    QAbstractItemView::SingleSelection);
                this->wordLists_->setSelectionBehavior(
                    QAbstractItemView::SelectRows);
                this->wordLists_->header()->setSectionResizeMode(
                    QHeaderView::ResizeToContents);
                this->wordLists_->header()->setSectionResizeMode(
                    0, QHeaderView::Stretch);
                QObject::connect(this->wordLists_,
                                 &QTreeWidget::itemDoubleClicked, this, [this] {
                                     this->editSelectedWordList();
                                 });
                QObject::connect(
                    this->wordLists_, &QTreeWidget::itemChanged, this,
                    [this](QTreeWidgetItem *item, int column) {
                        if (this->refreshingWordLists_ || column != 0)
                        {
                            return;
                        }
                        const auto index =
                            this->wordLists_->indexOfTopLevelItem(item);
                        const auto lists =
                            getSettings()->highlightWordLists.readOnly();
                        if (index < 0 ||
                            index >= static_cast<int>(lists->size()))
                        {
                            return;
                        }
                        auto updated = lists->at(index);
                        updated.setEnabled(item->checkState(0) == Qt::Checked);
                        getSettings()->highlightWordLists.removeAt(index);
                        getSettings()->highlightWordLists.insert(updated,
                                                                 index);
                        this->refreshWordLists();
                    });

                auto buttons = wordLists.emplace<QHBoxLayout>().withoutMargin();
                auto add = buttons.emplace<QPushButton>("New list");
                auto import = buttons.emplace<QPushButton>("Import");
                auto edit = buttons.emplace<QPushButton>("Edit list");
                auto exportButton = buttons.emplace<QPushButton>("Export list");
                auto remove = buttons.emplace<QPushButton>("Remove list");
                for (auto *button :
                     {add.getElement(), import.getElement(), edit.getElement(),
                      exportButton.getElement(), remove.getElement()})
                {
                    button->setMinimumWidth(
                        std::max(96, button->sizeHint().width()));
                }
                buttons->addStretch(1);
                const auto updateSelectionButtons =
                    [edit = edit.getElement(),
                     exportButton = exportButton.getElement(),
                     remove = remove.getElement()](QTreeWidgetItem *current,
                                                   QTreeWidgetItem *) {
                        const auto hasSelection = current != nullptr;
                        edit->setEnabled(hasSelection);
                        exportButton->setEnabled(hasSelection);
                        remove->setEnabled(hasSelection);
                    };
                QObject::connect(add.getElement(), &QPushButton::clicked, this,
                                 [this] {
                                     this->createWordList();
                                 });
                auto *importMenu = new QMenu(import.getElement());
                auto *listFile = importMenu->addAction("Moderation list file");
                auto *chatty =
                    importMenu->addAction("Chatty highlight settings");
                import.getElement()->setMenu(importMenu);
                QObject::connect(listFile, &QAction::triggered, this, [this] {
                    this->importWordList();
                });
                QObject::connect(chatty, &QAction::triggered, this, [this] {
                    this->importChattyHighlights();
                });
                QObject::connect(edit.getElement(), &QPushButton::clicked, this,
                                 [this] {
                                     this->editSelectedWordList();
                                 });
                QObject::connect(exportButton.getElement(),
                                 &QPushButton::clicked, this, [this] {
                                     this->exportSelectedWordList();
                                 });
                QObject::connect(remove.getElement(), &QPushButton::clicked,
                                 this, [this] {
                                     this->removeSelectedWordList();
                                 });
                QObject::connect(this->wordLists_,
                                 &QTreeWidget::currentItemChanged, this,
                                 updateSelectionButtons);
                updateSelectionButtons(nullptr, nullptr);
            }

            auto pingUsers = tabs.appendTab(new QVBoxLayout, "Users");
            {
                pingUsers.emplace<QLabel>(
                    "Play notification sounds and highlight messages from "
                    "certain users.\n"
                    "User highlights are prioritized over badge highlights, "
                    "but under message highlights.");
                EditableModelView *view =
                    pingUsers
                        .emplace<EditableModelView>(
                            (new UserHighlightModel(nullptr))
                                ->initialized(&getSettings()->highlightedUsers))
                        .getElement();

                this->addChannelScopeButton(view);
                view->addRegexHelpLink();
                view->getTableView()->horizontalHeader()->hideSection(
                    HighlightModel::Column::UseRegex);
                view->getTableView()->horizontalHeader()->hideSection(
                    HighlightModel::Column::CaseSensitive);
                view->getTableView()->horizontalHeader()->hideSection(
                    HighlightModel::Column::MatchAppearance);

                view->setTitles({"Username", "Show in\nMentions",
                                 "Flash\ntaskbar", "Enable\nregex",
                                 "Case-\nsensitive", "Play\nsound",
                                 "Custom\nsound", "Color", "Matched\ntext",
                                 "Applies\nin"});
                view->getTableView()->horizontalHeader()->setSectionResizeMode(
                    QHeaderView::Fixed);
                view->getTableView()->horizontalHeader()->setSectionResizeMode(
                    0, QHeaderView::Stretch);
                view->getTableView()->setItemDelegateForColumn(
                    UserHighlightModel::Column::Color,
                    new ColorItemDelegate(view));

                QTimer::singleShot(1, view, [view] {
                    view->getTableView()->resizeColumnsToContents();
                    view->getTableView()->setColumnWidth(0, 200);
                    view->getTableView()->setColumnWidth(
                        HighlightModel::Column::ChannelScope,
                        HIGHLIGHT_APPLIES_IN_WIDTH);
                });

                std::ignore = view->addButtonPressed.connect([] {
                    getSettings()->highlightedUsers.append(HighlightPhrase{
                        "highlighted user", true, true, false, false, false, "",
                        *ColorProvider::instance().color(
                            ColorType::SelfHighlight)});
                });

                QObject::connect(view->getTableView(), &QTableView::clicked,
                                 [this, view](const QModelIndex &clicked) {
                                     this->tableCellClicked(
                                         clicked, view, HighlightTab::Users);
                                 });
            }

            auto badgeHighlights = tabs.appendTab(new QVBoxLayout, "Badges");
            {
                badgeHighlights.emplace<QLabel>(
                    "Play notification sounds and highlight messages based on "
                    "user badges.\n"
                    "Badge highlights are prioritized under user and message "
                    "highlights.");
                auto *view = badgeHighlights
                                 .emplace<EditableModelView>(
                                     (new BadgeHighlightModel(nullptr))
                                         ->initialized(
                                             &getSettings()->highlightedBadges))
                                 .getElement();
                view->setTitles({"Name", "Show In\nMentions", "Flash\ntaskbar",
                                 "Play\nsound", "Custom\nsound", "Color"});
                view->getTableView()->horizontalHeader()->setSectionResizeMode(
                    QHeaderView::Fixed);
                view->getTableView()->horizontalHeader()->setSectionResizeMode(
                    0, QHeaderView::Stretch);
                view->getTableView()->setItemDelegateForColumn(
                    BadgeHighlightModel::Column::Color,
                    new ColorItemDelegate(view));

                QTimer::singleShot(1, view, [view] {
                    view->getTableView()->resizeColumnsToContents();
                    view->getTableView()->setColumnWidth(0, 200);
                });

                std::ignore = view->addButtonPressed.connect([this] {
                    QPointer<BadgePickerDialog> d =
                        new BadgePickerDialog(availableBadges, this);
                    const auto cleanup = qScopeGuard([d] {
                        delete d;
                    });

                    d->setWindowTitle("Choose badge");
                    const auto result = d->exec();
                    if (d && result == QDialog::Accepted)
                    {
                        auto s = d->getSelection();
                        if (!s)
                        {
                            return;
                        }
                        getSettings()->highlightedBadges.append(
                            HighlightBadge{s->badgeName(), s->displayName(),
                                           false, false, false, "",
                                           *ColorProvider::instance().color(
                                               ColorType::SelfHighlight)});
                    }
                });

                QObject::connect(view->getTableView(), &QTableView::clicked,
                                 [this, view](const QModelIndex &clicked) {
                                     this->tableCellClicked(
                                         clicked, view, HighlightTab::Badges);
                                 });
            }

            auto disabledUsers =
                tabs.appendTab(new QVBoxLayout, "Blacklisted Users");
            {
                disabledUsers.emplace<QLabel>(
                    "Disable notification sounds and highlights from certain "
                    "users (e.g. bots).");
                EditableModelView *view =
                    disabledUsers
                        .emplace<EditableModelView>(
                            (new HighlightBlacklistModel(nullptr))
                                ->initialized(&getSettings()->blacklistedUsers))
                        .getElement();

                view->addRegexHelpLink();
                view->setTitles({"Username", "Enable\nregex"});
                view->getTableView()->horizontalHeader()->setSectionResizeMode(
                    QHeaderView::Fixed);
                view->getTableView()->horizontalHeader()->setSectionResizeMode(
                    0, QHeaderView::Stretch);

                QTimer::singleShot(1, view, [view] {
                    view->getTableView()->resizeColumnsToContents();
                    view->getTableView()->setColumnWidth(0, 200);
                });

                std::ignore = view->addButtonPressed.connect([] {
                    getSettings()->blacklistedUsers.append(
                        HighlightBlacklistUser{"blacklisted user", false});
                });
            }
        }

        auto customSound = layout.emplace<QHBoxLayout>().withoutMargin();
        {
            auto label = customSound.append(this->createLabel<QString>(
                [](const auto &value) {
                    if (value.isEmpty())
                    {
                        return QString("Default sound: Chatterino Ping");
                    }

                    auto url = QUrl::fromLocalFile(value);
                    return QString("Default sound: <a href=\"%1\"><span "
                                   "style=\"color: white\">%2</span></a>")
                        .arg(url.toString(QUrl::FullyEncoded),
                             shortenString(url.fileName(), 50));
                },
                getSettings()->pathHighlightSound));
            label->setToolTip(
                "This sound will play for all highlight phrases that have "
                "sound enabled and don't have a custom sound set.");
            label->setTextFormat(Qt::RichText);
            label->setTextInteractionFlags(Qt::TextBrowserInteraction |
                                           Qt::LinksAccessibleByKeyboard);
            label->setOpenExternalLinks(true);
            customSound->setStretchFactor(label.getElement(), 1);

            auto clearSound = customSound.emplace<QPushButton>("Clear");
            auto selectFile = customSound.emplace<QPushButton>("Change...");

            QObject::connect(selectFile.getElement(), &QPushButton::clicked,
                             this, [this]() mutable {
                                 auto fileName = QFileDialog::getOpenFileName(
                                     this, tr("Open Sound"), "",
                                     tr("Audio Files (*.mp3 *.wav)"));

                                 getSettings()->pathHighlightSound = fileName;
                             });
            QObject::connect(clearSound.getElement(), &QPushButton::clicked,
                             this, [=]() mutable {
                                 getSettings()->pathHighlightSound = QString();
                             });

            getSettings()->pathHighlightSound.connect(
                [clearSound = clearSound.getElement()](const auto &value) {
                    if (value.isEmpty())
                    {
                        clearSound->hide();
                    }
                    else
                    {
                        clearSound->show();
                    }
                },
                this->managedConnections_);
        }

        layout.append(createCheckBox(
            "Play highlight sound even when Chatterino is focused",
            getSettings()->highlightAlwaysPlaySound));
        layout.append(createCheckBox(
            "Flash taskbar only stops highlighting when Chatterino is focused",
            getSettings()->longAlerts));
    }

    this->disabledUsersChangedTimer_.setSingleShot(true);
    this->refreshWordLists();
}

void HighlightingPage::openSoundDialog(const QModelIndex &clicked,
                                       EditableModelView *view, int soundColumn)
{
    const QPersistentModelIndex target(clicked);
    const QPointer<EditableModelView> guardedView(view);
    auto fileUrl = QFileDialog::getOpenFileUrl(this, tr("Open Sound"), QUrl(),
                                               tr("Audio Files (*.mp3 *.wav)"));
    if (!guardedView || fileUrl.isEmpty() || !target.isValid())
    {
        return;
    }
    view->getModel()->setData(target, fileUrl, Qt::UserRole);
    view->getModel()->setData(target, fileUrl.fileName(), Qt::DisplayRole);
    view->getModel()->setData(target, fileUrl.fileName(), Qt::ToolTipRole);
}

void HighlightingPage::openColorDialog(const QModelIndex &clicked,
                                       EditableModelView *view,
                                       HighlightTab tab)
{
    auto initial =
        view->getModel()->data(clicked, Qt::DecorationRole).value<QColor>();

    auto *dialog = new ColorPickerDialog(initial, this);

    const QPersistentModelIndex target(clicked);
    QObject::connect(dialog, &ColorPickerDialog::colorConfirmed, view,
                     [view, target](auto selected) {
                         if (selected.isValid() && target.isValid())
                         {
                             view->getModel()->setData(target, selected,
                                                       Qt::DecorationRole);
                         }
                     });
    dialog->show();
}

void HighlightingPage::tableCellClicked(const QModelIndex &clicked,
                                        EditableModelView *view,
                                        HighlightTab tab)
{
    if (!clicked.flags().testFlag(Qt::ItemIsEnabled))
    {
        return;
    }

    switch (tab)
    {
        case HighlightTab::Messages:
        case HighlightTab::Users: {
            using Column = HighlightModel::Column;

            if (clicked.column() == Column::SoundPath)
            {
                this->openSoundDialog(clicked, view, Column::SoundPath);
            }
            else if (clicked.column() == Column::Color)
            {
                this->openColorDialog(clicked, view, tab);
            }
            else if (tab == HighlightTab::Messages &&
                     clicked.column() == Column::MatchAppearance)
            {
                this->openMatchAppearanceDialog(clicked, view);
            }
            else if (clicked.column() == Column::ChannelScope)
            {
                this->openChannelScopeDialog(clicked, view);
            }
        }
        break;

        case HighlightTab::Badges: {
            using Column = BadgeHighlightModel::Column;
            if (clicked.column() == Column::SoundPath)
            {
                this->openSoundDialog(clicked, view, Column::SoundPath);
            }
            else if (clicked.column() == Column::Color)
            {
                this->openColorDialog(clicked, view, tab);
            }
        }
        break;

        case HighlightTab::Blacklist:
            break;
    }
}

void HighlightingPage::openMatchAppearanceDialog(const QModelIndex &clicked,
                                                 EditableModelView *view)
{
    auto color =
        view->getModel()->data(clicked, Qt::DecorationRole).value<QColor>();
    if (!color.isValid())
    {
        color = defaultHighlightMatchColor(
            *ColorProvider::instance().color(ColorType::SelfHighlight));
    }
    const auto style = static_cast<HighlightMatchStyle>(
        view->getModel()
            ->data(clicked, HighlightModel::MatchStyleRole)
            .toInt());
    const auto paintID = view->getModel()
                             ->data(clicked, HighlightModel::MatchPaintIDRole)
                             .toString();
    const auto paintAllowedData =
        view->getModel()->data(clicked, HighlightModel::MatchPaintAllowedRole);
    const bool allowPaint =
        !paintAllowedData.isValid() || paintAllowedData.toBool();

    const QPersistentModelIndex target(clicked);
    QPointer<MatchAppearanceDialog> dialog =
        new MatchAppearanceDialog(this, color, style, paintID, allowPaint);
    const auto cleanup = qScopeGuard([dialog] {
        delete dialog;
    });
    const auto result = dialog->exec();
    if (!dialog || result != QDialog::Accepted || !target.isValid())
    {
        return;
    }

    const auto label = highlightMatchAppearanceName(
        dialog->style(), !dialog->paintID().isEmpty());
    view->getModel()->setData(target, dialog->color(), Qt::DecorationRole);
    view->getModel()->setData(target, static_cast<int>(dialog->style()),
                              HighlightModel::MatchStyleRole);
    view->getModel()->setData(target, dialog->paintID(),
                              HighlightModel::MatchPaintIDRole);
    view->getModel()->setData(target, label, Qt::DisplayRole);
    view->getModel()->setData(
        target,
        highlightMatchAppearanceTooltip(dialog->color(), dialog->style(),
                                        dialog->paintID()),
        Qt::ToolTipRole);
}

void HighlightingPage::openChannelScopeDialog(const QModelIndex &clicked,
                                              EditableModelView *view)
{
    if (!clicked.isValid() || !clicked.flags().testFlag(Qt::ItemIsEnabled))
    {
        return;
    }

    const auto scope = HighlightModel::channelScopeFromData(
        view->getModel()->data(clicked, HighlightModel::ChannelScopeRole));
    const auto ruleName =
        view->getModel()
            ->data(view->getModel()->index(clicked.row(),
                                           HighlightModel::Column::Pattern),
                   Qt::DisplayRole)
            .toString();
    const QPersistentModelIndex target(clicked);
    QPointer<ChannelScopeDialog> dialog =
        new ChannelScopeDialog(this, ruleName, scope);
    const auto cleanup = qScopeGuard([dialog] {
        delete dialog;
    });
    const auto result = dialog->exec();
    if (!dialog || result != QDialog::Accepted || !target.isValid())
    {
        return;
    }

    const auto updated = dialog->scope();
    view->getModel()->setData(target, HighlightModel::channelScopeData(updated),
                              HighlightModel::ChannelScopeRole);
    view->getModel()->setData(
        target,
        highlightChannelScopeSummary(updated.mode(), updated.targets().size()),
        Qt::DisplayRole);
    view->getModel()->setData(
        target,
        highlightChannelScopeDescription(updated.mode(), updated.targets()),
        Qt::ToolTipRole);
}

void HighlightingPage::addChannelScopeButton(EditableModelView *view)
{
    auto *button = new QPushButton("Edit where it applies", view);
    button->setMinimumWidth(std::max(125, button->sizeHint().width()));
    button->setToolTip(
        "Choose the channels where the selected custom highlight can run.");
    button->setEnabled(false);
    view->addCustomButton(button);

    const auto updateButton = [view, button](const QModelIndex &current) {
        if (!current.isValid())
        {
            button->setEnabled(false);
            return;
        }
        const auto scopeIndex = view->getModel()->index(
            current.row(), HighlightModel::Column::ChannelScope);
        button->setEnabled(scopeIndex.flags().testFlag(Qt::ItemIsEnabled));
    };
    QObject::connect(
        view->getTableView()->selectionModel(),
        &QItemSelectionModel::currentChanged, this,
        [updateButton](const QModelIndex &current, const QModelIndex &) {
            updateButton(current);
        });
    QObject::connect(button, &QPushButton::clicked, this, [this, view] {
        const auto current =
            view->getTableView()->selectionModel()->currentIndex();
        if (!current.isValid())
        {
            return;
        }
        this->openChannelScopeDialog(
            view->getModel()->index(current.row(),
                                    HighlightModel::Column::ChannelScope),
            view);
    });
}

int HighlightingPage::selectedWordListIndex() const
{
    return this->wordLists_ ? this->wordLists_->indexOfTopLevelItem(
                                  this->wordLists_->currentItem())
                            : -1;
}

void HighlightingPage::refreshWordLists()
{
    if (!this->wordLists_)
    {
        return;
    }
    const auto previous = this->selectedWordListIndex();
    this->refreshingWordLists_ = true;
    QSignalBlocker blocker(this->wordLists_);
    this->wordLists_->clear();
    const auto lists = getSettings()->highlightWordLists.readOnly();
    for (const auto &list : *lists)
    {
        QStringList notifications;
        if (list.showInMentions())
        {
            notifications.append("Mentions");
        }
        if (list.hasAlert())
        {
            notifications.append("Taskbar");
        }
        if (list.hasSound())
        {
            notifications.append("Sound");
        }

        auto *item = new QTreeWidgetItem(
            {list.name(),
             list.isRegex() ? "Regular expressions" : "Words and phrases",
             QString::number(list.terms().size()),
             highlightWordListPlatformName(list.platform()),
             list.channels().empty() ? QStringLiteral("Everywhere")
                                     : QString::number(list.channels().size()),
             notifications.isEmpty() ? QStringLiteral("Highlight only")
                                     : notifications.join(", ")});
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, list.enabled() ? Qt::Checked : Qt::Unchecked);
        QPixmap swatch(16, 16);
        swatch.fill(*list.color());
        item->setIcon(0, QIcon(swatch));
        QString appearance = highlightMatchAppearanceName(
            list.matchStyle(), !list.matchPaintID().isEmpty());
        if (!list.matchPaintID().isEmpty())
        {
            appearance +=
                QString("\n7TV text paint %1").arg(list.matchPaintID());
        }
        item->setToolTip(
            0, QString("Message color %1\nExact marker %2")
                   .arg(list.color()->name(QColor::HexArgb), appearance));
        item->setToolTip(
            1, list.caseSensitive() ? "Case sensitive" : "Case insensitive");
        if (list.channels().empty())
        {
            item->setToolTip(4, "Applies in all channels");
        }
        else
        {
            QStringList channels;
            for (const auto &channel : list.channels())
            {
                channels.append(channel);
            }
            item->setToolTip(
                4,
                QString("Applies in %1 selected %2:\n%3")
                    .arg(list.channels().size())
                    .arg(list.channels().size() == 1 ? "channel" : "channels")
                    .arg(channels.join(", ")));
        }
        this->wordLists_->addTopLevelItem(item);
    }
    blocker.unblock();
    this->refreshingWordLists_ = false;
    if (this->wordLists_->topLevelItemCount() > 0)
    {
        this->wordLists_->setCurrentItem(
            this->wordLists_->topLevelItem(std::clamp(
                previous, 0, this->wordLists_->topLevelItemCount() - 1)));
    }
}

void HighlightingPage::createWordList()
{
    QPointer<WordListEditorDialog> dialog =
        new WordListEditorDialog(this, std::nullopt);
    const auto cleanup = qScopeGuard([dialog] {
        delete dialog;
    });
    const auto result = dialog->exec();
    if (!dialog || result != QDialog::Accepted)
    {
        return;
    }
    getSettings()->highlightWordLists.append(dialog->value());
    this->refreshWordLists();
    this->wordLists_->setCurrentItem(this->wordLists_->topLevelItem(
        this->wordLists_->topLevelItemCount() - 1));
}

void HighlightingPage::importWordList()
{
    const QPointer<HighlightingPage> self(this);
    auto imported = openWordListFile(this, "Import moderation list");
    if (!self || !imported)
    {
        return;
    }
    if (imported->chattySyntax)
    {
        this->importChattyHighlights(std::move(imported->text),
                                    std::move(imported->suggestedName));
        return;
    }
    QPointer<WordListEditorDialog> dialog = new WordListEditorDialog(
        this, std::nullopt, imported->text, imported->suggestedName,
        imported->suggestedRegex);
    const auto cleanup = qScopeGuard([dialog] {
        delete dialog;
    });
    const auto result = dialog->exec();
    if (!dialog || result != QDialog::Accepted)
    {
        return;
    }
    getSettings()->highlightWordLists.append(dialog->value());
    this->refreshWordLists();
    this->wordLists_->setCurrentItem(this->wordLists_->topLevelItem(
        this->wordLists_->topLevelItemCount() - 1));
}

void HighlightingPage::importChattyHighlights(QString source,
                                            QString suggestedName)
{
    QPointer<ChattyHighlightImportDialog> dialog =
        new ChattyHighlightImportDialog(this, std::move(source),
                                       std::move(suggestedName));
    const auto cleanup = qScopeGuard([dialog] {
        delete dialog;
    });
    const auto result = dialog->exec();
    if (!dialog || result != QDialog::Accepted)
    {
        return;
    }
    const auto first = getSettings()->highlightWordLists.readOnly()->size();
    for (auto &list : dialog->values())
    {
        getSettings()->highlightWordLists.append(std::move(list));
    }
    this->refreshWordLists();
    this->wordLists_->setCurrentItem(
        this->wordLists_->topLevelItem(static_cast<int>(first)));
}

void HighlightingPage::editSelectedWordList()
{
    const auto index = this->selectedWordListIndex();
    const auto lists = getSettings()->highlightWordLists.readOnly();
    if (index < 0 || index >= static_cast<int>(lists->size()))
    {
        return;
    }

    QPointer<WordListEditorDialog> dialog =
        new WordListEditorDialog(this, lists->at(index));
    const auto cleanup = qScopeGuard([dialog] {
        delete dialog;
    });
    const auto result = dialog->exec();
    if (!dialog || result != QDialog::Accepted ||
        getSettings()->highlightWordLists.readOnly() != lists)
    {
        return;
    }
    getSettings()->highlightWordLists.removeAt(index);
    getSettings()->highlightWordLists.insert(dialog->value(), index);
    this->refreshWordLists();
    this->wordLists_->setCurrentItem(this->wordLists_->topLevelItem(index));
}

void HighlightingPage::exportSelectedWordList()
{
    const auto index = this->selectedWordListIndex();
    const auto lists = getSettings()->highlightWordLists.readOnly();
    if (index < 0 || index >= static_cast<int>(lists->size()))
    {
        return;
    }
    const auto &list = lists->at(index);
    const QPointer<HighlightingPage> self(this);
    const auto fileName = QFileDialog::getSaveFileName(
        this, "Export moderation list", list.name() + ".txt",
        "Text files (*.txt)");
    if (!self || fileName.isEmpty())
    {
        return;
    }

    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        QMessageBox::warning(this, "Could not export list",
                             "Moltorino could not write the selected file.");
        return;
    }
    QStringList lines;
    for (const auto &term : list.terms())
    {
        lines.append(term);
    }
    file.write(lines.join(u'\n').toUtf8());
    file.write("\n");
}

void HighlightingPage::removeSelectedWordList()
{
    const auto index = this->selectedWordListIndex();
    const auto lists = getSettings()->highlightWordLists.readOnly();
    if (index < 0 || index >= static_cast<int>(lists->size()))
    {
        return;
    }
    const QPointer<HighlightingPage> self(this);
    const auto ruleCount = lists->at(index).terms().size();
    QPointer<QMessageBox> confirmation = new QMessageBox(
        QMessageBox::Question, "Remove moderation list",
        QString(ruleCount == 1 ? "Remove \"%1\" and its %2 rule?"
                               : "Remove \"%1\" and its %2 rules?")
            .arg(lists->at(index).name())
            .arg(ruleCount),
        QMessageBox::Yes | QMessageBox::No, this);
    const auto cleanup = qScopeGuard([confirmation] {
        delete confirmation;
    });
    confirmation->button(QMessageBox::Yes)->setText("Remove list");
    confirmation->button(QMessageBox::No)->setText("Cancel");
    const auto result = confirmation->exec();
    if (!self || !confirmation || result != QMessageBox::Yes ||
        getSettings()->highlightWordLists.readOnly() != lists)
    {
        return;
    }
    getSettings()->highlightWordLists.removeAt(index);
    this->refreshWordLists();
}

}
