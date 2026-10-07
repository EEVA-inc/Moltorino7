#pragma once

#include <QString>
#include <QValidator>

#include <optional>

namespace chatterino {

inline constexpr int MAX_POLL_POINTS_PER_VOTE = 1'000'000;

std::optional<int> parseChannelPointAmount(const QString &input, int maximum);

class ChannelPointAmountValidator final : public QValidator
{
public:
    explicit ChannelPointAmountValidator(int maximum,
                                         QObject *parent = nullptr);

    State validate(QString &input, int &pos) const override;

private:
    int maximum_;
};

}
