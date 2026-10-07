#pragma once

#include "providers/moltorino/MoltorinoSupporterBadges.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "widgets/DraggablePopup.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QColor>
#include <QDateTime>
#include <QPointer>
#include <QStringList>
#include <QVector>

#include <memory>
#include <optional>
#include <vector>

class QButtonGroup;
class QGridLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QResizeEvent;
class QTabWidget;
class QTimer;
class QVBoxLayout;
class QWidget;

namespace chatterino {

namespace vanity::detail {

bool canSaveIndependentVanityChanges(bool hasAccount, bool layoutLoaded,
                                     bool layoutRequestInFlight,
                                     bool saveInFlight);
std::vector<TwitchGqlAuth> buildTwitchAuthCandidates(
    const QString &accountToken, const QString &accountClientId,
    const QString &savedToken, const QString &savedClientId);
std::vector<TwitchBadge> withSelectedTwitchVanityBadge(
    std::vector<TwitchBadge> badges, const QString &selectedSetId,
    const QString &selectedVersion);

}

class Button;
class SvgButton;
class TwitchChannel;
class VanityChoiceGrid;
class VanityColorPicker;
class VanityColorPreview;
class VanityPreviewWidget;

struct SevenTVVanityCosmetic {
    QString id;
    QString name;
    QString imageUrl;
};

struct TwitchEventBadge {
    QString id;
    QString name;
    QString imageUrl;
    QString detailsUrl;
    QDateTime startAt;
    QDateTime endAt;
    std::optional<bool> free;

    bool operator==(const TwitchEventBadge &) const = default;
};

class VanityDialog final : public DraggablePopup
{
public:
    explicit VanityDialog(std::shared_ptr<TwitchChannel> channel,
                          QWidget *parent = nullptr);

    static void showDialog(std::shared_ptr<TwitchChannel> channel,
                           QWidget *parent = nullptr);

    float scale() const override;

protected:
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    struct ActiveBadgeRow {
        QString key;
        QString slot;
        QString name;

        bool operator==(const ActiveBadgeRow &) const = default;
    };

    void buildUi();
    QWidget *buildColorTab();
    QWidget *buildSevenTVPaintTab();
    QWidget *buildTwitchBadgeTab(bool channelBadges);
    QWidget *buildSevenTVBadgeTab();
    QWidget *buildMoltorinoBadgeTab();
    QWidget *buildLayoutTab();
    QWidget *buildSevenTVConnectionHeader(QWidget *parent, const QString &title,
                                          const QString &disconnectedText);
    void refreshStyle();
    void refreshColorPreviewChip();
    void scheduleResponsiveLayoutRefresh();
    void updateMinimumWidth();
    void loadTwitchState();
    void tryLoadTwitchStateCandidate();
    void retryTwitchState();
    void loadTwitchEventBadges();
    void scheduleTwitchEventBadgeRetry();
    void clearTwitchEventBadgePreview();
    void refreshTwitchBadgeHint(bool channelBadges);
    void refreshTwitchAuthCandidates();
    void setTwitchBadgesUnavailable(const QString &hint,
                                    const QString &details = {});
    void loadLayoutState();
    void retryLayoutSync();
    void resetLayoutToDefault();
    void loadSevenTVState(bool connecting = false, bool forceRefresh = false);
    void connectSevenTV();
    void disconnectSevenTV();
    void populateCurrentTab();
    void populatePaintChoices();
    void populateGlobalBadgeChoices();
    void populateChannelBadgeChoices();
    void populateSevenTVBadgeChoices();
    void populateMoltorinoBadgeChoices();
    std::vector<MoltorinoSupporterBadge> availableMoltorinoBadges() const;
    std::vector<MoltorinoSupporterBadge> moltorinoBadgeCatalog() const;
    void refreshMoltorinoBadgeAvailability();
    void syncEffectiveMoltorinoSelection();
    void useLoadedLayout(const MoltorinoVanityLayout &layout);
    void syncWorkingLayoutFromRows();
    void updateLayoutDirtyState();
    void rebuildLayoutRows();
    void refreshPreview();
    void refreshHeaderIdentity();
    void refreshConnectionText();
    void save();
    void runSaveStep(int step);
    void saveLayout(bool forceOnlineSync = false);
    void finishSave(const QString &message = {});
    void failSave(const QString &provider, const QString &error);
    void recordSavedChange(const QString &change);
    VanityChoiceGrid *currentChoiceGrid() const;
    void updateVisualRefreshTimer();
    void refreshLoadedImages();
    bool isOriginalAccountCurrent() const;
    void stopForAccountChange();
    QString storedSevenTVTokenForCurrentAccount() const;
    void clearSevenTVConnection();
    void releaseAutomaticPin();
    void updateSaveButtonState();
    void setBusy(bool busy);
    void setStatus(const QString &text, bool error = false);
    QString selectedSevenTVBadgeId() const;
    QString selectedSevenTVPaintId() const;
    const GqlVanityBadge *findGlobalBadge(const QString &key) const;
    const GqlVanityBadge *findChannelBadge(const QString &key) const;
    const TwitchEventBadge *findTwitchEventBadge(const QString &key) const;
    const SevenTVVanityCosmetic *findSevenTVBadge(const QString &id) const;
    MoltorinoVanityLayout editedLayout() const;
    QString layoutDisplayName(const QString &key) const;
    QString sevenTVTokenExpiryText() const;

    std::shared_ptr<TwitchChannel> channel_;
    GqlVanityState twitchState_;
    QVector<SevenTVVanityCosmetic> sevenTVBadges_;
    QVector<SevenTVVanityCosmetic> sevenTVPaints_;
    QVector<TwitchEventBadge> twitchEventBadges_;

    QVBoxLayout *mainLayout_{};
    QWidget *headerWidget_{};
    QLabel *headerTitleLabel_{};
    QLabel *headerIdentityLabel_{};
    Button *pinButton_{};
    SvgButton *closeButton_{};
    QWidget *previewFrame_{};
    VanityPreviewWidget *preview_{};
    QLabel *statusLabel_{};
    QTabWidget *tabs_{};
    QWidget *colorPage_{};
    QWidget *paintPage_{};
    QWidget *globalBadgePage_{};
    QWidget *channelBadgePage_{};
    QWidget *sevenTVBadgePage_{};
    QWidget *moltorinoBadgePage_{};
    QWidget *layoutPage_{};
    QButtonGroup *colorButtons_{};
    QGridLayout *colorSwatchLayout_{};
    VanityColorPicker *colorPicker_{};
    VanityColorPreview *colorPreviewChip_{};
    QLineEdit *colorHexInput_{};
    QVector<QLabel *> sevenTVConnectionLabels_;
    QVector<QPushButton *> sevenTVConnectButtons_;
    QVector<QPushButton *> sevenTVDisconnectButtons_;
    QVector<QPushButton *> twitchRetryButtons_;
    QVector<SvgButton *> sevenTVRefreshButtons_;
    VanityChoiceGrid *paintGrid_{};
    VanityChoiceGrid *globalBadgeGrid_{};
    VanityChoiceGrid *channelBadgeGrid_{};
    VanityChoiceGrid *sevenTVBadgeGrid_{};
    VanityChoiceGrid *moltorinoBadgeGrid_{};
    QLabel *globalBadgeHint_{};
    QLabel *channelBadgeHint_{};
    QLabel *sevenTVBadgeHint_{};
    QLabel *moltorinoBadgeHint_{};
    QLabel *paintHint_{};
    QLabel *layoutHintLabel_{};
    QPushButton *layoutResetButton_{};
    QPushButton *layoutRetryButton_{};
    QListWidget *layoutList_{};
    QPushButton *saveButton_{};
    QTimer *visualRefreshTimer_{};
    bool responsiveLayoutRefreshPending_ = false;
    pajlada::Signals::SignalHolder managedConnections_;

    QColor originalColor_;
    QColor selectedColor_;
    QString selectedColorValue_;
    QString originalGlobalBadge_;
    QString originalChannelBadge_;
    QString originalSevenTVBadge_;
    QString originalSevenTVPaint_;
    QString selectedMoltorinoBadge_;
    QString selectedGlobalBadge_;
    QString selectedChannelBadge_;
    QString selectedSevenTVBadge_;
    QString selectedSevenTVPaint_;
    QString previewMoltorinoBadge_;
    QString previewTwitchEventBadge_;
    QString accountUserId_;
    QString accountLogin_;
    QString accountDisplayName_;
    QString accountToken_;
    QString accountClientId_;
    QString twitchBadgeHint_;
    QString twitchBadgeError_;
    QString sevenTVUserId_;
    QString sevenTVLogin_;
    QString sevenTVToken_;
    QString pendingSevenTVToken_;
    QStringList savedChanges_;
    std::vector<QString> assignedMoltorinoBadgeIds_;
    std::vector<TwitchGqlAuth> twitchAuthCandidates_;
    std::optional<TwitchGqlAuth> activeTwitchAuth_;
    MoltorinoVanityLayout originalLayout_;
    MoltorinoVanityLayout workingLayout_;
    QVector<ActiveBadgeRow> activeBadgeRows_;

    bool twitchLoaded_ = false;
    bool twitchLoadFinished_ = false;
    bool twitchRequestInFlight_ = false;
    bool twitchEventBadgesRequested_ = false;
    bool twitchEventBadgeRetryScheduled_ = false;
    bool twitchEventBadgeAutomaticRetryUsed_ = false;
    bool previewTwitchEventForChannel_ = false;
    bool layoutLoaded_ = false;
    bool layoutRequestInFlight_ = false;
    bool sevenTVLoaded_ = false;
    bool sevenTVRequestInFlight_ = false;
    bool sevenTVBadgeTouched_ = false;
    bool sevenTVPaintTouched_ = false;
    bool saveInFlight_ = false;
    bool accountChanged_ = false;
    quint64 sevenTVAuthGeneration_ = 0;
    quint64 saveSevenTVAuthGeneration_ = 0;
    bool layoutTouched_ = false;
    bool layoutNeedsSync_ = false;
    int layoutRevision_ = 0;
    bool autoPinnedForSave_ = false;
    bool paintChoicesBuilt_ = false;
    bool globalBadgeChoicesBuilt_ = false;
    bool channelBadgeChoicesBuilt_ = false;
    bool sevenTVBadgeChoicesBuilt_ = false;
    bool moltorinoBadgeChoicesBuilt_ = false;
    bool moltorinoAssignmentsAuthoritative_ = false;
    bool rebuildingLayoutRows_ = false;
    int twitchAuthCandidateIndex_ = 0;
    int visualSettleTicks_ = 0;

    static QPointer<VanityDialog> activeDialog_;
};

}
