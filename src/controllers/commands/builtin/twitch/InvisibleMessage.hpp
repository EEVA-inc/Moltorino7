#pragma once

#include <QString>

namespace chatterino {

struct CommandContext;

namespace commands {

QString sendInvisibleMessage(const CommandContext &ctx);

}
}
