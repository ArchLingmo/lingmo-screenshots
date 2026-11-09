#include "screenshotintents.h"
#include "application.h"
#include "commandline.h"
#include "config.h"
#include "filenamepattern.h"
#include "ocr.h"
#include "screenrecorder.h"

#include <LingmoIntents/intentservice.h>

#include <QDateTime>
#include <QGuiApplication>
#include <QImage>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <memory>

using LingmoIntents::IntentCall;
using LingmoIntents::IntentResult;

namespace {

QScreen *screenAt(int index)
{
    const QList<QScreen *> screens = QGuiApplication::screens();
    if (index >= 0 && index < screens.size())
        return screens.at(index);
    return QGuiApplication::primaryScreen();
}

QRect regionOf(const QString &text)
{
    if (text.trimmed().isEmpty())
        return QRect();
    return CommandLine::parseRegion(text.trimmed());
}

void capture(Application *app, const IntentCall &call)
{
    const QVariantMap p = call.params();
    const QString mode = p.value(QStringLiteral("mode")).toString();
    const int screenIndex = p.value(QStringLiteral("screen")).toInt();
    if (screenIndex >= QGuiApplication::screens().size())
        return call.finish(IntentResult::failed(QObject::tr("There is no screen %1.").arg(screenIndex)));
    const QRect region = regionOf(p.value(QStringLiteral("region")).toString());
    if (mode == QLatin1String("area") && region.isNull())
        return call.finish(IntentResult::failed(QObject::tr("Invalid region, use WIDTHxHEIGHT+X+Y."),
                                                LingmoIntents::Errors::InvalidParams));
    const int delayMs = int(p.value(QStringLiteral("delay")).toDouble() * 1000);
    QPointer<Application> guard(app);
    QTimer::singleShot(delayMs, app, [guard, call, mode, screenIndex, region, p] {
        if (!guard)
            return;
        FileNamePattern::Context context;
        context.time = QDateTime::currentDateTime();
        QImage image;
        if (mode == QLatin1String("full")) {
            image = Application::grabAllScreens(nullptr);
        } else if (mode == QLatin1String("area")) {
            image = Application::grabArea(region);
        } else if (mode == QLatin1String("window")) {
            image = Application::grabActiveWindow(&context.windowTitle);
            if (image.isNull())
                return call.finish(IntentResult::failed(QObject::tr("No active window was found.")));
        } else {
            image = Application::grabOneScreen(screenAt(screenIndex));
        }
        if (image.isNull())
            return call.finish(IntentResult::failed(QObject::tr("Unable to capture the screen.")));
        context.width = image.width();
        context.height = image.height();
        QString path = p.value(QStringLiteral("path")).toString();
        if (path.isEmpty())
            path = guard->config()->saveFolder() + QLatin1Char('/');
        QString error;
        const QString saved = guard->saveImage(image, path, context, &error);
        if (saved.isEmpty())
            return call.finish(IntentResult::failed(error));
        call.finish(IntentResult::ok({{QStringLiteral("path"), saved},
                                      {QStringLiteral("width"), image.width()},
                                      {QStringLiteral("height"), image.height()}}));
    });
}

void recordStart(Application *app, const IntentCall &call)
{
    if (!Application::recordingSupported())
        return call.finish(IntentResult::failed(Application::notOnWaylandText(QObject::tr("Screen recording"))));
    if (app->recorder()->isRecording())
        return call.finish(IntentResult::failed(QObject::tr("Another screen recording is already in progress.")));
    const QVariantMap p = call.params();
    QScreen *screen = screenAt(p.value(QStringLiteral("screen")).toInt());
    QRect rect = Application::nativeGeometryOf(screen);
    const QRect region = regionOf(p.value(QStringLiteral("region")).toString());
    if (!region.isNull())
        rect = region;
    app->startRecording(screen, rect, 0);
    // ffmpeg starts asynchronously: answer when it records (or gave up)
    auto *timer = new QTimer(app);
    auto *ticks = new int(0);
    QObject::connect(timer, &QTimer::timeout, app, [app, call, timer, ticks] {
        const bool recording = app->recorder()->isRecording();
        if (!recording && ++*ticks < 25)
            return;
        timer->deleteLater();
        delete ticks;
        if (!recording)
            return call.finish(IntentResult::failed(QObject::tr("The screen recording did not start.")));
        call.finish(IntentResult::ok({{QStringLiteral("recording"), true},
                                      {QStringLiteral("folder"), ScreenRecorder::outputDirectory()}}));
    });
    timer->start(200);
}

void recordStop(Application *app, const IntentCall &call)
{
    ScreenRecorder *recorder = app->recorder();
    if (!recorder->isRecording())
        return call.finish(IntentResult::failed(QObject::tr("No screen recording is in progress.")));
    const QString file = recorder->fileName();
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = QObject::connect(recorder, &ScreenRecorder::done, app, [call, file, connection] {
        QObject::disconnect(*connection);
        call.finish(IntentResult::ok({{QStringLiteral("path"), file}}));
    });
    app->stopRecording();
}

void ocr(Application *app, const IntentCall &call)
{
    OcrEngine *engine = app->ocr();
    if (!engine->available())
        return call.finish(IntentResult::failed(QObject::tr("Text recognition needs Tesseract.")));
    if (engine->busy())
        return call.finish(IntentResult::failed(QObject::tr("Text recognition is busy, try again.")));
    const QImage image(call.params().value(QStringLiteral("path")).toString());
    if (image.isNull())
        return call.finish(IntentResult::failed(QObject::tr("This file is not an image.")));
    const QString languages = call.params().value(QStringLiteral("languages")).toString();
    auto connections = std::make_shared<QList<QMetaObject::Connection>>();
    auto finish = [call, connections](const IntentResult &r) {
        for (const auto &c : *connections)
            QObject::disconnect(c);
        call.finish(r);
    };
    connections->append(QObject::connect(engine, &OcrEngine::textReady, app, [finish](const QString &text) {
        finish(IntentResult::ok({{QStringLiteral("text"), text}}));
    }));
    connections->append(QObject::connect(engine, &OcrEngine::failed, app, [finish](const QString &message) {
        finish(IntentResult::failed(message));
    }));
    // No languages given: detected from the script
    engine->recognizeText(image, languages);
}

// "Ask about the screen": the overlay opens and the person selects the area;
// nothing is captured or sent before that. Answered at once (the overlay is
// the result), after a moment for the caller (Spotlight) to leave the screen.
void ask(Application *app, const IntentCall &call)
{
    const QString question = call.params().value(QStringLiteral("question")).toString();
    app->startAsk(question, 450);
    call.finish(IntentResult::ok({{QStringLiteral("opened"), true}}));
}

}

void publishScreenshotIntents(Application *app, bool headless)
{
    auto *intents = new LingmoIntents::IntentService(QStringLiteral("lingmo-screenshot"), app);
    intents->handleAsync(QStringLiteral("screenshot.capture"), [app](const IntentCall &c) { capture(app, c); });
    intents->handleAsync(QStringLiteral("screen.record.start"), [app](const IntentCall &c) { recordStart(app, c); });
    intents->handleAsync(QStringLiteral("screen.record.stop"), [app](const IntentCall &c) { recordStop(app, c); });
    intents->handleAsync(QStringLiteral("screenshot.ocr"), [app](const IntentCall &c) { ocr(app, c); });
    intents->handleAsync(QStringLiteral("screen.ask"), [app](const IntentCall &c) { ask(app, c); });
    intents->publish();     // next to /Screenshot on com.lingmo.Screenshot
    // An instance started only for the actions leaves when idle (and not recording)
    if (headless)
        intents->setIdleQuit(30000, [app] {
            return !app->recorder()->busy() && !app->ocr()->busy() && !app->hasWindows();
        });
}
