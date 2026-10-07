#pragma once

class QString;

namespace chatterino {

struct CommandContext;

}

namespace chatterino::commands {

QString doYouTubeBan(const CommandContext &ctx);

QString doYouTubeTimeout(const CommandContext &ctx);

QString doYouTubeUnban(const CommandContext &ctx);

QString doYouTubeDelete(const CommandContext &ctx);

}
