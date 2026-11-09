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

#include "kwinscreenshot.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusArgument>
#include <QDBusUnixFileDescriptor>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QScreen>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(lcKWin, "lingmo.screenshot.kwin", QtInfoMsg)

namespace {

const QString s_service = QStringLiteral("org.kde.KWin");
const QString s_path = QStringLiteral("/org/kde/KWin/ScreenShot2");
const QString s_interface = QStringLiteral("org.kde.KWin.ScreenShot2");
// A capture renders a frame first: a few seconds is plenty
const int s_timeout = 10000;

QImage failed(QString *error, const QString &message)
{
    qCWarning(lcKWin).noquote() << message;
    if (error)
        *error = message;
    return QImage();
}

// Until KWin closes its end of the pipe (the whole image is there).
bool readAll(int fd, QByteArray *out, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    char buffer[65536];
    for (;;) {
        const int left = timeoutMs - int(timer.elapsed());
        if (left <= 0)
            return false;
        pollfd p { fd, POLLIN, 0 };
        const int ready = ::poll(&p, 1, left);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            return false;
        const ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (n < 0)
            return false;
        if (n == 0)
            return true;
        out->append(buffer, n);
    }
}

QImage capture(const QString &method, QVariantList args, QVariantMap options, QString *error, bool opaque = true)
{
    options.insert(QStringLiteral("native-resolution"), true);

    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0)
        return failed(error, QStringLiteral("pipe: %1").arg(QString::fromLocal8Bit(strerror(errno))));

    QDBusMessage reply;
    {
        // The request (and any pending call) keeps a copy of the write end: it
        // must be gone before reading, or the pipe never ends. KWin answers
        // first and writes the pixels from a thread.
        QDBusMessage message = QDBusMessage::createMethodCall(s_service, s_path, s_interface, method);
        args << options << QVariant::fromValue(QDBusUnixFileDescriptor(fds[1]));
        ::close(fds[1]);
        message.setArguments(args);
        args.clear();
        reply = QDBusConnection::sessionBus().call(message, QDBus::Block, s_timeout);
    }

    if (reply.type() != QDBusMessage::ReplyMessage) {
        ::close(fds[0]);
        if (reply.errorName().endsWith(QLatin1String("NoAuthorized")))
            return failed(error, QStringLiteral("KWin did not allow the capture (%1). The installed "
                                                "lingmo-screenshot.desktop must name this executable (%2) in Exec.")
                                         .arg(reply.errorMessage(), QCoreApplication::applicationFilePath()));
        return failed(error, QStringLiteral("KWin %1: %2 %3").arg(method, reply.errorName(), reply.errorMessage()));
    }

    QByteArray data;
    const bool complete = readAll(fds[0], &data, s_timeout);
    ::close(fds[0]);
    if (!complete || reply.arguments().isEmpty())
        return failed(error, QStringLiteral("KWin %1: the image did not arrive").arg(method));

    const QVariantMap result = qdbus_cast<QVariantMap>(reply.arguments().constFirst());
    const QString type = result.value(QStringLiteral("type")).toString();
    const int width = int(result.value(QStringLiteral("width")).toUInt());
    const int height = int(result.value(QStringLiteral("height")).toUInt());
    const qsizetype stride = result.value(QStringLiteral("stride")).toUInt();
    const auto format = QImage::Format(result.value(QStringLiteral("format")).toUInt());
    if ((!type.isEmpty() && type != QLatin1String("raw")) || width <= 0 || height <= 0
            || format <= QImage::Format_Invalid || format >= QImage::NImageFormats
            || stride * height > data.size())
        return failed(error, QStringLiteral("KWin %1: unexpected image (%2 %3x%4, format %5, %6 bytes)")
                                     .arg(method, type).arg(width).arg(height).arg(int(format)).arg(data.size()));

    QImage image(reinterpret_cast<const uchar *>(data.constData()), width, height, stride, format);
    // A copy that owns its pixels; screens are opaque like the X11 grabs
    image = image.copy().convertToFormat(opaque ? QImage::Format_RGB32 : QImage::Format_ARGB32);
    image.setDevicePixelRatio(1);
    return image;
}

// Receives the report of the window list script (state()).
class StateReceiver : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.lingmo.Screenshot.KWinState")

public:
    using QObject::QObject;
    QByteArray json;

public Q_SLOTS:
    Q_SCRIPTABLE void Report(const QString &report)
    {
        json = report.toUtf8();
        Q_EMIT reported();
    }

Q_SIGNALS:
    void reported();
};

// Runs inside KWin: callDBus is the only way back. The service, path and
// interface are JSON strings put in by state().
const char s_script[] = R"JS(
(function () {
    var out = { windows: [], active: "", cursor: { x: 0, y: 0 }, screen: "" };
    try {
        var c = workspace.cursorPos;
        out.cursor = { x: Math.round(c.x), y: Math.round(c.y) };
        if (workspace.activeScreen)
            out.screen = workspace.activeScreen.name;
        if (workspace.activeWindow)
            out.active = workspace.activeWindow.caption;
        var current = workspace.currentDesktop;
        var stack = workspace.stackingOrder;
        for (var i = stack.length - 1; i >= 0; --i) {
            var w = stack[i];
            if (!w || w.minimized || w.hidden === true || w.desktopWindow || w.dock
                    || !(w.normalWindow || w.dialog || w.utility))
                continue;
            // Layer-shell surfaces (bar, dock, desktop, overlays) say they are
            // normal windows; only the class name tells them apart
            if (String(w).indexOf("LayerShellV1Window") >= 0)
                continue;
            if (!w.onAllDesktops && current && w.desktops && w.desktops.length > 0
                    && !w.desktops.some(function (d) { return d.id === current.id; }))
                continue;
            var g = w.frameGeometry;
            out.windows.push({ x: Math.round(g.x), y: Math.round(g.y), w: Math.round(g.width),
                               h: Math.round(g.height), title: w.caption });
        }
    } catch (e) {
        out.error = String(e);
    }
    callDBus(%1, %2, %3, "Report", JSON.stringify(out));
})();
)JS";

QDBusMessage scripting(const QString &path, const QString &interface, const QString &method, const QVariantList &args = {})
{
    QDBusMessage message = QDBusMessage::createMethodCall(s_service, path, interface, method);
    message.setArguments(args);
    return message;
}

QString jsString(const QString &s)
{
    return QString::fromUtf8(QJsonDocument(QJsonArray { s }).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
}

}

bool KWinScreenShot::enabled()
{
    static const bool wayland = QGuiApplication::platformName() == QLatin1String("wayland")
            || qEnvironmentVariable("XDG_SESSION_TYPE") == QLatin1String("wayland");
    return wayland;
}

QImage KWinScreenShot::screen(QScreen *screen, QString *error)
{
    if (!screen)
        return failed(error, QStringLiteral("No screen"));
    return capture(QStringLiteral("CaptureScreen"), { screen->name() }, {}, error);
}

QImage KWinScreenShot::workspace(QRect *bounds, QString *error)
{
    if (bounds) {
        QRect all;
        for (QScreen *s : QGuiApplication::screens())
            all |= s->geometry();
        *bounds = all;
    }
    return capture(QStringLiteral("CaptureWorkspace"), {}, {}, error);
}

QImage KWinScreenShot::area(const QRect &rect, QString *error)
{
    if (rect.isEmpty())
        return failed(error, QStringLiteral("Empty area"));
    return capture(QStringLiteral("CaptureArea"),
                   { rect.x(), rect.y(), uint(rect.width()), uint(rect.height()) }, {}, error);
}

QImage KWinScreenShot::activeWindow(QString *error)
{
    return capture(QStringLiteral("CaptureActiveWindow"), {},
                   { { QStringLiteral("include-decoration"), true }, { QStringLiteral("include-shadow"), false } },
                   error, false);
}

KWinScreenShot::State KWinScreenShot::state(int timeoutMs)
{
    State state;
    static int counter = 0;
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return state;

    const QString objectPath = QStringLiteral("/KWinState/%1").arg(++counter);
    StateReceiver receiver;
    if (!bus.registerObject(objectPath, &receiver, QDBusConnection::ExportScriptableSlots)) {
        qCWarning(lcKWin) << "Could not register" << objectPath;
        return state;
    }

    // Private file: KWin (same user) reads it when the script runs
    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty())
        dir = QDir::tempPath();
    QTemporaryFile file(dir + QStringLiteral("/lingmo-screenshot-XXXXXX.js"));
    const QString script = QString::fromLatin1(s_script).arg(jsString(bus.baseService()), jsString(objectPath),
                                                             jsString(QStringLiteral("com.lingmo.Screenshot.KWinState")));
    if (!file.open() || file.write(script.toUtf8()) < 0 || !file.flush()) {
        bus.unregisterObject(objectPath);
        return state;
    }

    const QString plugin = QStringLiteral("lingmo-screenshot-%1-%2").arg(QCoreApplication::applicationPid()).arg(counter);
    const QDBusMessage loaded = bus.call(scripting(QStringLiteral("/Scripting"), QStringLiteral("org.kde.kwin.Scripting"),
                                                   QStringLiteral("loadScript"), { file.fileName(), plugin }),
                                         QDBus::Block, timeoutMs);
    const int id = loaded.type() == QDBusMessage::ReplyMessage && !loaded.arguments().isEmpty()
            ? loaded.arguments().constFirst().toInt() : -1;
    if (id < 0) {
        qCWarning(lcKWin).noquote() << "KWin did not load the window list script:" << loaded.errorMessage();
        bus.unregisterObject(objectPath);
        return state;
    }

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&receiver, &StateReceiver::reported, &loop, &QEventLoop::quit);
    bus.send(scripting(QStringLiteral("/Scripting/Script%1").arg(id), QStringLiteral("org.kde.kwin.Script"),
                            QStringLiteral("run")));
    timeout.start(timeoutMs);
    if (receiver.json.isEmpty())
        loop.exec(QEventLoop::ExcludeUserInputEvents);

    bus.send(scripting(QStringLiteral("/Scripting"), QStringLiteral("org.kde.kwin.Scripting"),
                            QStringLiteral("unloadScript"), { plugin }));
    bus.unregisterObject(objectPath);

    if (receiver.json.isEmpty()) {
        qCWarning(lcKWin) << "The window list script did not answer";
        return state;
    }

    const QJsonObject root = QJsonDocument::fromJson(receiver.json).object();
    if (root.contains(QLatin1String("error")))
        qCWarning(lcKWin).noquote() << "Window list script:" << root.value(QLatin1String("error")).toString();
    for (const QJsonValue &v : root.value(QLatin1String("windows")).toArray()) {
        const QJsonObject o = v.toObject();
        Window w;
        w.rect = QRect(o.value(QLatin1String("x")).toInt(), o.value(QLatin1String("y")).toInt(),
                       o.value(QLatin1String("w")).toInt(), o.value(QLatin1String("h")).toInt());
        w.title = o.value(QLatin1String("title")).toString();
        if (w.rect.width() > 1 && w.rect.height() > 1)
            state.windows.append(w);
    }
    const QJsonObject cursor = root.value(QLatin1String("cursor")).toObject();
    state.cursor = QPoint(cursor.value(QLatin1String("x")).toInt(), cursor.value(QLatin1String("y")).toInt());
    state.activeTitle = root.value(QLatin1String("active")).toString();
    state.activeScreen = root.value(QLatin1String("screen")).toString();
    state.valid = true;
    qCDebug(lcKWin) << "KWin: windows" << state.windows.size() << "cursor" << state.cursor << "screen"
                    << state.activeScreen << "active" << state.activeTitle
                    << "topmost" << (state.windows.isEmpty() ? QRect() : state.windows.constFirst().rect);
    return state;
}

#include "kwinscreenshot.moc"
