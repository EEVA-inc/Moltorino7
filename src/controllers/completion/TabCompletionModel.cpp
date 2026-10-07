// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/completion/TabCompletionModel.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/completion/sources/CommandSource.hpp"
#include "controllers/completion/sources/EmoteSource.hpp"
#include "controllers/completion/sources/UnifiedSource.hpp"
#include "controllers/completion/sources/UserSource.hpp"
#include "controllers/completion/strategies/ClassicEmoteStrategy.hpp"
#include "controllers/completion/strategies/ClassicUserStrategy.hpp"
#include "controllers/completion/strategies/CommandStrategy.hpp"
#include "controllers/completion/strategies/SmartEmoteStrategy.hpp"
#include "controllers/plugins/LuaUtilities.hpp"
#include "controllers/plugins/Plugin.hpp"
#include "controllers/plugins/PluginController.hpp"
#include "singletons/Settings.hpp"

namespace chatterino {

TabCompletionModel::TabCompletionModel(Channel &channel, QObject *parent)
    : QStringListModel(parent)
    , channel_(channel)
{
}

void TabCompletionModel::updateResults(const QString &query,
                                       const QString &fullTextContent,
                                       int cursorPosition, bool isFirstWord)
{
    this->updateSourceFromQuery(query, isFirstWord);

    if (this->source_)
    {
        this->source_->update(query);

        QStringList results;
#ifdef CHATTERINO_HAVE_PLUGINS

        bool done{};
        std::tie(done, results) =
            getApp()->getPlugins()->updateCustomCompletions(
                query, fullTextContent, cursorPosition, isFirstWord);
        if (done)
        {
            auto uniqueResults = std::unique(results.begin(), results.end());
            results.erase(uniqueResults, results.end());
            this->sourceRows_.clear();
            this->setStringList(results);
            return;
        }
#endif
        const auto pluginResultCount = results.size();
        this->source_->addToStringList(results, 0, isFirstWord);

        QStringList uniqueResults;
        uniqueResults.reserve(results.size());
        this->sourceRows_.clear();
        this->sourceRows_.reserve(static_cast<size_t>(results.size()));
        for (qsizetype sourceRow = 0; sourceRow < results.size(); ++sourceRow)
        {
            if (!uniqueResults.isEmpty() &&
                uniqueResults.back() == results.at(sourceRow))
            {
                continue;
            }

            uniqueResults.push_back(results.at(sourceRow));
            this->sourceRows_.push_back(sourceRow < pluginResultCount
                                            ? -1
                                            : sourceRow - pluginResultCount);
        }
        this->setStringList(uniqueResults);
        return;
    }

    this->sourceRows_.clear();
    this->setStringList({});
}

void TabCompletionModel::clearResults()
{
    this->source_.reset();
    this->sourceRows_ = std::vector<qsizetype>{};
    if (this->rowCount() != 0)
    {
        this->setStringList({});
    }
}

const completion::EmoteItem *TabCompletionModel::emoteAt(int row) const
{
    if (this->source_ == nullptr || row < 0 ||
        static_cast<size_t>(row) >= this->sourceRows_.size())
    {
        return nullptr;
    }

    return this->source_->emoteAtTabCompletionIndex(
        this->sourceRows_[static_cast<size_t>(row)]);
}

void TabCompletionModel::updateSourceFromQuery(const QString &query,
                                               bool isFirstWord)
{
    auto deducedKind = this->deduceSourceKind(query, isFirstWord);
    if (!deducedKind)
    {

        this->source_ = nullptr;
        return;
    }

    this->source_ = this->buildSource(*deducedKind);
}

std::optional<TabCompletionModel::SourceKind>
    TabCompletionModel::deduceSourceKind(const QString &query,
                                         bool isFirstWord) const
{
    const bool commandPrefix =
        isFirstWord && (query.startsWith('/') || query.startsWith('.') ||
                        query.startsWith('#'));
    if ((query.length() < 2 && !commandPrefix) ||
        (!this->channel_.isTwitchOrKickChannel() &&
         !this->channel_.isYouTubeChannel() &&
         !this->channel_.isTikTokChannel()))
    {
        return std::nullopt;
    }

    if (query.startsWith('@'))
    {
        return SourceKind::User;
    }
    else if (query.startsWith(':'))
    {
        return SourceKind::Emote;
    }
    else if (commandPrefix)
    {
        return SourceKind::Command;
    }

    if (isFirstWord)
    {
        if (getSettings()->userCompletionOnlyWithAt)
        {

            return SourceKind::EmoteCommand;
        }

        return SourceKind::EmoteUserCommand;
    }

    if (getSettings()->userCompletionOnlyWithAt)
    {
        return SourceKind::Emote;
    }

    return SourceKind::EmoteUser;
}

std::unique_ptr<completion::Source> TabCompletionModel::buildSource(
    SourceKind kind) const
{
    switch (kind)
    {
        case SourceKind::Emote: {
            return this->buildEmoteSource();
        }
        case SourceKind::User: {
            return this->buildUserSource(true);
        }
        case SourceKind::Command: {
            return this->buildCommandSource(true);
        }
        case SourceKind::EmoteUser: {
            std::vector<std::unique_ptr<completion::Source>> sources;
            sources.push_back(this->buildEmoteSource());
            sources.push_back(this->buildUserSource(false));

            return std::make_unique<completion::UnifiedSource>(
                std::move(sources));
        }
        case SourceKind::EmoteCommand: {
            std::vector<std::unique_ptr<completion::Source>> sources;
            sources.push_back(this->buildEmoteSource());
            sources.push_back(this->buildCommandSource());

            return std::make_unique<completion::UnifiedSource>(
                std::move(sources));
        }
        case SourceKind::EmoteUserCommand: {
            std::vector<std::unique_ptr<completion::Source>> sources;
            sources.push_back(this->buildEmoteSource());
            sources.push_back(
                this->buildUserSource(false));
            sources.push_back(this->buildCommandSource());

            return std::make_unique<completion::UnifiedSource>(
                std::move(sources));
        }
        default:
            return nullptr;
    }
}

std::unique_ptr<completion::Source> TabCompletionModel::buildEmoteSource() const
{
    if (getSettings()->useSmartEmoteCompletion)
    {
        return std::make_unique<completion::EmoteSource>(
            &this->channel_,
            std::make_unique<completion::SmartTabEmoteStrategy>());
    }

    return std::make_unique<completion::EmoteSource>(
        &this->channel_,
        std::make_unique<completion::ClassicTabEmoteStrategy>());
}

std::unique_ptr<completion::Source> TabCompletionModel::buildUserSource(
    bool prependAt) const
{
    return std::make_unique<completion::UserSource>(
        &this->channel_, std::make_unique<completion::ClassicUserStrategy>(),
        nullptr, prependAt);
}

std::unique_ptr<completion::Source> TabCompletionModel::buildCommandSource(
    bool explicitCommand) const
{
    return std::make_unique<completion::CommandSource>(
        std::make_unique<completion::CommandStrategy>(!explicitCommand),
        nullptr, &this->channel_);
}

}
