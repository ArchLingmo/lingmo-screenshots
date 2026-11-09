/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

#ifndef KWINSCREENCAST_H
#define KWINSCREENCAST_H

#include <QObject>
#include <QRect>

class ScreencastManager;
class ScreencastStream;

// Wayland: a PipeWire stream of a part of the workspace, from KWin
// (zkde_screencast_unstable_v1, the protocol Spectacle records with). KWin only
// hands it to clients whose .desktop file lists it in X-KDE-Wayland-Interfaces.
class KWinScreencast : public QObject
{
    Q_OBJECT

public:
    explicit KWinScreencast(QObject *parent = nullptr);
    ~KWinScreencast() override;

    // The compositor offers the protocol to this process
    static bool available();

    // region: logical coordinates; scale: device pixels per logical one
    void streamRegion(const QRect &region, qreal scale);
    void close();

signals:
    // The PipeWire object to read: its serial when KWin sends one, else its node id
    void ready(const QString &target);
    void failed(const QString &error);
    // KWin ended the stream (an output went away, the session locked...)
    void closed();

private:
    ScreencastStream *m_stream = nullptr;
};

#endif // KWINSCREENCAST_H
