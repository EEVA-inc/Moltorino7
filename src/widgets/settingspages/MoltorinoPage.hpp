#pragma once

#include "widgets/settingspages/SettingsPage.hpp"

#include <pajlada/signals/scoped-connection.hpp>

class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QTabBar;

namespace chatterino {

class GeneralPageView;
class SignalLabel;

class MoltorinoPage : public SettingsPage
{
    Q_OBJECT

public:
    MoltorinoPage();

    bool filterElements(const QString &query) override;
    void showAccountSetup();
    void showBotBadgeSetup();

private:
    void openAuthDialog();
    void refreshAuthAccounts();
    void updateAuthSummary();
    void updateAuthInstructions(const QString &text, bool isError = false);
    void updateAuthStatus(const QString &text, bool isValid,
                          bool isError = false);
    void revealBotBadgeSettings(bool forceReveal = false);
    void syncBotBadgeVisibility();
    void populateBotBadgeFieldsFromSettings();
    void updateBotBadgeStatus(const QString &text, bool isValid = false,
                              bool isError = false);
    void openBotBadgeAuthorization();
    void verifyBotBadgeConfiguration();

    GeneralPageView *settingsView_{};
    GeneralPageView *moderationView_{};
    QTabBar *tabBar_{};

    QLabel *authStatusLabel_{};
    QLabel *authInstructionsLabel_{};
    QPushButton *addAuthAccountButton_{};
    QPushButton *refreshAuthAccountsButton_{};
    bool authRefreshInFlight_{false};
    int authRefreshGeneration_{0};

    SignalLabel *logoLabel_{};
    QFrame *botBadgeFrame_{};
    QLabel *botBadgeStatusLabel_{};
    QLabel *botBadgeIdentityLabel_{};
    QLineEdit *botBadgeClientIdEdit_{};
    QLineEdit *botBadgeClientSecretEdit_{};
    QLineEdit *botBadgeSenderEdit_{};
    QPushButton *botBadgeAuthorizeButton_{};
    QPushButton *botBadgeVerifyButton_{};
    int logoClickCount_{0};
    bool botBadgeUnlocked_{false};
    bool botBadgeIsValidating_{false};
    int botBadgeValidationGeneration_{0};
    pajlada::Signals::ScopedConnection chatAutomationRulesConnection_;

protected:
    void hideEvent(QHideEvent *event) override;
};

}  // namespace chatterino
