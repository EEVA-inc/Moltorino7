// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "buttons/SvgButton.hpp"
#include "common/ChatterinoSetting.hpp"
#include "widgets/BaseWindow.hpp"

#include <QPoint>
#include <QTimer>

#include <memory>

namespace chatterino {

class DraggablePopup : public BaseWindow
{
    Q_OBJECT

public:

    DraggablePopup(bool closeAutomatically, QWidget *parent);
    ~DraggablePopup() override;

protected:
    enum class ResizeMode { Setting, Always };

    void enableResize(QSizeSetting &setting, QSize defaultSize = {},
                      bool rememberSize = true,
                      ResizeMode resizeMode = ResizeMode::Setting);
    bool hasCustomSize() const;
    QSize preferredSize(QSize fallback) const;
    void applyPopupSize(QSize suggestedSize);
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void scaleChangedEvent(float scale) override;
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

    Button *createPinButton();

    std::shared_ptr<bool> lifetimeHack_;

    void togglePinned();

    bool ensurePinned();

private:
    bool resizeEnabled() const;
    void savePopupSize();
    void positionSizeGrip();
    QSizeSetting *sizeSetting_ = nullptr;
    QWidget *sizeGrip_ = nullptr;
    QSize customSize_;
    bool resizing_ = false;
    bool applyingSize_ = false;
    bool sizeDirty_ = false;
    QTimer saveSizeTimer_;

    bool isMoving_ = false;

    bool closeAutomatically_ = false;

    QPoint startPosDrag_;

    QPoint requestedDragPos_;

    QTimer dragTimer_;

    SvgButton *pinButton_{};
    SvgButton::Src pinDisabledSource_{
        .dark = ":/buttons/pinDisabled-darkMode.svg",
        .light = ":/buttons/pinDisabled-lightMode.svg",
    };
    SvgButton::Src pinEnabledSource_{
        .dark = ":/buttons/pinEnabled.svg",
        .light = ":/buttons/pinEnabled.svg",
    };
    bool isPinned_ = false;
    QSize defaultSize_;
    QSize suggestedSize_;
    bool rememberSize_ = true;
    ResizeMode resizeMode_ = ResizeMode::Setting;
    pajlada::Signals::SignalHolder resizeConnections_;
};

}
