#include "widgets/dialogs/TranslationProviderDialog.hpp"

#include "providers/translation/TranslationRequest.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

namespace chatterino {

namespace {

const TranslationProviderDescriptor &descriptorFor(TranslationProvider provider)
{
    for (const auto &descriptor : translationProviders())
    {
        if (descriptor.provider == provider)
        {
            return descriptor;
        }
    }
    return translationProviders().front();
}

QString providerDescription(TranslationProvider provider)
{
    switch (provider)
    {
        case TranslationProvider::GoogleBuiltIn:
            return QStringLiteral(
                "Works without setup. Google Translate handles the "
                "translation.");
        case TranslationProvider::MicrosoftFree:
            return QStringLiteral(
                "Works without setup. Microsoft Translator handles the "
                "translation.");
        case TranslationProvider::DeepL:
            return QStringLiteral(
                "Enter a DeepL API key. Your key is stored on this device.");
        case TranslationProvider::LibreTranslate:
            return QStringLiteral(
                "Use libretranslate.com or enter your own server address. "
                "Some private servers do not need a key.");
        case TranslationProvider::GoogleCloud:
            return QStringLiteral(
                "Use an API key from a project with Cloud Translation Basic "
                "enabled. Your key is stored on this device.");
        case TranslationProvider::MicrosoftAzure:
            return QStringLiteral(
                "Enter an Azure Translator key and its region. Leave the "
                "region empty for a global resource.");
    }
    return {};
}

}

TranslationProviderDialog::TranslationProviderDialog(QWidget *parent)
    : QDialog(parent)
    , provider_(translationProviderFromId(
          getSettings()->translationProvider.getValue()))
{
    const auto &descriptor = descriptorFor(this->provider_);
    this->setWindowTitle(QStringLiteral("Set up translation"));
    this->setModal(true);
    this->setMinimumWidth(520);

    auto *layout = new QVBoxLayout(this);
    this->providerName_ = new QLabel(descriptor.name, this);
    QFont titleFont = makeResolvedFont(this->providerName_->font(), QFont::Bold);
    titleFont.setPointSizeF(titleFont.pointSizeF() + 1);
    this->providerName_->setFont(titleFont);
    layout->addWidget(this->providerName_);

    this->description_ = new QLabel(providerDescription(this->provider_), this);
    this->description_->setWordWrap(true);
    layout->addWidget(this->description_);

    auto *settings = new QGroupBox(QStringLiteral("Connection"), this);
    auto *form = new QFormLayout(settings);

    this->apiKey_ = new QLineEdit(settings);
    this->apiKey_->setEchoMode(QLineEdit::Password);
    this->apiKey_->setPlaceholderText(QStringLiteral("Enter a new key"));
    this->removeKey_ = new QPushButton(QStringLiteral("Remove key"), settings);
    this->removeKey_->setEnabled(false);
    auto *keyRow = new QWidget(settings);
    auto *keyLayout = new QHBoxLayout(keyRow);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    keyLayout->addWidget(this->apiKey_, 1);
    keyLayout->addWidget(this->removeKey_);
    this->keyLabel_ = new QLabel(descriptor.apiKeyOptional
                                     ? QStringLiteral("API key (optional):")
                                     : QStringLiteral("API key:"),
                                 settings);
    form->addRow(this->keyLabel_, keyRow);

    this->libreEndpoint_ = new QLineEdit(
        getSettings()->translationLibreEndpoint.getValue(), settings);
    this->libreEndpoint_->setPlaceholderText(
        QStringLiteral("https://libretranslate.com"));
    this->endpointLabel_ = new QLabel(QStringLiteral("Server:"), settings);
    form->addRow(this->endpointLabel_, this->libreEndpoint_);

    this->azureRegion_ = new QLineEdit(
        getSettings()->translationAzureRegion.getValue(), settings);
    this->azureRegion_->setPlaceholderText(
        QStringLiteral("Optional, for example eastus"));
    this->regionLabel_ = new QLabel(QStringLiteral("Region:"), settings);
    form->addRow(this->regionLabel_, this->azureRegion_);

    this->azureEndpoint_ = new QLineEdit(
        getSettings()->translationAzureEndpoint.getValue(), settings);
    this->azureEndpoint_->setPlaceholderText(
        QStringLiteral("Use global endpoint"));
    this->azureEndpoint_->setToolTip(
        QStringLiteral("Optional. Paste the resource endpoint from Azure."));
    this->azureEndpointLabel_ =
        new QLabel(QStringLiteral("Endpoint:"), settings);
    form->addRow(this->azureEndpointLabel_, this->azureEndpoint_);

    const bool hasKey = descriptor.usesApiKey;
    this->keyLabel_->setVisible(hasKey);
    keyRow->setVisible(hasKey);
    this->endpointLabel_->setVisible(this->provider_ ==
                                     TranslationProvider::LibreTranslate);
    this->libreEndpoint_->setVisible(this->provider_ ==
                                     TranslationProvider::LibreTranslate);
    this->regionLabel_->setVisible(this->provider_ ==
                                   TranslationProvider::MicrosoftAzure);
    this->azureRegion_->setVisible(this->provider_ ==
                                   TranslationProvider::MicrosoftAzure);
    this->azureEndpointLabel_->setVisible(this->provider_ ==
                                          TranslationProvider::MicrosoftAzure);
    this->azureEndpoint_->setVisible(this->provider_ ==
                                     TranslationProvider::MicrosoftAzure);
    settings->setVisible(hasKey || this->provider_ ==
                                       TranslationProvider::LibreTranslate);
    layout->addWidget(settings);

    this->status_ = new QLabel(this);
    this->status_->setTextFormat(Qt::PlainText);
    this->status_->setWordWrap(true);
    layout->addWidget(this->status_);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    this->test_ = buttons->addButton(QStringLiteral("Test"),
                                     QDialogButtonBox::ActionRole);
    this->save_ = buttons->addButton(QDialogButtonBox::Save);
    this->save_->setVisible(hasKey || this->provider_ ==
                                          TranslationProvider::LibreTranslate);
    if (hasKey)
    {
        this->test_->setText(QStringLiteral("Save and test"));
    }
    QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                     &QDialog::close);
    QObject::connect(this->save_, &QPushButton::clicked, this, [this] {
        this->save(false);
    });
    QObject::connect(this->test_, &QPushButton::clicked, this, [this] {
        this->save(true);
    });
    QObject::connect(this->removeKey_, &QPushButton::clicked, this,
                     &TranslationProviderDialog::removeCredential);
    layout->addWidget(buttons);

    this->refreshCredentialStatus();
}

void TranslationProviderDialog::save(bool testAfter)
{
    const auto &descriptor = descriptorFor(this->provider_);
    const auto credential = this->apiKey_->text().trimmed();
    if (descriptor.usesApiKey && !descriptor.apiKeyOptional &&
        credential.isEmpty() && !this->credentialSaved_)
    {
        this->setStatus(testAfter
                            ? QStringLiteral("Enter an API key before testing.")
                            : QStringLiteral("Enter an API key before saving."),
                        true);
        this->apiKey_->setFocus(Qt::OtherFocusReason);
        return;
    }
    const translation::detail::ConnectionSettings connection{
        .libreEndpoint = this->libreEndpoint_->text(),
        .azureRegion = this->azureRegion_->text(),
        .azureEndpoint = this->azureEndpoint_->text(),
    };
    const auto validated = translation::detail::makeProviderRequest(
        this->provider_, QStringLiteral("test"), QStringLiteral("es"),
        credential.isEmpty() && this->credentialSaved_
            ? QStringLiteral("saved-key")
            : credential,
        connection);
    if (!validated)
    {
        this->setStatus(validated.error(), true);
        return;
    }
    if (translationProviderCredentialKey(this->provider_).isEmpty() ||
        credential.isEmpty())
    {
        this->saveConnectionSettings();
        if (testAfter)
        {
            this->testConnection();
        }
        else
        {
            this->setStatus(QStringLiteral("Saved."));
        }
        return;
    }

    this->setBusy(true);
    this->setStatus(QStringLiteral("Saving key..."));
    const QPointer<TranslationProviderDialog> self(this);
    writeTranslationProviderCredential(
        this->provider_, credential,
        [self, testAfter](ExpectedStr<void> result) {
            if (!self)
            {
                return;
            }
            if (!result)
            {
                self->setBusy(false);
                self->setStatus(result.error(), true);
                return;
            }
            self->saveConnectionSettings();
            self->apiKey_->clear();
            self->apiKey_->setPlaceholderText(QStringLiteral("Saved key"));
            self->credentialSaved_ = true;
            self->setBusy(false);
            if (testAfter)
            {
                self->testConnection();
            }
            else
            {
                self->setStatus(QStringLiteral("Key saved."));
            }
        });
}

void TranslationProviderDialog::saveConnectionSettings()
{
    if (this->provider_ == TranslationProvider::LibreTranslate)
    {
        const auto endpoint = translation::detail::normalizedServerEndpoint(
            this->provider_, this->libreEndpoint_->text());
        if (endpoint)
        {
            this->libreEndpoint_->setText(endpoint->toString());
            getSettings()->translationLibreEndpoint.setValue(
                endpoint->toString());
        }
    }
    else if (this->provider_ == TranslationProvider::MicrosoftAzure)
    {
        const auto endpoint = translation::detail::normalizedServerEndpoint(
            this->provider_, this->azureEndpoint_->text());
        if (endpoint)
        {
            const auto value = this->azureEndpoint_->text().trimmed().isEmpty()
                                   ? QString{}
                                   : endpoint->toString();
            this->azureEndpoint_->setText(value);
            getSettings()->translationAzureEndpoint.setValue(value);
        }
        const auto region = this->azureRegion_->text().trimmed().toLower();
        this->azureRegion_->setText(region);
        getSettings()->translationAzureRegion.setValue(region);
    }
}

void TranslationProviderDialog::testConnection()
{
    this->setBusy(true);
    this->setStatus(QStringLiteral("Testing..."));
    requestTextTranslation(
        QStringLiteral("Hello from Moltorino"), QStringLiteral("es"), this,
        [this](const TranslationResult &result) {
            this->setStatus(QStringLiteral("Connected. Test result: %1")
                                .arg(result.translatedText));
        },
        [this](const QString &error) {
            this->setStatus(error, true);
        },
        [this] {
            this->setBusy(false);
        },
        this->provider_);
}

void TranslationProviderDialog::removeCredential()
{
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("Remove saved key?"));
    box.setText(QStringLiteral("Remove the saved %1 key?")
                    .arg(translationProviderName(this->provider_)));
    auto *remove = box.addButton(QStringLiteral("Remove key"),
                                 QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != remove)
    {
        return;
    }

    this->setBusy(true);
    const QPointer<TranslationProviderDialog> self(this);
    removeTranslationProviderCredential(
        this->provider_, [self](ExpectedStr<void> result) {
            if (!self)
            {
                return;
            }
            if (!result)
            {
                self->setBusy(false);
                self->setStatus(result.error(), true);
                return;
            }
            self->apiKey_->clear();
            self->apiKey_->setPlaceholderText(QStringLiteral("No saved key"));
            self->credentialSaved_ = false;
            self->setBusy(false);
            self->setStatus(QStringLiteral("Key removed."));
        });
}

void TranslationProviderDialog::refreshCredentialStatus()
{
    const auto &descriptor = descriptorFor(this->provider_);
    if (!descriptor.usesApiKey)
    {
        this->setStatus(QStringLiteral("Ready to use. No setup needed."));
        return;
    }

    this->setBusy(true);
    this->setStatus(QStringLiteral("Checking for a saved key..."));
    const QPointer<TranslationProviderDialog> self(this);
    readTranslationProviderCredential(
        this->provider_,
        [self, optional =
                   descriptor.apiKeyOptional](ExpectedStr<QString> credential) {
            if (!self)
            {
                return;
            }
            if (!credential)
            {
                self->credentialSaved_ = false;
                self->apiKey_->setPlaceholderText(
                    QStringLiteral("Unable to check saved key"));
                self->setStatus(credential.error(), true);
                self->setBusy(false);
                return;
            }
            if (!credential->trimmed().isEmpty())
            {
                self->credentialSaved_ = true;
                self->apiKey_->setPlaceholderText(QStringLiteral("Saved key"));
                self->setStatus(QStringLiteral("A key is already saved."));
                self->setBusy(false);
                return;
            }
            self->credentialSaved_ = false;
            self->apiKey_->setPlaceholderText(
                optional ? QStringLiteral("Optional")
                         : QStringLiteral("No saved key"));
            self->setStatus(optional
                                ? QStringLiteral("A key is optional for many "
                                                 "private servers.")
                                : QStringLiteral("Enter an API key to use "
                                                 "this service."));
            self->setBusy(false);
        },
        true);
}

void TranslationProviderDialog::setBusy(bool busy)
{
    this->save_->setEnabled(!busy);
    this->test_->setEnabled(!busy);
    this->removeKey_->setEnabled(!busy && this->credentialSaved_);
    this->apiKey_->setEnabled(!busy);
    this->libreEndpoint_->setEnabled(!busy);
    this->azureRegion_->setEnabled(!busy);
    this->azureEndpoint_->setEnabled(!busy);
}

void TranslationProviderDialog::setStatus(const QString &text, bool error)
{
    this->status_->setText(text);
    this->status_->setStyleSheet(error ? QStringLiteral("color: #f28b82;")
                                       : QString{});
}

}
