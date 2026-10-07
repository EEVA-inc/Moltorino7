// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWindow.hpp"

#include <pajlada/signals/signalholder.hpp>

class QPushButton;
class QProgressBar;
class QTextBrowser;

namespace chatterino {

class Label;

class UpdateDialog : public BaseWindow
{
public:
    UpdateDialog();

private:
    void refresh();

    struct {
        Label *heading = nullptr;
        Label *details = nullptr;
        Label *status = nullptr;
        QTextBrowser *summary = nullptr;
        QProgressBar *progress = nullptr;
        QPushButton *restartButton = nullptr;
        QPushButton *changelogButton = nullptr;
        QPushButton *retryButton = nullptr;
    } ui_;

    pajlada::Signals::SignalHolder connections_;
};

}
