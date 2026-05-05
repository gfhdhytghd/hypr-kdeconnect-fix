#include "portal_backend.hpp"

#include <algorithm>

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusVariant>
#include <QDebug>

namespace hkcf {

namespace {

constexpr auto kDesktopPath = "/org/freedesktop/portal/desktop";
constexpr auto kRemoteDesktopInterface = "org.freedesktop.impl.portal.RemoteDesktop";
constexpr auto kSessionInterface = "org.freedesktop.impl.portal.Session";
constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
constexpr auto kIntrospectableInterface = "org.freedesktop.DBus.Introspectable";
constexpr auto kPeerInterface = "org.freedesktop.DBus.Peer";
constexpr auto kNotSupported = "org.freedesktop.DBus.Error.NotSupported";

template <typename T>
std::optional<T> typedArg(const QList<QVariant>& args, int index) {
    if (index < 0 || index >= args.size())
        return std::nullopt;
    if constexpr (std::is_same_v<T, QDBusObjectPath>) {
        if (args[index].canConvert<QDBusObjectPath>())
            return args[index].value<QDBusObjectPath>();
    } else if constexpr (std::is_same_v<T, QVariantMap>) {
        if (args[index].canConvert<QVariantMap>())
            return args[index].toMap();
    } else if constexpr (std::is_same_v<T, QString>) {
        if (args[index].canConvert<QString>())
            return args[index].toString();
    } else if constexpr (std::is_same_v<T, double>) {
        if (args[index].canConvert<double>())
            return args[index].toDouble();
    } else if constexpr (std::is_same_v<T, std::uint32_t>) {
        if (args[index].canConvert<uint>())
            return args[index].toUInt();
    } else if constexpr (std::is_same_v<T, int>) {
        if (args[index].canConvert<int>())
            return args[index].toInt();
    }
    return std::nullopt;
}

std::uint32_t boolState(std::uint32_t state) {
    return state != 0;
}

} // namespace

PortalBackend::PortalBackend(QObject* parent)
    : QDBusVirtualObject(parent) {
}

bool PortalBackend::handleMessage(const QDBusMessage& message, const QDBusConnection& connection) {
    const QString interface = message.interface();

    if (interface == QString::fromLatin1(kRemoteDesktopInterface))
        return handleRemoteDesktop(message, connection);
    if (interface == QString::fromLatin1(kSessionInterface))
        return handleSession(message, connection);
    if (interface == QString::fromLatin1(kPropertiesInterface))
        return handleProperties(message, connection);
    if (interface == QString::fromLatin1(kIntrospectableInterface))
        return handleIntrospectable(message, connection);
    if (interface == QString::fromLatin1(kPeerInterface))
        return handlePeer(message, connection);

    connection.send(error(message, QDBusError::UnknownInterface, QStringLiteral("unknown interface %1").arg(interface)));
    return true;
}

QString PortalBackend::introspect(const QString& path) const {
    if (path == QString::fromLatin1(kDesktopPath)) {
        return QStringLiteral(R"XML(
<node>
  <interface name="org.freedesktop.impl.portal.RemoteDesktop">
    <method name="CreateSession">
      <arg type="o" name="handle" direction="in"/>
      <arg type="o" name="session_handle" direction="in"/>
      <arg type="s" name="app_id" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/>
      <arg type="a{sv}" name="results" direction="out"/>
    </method>
    <method name="SelectDevices">
      <arg type="o" name="handle" direction="in"/>
      <arg type="o" name="session_handle" direction="in"/>
      <arg type="s" name="app_id" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/>
      <arg type="a{sv}" name="results" direction="out"/>
    </method>
    <method name="Start">
      <arg type="o" name="handle" direction="in"/>
      <arg type="o" name="session_handle" direction="in"/>
      <arg type="s" name="app_id" direction="in"/>
      <arg type="s" name="parent_window" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/>
      <arg type="a{sv}" name="results" direction="out"/>
    </method>
    <method name="NotifyPointerMotion">
      <arg type="o" name="session_handle" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="d" name="dx" direction="in"/>
      <arg type="d" name="dy" direction="in"/>
    </method>
    <method name="NotifyPointerMotionAbsolute">
      <arg type="o" name="session_handle" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="stream" direction="in"/>
      <arg type="d" name="x" direction="in"/>
      <arg type="d" name="y" direction="in"/>
    </method>
    <method name="NotifyPointerButton">
      <arg type="o" name="session_handle" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="i" name="button" direction="in"/>
      <arg type="u" name="state" direction="in"/>
    </method>
    <method name="NotifyPointerAxis">
      <arg type="o" name="session_handle" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="d" name="dx" direction="in"/>
      <arg type="d" name="dy" direction="in"/>
    </method>
    <method name="NotifyPointerAxisDiscrete">
      <arg type="o" name="session_handle" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="axis" direction="in"/>
      <arg type="i" name="steps" direction="in"/>
    </method>
    <method name="NotifyKeyboardKeycode">
      <arg type="o" name="session_handle" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="i" name="keycode" direction="in"/>
      <arg type="u" name="state" direction="in"/>
    </method>
    <method name="NotifyKeyboardKeysym">
      <arg type="o" name="session_handle" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="i" name="keysym" direction="in"/>
      <arg type="u" name="state" direction="in"/>
    </method>
    <method name="ConnectToEIS">
      <arg type="o" name="session_handle" direction="in"/>
      <arg type="s" name="app_id" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="h" name="fd" direction="out"/>
    </method>
    <property name="AvailableDeviceTypes" type="u" access="read"/>
    <property name="version" type="u" access="read"/>
  </interface>
  <interface name="org.freedesktop.DBus.Properties">
    <method name="Get">
      <arg type="s" name="interface_name" direction="in"/>
      <arg type="s" name="property_name" direction="in"/>
      <arg type="v" name="value" direction="out"/>
    </method>
    <method name="GetAll">
      <arg type="s" name="interface_name" direction="in"/>
      <arg type="a{sv}" name="properties" direction="out"/>
    </method>
  </interface>
  <interface name="org.freedesktop.DBus.Introspectable">
    <method name="Introspect">
      <arg type="s" name="xml_data" direction="out"/>
    </method>
  </interface>
</node>
)XML");
    }

    if (isSessionPath(path)) {
        return QStringLiteral(R"XML(
<node>
  <interface name="org.freedesktop.impl.portal.Session">
    <method name="Close"/>
    <signal name="Closed"/>
    <property name="version" type="u" access="read"/>
  </interface>
  <interface name="org.freedesktop.DBus.Properties">
    <method name="Get">
      <arg type="s" name="interface_name" direction="in"/>
      <arg type="s" name="property_name" direction="in"/>
      <arg type="v" name="value" direction="out"/>
    </method>
    <method name="GetAll">
      <arg type="s" name="interface_name" direction="in"/>
      <arg type="a{sv}" name="properties" direction="out"/>
    </method>
  </interface>
</node>
)XML");
    }

    return QStringLiteral("<node/>");
}

bool PortalBackend::handleRemoteDesktop(const QDBusMessage& message, const QDBusConnection& connection) {
    const auto args = message.arguments();
    const QString member = message.member();

    if (member == QStringLiteral("CreateSession")) {
        const auto sessionHandle = typedArg<QDBusObjectPath>(args, 1);
        const auto appId = typedArg<QString>(args, 2).value_or(QString());
        if (!sessionHandle) {
            connection.send(error(message, QDBusError::InvalidArgs, QStringLiteral("missing session handle")));
            return true;
        }

        Session session;
        session.appId = appId;
        m_sessions.insert(sessionHandle->path(), session);

        QVariantMap results;
        results.insert(QStringLiteral("session"), sessionHandle->path());
        results.insert(QStringLiteral("session_id"), sessionHandle->path());
        connection.send(response(message, 0, results));
        return true;
    }

    if (member == QStringLiteral("SelectDevices")) {
        const auto sessionHandle = typedArg<QDBusObjectPath>(args, 1);
        const auto options = typedArg<QVariantMap>(args, 3).value_or(QVariantMap());
        if (!sessionHandle || !m_sessions.contains(sessionHandle->path())) {
            connection.send(response(message, 2));
            return true;
        }

        auto& session = m_sessions[sessionHandle->path()];
        session.requestedTypes = options.value(QStringLiteral("types"), kSupportedDevices).toUInt() & kSupportedDevices;
        if (!session.requestedTypes)
            session.requestedTypes = kSupportedDevices;
        connection.send(response(message, 0));
        return true;
    }

    if (member == QStringLiteral("Start")) {
        const auto sessionHandle = typedArg<QDBusObjectPath>(args, 1);
        const auto appId = typedArg<QString>(args, 2).value_or(QString());
        if (!sessionHandle || !m_sessions.contains(sessionHandle->path())) {
            connection.send(response(message, 2));
            return true;
        }

        auto& session = m_sessions[sessionHandle->path()];
        if (!appId.isEmpty())
            session.appId = appId;
        if (!isAllowedApp(session.appId)) {
            qWarning() << "refusing RemoteDesktop session for app id" << session.appId;
            connection.send(response(message, 1));
            return true;
        }
        if (!ensureInputReady(message, connection))
            return true;

        session.started = true;
        QVariantMap results;
        results.insert(QStringLiteral("devices"), session.requestedTypes & kSupportedDevices);
        results.insert(QStringLiteral("clipboard_enabled"), false);
        connection.send(response(message, 0, results));
        return true;
    }

    if (member == QStringLiteral("ConnectToEIS")) {
        connection.send(error(message, QString::fromLatin1(kNotSupported), QStringLiteral("libei transport is not implemented; use Notify methods")));
        return true;
    }

    const auto sessionHandle = typedArg<QDBusObjectPath>(args, 0);
    if (!sessionHandle || !m_sessions.contains(sessionHandle->path()) || !m_sessions[sessionHandle->path()].started) {
        connection.send(message.createReply());
        return true;
    }

    if (member == QStringLiteral("NotifyPointerMotion")) {
        const auto dx = typedArg<double>(args, 2).value_or(0.0);
        const auto dy = typedArg<double>(args, 3).value_or(0.0);
        m_input.pointerMotion(dx, dy);
        connection.send(message.createReply());
        return true;
    }

    if (member == QStringLiteral("NotifyPointerMotionAbsolute")) {
        const auto x = typedArg<double>(args, 3).value_or(0.0);
        const auto y = typedArg<double>(args, 4).value_or(0.0);
        m_input.pointerMotionAbsolute(x, y);
        connection.send(message.createReply());
        return true;
    }

    if (member == QStringLiteral("NotifyPointerButton")) {
        const auto button = typedArg<int>(args, 2).value_or(0);
        const auto state = typedArg<std::uint32_t>(args, 3).value_or(0);
        if (button > 0)
            m_input.pointerButton(static_cast<std::uint32_t>(button), boolState(state));
        connection.send(message.createReply());
        return true;
    }

    if (member == QStringLiteral("NotifyPointerAxis")) {
        const auto dx = typedArg<double>(args, 2).value_or(0.0);
        const auto dy = typedArg<double>(args, 3).value_or(0.0);
        m_input.pointerAxis(dx, dy);
        connection.send(message.createReply());
        return true;
    }

    if (member == QStringLiteral("NotifyPointerAxisDiscrete")) {
        const auto axis = typedArg<std::uint32_t>(args, 2).value_or(0);
        const auto steps = typedArg<int>(args, 3).value_or(0);
        constexpr double kDiscreteStep = 15.0;
        m_input.pointerAxis(axis == 1 ? steps * kDiscreteStep : 0.0, axis == 0 ? steps * kDiscreteStep : 0.0);
        connection.send(message.createReply());
        return true;
    }

    if (member == QStringLiteral("NotifyKeyboardKeycode")) {
        const auto keycode = typedArg<int>(args, 2).value_or(0);
        const auto state = typedArg<std::uint32_t>(args, 3).value_or(0);
        if (keycode > 0)
            m_input.keyboardKeycode(static_cast<std::uint32_t>(keycode), boolState(state));
        connection.send(message.createReply());
        return true;
    }

    if (member == QStringLiteral("NotifyKeyboardKeysym")) {
        const auto keysym = typedArg<int>(args, 2).value_or(0);
        const auto state = typedArg<std::uint32_t>(args, 3).value_or(0);
        if (keysym > 0)
            m_input.keyboardKeysym(static_cast<std::uint32_t>(keysym), boolState(state));
        connection.send(message.createReply());
        return true;
    }

    connection.send(error(message, QDBusError::UnknownMethod, QStringLiteral("unknown RemoteDesktop method %1").arg(member)));
    return true;
}

bool PortalBackend::handleSession(const QDBusMessage& message, const QDBusConnection& connection) {
    if (message.member() != QStringLiteral("Close")) {
        connection.send(error(message, QDBusError::UnknownMethod, QStringLiteral("unknown Session method")));
        return true;
    }

    m_sessions.remove(message.path());
    connection.send(message.createReply());
    connection.send(QDBusMessage::createSignal(message.path(), QString::fromLatin1(kSessionInterface), QStringLiteral("Closed")));
    return true;
}

bool PortalBackend::handleProperties(const QDBusMessage& message, const QDBusConnection& connection) {
    const auto args = message.arguments();
    if (message.member() == QStringLiteral("Get")) {
        const auto interface = typedArg<QString>(args, 0).value_or(QString());
        const auto property = typedArg<QString>(args, 1).value_or(QString());
        const QVariant value = propertyValue(interface, property);
        if (!value.isValid()) {
            connection.send(error(message, QDBusError::InvalidArgs, QStringLiteral("unknown property")));
            return true;
        }
        connection.send(message.createReply(QVariant::fromValue(QDBusVariant(value))));
        return true;
    }

    if (message.member() == QStringLiteral("GetAll")) {
        const auto interface = typedArg<QString>(args, 0).value_or(QString());
        connection.send(message.createReply(propertiesFor(interface)));
        return true;
    }

    connection.send(error(message, QDBusError::UnknownMethod, QStringLiteral("unknown Properties method")));
    return true;
}

bool PortalBackend::handleIntrospectable(const QDBusMessage& message, const QDBusConnection& connection) {
    if (message.member() != QStringLiteral("Introspect")) {
        connection.send(error(message, QDBusError::UnknownMethod, QStringLiteral("unknown Introspectable method")));
        return true;
    }
    connection.send(message.createReply(introspect(message.path())));
    return true;
}

bool PortalBackend::handlePeer(const QDBusMessage& message, const QDBusConnection& connection) {
    if (message.member() == QStringLiteral("Ping")) {
        connection.send(message.createReply());
        return true;
    }
    if (message.member() == QStringLiteral("GetMachineId")) {
        connection.send(message.createReply(QString()));
        return true;
    }
    connection.send(error(message, QDBusError::UnknownMethod, QStringLiteral("unknown Peer method")));
    return true;
}

QVariant PortalBackend::propertyValue(const QString& interface, const QString& property) const {
    if (interface == QString::fromLatin1(kRemoteDesktopInterface)) {
        if (property == QStringLiteral("AvailableDeviceTypes"))
            return QVariant::fromValue(kSupportedDevices);
        if (property == QStringLiteral("version"))
            return QVariant::fromValue(2u);
    }
    if (interface == QString::fromLatin1(kSessionInterface) && property == QStringLiteral("version"))
        return QVariant::fromValue(1u);
    return {};
}

QVariantMap PortalBackend::propertiesFor(const QString& interface) const {
    QVariantMap map;
    if (interface == QString::fromLatin1(kRemoteDesktopInterface)) {
        map.insert(QStringLiteral("AvailableDeviceTypes"), kSupportedDevices);
        map.insert(QStringLiteral("version"), 2u);
    } else if (interface == QString::fromLatin1(kSessionInterface)) {
        map.insert(QStringLiteral("version"), 1u);
    }
    return map;
}

bool PortalBackend::ensureInputReady(const QDBusMessage& message, const QDBusConnection& connection) {
    if (m_input.ensureReady())
        return true;
    connection.send(response(message, 2, {{QStringLiteral("error"), m_input.lastError()}}));
    return false;
}

bool PortalBackend::isAllowedApp(const QString& appId) const {
    return appId.isEmpty() || appId.contains(QStringLiteral("kdeconnect"), Qt::CaseInsensitive) || appId == QStringLiteral("surface-transient");
}

bool PortalBackend::isSessionPath(const QString& path) const {
    return path.startsWith(QString::fromLatin1(kDesktopPath) + QStringLiteral("/session/"));
}

QDBusMessage PortalBackend::response(const QDBusMessage& message, std::uint32_t code, const QVariantMap& results) const {
    return message.createReply(QList<QVariant>{QVariant::fromValue(code), results});
}

QDBusMessage PortalBackend::error(const QDBusMessage& message, const QString& name, const QString& text) const {
    return message.createErrorReply(name, text);
}

QDBusMessage PortalBackend::error(const QDBusMessage& message, QDBusError::ErrorType type, const QString& text) const {
    return message.createErrorReply(type, text);
}

} // namespace hkcf
