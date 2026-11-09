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

#ifndef APPLICATION_H
#define APPLICATION_H

#include <QImage>
#include <QJsonArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QVariantMap>

#include "commandline.h"
#include "filenamepattern.h"

class CaptureController;
class Config;
class HistoryModel;
class Notifier;
class OcrEngine;
class PinWindow;
class QQmlApplicationEngine;
class QWindow;
class QQmlEngine;
class QScreen;
class ScreenRecorder;
class ScrollCapture;

#define SCREENSHOT_SERVICE "com.lingmo.Screenshot"
#define SCREENSHOT_PATH "/Screenshot"
#define SCREENSHOT_INTERFACE "com.lingmo.Screenshot"

// The running instance. Interactive requests (the hotkey, the launcher...)
// started while it runs are handed to it over D-Bus, so pins, the
// clipboard and recordings live in one process. It quits by itself when
// nothing is left: no window, pin, recording, notification or clipboard
// content it owns.
class Application : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", SCREENSHOT_INTERFACE)
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)

public:
    explicit Application(QObject *parent = nullptr);
    ~Application() override;

    static Application *instance();

    // Hands args to an instance that is already running. False when there
    // is none.
    static bool forward(const QStringList &args);
    bool registerService();
    // `lingmo-screenshot --intents`: started by D-Bus activation only to serve
    // the Lingmo Intents; leaves when idle
    void setIntentsOnly(bool on) { m_intentsOnly = on; }
    // Screen grabs, shared with the Lingmo Intents (screenshotintents.cpp)
    static QImage grabAllScreens(QRect *bounds);
    static QImage grabOneScreen(QScreen *screen);
    static QRect nativeGeometryOf(const QScreen *screen);
    // A global (root window; Wayland: logical) rectangle of the screens
    static QImage grabArea(const QRect &rect);
    // The focused window with its decorations; null when there is none
    static QImage grabActiveWindow(QString *title);
    // Recording and the scrolling capture (ffmpeg x11grab, XTest) work on X11
    // only for now; on Wayland they say so (notOnWaylandText)
    static bool recordingSupported();
    static bool scrollCaptureSupported();
    static QString notOnWaylandText(const QString &feature);

    // Runs a request. Returns false when the process should exit at once
    // with an error (already reported on stderr).
    bool handle(const CommandLine::Request &request);

    Config *config() const { return m_config; }
    HistoryModel *history() const { return m_history; }
    OcrEngine *ocr() const { return m_ocr; }
    Notifier *notifier() const { return m_notifier; }
    ScreenRecorder *recorder() const { return m_recorder; }
    bool recording() const;

    // The one QML engine of the process: app, config, history, ocr and
    // recorder in its root context, captures and pins in image://shots.
    QQmlEngine *engine();
    // Creates url in a child context of the engine with these properties.
    // The context belongs to owner. Errors are logged; null on failure.
    QObject *createQml(const QString &url, const QVariantMap &contextProperties, QObject *owner);

    // Captures
    void openEditor(const QImage &image, const QJsonArray &annotations, const QString &historyId,
                    const QString &title);
    // delayMs: time for the overlay to leave the screen first.
    void startScrollCapture(QScreen *screen, const QRect &nativeRect, int delayMs = 0);
    void startRecording(QScreen *screen, const QRect &nativeRect, int delayMs = 0);
    // "Ask about the screen": the overlay for a question to the AI
    void startAsk(const QString &question = QString(), int delayMs = 0);
    // A floating card with an answer of the AI
    void pinAnswer(const QString &question, const QString &answer, const QString &model);
    // Captures or answer cards on screen (the intents-only process stays for them)
    bool hasWindows() const;

    // Outputs. saveImage: path may be a file or a folder (then the pattern
    // names the file). Returns the saved file, empty on error.
    QString saveImage(const QImage &image, const QString &path, const FileNamePattern::Context &context,
                      QString *error);
    QString saveImageAs(const QImage &image, const FileNamePattern::Context &context);
    void copyImage(const QImage &image);
    void copyText(const QString &text);
    void pinImage(const QImage &image, const QPoint &pos, QScreen *screen);
    void upload(const QImage &image);
    // Hands the capture to Lingmo Share, which asks where to send it
    void sendNearby(const QImage &image);
    void playShutter();
    void notifySaved(const QString &path, bool copied);

    Q_INVOKABLE void showLauncher(const QString &page = QString());
    // From the launcher: capture after hiding it.
    Q_INVOKABLE void captureFromLauncher(const QString &mode, int delaySeconds);
    Q_INVOKABLE void stopRecording();
    // Recording works in this session (Wayland: KWin's screencast and GStreamer)
    Q_INVOKABLE bool canRecord() const { return recordingSupported(); }
    Q_INVOKABLE QString recordingsFolder() const;
    Q_INVOKABLE void openRecordingsFolder();

    // Something changed that may let the process exit.
    void checkQuit();

public slots:
    Q_SCRIPTABLE void Run(const QStringList &args);

signals:
    void recordingChanged();
    void launcherPageRequested(const QString &page);

private:
    void startInteractive(const CommandLine::Request &request);
    bool captureNow(const CommandLine::Request &request);
    void deliver(const QImage &image, const CommandLine::Request &request, const FileNamePattern::Context &context);
    void addController(CaptureController *controller);
    void editHistory(const QString &id);
    void pinHistory(const QString &id);

    Config *m_config;
    HistoryModel *m_history;
    OcrEngine *m_ocr;
    Notifier *m_notifier;
    ScreenRecorder *m_recorder;
    ScrollCapture *m_scroll;

    QList<QPointer<CaptureController>> m_controllers;
    QList<QPointer<PinWindow>> m_pins;
    QList<QPointer<QWindow>> m_cards;
    QQmlEngine *m_engine;
    QPointer<QWindow> m_launcher;
    int m_pending;
    int m_uploads;
    bool m_quitScheduled;
    bool m_intentsOnly = false;
};

#endif // APPLICATION_H
