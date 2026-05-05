#pragma once

#include "wayland_input.hpp"

#include <QDBusConnection>
#include <QDBusError>
#include <QDBusObjectPath>
#include <QDBusVirtualObject>
#include <QHash>
#include <QVariantMap>

namespace hkcf {

class PortalBackend : public QDBusVirtualObject {
    Q_OBJECT

  public:
    explicit PortalBackend(QObject* parent = nullptr);

    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override;
    QString introspect(const QString& path) const override;

  private:
    struct Session {
        QString appId;
        std::uint32_t requestedTypes = 3;
        bool started = false;
    };

    static constexpr std::uint32_t kKeyboard = 1;
    static constexpr std::uint32_t kPointer = 2;
    static constexpr std::uint32_t kSupportedDevices = kKeyboard | kPointer;

    [[nodiscard]] bool isAllowedApp(const QString& appId) const;
    [[nodiscard]] bool isSessionPath(const QString& path) const;
    [[nodiscard]] QDBusMessage response(const QDBusMessage& message, std::uint32_t code, const QVariantMap& results = {}) const;
    [[nodiscard]] QDBusMessage error(const QDBusMessage& message, const QString& name, const QString& text) const;
    [[nodiscard]] QDBusMessage error(const QDBusMessage& message, QDBusError::ErrorType type, const QString& text) const;

    bool handleRemoteDesktop(const QDBusMessage& message, const QDBusConnection& connection);
    bool handleSession(const QDBusMessage& message, const QDBusConnection& connection);
    bool handleProperties(const QDBusMessage& message, const QDBusConnection& connection);
    bool handleIntrospectable(const QDBusMessage& message, const QDBusConnection& connection);
    bool handlePeer(const QDBusMessage& message, const QDBusConnection& connection);

    QVariant propertyValue(const QString& interface, const QString& property) const;
    QVariantMap propertiesFor(const QString& interface) const;
    bool ensureInputReady(const QDBusMessage& message, const QDBusConnection& connection);

    QHash<QString, Session> m_sessions;
    WaylandInput m_input;
};

} // namespace hkcf
