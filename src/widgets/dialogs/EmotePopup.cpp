// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/EmotePopup.hpp"

#include "Application.hpp"
#include "common/enums/MessageContext.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "debug/Benchmark.hpp"
#include "messages/Image.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/emoji/Emojis.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/seventv/SeventvPersonalEmotes.hpp"
#include "providers/tiktok/TikTokEmotes.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/youtube/YouTubeChannel.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Helpers.hpp"
#include "util/MemoryReclaimer.hpp"
#include "util/QStringHash.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/TrimRegExpValidator.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/Scrollbar.hpp"

#include <QAbstractButton>
#include <QAction>
#include <QHBoxLayout>
#include <QMenu>
#include <QPointer>
#include <QRegularExpression>
#include <QStringBuilder>
#include <QTabWidget>

#include <algorithm>
#include <iterator>
#include <map>
#include <ranges>
#include <utility>

namespace {

using namespace chatterino;
using namespace Qt::Literals;

constexpr size_t EMOTES_PER_MESSAGE = 32;
constexpr size_t EMOTE_POPUP_MESSAGE_LIMIT = 2048;

ChannelPtr makeEmoteChannel()
{
    return std::make_shared<Channel>("", Channel::Type::None,
                                     EMOTE_POPUP_MESSAGE_LIMIT);
}

bool emojiHasShortCode(const EmojiPtr &emoji, const QString &shortCode)
{
    auto it = std::ranges::find(emoji->shortCodes, shortCode);
    return it != emoji->shortCodes.end();
}

std::optional<EmotePtr> findEmoteByName(const EmoteName &name,
                                        const EmoteMap &emoteMap)
{
    auto it = emoteMap.find(name);
    return it == emoteMap.cend() ? std::nullopt
                                 : std::optional<EmotePtr>(it->second);
}

QString toEmojiShortCode(const QString &shortCodeWithColons)
{
    if (shortCodeWithColons.length() > 2)
    {
        return shortCodeWithColons.mid(1, shortCodeWithColons.length() - 2);
    }

    return shortCodeWithColons;
}

bool isFavouriteEmoteOrEmoji(const QString &identifier, bool isEmoji)
{
    if (isEmoji)
    {
        return getSettings()->favouriteEmojis.getValue().contains(
            toEmojiShortCode(identifier));
    }

    return getSettings()->favouriteEmotes.getValue().contains(identifier);
}

auto saveFavouriteEmojis(const std::unordered_map<QString, EmojiPtr> &emojis)
{
    QStringList emojiNames;
    emojiNames.reserve(static_cast<qsizetype>(emojis.size()));

    std::ranges::transform(emojis, std::back_inserter(emojiNames),
                           [](const auto &it) {
                               return it.first;
                           });

    getSettings()->favouriteEmojis = emojiNames;
}

auto makeTitleMessage(const QString &title)
{
    MessageBuilder builder;
    builder.emplace<TextElement>(title, MessageElementFlag::Text);
    builder->flags.set(MessageFlag::Centered);
    return builder.release();
}

template <typename Item, typename AddElement>
void addPickerMessages(Channel &channel, const std::vector<Item> &items,
                       const QString &emptyText, AddElement &&addElement)
{
    if (items.empty())
    {
        if (emptyText.isEmpty())
        {
            return;
        }

        MessageBuilder builder;
        builder->flags.set(MessageFlag::Centered);
        builder->flags.set(MessageFlag::DisableCompactEmotes);
        builder.emplace<TextElement>(emptyText, MessageElementFlag::Text,
                                     MessageColor::System);
        channel.addMessage(builder.release(), MessageContext::Original);
        return;
    }

    for (size_t offset = 0; offset < items.size();
         offset += EMOTES_PER_MESSAGE)
    {
        MessageBuilder builder;
        builder->flags.set(MessageFlag::Centered);
        builder->flags.set(MessageFlag::DisableCompactEmotes);

        const auto end = std::min(items.size(), offset + EMOTES_PER_MESSAGE);
        for (size_t index = offset; index < end; ++index)
        {
            addElement(builder, items[index]);
        }

        channel.addMessage(builder.release(), MessageContext::Original);
    }
}

void addEmoteMessagesSorted(Channel &channel,
                            const std::vector<EmotePtr> &emotes,
                            const QString &emptyText = {})
{
    addPickerMessages(
        channel, emotes, emptyText,
        [](MessageBuilder &builder, const EmotePtr &emote) {
            auto *element = builder.emplace<EmoteElement>(
                emote, MessageElementFlags{MessageElementFlag::AlwaysShow,
                                           MessageElementFlag::Emote});
            element->setAnimatedPreview();
            element->setLink(Link(Link::InsertText, emote->name.string));
        });
}

void addEmoteMessages(Channel &channel, std::vector<EmotePtr> emotes,
                      const QString &emptyText = {})
{
    std::sort(emotes.begin(), emotes.end(), [](const auto &l, const auto &r) {
        return compareEmoteStrings(l->name.string, r->name.string);
    });

    addEmoteMessagesSorted(channel, emotes, emptyText);
}

void addEmoteMessages(Channel &channel, const EmoteMap &map)
{
    std::vector<EmotePtr> vec;
    vec.reserve(map.size());
    for (const auto &[_name, ptr] : map)
    {
        vec.emplace_back(ptr);
    }
    addEmoteMessages(channel, std::move(vec), "no emotes available");
}

void addEmojiMessages(Channel &channel, const std::vector<EmojiPtr> &emojiMap,
                      const QString &emptyText = {})
{
    addPickerMessages(
        channel, emojiMap, emptyText,
        [](MessageBuilder &builder, const EmojiPtr &value) {
            auto *element = builder.emplace<EmoteElement>(
                getApp()->getEmotes()->getEmojis()->getEmote(value),
                MessageElementFlags{MessageElementFlag::AlwaysShow,
                                    MessageElementFlag::EmojiAll});
            element->setAnimatedPreview();
            element->setLink(Link(Link::Type::InsertText,
                                  ":" + value->shortCodes[0] + ":"));
        });
}

auto makeInfoTextMessage(const QString &text)
{
    MessageBuilder builder;
    builder->flags.set(MessageFlag::Centered);
    builder.emplace<TextElement>(
        text,
        MessageElementFlags{MessageElementFlag::Text,
                            MessageElementFlag::AlwaysShow},
        MessageColor::System);

    return builder.release();
}

void addEmotes(Channel &channel, auto &&emotes, const QString &title)
{
    channel.addMessage(makeTitleMessage(title), MessageContext::Original);
    addEmoteMessages(channel, std::forward<decltype(emotes)>(emotes));
}

void addTwitchSubEmoteSets(const std::shared_ptr<const EmoteMap> &local,
                           const std::shared_ptr<const TwitchEmoteSetMap> &sets,
                           Channel &subChannel, const QString &currentChannelID,
                           const QString &channelName)
{
    if (!local->empty())
    {
        addEmotes(subChannel, *local, channelName % u" (Follower)");
    }

    std::vector<
        std::pair<QString, std::reference_wrapper<const TwitchEmoteSet>>>
        sortedSets;
    sortedSets.reserve(sets->size());
    for (const auto &[_id, set] : *sets)
    {
        if (set.owner && set.owner->id == currentChannelID)
        {

            addEmotes(subChannel, set.emotes, set.title());
        }
        else if (set.isSubLike)
        {
            sortedSets.emplace_back(set.title(), std::cref(set));
        }
    }

    std::ranges::sort(sortedSets, [](const auto &a, const auto &b) {
        return a.first.compare(b.first, Qt::CaseInsensitive) < 0;
    });
    for (const auto &[title, set] : sortedSets)
    {
        addEmotes(subChannel, set.get().emotes, title);
    }
}

void addTwitchGlobalEmoteSets(
    const std::shared_ptr<const TwitchEmoteSetMap> &sets, Channel &globalChannel,
    const QString &currentChannelID)
{
    std::vector<
        std::pair<QString, std::reference_wrapper<const TwitchEmoteSet>>>
        sortedSets;
    sortedSets.reserve(sets->size());
    for (const auto &[_id, set] : *sets)
    {
        if (!set.owner || set.owner->id != currentChannelID)
        {
            if (!set.isSubLike)
            {
                sortedSets.emplace_back(set.title(), std::cref(set));
            }
        }
    }

    std::ranges::sort(sortedSets, [](const auto &a, const auto &b) {
        return a.first.compare(b.first, Qt::CaseInsensitive) < 0;
    });

    for (const auto &[title, set] : sortedSets)
    {
        addEmotes(globalChannel, set.get().emotes, title);
    }
}

void loadEmojis(ChannelView &view, const std::vector<EmojiPtr> &emojiMap)
{
    static auto emoteCategoryMap = [&] {
        std::map<QString, std::vector<EmojiPtr>> emoteCatMap;

        for (const auto &emoji : emojiMap)
        {
            auto cat = emoteCatMap.find(emoji->category);
            if (cat != emoteCatMap.end())
            {
                auto &vec = cat->second;
                vec.push_back(emoji);
            }
            else
            {
                emoteCatMap.emplace(emoji->category,
                                    std::vector<EmojiPtr>{emoji});
            }
        }
        return emoteCatMap;
    }();

    auto emojiChannel = makeEmoteChannel();

    view.setChannel(emojiChannel);

    for (auto &it : emoteCategoryMap)
    {

        if (it.first == "Component")
        {
            continue;
        }

        emojiChannel->addMessage(makeTitleMessage(it.first),
                                 MessageContext::Original);
        addEmojiMessages(*emojiChannel, it.second);
    }

    emojiChannel->addMessage(makeTitleMessage("Component"),
                             MessageContext::Original);
    addEmojiMessages(*emojiChannel, emoteCategoryMap["Component"]);
}

void loadEmojis(Channel &channel, const std::vector<EmojiPtr> &emojiMap,
                const QString &title)
{
    channel.addMessage(makeTitleMessage(title), MessageContext::Original);
    addEmojiMessages(channel, emojiMap);
}

EmoteMap filterEmoteMap(const QString &text, const EmoteMap &emotes)
{
    EmoteMap filteredMap;

    for (const auto &emote : emotes)
    {
        if (emote.first.string.contains(text, Qt::CaseInsensitive))
        {
            filteredMap.insert(emote);
        }
    }

    return filteredMap;
}

}

namespace chatterino {

EmotePopup::EmotePopup(QWidget *parent)
    : BasePopup({BaseWindow::EnableCustomFrame, BaseWindow::DisableLayoutSave},
                parent)
    , search_(new QLineEdit())
    , notebook_(new Notebook(this))
{

    auto bounds = getApp()->getWindows()->emotePopupBounds();
    if (bounds.size().isEmpty())
    {
        bounds.setSize(QSize{300, 500} * this->scale());
    }
    this->setInitialBounds(bounds, widgets::BoundsChecking::DesiredPosition);

    auto *layout = new QVBoxLayout();
    this->getLayoutContainer()->setLayout(layout);

    QRegularExpression searchRegex("\\S*");
    searchRegex.setPatternOptions(QRegularExpression::CaseInsensitiveOption);

    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *layout2 = new QHBoxLayout();
    layout2->setContentsMargins(8, 8, 8, 8);
    layout2->setSpacing(8);

    this->search_->setPlaceholderText("Search all emotes...");
    this->search_->setValidator(new TrimRegExpValidator(searchRegex, this->search_));
    this->search_->setClearButtonEnabled(true);
    if (auto *clearButton = this->search_->findChild<QAbstractButton *>())
    {
        clearButton->setIcon(QPixmap(":/buttons/clearSearch.png"));
    }
    this->search_->installEventFilter(this);
    layout2->addWidget(this->search_);

    layout->addLayout(layout2);

    this->searchDebounce_.setSingleShot(true);
    this->searchDebounce_.setInterval(75);
    QObject::connect(&this->searchDebounce_, &QTimer::timeout, this, [this] {
        this->filterEmotes(this->search_->text());
    });
    QObject::connect(this->search_, &QLineEdit::textChanged, this,
                     [this](const QString &text) {
                         if (text.isEmpty())
                         {
                             this->searchDebounce_.stop();
                             this->filterEmotes(text);
                             return;
                         }
                         this->searchDebounce_.start();
                     });

    auto clicked = [this](const MessageLayoutElement *hoveredElement,
                          Qt::KeyboardModifiers modifiers) {
        if (hoveredElement == nullptr)
        {
            return;
        }

        const auto link = hoveredElement->getLink();
        if (modifiers.testFlag(Qt::KeyboardModifier::ControlModifier))
        {
            const auto *page = this->searchView_->isVisible()
                                   ? this->searchView_
                                   : this->notebook_->getSelectedPage();

            const auto identifier = link.value;
            const auto isEmoji = hoveredElement->getCreator().getFlags().hasAny(
                MessageElementFlag::EmojiAll);
            if (identifier.isEmpty())
            {
                return;
            }

            if (this->favouritesView_ == page)
            {
                if (isEmoji)
                {
                    this->removeFavouriteEmoji(toEmojiShortCode(identifier));
                }
                else
                {
                    this->removeFavouriteEmote(EmoteName{identifier});
                }
            }
            else if (isEmoji)
            {
                this->addFavouriteEmoji(toEmojiShortCode(identifier));
            }
            else
            {
                this->addFavouriteEmote(EmoteName{identifier});
            }

            if (!modifiers.testFlag(Qt::KeyboardModifier::ShiftModifier))
            {
                return;
            }
        }

        this->linkClicked.invoke(link);
    };

    auto makeView = [&](QString tabTitle, bool addToNotebook = true) {
        auto *view = new ChannelView(nullptr, ChannelView::Context::None,
                                     EMOTE_POPUP_MESSAGE_LIMIT);

        view->setChannel(makeEmoteChannel());
        view->setHoverAnimateOnly(true, true);
        view->setOverrideFlags(MessageElementFlags{
            MessageElementFlag::Default, MessageElementFlag::AlwaysShow,
            MessageElementFlag::EmoteImage});
        view->setEnableScrollingToBottom(false);

        view->installEventFilter(this);
        std::ignore = view->elementClicked.connect(clicked);

        if (addToNotebook)
        {
            this->notebook_->addPage(view, std::move(tabTitle));
        }

        std::ignore = view->messageMenuCreated.connect(
            [this](QMenu *menu, const MessageLayoutElement *hoveredElement) {
                if (hoveredElement == nullptr)
                {
                    return;
                }

                auto flags = hoveredElement->getCreator().getFlags();

                if (!flags.hasAny(MessageElementFlag::EmojiAll,
                                  MessageElementFlag::Emote))
                {
                    return;
                }

                QAction *favouriteAction;
                if (menu->actions().isEmpty())
                {
                    favouriteAction = menu->addAction("Favourite");
                }
                else
                {
                    favouriteAction = new QAction("Favourite", menu);
                    menu->insertAction(menu->actions().constFirst(),
                                       favouriteAction);
                }

                auto isEmoji = flags.hasAny(MessageElementFlag::EmojiAll);
                const auto &identifier = hoveredElement->getLink().value;

                favouriteAction->setCheckable(true);
                favouriteAction->setChecked(
                    isFavouriteEmoteOrEmoji(identifier, isEmoji));

                QObject::connect(
                    favouriteAction, &QAction::triggered, this,
                    [this, identifier, isEmoji](bool checked) {
                        if (!checked)
                        {
                            if (isEmoji)
                            {
                                this->removeFavouriteEmoji(
                                    toEmojiShortCode(identifier));
                            }
                            else
                            {
                                this->removeFavouriteEmote(
                                    EmoteName{identifier});
                            }
                        }
                        else
                        {
                            if (isEmoji)
                            {
                                this->addFavouriteEmoji(
                                    toEmojiShortCode(identifier));
                            }
                            else
                            {
                                this->addFavouriteEmote(EmoteName{identifier});
                            }
                        }
                    });
            });

        return view;
    };

    this->searchView_ = makeView("", false);
    this->searchView_->hide();
    layout->addWidget(this->searchView_);

    layout->addWidget(this->notebook_);
    layout->setContentsMargins(0, 0, 0, 0);

    this->favouritesView_ = makeView("Favourite");
    this->subEmotesView_ = makeView("Subs");
    this->channelEmotesView_ = makeView("Channel");
    this->globalEmotesView_ = makeView("Global");
    this->viewEmojis_ = makeView("Emojis");

    this->notebook_->select(this->subEmotesView_);

    this->addShortcuts();
    this->signalHolder_.managedConnect(getApp()->getHotkeys()->onItemsUpdated,
                                       [this]() {
                                           this->clearShortcuts();
                                           this->addShortcuts();
                                       });

    this->search_->setFocus();

    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->twitch.emotesReloaded,
        [this](auto * , const auto &result) {
            if (!result)
            {

                return;
            }
            this->reloadEmotes();
            if (this->isVisible() && !this->search_->text().isEmpty())
            {
                this->filterEmotes(this->search_->text());
            }
        });

    const auto accountChanged = [this] {
        this->reloadEmotes();
        if (this->isVisible() && !this->search_->text().isEmpty())
        {
            this->filterEmotes(this->search_->text());
        }
    };
    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->twitch.currentUserChanged, accountChanged);
    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->kick.currentUserChanged, accountChanged);

    this->themeChangedEvent();
    for (auto *setting :
         {&getSettings()->animateEmotes, &getSettings()->animationsWhenFocused})
    {
        setting->connect(
            [this](bool, auto) {
                for (auto *view :
                     {this->globalEmotesView_, this->channelEmotesView_,
                      this->subEmotesView_, this->viewEmojis_,
                      this->favouritesView_, this->searchView_})
                {
                    if (view->isVisible())
                    {
                        view->update();
                    }
                }
            },
            this->signalHolder_);
    }
}

void EmotePopup::addShortcuts()
{
    HotkeyController::HotkeyMap actions{
        {"openTab",
         [this](std::vector<QString> arguments) -> QString {
             if (arguments.empty())
             {
                 qCWarning(chatterinoHotkeys)
                     << "openTab shortcut called without arguments. Takes "
                        "only one argument: tab specifier";
                 return "openTab shortcut called without arguments. "
                        "Takes only one argument: tab specifier";
             }
             auto target = arguments.at(0);
             if (target == "last")
             {
                 this->notebook_->selectLastTab();
             }
             else if (target == "next")
             {
                 this->notebook_->selectNextTab();
             }
             else if (target == "previous")
             {
                 this->notebook_->selectPreviousTab();
             }
             else
             {
                 bool ok{false};
                 int result = target.toInt(&ok);
                 if (ok)
                 {
                     this->notebook_->selectVisibleIndex(result, false);
                 }
                 else
                 {
                     qCWarning(chatterinoHotkeys)
                         << "Invalid argument for openTab shortcut";
                     return QString("Invalid argument for openTab "
                                    "shortcut: \"%1\". Use \"last\", "
                                    "\"next\", \"previous\" or an integer.")
                         .arg(target);
                 }
             }
             this->loadTabIfNeeded(this->notebook_->getSelectedPage());
             return "";
         }},
        {"delete",
         [this](const std::vector<QString> &) -> QString {
             this->close();
             return "";
         }},
        {"scrollPage",
         [this](std::vector<QString> arguments) -> QString {
             if (arguments.empty())
             {
                 qCWarning(chatterinoHotkeys)
                     << "scrollPage hotkey called without arguments!";
                 return "scrollPage hotkey called without arguments!";
             }
             auto direction = arguments.at(0);
             auto *channelView = dynamic_cast<ChannelView *>(
                 this->notebook_->getSelectedPage());
             if (channelView == nullptr)
             {
                 return "No emote tab is selected";
             }

             auto &scrollbar = channelView->getScrollBar();
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

        {"reject", nullptr},
        {"accept", nullptr},
        {"search",
         [this](const std::vector<QString> &) -> QString {
             this->search_->setFocus();
             this->search_->selectAll();
             return "";
         }},
    };

    this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
        HotkeyCategory::PopupWindow, actions, this);
}

void EmotePopup::loadChannel(ChannelPtr channel)
{
    BenchmarkGuard guard("loadChannel");

    this->channel_ = std::move(channel);
    this->twitchChannel_ = dynamic_cast<TwitchChannel *>(this->channel_.get());
    this->kickChannel_ = dynamic_cast<KickChannel *>(this->channel_.get());
    this->youtubeChannel_ =
        dynamic_cast<YouTubeChannel *>(this->channel_.get());
    this->youtubeEmotesConnection_ = pajlada::Signals::ScopedConnection{};
    if (this->youtubeChannel_)
    {
        this->youtubeEmotesConnection_ =
            this->youtubeChannel_->youtubeEmotesChanged.connect([this] {
                this->reloadEmotes();
                if (this->isVisible() && !this->search_->text().isEmpty())
                {
                    this->filterEmotes(this->search_->text());
                }
            });
    }

    this->globalEmotesView_->setChannel(makeEmoteChannel());
    this->subEmotesView_->setChannel(makeEmoteChannel());
    this->channelEmotesView_->setChannel(makeEmoteChannel());
    this->viewEmojis_->setChannel(makeEmoteChannel());
    this->searchView_->setChannel(makeEmoteChannel());
    this->favouritesView_->setChannel(makeEmoteChannel());

    if (!this->channel_)
    {
        this->setWindowTitle("Emotes");
        this->reloadEmotes();
        return;
    }

    this->setWindowTitle("Emotes in #" + this->channel_->getName());

    this->reloadEmotes();
    if (this->youtubeChannel_ || this->channel_->isTikTokChannel())
    {
        this->notebook_->select(this->channelEmotesView_);
    }
    if (this->isVisible() && !this->search_->text().isEmpty())
    {
        this->filterEmotes(this->search_->text());
    }
}

void EmotePopup::addFavouriteEmoji(const QString &shortCode)
{
    this->populateFavourites();

    if (shortCode.isEmpty())
    {
        return;
    }
    if (this->favouriteEmojis_.contains(shortCode))
    {
        return;
    }

    for (const auto &emoji : getApp()->getEmotes()->getEmojis()->getEmojis())
    {
        if (emojiHasShortCode(emoji, shortCode))
        {
            this->favouriteEmojis_.emplace(shortCode, emoji);
            break;
        }
    }

    this->updateFavouriteEmotesAndEmojis();
    saveFavouriteEmojis(this->favouriteEmojis_);
}

void EmotePopup::addFavouriteEmote(const EmoteName &name)
{
    this->populateFavourites();

    auto emoteNames = getSettings()->favouriteEmotes.getValue();
    for (const auto &emotePresentName : emoteNames)
    {
        if (emotePresentName == name.string)
        {
            return;
        }
    }
    auto emote = this->findEmote(name);
    if (!emote)
    {
        return;
    }

    this->favouriteEmotes_.push_back(std::move(*emote));

    emoteNames.push_back(name.string);
    getSettings()->favouriteEmotes = emoteNames;

    this->updateFavouriteEmotesAndEmojis();
}

void EmotePopup::removeFavouriteEmoji(const QString &shortCode)
{
    this->populateFavourites();

    this->favouriteEmojis_.erase(shortCode);
    saveFavouriteEmojis(this->favouriteEmojis_);

    this->updateFavouriteEmotesAndEmojis();
}

void EmotePopup::removeFavouriteEmote(const EmoteName &name)
{
    this->populateFavourites();

    std::erase_if(this->favouriteEmotes_, [name](const auto &emote) {
        return emote->name == name;
    });

    auto emoteNames = getSettings()->favouriteEmotes.getValue();
    emoteNames.removeIf([name](const auto &emoteName) {
        return emoteName == name.string;
    });
    getSettings()->favouriteEmotes = emoteNames;

    this->updateFavouriteEmotesAndEmojis();
}

void EmotePopup::updateFavouriteEmotesAndEmojis()
{
    auto chan = this->favouritesView_->underlyingChannel();
    chan->clearMessages();

    if (this->favouriteEmotes_.empty() && this->favouriteEmojis_.empty() &&
        getSettings()->favouriteEmotes.getValue().isEmpty())
    {
        auto msg = makeInfoTextMessage(
            "No favourites yet. Ctrl+click an emote or emoji, or choose "
            "Favourite from its menu.");
        chan->addMessage(msg, MessageContext::Original);

        return;
    }

    if (!this->favouriteEmotes_.empty())
    {
        addEmoteMessagesSorted(*chan, this->favouriteEmotes_);
    }

    if (!this->favouriteEmojis_.empty())
    {
        std::vector<EmojiPtr> emojis;
        emojis.reserve(this->favouriteEmojis_.size());
        std::ranges::transform(this->favouriteEmojis_,
                               std::back_inserter(emojis), [](const auto &v) {
                                   return v.second;
                               });
        addEmojiMessages(*chan, emojis);
    }

    std::vector<QString> unavailableEmotes;
    for (const auto &emoteName : getSettings()->favouriteEmotes.getValue())
    {
        auto it = std::ranges::find_if(
            this->favouriteEmotes_, [emoteName](const auto &emote) {
                return emoteName == emote->name.string;
            });
        if (it == this->favouriteEmotes_.end())
        {
            unavailableEmotes.push_back(emoteName);
        }
    }
    if (!unavailableEmotes.empty())
    {
        static const auto explainUnavailability =
            u"These emotes may require a specific channel or an active "
            u"subscription, or their availability could not be checked."_s;

        auto msg =
            makeInfoTextMessage("Unavailable favourites");
        chan->addMessage(msg, MessageContext::Original);

        msg = makeInfoTextMessage(explainUnavailability);
        chan->addMessage(msg, MessageContext::Original);

        addPickerMessages(
            *chan, unavailableEmotes, {},
            [](MessageBuilder &builder, const QString &name) {
                builder.emplace<TextElement>(
                    name, MessageElementFlags{MessageElementFlag::EmoteText,
                                              MessageElementFlag::AlwaysShow})
                    ->setLink(Link(Link::InsertText, name));
            });
    }
}

void EmotePopup::reloadEmotes()
{
    this->channelEmotesLoaded_ = false;
    this->subEmotesLoaded_ = false;
    this->globalEmotesLoaded_ = false;
    this->emojisLoaded_ = false;
    this->favouritesLoaded_ = false;

    if (this->channelEmotesView_)
    {
        this->channelEmotesView_->underlyingChannel()->clearMessages();
    }
    if (this->subEmotesView_)
    {
        this->subEmotesView_->underlyingChannel()->clearMessages();
    }
    if (this->globalEmotesView_)
    {
        this->globalEmotesView_->underlyingChannel()->clearMessages();
    }
    if (this->viewEmojis_)
    {
        this->viewEmojis_->underlyingChannel()->clearMessages();
    }
    if (this->favouritesView_)
    {
        this->favouritesView_->underlyingChannel()->clearMessages();
    }

    if (this->searchView_)
    {
        this->searchView_->underlyingChannel()->clearMessages();
    }

    if (this->isVisible() && this->search_->text().isEmpty())
    {
        this->loadTabIfNeeded(this->notebook_->getSelectedPage());
    }
}

void EmotePopup::loadTabIfNeeded(QWidget *page)
{
    if (page == nullptr)
    {
        return;
    }

    if (page == this->channelEmotesView_)
    {
        this->populateChannelEmotes();
    }
    else if (page == this->subEmotesView_)
    {
        this->populateSubEmotes();
    }
    else if (page == this->globalEmotesView_)
    {
        this->populateGlobalEmotes();
    }
    else if (page == this->viewEmojis_)
    {
        this->populateEmojis();
    }
    else if (page == this->favouritesView_)
    {
        this->populateFavourites();
    }
}

void EmotePopup::populateChannelEmotes()
{
    if (this->channelEmotesLoaded_ || !this->channelEmotesView_)
    {
        return;
    }
    this->channelEmotesLoaded_ = true;
    auto channelChannel = this->channelEmotesView_->underlyingChannel();
    channelChannel->clearMessages();

    if (this->channel_ && this->channel_->isTikTokChannel())
    {
        addEmotes(*channelChannel, *tikTokBuiltinEmotes(), "TikTok Emoji");
        return;
    }

    if (this->twitchChannel_)
    {
        if (Settings::instance().enableBTTVChannelEmotes)
        {
            addEmotes(*channelChannel, *this->twitchChannel_->bttvEmotes(),
                      "BetterTTV");
        }
        if (Settings::instance().enableFFZChannelEmotes)
        {
            addEmotes(*channelChannel, *this->twitchChannel_->ffzEmotes(),
                      "FrankerFaceZ");
        }
        if (Settings::instance().enableSevenTVChannelEmotes)
        {
            addEmotes(*channelChannel, *this->twitchChannel_->seventvEmotes(),
                      "7TV");
        }
    }
    if (this->kickChannel_)
    {
        if (Settings::instance().enableSevenTVChannelEmotes)
        {
            addEmotes(*channelChannel, *this->kickChannel_->seventvEmotes(),
                      "7TV");
        }
    }
    if (this->youtubeChannel_)
    {
        const auto emotes = this->youtubeChannel_->youtubeEmotes();
        if (!emotes->empty())
        {
            addEmotes(*channelChannel, *emotes, "YouTube Emoji");
        }
        else
        {
            channelChannel->addMessage(
                makeInfoTextMessage("No YouTube custom emoji available"),
                MessageContext::Original);
        }
    }
}

void EmotePopup::populateSubEmotes()
{
    if (this->subEmotesLoaded_ || !this->subEmotesView_)
    {
        return;
    }
    this->subEmotesLoaded_ = true;
    auto subChannel = this->subEmotesView_->underlyingChannel();
    subChannel->clearMessages();

    if (this->twitchChannel_)
    {
        addTwitchSubEmoteSets(
            twitchChannel_->localTwitchEmotes(),
            *getApp()->getAccounts()->twitch.getCurrent()->accessEmoteSets(),
            *subChannel, twitchChannel_->roomId(), twitchChannel_->getName());

        for (const auto &map :
             getApp()->getSeventvPersonalEmotes()->getEmoteSetsForTwitchUser(
                 getApp()->getAccounts()->twitch.getCurrent()->getUserId()))
        {
            addEmotes(*subChannel, *map, "7TV (Personal)");
        }
    }
    if (this->kickChannel_)
    {

        const auto personalEmotes =
            getApp()->getSeventvPersonalEmotes()->getEmoteSetsForKickUser(
                getApp()->getAccounts()->kick.current()->userID());
        for (const auto &map : personalEmotes)
        {
            addEmotes(*subChannel, *map, "7TV (Personal)");
        }
    }

    if (!subChannel->hasMessages())
    {
        MessageBuilder builder;
        builder->flags.set(MessageFlag::Centered);
        builder->flags.set(MessageFlag::DisableCompactEmotes);
        builder.emplace<TextElement>("no subscription emotes available",
                                     MessageElementFlag::Text,
                                     MessageColor::System);
        subChannel->addMessage(builder.release(), MessageContext::Original);
    }
}

void EmotePopup::populateGlobalEmotes()
{
    if (this->globalEmotesLoaded_ || !this->globalEmotesView_)
    {
        return;
    }
    this->globalEmotesLoaded_ = true;
    auto globalChannel = this->globalEmotesView_->underlyingChannel();
    globalChannel->clearMessages();

    if (this->twitchChannel_)
    {
        addTwitchGlobalEmoteSets(
            *getApp()->getAccounts()->twitch.getCurrent()->accessEmoteSets(),
            *globalChannel, twitchChannel_->roomId());
    }
    if (this->kickChannel_)
    {

        addEmotes(*globalChannel,
                  *getApp()->getKickChatServer()->globalEmotes(), "Kick");
    }
    if (!this->youtubeChannel_ &&
        (!this->channel_ || !this->channel_->isTikTokChannel()))
    {
        if (Settings::instance().enableBTTVGlobalEmotes)
        {
            addEmotes(*globalChannel, *getApp()->getBttvEmotes()->emotes(),
                      "BetterTTV");
        }
        if (Settings::instance().enableFFZGlobalEmotes)
        {
            addEmotes(*globalChannel, *getApp()->getFfzEmotes()->emotes(),
                      "FrankerFaceZ");
        }
        if (Settings::instance().enableSevenTVGlobalEmotes)
        {
            addEmotes(*globalChannel,
                      *getApp()->getSeventvEmotes()->globalEmotes(), "7TV");
        }
    }
}

void EmotePopup::populateEmojis()
{
    if (this->emojisLoaded_ || !this->viewEmojis_)
    {
        return;
    }
    this->emojisLoaded_ = true;
    this->viewEmojis_->underlyingChannel()->clearMessages();
    loadEmojis(*this->viewEmojis_,
               getApp()->getEmotes()->getEmojis()->getEmojis());
}

void EmotePopup::populateFavourites()
{
    if (this->favouritesLoaded_ || !this->favouritesView_)
    {
        return;
    }
    this->favouritesLoaded_ = true;
    this->favouritesView_->underlyingChannel()->clearMessages();

    this->favouriteEmotes_.clear();
    const auto &emoteNames = getSettings()->favouriteEmotes;
    for (const auto &emoteName : emoteNames.getValue())
    {
        auto emote = this->findEmote(EmoteName{emoteName});
        if (emote)
        {
            this->favouriteEmotes_.push_back(*emote);
        }
    }
    this->favouriteEmojis_.clear();
    const auto &emojiShortCodes = getSettings()->favouriteEmojis;
    for (const auto &shortCode : emojiShortCodes.getValue())
    {
        for (const auto &emoji :
             getApp()->getEmotes()->getEmojis()->getEmojis())
        {
            if (emojiHasShortCode(emoji, shortCode))
            {
                this->favouriteEmojis_.emplace(shortCode, emoji);
                break;
            }
        }
    }
    this->updateFavouriteEmotesAndEmojis();
}

void EmotePopup::releaseLoadedEmotes()
{
    if (this->channelEmotesView_)
    {
        this->channelEmotesView_->underlyingChannel()->clearMessages();
        this->channelEmotesLoaded_ = false;
    }
    if (this->subEmotesView_)
    {
        this->subEmotesView_->underlyingChannel()->clearMessages();
        this->subEmotesLoaded_ = false;
    }
    if (this->globalEmotesView_)
    {
        this->globalEmotesView_->underlyingChannel()->clearMessages();
        this->globalEmotesLoaded_ = false;
    }
    if (this->viewEmojis_)
    {
        this->viewEmojis_->underlyingChannel()->clearMessages();
        this->emojisLoaded_ = false;
    }
    if (this->favouritesView_)
    {
        this->favouritesView_->underlyingChannel()->clearMessages();
        this->favouritesLoaded_ = false;
    }

    this->favouriteEmotes_.clear();
    this->favouriteEmojis_.clear();
    if (this->searchView_)
    {
        this->searchView_->underlyingChannel()->clearMessages();
    }

    Image::releaseUnusedCacheEntries();
    requestTransientMemoryPressureRelief();
}

bool EmotePopup::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() == QEvent::Show)
    {
        auto *page = qobject_cast<QWidget *>(object);
        if (page != nullptr)
        {
            QPointer<QWidget> guardedPage = page;
            QTimer::singleShot(0, this, [this, guardedPage] {
                if (this->isVisible() && guardedPage != nullptr &&
                    guardedPage->isVisible() &&
                    this->notebook_->getSelectedPage() == guardedPage)
                {
                    this->loadTabIfNeeded(guardedPage);
                }
            });
        }
    }

    if (object == this->search_ && event->type() == QEvent::KeyPress)
    {
        auto *keyEvent = dynamic_cast<QKeyEvent *>(event);
        if (keyEvent == QKeySequence::DeleteStartOfWord &&
            this->search_->selectionLength() > 0)
        {
            this->search_->backspace();
            return true;
        }
    }
    return false;
}

void EmotePopup::filterTwitchEmotes(std::shared_ptr<Channel> searchChannel,
                                    const QString &searchText)
{
    if (this->twitchChannel_)
    {
        auto local = filterEmoteMap(searchText,
                                    *this->twitchChannel_->localTwitchEmotes());
        if (!local.empty())
        {
            addEmotes(*searchChannel, local,
                      this->twitchChannel_->getName() % u" (Follower)");
        }

        for (const auto &[_id, set] :
             **getApp()->getAccounts()->twitch.getCurrent()->accessEmoteSets())
        {
            auto filtered = filterEmoteMap(searchText, set.emotes);
            if (!filtered.empty())
            {
                addEmotes(*searchChannel, std::move(filtered), set.title());
            }
        }
    }
    if (this->kickChannel_)
    {
        auto globalEmotes = filterEmoteMap(
            searchText, *getApp()->getKickChatServer()->globalEmotes());
        if (!globalEmotes.empty())
        {
            addEmotes(*searchChannel, std::move(globalEmotes), "Kick");
        }
    }

    auto bttvGlobalEmotes =
        filterEmoteMap(searchText, *getApp()->getBttvEmotes()->emotes());
    auto ffzGlobalEmotes =
        filterEmoteMap(searchText, *getApp()->getFfzEmotes()->emotes());
    auto seventvGlobalEmotes = filterEmoteMap(
        searchText, *getApp()->getSeventvEmotes()->globalEmotes());

    if (!bttvGlobalEmotes.empty())
    {
        addEmotes(*searchChannel, bttvGlobalEmotes, "BetterTTV (Global)");
    }
    if (!ffzGlobalEmotes.empty())
    {
        addEmotes(*searchChannel, ffzGlobalEmotes, "FrankerFaceZ (Global)");
    }
    if (!seventvGlobalEmotes.empty())
    {
        addEmotes(*searchChannel, seventvGlobalEmotes, "7TV (Global)");
    }

    if (this->kickChannel_)
    {
        auto seventvChannelEmotes =
            filterEmoteMap(searchText, *this->kickChannel_->seventvEmotes());

        if (!seventvChannelEmotes.empty())
        {
            addEmotes(*searchChannel, seventvChannelEmotes, "7TV (Channel)");
        }

        const auto personalEmotes =
            getApp()->getSeventvPersonalEmotes()->getEmoteSetsForKickUser(
                getApp()->getAccounts()->kick.current()->userID());
        for (const auto &map : personalEmotes)
        {
            auto seventvPersonalEmotes = filterEmoteMap(searchText, *map);
            if (!seventvPersonalEmotes.empty())
            {
                addEmotes(*searchChannel, seventvPersonalEmotes,
                          "SevenTV (Personal)");
            }
        }
    }

    if (this->twitchChannel_ == nullptr)
    {
        return;
    }

    auto bttvChannelEmotes =
        filterEmoteMap(searchText, *this->twitchChannel_->bttvEmotes());
    auto ffzChannelEmotes =
        filterEmoteMap(searchText, *this->twitchChannel_->ffzEmotes());
    auto seventvChannelEmotes =
        filterEmoteMap(searchText, *this->twitchChannel_->seventvEmotes());

    if (!bttvChannelEmotes.empty())
    {
        addEmotes(*searchChannel, bttvChannelEmotes, "BetterTTV (Channel)");
    }
    if (!ffzChannelEmotes.empty())
    {
        addEmotes(*searchChannel, ffzChannelEmotes, "FrankerFaceZ (Channel)");
    }
    if (!seventvChannelEmotes.empty())
    {
        addEmotes(*searchChannel, seventvChannelEmotes, "7TV (Channel)");
    }

    for (const auto &map :
         getApp()->getSeventvPersonalEmotes()->getEmoteSetsForTwitchUser(
             getApp()->getAccounts()->twitch.getCurrent()->getUserId()))
    {
        auto seventvPersonalEmotes = filterEmoteMap(searchText, *map);
        if (!seventvPersonalEmotes.empty())
        {
            addEmotes(*searchChannel, seventvPersonalEmotes,
                      "SevenTV (Personal)");
        }
    }
}

void EmotePopup::filterEmotes(const QString &searchText)
{
    this->searchDebounce_.stop();

    if (searchText.length() == 0)
    {
        this->notebook_->show();
        this->searchView_->hide();
        this->loadTabIfNeeded(this->notebook_->getSelectedPage());

        return;
    }
    auto searchChannel = this->searchView_->underlyingChannel();
    searchChannel->clearMessages();

    if (this->youtubeChannel_)
    {
        auto youtubeEmotes =
            filterEmoteMap(searchText, *this->youtubeChannel_->youtubeEmotes());
        if (!youtubeEmotes.empty())
        {
            addEmotes(*searchChannel, std::move(youtubeEmotes),
                      "YouTube Emoji");
        }
    }
    else if (this->channel_ && this->channel_->isTikTokChannel())
    {
        auto emotes = filterEmoteMap(searchText, *tikTokBuiltinEmotes());
        if (!emotes.empty())
        {
            addEmotes(*searchChannel, std::move(emotes), "TikTok Emoji");
        }
    }
    else if (this->channel_ && this->channel_->isTwitchOrKickChannel())
    {
        this->filterTwitchEmotes(searchChannel, searchText);
    }

    std::vector<EmojiPtr> filteredEmojis{};
    const auto &emojis = getApp()->getEmotes()->getEmojis()->getEmojis();
    for (const auto &emoji : emojis)
    {
        if (emoji->shortCodes[0].contains(searchText, Qt::CaseInsensitive))
        {
            filteredEmojis.push_back(emoji);
        }
    }
    if (!filteredEmojis.empty())
    {
        loadEmojis(*searchChannel, filteredEmojis, "Emojis");
    }

    this->notebook_->hide();
    this->searchView_->show();
}

std::optional<EmotePtr> EmotePopup::findEmote(const EmoteName &name)
{
    if (this->channel_ && this->channel_->isTikTokChannel())
    {
        return findEmoteByName(name, *tikTokBuiltinEmotes());
    }
    if (this->youtubeChannel_)
    {
        return findEmoteByName(name, *this->youtubeChannel_->youtubeEmotes());
    }

    if (this->twitchChannel_)
    {
        auto emotesToTry = this->twitchChannel_->localTwitchEmotes();

        auto emote = findEmoteByName(name, *emotesToTry);
        if (emote)
        {
            return emote;
        }

        auto twitchEmotes =
            *getApp()->getAccounts()->twitch.getCurrent()->accessEmoteSets();
        auto currentChannelID = this->twitchChannel_->roomId();
        auto currentChannelIt = std::ranges::find_if(
            *twitchEmotes, [currentChannelID](const auto &it) {
                return it.second.owner &&
                       it.second.owner->id == currentChannelID;
            });
        if (currentChannelIt != twitchEmotes->end())
        {
            const auto &emoteSet = currentChannelIt->second;
            auto setEmote = findEmoteByName(name, emoteSet.emotes);
            if (setEmote)
            {
                return setEmote;
            }
        }

        for (const auto &[setId, emoteSet] : *twitchEmotes)
        {
            auto setEmote = findEmoteByName(name, emoteSet.emotes);
            if (setEmote)
            {
                return setEmote;
            }
        }

        emote = this->twitchChannel_->ffzEmote(name);
        if (emote)
        {
            return emote;
        }

        emote = this->twitchChannel_->bttvEmote(name);
        if (emote)
        {
            return emote;
        }

        emote = this->twitchChannel_->seventvEmote(name);
        if (emote)
        {
            return emote;
        }
    }

    if (this->kickChannel_)
    {
        if (auto emote = this->kickChannel_->seventvEmote(name))
        {
            return emote;
        }
        if (auto emote = findEmoteByName(
                name, *getApp()->getKickChatServer()->globalEmotes()))
        {
            return emote;
        }
    }
    const auto personal = this->kickChannel_
        ? getApp()->getSeventvPersonalEmotes()->getEmoteSetsForKickUser(
              getApp()->getAccounts()->kick.current()->userID())
        : getApp()->getSeventvPersonalEmotes()->getEmoteSetsForTwitchUser(
              getApp()->getAccounts()->twitch.getCurrent()->getUserId());
    for (const auto &map : personal)
    {
        if (auto emote = findEmoteByName(name, *map))
        {
            return emote;
        }
    }

    auto emote = getApp()->getFfzEmotes()->emote(name);
    if (emote)
    {
        return emote;
    }

    emote = getApp()->getBttvEmotes()->emote(name);
    if (emote)
    {
        return emote;
    }

    emote = getApp()->getSeventvEmotes()->globalEmote(name);
    if (emote)
    {
        return emote;
    }

    return std::nullopt;
}

void EmotePopup::saveBounds() const
{
    if (isAppAboutToQuit())
    {
        return;
    }

    auto bounds = this->getBounds();
    if (!bounds.isNull())
    {
        getApp()->getWindows()->setEmotePopupBounds(bounds);
    }
}

void EmotePopup::resizeEvent(QResizeEvent *event)
{
    this->saveBounds();
    BasePopup::resizeEvent(event);
}

void EmotePopup::moveEvent(QMoveEvent *event)
{
    this->saveBounds();
    BasePopup::moveEvent(event);
}

void EmotePopup::themeChangedEvent()
{
    BasePopup::themeChangedEvent();

    this->setPalette(getTheme()->palette);
}

void EmotePopup::showEvent(QShowEvent *event)
{
    BasePopup::showEvent(event);
    if (this->search_->text().isEmpty())
    {
        this->loadTabIfNeeded(this->notebook_->getSelectedPage());
    }
    else
    {
        this->filterEmotes(this->search_->text());
    }
}

void EmotePopup::hideEvent(QHideEvent *event)
{
    this->searchDebounce_.stop();
    BasePopup::hideEvent(event);
    this->releaseLoadedEmotes();
}

void EmotePopup::closeEvent(QCloseEvent *event)
{
    this->saveBounds();
    BasePopup::closeEvent(event);
    this->releaseLoadedEmotes();
}

}
