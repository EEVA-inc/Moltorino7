// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/highlights/HighlightModel.hpp"

#include "Application.hpp"
#include "common/SignalVectorModel.hpp"
#include "controllers/highlights/HighlightPhrase.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/StandardItemHelper.hpp"

#include <QStringList>

#include <algorithm>

namespace chatterino {

namespace {

void setMatchAppearanceItem(QStandardItem *item, const QColor &color,
                            HighlightMatchStyle style, const QString &paintID,
                            bool enabled = true, bool allowPaint = true)
{
    item->setCheckable(false);
    item->setData(QVariant{}, Qt::CheckStateRole);
    if (!enabled)
    {
        item->setData(QVariant{}, Qt::DisplayRole);
        item->setData(QVariant{}, Qt::DecorationRole);
        item->setToolTip("Not applicable to this highlight.");
        item->setData(QVariant{}, HighlightModel::MatchStyleRole);
        item->setData(QVariant{}, HighlightModel::MatchPaintIDRole);
        item->setFlags({});
        return;
    }

    item->setData(highlightMatchAppearanceName(style, !paintID.isEmpty()),
                  Qt::DisplayRole);
    item->setData(color, Qt::DecorationRole);
    item->setData(static_cast<int>(style), HighlightModel::MatchStyleRole);
    item->setData(paintID, HighlightModel::MatchPaintIDRole);
    item->setData(allowPaint, HighlightModel::MatchPaintAllowedRole);
    item->setToolTip(highlightMatchAppearanceTooltip(color, style, paintID));
    item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
}

}

HighlightModel::HighlightModel(QObject *parent)
    : SignalVectorModel<HighlightPhrase>(Column::COUNT, parent)
{
}

void HighlightModel::setChannelScopeItem(QStandardItem *item,
                                         const HighlightChannelScope &scope,
                                         bool enabled)
{
    item->setData(channelScopeData(scope), ChannelScopeRole);
    if (!enabled)
    {
        item->setData(QVariant{}, Qt::DisplayRole);
        item->setData(QVariant{}, Qt::ToolTipRole);
        item->setFlags({});
        return;
    }

    item->setData(
        highlightChannelScopeSummary(scope.mode(), scope.targets().size()),
        Qt::DisplayRole);
    item->setData(
        highlightChannelScopeDescription(scope.mode(), scope.targets()),
        Qt::ToolTipRole);
    item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
}

QStringList HighlightModel::channelScopeData(const HighlightChannelScope &scope)
{
    QStringList encoded{highlightChannelScopeModeKey(scope.mode())};
    for (const auto &target : scope.targets())
    {
        encoded.append(encodeHighlightChannelTarget(target));
    }
    return encoded;
}

HighlightChannelScope HighlightModel::channelScopeFromData(const QVariant &data)
{
    const auto encoded = data.toStringList();
    if (encoded.isEmpty())
    {
        return {};
    }

    std::vector<HighlightChannelTarget> targets;
    targets.reserve(static_cast<std::size_t>(encoded.size() - 1));
    for (qsizetype index = 1; index < encoded.size(); ++index)
    {
        if (auto target = decodeHighlightChannelTarget(encoded.at(index)))
        {
            targets.emplace_back(std::move(*target));
        }
    }
    return {highlightChannelScopeModeFromKey(encoded.front()),
            std::move(targets)};
}

HighlightChannelScope HighlightModel::channelScopeFromItem(
    const QStandardItem *item)
{
    return channelScopeFromData(item->data(ChannelScopeRole));
}

HighlightPhrase HighlightModel::getItemFromRow(
    std::vector<QStandardItem *> &row, const HighlightPhrase &original)
{

    auto highlightColor = original.getColor();
    *highlightColor =
        row[Column::Color]->data(Qt::DecorationRole).value<QColor>();
    auto matchColor = original.getMatchColor();
    *matchColor =
        row[Column::MatchAppearance]->data(Qt::DecorationRole).value<QColor>();

    return HighlightPhrase{
        row[Column::Pattern]->data(Qt::DisplayRole).toString(),
        row[Column::ShowInMentions]->data(Qt::CheckStateRole).toBool(),
        row[Column::FlashTaskbar]->data(Qt::CheckStateRole).toBool(),
        row[Column::PlaySound]->data(Qt::CheckStateRole).toBool(),
        row[Column::UseRegex]->data(Qt::CheckStateRole).toBool(),
        row[Column::CaseSensitive]->data(Qt::CheckStateRole).toBool(),
        row[Column::SoundPath]->data(Qt::UserRole).toString(),
        highlightColor,
        matchColor,
        static_cast<HighlightMatchStyle>(
            row[Column::MatchAppearance]
                ->data(HighlightModel::MatchStyleRole)
                .toInt()),
        row[Column::MatchAppearance]
            ->data(HighlightModel::MatchPaintIDRole)
            .toString(),
        channelScopeFromItem(row[Column::ChannelScope])};
}

void HighlightModel::getRowFromItem(const HighlightPhrase &item,
                                    std::vector<QStandardItem *> &row)
{
    setStringItem(row[Column::Pattern], item.getPattern());
    setBoolItem(row[Column::ShowInMentions], item.showInMentions());
    setBoolItem(row[Column::FlashTaskbar], item.hasAlert());
    setBoolItem(row[Column::PlaySound], item.hasSound());
    setBoolItem(row[Column::UseRegex], item.isRegex());
    setBoolItem(row[Column::CaseSensitive], item.isCaseSensitive());
    setFilePathItem(row[Column::SoundPath], item.getSoundUrl());
    setColorItem(row[Column::Color], *item.getColor());
    setMatchAppearanceItem(row[Column::MatchAppearance], *item.getMatchColor(),
                           item.getMatchStyle(), item.getMatchPaintID());
    setChannelScopeItem(row[Column::ChannelScope], item.getChannelScope());
}

void HighlightModel::afterInit()
{
    const auto disableChannelScope = [](auto &row) {
        HighlightModel::setChannelScopeItem(row[Column::ChannelScope],
                                            HighlightChannelScope{}, false);
    };

    std::vector<QStandardItem *> usernameRow = this->createRow();
    setBoolItem(usernameRow[Column::Pattern],
                getSettings()->enableSelfHighlight.getValue(), true, false);
    usernameRow[Column::Pattern]->setData("Your username (automatic)",
                                          Qt::DisplayRole);
    setBoolItem(usernameRow[Column::ShowInMentions],
                getSettings()->showSelfHighlightInMentions.getValue(), true,
                false);
    setBoolItem(usernameRow[Column::FlashTaskbar],
                getSettings()->enableSelfHighlightTaskbar.getValue(), true,
                false);
    setBoolItem(usernameRow[Column::PlaySound],
                getSettings()->enableSelfHighlightSound.getValue(), true,
                false);
    usernameRow[Column::UseRegex]->setFlags({});
    usernameRow[Column::CaseSensitive]->setFlags({});

    QUrl selfSound = QUrl(getSettings()->selfHighlightSoundUrl.getValue());
    setFilePathItem(usernameRow[Column::SoundPath], selfSound, false);

    auto selfColor = ColorProvider::instance().color(ColorType::SelfHighlight);
    setColorItem(usernameRow[Column::Color], *selfColor, false);
    auto selfMatchColor =
        QColor(getSettings()->selfHighlightMatchColor.getValue());
    if (!selfMatchColor.isValid())
    {
        selfMatchColor = defaultNewHighlightMatchColor();
    }
    setMatchAppearanceItem(
        usernameRow[Column::MatchAppearance], selfMatchColor,
        highlightMatchStyleFromName(
            getSettings()->selfHighlightMatchStyle.getValue()),
        getSettings()->selfHighlightMatchPaintID.getValue());
    disableChannelScope(usernameRow);

    this->insertCustomRow(usernameRow, HighlightRowIndexes::SelfHighlightRow);

    std::vector<QStandardItem *> whisperRow = this->createRow();
    setBoolItem(whisperRow[Column::Pattern],
                getSettings()->enableWhisperHighlight.getValue(), true, false);
    whisperRow[Column::Pattern]->setData("Whispers", Qt::DisplayRole);
    whisperRow[Column::ShowInMentions]->setFlags({});
    setBoolItem(whisperRow[Column::FlashTaskbar],
                getSettings()->enableWhisperHighlightTaskbar.getValue(), true,
                false);
    setBoolItem(whisperRow[Column::PlaySound],
                getSettings()->enableWhisperHighlightSound.getValue(), true,
                false);
    whisperRow[Column::UseRegex]->setFlags({});
    whisperRow[Column::CaseSensitive]->setFlags({});

    QUrl whisperSound =
        QUrl(getSettings()->whisperHighlightSoundUrl.getValue());
    setFilePathItem(whisperRow[Column::SoundPath], whisperSound, false);

    auto whisperColor = ColorProvider::instance().color(ColorType::Whisper);
    setColorItem(whisperRow[Column::Color], *whisperColor, false);
    setMatchAppearanceItem(whisperRow[Column::MatchAppearance], *whisperColor,
                           HighlightMatchStyle::Outline, {}, false);
    disableChannelScope(whisperRow);

    this->insertCustomRow(whisperRow, HighlightRowIndexes::WhisperRow);

    std::vector<QStandardItem *> subRow = this->createRow();
    setBoolItem(subRow[Column::Pattern],
                getSettings()->enableSubHighlight.getValue(), true, false);
    subRow[Column::Pattern]->setData("Subscriptions", Qt::DisplayRole);
    subRow[Column::ShowInMentions]->setFlags({});
    setBoolItem(subRow[Column::FlashTaskbar],
                getSettings()->enableSubHighlightTaskbar.getValue(), true,
                false);
    setBoolItem(subRow[Column::PlaySound],
                getSettings()->enableSubHighlightSound.getValue(), true, false);
    subRow[Column::UseRegex]->setFlags({});
    subRow[Column::CaseSensitive]->setFlags({});

    QUrl subSound = QUrl(getSettings()->subHighlightSoundUrl.getValue());
    setFilePathItem(subRow[Column::SoundPath], subSound, false);

    auto subColor = ColorProvider::instance().color(ColorType::Subscription);
    setColorItem(subRow[Column::Color], *subColor, false);
    setMatchAppearanceItem(subRow[Column::MatchAppearance], *subColor,
                           HighlightMatchStyle::Outline, {}, false);
    disableChannelScope(subRow);

    this->insertCustomRow(subRow, HighlightRowIndexes::SubRow);

    std::vector<QStandardItem *> redeemedRow = this->createRow();
    setBoolItem(redeemedRow[Column::Pattern],
                getSettings()->enableRedeemedHighlight.getValue(), true, false);
    redeemedRow[Column::Pattern]->setData(
        "Highlights redeemed with Channel Points", Qt::DisplayRole);
    redeemedRow[Column::ShowInMentions]->setFlags({});

    redeemedRow[Column::FlashTaskbar]->setFlags({});
    redeemedRow[Column::PlaySound]->setFlags({});
    redeemedRow[Column::UseRegex]->setFlags({});
    redeemedRow[Column::CaseSensitive]->setFlags({});
    redeemedRow[Column::SoundPath]->setFlags(Qt::NoItemFlags);

    auto RedeemedColor =
        ColorProvider::instance().color(ColorType::RedeemedHighlight);
    setColorItem(redeemedRow[Column::Color], *RedeemedColor, false);
    setMatchAppearanceItem(redeemedRow[Column::MatchAppearance], *RedeemedColor,
                           HighlightMatchStyle::Outline, {}, false);
    disableChannelScope(redeemedRow);

    this->insertCustomRow(redeemedRow, HighlightRowIndexes::RedeemedRow);

    std::vector<QStandardItem *> firstMessageRow = this->createRow();
    setBoolItem(firstMessageRow[Column::Pattern],
                getSettings()->enableFirstMessageHighlight.getValue(), true,
                false);
    firstMessageRow[Column::Pattern]->setData("First Messages",
                                              Qt::DisplayRole);
    firstMessageRow[Column::ShowInMentions]->setFlags({});

    firstMessageRow[Column::FlashTaskbar]->setFlags({});
    firstMessageRow[Column::PlaySound]->setFlags({});
    firstMessageRow[Column::UseRegex]->setFlags({});
    firstMessageRow[Column::CaseSensitive]->setFlags({});
    firstMessageRow[Column::SoundPath]->setFlags(Qt::NoItemFlags);

    auto FirstMessageColor =
        ColorProvider::instance().color(ColorType::FirstMessageHighlight);
    setColorItem(firstMessageRow[Column::Color], *FirstMessageColor, false);
    setMatchAppearanceItem(firstMessageRow[Column::MatchAppearance],
                           *FirstMessageColor, HighlightMatchStyle::Outline, {},
                           false);
    disableChannelScope(firstMessageRow);

    this->insertCustomRow(firstMessageRow,
                          HighlightRowIndexes::FirstMessageRow);

    std::vector<QStandardItem *> elevatedMessageRow = this->createRow();
    setBoolItem(elevatedMessageRow[Column::Pattern],
                getSettings()->enableElevatedMessageHighlight.getValue(), true,
                false);
    elevatedMessageRow[Column::Pattern]->setData("Hype Chats", Qt::DisplayRole);
    elevatedMessageRow[Column::ShowInMentions]->setFlags({});

    elevatedMessageRow[Column::FlashTaskbar]->setFlags({});
    elevatedMessageRow[Column::PlaySound]->setFlags({});
    elevatedMessageRow[Column::UseRegex]->setFlags({});
    elevatedMessageRow[Column::CaseSensitive]->setFlags({});
    elevatedMessageRow[Column::SoundPath]->setFlags(Qt::NoItemFlags);

    auto elevatedMessageColor =
        ColorProvider::instance().color(ColorType::ElevatedMessageHighlight);
    setColorItem(elevatedMessageRow[Column::Color], *elevatedMessageColor,
                 false);
    setMatchAppearanceItem(elevatedMessageRow[Column::MatchAppearance],
                           *elevatedMessageColor, HighlightMatchStyle::Outline,
                           {}, false);
    disableChannelScope(elevatedMessageRow);

    this->insertCustomRow(elevatedMessageRow,
                          HighlightRowIndexes::ElevatedMessageRow);

    std::vector<QStandardItem *> threadMessageRow = this->createRow();
    setBoolItem(threadMessageRow[Column::Pattern],
                getSettings()->enableThreadHighlight.getValue(), true, false);
    threadMessageRow[Column::Pattern]->setData("Subscribed Reply Threads",
                                               Qt::DisplayRole);
    setBoolItem(threadMessageRow[Column::ShowInMentions],
                getSettings()->showThreadHighlightInMentions.getValue(), true,
                false);
    setBoolItem(threadMessageRow[Column::FlashTaskbar],
                getSettings()->enableThreadHighlightTaskbar.getValue(), true,
                false);
    setBoolItem(threadMessageRow[Column::PlaySound],
                getSettings()->enableThreadHighlightSound.getValue(), true,
                false);
    threadMessageRow[Column::UseRegex]->setFlags({});
    threadMessageRow[Column::CaseSensitive]->setFlags({});

    QUrl threadMessageSound =
        QUrl(getSettings()->threadHighlightSoundUrl.getValue());
    setFilePathItem(threadMessageRow[Column::SoundPath], threadMessageSound,
                    false);

    auto threadMessageColor =
        ColorProvider::instance().color(ColorType::ThreadMessageHighlight);
    setColorItem(threadMessageRow[Column::Color], *threadMessageColor, false);
    setMatchAppearanceItem(threadMessageRow[Column::MatchAppearance],
                           *threadMessageColor, HighlightMatchStyle::Outline,
                           {}, false);
    disableChannelScope(threadMessageRow);

    this->insertCustomRow(threadMessageRow,
                          HighlightRowIndexes::ThreadMessageRow);

    const std::vector<QStandardItem *> automodRow = this->createRow();
    setBoolItem(automodRow[Column::Pattern],
                getSettings()->enableAutomodHighlight.getValue(), true, false);
    setBoolItem(automodRow[Column::ShowInMentions],
                getSettings()->showAutomodInMentions.getValue(), true, false);
    automodRow[Column::Pattern]->setData("AutoMod Caught Messages",
                                         Qt::DisplayRole);
    setBoolItem(automodRow[Column::FlashTaskbar],
                getSettings()->enableAutomodHighlightTaskbar.getValue(), true,
                false);
    setBoolItem(automodRow[Column::PlaySound],
                getSettings()->enableAutomodHighlightSound.getValue(), true,
                false);
    automodRow[Column::UseRegex]->setFlags({});
    automodRow[Column::CaseSensitive]->setFlags({});

    const auto automodSound =
        QUrl(getSettings()->automodHighlightSoundUrl.getValue());
    setFilePathItem(automodRow[Column::SoundPath], automodSound, false);
    auto automodColor =
        ColorProvider::instance().color(ColorType::AutomodHighlight);
    setColorItem(automodRow[Column::Color], *automodColor, false);
    auto automodMatchColor =
        QColor(getSettings()->automodMatchHighlightColor.getValue());
    if (!automodMatchColor.isValid())
    {
        automodMatchColor = defaultAutoModMatchColor();
    }
    const auto automodMatchStyleValue =
        getSettings()->automodMatchHighlightStyle.getValue();
    const auto automodMatchStyle =
        automodMatchStyleValue >= static_cast<int>(HighlightMatchStyle::None) &&
                automodMatchStyleValue <=
                    static_cast<int>(HighlightMatchStyle::Underline)
            ? static_cast<HighlightMatchStyle>(automodMatchStyleValue)
            : HighlightMatchStyle::Fill;
    setMatchAppearanceItem(automodRow[Column::MatchAppearance],
                           automodMatchColor, automodMatchStyle, {}, true,
                           false);
    disableChannelScope(automodRow);

    this->insertCustomRow(automodRow, HighlightRowIndexes::AutomodRow);

    std::vector<QStandardItem *> watchStreakRow = this->createRow();
    setBoolItem(watchStreakRow[Column::Pattern],
                getSettings()->enableWatchStreakHighlight.getValue(), true,
                false);
    watchStreakRow[Column::Pattern]->setData("Watch Streaks", Qt::DisplayRole);
    watchStreakRow[Column::ShowInMentions]->setFlags({});
    watchStreakRow[Column::FlashTaskbar]->setFlags({});
    watchStreakRow[Column::PlaySound]->setFlags({});
    watchStreakRow[Column::UseRegex]->setFlags({});
    watchStreakRow[Column::CaseSensitive]->setFlags({});
    watchStreakRow[Column::SoundPath]->setFlags(Qt::NoItemFlags);

    auto watchStreakColor =
        ColorProvider::instance().color(ColorType::WatchStreak);
    setColorItem(watchStreakRow[Column::Color], *watchStreakColor, false);
    setMatchAppearanceItem(watchStreakRow[Column::MatchAppearance],
                           *watchStreakColor, HighlightMatchStyle::Outline, {},
                           false);
    disableChannelScope(watchStreakRow);

    this->insertCustomRow(watchStreakRow, HighlightRowIndexes::WatchStreakRow);

    std::vector<QStandardItem *> announcementRow = this->createRow();
    setBoolItem(announcementRow[Column::Pattern],
                getSettings()->enableAnnouncementHighlight.getValue(), true,
                false);
    announcementRow[Column::Pattern]->setData("Announcements", Qt::DisplayRole);
    announcementRow[Column::ShowInMentions]->setFlags({});
    announcementRow[Column::FlashTaskbar]->setFlags({});
    announcementRow[Column::PlaySound]->setFlags({});
    announcementRow[Column::UseRegex]->setFlags({});
    announcementRow[Column::CaseSensitive]->setFlags({});
    announcementRow[Column::SoundPath]->setFlags(Qt::NoItemFlags);

    auto announcementColor =
        ColorProvider::instance().color(ColorType::AnnouncementHighlight);
    setColorItem(announcementRow[Column::Color], *announcementColor, false);
    setMatchAppearanceItem(announcementRow[Column::MatchAppearance],
                           *announcementColor, HighlightMatchStyle::Outline, {},
                           false);
    disableChannelScope(announcementRow);

    this->insertCustomRow(announcementRow,
                          HighlightRowIndexes::AnnouncementRow);

    std::vector<QStandardItem *> coloredAnnouncementRow = this->createRow();
    setBoolItem(coloredAnnouncementRow[Column::Pattern],
                getSettings()->enableColoredAnnouncementHighlight.getValue(),
                true, false);
    coloredAnnouncementRow[Column::Pattern]->setData("Colored Announcements",
                                                     Qt::DisplayRole);
    coloredAnnouncementRow[Column::ShowInMentions]->setFlags({});
    coloredAnnouncementRow[Column::FlashTaskbar]->setFlags({});
    coloredAnnouncementRow[Column::PlaySound]->setFlags({});
    coloredAnnouncementRow[Column::UseRegex]->setFlags({});
    coloredAnnouncementRow[Column::CaseSensitive]->setFlags({});
    coloredAnnouncementRow[Column::SoundPath]->setFlags(Qt::NoItemFlags);
    coloredAnnouncementRow[Column::Color]->setFlags(Qt::NoItemFlags);
    coloredAnnouncementRow[Column::MatchAppearance]->setFlags(Qt::NoItemFlags);
    disableChannelScope(coloredAnnouncementRow);

    this->insertCustomRow(coloredAnnouncementRow,
                          HighlightRowIndexes::ColoredAnnouncementRow);
}

void HighlightModel::customRowSetData(const std::vector<QStandardItem *> &row,
                                      int column, const QVariant &value,
                                      int role, int rowIndex)
{
    switch (column)
    {
        case Column::Pattern: {
            if (role == Qt::CheckStateRole)
            {
                if (rowIndex == HighlightRowIndexes::SelfHighlightRow)
                {
                    getSettings()->enableSelfHighlight.setValue(value.toBool());
                }
                else if (rowIndex == HighlightRowIndexes::WhisperRow)
                {
                    getSettings()->enableWhisperHighlight.setValue(
                        value.toBool());
                }
                else if (rowIndex == HighlightRowIndexes::SubRow)
                {
                    getSettings()->enableSubHighlight.setValue(value.toBool());
                }
                else if (rowIndex == HighlightRowIndexes::WatchStreakRow)
                {
                    getSettings()->enableWatchStreakHighlight.setValue(
                        value.toBool());
                }
                else if (rowIndex == HighlightRowIndexes::RedeemedRow)
                {
                    getSettings()->enableRedeemedHighlight.setValue(
                        value.toBool());
                }
                else if (rowIndex == HighlightRowIndexes::FirstMessageRow)
                {
                    getSettings()->enableFirstMessageHighlight.setValue(
                        value.toBool());
                }
                else if (rowIndex == HighlightRowIndexes::ElevatedMessageRow)
                {
                    getSettings()->enableElevatedMessageHighlight.setValue(
                        value.toBool());
                }
                else if (rowIndex == HighlightRowIndexes::ThreadMessageRow)
                {
                    getSettings()->enableThreadHighlight.setValue(
                        value.toBool());
                }
                else if (rowIndex == HighlightRowIndexes::AutomodRow)
                {
                    getSettings()->enableAutomodHighlight.setValue(
                        value.toBool());
                }
                else if (rowIndex == HighlightRowIndexes::AnnouncementRow)
                {
                    getSettings()->enableAnnouncementHighlight.setValue(
                        value.toBool());
                }
                else if (rowIndex ==
                         HighlightRowIndexes::ColoredAnnouncementRow)
                {
                    getSettings()->enableColoredAnnouncementHighlight.setValue(
                        value.toBool());
                }
            }
        }
        break;
        case Column::ShowInMentions: {
            if (role == Qt::CheckStateRole)
            {
                if (rowIndex == HighlightRowIndexes::SelfHighlightRow)
                {
                    getSettings()->showSelfHighlightInMentions.setValue(
                        value.toBool());
                }
                else if (rowIndex == HighlightRowIndexes::ThreadMessageRow)
                {
                    getSettings()->showThreadHighlightInMentions.setValue(
                        value.toBool());
                }
                else if (rowIndex == HighlightRowIndexes::AutomodRow)
                {
                    getSettings()->showAutomodInMentions.setValue(
                        value.toBool());
                }
            }
        }
        break;
        case Column::FlashTaskbar: {
            if (role == Qt::CheckStateRole)
            {
                if (rowIndex == HighlightRowIndexes::SelfHighlightRow)
                {
                    getSettings()->enableSelfHighlightTaskbar.setValue(
                        value.toBool());
                }
                else if (rowIndex == HighlightRowIndexes::WhisperRow)
                {
                    getSettings()->enableWhisperHighlightTaskbar.setValue(
                        value.toBool());
                }
                else if (rowIndex == HighlightRowIndexes::SubRow)
                {
                    getSettings()->enableSubHighlightTaskbar.setValue(
                        value.toBool());
                }
                else if (rowIndex == HighlightRowIndexes::RedeemedRow)
                {

                }
                else if (rowIndex == HighlightRowIndexes::FirstMessageRow)
                {

                }
                else if (rowIndex == HighlightRowIndexes::ElevatedMessageRow)
                {

                }
                else if (rowIndex == HighlightRowIndexes::ThreadMessageRow)
                {
                    getSettings()->enableThreadHighlightTaskbar.setValue(
                        value.toBool());
                }
                else if (rowIndex == HighlightRowIndexes::AutomodRow)
                {
                    getSettings()->enableAutomodHighlightTaskbar.setValue(
                        value.toBool());
                }
            }
        }
        break;
        case Column::PlaySound: {
            if (role == Qt::CheckStateRole)
            {
                if (rowIndex == HighlightRowIndexes::SelfHighlightRow)
                {
                    getSettings()->enableSelfHighlightSound.setValue(
                        value.toBool());
                }
                else if (rowIndex == HighlightRowIndexes::WhisperRow)
                {
                    getSettings()->enableWhisperHighlightSound.setValue(
                        value.toBool());
                }
                else if (rowIndex == HighlightRowIndexes::SubRow)
                {
                    getSettings()->enableSubHighlightSound.setValue(
                        value.toBool());
                }
                else if (rowIndex == HighlightRowIndexes::RedeemedRow)
                {

                }
                else if (rowIndex == HighlightRowIndexes::FirstMessageRow)
                {

                }
                else if (rowIndex == HighlightRowIndexes::ElevatedMessageRow)
                {

                }
                else if (rowIndex == HighlightRowIndexes::ThreadMessageRow)
                {
                    getSettings()->enableThreadHighlightSound.setValue(
                        value.toBool());
                }
                else if (rowIndex == HighlightRowIndexes::AutomodRow)
                {
                    getSettings()->enableAutomodHighlightSound.setValue(
                        value.toBool());
                }
            }
        }
        break;
        case Column::UseRegex: {

        }
        break;
        case Column::CaseSensitive: {

        }
        break;
        case Column::SoundPath: {

            if (role == Qt::UserRole)
            {
                if (rowIndex == HighlightRowIndexes::SelfHighlightRow)
                {
                    getSettings()->selfHighlightSoundUrl.setValue(
                        value.toString());
                }
                else if (rowIndex == HighlightRowIndexes::WhisperRow)
                {
                    getSettings()->whisperHighlightSoundUrl.setValue(
                        value.toString());
                }
                else if (rowIndex == HighlightRowIndexes::SubRow)
                {
                    getSettings()->subHighlightSoundUrl.setValue(
                        value.toString());
                }
                else if (rowIndex == HighlightRowIndexes::ThreadMessageRow)
                {
                    getSettings()->threadHighlightSoundUrl.setValue(
                        value.toString());
                }
                else if (rowIndex == HighlightRowIndexes::AutomodRow)
                {
                    getSettings()->automodHighlightSoundUrl.setValue(
                        value.toString());
                }
            }
        }
        break;
        case Column::Color: {

            if (role == Qt::DecorationRole)
            {
                const auto setColor = [&](auto &setting, ColorType ty) {
                    auto color = value.value<QColor>();
                    setting.setValue(color.name(QColor::HexArgb));
                };

                if (rowIndex == HighlightRowIndexes::SelfHighlightRow)
                {
                    setColor(getSettings()->selfHighlightColor,
                             ColorType::SelfHighlight);
                }
                else if (rowIndex == HighlightRowIndexes::WhisperRow)
                {
                    setColor(getSettings()->whisperHighlightColor,
                             ColorType::Whisper);
                }
                else if (rowIndex == HighlightRowIndexes::SubRow)
                {
                    setColor(getSettings()->subHighlightColor,
                             ColorType::Subscription);
                }
                else if (rowIndex == HighlightRowIndexes::WatchStreakRow)
                {
                    setColor(getSettings()->watchStreakHighlightColor,
                             ColorType::WatchStreak);
                }
                else if (rowIndex == HighlightRowIndexes::RedeemedRow)
                {
                    setColor(getSettings()->redeemedHighlightColor,
                             ColorType::RedeemedHighlight);
                }
                else if (rowIndex == HighlightRowIndexes::FirstMessageRow)
                {
                    setColor(getSettings()->firstMessageHighlightColor,
                             ColorType::FirstMessageHighlight);
                }
                else if (rowIndex == HighlightRowIndexes::ElevatedMessageRow)
                {
                    setColor(getSettings()->elevatedMessageHighlightColor,
                             ColorType::ElevatedMessageHighlight);
                }
                else if (rowIndex == HighlightRowIndexes::ThreadMessageRow)
                {
                    setColor(getSettings()->threadHighlightColor,
                             ColorType::ThreadMessageHighlight);
                }
                else if (rowIndex == HighlightRowIndexes::AutomodRow)
                {
                    setColor(getSettings()->automodHighlightColor,
                             ColorType::AutomodHighlight);
                }
                else if (rowIndex == HighlightRowIndexes::AnnouncementRow)
                {
                    setColor(getSettings()->announcementHighlightColor,
                             ColorType::AnnouncementHighlight);
                }
            }
        }
        break;
        case Column::MatchAppearance: {
            if (rowIndex == HighlightRowIndexes::SelfHighlightRow)
            {
                if (role == Qt::DecorationRole)
                {
                    const auto color = value.value<QColor>();
                    if (color.isValid())
                    {
                        getSettings()->selfHighlightMatchColor.setValue(
                            color.name(QColor::HexArgb));
                    }
                }
                else if (role == MatchStyleRole)
                {
                    const auto style = value.toInt();
                    if (style >= static_cast<int>(HighlightMatchStyle::None) &&
                        style <=
                            static_cast<int>(HighlightMatchStyle::Underline))
                    {
                        getSettings()->selfHighlightMatchStyle.setValue(
                            highlightMatchStyleName(
                                static_cast<HighlightMatchStyle>(style)));
                    }
                }
                else if (role == MatchPaintIDRole)
                {
                    getSettings()->selfHighlightMatchPaintID.setValue(
                        value.toString().trimmed());
                }
                else
                {
                    return;
                }
                break;
            }
            if (rowIndex != HighlightRowIndexes::AutomodRow)
            {
                break;
            }

            if (role == Qt::DecorationRole)
            {
                const auto color = value.value<QColor>();
                if (color.isValid())
                {
                    getSettings()->automodMatchHighlightColor.setValue(
                        color.name(QColor::HexArgb));
                }
            }
            else if (role == MatchStyleRole)
            {
                const auto style = value.toInt();
                if (style >= static_cast<int>(HighlightMatchStyle::None) &&
                    style <= static_cast<int>(HighlightMatchStyle::Underline))
                {
                    getSettings()->automodMatchHighlightStyle.setValue(style);
                }
            }
            else
            {
                return;
            }
        }
        break;
    }

    getApp()->getWindows()->forceLayoutChannelViews();
}

}
