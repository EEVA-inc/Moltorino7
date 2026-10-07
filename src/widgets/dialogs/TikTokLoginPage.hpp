#pragma once

#include "providers/tiktok/TikTokApi.hpp"
#include "widgets/BaseWidget.hpp"

#include <QPointer>

class QLabel;
class QPushButton;
class QProgressBar;

namespace chatterino {
class TikTokLoginView;

class TikTokLoginPage : public BaseWidget
{
public:
    explicit TikTokLoginPage(QWidget *parent = nullptr);
    ~TikTokLoginPage() override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void themeChangedEvent() override;
    void hideEvent(QHideEvent *event) override;

private:
    void start();
    void status(const QString &text, bool busy);
    TikTokApi api_;
    QPointer<TikTokLoginView> login_;
    QLabel *title_ = nullptr;
    QLabel *description_ = nullptr;
    QPushButton *connect_ = nullptr;
    QLabel *status_ = nullptr;
    QProgressBar *progress_ = nullptr;
};
}
