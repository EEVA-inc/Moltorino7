// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QCoreApplication>
#include <QThread>

#include <cassert>

namespace chatterino {

inline bool isGuiThread()
{
    const auto *app = QCoreApplication::instance();
    return app != nullptr && app->thread() == QThread::currentThread();
}

inline void assertInGuiThread()
{
#ifdef _DEBUG
    assert(isGuiThread());
#endif
}

}
