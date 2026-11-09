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

#ifndef KWINSCREENSHOT_H
#define KWINSCREENSHOT_H

#include <QImage>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QString>

class QScreen;

// The Wayland session: captures through KWin (org.kde.KWin.ScreenShot2, the
// pixels come through a pipe) and the window list through a short KWin script.
// KWin only answers executables named by a .desktop file with
// X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2 (Exec must be the
// absolute path: /usr/bin/lingmo-screenshot).
namespace KWinScreenShot
{
    // A Wayland session (or a Wayland Qt platform): use KWin, not X11.
    bool enabled();

    // Images are in native pixels, with a device pixel ratio of 1. On error
    // they are null and error says why (also logged).
    QImage screen(QScreen *screen, QString *error = nullptr);
    // All screens; bounds = the logical area it shows.
    QImage workspace(QRect *bounds, QString *error = nullptr);
    // A logical (global) rectangle.
    QImage area(const QRect &rect, QString *error = nullptr);
    // The focused window with its decorations.
    QImage activeWindow(QString *error = nullptr);

    struct Window {
        QRect rect;      // logical, global, with decorations
        QString title;
    };

    struct State {
        bool valid = false;
        QList<Window> windows; // normal windows of the current desktop, topmost first
        QString activeTitle;
        QPoint cursor;         // logical, global
        QString activeScreen;  // output name
    };

    // Asks KWin (a script that calls back over D-Bus). Waits up to timeoutMs.
    State state(int timeoutMs = 1500);
}

#endif // KWINSCREENSHOT_H
