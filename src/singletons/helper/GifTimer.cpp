// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/helper/GifTimer.hpp"

#include "Application.hpp"
#include "messages/Image.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"

#include <QApplication>

namespace chatterino {

void GIFTimer::initialize()
{
    this->timer.setInterval(GIF_FRAME_LENGTH);
    this->timer.setTimerType(Qt::PreciseTimer);

    getSettings()->animateEmotes.connect([this](bool enabled, auto) {
        if (enabled)
        {
            this->timer.start();
        }
        else
        {
            this->timer.stop();
        }

        if (auto *app = tryGetApp())
        {
            app->getWindows()->repaintTwitchGifs();
        }
    });

    getSettings()->animationsWhenFocused.connect([](bool, auto) {
        if (auto *app = tryGetApp())
        {
            app->getWindows()->repaintTwitchGifs();
        }
    });

    QObject::connect(qApp, &QGuiApplication::applicationStateChanged,
                     &this->timer,
                     [](Qt::ApplicationState) {
                         if (auto *app = tryGetApp())
                         {
                             app->getWindows()->repaintTwitchGifs();
                         }
                     });
    QObject::connect(qApp, &QApplication::focusChanged, &this->timer,
                     [](QWidget *, QWidget *) {
                         if (auto *app = tryGetApp())
                         {
                             app->getWindows()->repaintTwitchGifs();
                         }
                     });

    QObject::connect(&this->timer, &QTimer::timeout, [this] {
        if (!this->shouldAnimate())
        {
            return;
        }

        this->position_ += GIF_FRAME_LENGTH;
        if (Image::takeStreamingGifRepaint(this->position_))
        {
            getApp()->getWindows()->repaintTwitchGifs();
        }
        getApp()->getWindows()->repaintGifEmotes();
    });
}

bool GIFTimer::shouldAnimate() const
{
    if (!getSettings()->animateEmotes)
    {
        return false;
    }

    return !getSettings()->animationsWhenFocused ||
           this->openOverlayWindows_ != 0 ||
           QApplication::activeWindow() != nullptr;
}

}
