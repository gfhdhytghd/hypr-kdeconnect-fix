#include "portal_backend.hpp"
#include "wayland_input.hpp"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDebug>
#include <QThread>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("hypr-kdeconnect-portal"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    const QStringList args = QCoreApplication::arguments();
    if (args.size() == 4 && args.at(1) == QStringLiteral("--self-test-motion")) {
        bool okX = false;
        bool okY = false;
        const double dx = args.at(2).toDouble(&okX);
        const double dy = args.at(3).toDouble(&okY);
        if (!okX || !okY) {
            qCritical() << "usage: hypr-kdeconnect-portal --self-test-motion <dx> <dy>";
            return 2;
        }
        hkcf::WaylandInput input;
        if (!input.pointerMotion(dx, dy)) {
            qCritical() << input.lastError();
            return 1;
        }
        QThread::msleep(150);
        return 0;
    }

    if (args.size() == 4 && args.at(1) == QStringLiteral("--self-test-absolute")) {
        bool okX = false;
        bool okY = false;
        const double x = args.at(2).toDouble(&okX);
        const double y = args.at(3).toDouble(&okY);
        if (!okX || !okY) {
            qCritical() << "usage: hypr-kdeconnect-portal --self-test-absolute <x> <y>";
            return 2;
        }
        hkcf::WaylandInput input;
        if (!input.pointerMotionAbsolute(x, y)) {
            qCritical() << input.lastError();
            return 1;
        }
        QThread::msleep(150);
        return 0;
    }

    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCritical() << "failed to connect to session bus";
        return 1;
    }

    constexpr auto serviceName = "org.freedesktop.impl.portal.desktop.hypr_kdeconnect";
    if (!bus.registerService(QString::fromLatin1(serviceName))) {
        qCritical() << "failed to own D-Bus service" << serviceName << bus.lastError().message();
        return 1;
    }

    hkcf::PortalBackend backend;
    if (!bus.registerVirtualObject(QStringLiteral("/org/freedesktop/portal/desktop"), &backend, QDBusConnection::SubPath)) {
        qCritical() << "failed to export portal object" << bus.lastError().message();
        return 1;
    }

    qInfo() << "hypr-kdeconnect RemoteDesktop portal backend running";
    return app.exec();
}
