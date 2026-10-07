// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/SignalVectorModel.hpp"

#include <QObject>

#include <cstdint>

namespace chatterino {

class NotificationController;
enum class Platform : uint8_t;

class NotificationModel : public SignalVectorModel<QString>
{
    explicit NotificationModel(QObject *parent, Platform platform);
    Platform platform_;

protected:

    QString getItemFromRow(std::vector<QStandardItem *> &row,
                           const QString &original) override;

    void getRowFromItem(const QString &item,
                        std::vector<QStandardItem *> &row) override;

    friend class NotificationController;
};

}
