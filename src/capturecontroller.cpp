/*
 * Copyright (C) 2021 LingmoOS Team.
 *
 * Author:     Reion Wong <reionwong@gmail.com>
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

#include "capturecontroller.h"
#include "application.h"
#include "capturesession.h"
#include "config.h"
#include "desktopapps.h"
#include "historymodel.h"
#include "ocr.h"
#include "screenask.h"

#include <QDir>
#include <QGuiApplication>
#include <QJsonArray>
#include "itemwindow.h"

#include <QQuickItem>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <QLoggingCategory>

#include <LayerShellQt/Window>

Q_LOGGING_CATEGORY(lcCapture, "lingmo.screenshot.capture", QtInfoMsg)

namespace {

// Time for the X server to take the overlay off the screen before something
// else grabs it (recording, scrolling capture).
const int HIDE_DELAY = 300;

// The overlay is a layer-shell surface (native Wayland); through XWayland
// it stays a window that bypasses the window manager.
bool waylandOverlay()
{
    return QGuiApplication::platformName() == QLatin1String("wayland");
}

QJsonArray annotationsRelativeTo(const AnnotationModel *model, const QPoint &origin)
{
    QJsonArray array;
    for (Annotation a : model->items()) {
        a.translate(-QPointF(origin));
        array.append(a.toJson());
    }
    return array;
}

}

CaptureController::CaptureController(CaptureSession *session, QObject *parent)
    : QObject(parent)
    , m_session(session)
    , m_overlay(false)
    , m_beautify(false)
    , m_closing(false)
    , m_ask(new ScreenAsk(session, this))
{
    m_session->setParent(this);
    connect(m_ask, &ScreenAsk::closeRequested, this, &CaptureController::close);
    connect(m_ask, &ScreenAsk::message, this, &CaptureController::message);
}

CaptureController::~CaptureController()
{
    delete m_view.data();
    delete m_editorWindow.data();
}

QWindow *CaptureController::window() const
{
    if (m_view)
        return m_view;
    return m_editorWindow;
}

void CaptureController::showOverlay(QScreen *screen)
{
    m_overlay = true;

    m_view = new ItemWindow;
    m_view->setTitle(tr("Screenshot"));
    m_view->setColor(Qt::black);
    m_view->setScreen(screen);
    m_view->setGeometry(screen->geometry());
    if (waylandOverlay()) {
        // Wayland: a layer-shell surface over everything (bar and dock too) on
        // this screen, the whole output, with the keyboard
        m_view->setFlags(Qt::FramelessWindowHint);
        auto *layer = LayerShellQt::Window::get(m_view);
        layer->setScope(QStringLiteral("lingmo-screenshot"));
        layer->setLayer(LayerShellQt::Window::LayerOverlay);
        layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorBottom
                                                        | LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight));
        layer->setExclusiveZone(-1);
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityExclusive);
        layer->setScreen(screen);
    } else {
        m_view->setFlags(Qt::FramelessWindowHint | Qt::X11BypassWindowManagerHint);
    }

    QObject *object = Application::instance()->createQml(
            QStringLiteral("qrc:/qml/Overlay.qml"),
            { { QStringLiteral("view"), QVariant::fromValue<QObject *>(this) },
              { QStringLiteral("capture"), QVariant::fromValue<QObject *>(m_session) },
              { QStringLiteral("screenAsk"), QVariant::fromValue<QObject *>(m_ask) } },
            this);
    auto *item = qobject_cast<QQuickItem *>(object);
    if (!item) {
        delete object;
        qCWarning(lcCapture) << "The capture overlay could not be created";
        QTimer::singleShot(0, this, &CaptureController::close);
        return;
    }
    m_view->setItem(item, false);

    m_view->show();
    m_view->requestActivate();
    if (!waylandOverlay())
        m_view->setKeyboardGrabEnabled(true);
}

void CaptureController::showEditor(const QString &title)
{
    m_overlay = false;

    QObject *object = Application::instance()->createQml(
            QStringLiteral("qrc:/qml/EditorWindow.qml"),
            { { QStringLiteral("view"), QVariant::fromValue<QObject *>(this) },
              { QStringLiteral("capture"), QVariant::fromValue<QObject *>(m_session) },
              { QStringLiteral("screenAsk"), QVariant::fromValue<QObject *>(m_ask) },
              { QStringLiteral("editorTitle"), title } },
            this);
    m_editorWindow = qobject_cast<QWindow *>(object);
    if (!m_editorWindow) {
        delete object;
        qCWarning(lcCapture) << "The editor window could not be created";
        QTimer::singleShot(0, this, &CaptureController::close);
        return;
    }

    connect(m_editorWindow, &QWindow::visibleChanged, this, [this](bool visible) {
        if (!visible && !m_closing)
            close();
    });
}

void CaptureController::setBeautify(bool beautify)
{
    if (m_beautify == beautify)
        return;
    m_beautify = beautify;
    emit beautifyChanged();
}

void CaptureController::setBusy(const QString &text)
{
    if (m_busyText == text)
        return;
    m_busyText = text;
    emit busyChanged();
}

QImage CaptureController::finalImage(const QRect &crop) const
{
    const Beautify::Options options = Application::instance()->config()->beautifyOptions();
    return m_session->render(crop, m_beautify ? &options : nullptr);
}

void CaptureController::remember(const QRect &crop, const QImage &image)
{
    Application *app = Application::instance();
    Config *config = app->config();
    HistoryModel *history = app->history();

    if (!config->historyEnabled())
        return;

    const QRect r = crop.intersected(QRect(QPoint(0, 0), m_session->image().size()));
    if (r.isEmpty())
        return;

    // The preview is without the presentation frame: editing starts again
    // from the plain capture.
    const QImage preview = m_beautify ? m_session->render(r) : image;

    if (!m_session->historyId().isEmpty() && m_session->editor()) {
        history->update(m_session->historyId(),
                        annotationsRelativeTo(m_session->annotations(), m_session->historyOrigin()), preview);
        return;
    }

    const QString id = history->add(m_session->image().copy(r),
                                    annotationsRelativeTo(m_session->annotations(), r.topLeft()), preview,
                                    m_session->windowTitleAt(r.center()), config->historySize());
    m_session->setHistoryId(id);
}

static FileNamePattern::Context contextFor(const CaptureSession *session, const QRect &crop)
{
    FileNamePattern::Context context;
    context.time = QDateTime::currentDateTime();
    context.width = crop.width();
    context.height = crop.height();
    context.windowTitle = session->windowTitleAt(crop.center());
    return context;
}

void CaptureController::accept(const QRect &crop)
{
    if (m_session->acceptPath().isEmpty() && !m_session->acceptPin()) {
        copy(crop);
        return;
    }

    Application *app = Application::instance();
    const QImage image = finalImage(crop);
    if (image.isNull())
        return;

    playSound();

    if (!m_session->acceptPath().isEmpty()) {
        QString error;
        const QString saved = app->saveImage(image, m_session->acceptPath(), contextFor(m_session, crop), &error);
        if (saved.isEmpty()) {
            emit message(error, true);
            return;
        }
        remember(crop, image);
        app->history()->setSavedPath(m_session->historyId(), saved);
        app->notifySaved(saved, m_session->acceptClipboard());
    } else {
        remember(crop, image);
    }

    if (m_session->acceptClipboard())
        app->copyImage(image);
    if (m_session->acceptPin())
        pin(crop);

    close();
}

void CaptureController::copy(const QRect &crop)
{
    const QImage image = finalImage(crop);
    if (image.isNull())
        return;

    playSound();
    Application::instance()->copyImage(image);
    remember(crop, image);
    close();
}

void CaptureController::save(const QRect &crop)
{
    Application *app = Application::instance();
    const QImage image = finalImage(crop);
    if (image.isNull())
        return;

    QString error;
    const QString saved = app->saveImage(image, app->config()->saveFolder() + QLatin1Char('/'), contextFor(m_session, crop), &error);
    if (saved.isEmpty()) {
        emit message(error, true);
        return;
    }

    playSound();
    const bool copied = app->config()->copyAfterSave();
    if (copied)
        app->copyImage(image);

    remember(crop, image);
    app->history()->setSavedPath(m_session->historyId(), saved);
    app->notifySaved(saved, copied);
    close();
}

void CaptureController::saveAs(const QRect &crop)
{
    Application *app = Application::instance();
    const QImage image = finalImage(crop);
    if (image.isNull())
        return;

    // The file dialog is a normal window: it would open under the overlay.
    hideForDialog();
    const QString saved = app->saveImageAs(image, contextFor(m_session, crop));
    if (saved.isEmpty()) {
        showAgain();
        return;
    }

    if (app->config()->copyAfterSave())
        app->copyImage(image);
    remember(crop, image);
    app->history()->setSavedPath(m_session->historyId(), saved);
    app->notifySaved(saved, app->config()->copyAfterSave());
    close();
}

void CaptureController::pin(const QRect &crop)
{
    const QImage image = finalImage(crop);
    if (image.isNull())
        return;

    QScreen *screen = m_session->screen();
    QPoint pos;
    if (screen && m_overlay) {
        // Where the selection was, so the pin covers the same spot.
        pos = screen->geometry().topLeft() + crop.topLeft() / screen->devicePixelRatio();
        if (m_beautify)
            pos -= QPoint(1, 1) * Application::instance()->config()->beautifyPadding() / screen->devicePixelRatio();
    } else {
        QWindow *w = window();
        screen = w ? w->screen() : qGuiApp->primaryScreen();
        const QPoint center = w ? w->geometry().center() : screen->geometry().center();
        pos = center - QPoint(image.width(), image.height()) / screen->devicePixelRatio() / 2;
    }

    Application::instance()->pinImage(image, pos, screen);
    remember(crop, image);
    close();
}

QVariantList CaptureController::openWithApps() const
{
    QVariantList list;
    for (const DesktopApps::App &app : DesktopApps::appsFor(QStringLiteral("image/png"))) {
        QVariantMap map;
        map.insert(QStringLiteral("id"), app.id);
        map.insert(QStringLiteral("name"), app.name);
        map.insert(QStringLiteral("icon"), app.icon);
        list.append(map);
    }
    return list;
}

void CaptureController::openWith(const QRect &crop, const QString &appId)
{
    const QImage image = finalImage(crop);
    if (image.isNull())
        return;

    DesktopApps::App target;
    for (const DesktopApps::App &app : DesktopApps::appsFor(QStringLiteral("image/png"))) {
        if (app.id == appId) {
            target = app;
            break;
        }
    }
    if (target.exec.isEmpty()) {
        emit message(tr("The application was not found."), true);
        return;
    }

    // A private copy the other application can read.
    const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath(QStringLiteral("open"));
    QDir().mkpath(dir);
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    FileNamePattern::Context context = contextFor(m_session, crop);
    const QString file = FileNamePattern::uniquePath(dir, Application::instance()->config()->fileNamePattern(), context,
                                                     QStringLiteral("png"));
    if (!image.save(file, "PNG")) {
        emit message(tr("Unable to write a temporary file."), true);
        return;
    }
    QFile::setPermissions(file, QFile::ReadOwner | QFile::WriteOwner);

    if (!DesktopApps::launch(target, file)) {
        emit message(tr("Unable to start %1.").arg(target.name), true);
        return;
    }

    remember(crop, image);
    close();
}

void CaptureController::upload(const QRect &crop)
{
    Application *app = Application::instance();
    if (!app->config()->uploadEnabled())
        return;

    const QImage image = finalImage(crop);
    if (image.isNull())
        return;

    app->upload(image);
    remember(crop, image);
    close();
}

void CaptureController::sendNearby(const QRect &crop)
{
    const QImage image = finalImage(crop);
    if (image.isNull())
        return;
    Application::instance()->sendNearby(image);
    remember(crop, image);
    close();
}

void CaptureController::recognizeText(const QRect &crop)
{
    OcrEngine *ocr = Application::instance()->ocr();
    const QImage image = m_session->image().copy(crop.intersected(m_session->image().rect()));

    auto *done = new QObject(this);
    connect(ocr, &OcrEngine::textReady, done, [this, done](const QString &text) {
        done->deleteLater();
        setBusy(QString());
        if (text.isEmpty())
            emit message(tr("No text was found in the selection."), false);
        else
            emit textRecognized(text);
    });
    connect(ocr, &OcrEngine::failed, done, [this, done](const QString &error) {
        done->deleteLater();
        setBusy(QString());
        emit message(error, true);
    });

    setBusy(tr("Recognizing text…"));
    ocr->recognizeText(image);
}

void CaptureController::copyText(const QString &text)
{
    Application::instance()->copyText(text);
    close();
}

void CaptureController::redact(const QRect &crop)
{
    OcrEngine *ocr = Application::instance()->ocr();
    const QRect r = crop.intersected(m_session->image().rect());
    const QImage image = m_session->image().copy(r);

    auto *done = new QObject(this);
    connect(ocr, &OcrEngine::boxesReady, done, [this, done, r](const QVector<QRect> &boxes) {
        done->deleteLater();
        setBusy(QString());

        QVariantList rects;
        for (const QRect &box : boxes)
            rects.append(QRectF(box.translated(r.topLeft())));

        const int count = m_session->annotations()->addEffects(QStringLiteral("pixelate"), rects, 6);
        if (count == 0)
            emit message(tr("No text was found in the selection."), false);
        else
            emit message(tr("%n text area(s) hidden. Undo with Ctrl+Z.", "", count), false);
    });
    connect(ocr, &OcrEngine::failed, done, [this, done](const QString &error) {
        done->deleteLater();
        setBusy(QString());
        emit message(error, true);
    });

    setBusy(tr("Looking for text…"));
    ocr->recognizeBoxes(image);
}

void CaptureController::record(const QRect &crop)
{
    QScreen *screen = m_session->screen();
    if (!m_overlay || !screen)
        return;

    const QRect nativeRect = crop.translated(m_session->nativeGeometry().topLeft());
    hideForDialog();
    Application::instance()->startRecording(screen, nativeRect, HIDE_DELAY);
    close();
}

void CaptureController::scrollCapture(const QRect &crop)
{
    QScreen *screen = m_session->screen();
    if (!m_overlay || !screen)
        return;

    const QRect nativeRect = crop.translated(m_session->nativeGeometry().topLeft());
    hideForDialog();
    Application::instance()->startScrollCapture(screen, nativeRect, HIDE_DELAY);
    close();
}

void CaptureController::cancel()
{
    Application::instance()->ocr()->cancel();
    close();
}

void CaptureController::openSettings(const QString &page)
{
    Application::instance()->showLauncher(page.isEmpty() ? QStringLiteral("settings") : page);
    close();
}

void CaptureController::playSound()
{
    Application::instance()->playShutter();
}

void CaptureController::hideForDialog()
{
    if (m_view) {
        if (!waylandOverlay())
            m_view->setKeyboardGrabEnabled(false);
        m_view->hide();
    } else if (m_editorWindow) {
        // The editor stays: dialogs open on top of normal windows.
    }
    QGuiApplication::processEvents();
}

void CaptureController::showAgain()
{
    if (m_view && !m_closing) {
        m_view->show();
        m_view->requestActivate();
        if (!waylandOverlay())
            m_view->setKeyboardGrabEnabled(true);
    }
}

void CaptureController::close()
{
    if (m_closing)
        return;
    m_closing = true;

    if (m_view) {
        if (!waylandOverlay())
            m_view->setKeyboardGrabEnabled(false);
        m_view->hide();
    }
    if (m_editorWindow)
        m_editorWindow->hide();

    emit finished();
    deleteLater();
}
