// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Emote.hpp"
#include "providers/emoji/Emojis.hpp"
#include "widgets/BasePopup.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <pajlada/signals/signal.hpp>
#include <QLineEdit>
#include <QTimer>

#include <optional>
#include <unordered_map>
#include <vector>

namespace chatterino {

struct Link;
class ChannelView;
class Channel;
using ChannelPtr = std::shared_ptr<Channel>;
class Notebook;
class TwitchChannel;
class KickChannel;
class YouTubeChannel;

class EmotePopup : public BasePopup
{
public:
    EmotePopup(QWidget *parent = nullptr);

    void loadChannel(ChannelPtr channel);

    void closeEvent(QCloseEvent *event) override;

    pajlada::Signals::Signal<Link> linkClicked;

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void themeChangedEvent() override;

private:
    ChannelView *globalEmotesView_{};
    ChannelView *channelEmotesView_{};
    ChannelView *subEmotesView_{};
    ChannelView *viewEmojis_{};
    ChannelView *favouritesView_{};

    ChannelView *searchView_{};

    ChannelPtr channel_;
    TwitchChannel *twitchChannel_{};
    KickChannel *kickChannel_{};
    YouTubeChannel *youtubeChannel_{};
    pajlada::Signals::ScopedConnection youtubeEmotesConnection_;

    QLineEdit *search_;
    QTimer searchDebounce_;
    Notebook *notebook_;

    void filterTwitchEmotes(std::shared_ptr<Channel> searchChannel,
                            const QString &searchText);
    void filterEmotes(const QString &text);
    void addShortcuts() override;
    bool eventFilter(QObject *object, QEvent *event) override;

    void reloadEmotes();
    void loadTabIfNeeded(QWidget *page);
    void populateChannelEmotes();
    void populateSubEmotes();
    void populateGlobalEmotes();
    void populateEmojis();
    void populateFavourites();
    void releaseLoadedEmotes();
    std::optional<EmotePtr> findEmote(const EmoteName &name);
    void addFavouriteEmoji(const QString &shortCode);
    void addFavouriteEmote(const EmoteName &name);
    void removeFavouriteEmoji(const QString &shortCode);
    void removeFavouriteEmote(const EmoteName &name);
    void updateFavouriteEmotesAndEmojis();

    bool channelEmotesLoaded_ = false;
    bool subEmotesLoaded_ = false;
    bool globalEmotesLoaded_ = false;
    bool emojisLoaded_ = false;
    bool favouritesLoaded_ = false;
    std::vector<EmotePtr> favouriteEmotes_;
    std::unordered_map<QString, EmojiPtr> favouriteEmojis_;

    void saveBounds() const;
};

}
