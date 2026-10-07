// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/ignores/HiddenUser.hpp"
#include "widgets/settingspages/SettingsPage.hpp"

#include <QStringListModel>

#include <vector>

class QVBoxLayout;
class QListView;

namespace chatterino {

class IgnoresPage : public SettingsPage
{
public:
    IgnoresPage();

    void onShow() final;

private:
    QStringListModel userListModel_;
    QStringListModel hiddenUserListModel_;
    QListView *hiddenUserList_{};
    std::vector<HiddenUser> hiddenUsers_;
};

}
