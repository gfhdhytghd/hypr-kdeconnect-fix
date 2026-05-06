#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

#include <QString>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

namespace hkcf::security {

constexpr auto kSessionPathPrefix = "/org/freedesktop/portal/desktop/session/";
constexpr qsizetype kMaxAppIdLength = 128;
constexpr qsizetype kMaxSessionPathLength = 256;
constexpr qsizetype kMaxSessions = 8;
constexpr double kMaxPointerDelta = 10000.0;
constexpr double kMaxAbsoluteCoordinate = 100000.0;
constexpr double kMaxAxisDelta = 1000.0;
constexpr int kMaxDiscreteScrollSteps = 120;
constexpr qint64 kNotifyRateWindowMs = 1000;
constexpr int kMaxNotifyEventsPerWindow = 2000;

inline QString normalizedAppId(const QString& appId) {
    return appId.trimmed();
}

inline bool isAllowedAppId(const QString& appId) {
    const QString normalized = normalizedAppId(appId);
    if (normalized.isEmpty() || normalized.size() > kMaxAppIdLength)
        return false;

    return normalized == QStringLiteral("org.kde.kdeconnect") || normalized == QStringLiteral("org.kde.kdeconnect.app") ||
           normalized == QStringLiteral("org.kde.kdeconnect.daemon") || normalized == QStringLiteral("org.kde.kdeconnect.handler") ||
           normalized == QStringLiteral("org.kde.kdeconnect.nonplasma") || normalized == QStringLiteral("org.kde.kdeconnect.sms");
}

inline bool needsKdeConnectCallerFallback(const QString& appId) {
    const QString normalized = normalizedAppId(appId);
    return normalized.isEmpty() || normalized == QStringLiteral("surface-transient");
}

inline bool isPlausibleAppId(const QString& appId) {
    return normalizedAppId(appId).size() <= kMaxAppIdLength;
}

inline bool isValidSessionPath(const QString& path) {
    static const QString prefix = QString::fromLatin1(kSessionPathPrefix);
    return path.size() > prefix.size() && path.size() <= kMaxSessionPathLength && path.startsWith(prefix) && !path.contains(QStringLiteral("//"));
}

inline std::optional<QString> senderBusNameFromSessionPath(const QString& path) {
    if (!isValidSessionPath(path))
        return std::nullopt;

    static const QString prefix = QString::fromLatin1(kSessionPathPrefix);
    const qsizetype senderStart = prefix.size();
    const qsizetype senderEnd = path.indexOf(QLatin1Char('/'), senderStart);
    if (senderEnd <= senderStart)
        return std::nullopt;

    QString encoded = path.mid(senderStart, senderEnd - senderStart);
    if (!encoded.contains(QLatin1Char('_')) || !encoded.at(0).isDigit())
        return std::nullopt;

    for (const QChar ch : encoded) {
        if (!ch.isDigit() && ch != QLatin1Char('_'))
            return std::nullopt;
    }

    encoded.replace(QLatin1Char('_'), QLatin1Char('.'));
    return QStringLiteral(":%1").arg(encoded);
}

inline bool isAllowedFallbackExecutablePath(const QString& executablePath) {
    return executablePath == QStringLiteral("/usr/bin/kdeconnectd") || executablePath == QStringLiteral("/usr/lib/kdeconnectd") ||
           executablePath == QStringLiteral("/usr/libexec/kdeconnectd");
}

inline std::optional<double> boundedFinite(double value, double maxAbs) {
    if (!std::isfinite(value) || maxAbs <= 0.0)
        return std::nullopt;
    return std::clamp(value, -maxAbs, maxAbs);
}

inline bool isValidState(std::uint32_t state) {
    return state == 0 || state == 1;
}

inline bool stateToPressed(std::uint32_t state) {
    return state == 1;
}

inline bool isAllowedPointerButton(std::uint32_t button) {
    return button >= BTN_LEFT && button <= BTN_TASK;
}

inline bool isAllowedKeyboardKeycode(std::uint32_t keycode) {
    return keycode > 0 && keycode <= KEY_MAX;
}

inline bool isAllowedKeysym(std::uint32_t keysym) {
    return keysym > 0 && keysym <= XKB_KEYSYM_MAX;
}

inline bool isAllowedDiscreteAxis(std::uint32_t axis) {
    return axis == 0 || axis == 1;
}

inline int clampDiscreteScrollSteps(int steps) {
    return std::clamp(steps, -kMaxDiscreteScrollSteps, kMaxDiscreteScrollSteps);
}

} // namespace hkcf::security
