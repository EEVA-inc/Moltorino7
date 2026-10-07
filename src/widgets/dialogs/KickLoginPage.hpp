#pragma once

#include <QLineEdit>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QPointer>
#include <QWidget>

class QDialog;

namespace chatterino {

class KickLoginPage : public QWidget
{
public:
    KickLoginPage();

protected:
    void paintEvent(QPaintEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    struct {
        QFormLayout *layout = nullptr;
        QLabel *description = nullptr;
        QComboBox *method = nullptr;
        QLineEdit *clientID = nullptr;
        QLineEdit *clientSecret = nullptr;
    } ui;
    QPointer<QDialog> authDialog_;
    void refreshState();
};

}
