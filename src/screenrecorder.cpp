/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "screenrecorder.h"
#include "kwinscreencast.h"
#include "x11windows.h"

#include <QGuiApplication>
#include <QScreen>
#include <QTimer>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QStandardPaths>
#include <QDesktopServices>
#include <QUrl>
#include <QQuickItem>
#include "application.h"
#include "itemwindow.h"
#include <QSurfaceFormat>
#include <QRasterWindow>
#include <QPainter>
#include <QRegion>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QMessageBox>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QLoggingCategory>
#include <QRegularExpression>

#include <signal.h>

Q_LOGGING_CATEGORY(lcRecorder, "lingmo.screenshot.recorder", QtInfoMsg)

namespace {

const char *NOTIFY_SERVICE = "org.freedesktop.Notifications";
const char *NOTIFY_PATH = "/org/freedesktop/Notifications";
const char *NOTIFY_INTERFACE = "org.freedesktop.Notifications";

// How long to wait for ffmpeg to finalize the file before interrupting it.
const int STOP_TIMEOUT = 5000;
// How long to stay alive so that the notification actions keep working.
const int NOTIFICATION_TIMEOUT = 2 * 60 * 1000;

// A small ring drawn around the pointer when a mouse button is pressed.
// It uses a shaped window so that it also works without a compositor.
class ClickRipple : public QRasterWindow
{
public:
    explicit ClickRipple(QScreen *screen, const QPoint &center)
        : m_step(0)
    {
        setFlags(Qt::FramelessWindowHint | Qt::X11BypassWindowManagerHint
                 | Qt::WindowStaysOnTopHint | Qt::WindowTransparentForInput
                 | Qt::WindowDoesNotAcceptFocus);
        setScreen(screen);

        const int size = SIZE;
        setGeometry(center.x() - size / 2, center.y() - size / 2, size, size);
        updateMask();

        QTimer *timer = new QTimer(this);
        timer->setInterval(30);
        connect(timer, &QTimer::timeout, this, [this, timer] {
            if (++m_step >= STEPS) {
                timer->stop();
                close();
                deleteLater();
                return;
            }
            updateMask();
        });
        timer->start();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(QRect(QPoint(0, 0), size()), QColor(255, 179, 0));
    }

private:
    void updateMask()
    {
        const int outer = 10 + (SIZE / 2 - 10) * m_step / (STEPS - 1);
        const int inner = qMax(0, outer - 4);
        const QPoint c(SIZE / 2, SIZE / 2);

        QRegion region(QRect(c.x() - outer, c.y() - outer, outer * 2, outer * 2), QRegion::Ellipse);
        region -= QRegion(QRect(c.x() - inner, c.y() - inner, inner * 2, inner * 2), QRegion::Ellipse);
        setMask(region);
    }

    static const int SIZE = 44;
    static const int STEPS = 12;
    int m_step;
};

}

ScreenRecorder::ScreenRecorder(QObject *parent)
    : QObject(parent)
    , m_process(nullptr)
    , m_converter(nullptr)
    , m_busy(false)
    , m_stopRequested(false)
    , m_failed(false)
    , m_tickTimer(new QTimer(this))
    , m_killTimer(new QTimer(this))
    , m_showingClicks(false)
    , m_indicator(nullptr)
    , m_trayIcon(nullptr)
    , m_notificationId(0)
{
    m_tickTimer->setInterval(500);
    connect(m_tickTimer, &QTimer::timeout, this, &ScreenRecorder::elapsedChanged);

    m_killTimer->setSingleShot(true);
    m_killTimer->setInterval(STOP_TIMEOUT);
    connect(m_killTimer, &QTimer::timeout, this, [this] {
        if (!m_process || m_process->state() == QProcess::NotRunning)
            return;

        if (m_process->property("interrupted").toBool()) {
            qCWarning(lcRecorder) << "ffmpeg did not exit, killing it";
            m_process->kill();
        } else {
            qCWarning(lcRecorder) << "ffmpeg did not exit, sending SIGINT";
            m_process->setProperty("interrupted", true);
            ::kill(m_process->processId(), SIGINT);
            m_killTimer->start();
        }
    });
}

ScreenRecorder::~ScreenRecorder()
{
    delete m_indicator;
    showClicks(false);

    if (m_process && m_process->state() != QProcess::NotRunning) {
        // ffmpeg quits on "q"; gst-launch -e finishes the file on SIGINT
        if (m_wayland)
            ::kill(m_process->processId(), SIGINT);
        else
            m_process->write("q");
        if (!m_process->waitForFinished(STOP_TIMEOUT))
            m_process->kill();
    }
}

QString ScreenRecorder::outputDirectory()
{
    QString movies = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (movies.isEmpty())
        movies = QDir::homePath();

    return QDir(movies).filePath(tr("Screen Recordings"));
}

QString ScreenRecorder::defaultMonitorSource()
{
    QProcess pactl;
    pactl.start("pactl", QStringList() << "get-default-sink");

    if (pactl.waitForFinished(2000) && pactl.exitStatus() == QProcess::NormalExit && pactl.exitCode() == 0) {
        const QString sink = QString::fromUtf8(pactl.readAllStandardOutput()).trimmed();
        if (!sink.isEmpty())
            return sink + ".monitor";
    }

    // Understood by PulseAudio and pipewire-pulse.
    return QStringLiteral("@DEFAULT_MONITOR@");
}

QStringList ScreenRecorder::ffmpegArguments(const QString &display, const QRect &rect,
                                            const Options &options, const QString &monitorSource,
                                            const QString &fileName)
{
    QStringList args;
    args << "-hide_banner" << "-loglevel" << "error" << "-nostats" << "-y";

    // Video input
    args << "-thread_queue_size" << "512"
         << "-f" << "x11grab"
         << "-framerate" << QString::number(options.frameRate)
         << "-draw_mouse" << "1"
         << "-video_size" << QString("%1x%2").arg(rect.width()).arg(rect.height())
         << "-i" << QString("%1+%2,%3").arg(display).arg(rect.x()).arg(rect.y());

    // Audio inputs
    int audioInputs = 0;
    if (options.microphone) {
        args << "-thread_queue_size" << "1024" << "-f" << "pulse" << "-i" << "default";
        ++audioInputs;
    }
    if (options.systemAudio) {
        args << "-thread_queue_size" << "1024" << "-f" << "pulse" << "-i" << monitorSource;
        ++audioInputs;
    }

    if (audioInputs == 2) {
        args << "-filter_complex" << "[1:a][2:a]amix=inputs=2:duration=longest:normalize=0[aout]"
             << "-map" << "0:v" << "-map" << "[aout]";
    } else if (audioInputs == 1) {
        args << "-map" << "0:v" << "-map" << "1:a";
    }

    args << "-c:v" << "libx264" << "-preset" << "veryfast" << "-crf" << "23"
         << "-pix_fmt" << "yuv420p";

    if (audioInputs > 0)
        args << "-c:a" << "aac" << "-b:a" << "160k";

    args << "-movflags" << "+faststart" << fileName;

    return args;
}

QStringList ScreenRecorder::gifArguments(const QString &input, const QString &output)
{
    // Two passes in one: a palette made from the clip, then dithering with it.
    return { QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
             QStringLiteral("-y"), QStringLiteral("-i"), input,
             QStringLiteral("-vf"),
             QStringLiteral("fps=15,scale='min(1280,iw)':-2:flags=lanczos,split[a][b];"
                            "[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=5"),
             QStringLiteral("-loop"), QStringLiteral("0"), output };
}

// GStreamer quotes values with spaces itself: gst-launch joins its arguments
static QString gstQuoted(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QStringList ScreenRecorder::gstArguments(const QString &target, const QSize &size, const Options &options,
                                         const QString &monitorSource, const QString &fileName)
{
    QStringList pipeline;
    // The same file as ffmpeg's on X11: H.264 (x264, veryfast, quality 23) and AAC in MP4
    pipeline << QStringLiteral("pipewiresrc target-object=%1 do-timestamp=true keepalive-time=1000 always-copy=true").arg(target)
             << QStringLiteral("! videoconvert ! videoscale ! videorate")
             << QStringLiteral("! video/x-raw,format=I420,width=%1,height=%2,framerate=%3/1")
                    .arg(size.width()).arg(size.height()).arg(options.frameRate)
             << QStringLiteral("! queue max-size-buffers=0 max-size-bytes=0 max-size-time=2000000000")
             << QStringLiteral("! x264enc speed-preset=veryfast pass=quant quantizer=23 ! video/x-h264,profile=high")
             << QStringLiteral("! h264parse ! queue ! mux.");

    QStringList sources;
    if (options.microphone)
        sources << QStringLiteral("pulsesrc do-timestamp=true");
    if (options.systemAudio)
        sources << QStringLiteral("pulsesrc device=%1 do-timestamp=true").arg(gstQuoted(monitorSource));
    const QString encode = QStringLiteral("audioconvert ! audioresample ! avenc_aac bitrate=160000 ! aacparse ! queue ! mux.");
    if (sources.size() == 2) {
        pipeline << QStringLiteral("audiomixer name=amix ! ") + encode;
        for (const QString &source : sources)
            pipeline << source + QStringLiteral(" ! queue ! audioconvert ! audioresample ! amix.");
    } else if (sources.size() == 1) {
        pipeline << sources.first() + QStringLiteral(" ! queue ! ") + encode;
    }

    pipeline << QStringLiteral("mp4mux name=mux faststart=true ! filesink location=%1").arg(gstQuoted(fileName));

    QStringList args{QStringLiteral("-e"), QStringLiteral("-q")};
    for (const QString &part : pipeline)
        args += part.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    return args;
}

bool ScreenRecorder::waylandRecordingAvailable()
{
    static const bool gst = [] {
        if (QStandardPaths::findExecutable(QStringLiteral("gst-launch-1.0")).isEmpty())
            return false;
        QProcess inspect;
        inspect.start(QStringLiteral("gst-inspect-1.0"), {QStringLiteral("--exists"), QStringLiteral("pipewiresrc")});
        return inspect.waitForFinished(5000) && inspect.exitStatus() == QProcess::NormalExit && inspect.exitCode() == 0;
    }();
    return gst && KWinScreencast::available();
}

bool ScreenRecorder::busy() const
{
    return m_busy;
}

bool ScreenRecorder::stopRunningRecording()
{
    QDBusMessage msg = QDBusMessage::createMethodCall(RECORDER_SERVICE, RECORDER_PATH,
                                                      RECORDER_INTERFACE, "StopRecording");
    QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 5000);
    return reply.type() == QDBusMessage::ReplyMessage;
}

void ScreenRecorder::start(QScreen *screen, const QRect &rect, const Options &options)
{
    if (m_busy)
        return;

    m_busy = true;
    m_screen = screen;
    m_options = options;
    m_stopRequested = false;
    m_failed = false;
    m_errorOutput.clear();

    m_wayland = QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (m_wayland ? !waylandRecordingAvailable() : ffmpeg.isEmpty()) {
        showError(m_wayland ? tr("Recording the screen needs GStreamer's PipeWire plugin. Install the gst-plugin-pipewire package.")
                            : tr("FFmpeg was not found. Install the ffmpeg package to record the screen."));
        return;
    }

    // yuv420p needs even dimensions.
    m_rect = rect;
    if (screen) {
        const QRect nativeScreen(screen->geometry().topLeft(), screen->geometry().size() * screen->devicePixelRatio());
        m_rect = m_rect.intersected(nativeScreen);
    }
    m_rect.setWidth(m_rect.width() & ~1);
    m_rect.setHeight(m_rect.height() & ~1);

    if (m_rect.width() < 2 || m_rect.height() < 2) {
        showError(tr("The selected area is too small to be recorded."));
        return;
    }

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.registerService(RECORDER_SERVICE)) {
        showError(tr("Another screen recording is already in progress."));
        return;
    }
    bus.registerObject(RECORDER_PATH, this, QDBusConnection::ExportScriptableSlots);

    const QString dirPath = outputDirectory();
    if (!QDir().mkpath(dirPath)) {
        showError(tr("Unable to create the folder %1.").arg(dirPath));
        return;
    }

    const QString baseName = tr("Screen Recording %1")
            .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh-mm-ss"));
    m_fileName = QDir(dirPath).filePath(baseName + ".mp4");
    for (int i = 2; QFile::exists(m_fileName); ++i)
        m_fileName = QDir(dirPath).filePath(QString("%1 (%2).mp4").arg(baseName).arg(i));

    const QString monitor = options.systemAudio ? defaultMonitorSource() : QString();

    if (m_wayland) {
        // KWin streams the area (logical coordinates, at the screen's own pixels);
        // GStreamer starts once the PipeWire stream is there
        const qreal dpr = screen ? screen->devicePixelRatio() : 1.0;
        const QPoint origin = screen ? screen->geometry().topLeft() : QPoint();
        const QRect logical(origin + (m_rect.topLeft() - origin) / dpr, m_rect.size() / dpr);
        if (!m_screencast)
            m_screencast = new KWinScreencast(this);
        m_screencast->disconnect(this);
        connect(m_screencast, &KWinScreencast::failed, this, [this](const QString &error) {
            showError(tr("Unable to start the screen recording.") + QStringLiteral("\n\n") + error);
        });
        // KWin ended it (an output went away, the screen locked): keep what was recorded
        connect(m_screencast, &KWinScreencast::closed, this, &ScreenRecorder::stop);
        connect(m_screencast, &KWinScreencast::ready, this, [this, options, monitor](const QString &target) {
            if (m_process || m_failed)
                return;
            startRecorderProcess(QStandardPaths::findExecutable(QStringLiteral("gst-launch-1.0")),
                                 gstArguments(target, m_rect.size(), options, monitor, m_fileName));
        });
        m_screencast->streamRegion(logical, dpr);
        return;
    }

    QString display = qEnvironmentVariable("DISPLAY");
    if (display.isEmpty())
        display = ":0";

    startRecorderProcess(ffmpeg, ffmpegArguments(display, m_rect, options, monitor, m_fileName));
}

void ScreenRecorder::startRecorderProcess(const QString &program, const QStringList &args)
{
    qCInfo(lcRecorder).noquote() << "Running:" << program << args.join(' ');

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_process, &QProcess::started, this, &ScreenRecorder::onStarted);
    connect(m_process, &QProcess::finished, this, &ScreenRecorder::onFinished);
    connect(m_process, &QProcess::errorOccurred, this, &ScreenRecorder::onErrorOccurred);
    connect(m_process, &QProcess::readyReadStandardError, this, [this] {
        m_errorOutput += m_process->readAllStandardError();
        m_errorOutput = m_errorOutput.right(4096);
    });
    m_process->start(program, args);
}

int ScreenRecorder::elapsed() const
{
    return m_elapsedTimer.isValid() ? int(m_elapsedTimer.elapsed() / 1000) : 0;
}

void ScreenRecorder::StopRecording()
{
    stop();
}

bool ScreenRecorder::IsRecording() const
{
    return isRecording();
}

bool ScreenRecorder::isRecording() const
{
    return m_process && m_process->state() == QProcess::Running;
}

void ScreenRecorder::stop()
{
    if (!isRecording() || m_stopRequested)
        return;

    qCInfo(lcRecorder) << "Stopping the recording";

    m_stopRequested = true;
    showClicks(false);
    hideIndicator();

    // Ask the recorder to quit so that the mp4 gets finalized: ffmpeg on "q",
    // gst-launch -e on SIGINT (it sends end-of-stream down the pipeline)
    if (m_wayland) {
        m_process->setProperty("interrupted", true);
        ::kill(m_process->processId(), SIGINT);
    } else {
        m_process->write("q");
    }
    m_killTimer->start();
}

void ScreenRecorder::onStarted()
{
    m_elapsedTimer.start();
    m_tickTimer->start();

    if (m_options.showClicks)
        showClicks(true);

    showIndicator();

    emit recordingChanged();
    emit elapsedChanged();
}

void ScreenRecorder::onFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    m_errorOutput += m_process->readAllStandardError();
    m_killTimer->stop();
    m_tickTimer->stop();
    showClicks(false);
    hideIndicator();

    qCInfo(lcRecorder) << "recorder finished, exit code" << exitCode << exitStatus;
    if (m_screencast)
        m_screencast->close();

    const QFileInfo file(m_fileName);

    if (m_stopRequested && file.exists() && file.size() > 0) {
        if (m_options.gif)
            convertToGif();
        else
            notifySaved();
    } else {
        if (file.exists() && file.size() == 0)
            QFile::remove(m_fileName);

        QString message = m_elapsedTimer.isValid() && m_elapsedTimer.elapsed() > 3000
                ? tr("The screen recording stopped unexpectedly.")
                : tr("Unable to start the screen recording.");

        const QString details = QString::fromLocal8Bit(m_errorOutput).trimmed();
        if (!details.isEmpty())
            message += "\n\n" + details.split('\n').mid(-6).join('\n');

        showError(message);
    }

    emit recordingChanged();
}

void ScreenRecorder::onErrorOccurred(QProcess::ProcessError error)
{
    if (error != QProcess::FailedToStart)
        return;

    showError(tr("Unable to start FFmpeg: %1").arg(m_process->errorString()));
}

void ScreenRecorder::showClicks(bool enable)
{
    if (enable == m_showingClicks)
        return;

    if (enable) {
        // XInput 2 raw events: X11 only
        if (m_wayland)
            return;
        if (!X11Windows::selectRawButtonPress(true)) {
            qCWarning(lcRecorder) << "XInput2 is not available, clicks will not be shown";
            return;
        }
        qGuiApp->installNativeEventFilter(this);
    } else {
        qGuiApp->removeNativeEventFilter(this);
        X11Windows::selectRawButtonPress(false);
    }

    m_showingClicks = enable;
}

bool ScreenRecorder::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
    Q_UNUSED(result)

    if (eventType != "xcb_generic_event_t" || !X11Windows::isRawButtonPress(message))
        return false;

    QPoint pos;
    if (m_screen && X11Windows::pointerPosition(&pos)) {
        // Root coordinates to logical coordinates.
        const QPoint origin = m_screen->geometry().topLeft();
        const QPoint logical = origin + (pos - origin) / m_screen->devicePixelRatio();
        ClickRipple *ripple = new ClickRipple(m_screen, logical);
        ripple->show();
    }

    return true;
}

void ScreenRecorder::showIndicator()
{
    QScreen *screen = m_screen ? m_screen.data() : qGuiApp->primaryScreen();

    // Wayland: KWin records everything in the area, a floating indicator too, and
    // won't let it be placed outside: the tray icon (click to stop) is enough
    if (m_wayland) {
        showTrayIcon();
        return;
    }

    if (!m_indicator) {
        m_indicator = new ItemWindow;
        m_indicator->setFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                              | Qt::Tool | Qt::WindowDoesNotAcceptFocus);
        m_indicator->setTitle(tr("Screen recording"));
        QSurfaceFormat format = m_indicator->format();
        format.setAlphaBufferSize(8);
        m_indicator->setFormat(format);
        m_indicator->setColor(Qt::transparent);
        // "recorder" is in the root context of the shared engine.
        QObject *object = Application::instance()->createQml(QStringLiteral("qrc:/qml/RecordingIndicator.qml"),
                                                             QVariantMap(), this);
        if (auto *item = qobject_cast<QQuickItem *>(object))
            m_indicator->setItem(item, true);
        else
            delete object;
    }

    m_indicator->setScreen(screen);

    // Keep the indicator out of the recorded area when possible.
    const QRect available = screen->availableGeometry();
    const qreal dpr = screen->devicePixelRatio();
    const QPoint origin = screen->geometry().topLeft();
    const QRect recorded(origin + (m_rect.topLeft() - origin) / dpr, m_rect.size() / dpr);
    const QSize size = m_indicator->size();
    const int margin = 16;

    const QList<QPoint> candidates {
        QPoint(available.right() - size.width() - margin, available.top() + margin),
        QPoint(available.right() - size.width() - margin, available.bottom() - size.height() - margin),
        QPoint(available.left() + margin, available.top() + margin),
        QPoint(available.left() + margin, available.bottom() - size.height() - margin),
    };

    QPoint pos = candidates.first();
    for (const QPoint &candidate : candidates) {
        if (!recorded.intersects(QRect(candidate, size))) {
            pos = candidate;
            break;
        }
    }

    m_indicator->setPosition(pos);
    m_indicator->show();

    showTrayIcon();
}

void ScreenRecorder::showTrayIcon()
{
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        if (!m_trayIcon) {
            m_trayIcon = new QSystemTrayIcon(QIcon::fromTheme("media-record", QIcon(":/images/record.svg")), this);
            m_trayIcon->setToolTip(tr("Screen recording in progress. Click to stop."));

            QMenu *menu = new QMenu;
            menu->addAction(QIcon::fromTheme("media-playback-stop"), tr("Stop recording"), this, &ScreenRecorder::stop);
            m_trayIcon->setContextMenu(menu);
            connect(m_trayIcon, &QObject::destroyed, menu, &QObject::deleteLater);

            connect(m_trayIcon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
                    stop();
            });
        }
        m_trayIcon->show();
    }
}

void ScreenRecorder::hideIndicator()
{
    if (m_indicator)
        m_indicator->hide();

    if (m_trayIcon)
        m_trayIcon->hide();
}

void ScreenRecorder::showError(const QString &message)
{
    if (m_failed)
        return;

    m_failed = true;
    hideIndicator();
    qCWarning(lcRecorder).noquote() << message;

    QMessageBox *box = new QMessageBox(QMessageBox::Critical, tr("Screen recording"), message, QMessageBox::Ok);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowFlag(Qt::WindowStaysOnTopHint);
    connect(box, &QMessageBox::finished, this, &ScreenRecorder::finish);
    box->show();
}

void ScreenRecorder::notifySaved()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusInterface iface(NOTIFY_SERVICE, NOTIFY_PATH, NOTIFY_INTERFACE, bus);

    if (!iface.isValid()) {
        finish();
        return;
    }

    QVariantMap hints;
    hints.insert("desktop-entry", "lingmo-screenshot");

    QList<QVariant> args;
    args << "lingmo-screenshot";
    args << ((unsigned int) 0);
    args << "lingmo-screenshot";
    args << tr("Recording saved");
    args << tr("The recording has been saved to %1").arg(m_fileName);
    args << (QStringList() << "default" << tr("Open")
                           << "open-folder" << tr("Open folder"));
    args << hints;
    args << (int) -1;

    QDBusReply<uint> reply = iface.callWithArgumentList(QDBus::Block, "Notify", args);
    if (!reply.isValid()) {
        finish();
        return;
    }

    m_notificationId = reply.value();

    bus.connect(NOTIFY_SERVICE, NOTIFY_PATH, NOTIFY_INTERFACE, "ActionInvoked",
                this, SLOT(onNotificationActionInvoked(uint, QString)));
    bus.connect(NOTIFY_SERVICE, NOTIFY_PATH, NOTIFY_INTERFACE, "NotificationClosed",
                this, SLOT(onNotificationClosed(uint, uint)));

    // Give up the D-Bus names while waiting for the notification actions.
    bus.unregisterObject(RECORDER_PATH);
    bus.unregisterService(RECORDER_SERVICE);

    QTimer::singleShot(NOTIFICATION_TIMEOUT, this, &ScreenRecorder::finish);
}

void ScreenRecorder::onNotificationActionInvoked(uint id, const QString &action)
{
    if (id != m_notificationId)
        return;

    if (action == "default")
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_fileName));
    else if (action == "open-folder")
        openFolder();

    QTimer::singleShot(500, this, &ScreenRecorder::finish);
}

void ScreenRecorder::onNotificationClosed(uint id, uint reason)
{
    Q_UNUSED(reason)

    if (id == m_notificationId)
        QTimer::singleShot(500, this, &ScreenRecorder::finish);
}

void ScreenRecorder::openFolder()
{
    QDBusMessage msg = QDBusMessage::createMethodCall("org.freedesktop.FileManager1",
                                                      "/org/freedesktop/FileManager1",
                                                      "org.freedesktop.FileManager1",
                                                      "ShowItems");
    msg << QStringList(QUrl::fromLocalFile(m_fileName).toString()) << QString();
    QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 3000);

    if (reply.type() != QDBusMessage::ReplyMessage)
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_fileName).absolutePath()));
}

void ScreenRecorder::convertToGif()
{
    const QString mp4 = m_fileName;
    QString gif = mp4;
    gif.replace(QRegularExpression(QStringLiteral("\\.mp4$")), QStringLiteral(".gif"));

    m_converter = new QProcess(this);
    m_converter->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_converter, &QProcess::finished, this, [this, mp4, gif](int exitCode, QProcess::ExitStatus status) {
        const QByteArray output = m_converter->readAll();
        m_converter->deleteLater();
        m_converter = nullptr;

        if (status == QProcess::NormalExit && exitCode == 0 && QFileInfo(gif).size() > 0) {
            QFile::remove(mp4);
            m_fileName = gif;
        } else {
            // Keep the video: nothing is lost.
            qCWarning(lcRecorder).noquote() << "GIF conversion failed:" << output.right(1000);
        }
        notifySaved();
    });

    qCInfo(lcRecorder) << "Converting to GIF:" << gif;
    m_converter->start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")), gifArguments(mp4, gif));
}

void ScreenRecorder::finish()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.unregisterObject(RECORDER_PATH);
    bus.unregisterService(RECORDER_SERVICE);

    if (m_process) {
        m_process->disconnect(this);
        m_process->deleteLater();
        m_process = nullptr;
    }
    m_notificationId = 0;
    m_elapsedTimer.invalidate();
    m_busy = false;

    emit done();
}
