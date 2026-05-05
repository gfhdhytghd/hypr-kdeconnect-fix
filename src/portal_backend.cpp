#include "portal_backend.hpp"

#include "security_policy.hpp"

#include <algorithm>
#include <optional>

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusReply>
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
constexpr auto kPortalFrontendService = "org.freedesktop.portal.Desktop";

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
        bool ok = false;
        const double value = args[index].toDouble(&ok);
        if (ok)
            return value;
    } else if constexpr (std::is_same_v<T, std::uint32_t>) {
        bool ok = false;
        const uint value = args[index].toUInt(&ok);
        if (ok)
            return value;
    } else if constexpr (std::is_same_v<T, int>) {
        bool ok = false;
        const int value = args[index].toInt(&ok);
        if (ok)
            return value;
    }
    return std::nullopt;
}

QVariant unwrapDbusVariant(const QVariant& value) {
    if (value.metaType() == QMetaType::fromType<QDBusVariant>())
        return value.value<QDBusVariant>().variant();
    return value;
}

std::optional<std::uint32_t> uintOption(const QVariantMap& options, const QString& key, std::uint32_t fallback) {
    if (!options.contains(key))
        return fallback;

    bool ok = false;
    const uint value = unwrapDbusVariant(options.value(key)).toUInt(&ok);
    if (!ok)
        return std::nullopt;
    return value;
}

QString safeForLog(QString value) {
    value = value.left(hkcf::security::kMaxAppIdLength);
    for (QChar& ch : value) {
        if (ch.unicode() < 0x20 || ch.unicode() == 0x7f)
            ch = QLatin1Char('?');
    }
    return value;
}

std::optional<double> boundedDoubleArg(const QList<QVariant>& args, int index, double maxAbs) {
    const auto value = typedArg<double>(args, index);
    if (!value)
        return std::nullopt;
    return hkcf::security::boundedFinite(*value, maxAbs);
}

} // namespace

PortalBackend::PortalBackend(QObject* parent)
    : QDBusVirtualObject(parent) {
    m_rateTimer.start();
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
    if (!isTrustedPortalCaller(message, connection)) {
        connection.send(error(message, QDBusError::AccessDenied, QStringLiteral("RemoteDesktop backend accepts calls only from xdg-desktop-portal")));
        return true;
    }

    const auto args = message.arguments();
    const QString member = message.member();

    if (member == QStringLiteral("CreateSession")) {
        const auto requestHandle = typedArg<QDBusObjectPath>(args, 0);
        const auto sessionHandle = typedArg<QDBusObjectPath>(args, 1);
        const auto appId = typedArg<QString>(args, 2);
        const auto options = typedArg<QVariantMap>(args, 3);
        if (args.size() != 4 || !requestHandle || !sessionHandle || !appId || !options) {
            connection.send(error(message, QDBusError::InvalidArgs, QStringLiteral("invalid CreateSession arguments")));
            return true;
        }

        const QString path = sessionHandle->path();
        if (!isSessionPath(path)) {
            connection.send(error(message, QDBusError::InvalidObjectPath, QStringLiteral("invalid session handle path")));
            return true;
        }
        if (!security::isPlausibleAppId(*appId)) {
            connection.send(error(message, QDBusError::InvalidArgs, QStringLiteral("invalid app id")));
            return true;
        }
        if (m_sessions.contains(path)) {
            connection.send(error(message, QDBusError::InvalidArgs, QStringLiteral("session handle already exists")));
            return true;
        }
        if (m_sessions.size() >= security::kMaxSessions) {
            connection.send(error(message, QDBusError::LimitsExceeded, QStringLiteral("too many active sessions")));
            return true;
        }

        Session session;
        session.owner = message.service();
        session.appId = security::normalizedAppId(*appId);
        m_sessions.insert(path, session);

        QVariantMap results;
        results.insert(QStringLiteral("session"), path);
        results.insert(QStringLiteral("session_id"), path);
        connection.send(response(message, 0, results));
        return true;
    }

    if (member == QStringLiteral("SelectDevices")) {
        const auto sessionHandle = typedArg<QDBusObjectPath>(args, 1);
        const auto options = typedArg<QVariantMap>(args, 3);
        if (args.size() != 4 || !typedArg<QDBusObjectPath>(args, 0) || !sessionHandle || !typedArg<QString>(args, 2) || !options) {
            connection.send(error(message, QDBusError::InvalidArgs, QStringLiteral("invalid SelectDevices arguments")));
            return true;
        }

        auto session = m_sessions.find(sessionHandle->path());
        if (session == m_sessions.end()) {
            connection.send(response(message, 2));
            return true;
        }
        if (!isSessionOwner(message, *session)) {
            connection.send(error(message, QDBusError::AccessDenied, QStringLiteral("session belongs to another D-Bus caller")));
            return true;
        }

        const auto requestedTypes = uintOption(*options, QStringLiteral("types"), kSupportedDevices);
        if (!requestedTypes) {
            connection.send(error(message, QDBusError::InvalidArgs, QStringLiteral("invalid device type mask")));
            return true;
        }

        session->requestedTypes = *requestedTypes & kSupportedDevices;
        if (!session->requestedTypes) {
            connection.send(response(message, 2, {{QStringLiteral("error"), QStringLiteral("no supported devices selected")}}));
            return true;
        }
        session->devicesSelected = true;
        connection.send(response(message, 0));
        return true;
    }

    if (member == QStringLiteral("Start")) {
        const auto sessionHandle = typedArg<QDBusObjectPath>(args, 1);
        const auto appId = typedArg<QString>(args, 2);
        if (args.size() != 5 || !typedArg<QDBusObjectPath>(args, 0) || !sessionHandle || !appId || !typedArg<QString>(args, 3) ||
            !typedArg<QVariantMap>(args, 4)) {
            connection.send(error(message, QDBusError::InvalidArgs, QStringLiteral("invalid Start arguments")));
            return true;
        }

        auto session = m_sessions.find(sessionHandle->path());
        if (session == m_sessions.end()) {
            connection.send(response(message, 2));
            return true;
        }
        if (!isSessionOwner(message, *session)) {
            connection.send(error(message, QDBusError::AccessDenied, QStringLiteral("session belongs to another D-Bus caller")));
            return true;
        }

        if (!session->devicesSelected) {
            connection.send(response(message, 2, {{QStringLiteral("error"), QStringLiteral("devices were not selected")}}));
            return true;
        }
        if (!appId->isEmpty())
            session->appId = security::normalizedAppId(*appId);
        if (!isAllowedApp(session->appId)) {
            qWarning() << "refusing RemoteDesktop session for app id" << safeForLog(session->appId);
            connection.send(response(message, 1, {{QStringLiteral("error"), QStringLiteral("app id is not allowed")}}));
            return true;
        }
        if (!ensureInputReady(message, connection))
            return true;

        session->started = true;
        QVariantMap results;
        results.insert(QStringLiteral("devices"), session->requestedTypes & kSupportedDevices);
        results.insert(QStringLiteral("clipboard_enabled"), false);
        connection.send(response(message, 0, results));
        return true;
    }

    if (member == QStringLiteral("ConnectToEIS")) {
        connection.send(error(message, QString::fromLatin1(kNotSupported), QStringLiteral("libei transport is not implemented; use Notify methods")));
        return true;
    }

    const auto sessionHandle = typedArg<QDBusObjectPath>(args, 0);
    if (!sessionHandle) {
        connection.send(error(message, QDBusError::InvalidArgs, QStringLiteral("missing session handle")));
        return true;
    }
    auto session = m_sessions.find(sessionHandle->path());
    if (session == m_sessions.end() || !session->started) {
        connection.send(message.createReply());
        return true;
    }
    if (!isSessionOwner(message, *session)) {
        connection.send(error(message, QDBusError::AccessDenied, QStringLiteral("session belongs to another D-Bus caller")));
        return true;
    }
    if (!checkNotifyRate(message, connection, *session))
        return true;

    if (member == QStringLiteral("NotifyPointerMotion")) {
        if (args.size() != 4 || !typedArg<QVariantMap>(args, 1) || !hasDevice(*session, kPointer)) {
            connection.send(error(message, QDBusError::AccessDenied, QStringLiteral("pointer device was not selected")));
            return true;
        }
        const auto dx = boundedDoubleArg(args, 2, security::kMaxPointerDelta);
        const auto dy = boundedDoubleArg(args, 3, security::kMaxPointerDelta);
        if (!dx || !dy) {
            connection.send(error(message, QDBusError::InvalidArgs, QStringLiteral("invalid pointer motion delta")));
            return true;
        }
        return sendInputResult(message, connection, m_input.pointerMotion(*dx, *dy));
    }

    if (member == QStringLiteral("NotifyPointerMotionAbsolute")) {
        if (args.size() != 5 || !typedArg<QVariantMap>(args, 1) || !typedArg<std::uint32_t>(args, 2) || !hasDevice(*session, kPointer)) {
            connection.send(error(message, QDBusError::AccessDenied, QStringLiteral("pointer device was not selected")));
            return true;
        }
        const auto x = boundedDoubleArg(args, 3, security::kMaxAbsoluteCoordinate);
        const auto y = boundedDoubleArg(args, 4, security::kMaxAbsoluteCoordinate);
        if (!x || !y) {
            connection.send(error(message, QDBusError::InvalidArgs, QStringLiteral("invalid absolute pointer coordinates")));
            return true;
        }
        return sendInputResult(message, connection, m_input.pointerMotionAbsolute(*x, *y));
    }

    if (member == QStringLiteral("NotifyPointerButton")) {
        const auto button = typedArg<int>(args, 2);
        const auto state = typedArg<std::uint32_t>(args, 3);
        if (args.size() != 4 || !typedArg<QVariantMap>(args, 1) || !button || !state || *button < 0 || !hasDevice(*session, kPointer)) {
            connection.send(error(message, QDBusError::InvalidArgs, QStringLiteral("invalid pointer button arguments")));
            return true;
        }
        const auto buttonCode = static_cast<std::uint32_t>(*button);
        if (!security::isAllowedPointerButton(buttonCode) || !security::isValidState(*state)) {
            connection.send(error(message, QDBusError::InvalidArgs, QStringLiteral("invalid pointer button or state")));
            return true;
        }
        return sendInputResult(message, connection, m_input.pointerButton(buttonCode, security::stateToPressed(*state)));
    }

    if (member == QStringLiteral("NotifyPointerAxis")) {
        if (args.size() != 4 || !typedArg<QVariantMap>(args, 1) || !hasDevice(*session, kPointer)) {
            connection.send(error(message, QDBusError::AccessDenied, QStringLiteral("pointer device was not selected")));
            return true;
        }
        const auto dx = boundedDoubleArg(args, 2, security::kMaxAxisDelta);
        const auto dy = boundedDoubleArg(args, 3, security::kMaxAxisDelta);
        if (!dx || !dy) {
            connection.send(error(message, QDBusError::InvalidArgs, QStringLiteral("invalid pointer axis delta")));
            return true;
        }
        return sendInputResult(message, connection, m_input.pointerAxis(*dx, *dy));
    }

    if (member == QStringLiteral("NotifyPointerAxisDiscrete")) {
        const auto axis = typedArg<std::uint32_t>(args, 2);
        const auto steps = typedArg<int>(args, 3);
        if (args.size() != 4 || !typedArg<QVariantMap>(args, 1) || !axis || !steps || !hasDevice(*session, kPointer)) {
            connection.send(error(message, QDBusError::InvalidArgs, QStringLiteral("invalid discrete axis arguments")));
            return true;
        }
        if (!security::isAllowedDiscreteAxis(*axis)) {
            connection.send(error(message, QDBusError::InvalidArgs, QStringLiteral("invalid discrete axis")));
            return true;
        }
        constexpr double kDiscreteStep = 15.0;
        const int boundedSteps = security::clampDiscreteScrollSteps(*steps);
        return sendInputResult(message, connection, m_input.pointerAxis(*axis == 1 ? boundedSteps * kDiscreteStep : 0.0, *axis == 0 ? boundedSteps * kDiscreteStep : 0.0));
    }

    if (member == QStringLiteral("NotifyKeyboardKeycode")) {
        const auto keycode = typedArg<int>(args, 2);
        const auto state = typedArg<std::uint32_t>(args, 3);
        if (args.size() != 4 || !typedArg<QVariantMap>(args, 1) || !keycode || !state || *keycode < 0 || !hasDevice(*session, kKeyboard)) {
            connection.send(error(message, QDBusError::InvalidArgs, QStringLiteral("invalid keyboard keycode arguments")));
            return true;
        }
        const auto keycodeValue = static_cast<std::uint32_t>(*keycode);
        if (!security::isAllowedKeyboardKeycode(keycodeValue) || !security::isValidState(*state)) {
            connection.send(error(message, QDBusError::InvalidArgs, QStringLiteral("invalid keyboard keycode or state")));
            return true;
        }
        return sendInputResult(message, connection, m_input.keyboardKeycode(keycodeValue, security::stateToPressed(*state)));
    }

    if (member == QStringLiteral("NotifyKeyboardKeysym")) {
        const auto keysym = typedArg<int>(args, 2);
        const auto state = typedArg<std::uint32_t>(args, 3);
        if (args.size() != 4 || !typedArg<QVariantMap>(args, 1) || !keysym || !state || *keysym < 0 || !hasDevice(*session, kKeyboard)) {
            connection.send(error(message, QDBusError::InvalidArgs, QStringLiteral("invalid keyboard keysym arguments")));
            return true;
        }
        const auto keysymValue = static_cast<std::uint32_t>(*keysym);
        if (!security::isAllowedKeysym(keysymValue) || !security::isValidState(*state)) {
            connection.send(error(message, QDBusError::InvalidArgs, QStringLiteral("invalid keyboard keysym or state")));
            return true;
        }
        return sendInputResult(message, connection, m_input.keyboardKeysym(keysymValue, security::stateToPressed(*state)));
    }

    connection.send(error(message, QDBusError::UnknownMethod, QStringLiteral("unknown RemoteDesktop method %1").arg(member)));
    return true;
}

bool PortalBackend::handleSession(const QDBusMessage& message, const QDBusConnection& connection) {
    if (!isTrustedPortalCaller(message, connection)) {
        connection.send(error(message, QDBusError::AccessDenied, QStringLiteral("Session backend accepts calls only from xdg-desktop-portal")));
        return true;
    }

    if (message.member() != QStringLiteral("Close")) {
        connection.send(error(message, QDBusError::UnknownMethod, QStringLiteral("unknown Session method")));
        return true;
    }

    auto session = m_sessions.find(message.path());
    if (session == m_sessions.end()) {
        connection.send(message.createReply());
        return true;
    }
    if (!isSessionOwner(message, *session)) {
        connection.send(error(message, QDBusError::AccessDenied, QStringLiteral("session belongs to another D-Bus caller")));
        return true;
    }

    m_sessions.erase(session);
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
    qWarning() << "failed to initialize Wayland input:" << m_input.lastError();
    connection.send(response(message, 2, {{QStringLiteral("error"), QStringLiteral("input backend unavailable")}}));
    return false;
}

bool PortalBackend::sendInputResult(const QDBusMessage& message, const QDBusConnection& connection, bool ok) {
    if (ok) {
        connection.send(message.createReply());
        return true;
    }

    qWarning() << "input injection failed:" << m_input.lastError();
    connection.send(error(message, QDBusError::Failed, QStringLiteral("input injection failed")));
    return true;
}

bool PortalBackend::checkNotifyRate(const QDBusMessage& message, const QDBusConnection& connection, Session& session) {
    const qint64 now = m_rateTimer.elapsed();
    if (now - session.rateWindowStartMs >= security::kNotifyRateWindowMs) {
        session.rateWindowStartMs = now;
        session.eventsInRateWindow = 0;
    }

    ++session.eventsInRateWindow;
    if (session.eventsInRateWindow <= security::kMaxNotifyEventsPerWindow)
        return true;

    connection.send(error(message, QDBusError::LimitsExceeded, QStringLiteral("too many input events")));
    return false;
}

bool PortalBackend::isAllowedApp(const QString& appId) const {
    return security::isAllowedAppId(appId);
}

bool PortalBackend::isTrustedPortalCaller(const QDBusMessage& message, const QDBusConnection& connection) const {
    const QString sender = message.service();
    if (sender.isEmpty())
        return false;

    QDBusConnectionInterface* bus = connection.interface();
    if (!bus)
        return false;

    const QDBusReply<QString> owner = bus->serviceOwner(QString::fromLatin1(kPortalFrontendService));
    return owner.isValid() && !owner.value().isEmpty() && owner.value() == sender;
}

bool PortalBackend::isSessionOwner(const QDBusMessage& message, const Session& session) const {
    return !session.owner.isEmpty() && session.owner == message.service();
}

bool PortalBackend::isSessionPath(const QString& path) const {
    return security::isValidSessionPath(path);
}

bool PortalBackend::hasDevice(const Session& session, std::uint32_t device) const {
    return session.devicesSelected && (session.requestedTypes & device) == device;
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
