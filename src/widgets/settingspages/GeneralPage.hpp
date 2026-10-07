// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/settingspages/SettingsPage.hpp"

class QLabel;
class QCheckBox;
class QComboBox;

namespace chatterino {

class GeneralPageView;

class GeneralPage : public SettingsPage
{
    Q_OBJECT

public:
    GeneralPage();

    bool filterElements(const QString &query) override;
    void onShow() override;

private:
    void initLayout(GeneralPageView &layout);

    GeneralPageView *view_{};
};

}
