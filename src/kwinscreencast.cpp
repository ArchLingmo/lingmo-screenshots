/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

#include "kwinscreencast.h"

#include <QGuiApplication>
#include <QWaylandClientExtensionTemplate>

#include "qwayland-zkde-screencast-unstable-v1.h"

class ScreencastManager : public QWaylandClientExtensionTemplate<ScreencastManager>,
                          public QtWayland::zkde_screencast_unstable_v1
{
public:
    ScreencastManager()
        : QWaylandClientExtensionTemplate<ScreencastManager>(6)
    {
        initialize();
    }

    ~ScreencastManager() override
    {
        if (isActive())
            destroy();
    }

    static ScreencastManager *instance()
    {
        static ScreencastManager *manager = new ScreencastManager;
        return manager;
    }
};

class ScreencastStream : public QObject, public QtWayland::zkde_screencast_stream_unstable_v1
{
    Q_OBJECT

public:
    explicit ScreencastStream(::zkde_screencast_stream_unstable_v1 *stream)
        : QtWayland::zkde_screencast_stream_unstable_v1(stream)
    {
    }

    ~ScreencastStream() override
    {
        if (object())
            close();
    }

signals:
    void ready(const QString &target);
    void failed(const QString &error);
    void closed();

protected:
    // Version 6 sends the serial first, then created
    void zkde_screencast_stream_unstable_v1_serial(uint32_t hi, uint32_t low) override
    {
        m_serial = QString::number((quint64(hi) << 32) | low);
    }
    void zkde_screencast_stream_unstable_v1_created(uint32_t node) override
    {
        Q_EMIT ready(m_serial.isEmpty() ? QString::number(node) : m_serial);
    }
    void zkde_screencast_stream_unstable_v1_failed(const QString &error) override { Q_EMIT failed(error); }
    void zkde_screencast_stream_unstable_v1_closed() override { Q_EMIT closed(); }

private:
    QString m_serial;
};

KWinScreencast::KWinScreencast(QObject *parent)
    : QObject(parent)
{
}

KWinScreencast::~KWinScreencast()
{
    close();
}

bool KWinScreencast::available()
{
    return QGuiApplication::platformName().startsWith(QLatin1String("wayland"))
            && ScreencastManager::instance()->isActive()
            && ScreencastManager::instance()->QtWayland::zkde_screencast_unstable_v1::version() >= 3;   // stream_region
}

void KWinScreencast::streamRegion(const QRect &region, qreal scale)
{
    close();
    if (!available()) {
        Q_EMIT failed(tr("The compositor does not offer screen recording."));
        return;
    }

    // The cursor drawn into the video, as on X11
    constexpr uint embeddedPointer = 2;
    m_stream = new ScreencastStream(ScreencastManager::instance()->stream_region(
            region.x(), region.y(), region.width(), region.height(), wl_fixed_from_double(scale), embeddedPointer));
    connect(m_stream, &ScreencastStream::ready, this, &KWinScreencast::ready);
    connect(m_stream, &ScreencastStream::failed, this, &KWinScreencast::failed);
    connect(m_stream, &ScreencastStream::closed, this, &KWinScreencast::closed);
}

void KWinScreencast::close()
{
    delete m_stream;
    m_stream = nullptr;
}

#include "kwinscreencast.moc"
