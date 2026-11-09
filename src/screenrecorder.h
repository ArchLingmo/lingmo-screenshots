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

#ifndef SCREENRECORDER_H
#define SCREENRECORDER_H

#include <QObject>
#include <QRect>
#include <QProcess>
#include <QElapsedTimer>
#include <QPointer>
#include <QAbstractNativeEventFilter>

class QScreen;
class QTimer;
class ItemWindow;
class QSystemTrayIcon;
class KWinScreencast;

#define RECORDER_SERVICE "com.lingmo.ScreenRecorder"
#define RECORDER_PATH "/Recorder"
#define RECORDER_INTERFACE "com.lingmo.ScreenRecorder"

class ScreenRecorder : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", RECORDER_INTERFACE)
    Q_PROPERTY(bool recording READ isRecording NOTIFY recordingChanged)
    Q_PROPERTY(int elapsed READ elapsed NOTIFY elapsedChanged)

public:
    struct Options {
        bool microphone = false;
        bool systemAudio = false;
        bool showClicks = false;
        int frameRate = 30;
        // Convert to an animated GIF when the recording ends (no audio).
        bool gif = false;
    };

    explicit ScreenRecorder(QObject *parent = nullptr);
    ~ScreenRecorder();

    // rect: area to record in root window coordinates (native pixels).
    void start(QScreen *screen, const QRect &rect, const Options &options);

    int elapsed() const;

    static QString outputDirectory();
    static QStringList ffmpegArguments(const QString &display, const QRect &rect,
                                       const Options &options, const QString &monitorSource,
                                       const QString &fileName);
    static QString defaultMonitorSource();
    static QStringList gifArguments(const QString &input, const QString &output);
    // Wayland: gst-launch-1.0 reading KWin's PipeWire stream (target: its serial or node)
    static QStringList gstArguments(const QString &target, const QSize &size, const Options &options,
                                    const QString &monitorSource, const QString &fileName);
    // Wayland recording works: KWin's screencast and GStreamer's pipewiresrc are there
    static bool waylandRecordingAvailable();

    // Asks a recorder running in another process to stop.
    static bool stopRunningRecording();

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

public slots:
    Q_SCRIPTABLE void StopRecording();
    Q_SCRIPTABLE bool IsRecording() const;

    void stop();
    bool isRecording() const;
    // The file being recorded (the last one after stop())
    QString fileName() const { return m_fileName; }
    // Recording, converting, or waiting for the notification actions.
    bool busy() const;

signals:
    void recordingChanged();
    void elapsedChanged();
    // Emitted when the recorder has nothing left to do.
    void done();

private slots:
    void onStarted();
    void onFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onErrorOccurred(QProcess::ProcessError error);
    void onNotificationActionInvoked(uint id, const QString &action);
    void onNotificationClosed(uint id, uint reason);

private:
    void showIndicator();
    void showTrayIcon();
    void hideIndicator();
    void showError(const QString &message);
    void notifySaved();
    void openFolder();
    void finish();
    void showClicks(bool enable);
    void convertToGif();
    void startRecorderProcess(const QString &program, const QStringList &args);

private:
    QProcess *m_process;
    QProcess *m_converter;
    bool m_busy;
    QPointer<QScreen> m_screen;
    QRect m_rect;
    Options m_options;
    QString m_fileName;
    QByteArray m_errorOutput;
    bool m_stopRequested;
    bool m_failed;

    QElapsedTimer m_elapsedTimer;
    QTimer *m_tickTimer;
    QTimer *m_killTimer;
    bool m_showingClicks;

    ItemWindow *m_indicator;
    QSystemTrayIcon *m_trayIcon;

    uint m_notificationId;

    // Wayland: the stream KWin gives (GStreamer records it); X11: ffmpeg x11grab
    bool m_wayland = false;
    KWinScreencast *m_screencast = nullptr;
};

#endif // SCREENRECORDER_H
