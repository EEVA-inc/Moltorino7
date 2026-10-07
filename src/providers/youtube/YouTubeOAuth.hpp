#pragma once

#include "providers/youtube/YouTubeAccount.hpp"

#include <QString>

#include <functional>

class QWidget;

namespace chatterino {

class YouTubeOAuth
{
public:
    using SuccessCallback = std::function<void(YouTubeAccountData)>;
    using ErrorCallback = std::function<void(const QString &)>;
    using AuthorizationAcceptedCallback = std::function<void()>;
    using CancelCallback = std::function<void()>;

    static CancelCallback start(QWidget *parent, SuccessCallback success,
                                ErrorCallback error,
                                AuthorizationAcceptedCallback accepted = {});
};

}
