#include "singletons/ThemeVideoImport.hpp"

#include "singletons/ThemeVideoDecoder.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <cmath>

namespace chatterino {
namespace {

const QStringList INPUT_OPTIONS{
    QStringLiteral("-v"), QStringLiteral("error"),
    QStringLiteral("-protocol_whitelist"), QStringLiteral("file,pipe"),
    QStringLiteral("-probesize"), QStringLiteral("5000000"),
    QStringLiteral("-analyzeduration"), QStringLiteral("5000000"),
};

QString mediaTool(const QString &name)
{
    auto filename = name;
#ifdef Q_OS_WIN
    filename += QStringLiteral(".exe");
#endif

    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("support/media/") + filename);
}

bool runTool(const QString &name, const QStringList &arguments,
             std::stop_token cancellation, QByteArray *output, int timeoutMs,
             const QString &destination = {})
{
    if (cancellation.stop_requested())
    {
        return false;
    }
    QProcess process;
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) {
            args->flags |= 0x08000000;
        });
#endif
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    process.setProgram(mediaTool(name));
    process.setArguments(arguments);
    QElapsedTimer timer;
    timer.start();
    process.start();
    if (!process.waitForStarted(5000))
    {
        return false;
    }
    while (process.state() != QProcess::NotRunning)
    {
        process.waitForFinished(50);
        const auto bytes = process.readAllStandardOutput();
        if (output)
        {
            output->append(bytes);
        }
        if (cancellation.stop_requested() || timer.elapsed() > timeoutMs ||
            (output && output->size() > 256 * 1024) ||
            (!destination.isEmpty() &&
             QFileInfo(destination).size() > THEME_BACKGROUND_BYTES))
        {
            process.kill();
            process.waitForFinished(5000);
            return false;
        }
    }
    if (output)
    {
        output->append(process.readAllStandardOutput());
    }
    if (cancellation.stop_requested() ||
        (output && output->size() > 256 * 1024))
    {
        return false;
    }
    return process.exitStatus() == QProcess::NormalExit &&
           process.exitCode() == 0;
}

double ratio(const QString &text, QChar separator)
{
    const auto parts = text.split(separator);
    if (parts.size() != 2)
    {
        return 0;
    }
    const double numerator = parts[0].toDouble();
    const double denominator = parts[1].toDouble();
    return denominator > 0 ? numerator / denominator : 0;
}

}

bool isThemeVideoInput(const QString &source)
{
    return themeVideoInputExtensions().contains(
        QFileInfo(source).suffix().toLower());
}

QStringList themeVideoInputExtensions()
{
    return {
        QStringLiteral("mp4"), QStringLiteral("m4v"), QStringLiteral("mov"),
        QStringLiteral("mkv"), QStringLiteral("avi"), QStringLiteral("webm"),
        QStringLiteral("gif"), QStringLiteral("3gp"), QStringLiteral("3g2"),
        QStringLiteral("qt"),  QStringLiteral("mk3d")};
}

ThemeVideoInfo inspectThemeVideo(const QString &source,
                                 std::stop_token cancellation)
{
    const auto cancelled = [] {
        return ThemeVideoInfo{
            QStringLiteral("Background preparation cancelled.")};
    };
    if (cancellation.stop_requested())
    {
        return cancelled();
    }
    const bool gif = QFileInfo(source).suffix().compare(
                         QStringLiteral("gif"), Qt::CaseInsensitive) == 0;
    if (!QFileInfo(source).isFile() ||
        QFileInfo(source).size() >
            (gif ? THEME_BACKGROUND_BYTES : 1024LL * 1024 * 1024))
    {
        if (gif)
        {
            return {QStringLiteral("Choose a background smaller than 50 MB.")};
        }
        return {QStringLiteral("Choose a video smaller than 1 GB.")};
    }
    if (!QFileInfo::exists(mediaTool(QStringLiteral("ffmpeg"))) ||
        !QFileInfo::exists(mediaTool(QStringLiteral("ffprobe"))))
    {
        return {QStringLiteral("The video converter is missing. Reinstall "
                               "Moltorino and try again.")};
    }

    QByteArray metadata;
    auto probe = INPUT_OPTIONS;
    probe << QStringLiteral("-select_streams") << QStringLiteral("v:0")
          << QStringLiteral("-show_entries")
          << QStringLiteral(
                 "stream=width,height,sample_aspect_ratio,avg_frame_rate,color_"
                 "transfer:stream_side_data=rotation:format=duration")
          << QStringLiteral("-of") << QStringLiteral("json")
          << QFileInfo(source).absoluteFilePath();
    if (!runTool(QStringLiteral("ffprobe"), probe, cancellation, &metadata,
                 30000))
    {
        return cancellation.stop_requested()
                   ? cancelled()
                   : ThemeVideoInfo{
                         QStringLiteral("Moltorino couldn't read that video.")};
    }
    const auto document = QJsonDocument::fromJson(metadata).object();
    const auto streams = document.value(QStringLiteral("streams")).toArray();
    const auto stream =
        streams.isEmpty() ? QJsonObject() : streams[0].toObject();
    const int width = stream.value(QStringLiteral("width")).toInt();
    const int height = stream.value(QStringLiteral("height")).toInt();
    const double duration = document.value(QStringLiteral("format"))
                                .toObject()
                                .value(QStringLiteral("duration"))
                                .toString()
                                .toDouble();
    if (width < (gif ? 1 : 2) || height < (gif ? 1 : 2) || width > 8192 ||
        height > 8192 || qint64(width) * height > 32LL * 1024 * 1024 ||
        !std::isfinite(duration) || duration <= 0)
    {
        return {QStringLiteral("Choose a video with a valid duration and under "
                               "32 megapixels.")};
    }
    const auto transfer =
        stream.value(QStringLiteral("color_transfer")).toString();
    if (transfer == QStringLiteral("smpte2084") ||
        transfer == QStringLiteral("arib-std-b67"))
    {
        return {QStringLiteral("Choose an SDR version of that video so its "
                               "colors stay correct.")};
    }

    double sar = ratio(
        stream.value(QStringLiteral("sample_aspect_ratio")).toString(), u':');
    if (sar <= 0 || !std::isfinite(sar))
    {
        sar = 1;
    }
    if (sar < 0.01 || sar > 100)
    {
        return {QStringLiteral("That video has an unsupported aspect ratio.")};
    }
    double displayWidth = width * sar;
    double displayHeight = height;
    for (const auto &item :
         stream.value(QStringLiteral("side_data_list")).toArray())
    {
        const int rotation =
            item.toObject().value(QStringLiteral("rotation")).toInt();
        if (std::abs(rotation % 180) == 90)
        {
            std::swap(displayWidth, displayHeight);
        }
    }
    const double scale = std::min(
        {1.0, THEME_VIDEO_DIMENSION / std::max(displayWidth, displayHeight),
         std::sqrt(THEME_VIDEO_PIXELS / (displayWidth * displayHeight))});
    const int targetWidth = std::max(2, int(displayWidth * scale) / 2 * 2);
    const int targetHeight = std::max(2, int(displayHeight * scale) / 2 * 2);
    double fps =
        ratio(stream.value(QStringLiteral("avg_frame_rate")).toString(), u'/');
    if (!std::isfinite(fps) || fps <= 0)
    {
        fps = 24;
    }
    fps = std::clamp(fps, 1.0, 24.0);

    return {{}, duration, QSize(targetWidth, targetHeight), fps};
}

ThemeWallpaperData prepareThemeVideo(const QString &source,
                                     std::stop_token cancellation, int seconds)
{
    const auto info = inspectThemeVideo(source, cancellation);
    if (!info.error.isEmpty())
    {
        return {{}, info.error};
    }
    if (seconds < 0 || seconds > THEME_VIDEO_DURATION_MS / 1000 ||
        (seconds == 0 && info.duration > THEME_VIDEO_DURATION_MS / 1000))
    {
        return {
            {},
            QStringLiteral(
                "Choose a video up to 2 minutes long or use a shorter loop.")};
    }
    const auto duration =
        seconds > 0 ? std::min(info.duration, double(seconds)) : info.duration;
    const int targetWidth = info.outputSize.width();
    const int targetHeight = info.outputSize.height();
    const double fps = info.framesPerSecond;
    QTemporaryDir temporary;
    if (!temporary.isValid())
    {
        return {{},
                QStringLiteral(
                    "Moltorino couldn't create a temporary video file.")};
    }
    const auto destination =
        temporary.filePath(QStringLiteral("background.webm"));
    auto arguments = INPUT_OPTIONS;

    arguments
        << QStringLiteral("-nostdin") << QStringLiteral("-y")
        << QStringLiteral("-filter_threads") << QStringLiteral("2")
        << QStringLiteral("-threads") << QStringLiteral("2")
        << QStringLiteral("-i") << QFileInfo(source).absoluteFilePath()
        << QStringLiteral("-map") << QStringLiteral("0:v:0")
        << QStringLiteral("-an") << QStringLiteral("-sn")
        << QStringLiteral("-dn") << QStringLiteral("-map_metadata")
        << QStringLiteral("-1") << QStringLiteral("-map_chapters")
        << QStringLiteral("-1") << QStringLiteral("-t")
        << QString::number(duration, 'f', 6) << QStringLiteral("-vf")
        << QStringLiteral(
               "setpts=PTS-STARTPTS,fps=%3,scale=%1:%2:flags=lanczos:"
               "out_color_matrix=bt601:out_range=tv,setsar=1,format=yuv420p")
               .arg(targetWidth)
               .arg(targetHeight)
               .arg(fps, 0, 'f', 6)
        << QStringLiteral("-c:v") << QStringLiteral("libvpx")
        << QStringLiteral("-quality") << QStringLiteral("good")
        << QStringLiteral("-cpu-used") << QStringLiteral("2")
        << QStringLiteral("-crf") << QStringLiteral("10")
        << QStringLiteral("-b:v") << QStringLiteral("6M")
        << QStringLiteral("-auto-alt-ref") << QStringLiteral("0")
        << QStringLiteral("-lag-in-frames") << QStringLiteral("0")
        << QStringLiteral("-g") << QStringLiteral("240")
        << QStringLiteral("-threads")
        << QString::number(std::clamp(QThread::idealThreadCount() / 2, 1, 4))
        << QStringLiteral("-color_range") << QStringLiteral("tv")
        << QStringLiteral("-colorspace") << QStringLiteral("smpte170m")
        << destination;
    if (!runTool(QStringLiteral("ffmpeg"), arguments, cancellation, nullptr,
                 20 * 60 * 1000, destination))
    {
        return cancellation.stop_requested()
                   ? ThemeWallpaperData{{},
                                        QStringLiteral("Background preparation "
                                                       "cancelled.")}
                   : ThemeWallpaperData{
                         {},
                         QStringLiteral("Couldn't prepare that video. Try a "
                                        "shorter or less detailed clip.")};
    }
    const auto error = validateThemeVideo(destination, cancellation);
    if (!error.isEmpty())
    {
        return {{}, error};
    }
    QFile file(destination);
    if (!file.open(QIODevice::ReadOnly) || file.size() > THEME_BACKGROUND_BYTES)
    {
        return {
            {},
            QStringLiteral(
                "The prepared video exceeds 50 MB. Choose a shorter clip.")};
    }
    auto bytes = file.read(THEME_BACKGROUND_BYTES + 1);
    if (bytes.size() > THEME_BACKGROUND_BYTES || !file.atEnd() ||
        file.error() != QFile::NoError)
    {
        return {{}, QStringLiteral("Moltorino couldn't read that background.")};
    }
    return {std::move(bytes), {}, QStringLiteral("webm")};
}

}
