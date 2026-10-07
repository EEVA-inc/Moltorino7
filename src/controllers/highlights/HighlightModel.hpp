// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/SignalVectorModel.hpp"
#include "controllers/highlights/HighlightChannelScope.hpp"

#include <QObject>
#include <QStringList>
#include <QVariant>

namespace chatterino {

class HighlightPhrase;

class HighlightModel : public SignalVectorModel<HighlightPhrase>
{
public:
    explicit HighlightModel(QObject *parent);

    static constexpr int MatchStyleRole = Qt::UserRole;
    static constexpr int MatchPaintIDRole = Qt::UserRole + 1;
    static constexpr int ChannelScopeRole = Qt::UserRole + 2;
    static constexpr int MatchPaintAllowedRole = Qt::UserRole + 3;

    enum Column {
        Pattern = 0,
        ShowInMentions = 1,
        FlashTaskbar = 2,
        UseRegex = 3,
        CaseSensitive = 4,
        PlaySound = 5,
        SoundPath = 6,
        Color = 7,
        MatchAppearance = 8,
        ChannelScope = 9,
        COUNT
    };

    static void setChannelScopeItem(QStandardItem *item,
                                    const HighlightChannelScope &scope,
                                    bool enabled = true);
    static QStringList channelScopeData(const HighlightChannelScope &scope);
    static HighlightChannelScope channelScopeFromData(const QVariant &data);
    static HighlightChannelScope channelScopeFromItem(
        const QStandardItem *item);

    enum HighlightRowIndexes {
        SelfHighlightRow = 0,
        WhisperRow = 1,
        SubRow = 2,
        RedeemedRow = 3,
        FirstMessageRow = 4,
        ElevatedMessageRow = 5,
        ThreadMessageRow = 6,
        AutomodRow = 7,
        WatchStreakRow = 8,
        AnnouncementRow = 9,
        ColoredAnnouncementRow = 10,
    };

    enum UserHighlightRowIndexes {
        SelfMessageRow = 0,
    };

protected:

    HighlightPhrase getItemFromRow(std::vector<QStandardItem *> &row,
                                   const HighlightPhrase &original) override;

    void getRowFromItem(const HighlightPhrase &item,
                        std::vector<QStandardItem *> &row) override;

    void afterInit() override;

    void customRowSetData(const std::vector<QStandardItem *> &row, int column,
                          const QVariant &value, int role,
                          int rowIndex) override;
};

}
