// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

namespace chatterino {

struct Link {
public:
    enum Type {
        None,
        Url,
        UserInfo,
        UserWhisper,
        InsertText,
        UserAction,
        AutoModAllow,
        AutoModDeny,
        AutoModReviewApprove,
        AutoModReviewDeny,
        AutoModReviewTimeout,
        AutoModReviewBan,
        AutoModReviewRetry,
        OpenModerationReport,
        OpenAccountsPage,
        JumpToChannel,
        OpenChannel,
        Reconnect,
        CopyToClipboard,
        ReplyToMessage,
        ViewThread,
        JumpToMessage,
        AcknowledgeChatWarning,
    };

    Link();
    Link(Type getType, const QString &getValue);

    Type type;
    QString value;

    bool isValid() const;
    bool isUrl() const;
};

}
