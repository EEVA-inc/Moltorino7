#pragma once

#include <QWidget>

#include <memory>

namespace chatterino {

class UsercardModerationSettings final : public QWidget
{
public:
    explicit UsercardModerationSettings(QWidget *parent = nullptr);
    ~UsercardModerationSettings() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
