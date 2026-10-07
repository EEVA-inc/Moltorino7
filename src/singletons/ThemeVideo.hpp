#pragma once

#include <pajlada/signals/scoped-connection.hpp>
#include <QElapsedTimer>
#include <QMap>
#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QTimer>
#include <QVector>

#include <memory>

class QPainter;
class QWidget;

namespace chatterino {

struct ThemeCustomizationProfile;
class ThemeVideoDecoder;

class ThemeVideo final : public QObject
{
    Q_OBJECT

public:
    static std::shared_ptr<ThemeVideo> acquire(const QString &source,
                                               QWidget *consumer);
    ~ThemeVideo() override;
    void detach(QWidget *consumer);
    QImage currentFrame() const;
    bool paint(QPainter &painter, const QRectF &target,
               const ThemeCustomizationProfile &profile);

Q_SIGNALS:
    void frameChanged();

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    explicit ThemeVideo(QString source);
    bool shouldPlay() const;
    void updatePlayback();
    void decode();

    QString source_;
    std::shared_ptr<ThemeVideoDecoder> decoder_;
    QVector<QPointer<QWidget>> consumers_;
    QPixmap frame_;
    QMap<int, QPixmap> blurred_;
    QTimer timer_;
    QElapsedTimer clock_;
    qint64 deadline_ = 0;
    bool decoding_ = false;
    bool failed_ = false;
    int frameCount_ = 0;
    pajlada::Signals::ScopedConnection focusSettingConnection_;
};

}
