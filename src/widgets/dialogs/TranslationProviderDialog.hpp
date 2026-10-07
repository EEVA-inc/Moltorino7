#pragma once

#include "providers/translation/Translator.hpp"

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;

namespace chatterino {

class TranslationProviderDialog final : public QDialog
{
public:
    explicit TranslationProviderDialog(QWidget *parent = nullptr);

private:
    void save(bool testAfter);
    void saveConnectionSettings();
    void testConnection();
    void removeCredential();
    void refreshCredentialStatus();
    void setBusy(bool busy);
    void setStatus(const QString &text, bool error = false);

    TranslationProvider provider_ = TranslationProvider::GoogleBuiltIn;
    QLabel *providerName_{};
    QLabel *description_{};
    QLabel *status_{};
    QLabel *keyLabel_{};
    QLineEdit *apiKey_{};
    QPushButton *removeKey_{};
    QLabel *endpointLabel_{};
    QLineEdit *libreEndpoint_{};
    QLabel *regionLabel_{};
    QLineEdit *azureRegion_{};
    QLabel *azureEndpointLabel_{};
    QLineEdit *azureEndpoint_{};
    QPushButton *save_{};
    QPushButton *test_{};
    bool credentialSaved_ = false;
};

}
