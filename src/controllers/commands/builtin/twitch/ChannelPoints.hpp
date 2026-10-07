#pragma once

class QString;

namespace chatterino {

struct CommandContext;

}  // namespace chatterino

namespace chatterino::commands {

QString openChannelPointRewards(const CommandContext &ctx);
QString openGifPicker(const CommandContext &ctx);
QString sendGigantifiedEmote(const CommandContext &ctx);

}  // namespace chatterino::commands
