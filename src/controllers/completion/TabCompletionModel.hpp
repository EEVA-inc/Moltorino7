// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/completion/sources/Source.hpp"

#include <QObject>
#include <QString>
#include <QStringListModel>

#include <optional>
#include <vector>

namespace chatterino {

class Channel;
namespace completion {
struct EmoteItem;
}

class TabCompletionModel : public QStringListModel
{
public:

    explicit TabCompletionModel(Channel &channel, QObject *parent);

    void updateResults(const QString &query, const QString &fullTextContent,
                       int cursorPosition, bool isFirstWord = false);

    void clearResults();
    const completion::EmoteItem *emoteAt(int row) const;

private:
    enum class SourceKind {

        Emote,

        User,

        Command,

        EmoteUser,

        EmoteCommand,

        EmoteUserCommand
    };

    void updateSourceFromQuery(const QString &query, bool isFirstWord);

    std::optional<SourceKind> deduceSourceKind(const QString &query,
                                               bool isFirstWord) const;

    std::unique_ptr<completion::Source> buildSource(SourceKind kind) const;

    std::unique_ptr<completion::Source> buildEmoteSource() const;
    std::unique_ptr<completion::Source> buildUserSource(bool prependAt) const;
    std::unique_ptr<completion::Source> buildCommandSource(
        bool explicitCommand = false) const;

    Channel &channel_;
    std::unique_ptr<completion::Source> source_{};
    std::vector<qsizetype> sourceRows_;
};

}
