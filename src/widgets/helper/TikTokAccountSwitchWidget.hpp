#pragma once

#include <pajlada/signals/signalholder.hpp>
#include <QListWidget>

namespace chatterino {

class TikTokAccountSwitchWidget : public QListWidget
{
public:
    explicit TikTokAccountSwitchWidget(QWidget *parent = nullptr);

    void refresh();

private:
    void refreshItems();

    pajlada::Signals::SignalHolder managedConnections_;
};

}
