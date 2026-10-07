#include "widgets/dialogs/ChatAutomationDialog.hpp"

#include "Application.hpp"
#include "messages/Message.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "widgets/dialogs/MoltorinoDialogTheme.hpp"
#include "widgets/dialogs/SettingsDialog.hpp"

#include <QAbstractItemModel>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStringListModel>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTextCursor>
#include <QTimeZone>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <ranges>
#include <tuple>
#include <unordered_map>

namespace {

using namespace chatterino;

constexpr auto RULE_ID_ROLE = Qt::UserRole;
constexpr auto RULE_TITLE_ROLE = Qt::UserRole + 1;
constexpr auto RULE_SUMMARY_ROLE = Qt::UserRole + 2;
constexpr auto RULE_ENABLED_ROLE = Qt::UserRole + 3;
constexpr auto RULE_SCOPE_ROLE = Qt::UserRole + 4;
constexpr auto CHANNEL_AVAILABLE_ROLE = Qt::UserRole + 1;
constexpr auto CHANNEL_LOCKED_ROLE = Qt::UserRole + 2;

struct AutomationExample {
    ChatAutomation rule;
    QString description;
    QString message;
};

const std::vector<AutomationExample> &automationExamples()
{
    static const auto examples = [] {
        std::vector<AutomationExample> result;
        auto add = [&result](const QString &name, const QString &description,
                             const QString &message) -> ChatAutomation & {
            result.push_back({{}, description, message});
            auto &rule = result.back().rule;
            rule.name = name;
            rule.enabled = false;
            return rule;
        };
        {
            auto &rule = add(
                "Greeting",
                "Replies to !hello, !hi or !hey with a greeting. Avoids repeats "
                "per channel, with a five minute cooldown per person. Edit the "
                "replies to suit your chat.",
                "!hi");
            rule.trigger = "!hello";
            rule.aliases = {"!hi", "!hey"};
            rule.response = "Hey {user.display_name}, good to see you!\n"
                            "Welcome in, {user.display_name}!\n"
                            "Hello {user.display_name}! How is your day going?";
            rule.arguments = ChatAutomationArguments::None;
            rule.action = ChatAutomationAction::ReplyToMessage;
            rule.randomResponse = true;
            rule.responseOrder = ChatAutomationResponseOrder::NoRepeat;
            rule.respondToSelf = false;
            rule.cooldownSeconds = 5;
            rule.userCooldownSeconds = 300;
        }
        {
            auto &rule =
                add("Current time",
                    "Shows your local time, date and time zone with !time or "
                    "!clock. Change the time zone and format in Trigger and "
                    "response.",
                    "!clock");
            rule.trigger = "!time";
            rule.aliases = {"!clock"};
            rule.arguments = ChatAutomationArguments::None;
            rule.response = "It is {time} on {date} ({time.zone}).";
            rule.timeFormat = "HH:mm";
            rule.dateFormat = "d MMM yyyy";
            rule.userCooldownSeconds = 60;
        }
        {
            auto &rule =
                add("Magic eight ball",
                    "Replies with a random answer to the question after !8ball. "
                    "Avoids repeats per channel, with a one minute cooldown per "
                    "person. Questions are limited to 240 characters.",
                    "!8ball Will we win this round?");
            rule.trigger = "!8ball";
            rule.arguments = ChatAutomationArguments::Required;
            rule.action = ChatAutomationAction::ReplyToMessage;
            rule.response = "\"{args}\" All signs point to yes.\n"
                            "\"{args}\" Not this time, {user.display_name}.\n"
                            "\"{args}\" Ask again after a snack.\n"
                            "\"{args}\" The odds are in your favor.\n"
                            "\"{args}\" Even the ball needs a moment.";
            rule.randomResponse = true;
            rule.responseOrder = ChatAutomationResponseOrder::NoRepeat;
            rule.maximumMessageLength = 240;
            rule.cooldownSeconds = 5;
            rule.userCooldownSeconds = 60;
        }
        {
            auto &rule = add(
                "Shoutout announcement",
                "A moderator can use !so @username to post a purple "
                "announcement linking to that channel. Your selected account "
                "needs moderator access too. Change /announcepurple for another "
                "color; usernames aren't verified.",
                "!so @viewer42");
            rule.trigger = "!shoutout";
            rule.aliases = {"!so"};
            rule.arguments = ChatAutomationArguments::Required;
            rule.action = ChatAutomationAction::RunCommand;
            rule.response = "/announcepurple Go show {target} some love: "
                            "https://twitch.tv/{target}";
            rule.access = ChatAutomationAccess::Moderators;
            rule.channelRole = ChatAutomationChannelRole::Moderator;
            rule.cooldownSeconds = 60;
        }
        {
            auto &rule =
                add("Choose between two",
                    "Picks between two options separated by | and replies to the "
                    "viewer. The pattern names them first and second. Each person "
                    "can use it every 30 seconds.",
                    "!choose pizza | sushi");
            rule.trigger =
                R"(^!choose\s+(?<first>[^|]*[^\s|])\s*\|\s*(?<second>[^|]*[^\s|])\s*$)";
            rule.match = ChatAutomationMatch::RegularExpression;
            rule.action = ChatAutomationAction::ReplyToMessage;
            rule.response = "{user.display_name}, try {match.first}.\n"
                            "{user.display_name}, try {match.second}.";
            rule.randomResponse = true;
            rule.maximumMessageLength = 250;
            rule.cooldownSeconds = 5;
            rule.userCooldownSeconds = 30;
        }
        {
            auto &rule =
                add("Personal timeout",
                    "Use !to @username for a 60 second timeout, or add a duration "
                    "such as 10m and an optional reason. Only responds to your "
                    "selected account in channels where it can moderate.",
                    "!to @viewer42 10m Repeated links");
            rule.trigger = "!timeout";
            rule.aliases = {"!to"};
            rule.arguments = ChatAutomationArguments::Required;
            rule.action = ChatAutomationAction::RunCommand;
            rule.response = "/timeout {target} {2;60} "
                            "{3+;Please follow the chat rules.}";
            rule.onlySelf = true;
            rule.channelRole = ChatAutomationChannelRole::Moderator;
            rule.cooldownSeconds = 5;
        }
        {
            auto &rule =
                add("Rotating reminders",
                    "Posts the next orange announcement with !reminder, cycling "
                    "through the lines per channel. Both the selected account and "
                    "sender need moderator access. Replace the reminders with your "
                    "own rules; this runs on request, not a timer.",
                    "!reminder");
            rule.trigger = "!reminder";
            rule.aliases = {"!remind"};
            rule.arguments = ChatAutomationArguments::None;
            rule.action = ChatAutomationAction::RunCommand;
            rule.response =
                "/announceorange Keep spoilers to yourself, please.\n"
                "/announceorange Be kind to each other and welcome new "
                "people.\n"
                "/announceorange Ask before posting links in chat.";
            rule.randomResponse = true;
            rule.responseOrder = ChatAutomationResponseOrder::InOrder;
            rule.access = ChatAutomationAccess::Moderators;
            rule.channelRole = ChatAutomationChannelRole::Moderator;
            rule.cooldownSeconds = 60;
        }
        {
            auto &rule =
                add("Chat check in",
                    "Viewers can use !checkin once an hour while your channel is "
                    "live. The counter counts uses across streams and restarts, not "
                    "unique viewers. Counter value in Test rule changes only the "
                    "preview.",
                    "!checkin");
            rule.trigger = "!checkin";
            rule.arguments = ChatAutomationArguments::None;
            rule.action = ChatAutomationAction::ReplyToMessage;
            rule.response =
                "Check in #{count}. Glad you are here, {user.display_name}!";
            rule.respondToSelf = false;
            rule.channelRole = ChatAutomationChannelRole::Broadcaster;
            rule.stream = ChatAutomationStream::Live;
            rule.cooldownSeconds = 0;
            rule.userCooldownSeconds = 3600;
        }
        {
            auto &rule =
                add("Offline schedule help",
                    "Replies with your Twitch schedule when someone mentions "
                    "schedule while your channel is offline. Ignores your own "
                    "messages, with a one minute channel cooldown and five minutes "
                    "per person. Keep your Twitch schedule up to date.",
                    "Where can I find the schedule?");
            rule.trigger = "schedule";
            rule.match = ChatAutomationMatch::ContainsWords;
            rule.action = ChatAutomationAction::ReplyToMessage;
            rule.response =
                "{user.display_name}, you can find the next "
                "streams here: https://twitch.tv/{channel.name}/schedule";
            rule.respondToSelf = false;
            rule.channelRole = ChatAutomationChannelRole::Broadcaster;
            rule.stream = ChatAutomationStream::Offline;
            rule.cooldownSeconds = 60;
            rule.userCooldownSeconds = 300;
        }
        return result;
    }();
    return examples;
}

struct AutomationVariable {
    const char *label;
    const char *token;
    const char *description;
};

constexpr AutomationVariable AUTOMATION_VARIABLES[]{
    {"Display name", "{user.display_name}",
     "The name shown in chat for the person who triggered this rule."},
    {"Username", "{user.name}",
     "The Twitch username of the person who triggered this rule."},
    {"User ID", "{user.id}", "Their Twitch user ID."},
    {"Text after trigger", "{args}", "Everything written after the trigger."},
    {"Target username", "{target}",
     "The first word after the trigger, without a leading @."},
    {"First word", "{1}", "The first word after the trigger."},
    {"Second word", "{2}", "The second word after the trigger."},
    {"From first word", "{1+}",
     "The first word after the trigger and everything after it."},
    {"Trigger", "{trigger}", "The command, alias, or text that matched."},
    {"Full message", "{msg.text}", "The complete incoming chat message."},
    {"Message ID", "{msg.id}", "The Twitch ID of the incoming message."},
    {"Channel", "{channel.name}", "The channel where the rule ran."},
    {"Channel ID", "{channel.id}", "The Twitch ID of that channel."},
    {"My username", "{my.name}", "The account sending the response."},
    {"My user ID", "{my.id}", "The Twitch ID of the sending account."},
    {"Stream title", "{stream.title}", "The current stream title."},
    {"Category", "{stream.game}", "The current stream category."},
    {"Time", "{time}", "The current time in the selected time zone."},
    {"Date", "{date}", "The current date in the selected time zone."},
    {"Date and time", "{datetime}",
     "The current date and time in the selected time zone."},
    {"Time zone", "{time.zone}",
     "The selected time zone's current abbreviation."},
    {"Weekday", "{date.weekday}",
     "The full weekday name in the selected time zone."},
    {"ISO date and time", "{datetime.iso}",
     "Date and time with its UTC offset."},
    {"Unix timestamp", "{timestamp}",
     "The number of seconds since the Unix epoch."},
    {"Rule count", "{count}",
     "Counts uses of this rule across its channels. Kept after restarting "
     "Moltorino."},
};

QString accessText(ChatAutomationAccess access)
{
    switch (access)
    {
        case ChatAutomationAccess::NamedUsers:
            return QStringLiteral("Selected users");
        case ChatAutomationAccess::Moderators:
            return QStringLiteral("Moderators");
        case ChatAutomationAccess::Broadcaster:
            return QStringLiteral("Broadcaster");
        case ChatAutomationAccess::Subscribers:
            return QStringLiteral("Subscribers");
        case ChatAutomationAccess::VIPs:
            return QStringLiteral("VIPs");
        default:
            return QStringLiteral("Everyone");
    }
}

QString matchText(ChatAutomationMatch match)
{
    switch (match)
    {
        case ChatAutomationMatch::Exact:
            return QStringLiteral("Exact");
        case ChatAutomationMatch::StartsWith:
            return QStringLiteral("Starts with");
        case ChatAutomationMatch::ContainsWords:
            return QStringLiteral("Contains words");
        case ChatAutomationMatch::ContainsText:
            return QStringLiteral("Contains text");
        case ChatAutomationMatch::RegularExpression:
            return QStringLiteral("Pattern");
        case ChatAutomationMatch::AnyMessage:
            return QStringLiteral("Any message");
        default:
            return QStringLiteral("Command");
    }
}

QString actionText(ChatAutomationAction action)
{
    switch (action)
    {
        case ChatAutomationAction::RunCommand:
            return QStringLiteral("Run command");
        case ChatAutomationAction::ReplyToMessage:
            return QStringLiteral("Reply");
        default:
            return QStringLiteral("Message");
    }
}

QString scopeText(const ChatAutomation &rule)
{
    if (rule.allOpenTwitchChannels)
    {
        if (rule.excludedChannels.isEmpty())
        {
            return QStringLiteral("All open tabs");
        }
        return rule.excludedChannels.size() == 1
                   ? QStringLiteral("All except 1 channel")
                   : QStringLiteral("All except %1 channels")
                         .arg(rule.excludedChannels.size());
    }
    if (rule.channels.isEmpty())
    {
        return QStringLiteral("No channels");
    }
    return rule.channels.size() == 1
               ? QStringLiteral("1 channel")
               : QStringLiteral("%1 channels").arg(rule.channels.size());
}

QString ruleSummary(const ChatAutomation &rule)
{
    return QStringLiteral("%1 · %2 · %3")
        .arg(matchText(rule.match), actionText(rule.action),
             accessText(rule.access));
}

class AutomationRuleDelegate final : public QStyledItemDelegate
{
public:
    explicit AutomationRuleDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    void setColors(QColor background, QColor hover, QColor selected,
                   QColor text, QColor selectedText, QColor muted,
                   QColor divider)
    {
        this->background_ = std::move(background);
        this->hover_ = std::move(hover);
        this->selected_ = std::move(selected);
        this->text_ = std::move(text);
        this->selectedText_ = std::move(selectedText);
        this->muted_ = std::move(muted);
        this->divider_ = std::move(divider);
    }

    void setFonts(QFont regular, QFont title)
    {
        this->regularFont_ = std::move(regular);
        this->titleFont_ = std::move(title);
    }

    QSize sizeHint(const QStyleOptionViewItem &,
                   const QModelIndex &) const override
    {
        return {210, std::max(QFontMetrics(this->titleFont_).height(),
                              QFontMetrics(this->regularFont_).height()) +
                         2 * QFontMetrics(this->regularFont_).height() + 14};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        painter->setClipRect(option.rect);
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        painter->fillRect(option.rect, selected  ? this->selected_
                                       : hovered ? this->hover_
                                                 : this->background_);

        const bool enabled = index.data(RULE_ENABLED_ROLE).toBool();
        const auto status =
            enabled ? QStringLiteral("Enabled") : QStringLiteral("Disabled");
        painter->setFont(this->regularFont_);
        const QFontMetrics statusMetrics(this->regularFont_);
        const auto statusWidth = statusMetrics.horizontalAdvance(status);
        const auto statusRight = option.rect.right() - 10;
        const auto statusLeft = statusRight - statusWidth;
        painter->setPen(selected  ? this->selectedText_
                        : enabled ? this->text_
                                  : this->muted_);
        painter->drawText(QRect(statusLeft, option.rect.top() + 5, statusWidth,
                                statusMetrics.height()),
                          Qt::AlignRight | Qt::AlignVCenter, status);

        const auto contentLeft = option.rect.left() + 10;
        const auto contentRight = statusLeft - 10;
        const auto contentWidth = std::max(0, contentRight - contentLeft);
        painter->setFont(this->titleFont_);
        painter->setPen(selected ? this->selectedText_ : this->text_);
        const QFontMetrics titleMetrics(this->titleFont_);
        const auto title =
            titleMetrics.elidedText(index.data(RULE_TITLE_ROLE).toString(),
                                    Qt::ElideRight, contentWidth);
        painter->drawText(QRect(contentLeft, option.rect.top() + 5,
                                contentWidth, titleMetrics.height()),
                          Qt::AlignLeft | Qt::AlignVCenter, title);

        painter->setFont(this->regularFont_);
        painter->setPen(selected ? this->selectedText_ : this->muted_);
        const QFontMetrics summaryMetrics(this->regularFont_);
        const auto rowWidth = option.rect.right() - 10 - contentLeft;
        const auto summary = summaryMetrics.elidedText(
            index.data(RULE_SUMMARY_ROLE).toString(), Qt::ElideRight, rowWidth);
        const auto summaryTop =
            option.rect.top() + 7 +
            std::max(titleMetrics.height(), statusMetrics.height());
        painter->drawText(QRect(contentLeft, summaryTop, rowWidth,
                                summaryMetrics.height()),
                          Qt::AlignLeft | Qt::AlignVCenter, summary);

        const auto scope = summaryMetrics.elidedText(
            index.data(RULE_SCOPE_ROLE).toString(), Qt::ElideRight, rowWidth);
        painter->drawText(
            QRect(contentLeft, summaryTop + summaryMetrics.height() + 2,
                  rowWidth, summaryMetrics.height()),
            Qt::AlignLeft | Qt::AlignVCenter, scope);

        painter->setPen(this->divider_);
        painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
        painter->restore();
    }

private:
    QColor background_;
    QColor hover_;
    QColor selected_;
    QColor text_;
    QColor selectedText_;
    QColor muted_;
    QColor divider_;
    QFont regularFont_;
    QFont titleFont_;
};

class AutomationChannelDelegate final : public QStyledItemDelegate
{
public:
    explicit AutomationChannelDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    void setColors(QColor background, QColor hover, QColor selected,
                   QColor text, QColor muted, QColor divider, QColor accent)
    {
        this->background_ = std::move(background);
        this->hover_ = std::move(hover);
        this->selected_ = std::move(selected);
        this->text_ = std::move(text);
        this->muted_ = std::move(muted);
        this->divider_ = std::move(divider);
        this->accent_ = std::move(accent);
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &) const override
    {
        return {option.rect.width(),
                std::max(28, QFontMetrics(option.font).height() + 10)};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        painter->setClipRect(option.rect);
        const auto background =
            option.state & QStyle::State_Selected    ? this->selected_
            : option.state & QStyle::State_MouseOver ? this->hover_
                                                     : this->background_;
        painter->fillRect(option.rect, background);

        constexpr auto SIDE_PADDING = 8;
        const auto checkSize = std::min(14, option.rect.height() - 10);
        const QRect checkRect(option.rect.left() + SIDE_PADDING,
                              option.rect.center().y() - checkSize / 2,
                              checkSize, checkSize);
        const bool checked =
            index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
        const bool locked = index.data(CHANNEL_LOCKED_ROLE).toBool();
        const bool available = index.data(CHANNEL_AVAILABLE_ROLE).toBool();

        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(checked ? this->accent_ : this->muted_, 1));
        auto fill = this->accent_;
        fill.setAlpha(checked ? 38 : 0);
        painter->setBrush(fill);
        painter->drawRect(QRectF(checkRect).adjusted(0.5, 0.5, -0.5, -0.5));
        if (checked)
        {
            QPainterPath tick;
            tick.moveTo(checkRect.left() + checkSize * 0.22,
                        checkRect.top() + checkSize * 0.52);
            tick.lineTo(checkRect.left() + checkSize * 0.43,
                        checkRect.top() + checkSize * 0.72);
            tick.lineTo(checkRect.left() + checkSize * 0.8,
                        checkRect.top() + checkSize * 0.27);
            painter->setPen(QPen(this->accent_, 1.4, Qt::SolidLine,
                                 Qt::RoundCap, Qt::RoundJoin));
            painter->setBrush(Qt::NoBrush);
            painter->drawPath(tick);
        }

        const auto name = index.data(Qt::DisplayRole).toString();
        const auto status =
            available ? QString{} : QStringLiteral("Tab closed");
        const QFontMetrics metrics(option.font);
        const auto left = checkRect.right() + 9;
        const auto right = option.rect.right() - SIDE_PADDING;
        const auto statusWidth =
            status.isEmpty() ? 0 : metrics.horizontalAdvance(status);
        const auto nameWidth = std::max(
            0, right - left - (statusWidth == 0 ? 0 : statusWidth + 12));
        painter->setFont(option.font);
        painter->setPen(locked || !available ? this->muted_ : this->text_);
        painter->drawText(
            QRect(left, option.rect.top(), nameWidth, option.rect.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            metrics.elidedText(name, Qt::ElideRight, nameWidth));
        if (!status.isEmpty())
        {
            painter->setPen(this->muted_);
            painter->drawText(QRect(right - statusWidth + 1, option.rect.top(),
                                    statusWidth, option.rect.height()),
                              Qt::AlignRight | Qt::AlignVCenter, status);
        }

        painter->setPen(this->divider_);
        painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
        painter->restore();
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *model,
                     const QStyleOptionViewItem &,
                     const QModelIndex &index) override
    {
        bool activate = false;
        if (event->type() == QEvent::MouseButtonRelease)
        {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            activate = mouse->button() == Qt::LeftButton;
        }
        else if (event->type() == QEvent::KeyPress)
        {
            const auto *key = static_cast<QKeyEvent *>(event);
            activate = key->key() == Qt::Key_Space ||
                       key->key() == Qt::Key_Return ||
                       key->key() == Qt::Key_Enter;
        }
        if (!activate)
        {
            return false;
        }
        if (index.data(CHANNEL_LOCKED_ROLE).toBool())
        {
            return true;
        }
        const auto next = index.data(Qt::CheckStateRole).toInt() == Qt::Checked
                              ? Qt::Unchecked
                              : Qt::Checked;
        return model->setData(index, next, Qt::CheckStateRole);
    }

private:
    QColor background_;
    QColor hover_;
    QColor selected_;
    QColor text_;
    QColor muted_;
    QColor divider_;
    QColor accent_;
};

class AutomationResponseEdit final : public QPlainTextEdit
{
public:
    explicit AutomationResponseEdit(QWidget *parent)
        : QPlainTextEdit(parent)
    {
        QStringList tokens;
        for (const auto &variable : AUTOMATION_VARIABLES)
        {
            tokens.push_back(QString::fromUtf8(variable.token));
        }
        this->completer_ = new QCompleter(tokens, this);
        this->completer_->setWidget(this);
        this->completer_->setCaseSensitivity(Qt::CaseInsensitive);
        this->completer_->setCompletionMode(QCompleter::PopupCompletion);
        QObject::connect(this->completer_,
                         qOverload<const QString &>(&QCompleter::activated),
                         this, [this](const QString &token) {
                             auto cursor = this->textCursor();
                             cursor.movePosition(
                                 QTextCursor::Left, QTextCursor::KeepAnchor,
                                 this->completer_->completionPrefix().size());
                             cursor.insertText(token);
                             this->setTextCursor(cursor);
                         });
    }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (this->completer_->popup()->isVisible() &&
            (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
             event->key() == Qt::Key_Escape || event->key() == Qt::Key_Tab))
        {
            event->ignore();
            return;
        }
        QPlainTextEdit::keyPressEvent(event);
        const auto cursor = this->textCursor();
        const auto before = this->toPlainText().left(cursor.position());
        const auto brace = before.lastIndexOf(QLatin1Char('{'));
        const auto prefix = brace < 0 ? QString{} : before.mid(brace);
        if (prefix.isEmpty() || prefix.contains(QLatin1Char('}')) ||
            prefix.contains(QLatin1Char(' ')) ||
            prefix.contains(QLatin1Char('\n')) ||
            (brace > 0 && before.at(brace - 1) == QLatin1Char('{')))
        {
            this->completer_->popup()->hide();
            return;
        }
        this->completer_->setCompletionPrefix(prefix);
        this->completer_->popup()->setCurrentIndex(
            this->completer_->completionModel()->index(0, 0));
        auto rect = this->cursorRect();
        rect.setWidth(std::max(
            220, this->completer_->popup()->sizeHintForColumn(0) + 20));
        this->completer_->complete(rect);
    }

private:
    QCompleter *completer_{};
};

QString selectedTimeZone(const QComboBox *combo)
{
    const auto id = combo->currentText().trimmed();
    if (id.compare(QStringLiteral("Local time"), Qt::CaseInsensitive) == 0)
    {
        return {};
    }
    return id;
}

QStringList parseNames(const QString &text)
{
    static const QRegularExpression separators(QStringLiteral("[,\\s]+"));
    auto names = text.toCaseFolded().split(separators, Qt::SkipEmptyParts);
    for (auto &name : names)
    {
        if (name.startsWith(QLatin1Char('@')) ||
            name.startsWith(QLatin1Char('#')))
        {
            name.remove(0, 1);
        }
    }
    names.removeAll(QString{});
    names.removeDuplicates();
    return names;
}

QString ruleTitle(const ChatAutomation &rule)
{
    if (!rule.name.isEmpty())
    {
        return rule.name;
    }
    if (rule.match == ChatAutomationMatch::AnyMessage)
    {
        return QStringLiteral("Any message");
    }
    return rule.trigger.isEmpty() ? QStringLiteral("New rule") : rule.trigger;
}

void updateRuleItem(QListWidgetItem *item, const ChatAutomation &rule)
{
    const auto title = ruleTitle(rule);
    const auto summary = ruleSummary(rule);
    const auto scope = scopeText(rule);
    item->setText(title);
    item->setData(RULE_ID_ROLE, rule.id);
    item->setData(RULE_TITLE_ROLE, title);
    item->setData(RULE_SUMMARY_ROLE, summary);
    item->setData(RULE_ENABLED_ROLE, rule.enabled);
    item->setData(RULE_SCOPE_ROLE, scope);
    item->setToolTip(QStringLiteral("%1\n%2\n%3\n%4")
                         .arg(title, summary, scope,
                              rule.enabled ? QStringLiteral("Enabled")
                                           : QStringLiteral("Disabled")));
}

void compactCombo(QComboBox *combo)
{
    combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    combo->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
}

void loadFormat(QComboBox *combo, QLineEdit *custom, const QString &pattern)
{
    const auto index = combo->findData(pattern);
    combo->setCurrentIndex(index < 0 ? combo->count() - 1 : index);
    custom->setText(pattern);
}

QString selectedFormat(const QComboBox *combo, const QLineEdit *custom)
{
    return combo->currentData().isNull() ? custom->text()
                                         : combo->currentData().toString();
}

QWidget *controlRow(QWidget *parent, std::initializer_list<QWidget *> controls)
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    for (auto *control : controls)
    {
        layout->addWidget(control);
    }
    row->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    return row;
}

void setDuration(QSpinBox *value, QComboBox *unit, int seconds)
{
    const int index = seconds > 0 && seconds % 3600 == 0 ? 2
                      : seconds > 0 && seconds % 60 == 0 ? 1
                                                         : 0;
    unit->setCurrentIndex(index);
    const int factor = unit->currentData().toInt();
    value->setMaximum(86400 / factor);
    value->setValue(seconds / factor);
}

int durationSeconds(const QSpinBox *value, const QComboBox *unit)
{
    return value->value() * unit->currentData().toInt();
}

QString durationText(int seconds)
{
    if (seconds > 0 && seconds % 3600 == 0)
    {
        return seconds == 3600 ? QStringLiteral("1 hour")
                               : QStringLiteral("%1 hours").arg(seconds / 3600);
    }
    if (seconds > 0 && seconds % 60 == 0)
    {
        return seconds == 60 ? QStringLiteral("1 minute")
                             : QStringLiteral("%1 minutes").arg(seconds / 60);
    }
    return seconds == 1 ? QStringLiteral("1 second")
                        : QStringLiteral("%1 seconds").arg(seconds);
}

QString channelRoleText(ChatAutomationChannelRole role)
{
    switch (role)
    {
        case ChatAutomationChannelRole::Moderator:
            return QStringLiteral("Where I moderate");
        case ChatAutomationChannelRole::Broadcaster:
            return QStringLiteral("My own channel");
        case ChatAutomationChannelRole::VIP:
            return QStringLiteral("Where I am VIP or moderator");
        default:
            return {};
    }
}

}

namespace chatterino {

void ChatAutomationDialog::showDialog(QString initialChannel, QWidget *parent)
{
    static QPointer<ChatAutomationDialog> dialog;
    if (dialog)
    {
        dialog->initialChannel_ =
            std::move(initialChannel).trimmed().toCaseFolded();
        dialog->refreshPreview();
        if (dialog->isMinimized())
        {
            dialog->setWindowState(
                (dialog->windowState() & ~Qt::WindowMinimized) |
                Qt::WindowActive);
        }
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
        dialog->setFocus(Qt::ActiveWindowFocusReason);
        return;
    }

    if (getApp()->getChatAutomations() == nullptr)
    {
        return;
    }

    dialog = new ChatAutomationDialog(std::move(initialChannel), parent);
    dialog->show();
}

ChatAutomationDialog::ChatAutomationDialog(QString initialChannel,
                                           QWidget *parent)
    : QDialog(parent)
    , controller_(getApp()->getChatAutomations())
    , rules_(this->controller_->rules())
    , initialChannel_(std::move(initialChannel).trimmed().toCaseFolded())
{
    this->setWindowTitle(QStringLiteral("Chat automations"));
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setMinimumSize(860, 580);
    this->resize(1000, 700);
    this->setFont(getApp()->getFonts()->getFont(FontStyle::UiMedium, 1.F));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 11, 12, 11);
    root->setSpacing(9);

    auto *header = new QHBoxLayout;
    this->masterEnabled_ =
        new QCheckBox(QStringLiteral("Enable automations"), this);
    this->masterEnabled_->setObjectName(QStringLiteral("automationsEnabled"));
    this->masterEnabled_->setChecked(this->controller_->enabled());
    header->addWidget(this->masterEnabled_);
    header->addStretch();

    auto *settingsButton =
        new QPushButton(QStringLiteral("Automation settings"), this);
    settingsButton->setObjectName(QStringLiteral("automationSettings"));
    settingsButton->setAutoDefault(false);
    header->addWidget(settingsButton);
    root->addLayout(header);

    auto *globalSettings = new QDialog(this, Qt::Popup);
    globalSettings->setObjectName(QStringLiteral("globalAutomationSettings"));
    globalSettings->setWindowTitle(QStringLiteral("Automation settings"));
    globalSettings->setMinimumWidth(360);
    auto *globalGrid = new QGridLayout(globalSettings);
    globalGrid->setContentsMargins(12, 12, 12, 12);
    globalGrid->setVerticalSpacing(10);
    auto *globalTitle =
        new QLabel(QStringLiteral("Automation settings"), globalSettings);
    auto globalTitleFont = globalTitle->font();
    globalTitleFont.setBold(true);
    globalTitle->setFont(globalTitleFont);
    globalGrid->addWidget(globalTitle, 0, 0, 1, 2);

    this->initialBackground_ =
        getSettings()->chatAutomationsRunInBackground.getValue();
    this->initialMaxRuns_ = std::clamp(
        getSettings()->chatAutomationsMaxRunsPer30Seconds.getValue(), 1, 10);
    this->runInBackground_ =
        new QCheckBox(QStringLiteral("Run in the background"), globalSettings);
    this->runInBackground_->setObjectName(QStringLiteral("runInBackground"));
    this->runInBackground_->setChecked(this->initialBackground_);
    this->runInBackground_->setToolTip(
        QStringLiteral("Keep rules active while Moltorino is minimized or "
                       "another app is focused."));
    globalGrid->addWidget(this->runInBackground_, 1, 0, 1, 2);

    auto *maxRunsLabel = new QLabel(
        QStringLiteral("Maximum actions per 30 seconds"), globalSettings);
    this->maxRuns_ = new QSpinBox(globalSettings);
    this->maxRuns_->setObjectName(QStringLiteral("maximumActions"));
    this->maxRuns_->setRange(1, 10);
    this->maxRuns_->setValue(this->initialMaxRuns_);
    maxRunsLabel->setBuddy(this->maxRuns_);
    globalGrid->addWidget(maxRunsLabel, 2, 0);
    globalGrid->addWidget(this->maxRuns_, 2, 1, Qt::AlignLeft);
    globalGrid->setColumnStretch(0, 1);

    this->runStatus_ = new QLabel(globalSettings);
    this->runStatus_->setObjectName(QStringLiteral("runStatus"));
    this->runStatus_->setWordWrap(true);
    globalGrid->addWidget(this->runStatus_, 3, 0, 1, 2);

    auto *done = new QPushButton(QStringLiteral("Done"), globalSettings);
    QObject::connect(done, &QPushButton::clicked, globalSettings,
                     &QDialog::accept);
    globalGrid->addWidget(done, 4, 0, 1, 2, Qt::AlignRight);

    QObject::connect(
        settingsButton, &QPushButton::clicked, this,
        [this, globalSettings, settingsButton] {
            this->refreshGlobalSettings();
            globalSettings->adjustSize();
            const auto *screen = settingsButton->screen();
            if (!screen)
            {
                return;
            }
            const auto available = screen->availableGeometry();
            auto position = settingsButton->mapToGlobal(
                QPoint(settingsButton->width() - globalSettings->width(),
                       settingsButton->height() + 4));
            position.setX(std::clamp(
                position.x(), available.left(),
                std::max(available.left(),
                         available.right() - globalSettings->width() + 1)));
            position.setY(std::clamp(
                position.y(), available.top(),
                std::max(available.top(),
                         available.bottom() - globalSettings->height() + 1)));
            globalSettings->move(position);
            globalSettings->show();
            this->runInBackground_->setFocus(Qt::PopupFocusReason);
        });

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    root->addWidget(splitter, 1);

    auto *left = new QWidget(splitter);
    left->setMinimumWidth(220);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 4, 0);
    leftLayout->setSpacing(7);

    auto *rulesHeader = new QHBoxLayout;
    auto *rulesTitle = new QLabel(QStringLiteral("Rules"), left);
    auto titleFont = rulesTitle->font();
    titleFont.setBold(true);
    rulesTitle->setFont(titleFont);
    rulesHeader->addWidget(rulesTitle);
    rulesHeader->addStretch();

    auto *examples = new QPushButton(QStringLiteral("Examples"), left);
    examples->setObjectName(QStringLiteral("examples"));
    examples->setAutoDefault(false);
    QObject::connect(examples, &QPushButton::clicked, this,
                     &ChatAutomationDialog::showExamples);
    rulesHeader->addWidget(examples);
    leftLayout->addLayout(rulesHeader);

    this->ruleList_ = new QListWidget(left);
    this->ruleList_->setObjectName(QStringLiteral("ruleList"));
    this->ruleList_->setToolTip(
        QStringLiteral("Drag rules to change their order, or use Alt+Up and "
                       "Alt+Down while the list is focused."));
    this->ruleList_->setAlternatingRowColors(false);
    this->ruleList_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    this->ruleList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->ruleList_->setMouseTracking(true);
    this->ruleList_->setDragDropMode(QAbstractItemView::InternalMove);
    this->ruleList_->setDefaultDropAction(Qt::MoveAction);
    this->ruleList_->setDragDropOverwriteMode(false);
    this->ruleList_->setDropIndicatorShown(true);
    this->ruleList_->setItemDelegate(
        new AutomationRuleDelegate(this->ruleList_));
    leftLayout->addWidget(this->ruleList_, 1);

    auto *ruleTools = new QHBoxLayout;
    ruleTools->setSpacing(5);
    auto *add = new QPushButton(QStringLiteral("Add rule"), left);
    this->duplicate_ = new QPushButton(QStringLiteral("Duplicate"), left);
    this->remove_ = new QPushButton(QStringLiteral("Delete"), left);
    for (auto *button : {add, this->duplicate_, this->remove_})
    {
        button->setAutoDefault(false);
        ruleTools->addWidget(button);
    }
    leftLayout->addLayout(ruleTools);

    this->undoRemove_ = new QPushButton(QStringLiteral("Undo delete"), left);
    this->undoRemove_->setObjectName(QStringLiteral("undoDelete"));
    this->undoRemove_->setAutoDefault(false);
    this->undoRemove_->hide();
    leftLayout->addWidget(this->undoRemove_, 0, Qt::AlignLeft);
    QObject::connect(this->undoRemove_, &QPushButton::clicked, this, [this] {
        this->undoRemoveRule();
    });

    auto *right = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(4, 0, 0, 0);
    this->empty_ = new QLabel(
        QStringLiteral("Add a rule or start with an example."), right);
    this->empty_->setAlignment(Qt::AlignCenter);
    this->empty_->setObjectName(QStringLiteral("SecondaryText"));
    rightLayout->addWidget(this->empty_, 1);

    this->tabs_ = new QTabWidget(right);
    this->tabs_->setObjectName(QStringLiteral("automationTabs"));
    rightLayout->addWidget(this->tabs_, 1);

    auto *scroll = new QScrollArea(this->tabs_);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    this->editor_ = new QWidget(scroll);
    scroll->setWidget(this->editor_);
    this->tabs_->addTab(scroll, QStringLiteral("Trigger and response"));

    auto *editorLayout = new QVBoxLayout(this->editor_);
    editorLayout->setContentsMargins(8, 8, 8, 8);
    editorLayout->setSpacing(10);

    auto makeSection = [this, editorLayout](const QString &title) {
        auto *section = new QGroupBox(title, this->editor_);
        auto *layout = new QGridLayout(section);
        layout->setHorizontalSpacing(8);
        layout->setVerticalSpacing(7);
        editorLayout->addWidget(section);
        return std::pair{section, layout};
    };

    auto *identity = new QHBoxLayout;
    this->name_ = new QLineEdit(this->editor_);
    this->name_->setObjectName(QStringLiteral("ruleName"));
    this->name_->setPlaceholderText(QStringLiteral("Optional name"));
    identity->addWidget(new QLabel(QStringLiteral("Name"), this->editor_));
    identity->addWidget(this->name_, 1);
    this->ruleEnabled_ =
        new QCheckBox(QStringLiteral("Enable rule"), this->editor_);
    this->ruleEnabled_->setObjectName(QStringLiteral("ruleEnabled"));
    identity->addWidget(this->ruleEnabled_);
    editorLayout->addLayout(identity);

    auto [triggerSection, triggerGrid] = makeSection(QStringLiteral("Trigger"));
    triggerGrid->addWidget(new QLabel(QStringLiteral("Match"), triggerSection),
                           0, 0);
    this->match_ = new QComboBox(triggerSection);
    this->match_->setObjectName(QStringLiteral("matchMode"));
    const std::pair<const char *, ChatAutomationMatch> matches[]{
        {"Chat command", ChatAutomationMatch::Command},
        {"Exact message", ChatAutomationMatch::Exact},
        {"Starts with", ChatAutomationMatch::StartsWith},
        {"Contains words", ChatAutomationMatch::ContainsWords},
        {"Contains text", ChatAutomationMatch::ContainsText},
        {"Regular expression", ChatAutomationMatch::RegularExpression},
        {"Any message", ChatAutomationMatch::AnyMessage},
    };
    for (const auto &[label, mode] : matches)
    {
        this->match_->addItem(QString::fromUtf8(label), static_cast<int>(mode));
    }
    this->caseSensitive_ =
        new QCheckBox(QStringLiteral("Match case"), triggerSection);
    this->caseSensitive_->setObjectName(QStringLiteral("caseSensitive"));
    triggerGrid->addWidget(
        controlRow(triggerSection, {this->match_, this->caseSensitive_}), 0, 1,
        Qt::AlignLeft);

    this->triggerLabel_ =
        new QLabel(QStringLiteral("Chat command"), triggerSection);
    triggerGrid->addWidget(this->triggerLabel_, 1, 0);
    this->trigger_ = new QLineEdit(triggerSection);
    this->trigger_->setObjectName(QStringLiteral("trigger"));
    this->trigger_->setPlaceholderText(QStringLiteral("!hello"));
    this->trigger_->setClearButtonEnabled(true);
    triggerGrid->addWidget(this->trigger_, 1, 1);

    this->aliasesLabel_ =
        new QLabel(QStringLiteral("Other names"), triggerSection);
    this->aliases_ = new QLineEdit(triggerSection);
    this->aliases_->setObjectName(QStringLiteral("aliases"));
    this->aliases_->setPlaceholderText(QStringLiteral("!hi, !hey"));
    triggerGrid->addWidget(this->aliasesLabel_, 2, 0);
    triggerGrid->addWidget(this->aliases_, 2, 1);

    this->argumentLabel_ =
        new QLabel(QStringLiteral("Text after trigger"), triggerSection);
    this->argumentMode_ = new QComboBox(triggerSection);
    this->argumentMode_->setObjectName(QStringLiteral("argumentMode"));
    this->argumentMode_->addItems({QStringLiteral("Optional"),
                                   QStringLiteral("Required"),
                                   QStringLiteral("Not allowed")});
    triggerGrid->addWidget(this->argumentLabel_, 3, 0);
    triggerGrid->addWidget(this->argumentMode_, 3, 1, Qt::AlignLeft);

    this->matchHint_ = new QLabel(triggerSection);
    this->matchHint_->setObjectName(QStringLiteral("SecondaryText"));
    this->matchHint_->setWordWrap(true);
    triggerGrid->addWidget(this->matchHint_, 4, 0, 1, 2);
    triggerGrid->setColumnStretch(1, 1);

    auto [responseSection, responseGrid] =
        makeSection(QStringLiteral("Response"));
    this->action_ = new QComboBox(responseSection);
    this->action_->setObjectName(QStringLiteral("action"));
    this->action_->addItem(QStringLiteral("Send message"),
                           static_cast<int>(ChatAutomationAction::SendMessage));
    this->action_->addItem(
        QStringLiteral("Reply to message"),
        static_cast<int>(ChatAutomationAction::ReplyToMessage));
    this->action_->addItem(QStringLiteral("Run Moltorino command"),
                           static_cast<int>(ChatAutomationAction::RunCommand));
    this->useBotBadge_ =
        new QCheckBox(QStringLiteral("Use bot badge"), responseSection);
    responseGrid->addWidget(
        controlRow(responseSection, {this->action_, this->useBotBadge_}), 0, 0,
        1, 3, Qt::AlignLeft);

    this->responseMode_ = new QComboBox(responseSection);
    this->responseMode_->setObjectName(QStringLiteral("responseMode"));
    this->responseMode_->addItems(
        {QStringLiteral("One response"), QStringLiteral("Random response"),
         QStringLiteral("Random, avoid repeats"), QStringLiteral("In order")});
    responseGrid->addWidget(this->responseMode_, 1, 0, Qt::AlignLeft);
    this->responseHint_ = new QLabel(responseSection);
    this->responseHint_->setObjectName(QStringLiteral("SecondaryText"));
    this->responseHint_->setWordWrap(true);
    responseGrid->addWidget(this->responseHint_, 1, 1, 1, 2);

    this->response_ = new AutomationResponseEdit(responseSection);
    this->response_->setObjectName(QStringLiteral("response"));
    this->response_->setMinimumHeight(86);
    this->response_->setMaximumHeight(145);
    this->response_->setTabChangesFocus(true);
    responseGrid->addWidget(this->response_, 2, 0, 1, 3);

    this->variable_ =
        new QPushButton(QStringLiteral("Insert variable"), responseSection);
    this->variable_->setObjectName(QStringLiteral("insertVariable"));
    this->variable_->setAutoDefault(false);
    auto *variables = new QMenu(this->variable_);
    const QStringList categories{
        QStringLiteral("Person"), QStringLiteral("Message and text"),
        QStringLiteral("Channel and stream"), QStringLiteral("Sending account"),
        QStringLiteral("Date and time")};
    QList<QMenu *> variableGroups;
    for (const auto &category : categories)
    {
        auto *menu = variables->addMenu(category);
        menu->setToolTipsVisible(true);
        variableGroups.push_back(menu);
    }
    for (const auto &variable : AUTOMATION_VARIABLES)
    {
        const auto token = QString::fromUtf8(variable.token);
        int group = 1;
        if (token.startsWith("{user."))
        {
            group = 0;
        }
        else if (token.startsWith("{my."))
        {
            group = 3;
        }
        else if (token.startsWith("{channel.") || token.startsWith("{stream."))
        {
            group = 2;
        }
        else if (token.startsWith("{time") || token.startsWith("{date"))
        {
            group = 4;
        }
        auto *entry = variableGroups.at(group)->addAction(
            QStringLiteral("%1  %2").arg(QString::fromUtf8(variable.label),
                                         token),
            this, [this, token] {
                this->insertVariable(token);
            });
        entry->setToolTip(QString::fromUtf8(variable.description));
        entry->setData(token);
    }
    variables->addSeparator();
    auto *captures = variables->addMenu(QStringLiteral("Captured groups"));
    QObject::connect(
        variables, &QMenu::aboutToShow, this, [this, captures, variableGroups] {
            captures->clear();
            const bool regex =
                this->match_->currentData().toInt() ==
                static_cast<int>(ChatAutomationMatch::RegularExpression);
            for (auto *entry : variableGroups.at(1)->actions())
            {
                const auto token = entry->data().toString();
                if (token == "{1}" || token == "{2}" || token == "{1+}" ||
                    token == "{target}")
                {
                    entry->setVisible(!regex);
                }
                if (token == "{args}")
                {
                    entry->setText(
                        regex ? QStringLiteral("Text after match  {args}")
                              : QStringLiteral("Text after trigger  {args}"));
                }
            }
            if (regex && this->trigger_->text().size() <= 512)
            {
                const QRegularExpression pattern(this->trigger_->text());
                const auto names = pattern.namedCaptureGroups();
                if (pattern.isValid())
                {
                    for (int i = 1; i <= pattern.captureCount(); ++i)
                    {
                        const auto token =
                            names.value(i).isEmpty()
                                ? QStringLiteral("{%1}").arg(i)
                                : QStringLiteral("{match.%1}").arg(names.at(i));
                        captures->addAction(token, this, [this, token] {
                            this->insertVariable(token);
                        });
                    }
                }
            }
            captures->menuAction()->setVisible(!captures->isEmpty());
        });
    auto *fallback = variables->addAction(
        QStringLiteral("Default text when empty  {args;friend}"), this, [this] {
            this->insertVariable(QStringLiteral("{args;friend}"));
        });
    fallback->setToolTip(
        QStringLiteral("Uses friend when no arguments are given."));
    variables->setToolTipsVisible(true);
    this->variable_->setMenu(variables);
    responseGrid->addWidget(this->variable_, 3, 0, Qt::AlignLeft);
    auto *variableHint = new QLabel(
        QStringLiteral("Variables in {braces} are filled in for each message."),
        responseSection);
    variableHint->setObjectName(QStringLiteral("SecondaryText"));
    variableHint->setWordWrap(true);
    responseGrid->addWidget(variableHint, 3, 1, 1, 2);

    this->timeZoneLabel_ =
        new QLabel(QStringLiteral("Time zone"), responseSection);
    responseGrid->addWidget(this->timeZoneLabel_, 4, 0);
    this->timeZone_ = new QComboBox(responseSection);
    this->timeZone_->setObjectName(QStringLiteral("timeZone"));
    this->timeZone_->setEditable(true);
    this->timeZone_->setInsertPolicy(QComboBox::NoInsert);
    this->timeZone_->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
    this->timeZone_->setMinimumContentsLength(22);
    this->timeZone_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    this->timeZone_->addItem(QStringLiteral("Local time"), QString{});
    this->timeZone_->addItem(QStringLiteral("UTC"), QStringLiteral("UTC"));
    for (const auto &id : QTimeZone::availableTimeZoneIds())
    {
        if (id != QByteArrayLiteral("UTC"))
        {
            this->timeZone_->addItem(QString::fromUtf8(id),
                                     QString::fromUtf8(id));
        }
    }
    this->timeZone_->completer()->setCaseSensitivity(Qt::CaseInsensitive);
    this->timeZone_->completer()->setFilterMode(Qt::MatchContains);
    responseGrid->addWidget(this->timeZone_, 4, 1, 1, 2, Qt::AlignLeft);

    this->timeFormatLabel_ =
        new QLabel(QStringLiteral("Time format"), responseSection);
    this->timeFormat_ = new QComboBox(responseSection);
    this->timeFormat_->setObjectName(QStringLiteral("timeFormat"));
    const std::pair<const char *, const char *> clockFormats[]{
        {"24 hour (20:30)", "HH:mm"},
        {"12 hour (8:30 PM)", "h:mm AP"},
        {"24 hour (20:30:15)", "HH:mm:ss"},
        {"12 hour (8:30:15 PM)", "h:mm:ss AP"},
        {"Lowercase (8:30 pm)", "h:mm ap"},
        {"Milliseconds (20:30:15.125)", "HH:mm:ss.zzz"},
    };
    for (const auto &[label, pattern] : clockFormats)
    {
        this->timeFormat_->addItem(QString::fromUtf8(label),
                                   QString::fromUtf8(pattern));
    }
    this->timeFormat_->addItem(QStringLiteral("Custom format"));
    responseGrid->addWidget(this->timeFormatLabel_, 5, 0);
    responseGrid->addWidget(this->timeFormat_, 5, 1, 1, 2, Qt::AlignLeft);

    auto makeCustomFormat = [responseSection](const QString &objectName,
                                              const QString &example,
                                              const QString &help) {
        auto *row = new QWidget(responseSection);
        auto *layout = new QVBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(5);
        auto *edit = new QLineEdit(row);
        edit->setObjectName(objectName);
        edit->setMaxLength(128);
        edit->setPlaceholderText(example);
        layout->addWidget(edit);
        auto *hint = new QLabel(help, row);
        hint->setObjectName(QStringLiteral("SecondaryText"));
        hint->setWordWrap(true);
        layout->addWidget(hint);
        return std::pair{row, edit};
    };

    std::tie(this->customTimeRow_, this->customTimeFormat_) = makeCustomFormat(
        QStringLiteral("customTimeFormat"), QStringLiteral("HH:mm:ss 'in' t"),
        QStringLiteral(
            "HH: hour (00 to 23), hh: hour (01 to 12 with AP), mm: minute, "
            "ss: second, AP: AM/PM, t: zone. Put literal text in single "
            "quotes."));
    this->customTimeFormat_->setAccessibleName(
        QStringLiteral("Custom time format"));
    responseGrid->addWidget(this->customTimeRow_, 6, 1, 1, 2);

    this->dateFormatLabel_ =
        new QLabel(QStringLiteral("Date format"), responseSection);
    this->dateFormat_ = new QComboBox(responseSection);
    this->dateFormat_->setObjectName(QStringLiteral("dateFormat"));
    const std::pair<const char *, const char *> dateFormats[]{
        {"Year first (2026-09-10)", "yyyy-MM-dd"},
        {"Day first (10/09/2026)", "dd/MM/yyyy"},
        {"Month first (09/10/2026)", "MM/dd/yyyy"},
        {"Dots (10.09.2026)", "dd.MM.yyyy"},
        {"Named month (10 Sep 2026)", "d MMM yyyy"},
        {"Thursday, 10 September 2026", "dddd, d MMMM yyyy"},
    };
    for (const auto &[label, pattern] : dateFormats)
    {
        this->dateFormat_->addItem(QString::fromUtf8(label),
                                   QString::fromUtf8(pattern));
    }
    this->dateFormat_->addItem(QStringLiteral("Custom format"));
    responseGrid->addWidget(this->dateFormatLabel_, 7, 0);
    responseGrid->addWidget(this->dateFormat_, 7, 1, 1, 2, Qt::AlignLeft);

    std::tie(this->customDateRow_, this->customDateFormat_) = makeCustomFormat(
        QStringLiteral("customDateFormat"), QStringLiteral("ddd, d MMM yyyy"),
        QStringLiteral(
            "dd: day, MM: month, yyyy: year, ddd/dddd: weekday, "
            "MMM/MMMM: month name. Put literal text in single quotes."));
    this->customDateFormat_->setAccessibleName(
        QStringLiteral("Custom date format"));
    responseGrid->addWidget(this->customDateRow_, 8, 1, 1, 2);

    this->formatPreview_ = new QLabel(responseSection);
    this->formatPreview_->setObjectName(QStringLiteral("dateTimePreview"));
    this->formatPreview_->setTextFormat(Qt::PlainText);
    this->formatPreview_->setWordWrap(true);
    responseGrid->addWidget(this->formatPreview_, 9, 0, 1, 3);

    this->botSetup_ = new QPushButton(
        QStringLiteral("Set up bot badge account"), responseSection);
    this->botSetup_->setAutoDefault(false);
    responseGrid->addWidget(this->botSetup_, 10, 0, 1, 3, Qt::AlignLeft);
    QObject::connect(this->botSetup_, &QPushButton::clicked, this, [] {
        SettingsDialog::showDialog(SettingsDialogPreference::BotBadge);
    });
    responseGrid->setColumnStretch(2, 1);

    auto *conditionSummaryRow = new QHBoxLayout;
    this->conditionsSummary_ = new QLabel(this->editor_);
    this->conditionsSummary_->setObjectName(QStringLiteral("SecondaryText"));
    this->conditionsSummary_->setTextFormat(Qt::PlainText);
    this->conditionsSummary_->setWordWrap(true);
    conditionSummaryRow->addWidget(this->conditionsSummary_, 1);

    auto *editConditions =
        new QPushButton(QStringLiteral("Edit conditions"), this->editor_);
    editConditions->setAutoDefault(false);
    conditionSummaryRow->addWidget(editConditions);
    editorLayout->addLayout(conditionSummaryRow);
    editorLayout->addStretch();

    auto *conditionsScroll = new QScrollArea(this->tabs_);
    conditionsScroll->setWidgetResizable(true);
    conditionsScroll->setFrameShape(QFrame::NoFrame);
    auto *conditions = new QWidget(conditionsScroll);
    conditionsScroll->setWidget(conditions);
    this->tabs_->addTab(conditionsScroll, QStringLiteral("Conditions"));
    QObject::connect(editConditions, &QPushButton::clicked, this, [this] {
        this->tabs_->setCurrentIndex(1);
    });

    auto *conditionsLayout = new QVBoxLayout(conditions);
    conditionsLayout->setContentsMargins(8, 8, 8, 8);
    conditionsLayout->setSpacing(12);
    auto *availability =
        new QGroupBox(QStringLiteral("Who can trigger this rule"), conditions);
    auto *availabilityGrid = new QGridLayout(availability);
    availabilityGrid->setHorizontalSpacing(8);
    availabilityGrid->setVerticalSpacing(7);
    conditionsLayout->addWidget(availability);
    availabilityGrid->addWidget(
        new QLabel(QStringLiteral("Messages from"), availability), 0, 0);
    this->sender_ = new QComboBox(availability);
    this->sender_->setObjectName(QStringLiteral("messageSource"));
    this->sender_->addItems({QStringLiteral("Anyone"),
                             QStringLiteral("Other people"),
                             QStringLiteral("My selected account")});
    availabilityGrid->addWidget(this->sender_, 0, 1, Qt::AlignLeft);

    availabilityGrid->addWidget(
        new QLabel(QStringLiteral("Audience"), availability), 1, 0);
    this->access_ = new QComboBox(availability);
    const std::pair<const char *, ChatAutomationAccess> accessOptions[]{
        {"Everyone", ChatAutomationAccess::Everyone},
        {"Subscribers and moderators", ChatAutomationAccess::Subscribers},
        {"VIPs and moderators", ChatAutomationAccess::VIPs},
        {"Moderators", ChatAutomationAccess::Moderators},
        {"Broadcaster only", ChatAutomationAccess::Broadcaster},
        {"Selected usernames", ChatAutomationAccess::NamedUsers},
    };
    for (const auto &[label, access] : accessOptions)
    {
        this->access_->addItem(QString::fromUtf8(label),
                               static_cast<int>(access));
    }
    this->access_->setObjectName(QStringLiteral("audience"));
    availabilityGrid->addWidget(this->access_, 1, 1, Qt::AlignLeft);

    this->usersLabel_ = new QLabel(QStringLiteral("Usernames"), availability);
    this->users_ = new QLineEdit(availability);
    this->users_->setObjectName(QStringLiteral("allowedUsers"));
    this->users_->setPlaceholderText(QStringLiteral("alice, bob"));
    availabilityGrid->addWidget(this->usersLabel_, 2, 0);
    availabilityGrid->addWidget(this->users_, 2, 1);

    availabilityGrid->addWidget(
        new QLabel(QStringLiteral("Ignore users"), availability), 3, 0);
    this->ignoredUsers_ = new QLineEdit(availability);
    this->ignoredUsers_->setObjectName(QStringLiteral("ignoredUsers"));
    this->ignoredUsers_->setPlaceholderText(
        QStringLiteral("nightbot, streamelements"));
    availabilityGrid->addWidget(this->ignoredUsers_, 3, 1);

    auto *messageSection =
        new QGroupBox(QStringLiteral("Message filters"), conditions);
    auto *messageGrid = new QGridLayout(messageSection);
    messageGrid->setVerticalSpacing(7);
    this->requiredText_ = new QLineEdit(messageSection);
    this->requiredText_->setObjectName(QStringLiteral("requiredText"));
    this->requiredText_->setMaxLength(512);
    this->requiredText_->setPlaceholderText(
        QStringLiteral("Optional word or phrase"));

    this->excludedText_ = new QLineEdit(messageSection);
    this->excludedText_->setObjectName(QStringLiteral("excludedText"));
    this->excludedText_->setMaxLength(512);
    this->excludedText_->setPlaceholderText(
        QStringLiteral("Optional word or phrase"));
    messageGrid->addWidget(
        new QLabel(QStringLiteral("Must contain"), messageSection), 0, 0);
    messageGrid->addWidget(this->requiredText_, 0, 1, 1, 3);
    messageGrid->addWidget(
        new QLabel(QStringLiteral("Must not contain"), messageSection), 1, 0);
    messageGrid->addWidget(this->excludedText_, 1, 1, 1, 3);

    this->minimumLength_ = new QSpinBox(messageSection);
    this->minimumLength_->setObjectName(QStringLiteral("minimumMessageLength"));
    this->minimumLength_->setRange(0, 4096);
    this->minimumLength_->setSpecialValueText(QStringLiteral("Any"));
    this->maximumLength_ = new QSpinBox(messageSection);
    this->maximumLength_->setObjectName(QStringLiteral("maximumMessageLength"));
    this->maximumLength_->setRange(0, 4096);
    this->maximumLength_->setSpecialValueText(QStringLiteral("No limit"));
    messageGrid->addWidget(
        new QLabel(QStringLiteral("Message length"), messageSection), 2, 0);
    auto *lengthRow = controlRow(
        messageSection, {new QLabel(QStringLiteral("Minimum"), messageSection),
                         this->minimumLength_,
                         new QLabel(QStringLiteral("Maximum"), messageSection),
                         this->maximumLength_});
    messageGrid->addWidget(lengthRow, 2, 1, 1, 3, Qt::AlignLeft);

    auto *messageHint = new QLabel(
        QStringLiteral("Checks the full message, including the "
                       "command. Uses Match case from Trigger and response."),
        messageSection);
    messageHint->setObjectName(QStringLiteral("SecondaryText"));
    messageHint->setWordWrap(true);
    messageGrid->addWidget(messageHint, 3, 0, 1, 4);
    messageGrid->setColumnStretch(3, 1);
    conditionsLayout->addWidget(messageSection);

    auto *limits =
        new QGroupBox(QStringLiteral("Time between uses"), conditions);
    auto *limitsGrid = new QGridLayout(limits);
    limitsGrid->addWidget(new QLabel(QStringLiteral("Per channel"), limits), 0,
                          0);
    this->cooldown_ = new QSpinBox(limits);
    this->cooldown_->setObjectName(QStringLiteral("channelCooldown"));
    this->cooldown_->setRange(0, 86400);
    this->cooldown_->setSpecialValueText(QStringLiteral("Off"));
    this->cooldownUnit_ = new QComboBox(limits);
    this->cooldownUnit_->setObjectName(QStringLiteral("channelCooldownUnit"));
    limitsGrid->addWidget(
        controlRow(limits, {this->cooldown_, this->cooldownUnit_}), 0, 1,
        Qt::AlignLeft);

    limitsGrid->addWidget(new QLabel(QStringLiteral("Per person"), limits), 0,
                          2);
    this->userCooldown_ = new QSpinBox(limits);
    this->userCooldown_->setObjectName(QStringLiteral("userCooldown"));
    this->userCooldown_->setRange(0, 86400);
    this->userCooldown_->setSpecialValueText(QStringLiteral("Off"));
    this->userCooldownUnit_ = new QComboBox(limits);
    this->userCooldownUnit_->setObjectName(QStringLiteral("userCooldownUnit"));
    limitsGrid->addWidget(
        controlRow(limits, {this->userCooldown_, this->userCooldownUnit_}), 0,
        3, Qt::AlignLeft);

    for (auto [value, unit] :
         {std::pair{this->cooldown_, this->cooldownUnit_},
          std::pair{this->userCooldown_, this->userCooldownUnit_}})
    {
        unit->addItem(QStringLiteral("seconds"), 1);
        unit->addItem(QStringLiteral("minutes"), 60);
        unit->addItem(QStringLiteral("hours"), 3600);
        QObject::connect(
            unit, &QComboBox::currentIndexChanged, value, [value, unit] {
                value->setMaximum(86400 / unit->currentData().toInt());
            });
    }

    this->cooldownHint_ = new QLabel(limits);
    this->cooldownHint_->setObjectName(QStringLiteral("SecondaryText"));
    this->cooldownHint_->setWordWrap(true);
    limitsGrid->addWidget(this->cooldownHint_, 1, 0, 1, 5);
    limitsGrid->setColumnStretch(4, 1);

    auto *channelSection =
        new QGroupBox(QStringLiteral("Where and when"), conditions);
    auto *channelGrid = new QGridLayout(channelSection);
    channelGrid->setVerticalSpacing(7);
    channelGrid->addWidget(new QLabel(QStringLiteral("Stream"), channelSection),
                           2, 0);
    this->stream_ = new QComboBox(channelSection);
    this->stream_->setObjectName(QStringLiteral("streamCondition"));
    this->stream_->addItems({QStringLiteral("Live or offline"),
                             QStringLiteral("Live only"),
                             QStringLiteral("Offline only")});
    channelGrid->addWidget(this->stream_, 2, 1, Qt::AlignLeft);

    channelGrid->addWidget(
        new QLabel(QStringLiteral("Channels"), channelSection), 0, 0);
    this->channelMode_ = new QComboBox(channelSection);
    this->channelMode_->setObjectName(QStringLiteral("channelMode"));
    this->channelMode_->addItems({QStringLiteral("All open channels"),
                                  QStringLiteral("Selected channels"),
                                  QStringLiteral("All except selected")});
    channelGrid->addWidget(this->channelMode_, 0, 1, Qt::AlignLeft);

    channelGrid->addWidget(
        new QLabel(QStringLiteral("My role"), channelSection), 1, 0);
    this->channelRole_ = new QComboBox(channelSection);
    this->channelRole_->setObjectName(QStringLiteral("channelRole"));
    this->channelRole_->addItems(
        {QStringLiteral("Any role"), QStringLiteral("Moderator or broadcaster"),
         QStringLiteral("Broadcaster (my channel)"),
         QStringLiteral("VIP, moderator or broadcaster")});
    channelGrid->addWidget(this->channelRole_, 1, 1, Qt::AlignLeft);
    channelGrid->setColumnStretch(1, 1);
    conditionsLayout->addWidget(channelSection);

    this->channelScope_ = new QLabel(channelSection);
    this->channelScope_->setObjectName(QStringLiteral("SecondaryText"));
    this->channelScope_->setWordWrap(true);
    channelGrid->addWidget(this->channelScope_, 3, 0, 1, 2);
    this->channelPicker_ = new QWidget(channelSection);
    auto *channelLayout = new QVBoxLayout(this->channelPicker_);
    channelLayout->setContentsMargins(0, 0, 0, 0);

    auto *channelTools = new QHBoxLayout;
    this->channelSearch_ = new QLineEdit(this->channelPicker_);
    this->channelSearch_->setPlaceholderText(
        QStringLiteral("Find or add a channel"));
    this->channelSearch_->setClearButtonEnabled(true);
    this->channelSearch_->setObjectName(QStringLiteral("channelSearch"));
    channelTools->addWidget(this->channelSearch_, 1);

    this->channelAdd_ =
        new QPushButton(QStringLiteral("Add channel"), this->channelPicker_);
    this->channelAdd_->setObjectName(QStringLiteral("addChannel"));
    this->channelAdd_->setAutoDefault(false);
    channelTools->addWidget(this->channelAdd_);
    QObject::connect(this->channelAdd_, &QPushButton::clicked, this, [this] {
        this->addChannel();
    });
    QObject::connect(this->channelSearch_, &QLineEdit::returnPressed, this,
                     [this] {
                         this->addChannel();
                     });

    this->channelToggle_ =
        new QPushButton(QStringLiteral("Select shown"), this->channelPicker_);
    this->channelToggle_->setAutoDefault(false);
    channelTools->addWidget(this->channelToggle_);
    channelLayout->addLayout(channelTools);

    this->channels_ = new QListWidget(this->channelPicker_);
    this->channels_->setObjectName(QStringLiteral("channelList"));
    this->channels_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    this->channels_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->channels_->setMouseTracking(true);
    this->channels_->setFrameShape(QFrame::NoFrame);
    this->channels_->setItemDelegate(
        new AutomationChannelDelegate(this->channels_));
    this->channels_->setMinimumHeight(95);
    this->channels_->setMaximumHeight(170);
    channelLayout->addWidget(this->channels_);
    channelGrid->addWidget(this->channelPicker_, 4, 0, 1, 2);

    conditionsLayout->addWidget(limits);
    conditionsLayout->addStretch();
    availabilityGrid->setColumnStretch(1, 1);

    this->validation_ = new QLabel(right);
    this->validation_->setWordWrap(true);
    this->validation_->setTextFormat(Qt::PlainText);
    this->validation_->setObjectName(QStringLiteral("ruleValidation"));
    rightLayout->addWidget(this->validation_);

    auto *test = new QWidget(this->tabs_);
    test->setAutoFillBackground(true);
    this->tabs_->addTab(test, QStringLiteral("Test rule"));
    auto *testLayout = new QVBoxLayout(test);
    testLayout->setContentsMargins(12, 12, 12, 12);
    testLayout->setSpacing(9);

    auto *testHint = new QLabel(
        QStringLiteral(
            "Try your unsaved rule with a sample message. This preview never "
            "sends messages or runs commands."),
        test);
    testHint->setWordWrap(true);
    testHint->setObjectName(QStringLiteral("SecondaryText"));
    testLayout->addWidget(testHint);

    auto *testGrid = new QGridLayout;
    testGrid->addWidget(new QLabel(QStringLiteral("Message"), test), 0, 0);
    this->testMessage_ = new QLineEdit(test);
    this->testMessage_->setObjectName(QStringLiteral("testMessage"));
    this->testMessage_->setPlaceholderText(QStringLiteral("!hello some words"));
    this->testMessage_->setMaxLength(4096);
    testGrid->addWidget(this->testMessage_, 0, 1, 1, 3);

    testGrid->addWidget(new QLabel(QStringLiteral("Username"), test), 1, 0);
    this->testUser_ = new QLineEdit(QStringLiteral("Viewer42"), test);
    this->testUser_->setObjectName(QStringLiteral("testUser"));
    testGrid->addWidget(this->testUser_, 1, 1);

    testGrid->addWidget(new QLabel(QStringLiteral("Channel"), test), 1, 2);
    this->testChannel_ = new QLineEdit(this->initialChannel_, test);
    this->testChannel_->setPlaceholderText(QStringLiteral("channel"));
    this->testChannel_->setObjectName(QStringLiteral("testChannel"));
    testGrid->addWidget(this->testChannel_, 1, 3);

    testGrid->addWidget(new QLabel(QStringLiteral("Their role"), test), 2, 0);
    this->testRole_ = new QComboBox(test);
    this->testRole_->setObjectName(QStringLiteral("testRole"));
    this->testRole_->addItems(
        {QStringLiteral("Regular viewer"), QStringLiteral("Subscriber"),
         QStringLiteral("VIP"), QStringLiteral("Moderator"),
         QStringLiteral("Broadcaster")});
    testGrid->addWidget(this->testRole_, 2, 1, Qt::AlignLeft);

    testGrid->addWidget(new QLabel(QStringLiteral("Stream"), test), 2, 2);
    this->testStream_ = new QComboBox(test);
    this->testStream_->setObjectName(QStringLiteral("testStream"));
    this->testStream_->addItems(
        {QStringLiteral("Live"), QStringLiteral("Offline")});
    testGrid->addWidget(this->testStream_, 2, 3, Qt::AlignLeft);

    this->testSelf_ =
        new QCheckBox(QStringLiteral("From my selected account"), test);
    this->testSelf_->setObjectName(QStringLiteral("testSelf"));
    testGrid->addWidget(this->testSelf_, 3, 0, 1, 2);

    testGrid->addWidget(new QLabel(QStringLiteral("My role"), test), 3, 2);
    this->testMyRole_ = new QComboBox(test);
    this->testMyRole_->setObjectName(QStringLiteral("testMyRole"));
    this->testMyRole_->addItems(
        {QStringLiteral("Regular viewer"), QStringLiteral("VIP"),
         QStringLiteral("Moderator"), QStringLiteral("Broadcaster")});
    testGrid->addWidget(this->testMyRole_, 3, 3, Qt::AlignLeft);

    this->testChoiceLabel_ = new QLabel(QStringLiteral("Response line"), test);
    testGrid->addWidget(this->testChoiceLabel_, 4, 0);
    this->testChoice_ = new QSpinBox(test);
    this->testChoice_->setRange(1, 1);
    this->testChoice_->setObjectName(QStringLiteral("testChoice"));
    testGrid->addWidget(this->testChoice_, 4, 1, Qt::AlignLeft);

    this->testCounterLabel_ = new QLabel(QStringLiteral("Counter value"), test);
    this->testCounter_ = new QSpinBox(test);
    this->testCounter_->setObjectName(QStringLiteral("testCounter"));
    this->testCounter_->setRange(1, 1000000000);
    this->testCounter_->setToolTip(QStringLiteral(
        "A sample value. Testing never changes your saved counter."));
    testGrid->addWidget(this->testCounterLabel_, 5, 0);
    testGrid->addWidget(this->testCounter_, 5, 1, Qt::AlignLeft);
    testLayout->addLayout(testGrid);

    this->testStatus_ = new QLabel(test);
    this->testStatus_->setObjectName(QStringLiteral("testStatus"));
    this->testStatus_->setWordWrap(true);
    testLayout->addWidget(this->testStatus_);

    this->testArguments_ = new QPlainTextEdit(test);
    this->testArguments_->setObjectName(QStringLiteral("testArguments"));
    this->testArguments_->setReadOnly(true);
    this->testArguments_->setMinimumHeight(44);
    this->testArguments_->setMaximumHeight(96);
    this->testArguments_->setFrameShape(QFrame::NoFrame);
    testLayout->addWidget(this->testArguments_);

    this->previewLabel_ = new QLabel(test);
    testLayout->addWidget(this->previewLabel_);
    this->preview_ = new QPlainTextEdit(test);
    this->preview_->setObjectName(QStringLiteral("preview"));
    this->preview_->setReadOnly(true);
    this->preview_->setPlaceholderText(
        QStringLiteral("The response appears here when the message matches."));
    testLayout->addWidget(this->preview_, 1);

    auto *testLimits = new QLabel(
        QStringLiteral("Live delivery also needs an open tab, a connected "
                       "account and permission to send. Cooldowns and overall "
                       "send limits still apply."),
        test);
    testLimits->setWordWrap(true);
    testLimits->setObjectName(QStringLiteral("SecondaryText"));
    testLayout->addWidget(testLimits);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({245, 755});

    auto *footer = new QHBoxLayout;
    auto *priority = new QLabel(
        QStringLiteral("Rules run from top to bottom. The first match wins."),
        this);
    priority->setObjectName(QStringLiteral("SecondaryText"));
    priority->setWordWrap(true);
    footer->addWidget(priority, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    this->save_ = buttons->addButton(QStringLiteral("Save automations"),
                                     QDialogButtonBox::AcceptRole);
    this->save_->setObjectName(QStringLiteral("saveAutomations"));
    for (auto *button : {this->save_, buttons->button(QDialogButtonBox::Cancel)})
    {
        button->setAutoDefault(false);
        button->setDefault(false);
    }

    auto *saveShortcut = new QShortcut(QKeySequence::Save, this);
    QObject::connect(saveShortcut, &QShortcut::activated, this, [this] {
        this->save();
    });
    footer->addWidget(buttons);
    root->addLayout(footer);

    for (auto *combo :
         {this->match_, this->action_, this->responseMode_, this->argumentMode_,
          this->timeFormat_, this->dateFormat_, this->access_, this->sender_,
          this->stream_, this->channelMode_, this->channelRole_,
          this->cooldownUnit_, this->userCooldownUnit_, this->testRole_,
          this->testStream_, this->testMyRole_})
    {
        compactCombo(combo);
    }
    for (auto *spin : {this->cooldown_, this->userCooldown_, this->maxRuns_,
                       this->testChoice_, this->testCounter_,
                       this->minimumLength_, this->maximumLength_})
    {
        spin->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    }

    QObject::connect(add, &QPushButton::clicked, this, [this] {
        this->addRule();
    });
    QObject::connect(this->duplicate_, &QPushButton::clicked, this, [this] {
        this->duplicateRule();
    });
    QObject::connect(this->remove_, &QPushButton::clicked, this, [this] {
        this->removeRule();
    });
    QObject::connect(this->ruleList_, &QListWidget::currentRowChanged, this,
                     [this](int row) {
                         if (!this->reordering_)
                         {
                             this->loadRule(row);
                         }
                     });
    QObject::connect(this->ruleList_->model(),
                     &QAbstractItemModel::rowsAboutToBeMoved, this, [this] {
                         this->storeCurrentRule();
                         this->reordering_ = true;
                     });
    QObject::connect(this->ruleList_->model(), &QAbstractItemModel::rowsMoved,
                     this, [this] {
                         this->syncRuleOrderFromList();
                         this->reordering_ = false;
                     });
    QObject::connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        this->save();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                     &QDialog::reject);

    const auto editChanged = [this] {
        if (this->loading_)
        {
            return;
        }
        this->storeCurrentRule();
        this->refreshCurrentRuleListItem();
        this->refreshEditorState();
        this->refreshMatchHint();
        this->refreshPreview();
    };

    for (auto *edit :
         {this->name_, this->trigger_, this->aliases_, this->users_,
          this->ignoredUsers_, this->requiredText_, this->excludedText_,
          this->customTimeFormat_, this->customDateFormat_})
    {
        QObject::connect(edit, &QLineEdit::textChanged, this, editChanged);
    }
    QObject::connect(this->response_, &QPlainTextEdit::textChanged, this,
                     editChanged);

    for (auto *check :
         {this->ruleEnabled_, this->caseSensitive_, this->useBotBadge_})
    {
        QObject::connect(check, &QCheckBox::toggled, this, editChanged);
    }

    for (auto *combo :
         {this->match_, this->action_, this->responseMode_, this->access_,
          this->argumentMode_, this->stream_, this->sender_, this->channelRole_,
          this->cooldownUnit_, this->userCooldownUnit_})
    {
        QObject::connect(combo, &QComboBox::currentIndexChanged, this,
                         editChanged);
    }

    for (auto [combo, custom] :
         {std::pair{this->timeFormat_, this->customTimeFormat_},
          std::pair{this->dateFormat_, this->customDateFormat_}})
    {
        QObject::connect(
            combo, &QComboBox::currentIndexChanged, this,
            [combo, custom, editChanged] {
                if (!combo->currentData().isNull())
                {
                    const QSignalBlocker blocker(custom);
                    custom->setText(combo->currentData().toString());
                }
                editChanged();
            });
    }

    for (auto *spin : {this->cooldown_, this->userCooldown_,
                       this->minimumLength_, this->maximumLength_})
    {
        QObject::connect(spin, &QSpinBox::valueChanged, this, editChanged);
    }
    QObject::connect(this->timeZone_, &QComboBox::currentTextChanged, this,
                     editChanged);

    QObject::connect(this->channelSearch_, &QLineEdit::textChanged, this,
                     [this](const QString &query) {
                         this->filterChannels(query);
                     });
    QObject::connect(this->channelToggle_, &QPushButton::clicked, this, [this] {
        this->toggleShownChannels();
    });
    QObject::connect(
        this->channelMode_, &QComboBox::currentIndexChanged, this,
        [this](int mode) {
            if (this->loading_ || this->currentRule_ < 0)
            {
                return;
            }
            this->rules_[this->currentRule_].allOpenTwitchChannels = mode != 1;
            if (mode == 0)
            {
                this->rules_[this->currentRule_].excludedChannels.clear();
            }
            this->refreshChannels();
            this->refreshCurrentRuleListItem();
            this->refreshEditorState();
            this->refreshPreview();
        });
    QObject::connect(this->channels_, &QListWidget::itemChanged, this,
                     editChanged);
    for (auto *edit : {this->testMessage_, this->testUser_, this->testChannel_})
    {
        QObject::connect(edit, &QLineEdit::textChanged, this, [this] {
            this->refreshPreview();
        });
    }

    for (auto *combo : {this->testRole_, this->testStream_, this->testMyRole_})
    {
        QObject::connect(combo, &QComboBox::currentIndexChanged, this, [this] {
            this->refreshPreview();
        });
    }
    QObject::connect(this->testSelf_, &QCheckBox::toggled, this, [this] {
        this->refreshPreview();
    });
    QObject::connect(this->testChoice_, &QSpinBox::valueChanged, this, [this] {
        this->refreshPreview();
    });
    QObject::connect(this->testCounter_, &QSpinBox::valueChanged, this, [this] {
        this->refreshPreview();
    });
    QObject::connect(this->masterEnabled_, &QCheckBox::toggled, this, [this] {
        this->refreshPreview();
    });
    QObject::connect(this->runInBackground_, &QCheckBox::toggled, this, [this] {
        this->refreshEditorState();
    });
    QObject::connect(this->maxRuns_, &QSpinBox::valueChanged, this, [this] {
        this->refreshEditorState();
    });

    for (int direction : {-1, 1})
    {
        auto *shortcut =
            new QShortcut(QKeySequence(direction < 0 ? Qt::ALT | Qt::Key_Up
                                                     : Qt::ALT | Qt::Key_Down),
                          this->ruleList_);
        shortcut->setContext(Qt::WidgetShortcut);
        QObject::connect(shortcut, &QShortcut::activated, this,
                         [this, direction] {
                             this->moveRule(direction);
                         });
    }

    this->openChannelsConnection_ =
        this->controller_->openChannelsChanged.connect([this] {
            this->refreshChannels();
            this->refreshEditorState();
        });
    this->rulesConnection_ = this->controller_->rulesChanged.connect(
        [this, lastEnabled = this->controller_->enabled()]() mutable {
            if (this->applying_)
            {
                return;
            }
            const auto enabled = this->controller_->enabled();
            if (this->masterEnabled_->isChecked() == lastEnabled)
            {
                const QSignalBlocker blocker(this->masterEnabled_);
                this->masterEnabled_->setChecked(enabled);
            }
            lastEnabled = enabled;
            this->refreshPreview();
        });

    for (auto *label : this->findChildren<QLabel *>())
    {
        label->setTextFormat(Qt::PlainText);
    }

    installMoltorinoDialogTheme(this, [this] {
        this->refreshStyle();
        this->refreshRuleList();
        this->refreshChannels();
    });
    this->refreshRuleList();
    if (!this->rules_.empty())
    {
        this->ruleList_->setCurrentRow(0);
    }
    this->refreshEditorState();
}

void ChatAutomationDialog::showExamples()
{
    if (auto *existing = this->findChild<QDialog *>("automationExamples"))
    {
        if (existing->isVisible())
        {
            existing->raise();
            existing->activateWindow();
            return;
        }
        existing->setObjectName({});
    }

    auto *picker = new QDialog(this);
    picker->setObjectName(QStringLiteral("automationExamples"));
    picker->setWindowTitle(QStringLiteral("Automation examples"));
    picker->setAttribute(Qt::WA_DeleteOnClose);
    picker->resize(760, 440);
    picker->setMinimumSize(660, 400);
    auto *layout = new QVBoxLayout(picker);
    auto *body = new QHBoxLayout;
    auto *list = new QListWidget(picker);
    list->setObjectName(QStringLiteral("exampleList"));
    list->setMinimumWidth(205);
    list->setSpacing(2);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    for (const auto &example : automationExamples())
    {
        list->addItem(example.rule.name);
    }
    body->addWidget(list, 2);

    auto *details = new QWidget(picker);
    auto *detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(4, 0, 0, 0);
    auto *description = new QLabel(details);
    description->setObjectName(QStringLiteral("exampleDescription"));
    description->setTextFormat(Qt::PlainText);
    description->setWordWrap(true);
    detailsLayout->addWidget(description);
    detailsLayout->addWidget(
        new QLabel(QStringLiteral("Example message"), details));
    auto *message = new QLineEdit(details);
    message->setObjectName(QStringLiteral("exampleMessage"));
    message->setReadOnly(true);
    detailsLayout->addWidget(message);

    auto *outputLabel = new QLabel(details);
    detailsLayout->addWidget(outputLabel);
    auto *output = new QPlainTextEdit(details);
    output->setObjectName(QStringLiteral("examplePreview"));
    output->setReadOnly(true);
    detailsLayout->addWidget(output, 1);
    body->addWidget(details, 5);
    layout->addLayout(body, 1);

    auto *footer = new QHBoxLayout;
    footer->setSpacing(12);
    auto *hint = new QLabel(
        QStringLiteral("Adds a disabled rule. Edit it, test it, then enable "
                       "and save it."),
        picker);
    hint->setWordWrap(true);
    footer->addWidget(hint, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, picker);
    auto *add = buttons->addButton(QStringLiteral("Add example"),
                                   QDialogButtonBox::AcceptRole);
    add->setObjectName(QStringLiteral("addExample"));
    footer->addWidget(buttons, 0, Qt::AlignVCenter);
    layout->addLayout(footer);
    QObject::connect(buttons, &QDialogButtonBox::rejected, picker,
                     &QDialog::reject);
    QObject::connect(add, &QPushButton::clicked, picker, [this, picker, list] {
        this->addRule(list->currentRow() + 1);
        picker->accept();
    });

    QObject::connect(
        list, &QListWidget::currentRowChanged, picker,
        [this, description, message, output, outputLabel](int row) {
            if (row < 0 || row >= static_cast<int>(automationExamples().size()))
            {
                return;
            }
            const auto &example = automationExamples().at(row);
            description->setText(example.description);
            message->setText(example.message);
            message->setCursorPosition(0);
            const auto &rule = example.rule;
            const bool command =
                rule.action == ChatAutomationAction::RunCommand;
            outputLabel->setText(
                command
                    ? (rule.randomResponse
                           ? QStringLiteral("Commands, one per use")
                           : QStringLiteral("Command to run"))
                    : (rule.randomResponse ? QStringLiteral("Possible replies")
                                           : QStringLiteral("Reply preview")));
            QStringList previews;
            const auto choices =
                ChatAutomationController::responseChoices(rule);
            for (int i = 0; i < choices.size(); ++i)
            {
                previews.push_back(
                    this->controller_
                        ->preview(rule, example.message,
                                  rule.onlySelf ? QStringLiteral("Moderator42")
                                                : QStringLiteral("Viewer42"),
                                  this->initialChannel_.isEmpty()
                                      ? QStringLiteral("channel")
                                      : this->initialChannel_,
                                  i)
                        .response);
            }
            output->setPlainText(previews.join(QStringLiteral("\n\n")));
        });
    list->setCurrentRow(0);
    installMoltorinoDialogTheme(picker);
    picker->open();
    list->setFocus();
}

void ChatAutomationDialog::addRule(int example)
{
    this->storeCurrentRule();
    ChatAutomation rule;
    const bool isExample =
        example > 0 && example <= static_cast<int>(automationExamples().size());
    if (isExample)
    {
        rule = automationExamples().at(example - 1).rule;
    }
    rule.enabled = false;
    if (!this->initialChannel_.isEmpty())
    {
        rule.channels.push_back(this->initialChannel_);
    }
    else
    {
        rule.allOpenTwitchChannels = true;
    }
    rule.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    this->rules_.push_back(std::move(rule));
    this->refreshRuleList();
    this->ruleList_->setCurrentRow(static_cast<int>(this->rules_.size() - 1));
    this->trigger_->selectAll();
    this->trigger_->setFocus();
    this->tabs_->setCurrentIndex(0);
    if (isExample)
    {
        const auto &preset = automationExamples().at(example - 1);
        const auto &sample = preset.rule;
        this->testUser_->setText(sample.onlySelf ? QStringLiteral("Moderator42")
                                                 : QStringLiteral("Viewer42"));
        this->testSelf_->setChecked(sample.onlySelf);
        this->testRole_->setCurrentIndex(
            sample.access == ChatAutomationAccess::Moderators || sample.onlySelf
                ? 3
                : 0);
        this->testMyRole_->setCurrentIndex(
            sample.channelRole == ChatAutomationChannelRole::Broadcaster ? 3
            : sample.channelRole == ChatAutomationChannelRole::Moderator ? 2
                                                                         : 0);
        this->testStream_->setCurrentIndex(
            sample.stream == ChatAutomationStream::Offline ? 1 : 0);
        this->testChannel_->setText(this->initialChannel_.isEmpty()
                                        ? QStringLiteral("channel")
                                        : this->initialChannel_);
        this->testChoice_->setValue(1);
        this->testCounter_->setValue(1);
        this->testMessage_->setText(preset.message);
        this->refreshPreview();
    }
}

void ChatAutomationDialog::duplicateRule()
{
    if (this->currentRule_ < 0)
    {
        return;
    }
    this->storeCurrentRule();
    auto rule = this->rules_.at(this->currentRule_);
    rule.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    rule.name = ruleTitle(rule) + QStringLiteral(" copy");
    rule.enabled = false;
    const auto index = this->currentRule_ + 1;
    this->rules_.insert(this->rules_.begin() + index, std::move(rule));
    this->refreshRuleList();
    this->ruleList_->setCurrentRow(index);
    this->trigger_->selectAll();
}

void ChatAutomationDialog::removeRule()
{
    if (this->currentRule_ < 0)
    {
        return;
    }
    const auto removed = this->currentRule_;
    this->storeCurrentRule();
    this->removedRule_ = this->rules_[removed];
    this->removedRuleIndex_ = removed;
    this->undoRemove_->show();
    this->currentRule_ = -1;
    this->rules_.erase(this->rules_.begin() + removed);
    this->refreshRuleList();
    this->ruleList_->setCurrentRow(
        this->rules_.empty()
            ? -1
            : std::min(removed, static_cast<int>(this->rules_.size() - 1)));
    this->refreshEditorState();
    this->refreshPreview();
}

void ChatAutomationDialog::undoRemoveRule()
{
    if (!this->removedRule_)
    {
        return;
    }
    this->storeCurrentRule();
    const auto index = std::min(this->removedRuleIndex_,
                                static_cast<int>(this->rules_.size()));
    this->rules_.insert(this->rules_.begin() + index,
                        std::move(*this->removedRule_));
    this->removedRule_.reset();
    this->undoRemove_->hide();
    this->currentRule_ = -1;
    this->refreshRuleList();
    this->ruleList_->setCurrentRow(index);
}

void ChatAutomationDialog::syncRuleOrderFromList()
{
    if (this->ruleList_->count() != static_cast<int>(this->rules_.size()))
    {
        this->refreshRuleList();
        return;
    }

    const auto selectedID =
        this->ruleList_->currentItem()
            ? this->ruleList_->currentItem()->data(RULE_ID_ROLE).toString()
            : QString{};
    std::unordered_map<QString, const ChatAutomation *> byID;
    byID.reserve(this->rules_.size());
    for (const auto &rule : this->rules_)
    {
        byID.emplace(rule.id, &rule);
    }

    std::vector<ChatAutomation> reordered;
    reordered.reserve(this->rules_.size());
    for (int row = 0; row < this->ruleList_->count(); ++row)
    {
        const auto id =
            this->ruleList_->item(row)->data(RULE_ID_ROLE).toString();
        auto found = byID.find(id);
        if (found == byID.end())
        {
            this->refreshRuleList();
            return;
        }
        reordered.push_back(*found->second);
        byID.erase(found);
    }
    this->rules_ = std::move(reordered);
    this->currentRule_ = -1;
    for (int index = 0; index < static_cast<int>(this->rules_.size()); ++index)
    {
        if (this->rules_[index].id == selectedID)
        {
            this->currentRule_ = index;
            break;
        }
    }
    this->refreshEditorState();
    this->refreshPreview();
}

void ChatAutomationDialog::moveRule(int offset)
{
    const auto target = this->currentRule_ + offset;
    if (this->currentRule_ < 0 || target < 0 ||
        target >= static_cast<int>(this->rules_.size()))
    {
        return;
    }

    this->storeCurrentRule();
    std::swap(this->rules_[this->currentRule_], this->rules_[target]);
    this->currentRule_ = target;
    this->refreshRuleList();
    this->refreshPreview();
}

void ChatAutomationDialog::insertVariable(const QString &variable)
{
    if (!this->response_->isEnabled())
    {
        return;
    }
    this->response_->setFocus();
    this->response_->insertPlainText(variable);
}

void ChatAutomationDialog::loadRule(int index)
{
    if (!this->loading_)
    {
        this->storeCurrentRule();
    }
    this->currentRule_ = index;
    this->loading_ = true;
    if (index >= 0 && index < static_cast<int>(this->rules_.size()))
    {
        const auto &rule = this->rules_[index];
        this->ruleEnabled_->setChecked(rule.enabled);
        this->name_->setText(rule.name);
        this->trigger_->setText(rule.trigger);
        this->aliases_->setText(rule.aliases.join(QStringLiteral(", ")));
        this->caseSensitive_->setChecked(rule.caseSensitive);
        this->argumentMode_->setCurrentIndex(static_cast<int>(rule.arguments));
        this->stream_->setCurrentIndex(static_cast<int>(rule.stream));
        this->channelRole_->setCurrentIndex(static_cast<int>(rule.channelRole));
        loadFormat(this->timeFormat_, this->customTimeFormat_,
                   rule.timeFormat.isEmpty()
                       ? (rule.use12HourTime ? QStringLiteral("h:mm AP")
                                             : QStringLiteral("HH:mm"))
                       : rule.timeFormat);
        loadFormat(this->dateFormat_, this->customDateFormat_,
                   rule.dateFormat.isEmpty() ? QStringLiteral("yyyy-MM-dd")
                                             : rule.dateFormat);
        this->users_->setText(rule.users.join(QStringLiteral(", ")));
        this->ignoredUsers_->setText(
            rule.ignoredUsers.join(QStringLiteral(", ")));
        this->requiredText_->setText(rule.requiredText);
        this->excludedText_->setText(rule.excludedText);
        this->minimumLength_->setValue(rule.minimumMessageLength);
        this->maximumLength_->setValue(rule.maximumMessageLength);
        this->response_->setPlainText(rule.response);
        this->responseMode_->setCurrentIndex(
            rule.randomResponse ? static_cast<int>(rule.responseOrder) + 1 : 0);
        this->match_->setCurrentIndex(
            this->match_->findData(static_cast<int>(rule.match)));
        this->action_->setCurrentIndex(
            this->action_->findData(static_cast<int>(rule.action)));
        this->access_->setCurrentIndex(
            this->access_->findData(static_cast<int>(rule.access)));
        setDuration(this->cooldown_, this->cooldownUnit_, rule.cooldownSeconds);
        setDuration(this->userCooldown_, this->userCooldownUnit_,
                    rule.userCooldownSeconds);
        auto zoneIndex = this->timeZone_->findData(rule.timeZone);
        this->timeZone_->setCurrentIndex(zoneIndex < 0 ? 0 : zoneIndex);
        if (zoneIndex < 0 && !rule.timeZone.isEmpty())
        {
            this->timeZone_->setEditText(rule.timeZone);
        }
        this->sender_->setCurrentIndex(rule.onlySelf        ? 2
                                       : rule.respondToSelf ? 0
                                                            : 1);
        this->useBotBadge_->setChecked(rule.useBotBadge);
        this->channelMode_->setCurrentIndex(!rule.allOpenTwitchChannels ? 1
                                            : rule.excludedChannels.isEmpty()
                                                ? 0
                                                : 2);
        if (this->testChannel_->text().trimmed().isEmpty())
        {
            const auto channels = rule.allOpenTwitchChannels
                                      ? this->controller_->openChannelNames()
                                      : rule.channels;
            for (const auto &channel : channels)
            {
                if (!rule.allOpenTwitchChannels ||
                    !rule.excludedChannels.contains(channel,
                                                    Qt::CaseInsensitive))
                {
                    this->testChannel_->setText(channel);
                    break;
                }
            }
        }
        const QSignalBlocker sampleBlocker(this->testMessage_);
        this->testMessage_->setText(
            rule.match == ChatAutomationMatch::RegularExpression ? QString{}
            : rule.match == ChatAutomationMatch::AnyMessage
                ? QStringLiteral("Hello everyone")
            : rule.match == ChatAutomationMatch::Exact ? rule.trigger
            : rule.arguments == ChatAutomationArguments::None
                ? rule.trigger
                : rule.trigger + QStringLiteral(" some words"));
    }
    this->refreshChannels();
    this->loading_ = false;
    this->refreshEditorState();
    this->refreshMatchHint();
    this->refreshPreview();
}

void ChatAutomationDialog::storeCurrentRule()
{
    if (this->loading_ || this->reordering_ || this->currentRule_ < 0 ||
        this->currentRule_ >= static_cast<int>(this->rules_.size()))
    {
        return;
    }
    auto &rule = this->rules_[this->currentRule_];
    rule.enabled = this->ruleEnabled_->isChecked();
    rule.name = this->name_->text().trimmed();
    rule.trigger = this->trigger_->text().trimmed();
    rule.aliases =
        this->aliases_->text().split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (auto &alias : rule.aliases)
    {
        alias = alias.trimmed();
    }
    rule.caseSensitive = this->caseSensitive_->isChecked();
    rule.timeFormat =
        selectedFormat(this->timeFormat_, this->customTimeFormat_);
    rule.dateFormat =
        selectedFormat(this->dateFormat_, this->customDateFormat_);
    rule.use12HourTime =
        rule.timeFormat.contains(QStringLiteral("ap"), Qt::CaseInsensitive);
    rule.arguments = static_cast<ChatAutomationArguments>(
        this->argumentMode_->currentIndex());
    rule.stream =
        static_cast<ChatAutomationStream>(this->stream_->currentIndex());
    rule.channelRole = static_cast<ChatAutomationChannelRole>(
        this->channelRole_->currentIndex());
    rule.users = parseNames(this->users_->text());
    rule.ignoredUsers = parseNames(this->ignoredUsers_->text());
    rule.requiredText = this->requiredText_->text().trimmed();
    rule.excludedText = this->excludedText_->text().trimmed();
    rule.minimumMessageLength = this->minimumLength_->value();
    rule.maximumMessageLength = this->maximumLength_->value();
    rule.response = this->response_->toPlainText().trimmed();
    rule.randomResponse = this->responseMode_->currentIndex() != 0;
    rule.responseOrder = static_cast<ChatAutomationResponseOrder>(
        std::max(0, this->responseMode_->currentIndex() - 1));
    rule.match =
        static_cast<ChatAutomationMatch>(this->match_->currentData().toInt());
    rule.action =
        static_cast<ChatAutomationAction>(this->action_->currentData().toInt());
    rule.access =
        static_cast<ChatAutomationAccess>(this->access_->currentData().toInt());
    rule.cooldownSeconds =
        durationSeconds(this->cooldown_, this->cooldownUnit_);
    rule.userCooldownSeconds =
        durationSeconds(this->userCooldown_, this->userCooldownUnit_);
    rule.timeZone = selectedTimeZone(this->timeZone_);
    rule.respondToSelf = this->sender_->currentIndex() != 1;
    rule.onlySelf = this->sender_->currentIndex() == 2;
    rule.useBotBadge = this->useBotBadge_->isChecked();
    rule.allOpenTwitchChannels = this->channelMode_->currentIndex() != 1;
    if (this->channelMode_->currentIndex() == 0)
    {
        rule.excludedChannels.clear();
    }
    else
    {
        auto &selected =
            rule.allOpenTwitchChannels ? rule.excludedChannels : rule.channels;
        selected.clear();
        for (int index = 0; index < this->channels_->count(); ++index)
        {
            const auto *item = this->channels_->item(index);
            if (item->checkState() == Qt::Checked)
            {
                selected.push_back(item->data(Qt::UserRole).toString());
            }
        }
    }
}

void ChatAutomationDialog::refreshRuleList()
{
    const auto selectedID =
        this->currentRule_ >= 0 &&
                this->currentRule_ < static_cast<int>(this->rules_.size())
            ? this->rules_[this->currentRule_].id
            : QString{};
    const QSignalBlocker blocker(this->ruleList_);
    this->ruleList_->clear();
    int selectedRow = -1;
    for (int index = 0; index < static_cast<int>(this->rules_.size()); ++index)
    {
        const auto &rule = this->rules_[index];
        auto *item = new QListWidgetItem(this->ruleList_);
        updateRuleItem(item, rule);
        item->setFlags(item->flags() | Qt::ItemIsDragEnabled |
                       Qt::ItemIsDropEnabled);
        if (rule.id == selectedID)
        {
            selectedRow = index;
        }
    }
    if (selectedRow >= 0)
    {
        this->ruleList_->setCurrentRow(selectedRow);
    }
    this->ruleList_->viewport()->update();
}

void ChatAutomationDialog::refreshCurrentRuleListItem()
{
    if (this->currentRule_ < 0 ||
        this->currentRule_ >= static_cast<int>(this->rules_.size()) ||
        this->currentRule_ >= this->ruleList_->count())
    {
        return;
    }

    const auto &rule = this->rules_[this->currentRule_];
    auto *item = this->ruleList_->item(this->currentRule_);
    const QSignalBlocker blocker(this->ruleList_);
    updateRuleItem(item, rule);
    this->ruleList_->viewport()->update(this->ruleList_->visualItemRect(item));
}

void ChatAutomationDialog::refreshChannels()
{
    QStringList selected;
    bool allChannels = false;
    if (this->currentRule_ >= 0 &&
        this->currentRule_ < static_cast<int>(this->rules_.size()))
    {
        const auto &rule = this->rules_[this->currentRule_];
        selected = this->channelMode_->currentIndex() == 2
                       ? rule.excludedChannels
                       : rule.channels;
        allChannels = this->channelMode_->currentIndex() == 0;
    }

    const QSignalBlocker blocker(this->channels_);
    this->channels_->clear();
    const auto openChannels = this->controller_->openChannelNames();
    auto open = openChannels;
    for (const auto &stored : selected)
    {
        if (!open.contains(stored, Qt::CaseInsensitive))
        {
            open.push_back(stored);
        }
    }
    open.removeDuplicates();
    open.sort(Qt::CaseInsensitive);
    const auto rowHeight =
        std::max(28, this->channels_->fontMetrics().height() + 10);
    this->channels_->setFixedHeight(
        std::clamp(static_cast<int>(open.size()) * rowHeight + 2, 95, 170));
    for (const auto &name : open)
    {
        const auto key = name.toCaseFolded();
        const bool available = openChannels.contains(name, Qt::CaseInsensitive);
        auto *item = new QListWidgetItem(name, this->channels_);
        item->setData(Qt::UserRole, key);
        item->setData(CHANNEL_AVAILABLE_ROLE, available);
        item->setData(CHANNEL_LOCKED_ROLE, allChannels);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable |
                       Qt::ItemIsUserCheckable);
        item->setCheckState(allChannels || selected.contains(key)
                                ? Qt::Checked
                                : Qt::Unchecked);
        if (!available)
        {
            item->setToolTip(
                QStringLiteral("This rule resumes when the tab opens again."));
        }
        else if (allChannels)
        {
            item->setToolTip(QStringLiteral("All tabs includes this channel."));
        }
    }
    this->filterChannels(this->channelSearch_->text());
}

void ChatAutomationDialog::filterChannels(const QString &query)
{
    const auto needle = query.trimmed();
    for (int index = 0; index < this->channels_->count(); ++index)
    {
        auto *item = this->channels_->item(index);
        item->setHidden(!needle.isEmpty() &&
                        !item->text().contains(needle, Qt::CaseInsensitive));
    }
    this->refreshChannelTools();
}

void ChatAutomationDialog::toggleShownChannels()
{
    if (this->channelMode_->currentIndex() == 0)
    {
        return;
    }

    bool hasShown = false;
    bool allShownChecked = true;
    for (int index = 0; index < this->channels_->count(); ++index)
    {
        const auto *item = this->channels_->item(index);
        if (item->isHidden())
        {
            continue;
        }
        hasShown = true;
        allShownChecked &= item->checkState() == Qt::Checked;
    }
    if (!hasShown)
    {
        return;
    }

    const QSignalBlocker blocker(this->channels_);
    const auto state = allShownChecked ? Qt::Unchecked : Qt::Checked;
    for (int index = 0; index < this->channels_->count(); ++index)
    {
        auto *item = this->channels_->item(index);
        if (!item->isHidden())
        {
            item->setCheckState(state);
        }
    }
    this->storeCurrentRule();
    this->refreshCurrentRuleListItem();
    this->channels_->viewport()->update();
    this->refreshChannelTools();
    this->refreshEditorState();
    this->refreshPreview();
}

void ChatAutomationDialog::addChannel()
{
    if (!this->channelAdd_->isEnabled() || this->currentRule_ < 0)
    {
        return;
    }
    this->storeCurrentRule();
    auto &rule = this->rules_[this->currentRule_];
    auto &selected = this->channelMode_->currentIndex() == 2
                         ? rule.excludedChannels
                         : rule.channels;
    const auto names = parseNames(this->channelSearch_->text());
    if (names.size() != 1)
    {
        return;
    }
    selected.push_back(names.front());
    selected.removeDuplicates();
    this->channelSearch_->clear();
    this->refreshChannels();
    this->refreshCurrentRuleListItem();
    this->refreshEditorState();
    this->refreshPreview();
}

void ChatAutomationDialog::refreshChannelTools()
{
    const bool locked = this->channelMode_->currentIndex() == 0;
    const auto names = parseNames(this->channelSearch_->text());
    static const QRegularExpression login(QStringLiteral("^[a-z0-9_]+$"));
    const bool canAdd = names.size() == 1 && names.front().size() <= 100 &&
                        login.match(names.front()).hasMatch();
    bool alreadySelected = false;
    for (int i = 0; i < this->channels_->count(); ++i)
    {
        const auto *item = this->channels_->item(i);
        alreadySelected |=
            canAdd && item->data(Qt::UserRole).toString() == names.front() &&
            item->checkState() == Qt::Checked;
    }
    this->channelAdd_->setEnabled(!locked && canAdd && !alreadySelected);
    bool hasShown = false;
    bool allShownChecked = true;
    for (int index = 0; index < this->channels_->count(); ++index)
    {
        const auto *item = this->channels_->item(index);
        if (item->isHidden())
        {
            continue;
        }
        hasShown = true;
        allShownChecked &= item->checkState() == Qt::Checked;
    }

    this->channelToggle_->setText(allShownChecked && hasShown
                                      ? QStringLiteral("Clear shown")
                                      : QStringLiteral("Select shown"));
    this->channelToggle_->setEnabled(!locked && hasShown);
    this->channelToggle_->setToolTip(
        allShownChecked && hasShown
            ? QStringLiteral("Clear the channels shown by this search.")
            : QStringLiteral("Select the channels shown by this search."));
}

void ChatAutomationDialog::refreshEditorState()
{
    this->runStatus_->setText(
        QStringLiteral("The limit applies in each channel, with at least 1.5 "
                       "seconds between actions. Save automations applies "
                       "these settings too."));
    const bool hasRule =
        this->currentRule_ >= 0 &&
        this->currentRule_ < static_cast<int>(this->rules_.size());
    this->editor_->setEnabled(hasRule);
    this->tabs_->setVisible(hasRule);
    this->empty_->setVisible(!hasRule);
    this->duplicate_->setEnabled(hasRule);
    this->remove_->setEnabled(hasRule);
    if (!hasRule)
    {
        this->validation_->hide();
    }
    const auto action =
        static_cast<ChatAutomationAction>(this->action_->currentData().toInt());
    const bool runsCommand = action == ChatAutomationAction::RunCommand;
    this->useBotBadge_->setVisible(action == ChatAutomationAction::SendMessage);
    const bool botConfigured = ChatAutomationController::botBadgeConfigured();
    const bool needsBotSetup = action == ChatAutomationAction::SendMessage &&
                               this->useBotBadge_->isChecked() &&
                               !botConfigured;
    this->botSetup_->setVisible(needsBotSetup);
    this->useBotBadge_->setToolTip(
        botConfigured
            ? QStringLiteral(
                  "Send through the bot badge account in Moltorino settings.")
            : QStringLiteral("This rule needs a bot badge account configured "
                             "before it can run."));
    this->response_->setPlaceholderText(
        runsCommand ? QStringLiteral("/announcepurple {args}")
                    : QStringLiteral("Hey {user.display_name}!"));
    this->response_->setMaximumHeight(
        this->responseMode_->currentIndex() != 0 ? 145 : 96);
    const auto lineHint = runsCommand
                              ? QStringLiteral("One command per line. ")
                              : QStringLiteral("One response per line. ");
    this->responseHint_->setText(
        this->responseMode_->currentIndex() == 3
            ? lineHint +
                  QStringLiteral("Uses the next line each time, per channel.")
        : this->responseMode_->currentIndex() == 2
            ? lineHint + QStringLiteral("Picks at random, avoiding the last "
                                        "choice in that channel.")
        : this->responseMode_->currentIndex() == 1
            ? lineHint + QStringLiteral("Picks one at random each time.")
        : runsCommand
            ? QStringLiteral("Runs one command using your selected account.")
        : action == ChatAutomationAction::ReplyToMessage
            ? QStringLiteral(
                  "Replies directly to the message that triggered this rule.")
            : QStringLiteral(
                  "Sends one chat message. Line breaks become spaces."));
    this->previewLabel_->setText(runsCommand ? QStringLiteral("Command to run")
                                 : action ==
                                         ChatAutomationAction::ReplyToMessage
                                     ? QStringLiteral("Reply preview")
                                     : QStringLiteral("Message preview"));
    if (needsBotSetup)
    {
        this->responseHint_->setText(
            QStringLiteral("Set up a bot badge account in Moltorino settings "
                           "before this rule can run."));
    }
    const auto mode =
        static_cast<ChatAutomationMatch>(this->match_->currentData().toInt());
    this->aliases_->setVisible(mode == ChatAutomationMatch::Command);
    this->aliasesLabel_->setVisible(mode == ChatAutomationMatch::Command);
    this->trigger_->setVisible(mode != ChatAutomationMatch::AnyMessage);
    this->triggerLabel_->setVisible(mode != ChatAutomationMatch::AnyMessage);
    this->triggerLabel_->setText(
        mode == ChatAutomationMatch::Command ? QStringLiteral("Chat command")
        : mode == ChatAutomationMatch::RegularExpression
            ? QStringLiteral("Pattern")
            : QStringLiteral("Message text"));
    const bool hasArguments = mode == ChatAutomationMatch::Command ||
                              mode == ChatAutomationMatch::StartsWith;
    this->argumentMode_->setVisible(hasArguments);
    this->argumentLabel_->setVisible(hasArguments);
    const bool selectedUsers =
        this->access_->currentData().toInt() ==
        static_cast<int>(ChatAutomationAccess::NamedUsers);
    this->users_->setVisible(selectedUsers);
    this->usersLabel_->setVisible(selectedUsers);
    this->caseSensitive_->setEnabled(
        mode != ChatAutomationMatch::AnyMessage ||
        !this->requiredText_->text().trimmed().isEmpty() ||
        !this->excludedText_->text().trimmed().isEmpty());
    this->trigger_->setPlaceholderText(
        mode == ChatAutomationMatch::RegularExpression
            ? QStringLiteral("^!hello\\s+(.+)$")
            : QStringLiteral("!hello"));
    this->channelPicker_->setVisible(this->channelMode_->currentIndex() != 0);
    this->channelScope_->setText(
        this->channelMode_->currentIndex() == 0
            ? QStringLiteral("Includes Twitch channels opened later.")
        : this->channelMode_->currentIndex() == 2
            ? QStringLiteral("Checked channels are excluded.")
            : QStringLiteral(
                  "Checked channels can run this rule while open. You can add "
                  "channels whose tabs are closed."));
    if (this->channelRole_->currentIndex() != 0)
    {
        this->channelScope_->setText(
            this->channelScope_->text() +
            QStringLiteral(" My role uses the account selected in Moltorino."));
    }
    QStringList cooldowns;
    const int channelSeconds =
        durationSeconds(this->cooldown_, this->cooldownUnit_);
    const int userSeconds =
        durationSeconds(this->userCooldown_, this->userCooldownUnit_);
    if (channelSeconds > 0)
    {
        cooldowns.push_back(
            QStringLiteral("Everyone waits %1 between uses in a channel.")
                .arg(durationText(channelSeconds)));
    }
    if (userSeconds > 0)
    {
        cooldowns.push_back(
            QStringLiteral("Each person waits %1 before using it again "
                           "in that channel.")
                .arg(durationText(userSeconds)));
    }
    cooldowns.push_back(
        QStringLiteral("All rules share a limit of %1 actions per 30 seconds "
                       "in each channel, at least 1.5 seconds apart.")
            .arg(this->maxRuns_->value()));
    this->cooldownHint_->setText(cooldowns.join(QLatin1Char(' ')));
    this->refreshVariableOptions();
    this->refreshChannelTools();
    if (hasRule)
    {
        const auto &rule = this->rules_[this->currentRule_];
        QStringList conditions{accessText(rule.access), scopeText(rule)};
        if (rule.onlySelf)
        {
            conditions.prepend(QStringLiteral("Only my messages"));
        }
        else if (!rule.respondToSelf)
        {
            conditions.prepend(QStringLiteral("Other people's messages"));
        }
        if (rule.channelRole != ChatAutomationChannelRole::Any)
        {
            conditions.push_back(channelRoleText(rule.channelRole));
        }
        if (!rule.requiredText.isEmpty() || !rule.excludedText.isEmpty() ||
            rule.minimumMessageLength > 0 || rule.maximumMessageLength > 0)
        {
            conditions.push_back(QStringLiteral("Message filters"));
        }
        if (rule.stream != ChatAutomationStream::Always)
        {
            conditions.push_back(rule.stream == ChatAutomationStream::Live
                                     ? QStringLiteral("Live only")
                                     : QStringLiteral("Offline only"));
        }
        if (rule.cooldownSeconds > 0)
        {
            conditions.push_back(QStringLiteral("%1 per channel")
                                     .arg(durationText(rule.cooldownSeconds)));
        }
        if (rule.userCooldownSeconds > 0)
        {
            conditions.push_back(
                QStringLiteral("%1 per person")
                    .arg(durationText(rule.userCooldownSeconds)));
        }
        this->conditionsSummary_->setText(
            conditions.join(QStringLiteral(" · ")));
        const auto error = ChatAutomationController::validateRule(rule);
        this->validation_->setText(error);
        this->validation_->setVisible(!error.isEmpty());
    }
}

void ChatAutomationDialog::refreshMatchHint()
{
    const auto mode =
        static_cast<ChatAutomationMatch>(this->match_->currentData().toInt());
    switch (mode)
    {
        case ChatAutomationMatch::StartsWith:
            this->matchHint_->setText(
                QStringLiteral("Matches a phrase at the start, followed by a "
                               "space or the end of the message."));
            break;
        case ChatAutomationMatch::ContainsWords:
            this->matchHint_->setText(
                QStringLiteral("Matches whole words anywhere in the message."));
            break;
        case ChatAutomationMatch::ContainsText:
            this->matchHint_->setText(QStringLiteral(
                "Matches text anywhere, including inside a word or link."));
            break;
        case ChatAutomationMatch::Exact:
            this->matchHint_->setText(
                QStringLiteral("Matches the whole message. Spaces at either "
                               "end are ignored."));
            break;
        case ChatAutomationMatch::RegularExpression:
            this->matchHint_->setText(QStringLiteral(
                "A pattern can capture parts of the message. Use {1}, {2} "
                "for captured groups, or {match.name} for a named group. "
                "Try the pattern in Test rule."));
            break;
        case ChatAutomationMatch::AnyMessage:
            this->matchHint_->setText(
                QStringLiteral("Matches every chat message from the people "
                               "allowed in Conditions. Cooldowns still "
                               "apply."));
            break;
        default:
            this->matchHint_->setText(QStringLiteral(
                "Matches a chat command, such as !hello. Use {args} in the "
                "response for any text written after it."));
            break;
    }
}

void ChatAutomationDialog::refreshVariableOptions()
{
    static const QRegularExpression timeToken(QStringLiteral(
        R"((?<!\{)(?:\{\{)*\{(time(?:\.zone)?|date(?:\.weekday)?|datetime(?:\.iso)?|timestamp)(?:;[^}]*)?\})"));
    QStringList timeTokens;
    auto timeMatches =
        timeToken.globalMatch(this->response_->toPlainText().left(16384));
    while (timeMatches.hasNext())
    {
        timeTokens.push_back(timeMatches.next().captured(1));
    }
    timeTokens.removeDuplicates();
    const bool usesTimeZone =
        std::ranges::any_of(timeTokens, [](const auto &token) {
            return token != QStringLiteral("timestamp");
        });
    this->timeZoneLabel_->setVisible(usesTimeZone);
    this->timeZone_->setVisible(usesTimeZone);
    const bool usesClock = timeTokens.contains(QStringLiteral("time")) ||
                           timeTokens.contains(QStringLiteral("datetime"));
    const bool usesDate = timeTokens.contains(QStringLiteral("date")) ||
                          timeTokens.contains(QStringLiteral("datetime"));
    this->timeFormat_->setVisible(usesClock);
    this->timeFormatLabel_->setVisible(usesClock);
    this->customTimeRow_->setVisible(usesClock &&
                                     this->timeFormat_->currentData().isNull());
    this->dateFormat_->setVisible(usesDate);
    this->dateFormatLabel_->setVisible(usesDate);
    this->customDateRow_->setVisible(usesDate &&
                                     this->dateFormat_->currentData().isNull());
    this->formatPreview_->setVisible(!timeTokens.isEmpty());
    if (!timeTokens.isEmpty())
    {
        ChatAutomation formatting;
        formatting.timeZone = selectedTimeZone(this->timeZone_);
        formatting.timeFormat =
            selectedFormat(this->timeFormat_, this->customTimeFormat_);
        formatting.dateFormat =
            selectedFormat(this->dateFormat_, this->customDateFormat_);
        formatting.use12HourTime = this->timeFormat_->currentIndex() == 1;
        const auto values = ChatAutomationController::timeVariables(
            formatting, QDateTime::currentDateTimeUtc());
        QStringList examples;
        for (const auto &token : timeTokens)
        {
            examples.push_back(values.at(token));
        }
        this->formatPreview_->setText(
            QStringLiteral("Preview: %1")
                .arg(examples.join(QStringLiteral(" · "))));
    }
    QStringList tokens;
    for (const auto &variable : AUTOMATION_VARIABLES)
    {
        tokens.push_back(QString::fromUtf8(variable.token));
    }
    if (this->match_->currentData().toInt() ==
            static_cast<int>(ChatAutomationMatch::RegularExpression) &&
        this->trigger_->text().size() <= 512)
    {
        const QRegularExpression pattern(this->trigger_->text());
        if (pattern.isValid())
        {
            const auto names = pattern.namedCaptureGroups();
            for (int i = 1; i <= pattern.captureCount(); ++i)
            {
                tokens.push_back(QStringLiteral("{%1}").arg(i));
                if (!names.value(i).isEmpty())
                {
                    tokens.push_back(
                        QStringLiteral("{match.%1}").arg(names.at(i)));
                }
            }
        }
    }
    tokens.removeDuplicates();
    auto *completer = this->response_->findChild<QCompleter *>();
    auto *model = qobject_cast<QStringListModel *>(completer->model());
    if (model && model->stringList() != tokens)
    {
        model->setStringList(tokens);
    }
}

void ChatAutomationDialog::refreshPreview()
{
    if (this->loading_ || this->currentRule_ < 0 ||
        this->currentRule_ >= static_cast<int>(this->rules_.size()))
    {
        return;
    }
    const auto &rule = this->rules_[this->currentRule_];
    const QSignalBlocker choiceBlocker(this->testChoice_);
    const auto choices = ChatAutomationController::responseChoices(rule);
    this->testChoice_->setMaximum(
        rule.randomResponse ? std::max(1, static_cast<int>(choices.size()))
                            : 1);
    this->testChoice_->setEnabled(rule.randomResponse && choices.size() > 1);
    this->testChoice_->setVisible(rule.randomResponse);
    this->testChoiceLabel_->setVisible(rule.randomResponse);
    static const QRegularExpression countToken(
        QStringLiteral(R"((^|[^{])(?:\{\{)*\{count(?:;[^}]*)?\})"));
    const bool usesCounter = countToken.match(rule.response).hasMatch();
    this->testCounter_->setVisible(usesCounter);
    this->testCounterLabel_->setVisible(usesCounter);
    const auto result = this->controller_->preview(
        rule, this->testMessage_->text(), this->testUser_->text(),
        this->testChannel_->text(), this->testChoice_->value() - 1,
        this->testCounter_->value());
    QString status;
    if (!result.match.error.isEmpty())
    {
        status = result.match.error;
    }
    else if (this->testMessage_->text().trimmed().isEmpty())
    {
        status = QStringLiteral("Enter a message to test this rule.");
    }
    else if (!result.match.matched)
    {
        status = QStringLiteral("The message does not match this trigger.");
    }
    else
    {
        Message sample;
        sample.messageText = this->testMessage_->text();
        sample.loginName = this->testUser_->text().trimmed().toCaseFolded();
        if (sample.loginName.startsWith(QLatin1Char('@')))
        {
            sample.loginName.remove(0, 1);
        }
        const QStringList badges{{},
                                 QStringLiteral("subscriber"),
                                 QStringLiteral("vip"),
                                 QStringLiteral("moderator"),
                                 QStringLiteral("broadcaster")};
        if (this->testRole_->currentIndex() > 0)
        {
            sample.twitchBadges.emplace_back(
                badges.at(this->testRole_->currentIndex()),
                QStringLiteral("1"));
        }
        const ChatAutomationChannelRoles myRoles{
            .moderator = this->testMyRole_->currentIndex() == 2,
            .broadcaster = this->testMyRole_->currentIndex() == 3,
            .vip = this->testMyRole_->currentIndex() == 1,
        };
        const auto failure = ChatAutomationController::conditionFailure(
            rule, sample, this->testChannel_->text(),
            this->testSelf_->isChecked(),
            this->testStream_->currentIndex() == 0, myRoles);
        if (!failure.isEmpty())
        {
            status = QStringLiteral(
                         "Trigger matches, but this rule would not run. %1")
                         .arg(failure);
        }
        else if (rule.action == ChatAutomationAction::RunCommand)
        {
            status = QStringLiteral("Trigger and conditions match. This rule "
                                    "would run the command below.");
        }
        else
        {
            status =
                QStringLiteral(
                    "Trigger and conditions match. Response: %1 characters.")
                    .arg(result.response.size());
        }
        if (!rule.enabled)
        {
            status += QStringLiteral(" Enable this rule to use it in chat.");
        }
        if (!this->masterEnabled_->isChecked())
        {
            status +=
                QStringLiteral(" Automations are switched off in this draft.");
        }
        if (failure.isEmpty())
        {
            for (int i = 0; i < this->currentRule_; ++i)
            {
                const auto &earlier = this->rules_[i];
                if (earlier.enabled && !earlier.response.trimmed().isEmpty() &&
                    ChatAutomationController::conditionFailure(
                        earlier, sample, this->testChannel_->text(),
                        this->testSelf_->isChecked(),
                        this->testStream_->currentIndex() == 0, myRoles)
                        .isEmpty() &&
                    ChatAutomationController::matchMessage(
                        earlier, this->testMessage_->text())
                        .matched)
                {
                    status += QStringLiteral(
                                  " Earlier rule \"%1\" takes priority, "
                                  "including during its cooldown. Move this "
                                  "rule above it to use this response.")
                                  .arg(ruleTitle(earlier));
                    break;
                }
            }
        }
        if (result.response.size() > 500 &&
            rule.action != ChatAutomationAction::RunCommand)
        {
            status += QStringLiteral(
                " Shorten the response to fit in one chat message.");
        }
        if (result.response.isEmpty())
        {
            status += QStringLiteral(
                " The response is empty with these sample values.");
        }
        if (rule.useBotBadge &&
            rule.action == ChatAutomationAction::SendMessage &&
            !ChatAutomationController::botBadgeConfigured())
        {
            status += QStringLiteral(
                " Configure the bot badge account before enabling this rule.");
        }
    }
    this->testStatus_->setTextFormat(Qt::PlainText);
    this->testStatus_->setText(status);
    QStringList arguments;
    if (result.match.matched)
    {
        arguments.push_back(
            QStringLiteral("{trigger} = %1").arg(result.match.trigger));
        arguments.push_back(
            QStringLiteral("{args} = %1").arg(result.match.arguments));
        const auto numbered =
            rule.match == ChatAutomationMatch::RegularExpression
                ? result.match.captures
                : result.match.arguments.simplified().split(QLatin1Char(' '),
                                                            Qt::SkipEmptyParts);
        const auto count = std::min<qsizetype>(numbered.size(), 10);
        for (int i = 0; i < count; ++i)
        {
            arguments.push_back(
                QStringLiteral("{%1} = %2").arg(i + 1).arg(numbered.at(i)));
        }
        QStringList names;
        for (const auto &[name, value] : result.match.namedCaptures)
        {
            names.push_back(name);
        }
        names.sort();
        for (const auto &name : names)
        {
            arguments.push_back(
                QStringLiteral("{match.%1} = %2")
                    .arg(name, result.match.namedCaptures.at(name)));
        }
    }
    this->testArguments_->setPlainText(arguments.join(QLatin1Char('\n')));
    this->testArguments_->setFixedHeight(
        std::clamp(this->testArguments_->document()->blockCount() *
                           this->testArguments_->fontMetrics().height() +
                       12,
                   36, 96));
    this->testArguments_->setVisible(!arguments.isEmpty());
    this->preview_->setPlainText(result.response);
}

void ChatAutomationDialog::refreshStyle()
{
    const auto *theme = getTheme();
    this->setFont(getApp()->getFonts()->getFont(FontStyle::UiMedium, 1.F));
    this->setStyleSheet(
        moltorinoDialogStyleSheet() +
        QStringLiteral("QLabel#SecondaryText, QLabel#runStatus { color: %1; }")
            .arg(theme->tabs.regular.text.name()) +
        QStringLiteral("QTabWidget#automationTabs::pane { border: 0; "
                       "border-top: 1px solid %1; }")
            .arg(theme->tabs.dividerLine.name()));

    auto previewPalette = this->preview_->palette();
    previewPalette.setColor(QPalette::Window,
                            previewPalette.color(QPalette::Base));
    this->preview_->setPalette(previewPalette);
    this->preview_->setAutoFillBackground(true);

    if (auto *delegate = dynamic_cast<AutomationRuleDelegate *>(
            this->ruleList_->itemDelegate()))
    {
        delegate->setColors(theme->tabs.regular.backgrounds.regular,
                            theme->tabs.regular.backgrounds.hover,
                            theme->tabs.selected.backgrounds.regular,
                            theme->window.text, theme->tabs.selected.text,
                            theme->tabs.regular.text, theme->tabs.dividerLine);
        delegate->setFonts(
            getApp()->getFonts()->getFont(FontStyle::UiMedium, 1.F),
            getApp()->getFonts()->getFont(FontStyle::UiMediumBold, 1.F));
        this->ruleList_->viewport()->update();
    }
    if (auto *delegate = dynamic_cast<AutomationChannelDelegate *>(
            this->channels_->itemDelegate()))
    {
        delegate->setColors(theme->tabs.regular.backgrounds.regular,
                            theme->tabs.regular.backgrounds.hover,
                            theme->tabs.selected.backgrounds.regular,
                            theme->window.text, theme->tabs.regular.text,
                            theme->tabs.dividerLine, theme->accent);
        this->channels_->viewport()->update();
    }
}

bool ChatAutomationDialog::validateRules()
{
    this->storeCurrentRule();
    for (int index = 0; index < static_cast<int>(this->rules_.size()); ++index)
    {
        const auto &rule = this->rules_[index];
        if (!rule.enabled)
        {
            continue;
        }
        const auto error = ChatAutomationController::validateRule(rule);
        if (!error.isEmpty())
        {
            this->ruleList_->setCurrentRow(index);
            this->tabs_->setCurrentIndex(0);
            this->validation_->setText(error);
            this->validation_->show();
            if ((!rule.allOpenTwitchChannels && rule.channels.isEmpty()) ||
                (rule.access == ChatAutomationAccess::NamedUsers &&
                 rule.users.isEmpty()) ||
                (rule.maximumMessageLength > 0 &&
                 rule.minimumMessageLength > rule.maximumMessageLength))
            {
                this->tabs_->setCurrentIndex(1);
            }
            return false;
        }
    }
    return true;
}

void ChatAutomationDialog::save()
{
    this->refreshGlobalSettings();
    if (!this->validateRules())
    {
        return;
    }
    const auto previous = this->controller_->rules();
    const auto previousEnabled = this->controller_->enabled();
    this->applying_ = true;
    this->controller_->setRules(this->rules_);
    this->controller_->setEnabled(this->masterEnabled_->isChecked());
    if (!this->controller_->save())
    {
        this->controller_->setRules(previous);
        this->controller_->setEnabled(previousEnabled);
        this->applying_ = false;
        QMessageBox::warning(
            this, QStringLiteral("Could not save automations"),
            QStringLiteral("Your changes are still in this window. Check that "
                           "the settings folder is writable, then try again."));
        return;
    }
    this->applying_ = false;
    getSettings()->chatAutomationsRunInBackground =
        this->runInBackground_->isChecked();
    getSettings()->chatAutomationsMaxRunsPer30Seconds = this->maxRuns_->value();
    this->accept();
}

void ChatAutomationDialog::refreshGlobalSettings()
{
    const auto background =
        getSettings()->chatAutomationsRunInBackground.getValue();
    const auto maxRuns = std::clamp(
        getSettings()->chatAutomationsMaxRunsPer30Seconds.getValue(), 1, 10);
    if (this->runInBackground_->isChecked() == this->initialBackground_)
    {
        const QSignalBlocker blocker(this->runInBackground_);
        this->runInBackground_->setChecked(background);
    }
    if (this->maxRuns_->value() == this->initialMaxRuns_)
    {
        const QSignalBlocker blocker(this->maxRuns_);
        this->maxRuns_->setValue(maxRuns);
    }
    this->initialBackground_ = background;
    this->initialMaxRuns_ = maxRuns;
}

bool ChatAutomationDialog::event(QEvent *event)
{
    if (event->type() == QEvent::WindowActivate && this->save_)
    {
        this->refreshGlobalSettings();
        this->refreshEditorState();
        this->refreshPreview();
    }
    return QDialog::event(event);
}

void ChatAutomationDialog::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
    {
        if (auto *button = qobject_cast<QPushButton *>(this->focusWidget()))
        {
            button->click();
        }
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

}
