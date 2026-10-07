#pragma once

#include "singletons/ThemeCustomization.hpp"
#include "widgets/settingspages/SettingsPage.hpp"

#include <QSet>

#include <functional>
#include <stop_token>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QTabWidget;
class QTimer;

namespace chatterino {

class ThemePreviewWidget;
class TypographyPreviewWidget;
class WallpaperFocusWidget;
class ShadowPositionWidget;

class CustomizationPage final : public SettingsPage
{
public:
    CustomizationPage();
    ~CustomizationPage() override;

    void onShow() override;
    bool filterElements(const QString &query) override;

private:
    void buildUi();
    QWidget *buildColorsTab();
    QWidget *buildChatTab();
    QWidget *buildInterfaceTab();
    QWidget *buildTypographyTab();
    QWidget *buildShareTab();

    void reloadProfiles(const QString &preferredKey = {});
    bool confirmProfileChange();
    void loadSelectedProfile();
    void loadProfileIntoControls();
    void readControlsIntoProfile();
    void updatePreview();
    void updateControlState();
    void updateReadability();

    void applySelectedTheme();
    void duplicateProfile();
    void renameProfile();
    void deleteProfile();
    bool saveCustomProfile(bool apply);
    void resetDraft();

    void chooseWallpaper();
    void prepareWallpaper(const QString &path);
    std::optional<int> chooseVideoLength(double duration);
    void clearWallpaper();
    void suggestWallpaperPalette();
    void markColorEdited(const QString &role);
    void buildPalette();

    void exportThemeFile();
    void importThemeFile();
    void createThemeCode();
    void importThemeCode();
    void prepareSharedTheme(std::function<void(QJsonObject, bool)> ready);
    void setTransferBusy(bool busy);
    bool writeImportedTheme(ThemeCustomizationProfile profile, bool adapted,
                            QString *error);

    QString uniqueThemePath(const QString &name) const;
    QString managedWallpaperSource(const QString &source, QString *error) const;
    void cleanupManagedWallpapers() const;
    bool installImportedTheme(ThemeCustomizationProfile profile, bool adapted,
                              bool localWallpaperRemoved, QString *error);
    void setStatus(const QString &text, bool error = false, bool busy = false);

    ThemeCustomizationProfile profile_;
    ThemeCustomizationProfile savedProfile_;
    QString selectedKey_;
    QString selectedPath_;
    bool selectedCustom_ = false;
    bool loadingControls_ = false;
    bool wallpaperPreparing_ = false;
    bool transferBusy_ = false;
    std::uint64_t profileGeneration_ = 0;
    std::uint64_t shareGeneration_ = 0;
    std::uint64_t wallpaperGeneration_ = 0;
    std::stop_source transferCancellation_;
    std::stop_source wallpaperCancellation_;
    QSet<QString> editedColorRoles_;

    QComboBox *profileBox_{};
    QLabel *profileKind_{};
    QPushButton *useButton_{};
    QPushButton *duplicateButton_{};
    QPushButton *renameButton_{};
    QPushButton *deleteButton_{};
    QPushButton *saveButton_{};
    QPushButton *resetButton_{};
    QPushButton *cancelWallpaperButton_{};
    ThemePreviewWidget *preview_{};
    QWidget *colorsEditor_{};
    QWidget *chatEditor_{};
    QWidget *interfaceEditor_{};
    QWidget *typographyEditor_{};
    QLabel *readability_{};
    QLabel *status_{};
    QTimer *statusAnimation_{};
    QString statusText_;
    int statusDots_ = 0;
    QCheckBox *chatTextFollowsText_{};

    struct ColorControl {
        QPushButton *button{};
        QLineEdit *input{};
        QColor ThemeCustomizationProfile::*member{};
    };
    QVector<ColorControl> colorControls_;

    QLineEdit *wallpaperPath_{};
    QComboBox *wallpaperMode_{};
    QSlider *wallpaperOpacity_{};
    QPushButton *wallpaperOverlayColorButton_{};
    QLineEdit *wallpaperOverlayColorInput_{};
    QSlider *wallpaperOverlayOpacity_{};
    QSlider *wallpaperBlur_{};
    QSlider *wallpaperZoom_{};
    WallpaperFocusWidget *wallpaperFocus_{};
    QCheckBox *alternateMessageRows_{};
    QSlider *alternateMessageOpacity_{};
    QSlider *alternateMessageContrast_{};
    QSlider *highlightOpacityAdjustment_{};
    QCheckBox *roundChat_{};
    QSlider *chatCornerRadius_{};
    QCheckBox *chatBorder_{};
    QPushButton *chatBorderColorButton_{};
    QLineEdit *chatBorderColorInput_{};
    QSlider *chatBorderWidth_{};
    QSlider *chatBorderOpacity_{};
    QCheckBox *messageShadow_{};
    QCheckBox *messageShadowEmotes_{};
    QPushButton *messageShadowColorButton_{};
    QLineEdit *messageShadowColorInput_{};
    QSlider *messageShadowOpacity_{};
    ShadowPositionWidget *messageShadowPosition_{};
    QSlider *messageShadowBlur_{};
    QPushButton *wallpaperCenterFocus_{};

    QComboBox *foundation_{};
    QSlider *panelContrast_{};
    QComboBox *accentStrength_{};
    QComboBox *tabShape_{};
    QSlider *tabCornerRadius_{};
    QSlider *tabSpacing_{};
    QSlider *inactiveTabContrast_{};
    QSlider *channelBarContrast_{};
    QCheckBox *useThemeFonts_{};
    QCheckBox *useThemeFontSizes_{};
    QComboBox *chatFont_{};
    QSlider *chatFontSize_{};
    QSlider *chatFontWeight_{};
    QComboBox *usernameFont_{};
    QSlider *usernameFontSize_{};
    QSlider *usernameFontWeight_{};
    QComboBox *interfaceFont_{};
    QSlider *interfaceFontSize_{};
    TypographyPreviewWidget *typographyPreview_{};

    QComboBox *shareExpiry_{};
    QLineEdit *shareCode_{};
    QLineEdit *importCode_{};
    QPushButton *createCodeButton_{};
    QPushButton *importCodeButton_{};
    QPushButton *exportFileButton_{};
    QPushButton *importFileButton_{};
    QCheckBox *shareWallpaper_{};
};

}
