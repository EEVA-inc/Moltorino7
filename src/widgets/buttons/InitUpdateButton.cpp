// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/buttons/InitUpdateButton.hpp"

#include "widgets/buttons/InitMoltorinoUpdateButton.hpp"

namespace chatterino {

void initUpdateButton(PixmapButton &button,
                      const std::function<void()> &relayout,
                      pajlada::Signals::SignalHolder &signalHolder)
{
    initMoltorinoUpdateButton(button, relayout, signalHolder);
}

}
