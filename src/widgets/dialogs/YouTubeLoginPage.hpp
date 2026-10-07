#pragma once

#include "widgets/BaseWidget.hpp"

#include <QString>

#include <functional>

class QLabel;
class QProgressBar;
class QPushButton;

namespace chatterino {

class YouTubeLoginPage : public BaseWidget
{
public:
    explicit YouTubeLoginPage(QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;
    void themeChangedEvent() override;

private:
    enum class StatusKind {
        None,
        Waiting,
        Success,
        Error,
    };

    void startLogin();
    void setBusy(bool busy);
    void setStatus(const QString &status,
                   StatusKind kind = StatusKind::None);

    QLabel *titleLabel_ = nullptr;
    QLabel *descriptionLabel_ = nullptr;
    QLabel *securityLabel_ = nullptr;
    QLabel *disconnectLabel_ = nullptr;
    QPushButton *connectButton_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QProgressBar *statusProgress_ = nullptr;
    std::function<void()> cancelLogin_;
    bool loginInProgress_ = false;
    bool authorizationAccepted_ = false;
    bool loginSucceeded_ = false;
    StatusKind statusKind_ = StatusKind::None;
};

}
