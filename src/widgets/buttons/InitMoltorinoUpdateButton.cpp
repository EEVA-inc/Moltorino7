#include "widgets/buttons/InitMoltorinoUpdateButton.hpp"

#include "providers/moltorino/MoltorinoUpdater.hpp"
#include "widgets/buttons/PixmapButton.hpp"

namespace chatterino {

void initMoltorinoUpdateButton(PixmapButton &button,
                               const std::function<void()> &relayout,
                               pajlada::Signals::SignalHolder &signalHolder)
{
    button.hide();

    QObject::connect(&button, &Button::leftClicked, [] {
        getMoltorinoUpdater()->requestUpdatePrompt();
    });

    auto updateButton = [&button, relayout] {
        auto *updater = getMoltorinoUpdater();
        const bool shouldShow = updater->shouldShowUpdateButton();
        const bool visibilityChanged = button.isHidden() == shouldShow;
        button.setVisible(shouldShow);
        button.setPixmap(QPixmap(updater->isError()
                                     ? ":/buttons/updateError.png"
                                     : ":/buttons/update.png"));
        button.setDim(updater->isBusy() ? DimButton::Dim::Lots
                                        : DimButton::Dim::Some);
        button.setToolTip(updater->statusText());
        if (visibilityChanged)
        {
            relayout();
        }
    };

    updateButton();
    signalHolder.managedConnect(getMoltorinoUpdater()->stateChanged,
                                [updateButton] {
                                    updateButton();
                                });
}

}
