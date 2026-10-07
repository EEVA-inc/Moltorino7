// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/SelectChannelDialog.hpp"

#include "Application.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/tiktok/TikTokChatServer.hpp"
#include "providers/tiktok/TikTokTypes.hpp"
#include "providers/youtube/YouTubeChatServer.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"
#include "util/MultiChannel.hpp"
#include "widgets/BasePopup.hpp"
#include "widgets/helper/MicroNotebook.hpp"

#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

#include <vector>

namespace {

using namespace chatterino;

constexpr auto ActiveDestinationRole = Qt::UserRole + 1;

class AddToMultiChannel : public BasePopup
{
    Q_OBJECT

public:
    AddToMultiChannel(QWidget *parent = nullptr)
        : BasePopup(
              {
                  BaseWindow::EnableCustomFrame,
                  BaseWindow::DisableLayoutSave,
                  BaseWindow::BoundsCheckOnShow,
              },
              parent)
        , platform(new QComboBox)
        , name(new QLineEdit)
    {
        this->setAttribute(Qt::WA_DeleteOnClose);
        this->setWindowTitle("Add Channel");

        auto *layout = new QVBoxLayout(this->getLayoutContainer());

        this->platform->addItem(
            "Twitch", QVariant::fromValue(MultiChannel::Platform::Twitch));
        this->platform->addItem(
            "Kick", QVariant::fromValue(MultiChannel::Platform::Kick));
        this->platform->addItem(
            "YouTube", QVariant::fromValue(MultiChannel::Platform::YouTube));
        this->platform->addItem(
            "TikTok", QVariant::fromValue(MultiChannel::Platform::TikTok));
        layout->addWidget(this->platform);

        this->name->setPlaceholderText("Channel name or source");
        QObject::connect(
            this->platform, &QComboBox::currentIndexChanged, this, [this] {
                const auto selected = this->platform->currentData()
                                          .value<MultiChannel::Platform>();
                this->name->setPlaceholderText(
                    selected == MultiChannel::Platform::YouTube
                        ? "@handle, channel ID/URL, video URL, or video:<ID>"
                        : "Channel name");
            });
        layout->addWidget(this->name);
        this->error = new QLabel;
        this->error->setWordWrap(true);
        this->error->hide();
        layout->addWidget(this->error);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                             QDialogButtonBox::Cancel);
        QObject::connect(buttons, &QDialogButtonBox::accepted, this,
                         &AddToMultiChannel::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                         &AddToMultiChannel::close);
        layout->addStretch();
        layout->addWidget(buttons);

        this->addShortcuts();
        this->name->setFocus();
    }

    void addShortcuts() override
    {
        HotkeyController::HotkeyMap actions{
            {"accept",
             [this](const std::vector<QString> &) -> QString {
                 this->accept();
                 return {};
             }},
            {"reject",
             [this](const std::vector<QString> &) -> QString {
                 this->close();
                 return {};
             }},
        };

        this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
            HotkeyCategory::PopupWindow, actions, this);
    }

Q_SIGNALS:
    // NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
    void specAdded(chatterino::MultiChannel::Spec spec);

private:
    void accept()
    {
        auto nameText = this->name->text().trimmed();
        auto platform =
            this->platform->currentData().value<MultiChannel::Platform>();
        if (platform == MultiChannel::Platform::TikTok)
        {
            const auto handle = normalizeTikTokHandle(nameText);
            if (!handle)
            {
                this->error->setText(
                    "Enter a TikTok handle or a profile or LIVE URL.");
                this->error->show();
                this->name->setFocus();
                return;
            }
            nameText = *handle;
        }
        if (!nameText.isEmpty())
        {
            this->specAdded(MultiChannel::Spec{
                .platform = platform,
                .name = nameText,
            });
        }
        this->close();
    }

    QComboBox *platform = nullptr;
    QLineEdit *name = nullptr;
    QLabel *error = nullptr;
};

QListWidgetItem *makeMultiChannelItem(const MultiChannel::Spec &spec)
{
    QString name;
    switch (spec.platform)
    {
        case MultiChannel::Platform::Twitch:
            name += u"[Twitch] ";
            break;
        case MultiChannel::Platform::Kick:
            name += u"[Kick] ";
            break;
        case MultiChannel::Platform::YouTube:
            name += u"[YouTube] ";
            break;
        case MultiChannel::Platform::TikTok:
            name += u"[TikTok] ";
            break;
    }
    name += spec.name;
    auto *item = new QListWidgetItem(name);
    item->setData(Qt::UserRole, QVariant::fromValue(spec));
    return item;
}

QListWidgetItem *makeMultiChannelItem(const MultiChannel::ChildChannel &chan)
{
    return makeMultiChannelItem(MultiChannel::Spec{
        .platform = chan.platform,
        .name = chan.channel->getName(),
    });
}

}  // namespace

namespace chatterino {

SelectChannelDialog::SelectChannelDialog(QWidget *parent)
    : BaseWindow(
          {
              BaseWindow::Flags::EnableCustomFrame,
              BaseWindow::Flags::Dialog,
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
          },
          parent)
    , selectedChannel_(Channel::getEmpty())
{
    using AutoCheckedRadioButton = detail::AutoCheckedRadioButton;

    this->setWindowTitle("Select a channel to join");

    this->tabFilter_.dialog = this;

    auto &ui = this->ui_;
    auto *rootLayout = new QVBoxLayout(this->getLayoutContainer());
    rootLayout->setContentsMargins({});
    ui.notebook = new MicroNotebook(this->getLayoutContainer());
    rootLayout->addWidget(ui.notebook, 1);

    ui.twitchPage = new QWidget;
    auto *layout = new QVBoxLayout(ui.twitchPage);

    // Channel
    ui.channel = new AutoCheckedRadioButton("Channel");
    layout->addWidget(ui.channel);

    ui.channelLabel = new QLabel("Join a Twitch channel by its channel name");
    ui.channelLabel->setVisible(false);
    layout->addWidget(ui.channelLabel);

    ui.channelName = new QLineEdit();
    ui.channelName->setVisible(false);
    layout->addWidget(ui.channelName);

    ui.channelAnonymous = new QCheckBox("Join anonymously (read only)");
    ui.channelAnonymous->setVisible(false);
    ui.channelAnonymous->setToolTip(
        "Connect with a Twitch anonymous chat session. You can read chat, but "
        "you can't send messages from this split.");
    layout->addWidget(ui.channelAnonymous);

    QObject::connect(ui.channel, &AutoCheckedRadioButton::toggled, this,
                     [this](bool enabled) {
                         auto &ui = this->ui_;
                         ui.channelName->setVisible(enabled);
                         ui.channelLabel->setVisible(enabled);
                         ui.channelAnonymous->setVisible(enabled);

                         if (enabled)
                         {
                             ui.channelName->setFocus();
                             ui.channelName->selectAll();
                         }
                     });

    ui.channel->installEventFilter(&this->tabFilter_);
    ui.channelName->installEventFilter(&this->tabFilter_);

    // Whispers
    ui.whispers = new AutoCheckedRadioButton("Whispers");
    layout->addWidget(ui.whispers);

    ui.whispersLabel = new QLabel(
        "Shows the whispers that you receive while Chatterino is running");
    ui.whispersLabel->setVisible(false);
    ui.whispersLabel->setWordWrap(true);
    layout->addWidget(ui.whispersLabel);

    QObject::connect(ui.whispers, &AutoCheckedRadioButton::toggled, this,
                     [this](bool enabled) {
                         auto &ui = this->ui_;
                         ui.whispersLabel->setVisible(enabled);
                     });

    ui.whispers->installEventFilter(&this->tabFilter_);

    // Mentions
    ui.mentions = new AutoCheckedRadioButton("Mentions");
    layout->addWidget(ui.mentions);

    ui.mentionsLabel = new QLabel(
        "Shows all the messages that highlight you from any channel");
    ui.mentionsLabel->setVisible(false);
    ui.mentionsLabel->setWordWrap(true);
    layout->addWidget(ui.mentionsLabel);

    QObject::connect(ui.mentions, &AutoCheckedRadioButton::toggled, this,
                     [this](bool enabled) {
                         auto &ui = this->ui_;
                         ui.mentionsLabel->setVisible(enabled);
                     });

    ui.mentions->installEventFilter(&this->tabFilter_);

    // Watching
    ui.watching = new AutoCheckedRadioButton("Watching");
    layout->addWidget(ui.watching);

    ui.watchingLabel = new QLabel("Requires the Chatterino browser extension");
    ui.watchingLabel->setVisible(false);
    layout->addWidget(ui.watchingLabel);

    QObject::connect(ui.watching, &AutoCheckedRadioButton::toggled, this,
                     [this](bool enabled) {
                         auto &ui = this->ui_;
                         ui.watchingLabel->setVisible(enabled);
                     });

    ui.watching->installEventFilter(&this->tabFilter_);

    // Live
    ui.live = new AutoCheckedRadioButton("Live");
    layout->addWidget(ui.live);

    ui.liveLabel = new QLabel("Shows when channels go live");
    ui.liveLabel->setVisible(false);
    layout->addWidget(ui.liveLabel);

    QObject::connect(ui.live, &AutoCheckedRadioButton::toggled, this,
                     [this](bool enabled) {
                         auto &ui = this->ui_;
                         ui.liveLabel->setVisible(enabled);
                     });

    ui.live->installEventFilter(&this->tabFilter_);

    // Automod
    ui.automod = new AutoCheckedRadioButton("AutoMod");
    layout->addWidget(ui.automod);

    ui.automodLabel = new QLabel("Channel filter:");
    ui.automodLabel->setVisible(false);
    ui.automodLabel->setWordWrap(true);
    layout->addWidget(ui.automodLabel);

    ui.automodChannel = new QLineEdit();
    ui.automodChannel->setPlaceholderText("All channels");
    ui.automodChannel->setVisible(false);
    layout->addWidget(ui.automodChannel);

    QObject::connect(ui.automod, &AutoCheckedRadioButton::toggled, this,
                     [this](bool enabled) {
                         auto &ui = this->ui_;
                         ui.automodLabel->setVisible(enabled);
                         ui.automodChannel->setVisible(enabled);
                     });

    ui.automod->installEventFilter(&this->tabFilter_);
    ui.automodChannel->installEventFilter(&this->tabFilter_);

    layout->addStretch(1);

    ui.notebook->addPage(ui.twitchPage, "Twitch");

    // Kick
    {
        ui.kickPage = new QWidget;
        auto *layout = new QVBoxLayout(ui.kickPage);

        auto *kickLabel = new QLabel(
            "Join a Kick channel by its name.<br>This is <b>very "
            "experimental</b> and Chatterino7 specific. Only basic features "
            "are supported. Please report bugs <a "
            "href=\"https://github.com/SevenTV/chatterino7/issues\">here</a>.");
        kickLabel->setOpenExternalLinks(true);
        kickLabel->setWordWrap(true);
        layout->addWidget(kickLabel);

        ui.kickName = new QLineEdit();
        ui.kickName->setPlaceholderText("Username");
        layout->addWidget(ui.kickName);

        layout->addStretch(1);

        ui.notebook->addPage(ui.kickPage, "Kick");
    }

    {
        ui.youtubePage = new QWidget;
        auto *youtubeLayout = new QVBoxLayout(ui.youtubePage);

        auto *youtubeLabel = new QLabel(
            "Join a YouTube live chat using a channel handle, channel ID or "
            "URL, video URL, or video:<ID>.");
        youtubeLabel->setWordWrap(true);
        youtubeLayout->addWidget(youtubeLabel);

        ui.youtubeSource = new QLineEdit();
        ui.youtubeSource->setPlaceholderText(
            "@handle, channel ID/URL, video URL, or video:<ID>");
        youtubeLayout->addWidget(ui.youtubeSource);

        youtubeLayout->addStretch(1);

        ui.notebook->addPage(ui.youtubePage, "YouTube");
    }

    {
        ui.tiktokPage = new QWidget;
        auto *tiktokLayout = new QVBoxLayout(ui.tiktokPage);
        auto *description = new QLabel(
            "Read TikTok LIVE chat without an account. Connect a TikTok "
            "account in Settings > Accounts to send messages.");
        description->setWordWrap(true);
        tiktokLayout->addWidget(description);
        auto *label = new QLabel("Channel:");
        ui.tiktokSource = new QLineEdit;
        ui.tiktokSource->setObjectName("tiktokSource");
        ui.tiktokSource->setPlaceholderText("@handle or TikTok LIVE URL");
        label->setBuddy(ui.tiktokSource);
        tiktokLayout->addWidget(label);
        tiktokLayout->addWidget(ui.tiktokSource);
        ui.tiktokError = new QLabel;
        ui.tiktokError->setWordWrap(true);
        ui.tiktokError->hide();
        tiktokLayout->addWidget(ui.tiktokError);
        tiktokLayout->addStretch(1);
        ui.notebook->addPage(ui.tiktokPage, "TikTok");
    }

    // Multi
    {
        ui.multiPage = new QWidget;
        ui.multiView = new QListWidget;
        ui.multiIndicatorMode = new QComboBox;
        auto *layout = new QVBoxLayout(ui.multiPage);
        {
            auto *descriptionLabel = new QLabel(
                "Combine channels in one split. Choose where to send from "
                "the input box.");
            descriptionLabel->setWordWrap(true);
            descriptionLabel->setSizePolicy(QSizePolicy::Preferred,
                                            QSizePolicy::Minimum);
            layout->addWidget(descriptionLabel);

            auto *header = new QWidget;
            auto *add = new QPushButton("Add");
            auto *remove = new QPushButton("Remove");
            auto *headerLayout = new QHBoxLayout(header);
            headerLayout->addWidget(add, 1);
            headerLayout->addWidget(remove, 1);
            layout->addWidget(header);

            QObject::connect(add, &QPushButton::clicked, this, [this] {
                auto *diag = new AddToMultiChannel(this);
                QObject::connect(
                    diag, &AddToMultiChannel::specAdded, this,
                    [this](const MultiChannel::Spec &spec) {
                        const auto caseSensitivity =
                            spec.platform == MultiChannel::Platform::YouTube
                                ? Qt::CaseSensitive
                                : Qt::CaseInsensitive;
                        for (int i = 0; i < this->ui_.multiView->count(); ++i)
                        {
                            auto *item = this->ui_.multiView->item(i);
                            const auto existing =
                                item->data(Qt::UserRole)
                                    .value<MultiChannel::Spec>();
                            if (existing.platform == spec.platform &&
                                existing.name.compare(spec.name,
                                                      caseSensitivity) == 0)
                            {
                                this->ui_.multiView->setCurrentItem(item);
                                return;
                            }
                        }
                        this->ui_.multiView->addItem(
                            makeMultiChannelItem(spec));
                    });
                diag->show();
            });
            QObject::connect(remove, &QPushButton::clicked, this, [this] {
                delete this->ui_.multiView->currentItem();
            });
        }

        ui.multiView->setAutoScroll(true);
        ui.multiView->setSelectionMode(QListWidget::SingleSelection);
        ui.multiView->setSelectionBehavior(QListWidget::SelectRows);
        ui.multiView->setDragDropMode(QListWidget::InternalMove);
        ui.multiView->setFrameStyle(QFrame::NoFrame);
        ui.multiView->setSizeAdjustPolicy(QListView::AdjustToContents);
        ui.multiView->setMinimumHeight(qRound(120 * this->scale()));
        layout->addWidget(ui.multiView, 1);

        layout->addWidget(new QLabel("Channel indicator:"));
        {
            using Mode = MultiChannelIndicatorMode;

            auto v = [](Mode mode) {
                return QVariant::fromValue(mode);
            };
            const auto addIndicator = [&](const QString &label, Mode mode,
                                          const QString &tooltip) {
                ui.multiIndicatorMode->addItem(label, v(mode));
                ui.multiIndicatorMode->setItemData(
                    ui.multiIndicatorMode->count() - 1, tooltip,
                    Qt::ToolTipRole);
            };
            addIndicator("None", Mode::None, "Do not label message sources.");
            addIndicator(
                "Platform badge for other platforms",
                Mode::PlatformBadgeIfUnselected,
                "Mark messages from a different platform than the selected "
                "send channel.");
            addIndicator("Platform badge on every message",
                         Mode::PlatformBadgeAlways,
                         "Show the source platform's badge on every "
                         "message.");
            addIndicator(
                "Channel avatar", Mode::ChannelAvatar,
                "Show the source channel's profile picture. The channel name "
                "is used when no picture is available.");
            addIndicator("Channel name", Mode::ChannelName,
                         "Show the source channel's name on every message.");
            ui.multiIndicatorMode->setCurrentIndex(1);
            const auto updateTooltip = [combo = ui.multiIndicatorMode] {
                combo->setToolTip(
                    combo->currentData(Qt::ToolTipRole).toString());
            };
            QObject::connect(ui.multiIndicatorMode,
                             &QComboBox::currentIndexChanged, this,
                             updateTooltip);
            updateTooltip();
        }
        layout->addWidget(ui.multiIndicatorMode);

        ui.notebook->addPage(ui.multiPage, "Multi");
    }

    auto *buttonBox =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttonBox->setContentsMargins({10, 10, 10, 10});
    rootLayout->addWidget(buttonBox);

    QObject::connect(buttonBox, &QDialogButtonBox::accepted, this, [this] {
        this->ok();
    });
    QObject::connect(buttonBox, &QDialogButtonBox::rejected, [this] {
        this->close();
    });

    this->addShortcuts();

    this->themeChangedEvent();
}

void SelectChannelDialog::ok()
{
    if (this->ui_.notebook->isSelected(this->ui_.tiktokPage) &&
        !normalizeTikTokHandle(this->ui_.tiktokSource->text()))
    {
        this->ui_.tiktokError->setText(
            "Enter a TikTok handle or a profile or LIVE URL.");
        this->ui_.tiktokError->show();
        this->ui_.tiktokSource->setFocus();
        return;
    }
    // accept and close
    this->hasSelectedChannel_ = true;
    this->close();
}

void SelectChannelDialog::setSelectedChannel(
    std::optional<IndirectChannel> channel_)
{
    if (!channel_.has_value())
    {
        this->ui_.channel->setChecked(true);
        this->ui_.channelAnonymous->setChecked(false);

        this->hasSelectedChannel_ = false;
        return;
    }

    const auto &indirectChannel = channel_.value();
    const auto &channel = indirectChannel.get();

    assert(channel);

    this->selectedChannel_ = channel;
    this->ui_.multiView->clear();

    switch (indirectChannel.getType())
    {
        case Channel::Type::Twitch: {
            this->ui_.channelName->setText(channel->getName());
            if (auto *twitchChannel =
                    dynamic_cast<TwitchChannel *>(channel.get()))
            {
                this->ui_.channelAnonymous->setChecked(
                    twitchChannel->isAnonymous());
            }
            else
            {
                this->ui_.channelAnonymous->setChecked(false);
            }
            this->ui_.channel->setChecked(true);
        }
        break;
        case Channel::Type::TwitchWatching: {
            this->ui_.channelAnonymous->setChecked(false);
            this->ui_.watching->setFocus();
        }
        break;
        case Channel::Type::TwitchMentions: {
            this->ui_.channelAnonymous->setChecked(false);
            this->ui_.mentions->setFocus();
        }
        break;
        case Channel::Type::TwitchWhispers: {
            this->ui_.channelAnonymous->setChecked(false);
            this->ui_.whispers->setFocus();
        }
        break;
        case Channel::Type::TwitchLive: {
            this->ui_.channelAnonymous->setChecked(false);
            this->ui_.live->setFocus();
        }
        break;
        case Channel::Type::TwitchAutomod: {
            this->ui_.channelAnonymous->setChecked(false);
            this->ui_.automod->setFocus();
        }
        break;
        case Channel::Type::Kick: {
            this->ui_.channelAnonymous->setChecked(false);
            this->ui_.kickName->setText(channel->getName());
            this->ui_.kickName->selectAll();
            this->ui_.notebook->select(this->ui_.kickPage);
        }
        break;
        case Channel::Type::YouTube: {
            this->ui_.channelAnonymous->setChecked(false);
            this->ui_.youtubeSource->setText(channel->getName());
            this->ui_.youtubeSource->selectAll();
            this->ui_.notebook->select(this->ui_.youtubePage);
        }
        break;
        case Channel::Type::TikTok: {
            this->ui_.channelAnonymous->setChecked(false);
            this->ui_.tiktokSource->setText(channel->getName());
            this->ui_.tiktokSource->selectAll();
            this->ui_.notebook->select(this->ui_.tiktokPage);
        }
        break;
        case Channel::Type::Multi: {
            this->ui_.channelAnonymous->setChecked(false);
            const auto *mc = dynamic_cast<const MultiChannel *>(channel.get());
            if (mc)
            {
                size_t index = 0;
                for (const auto &child : mc->channels())
                {
                    auto *item = makeMultiChannelItem(child);
                    item->setData(ActiveDestinationRole,
                                  index++ == mc->activeChannelIndex());
                    this->ui_.multiView->addItem(item);
                }
                int indicatorIdx = this->ui_.multiIndicatorMode->findData(
                    QVariant::fromValue(mc->indicatorMode()));
                if (indicatorIdx >= 0)
                {
                    this->ui_.multiIndicatorMode->setCurrentIndex(indicatorIdx);
                }
            }
            this->ui_.notebook->select(this->ui_.multiPage);
        }
        break;
        default: {
            this->ui_.channelAnonymous->setChecked(false);
            this->ui_.channel->setChecked(true);
        }
    }

    this->hasSelectedChannel_ = false;
}

IndirectChannel SelectChannelDialog::getSelectedChannel() const
{
    if (!this->hasSelectedChannel_)
    {
        return this->selectedChannel_;
    }

    if (this->ui_.notebook->isSelected(this->ui_.kickPage))
    {
        return getApp()->getKickChatServer()->getOrCreate(
            this->ui_.kickName->text().trimmed());
    }

    if (this->ui_.notebook->isSelected(this->ui_.youtubePage))
    {
        return getApp()->getYouTubeChatServer()->getOrCreate(
            this->ui_.youtubeSource->text().trimmed());
    }

    if (this->ui_.notebook->isSelected(this->ui_.tiktokPage))
    {
        return getApp()->getTikTokChatServer()->getOrCreate(
            this->ui_.tiktokSource->text());
    }

    if (this->ui_.notebook->isSelected(this->ui_.multiPage))
    {
        QVarLengthArray<MultiChannel::Spec, 4> specs;
        size_t activeIndex = 0;
        for (int i = 0; i < this->ui_.multiView->count(); i++)
        {
            auto *item = this->ui_.multiView->item(i);
            if (!item)
            {
                continue;
            }
            QVariant data = item->data(Qt::UserRole);
            auto *spec = get_if<MultiChannel::Spec>(&data);
            if (spec)
            {
                if (item->data(ActiveDestinationRole).toBool())
                {
                    activeIndex = specs.size();
                }
                specs.emplace_back(std::move(*spec));
            }
        }
        auto ptr = std::make_shared<MultiChannel>(
            specs, this->ui_.multiIndicatorMode->currentData()
                       .value<MultiChannelIndicatorMode>());
        ptr->setActiveChannelIndex(activeIndex);
        return {std::move(ptr)};
    }

    if (this->ui_.channel->isChecked())
    {
        const auto channelName = this->ui_.channelName->text().trimmed();
        if (this->ui_.channelAnonymous->isChecked())
        {
            return getApp()->getTwitch()->getOrAddAnonymousChannel(
                channelName);
        }

        return getApp()->getTwitch()->getOrAddChannel(channelName);
    }

    if (this->ui_.watching->isChecked())
    {
        return getApp()->getTwitch()->getWatchingChannel();
    }

    if (this->ui_.mentions->isChecked())
    {
        return getApp()->getTwitch()->getMentionsChannel();
    }

    if (this->ui_.whispers->isChecked())
    {
        return getApp()->getTwitch()->getWhispersChannel();
    }

    if (this->ui_.live->isChecked())
    {
        return getApp()->getTwitch()->getLiveChannel();
    }

    if (this->ui_.automod->isChecked())
    {
        return getApp()->getTwitch()->getAutomodChannel();
    }

    return this->selectedChannel_;
}

void SelectChannelDialog::setAutoModChannelFilter(QString channel)
{
    channel = channel.trimmed();
    if (channel.startsWith(u'#'))
    {
        channel.remove(0, 1);
    }
    this->ui_.automodChannel->setText(channel);
}

QString SelectChannelDialog::getAutoModChannelFilter() const
{
    auto channel = this->ui_.automodChannel->text().trimmed().toLower();
    if (channel.startsWith(u'#'))
    {
        channel.remove(0, 1);
    }
    return channel;
}

bool SelectChannelDialog::hasSeletedChannel() const
{
    return this->hasSelectedChannel_;
}

bool SelectChannelDialog::EventFilter::eventFilter(QObject *watched,
                                                   QEvent *event)
{
    auto *widget = dynamic_cast<QWidget *>(watched);
    assert(widget);

    auto &ui = this->dialog->ui_;

    if (event->type() == QEvent::KeyPress)
    {
        auto *keyEvent = dynamic_cast<QKeyEvent *>(event);
        assert(keyEvent);

        if ((keyEvent->key() == Qt::Key_Tab ||
             keyEvent->key() == Qt::Key_Down) &&
            keyEvent->modifiers() == Qt::NoModifier)
        {
            // Tab has been pressed, focus next entry in list

            if (widget == ui.channelName)
            {
                // Special case for when current selection is the "Channel" entry's edit box since the Edit box actually has the focus
                ui.whispers->setFocus();
                return true;
            }

            if (widget == ui.automod)
            {
                ui.automodChannel->setFocus();
                ui.automodChannel->selectAll();
                return true;
            }

            if (widget == ui.automodChannel)
            {
                ui.channel->setFocus();
                return true;
            }

            auto *nextInFocusChain = widget->nextInFocusChain();
            if (nextInFocusChain->focusPolicy() == Qt::FocusPolicy::NoFocus)
            {
                // Make sure we're not selecting one of the labels
                nextInFocusChain = nextInFocusChain->nextInFocusChain();
            }
            nextInFocusChain->setFocus();
            return true;
        }

        if (((keyEvent->key() == Qt::Key_Tab ||
              keyEvent->key() == Qt::Key_Backtab) &&
             keyEvent->modifiers() == Qt::ShiftModifier) ||
            ((keyEvent->key() == Qt::Key_Up) &&
             keyEvent->modifiers() == Qt::NoModifier))
        {
            // Shift+Tab has been pressed, focus previous entry in list

            if (widget == ui.channelName)
            {
                if (ui.automodChannel->isVisible())
                {
                    ui.automodChannel->setFocus();
                    ui.automodChannel->selectAll();
                }
                else
                {
                    ui.automod->setFocus();
                }
                return true;
            }

            if (widget == ui.automodChannel)
            {
                ui.automod->setFocus();
                return true;
            }

            if (widget == ui.whispers)
            {
                ui.channel->setFocus();
                return true;
            }

            auto *previousInFocusChain = widget->previousInFocusChain();
            if (previousInFocusChain->focusPolicy() == Qt::FocusPolicy::NoFocus)
            {
                // Make sure we're not selecting one of the labels
                previousInFocusChain =
                    previousInFocusChain->previousInFocusChain();
            }
            previousInFocusChain->setFocus();
            return true;
        }

        if (keyEvent == QKeySequence::DeleteStartOfWord)
        {
            if (auto *lineEdit = qobject_cast<QLineEdit *>(widget);
                lineEdit != nullptr && lineEdit->selectionLength() > 0)
            {
                lineEdit->backspace();
                return true;
            }
        }

        return false;
    }

    return false;
}

void SelectChannelDialog::closeEvent(QCloseEvent * /*event*/)
{
    this->closed.invoke();
}

void SelectChannelDialog::themeChangedEvent()
{
    BaseWindow::themeChangedEvent();

    this->setPalette(getTheme()->palette);
}

void SelectChannelDialog::scaleChangedEvent(float newScale)
{
    BaseWindow::scaleChangedEvent(newScale);

    auto &ui = this->ui_;

    // NOTE: Normally the font is automatically inherited from its parent, but since we override
    // the style sheet to respect light/dark theme, we have to manually update the font here
    auto uiFont =
        getApp()->getFonts()->getFont(FontStyle::UiMedium, this->scale());

    ui.channelName->setFont(uiFont);
    ui.channelAnonymous->setFont(uiFont);
    ui.automodChannel->setFont(uiFont);
    ui.kickName->setFont(uiFont);
    ui.youtubeSource->setFont(uiFont);
    ui.tiktokSource->setFont(uiFont);
    ui.multiView->setMinimumHeight(qRound(120 * newScale));
}

void SelectChannelDialog::addShortcuts()
{
    HotkeyController::HotkeyMap actions{
        {"accept",
         [this](const std::vector<QString> &) -> QString {
             this->ok();
             return "";
         }},
        {"reject",
         [this](const std::vector<QString> &) -> QString {
             this->close();
             return "";
         }},

        // these make no sense, so they aren't implemented
        {"scrollPage", nullptr},
        {"search", nullptr},
        {"delete", nullptr},
        {"openTab", nullptr},
    };

    this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
        HotkeyCategory::PopupWindow, actions, this);
}

}  // namespace chatterino

#include "SelectChannelDialog.moc"
