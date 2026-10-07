// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

class QString;

namespace chatterino {

class Channel;
struct CommandContext;

}

namespace chatterino::commands {

bool isChattersCommandAvailable(const Channel *channel);

QString chatters(const CommandContext &ctx);

QString testChatters(const CommandContext &ctx);

}
