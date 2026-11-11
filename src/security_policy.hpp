#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>

#include <QString>
#include <QStringDecoder>

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
constexpr std::size_t kMaxTextUtf8Bytes = 4096;
constexpr int kKeymapReplacementsPerSecond = 30;
// One full slot cache per burst, so a pasted text with many new symbols is not truncated.
constexpr int kKeymapReplacementBurst = 64;
constexpr std::int64_t kKeymapWarningIntervalMs = 1000;

// KDE Connect release/26.04 and release/26.08, mousepad/waylandremoteinput.cpp
// SpecialKeysMap (excluding its invalid zero entry), then the other modifiers.
// These codes must never become lookup slots, even when absent from the map.
inline constexpr std::array kKdeConnectRawKeycodes = {
    KEY_BACKSPACE, KEY_TAB, KEY_LINEFEED, KEY_LEFT, KEY_UP, KEY_RIGHT, KEY_DOWN,
    KEY_PAGEUP, KEY_PAGEDOWN, KEY_HOME, KEY_END, KEY_ENTER, KEY_DELETE, KEY_ESC,
    KEY_SYSRQ, KEY_SCROLLLOCK, KEY_LEFTCTRL, KEY_LEFTALT, KEY_LEFTSHIFT, KEY_LEFTMETA,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_RIGHTCTRL, KEY_RIGHTALT, KEY_RIGHTSHIFT, KEY_RIGHTMETA, KEY_CAPSLOCK, KEY_NUMLOCK, KEY_COMPOSE, KEY_FN,
};

// Reject the complete event before injecting any input, including incomplete
// trailing sequences. A text event is not a streaming UTF-8 fragment.
inline std::optional<QString> decodeTextUtf8(const char* text) {
    if (!text)
        return std::nullopt;
    const auto length = strnlen(text, kMaxTextUtf8Bytes + 1);
    if (length > kMaxTextUtf8Bytes)
        return std::nullopt;
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const QString decoded = decoder(QByteArrayView(text, static_cast<qsizetype>(length)));
    if (decoder.hasError())
        return std::nullopt;
    return decoded;
}

inline QString normalizedAppId(const QString& appId) {
    return appId.trimmed();
}

inline bool isAllowedAppId(const QString& appId) {
    const QString normalized = normalizedAppId(appId);
    if (normalized.isEmpty() || normalized.size() > kMaxAppIdLength)
        return false;

    return normalized == QStringLiteral("org.deskflow.deskflow") ||
           normalized == QStringLiteral("org.kde.kdeconnect") || normalized == QStringLiteral("org.kde.kdeconnect.app") ||
           normalized == QStringLiteral("org.kde.kdeconnect.daemon") || normalized == QStringLiteral("org.kde.kdeconnect.handler") ||
           normalized == QStringLiteral("org.kde.kdeconnect.nonplasma") || normalized == QStringLiteral("org.kde.kdeconnect.sms");
}

inline bool needsKdeConnectCallerFallback(const QString& appId) {
    const QString normalized = normalizedAppId(appId);
    return normalized.isEmpty() || normalized == QStringLiteral("surface-transient");
}

// verifiedExecutable must come from the existing D-Bus PID/owner verification.
// Never infer keyboard semantics from a sender-controlled libei display name.
inline bool usesLookupKeyboard(const QString& appId, const QString& verifiedExecutable = {}) {
    const auto id = normalizedAppId(appId);
    if (isAllowedAppId(id) && (id == QStringLiteral("org.kde.kdeconnect") || id.startsWith(QStringLiteral("org.kde.kdeconnect."))))
        return true;
    return needsKdeConnectCallerFallback(id) &&
           (verifiedExecutable == QStringLiteral("/usr/bin/kdeconnectd") ||
            verifiedExecutable == QStringLiteral("/usr/lib/kdeconnectd") ||
            verifiedExecutable == QStringLiteral("/usr/libexec/kdeconnectd"));
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

// Keep packaged Deskflow working and allow one explicitly configured source
// or user-local installation instead of trusting a build-directory suffix.
inline bool isDeskflowExecutablePath(const QString& executablePath, const QString& configuredPath = {}) {
    return executablePath == QStringLiteral("/usr/bin/deskflow-core") ||
           (configuredPath.startsWith('/') && executablePath == configuredPath);
}

inline bool isAllowedFallbackProcess(const QString& executablePath,
                                     std::uint32_t senderPid,
                                     std::uint32_t kdeConnectOwnerPid,
                                     std::uint32_t kdeConnectDaemonOwnerPid,
                                     const QString& deskflowExecutablePath = {}) {
    if (senderPid == 0)
        return false;

    if (isDeskflowExecutablePath(executablePath, deskflowExecutablePath))
        return true;

    if (!isAllowedFallbackExecutablePath(executablePath))
        return false;

    return senderPid == kdeConnectOwnerPid || senderPid == kdeConnectDaemonOwnerPid;
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
