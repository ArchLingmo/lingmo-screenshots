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

#include "application.h"
#ifdef HAVE_LINGMO_INTENTS
#include "screenshotintents.h"
#endif
#include "annotationcanvas.h"
#include "capturecontroller.h"
#include "capturesession.h"
#include "config.h"
#include "historymodel.h"
#include "kwinscreenshot.h"
#include "notifier.h"
#include "ocr.h"
#include "pinwindow.h"
#include "screenrecorder.h"
#include "scrollcapture.h"
#include "x11windows.h"

#include <QBuffer>
#include <QClipboard>
#include <QCursor>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageWriter>
#include <QJsonArray>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlContext>
#include <QRegularExpression>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <QWindow>
#include <QLoggingCategory>

#include <cstdio>

Q_LOGGING_CATEGORY(lcApp, "lingmo.screenshot", QtInfoMsg)

namespace {

Application *s_instance = nullptr;

// Native (root window) geometry of a screen. Qt keeps the native origin of
// screens and scales only their size.
QRect nativeGeometry(const QScreen *screen)
{
    return QRect(screen->geometry().topLeft(), screen->geometry().size() * screen->devicePixelRatio());
}

QImage grabScreen(QScreen *screen)
{
    // Wayland: only the compositor sees the other windows
    if (KWinScreenShot::enabled())
        return KWinScreenShot::screen(screen);
    QImage image = screen->grabWindow(0).toImage();
    image.setDevicePixelRatio(1);
    return image;
}

// All screens in one image, in root window coordinates (Wayland: logical
// bounds, native pixels).
QImage grabAll(QRect *bounds)
{
    if (KWinScreenShot::enabled())
        return KWinScreenShot::workspace(bounds);

    QRect all;
    for (QScreen *s : QGuiApplication::screens())
        all |= nativeGeometry(s);

    QImage image(all.size(), QImage::Format_RGB32);
    image.fill(Qt::black);
    QPainter p(&image);
    for (QScreen *s : QGuiApplication::screens())
        p.drawImage(nativeGeometry(s).topLeft() - all.topLeft(), grabScreen(s));
    p.end();

    if (bounds)
        *bounds = all;
    return image;
}

// Wayland: Qt only knows the pointer over its own windows, KWin knows it everywhere.
QScreen *screenOf(const KWinScreenShot::State &state)
{
    if (state.valid) {
        if (QScreen *s = QGuiApplication::screenAt(state.cursor))
            return s;
        for (QScreen *s : QGuiApplication::screens()) {
            if (s->name() == state.activeScreen)
                return s;
        }
    }
    return qGuiApp->primaryScreen();
}

QScreen *screenUnderPointer()
{
    if (KWinScreenShot::enabled())
        return screenOf(KWinScreenShot::state());
    QScreen *s = QGuiApplication::screenAt(QCursor::pos());
    return s ? s : qGuiApp->primaryScreen();
}

// Wayland: KWin's windows (logical) in image pixels of the screen.
QList<CaptureSession::WindowInfo> windowsOn(const KWinScreenShot::State &state, const QScreen *screen, const QRect &native)
{
    QList<CaptureSession::WindowInfo> list;
    const QPoint origin = screen->geometry().topLeft();
    const qreal dpr = screen->devicePixelRatio();
    for (const KWinScreenShot::Window &w : state.windows) {
        const QRect r = w.rect.translated(-origin);
        CaptureSession::WindowInfo info;
        info.rect = QRect((QPointF(r.topLeft()) * dpr).toPoint(), (QSizeF(r.size()) * dpr).toSize())
                            .intersected(QRect(QPoint(0, 0), native.size()));
        info.title = w.title;
        if (!info.rect.isEmpty())
            list.append(info);
    }
    return list;
}

QList<CaptureSession::WindowInfo> windowsOn(const QRect &native)
{
    QList<CaptureSession::WindowInfo> list;
    for (const X11Windows::Window &w : X11Windows::windows()) {
        CaptureSession::WindowInfo info;
        info.rect = w.rect.translated(-native.topLeft()).intersected(QRect(QPoint(0, 0), native.size()));
        info.title = w.title;
        if (!info.rect.isEmpty())
            list.append(info);
    }
    return list;
}

}

Application::Application(QObject *parent)
    : QObject(parent)
    , m_config(new Config(this))
    , m_history(new HistoryModel(this))
    , m_ocr(new OcrEngine(this))
    , m_notifier(new Notifier(this))
    , m_recorder(new ScreenRecorder(this))
    , m_scroll(new ScrollCapture(this))
    , m_engine(nullptr)
    , m_pending(0)
    , m_uploads(0)
    , m_quitScheduled(false)
{
    s_instance = this;

    qmlRegisterType<AnnotationCanvas>("LingmoScreenshot", 1, 0, "AnnotationCanvas");
    qmlRegisterUncreatableType<AnnotationModel>("LingmoScreenshot", 1, 0, "AnnotationModel",
                                                QStringLiteral("Comes from the capture"));
    qmlRegisterUncreatableType<CaptureSession>("LingmoScreenshot", 1, 0, "CaptureSession",
                                               QStringLiteral("Comes from the application"));

    m_ocr->setWantedLanguages(m_config->ocrLanguages());
    connect(m_config, &Config::changed, this, [this] { m_ocr->setWantedLanguages(m_config->ocrLanguages()); });

    connect(m_history, &HistoryModel::editRequested, this, &Application::editHistory);
    connect(m_history, &HistoryModel::pinRequested, this, &Application::pinHistory);
    connect(m_history, &HistoryModel::copied, this, &Application::checkQuit);

    connect(m_recorder, &ScreenRecorder::recordingChanged, this, &Application::recordingChanged);
    connect(m_recorder, &ScreenRecorder::recordingChanged, this, &Application::checkQuit);
    connect(m_recorder, &ScreenRecorder::done, this, &Application::checkQuit);
    connect(m_notifier, &Notifier::pendingChanged, this, &Application::checkQuit);
    connect(m_ocr, &OcrEngine::installingChanged, this, &Application::checkQuit);
    connect(QGuiApplication::clipboard(), &QClipboard::changed, this, &Application::checkQuit);

    connect(m_scroll, &ScrollCapture::runningChanged, this, &Application::checkQuit);
    connect(m_scroll, &ScrollCapture::finished, this, [this](const QImage &image) {
        openEditor(image, QJsonArray(), QString(), tr("Scrolling capture"));
    });
    connect(m_scroll, &ScrollCapture::failed, this, [this](const QString &message) {
        qCWarning(lcApp).noquote() << "Scrolling capture failed:" << message;
        m_notifier->notify(tr("Scrolling capture"), message);
        checkQuit();
    });
}

Application::~Application()
{
    for (const QPointer<PinWindow> &pin : std::as_const(m_pins))
        delete pin.data();
    for (const QPointer<QWindow> &card : std::as_const(m_cards))
        delete card.data();
    for (const QPointer<CaptureController> &c : std::as_const(m_controllers))
        delete c.data();
    delete m_launcher.data();
    delete m_scroll;
    m_scroll = nullptr;
    delete m_recorder;
    m_recorder = nullptr;
    delete m_engine;
    s_instance = nullptr;
}

Application *Application::instance()
{
    return s_instance;
}

bool Application::forward(const QStringList &args)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || !bus.interface()->isServiceRegistered(QStringLiteral(SCREENSHOT_SERVICE)))
        return false;

    QDBusMessage msg = QDBusMessage::createMethodCall(QStringLiteral(SCREENSHOT_SERVICE), QStringLiteral(SCREENSHOT_PATH),
                                                      QStringLiteral(SCREENSHOT_INTERFACE), QStringLiteral("Run"));
    msg << args;
    const QDBusMessage reply = bus.call(msg, QDBus::Block, 5000);
    return reply.type() == QDBusMessage::ReplyMessage;
}

bool Application::registerService()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.registerService(QStringLiteral(SCREENSHOT_SERVICE)))
        return false;
    bus.registerObject(QStringLiteral(SCREENSHOT_PATH), this, QDBusConnection::ExportScriptableSlots);
#ifdef HAVE_LINGMO_INTENTS
    publishScreenshotIntents(this, m_intentsOnly);
#endif
    return true;
}

QImage Application::grabAllScreens(QRect *bounds) { return grabAll(bounds); }
QImage Application::grabOneScreen(QScreen *screen) { return grabScreen(screen); }
QRect Application::nativeGeometryOf(const QScreen *screen) { return nativeGeometry(screen); }

QImage Application::grabArea(const QRect &rect)
{
    if (KWinScreenShot::enabled())
        return KWinScreenShot::area(rect);
    QRect bounds;
    const QImage all = grabAll(&bounds);
    return all.copy(rect.translated(-bounds.topLeft()).intersected(QRect(QPoint(0, 0), bounds.size())));
}

QImage Application::grabActiveWindow(QString *title)
{
    if (KWinScreenShot::enabled()) {
        const QImage image = KWinScreenShot::activeWindow();
        if (!image.isNull() && title)
            *title = KWinScreenShot::state().activeTitle;
        return image;
    }
    X11Windows::Window window;
    if (!X11Windows::activeWindow(&window))
        return QImage();
    if (title)
        *title = window.title;
    return grabArea(window.rect);
}

bool Application::recordingSupported()
{
    // Wayland: KWin's screencast stream, recorded with GStreamer
    return !KWinScreenShot::enabled() || ScreenRecorder::waylandRecordingAvailable();
}

bool Application::scrollCaptureSupported()
{
    // XTest scrolling and x11grab frames: X11 only
    return !KWinScreenShot::enabled();
}

QString Application::notOnWaylandText(const QString &feature)
{
    return tr("%1 is not available in the Wayland session yet.").arg(feature);
}

void Application::Run(const QStringList &args)
{
    CommandLine::Request request;
    QString error;
    if (!CommandLine::parse(args, &request, &error)) {
        qCWarning(lcApp).noquote() << error;
        return;
    }
    // Delay the work: the caller waits for the D-Bus reply.
    QTimer::singleShot(0, this, [this, request] { handle(request); });
}

bool Application::recording() const
{
    return m_recorder->isRecording();
}

QQmlEngine *Application::engine()
{
    if (m_engine)
        return m_engine;

    m_engine = new QQmlEngine;
    m_engine->addImageProvider(QStringLiteral("shots"), new ImageStore);
    QQmlContext *context = m_engine->rootContext();
    context->setContextProperty(QStringLiteral("app"), this);
    context->setContextProperty(QStringLiteral("config"), m_config);
    context->setContextProperty(QStringLiteral("history"), m_history);
    context->setContextProperty(QStringLiteral("ocr"), m_ocr);
    context->setContextProperty(QStringLiteral("recorder"), m_recorder);
    return m_engine;
}

QObject *Application::createQml(const QString &url, const QVariantMap &contextProperties, QObject *owner)
{
    QQmlEngine *e = engine();
    auto *context = new QQmlContext(e->rootContext(), owner);
    for (auto it = contextProperties.cbegin(); it != contextProperties.cend(); ++it)
        context->setContextProperty(it.key(), it.value());

    QQmlComponent component(e, QUrl(url));
    QObject *object = component.create(context);
    if (!object) {
        for (const QQmlError &error : component.errors())
            qCWarning(lcApp).noquote() << error.toString();
        delete context;
        return nullptr;
    }
    return object;
}

bool Application::handle(const CommandLine::Request &request)
{
    switch (request.command) {
    case CommandLine::Gui:
    case CommandLine::Window:
    case CommandLine::Record:
    case CommandLine::Scroll:
    case CommandLine::Ask:
        if (request.command == CommandLine::Window && request.active)
            return captureNow(request);
        startInteractive(request);
        return true;
    case CommandLine::Full:
    case CommandLine::Screen:
        return captureNow(request);
    case CommandLine::Launcher:
        showLauncher();
        return true;
    case CommandLine::History:
        showLauncher(QStringLiteral("history"));
        return true;
    case CommandLine::Settings:
        showLauncher(QStringLiteral("settings"));
        return true;
    default:
        return false;
    }
}

void Application::startInteractive(const CommandLine::Request &request)
{
    // One overlay at a time: pressing the hotkey twice does nothing.
    for (const QPointer<CaptureController> &c : std::as_const(m_controllers)) {
        if (c && c->overlay()) {
            if (QWindow *w = c->window())
                w->requestActivate();
            return;
        }
    }

    // Wayland: scrolling capture needs XTest (not ported yet); recording needs KWin's
    // screencast and GStreamer's pipewiresrc
    if ((request.command == CommandLine::Record && !recordingSupported())
            || (request.command == CommandLine::Scroll && !scrollCaptureSupported())) {
        const QString feature = request.command == CommandLine::Record ? tr("Screen recording") : tr("Scrolling capture");
        fprintf(stderr, "%s\n", qPrintable(notOnWaylandText(feature)));
        m_notifier->notify(feature, notOnWaylandText(feature));
        checkQuit();
        return;
    }

    ++m_pending;
    QTimer::singleShot(qMax(0, request.delayMs), this, [this, request] {
        --m_pending;

        KWinScreenShot::State kwin;
        if (KWinScreenShot::enabled())
            kwin = KWinScreenShot::state();
        QScreen *screen = KWinScreenShot::enabled() ? screenOf(kwin) : screenUnderPointer();
        if (!request.region.isNull()) {
            if (QScreen *s = QGuiApplication::screenAt(request.region.center()))
                screen = s;
        }

        const QRect native = nativeGeometry(screen);
        const QImage image = grabScreen(screen);
        if (image.isNull()) {
            fprintf(stderr, "%s\n", qPrintable(tr("Unable to capture the screen.")));
            m_notifier->notify(tr("Screenshot"), tr("Unable to capture the screen."));
            checkQuit();
            return;
        }
        auto *session = new CaptureSession(image);
        session->setSource(screen, native);
        session->setWindows(KWinScreenShot::enabled() ? windowsOn(kwin, screen, native) : windowsOn(native));
        session->setAccept(request.path, request.clipboard, request.pin);

        if (request.command == CommandLine::Window)
            session->setTarget(QStringLiteral("window"));
        else if (!request.region.isNull() && request.region == screen->geometry())
            session->setTarget(QStringLiteral("screen"));
        session->setVideo(request.command == CommandLine::Record);
        session->setScroll(request.command == CommandLine::Scroll);
        session->setAsk(request.command == CommandLine::Ask);
        session->setAskQuestion(request.question);

        if (!request.region.isNull()) {
            // Logical (relative to the screen) to image pixels.
            const qreal dpr = screen->devicePixelRatio();
            const QRect r = request.region.translated(-screen->geometry().topLeft());
            session->setInitialRegion(QRect((QPointF(r.topLeft()) * dpr).toPoint(), (QSizeF(r.size()) * dpr).toSize())
                                              .intersected(QRect(QPoint(0, 0), native.size())));
        }

        auto *controller = new CaptureController(session, this);
        addController(controller);
        controller->showOverlay(screen);
    });
}

void Application::addController(CaptureController *controller)
{
    m_controllers.append(controller);
    connect(controller, &CaptureController::finished, this, [this] {
        // The controller deletes itself.
        QTimer::singleShot(0, this, &Application::checkQuit);
    });
}

bool Application::captureNow(const CommandLine::Request &request)
{
    ++m_pending;
    QTimer::singleShot(qMax(0, request.delayMs), this, [this, request] {
        --m_pending;

        QImage image;
        FileNamePattern::Context context;
        context.time = QDateTime::currentDateTime();

        if (request.command == CommandLine::Full) {
            image = grabAll(nullptr);
        } else if (request.command == CommandLine::Screen) {
            const QList<QScreen *> screens = QGuiApplication::screens();
            QScreen *screen = screenUnderPointer();
            if (request.screen >= 0) {
                if (request.screen >= screens.size()) {
                    fprintf(stderr, "%s\n", qPrintable(tr("There is no screen %1.").arg(request.screen)));
                    QCoreApplication::exit(1);
                    return;
                }
                screen = screens.at(request.screen);
            }
            image = grabScreen(screen);
        } else {
            if (!KWinScreenShot::enabled() && !X11Windows::activeWindow(nullptr)) {
                fprintf(stderr, "%s\n", qPrintable(tr("No active window was found.")));
                QCoreApplication::exit(1);
                return;
            }
            image = grabActiveWindow(&context.windowTitle);
        }

        if (image.isNull()) {
            fprintf(stderr, "%s\n", qPrintable(tr("Unable to capture the screen.")));
            QCoreApplication::exit(1);
            return;
        }

        context.width = image.width();
        context.height = image.height();
        deliver(image, request, context);
    });
    return true;
}

void Application::deliver(const QImage &image, const CommandLine::Request &request,
                          const FileNamePattern::Context &context)
{
    if (request.raw) {
        QByteArray data;
        QBuffer buffer(&data);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        fwrite(data.constData(), 1, size_t(data.size()), stdout);
        fflush(stdout);
        if (!request.clipboard && !request.pin && request.path.isEmpty()) {
            QCoreApplication::exit(0);
            return;
        }
    }

    playShutter();

    const bool saveIt = !request.path.isEmpty() || (!request.clipboard && !request.pin && !request.raw);
    QString saved;
    if (saveIt) {
        QString error;
        saved = saveImage(image, request.path.isEmpty() ? m_config->saveFolder() + QLatin1Char('/') : request.path, context, &error);
        if (saved.isEmpty()) {
            fprintf(stderr, "%s\n", qPrintable(error));
            QCoreApplication::exit(1);
            return;
        }
        printf("%s\n", qPrintable(saved));
        fflush(stdout);
    }

    if (request.clipboard)
        copyImage(image);
    if (request.pin)
        pinImage(image, QGuiApplication::primaryScreen()->geometry().center()
                                - QPoint(image.width(), image.height()) / 4, QGuiApplication::primaryScreen());

    if (m_config->historyEnabled()) {
        const QString id = m_history->add(image, QJsonArray(), image, context.windowTitle, m_config->historySize());
        if (!saved.isEmpty())
            m_history->setSavedPath(id, saved);
    }

    if (!saved.isEmpty())
        notifySaved(saved, request.clipboard);

    checkQuit();
}

void Application::openEditor(const QImage &image, const QJsonArray &annotations, const QString &historyId,
                             const QString &title)
{
    if (image.isNull())
        return;

    auto *session = new CaptureSession(image);
    session->setEditor(true);
    session->setHistoryId(historyId);
    session->annotations()->fromJson(annotations);

    auto *controller = new CaptureController(session, this);
    addController(controller);
    controller->showEditor(title);
}

void Application::editHistory(const QString &id)
{
    QImage image;
    QJsonArray annotations;
    if (!m_history->load(id, &image, &annotations))
        return;

    openEditor(image, annotations, id, tr("Edit screenshot"));
}

void Application::pinHistory(const QString &id)
{
    const QImage image = m_history->preview(id);
    if (image.isNull())
        return;
    QScreen *screen = screenUnderPointer();
    pinImage(image, screen->geometry().center() - QPoint(image.width(), image.height()) / screen->devicePixelRatio() / 2,
             screen);
}

void Application::startScrollCapture(QScreen *screen, const QRect &nativeRect, int delayMs)
{
    if (!screen)
        return;
    if (!scrollCaptureSupported()) {
        m_notifier->notify(tr("Scrolling capture"), notOnWaylandText(tr("Scrolling capture")));
        checkQuit();
        return;
    }

    if (delayMs > 0) {
        ++m_pending;
        QTimer::singleShot(delayMs, this, [this, screen = QPointer<QScreen>(screen), nativeRect] {
            --m_pending;
            startScrollCapture(screen, nativeRect);
        });
        return;
    }

    m_scroll->start(screen, nativeRect);
    checkQuit();
}

void Application::startRecording(QScreen *screen, const QRect &nativeRect, int delayMs)
{
    if (!screen)
        return;
    if (!recordingSupported()) {
        m_notifier->notify(tr("Screen recording"), notOnWaylandText(tr("Screen recording")));
        checkQuit();
        return;
    }

    if (delayMs > 0) {
        ++m_pending;
        QTimer::singleShot(delayMs, this, [this, screen = QPointer<QScreen>(screen), nativeRect] {
            --m_pending;
            startRecording(screen, nativeRect);
        });
        return;
    }

    ScreenRecorder::Options options;
    options.gif = m_config->recordFormat() == QLatin1String("gif");
    options.microphone = m_config->recordMicrophone() && !options.gif;
    options.systemAudio = m_config->recordSystemAudio() && !options.gif;
    options.showClicks = m_config->recordShowClicks();
    options.frameRate = m_config->recordFrameRate();
    m_recorder->start(screen, nativeRect, options);
    checkQuit();
}

void Application::startAsk(const QString &question, int delayMs)
{
    CommandLine::Request request;
    request.command = CommandLine::Ask;
    request.question = question.trimmed().left(2000);
    request.delayMs = delayMs;
    startInteractive(request);
}

void Application::pinAnswer(const QString &question, const QString &answer, const QString &model)
{
    QScreen *screen = screenUnderPointer();
    QObject *object = createQml(QStringLiteral("qrc:/qml/AnswerCard.qml"),
                                { { QStringLiteral("cardQuestion"), question },
                                  { QStringLiteral("cardAnswer"), answer },
                                  { QStringLiteral("cardModel"), model } },
                                this);
    auto *window = qobject_cast<QWindow *>(object);
    if (!window) {
        delete object;
        checkQuit();
        return;
    }
    window->setParent(nullptr);
    window->setScreen(screen);
    const QRect area = screen->availableGeometry();
    window->setPosition(area.right() - window->width() - 24, area.top() + 24);
    m_cards.append(window);
    connect(window, &QWindow::visibleChanged, this, [this, window](bool visible) {
        if (!visible) {
            window->deleteLater();
            QTimer::singleShot(0, this, &Application::checkQuit);
        }
    });
    connect(window, &QObject::destroyed, this, &Application::checkQuit);
    window->show();
}

bool Application::hasWindows() const
{
    auto alive = [](const auto &list) {
        for (const auto &p : list) {
            if (p)
                return true;
        }
        return false;
    };
    return alive(m_controllers) || alive(m_pins) || alive(m_cards) || m_pending > 0;
}

void Application::stopRecording()
{
    m_recorder->stop();
}

QString Application::recordingsFolder() const
{
    return ScreenRecorder::outputDirectory();
}

void Application::openRecordingsFolder()
{
    QDir().mkpath(ScreenRecorder::outputDirectory());
    QDesktopServices::openUrl(QUrl::fromLocalFile(ScreenRecorder::outputDirectory()));
}

QString Application::saveImage(const QImage &image, const QString &path, const FileNamePattern::Context &context,
                               QString *error)
{
    QString target = path;
    if (target.startsWith(QLatin1String("~/")))
        target = QDir::homePath() + target.mid(1);
    target = QDir::cleanPath(QDir().absoluteFilePath(target));

    const QFileInfo info(target);
    if (path.endsWith(QLatin1Char('/')) || info.isDir()) {
        if (!QDir().mkpath(target)) {
            if (error)
                *error = tr("Unable to create the folder %1.").arg(target);
            return QString();
        }
        target = FileNamePattern::uniquePath(target, m_config->fileNamePattern(), context, m_config->imageFormat());
    } else {
        if (info.suffix().isEmpty())
            target += QLatin1Char('.') + m_config->imageFormat();
        QDir().mkpath(QFileInfo(target).absolutePath());
    }

    const QString format = FileNamePattern::formatForPath(target);
    QImage out = image;
    if (format == QLatin1String("jpg") || format == QLatin1String("bmp")) {
        // No transparency: rounded corners go on white.
        QImage flat(out.size(), QImage::Format_RGB32);
        flat.fill(Qt::white);
        QPainter p(&flat);
        p.drawImage(0, 0, out);
        p.end();
        out = flat;
    }

    QImageWriter writer(target, format.toLatin1());
    if (format == QLatin1String("jpg") || format == QLatin1String("webp"))
        writer.setQuality(92);
    if (!writer.write(out)) {
        if (error)
            *error = tr("Unable to save the screenshot to %1: %2").arg(target, writer.errorString());
        return QString();
    }

    qCInfo(lcApp).noquote() << "Saved" << target;
    return target;
}

QString Application::saveImageAs(const QImage &image, const FileNamePattern::Context &context)
{
    QDir().mkpath(m_config->saveFolder());
    const QString suggested = FileNamePattern::uniquePath(m_config->saveFolder(), m_config->fileNamePattern(), context,
                                                          m_config->imageFormat());

    const QString file = QFileDialog::getSaveFileName(nullptr, tr("Save screenshot"), suggested,
                                                      tr("PNG image (*.png);;JPEG image (*.jpg *.jpeg);;WebP image (*.webp)"));
    if (file.isEmpty())
        return QString();

    QString error;
    const QString saved = saveImage(image, file, context, &error);
    if (saved.isEmpty())
        m_notifier->notify(tr("Screenshot not saved"), error);
    return saved;
}

void Application::copyImage(const QImage &image)
{
    QGuiApplication::clipboard()->setImage(image, QClipboard::Clipboard);
    checkQuit();
}

void Application::copyText(const QString &text)
{
    QGuiApplication::clipboard()->setText(text, QClipboard::Clipboard);
    checkQuit();
}

void Application::pinImage(const QImage &image, const QPoint &pos, QScreen *screen)
{
    auto *pin = new PinWindow(image, pos, screen);
    m_pins.append(pin);
    connect(pin, &QObject::destroyed, this, &Application::checkQuit);
    pin->show();
    pin->requestActivate();
}

void Application::upload(const QImage &image)
{
    const QString command = m_config->uploadCommand();
    QStringList parts = QProcess::splitCommand(command);
    if (parts.isEmpty() || !command.contains(QLatin1String("%f")))
        return;

    const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath(QStringLiteral("upload"));
    QDir().mkpath(dir);
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    FileNamePattern::Context context;
    context.time = QDateTime::currentDateTime();
    context.width = image.width();
    context.height = image.height();
    const QString file = FileNamePattern::uniquePath(dir, m_config->fileNamePattern(), context, QStringLiteral("png"));
    if (!image.save(file, "PNG"))
        return;

    // No shell: the file name can't inject anything.
    for (QString &part : parts)
        part.replace(QLatin1String("%f"), file);

    const QString name = m_config->uploadName().isEmpty() ? parts.first() : m_config->uploadName();
    auto *process = new QProcess(this);
    ++m_uploads;

    connect(process, &QProcess::finished, this, [this, process, file, name](int exitCode, QProcess::ExitStatus status) {
        const QString out = QString::fromUtf8(process->readAllStandardOutput());
        const QString err = QString::fromUtf8(process->readAllStandardError()).trimmed();
        process->deleteLater();
        QFile::remove(file);
        --m_uploads;

        static const QRegularExpression urlRe(QStringLiteral("https?://[^\\s\"'<>]+"));
        const QRegularExpressionMatch match = urlRe.match(out);

        if (status == QProcess::NormalExit && exitCode == 0 && match.hasMatch()) {
            const QString url = match.captured(0);
            copyText(url);
            m_notifier->notify(tr("Sent to %1").arg(name), tr("The link was copied: %1").arg(url),
                               { QStringLiteral("default"), tr("Open link") },
                               [url](const QString &action) {
                                   if (action == QLatin1String("default"))
                                       QDesktopServices::openUrl(QUrl(url));
                               });
        } else {
            m_notifier->notify(tr("Could not send to %1").arg(name),
                               err.isEmpty() ? tr("The command did not return a link.") : err.right(300));
        }
        checkQuit();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, file, name](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        process->deleteLater();
        QFile::remove(file);
        --m_uploads;
        m_notifier->notify(tr("Could not send to %1").arg(name), tr("The command could not be started."));
        checkQuit();
    });

    process->start(parts.first(), parts.mid(1));
}

void Application::sendNearby(const QImage &image)
{
    // Kept until lingmo-share has sent it; older ones are cleared here
    const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath(QStringLiteral("share"));
    QDir().mkpath(dir);
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const QDateTime old = QDateTime::currentDateTime().addDays(-1);
    for (const QFileInfo &f : QDir(dir).entryInfoList(QDir::Files)) {
        if (f.lastModified() < old)
            QFile::remove(f.absoluteFilePath());
    }
    FileNamePattern::Context context;
    context.time = QDateTime::currentDateTime();
    context.width = image.width();
    context.height = image.height();
    const QString file = FileNamePattern::uniquePath(dir, m_config->fileNamePattern(), context, QStringLiteral("png"));
    if (!image.save(file, "PNG"))
        return;
    QProcess::startDetached(QStringLiteral("lingmo-share"), {QStringLiteral("send"), QStringLiteral("--pick"), file});
}

void Application::playShutter()
{
    if (!m_config->playSound())
        return;

    if (!QStandardPaths::findExecutable(QStringLiteral("canberra-gtk-play")).isEmpty()) {
        QProcess::startDetached(QStringLiteral("canberra-gtk-play"),
                                { QStringLiteral("-i"), QStringLiteral("camera-shutter"),
                                  QStringLiteral("-d"), QStringLiteral("lingmo-screenshot") });
        return;
    }

    const QString sound = QStringLiteral("/usr/share/sounds/freedesktop/stereo/camera-shutter.oga");
    if (!QFile::exists(sound))
        return;
    for (const QString &player : { QStringLiteral("pw-play"), QStringLiteral("paplay") }) {
        if (!QStandardPaths::findExecutable(player).isEmpty()) {
            QProcess::startDetached(player, { sound });
            return;
        }
    }
}

void Application::notifySaved(const QString &path, bool copied)
{
    const QString body = copied ? tr("Saved to %1 and copied to the clipboard.").arg(path)
                                : tr("Saved to %1").arg(path);
    m_notifier->notify(tr("Screenshot saved"), body,
                       { QStringLiteral("default"), tr("Open"), QStringLiteral("folder"), tr("Show in folder") },
                       [path](const QString &action) {
                           if (action == QLatin1String("default")) {
                               QDesktopServices::openUrl(QUrl::fromLocalFile(path));
                               return;
                           }
                           QDBusMessage msg = QDBusMessage::createMethodCall(
                                   QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("/org/freedesktop/FileManager1"),
                                   QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("ShowItems"));
                           msg << QStringList(QUrl::fromLocalFile(path).toString()) << QString();
                           if (QDBusConnection::sessionBus().call(msg, QDBus::Block, 3000).type() != QDBusMessage::ReplyMessage)
                               QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
                       },
                       path);
}

void Application::showLauncher(const QString &page)
{
    if (!m_launcher) {
        QObject *object = createQml(QStringLiteral("qrc:/qml/Launcher.qml"),
                                    { { QStringLiteral("initialPage"), page } }, this);
        m_launcher = qobject_cast<QWindow *>(object);
        if (!m_launcher) {
            qCWarning(lcApp) << "The launcher window could not be created";
            delete object;
            checkQuit();
            return;
        }
        m_launcher->setParent(nullptr);
        connect(m_launcher, &QWindow::visibleChanged, this, [this](bool visible) {
            if (!visible)
                QTimer::singleShot(0, this, &Application::checkQuit);
        });
        return;
    }

    m_launcher->show();
    m_launcher->raise();
    m_launcher->requestActivate();
    emit launcherPageRequested(page);
}

void Application::captureFromLauncher(const QString &mode, int delaySeconds)
{
    if (m_launcher)
        m_launcher->hide();

    // Give the window manager time to take the launcher off the screen.
    const int delay = qMax(450, delaySeconds * 1000);

    CommandLine::Request request;
    request.delayMs = delay;

    if (mode == QLatin1String("full")) {
        ++m_pending;
        QTimer::singleShot(delay, this, [this] {
            --m_pending;
            const QImage image = grabAll(nullptr);
            playShutter();
            openEditor(image, QJsonArray(), QString(), tr("All screens"));
        });
        return;
    }

    if (mode == QLatin1String("window"))
        request.command = CommandLine::Window;
    else if (mode == QLatin1String("record"))
        request.command = CommandLine::Record;
    else if (mode == QLatin1String("scroll"))
        request.command = CommandLine::Scroll;
    else if (mode == QLatin1String("screen"))
        request.region = screenUnderPointer()->geometry();

    startInteractive(request);
}

void Application::checkQuit()
{
    if (m_quitScheduled)
        return;

    m_quitScheduled = true;
    QTimer::singleShot(0, this, [this] {
        m_quitScheduled = false;

        m_controllers.removeAll(nullptr);
        m_pins.removeAll(nullptr);
        m_cards.removeAll(nullptr);

        const bool launcherVisible = m_launcher && m_launcher->isVisible();

        const bool busy = !m_controllers.isEmpty() || !m_pins.isEmpty() || !m_cards.isEmpty() || launcherVisible
                || m_pending > 0 || m_uploads > 0 || m_recorder->busy() || m_notifier->pending()
                || m_scroll->running() || m_ocr->installing() || m_ocr->busy()
                || QGuiApplication::clipboard()->ownsClipboard();

        if (busy)
            return;

        qCInfo(lcApp) << "Nothing left to do, quitting";
        QDBusConnection::sessionBus().unregisterService(QStringLiteral(SCREENSHOT_SERVICE));
        QCoreApplication::quit();
    });
}
