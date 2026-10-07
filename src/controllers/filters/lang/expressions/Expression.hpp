// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/filters/lang/Tokenizer.hpp"
#include "controllers/filters/lang/Types.hpp"

#include <QString>
#include <QVariant>

#include <memory>
#include <vector>

namespace chatterino {
struct Message;
class Channel;
}

namespace chatterino::filters {

struct RunContext {
    RunContext(const Message &message, Channel *channel)
        : message(&message)
        , channel(channel)
    {
    }

    RunContext(const ContextMap &values)
        : values(&values)
    {
    }

    const Message *message{};
    Channel *channel{};
    const ContextMap *values{};
};

class Expression
{
public:
    virtual ~Expression() = default;

    virtual QVariant execute(RunContext context) const = 0;
    virtual PossibleType synthesizeType(const TypingContext &context) const = 0;
    virtual QString debug(const TypingContext &context) const = 0;
    virtual QString filterString() const = 0;
};

using ExpressionPtr = std::unique_ptr<Expression>;
using ExpressionList = std::vector<std::unique_ptr<Expression>>;

}
