// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/notifications/NotificationModel.hpp"

#include "Application.hpp"
#include "controllers/notifications/NotificationController.hpp"
#include "providers/youtube/YouTubeApi.hpp"
#include "singletons/Settings.hpp"
#include "util/StandardItemHelper.hpp"

namespace chatterino {

NotificationModel::NotificationModel(QObject *parent, Platform platform)
    : SignalVectorModel<QString>(1, parent)
    , platform_(platform)
{
}

QString NotificationModel::getItemFromRow(std::vector<QStandardItem *> &row,
                                          const QString &original)
{
    const auto name = row[0]->data(Qt::DisplayRole).toString();
    return this->platform_ == Platform::YouTube
               ? YouTubeApi::normalizeSource(name)
               : name;
}

void NotificationModel::getRowFromItem(const QString &item,
                                       std::vector<QStandardItem *> &row)
{
    setStringItem(row[0], item);
}

}
