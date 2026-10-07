// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/FontSettingWidget.hpp"

#include "Application.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "widgets/dialogs/font/FontSettingDialog.hpp"

#include <QFont>
#include <QHBoxLayout>
#include <QString>
#include <QToolButton>
#include <QVBoxLayout>

namespace chatterino {

FontSettingWidget::FontSettingWidget(QStringSetting &family, IntSetting &size,
                                     IntSetting &weight, QWidget *parent)
    : QWidget(parent)
    , familySetting(family)
    , sizeSetting(size)
    , weightSetting(weight)
    , currentLabel(new QLabel)
    , themeFontNotice(new QLabel("Theme font settings are active. Change them "
                                 "in Customization > Text."))
    , listener([this] {
        this->updateCurrentLabel();
    })
{
    auto *layout = new QHBoxLayout;
    auto *button = new QToolButton;

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addLayout(layout);
    this->themeFontNotice->setWordWrap(true);
    outer->addWidget(this->themeFontNotice);
    this->updateCurrentLabel();

    this->listener.add(getApp()->getFonts()->fontChanged);

    layout->addWidget(new QLabel("Font:"));
    layout->addStretch(1);
    layout->addWidget(this->currentLabel);
    layout->addWidget(button);
    layout->setContentsMargins(0, 0, 0, 0);

    button->setIcon(QIcon(":/buttons/edit.svg"));

    QObject::connect(button, &QToolButton::clicked, this,
                     &FontSettingWidget::showDialog);
}

void FontSettingWidget::updateCurrentLabel()
{
    QFont font = getApp()->getFonts()->getFont(FontStyle::ChatMedium, 1);
    QString family = font.family();
    QString ptSize = QString::number(font.pointSize());
    this->currentLabel->setText(family + ", " + ptSize + "pt");
    const auto &theme = getTheme()->customization;
    this->themeFontNotice->setVisible(
        !theme.chatFontFamily.isEmpty() ||
        !theme.usernameFontFamily.isEmpty() || theme.chatFontWeight > 0 ||
        theme.usernameFontWeight > 0 || theme.chatFontSize > 0 ||
        theme.usernameFontSize > 0);
}

void FontSettingWidget::showDialog()
{
    if (!this->dialog)
    {
        this->dialog = new FontSettingDialog(
            this->familySetting, this->sizeSetting, this->weightSetting, this);

        QObject::connect(this->dialog, &QObject::destroyed, [this] {
            this->dialog = nullptr;
        });
    }

    this->dialog->show();
}

}
