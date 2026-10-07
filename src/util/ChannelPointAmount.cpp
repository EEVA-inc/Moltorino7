#include "util/ChannelPointAmount.hpp"

#include <algorithm>

namespace chatterino {

namespace {

std::optional<int> parseAmount(const QString &input, int maximum)
{
    if (maximum < 1 || input.isEmpty())
    {
        return std::nullopt;
    }

    auto digits = input;
    int multiplier = 1;
    if (digits.endsWith('k', Qt::CaseInsensitive))
    {
        multiplier = 1000;
        digits.chop(1);
    }

    if (digits.isEmpty())
    {
        return std::nullopt;
    }

    for (const auto character : digits)
    {
        if (!character.isDigit())
        {
            return std::nullopt;
        }
    }

    bool ok = false;
    const auto base = digits.toLongLong(&ok);
    if (!ok || base < 1 || base > maximum / multiplier)
    {
        return std::nullopt;
    }

    return static_cast<int>(base * multiplier);
}

}

std::optional<int> parseChannelPointAmount(const QString &input, int maximum)
{
    return parseAmount(input.trimmed(), maximum);
}

ChannelPointAmountValidator::ChannelPointAmountValidator(int maximum,
                                                         QObject *parent)
    : QValidator(parent)
    , maximum_(maximum)
{
}

QValidator::State ChannelPointAmountValidator::validate(QString &input,
                                                        int &pos) const
{
    const auto trimmed = input.trimmed();
    if (trimmed != input)
    {
        input = trimmed;
        pos = std::min(pos, static_cast<int>(input.size()));
    }

    if (input.isEmpty() || input.compare("k", Qt::CaseInsensitive) == 0 ||
        input == "0")
    {
        return Intermediate;
    }

    return parseAmount(input, this->maximum_).has_value() ? Acceptable
                                                          : Invalid;
}

}
