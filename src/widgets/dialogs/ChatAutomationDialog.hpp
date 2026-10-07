#pragma once

#include "controllers/chat/ChatAutomationController.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <QDialog>

#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QKeyEvent;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;

namespace chatterino {

class ChatAutomationDialog final : public QDialog
{
public:
    explicit ChatAutomationDialog(QString initialChannel = {},
                                  QWidget *parent = nullptr);
    static void showDialog(QString initialChannel = {},
                           QWidget *parent = nullptr);

private:
    bool event(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void showExamples();
    void addRule(int example = 0);
    void duplicateRule();
    void removeRule();
    void undoRemoveRule();
    void syncRuleOrderFromList();
    void moveRule(int offset);
    void insertVariable(const QString &variable);
    void loadRule(int index);
    void storeCurrentRule();
    void refreshRuleList();
    void refreshCurrentRuleListItem();
    void refreshChannels();
    void filterChannels(const QString &query);
    void toggleShownChannels();
    void refreshChannelTools();
    void addChannel();
    void refreshEditorState();
    void refreshMatchHint();
    void refreshVariableOptions();
    void refreshPreview();
    void refreshStyle();
    void refreshGlobalSettings();
    bool validateRules();
    void save();

    ChatAutomationController *controller_{};
    std::vector<ChatAutomation> rules_;
    QString initialChannel_;
    int currentRule_ = -1;
    bool loading_ = false;
    bool reordering_ = false;
    bool applying_ = false;
    bool initialBackground_ = false;
    int initialMaxRuns_ = 6;
    std::optional<ChatAutomation> removedRule_;
    int removedRuleIndex_ = 0;

    QListWidget *ruleList_{};
    QPushButton *duplicate_{};
    QPushButton *remove_{};
    QPushButton *undoRemove_{};
    QWidget *editor_{};
    QCheckBox *ruleEnabled_{};
    QCheckBox *masterEnabled_{};
    QCheckBox *runInBackground_{};
    QSpinBox *maxRuns_{};
    QLabel *runStatus_{};
    QLineEdit *name_{};
    QLineEdit *trigger_{};
    QLabel *triggerLabel_{};
    QComboBox *argumentMode_{};
    QLabel *argumentLabel_{};
    QLineEdit *aliases_{};
    QLabel *aliasesLabel_{};
    QCheckBox *caseSensitive_{};
    QComboBox *match_{};
    QComboBox *action_{};
    QPlainTextEdit *response_{};
    QPushButton *variable_{};
    QComboBox *responseMode_{};
    QLabel *responseHint_{};
    QComboBox *access_{};
    QLineEdit *users_{};
    QLabel *usersLabel_{};
    QLineEdit *ignoredUsers_{};
    QLineEdit *requiredText_{};
    QLineEdit *excludedText_{};
    QSpinBox *minimumLength_{};
    QSpinBox *maximumLength_{};
    QComboBox *stream_{};
    QComboBox *channelRole_{};
    QComboBox *sender_{};
    QLabel *conditionsSummary_{};
    QLabel *cooldownHint_{};
    QSpinBox *cooldown_{};
    QSpinBox *userCooldown_{};
    QComboBox *cooldownUnit_{};
    QComboBox *userCooldownUnit_{};
    QComboBox *timeZone_{};
    QLabel *timeZoneLabel_{};
    QComboBox *timeFormat_{};
    QLabel *timeFormatLabel_{};
    QLineEdit *customTimeFormat_{};
    QWidget *customTimeRow_{};
    QComboBox *dateFormat_{};
    QLabel *dateFormatLabel_{};
    QLineEdit *customDateFormat_{};
    QWidget *customDateRow_{};
    QLabel *formatPreview_{};
    QLabel *matchHint_{};
    QComboBox *channelMode_{};
    QCheckBox *useBotBadge_{};
    QPushButton *botSetup_{};
    QLineEdit *channelSearch_{};
    QPushButton *channelToggle_{};
    QPushButton *channelAdd_{};
    QListWidget *channels_{};
    QWidget *channelPicker_{};
    QLabel *channelScope_{};
    QLabel *empty_{};
    QLabel *validation_{};
    QTabWidget *tabs_{};
    QLineEdit *testMessage_{};
    QLineEdit *testUser_{};
    QLineEdit *testChannel_{};
    QSpinBox *testChoice_{};
    QSpinBox *testCounter_{};
    QLabel *testCounterLabel_{};
    QLabel *testChoiceLabel_{};
    QComboBox *testRole_{};
    QComboBox *testMyRole_{};
    QComboBox *testStream_{};
    QCheckBox *testSelf_{};
    QLabel *testStatus_{};
    QPlainTextEdit *testArguments_{};
    QPlainTextEdit *preview_{};
    QLabel *previewLabel_{};
    QPushButton *save_{};

    pajlada::Signals::ScopedConnection openChannelsConnection_;
    pajlada::Signals::ScopedConnection rulesConnection_;
};

}
