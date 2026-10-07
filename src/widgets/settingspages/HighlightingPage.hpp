// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/settingspages/SettingsPage.hpp"

#include <QAbstractTableModel>
#include <QString>
#include <QTimer>

class QPushButton;
class QTreeWidget;

namespace chatterino {

class EditableModelView;

class HighlightingPage : public SettingsPage
{
public:
    HighlightingPage();

private:
    enum HighlightTab { Messages = 0, Users = 1, Badges = 2, Blacklist = 3 };

    QTimer disabledUsersChangedTimer_;
    QTreeWidget *wordLists_{};
    bool refreshingWordLists_{};

    void tableCellClicked(const QModelIndex &clicked, EditableModelView *view,
                          HighlightTab tab);
    void openSoundDialog(const QModelIndex &clicked, EditableModelView *view,
                         int soundColumn);
    void openColorDialog(const QModelIndex &clicked, EditableModelView *view,
                         HighlightTab tab);
    void openMatchAppearanceDialog(const QModelIndex &clicked,
                                   EditableModelView *view);
    void openChannelScopeDialog(const QModelIndex &clicked,
                                EditableModelView *view);
    void addChannelScopeButton(EditableModelView *view);
    void refreshWordLists();
    void createWordList();
    void importWordList();
    void importChattyHighlights(QString source = {},
                                QString suggestedName = {});
    void editSelectedWordList();
    void exportSelectedWordList();
    void removeSelectedWordList();
    int selectedWordListIndex() const;
};

}
