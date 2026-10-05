#include "security_policy.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

#include <linux/input-event-codes.h>

namespace {

int expect(bool condition, const char* message) {
    if (condition)
        return 0;
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

} // namespace

int main() {
    int failures = 0;
    failures += expect(hkcf::security::isAllowedAppId(QStringLiteral("org.deskflow.deskflow")), "Deskflow app id should be accepted");
    failures += expect(!hkcf::security::isAllowedAppId(QStringLiteral("org.deskflow.deskflow.evil")), "Deskflow-like ids should be rejected");
    const QString expected = QStringLiteral("/home/test/.local/bin/deskflow-core");
    failures += expect(hkcf::security::isDeskflowExecutablePath(expected, expected), "exact configured Deskflow path should pass");
    failures += expect(!hkcf::security::isDeskflowExecutablePath(QStringLiteral("/tmp/deskflow-core"), expected), "different binary path should fail");
    failures += expect(!hkcf::security::isDeskflowExecutablePath(QString(), QString()), "empty binary path should fail");
    failures += expect(!hkcf::security::isDeskflowExecutablePath(QStringLiteral("deskflow-core"), QStringLiteral("deskflow-core")), "bare executable should fail");

    failures += expect(hkcf::security::isAllowedFallbackProcess(QStringLiteral("/usr/bin/deskflow-core"), 42, 0, 0),
                       "packaged Deskflow should remain accepted");
    failures += expect(hkcf::security::isAllowedFallbackProcess(expected, 42, 0, 0, expected),
                       "configured native Deskflow should pass without KDE ownership");
    failures += expect(!hkcf::security::isAllowedFallbackProcess(expected, 0, 0, 0, expected),
                       "configured Deskflow still requires a valid sender pid");
    const QString sourceBuild = QStringLiteral("/tmp/deskflow-src/build/bin/deskflow-core");
    failures += expect(!hkcf::security::isAllowedFallbackProcess(sourceBuild, 42, 0, 0, expected),
                       "source-build suffix alone should not pass");
    failures += expect(hkcf::security::isAllowedFallbackProcess(sourceBuild, 42, 0, 0, sourceBuild),
                       "explicitly configured source build should pass");

    failures += expect(hkcf::security::isAllowedAppId(QStringLiteral("org.kde.kdeconnect")), "KDE Connect base app id should be allowed");
    failures += expect(hkcf::security::isAllowedAppId(QStringLiteral("org.kde.kdeconnect.daemon")), "KDE Connect daemon app id should be allowed");
    failures += expect(!hkcf::security::isAllowedAppId(QStringLiteral("surface-transient")), "transient fallback should require caller verification");
    failures += expect(hkcf::security::needsKdeConnectCallerFallback(QStringLiteral("surface-transient")),
                       "known KDE Connect transient fallback should use caller verification");
    failures += expect(hkcf::security::needsKdeConnectCallerFallback(QString()), "empty app id should use caller verification");
    failures += expect(!hkcf::security::isAllowedAppId(QString()), "empty app id should be denied");
    failures += expect(!hkcf::security::isAllowedAppId(QStringLiteral("evil.kdeconnect")), "substring app id spoof should be denied");
    failures += expect(!hkcf::security::isAllowedAppId(QStringLiteral("org.kde.kdeconnect.evil")), "unknown KDE Connect-like suffix should be denied");

    failures += expect(hkcf::security::isValidSessionPath(QStringLiteral("/org/freedesktop/portal/desktop/session/app/token")),
                       "valid portal session path should be accepted");
    failures += expect(hkcf::security::senderBusNameFromSessionPath(QStringLiteral("/org/freedesktop/portal/desktop/session/1_19/token")).value_or(QString()) ==
                           QStringLiteral(":1.19"),
                       "xdg-desktop-portal session sender segment should decode to a D-Bus unique name");
    failures += expect(!hkcf::security::senderBusNameFromSessionPath(QStringLiteral("/org/freedesktop/portal/desktop/session/org_kde_kdeconnect/token")),
                       "non-unique-name session sender segment should not decode to a D-Bus name");
    failures += expect(!hkcf::security::isValidSessionPath(QStringLiteral("/org/freedesktop/portal/desktop/request/app/token")),
                       "request path should not be accepted as a session");
    failures += expect(!hkcf::security::isValidSessionPath(QStringLiteral("/org/freedesktop/portal/desktop/session/")),
                       "empty session suffix should be denied");
    failures += expect(!hkcf::security::isValidSessionPath(QStringLiteral("/org/freedesktop/portal/desktop/session/app//token")),
                       "double slash session path should be denied");

    failures += expect(hkcf::security::boundedFinite(5.0, 10.0).value_or(0.0) == 5.0, "finite value in range should pass");
    failures += expect(hkcf::security::boundedFinite(50.0, 10.0).value_or(0.0) == 10.0, "large value should clamp");
    failures += expect(!hkcf::security::boundedFinite(std::numeric_limits<double>::infinity(), 10.0), "infinity should be denied");
    failures += expect(!hkcf::security::boundedFinite(std::nan(""), 10.0), "NaN should be denied");

    failures += expect(hkcf::security::isAllowedPointerButton(BTN_LEFT), "left mouse button should be allowed");
    failures += expect(hkcf::security::isAllowedPointerButton(BTN_BACK), "back mouse button should be allowed");
    failures += expect(!hkcf::security::isAllowedPointerButton(KEY_ESC), "keyboard code should not be accepted as pointer button");
    failures += expect(hkcf::security::isAllowedKeyboardKeycode(KEY_A), "normal keyboard key should be allowed");
    failures += expect(!hkcf::security::isAllowedKeyboardKeycode(0), "zero keycode should be denied");
    failures += expect(!hkcf::security::isAllowedKeyboardKeycode(KEY_MAX + 1), "oversized keycode should be denied");

    failures += expect(hkcf::security::isValidState(0) && hkcf::security::isValidState(1), "binary input states should be accepted");
    failures += expect(!hkcf::security::isValidState(2), "non-binary input state should be denied");
    failures += expect(hkcf::security::isAllowedDiscreteAxis(0) && hkcf::security::isAllowedDiscreteAxis(1), "known scroll axes should be accepted");
    failures += expect(!hkcf::security::isAllowedDiscreteAxis(2), "unknown scroll axis should be denied");
    failures += expect(hkcf::security::clampDiscreteScrollSteps(500) == hkcf::security::kMaxDiscreteScrollSteps, "scroll steps should clamp high");
    failures += expect(hkcf::security::clampDiscreteScrollSteps(-500) == -hkcf::security::kMaxDiscreteScrollSteps, "scroll steps should clamp low");
    failures += expect(hkcf::security::kNotifyRateWindowMs > 0 && hkcf::security::kMaxNotifyEventsPerWindow > 0,
                       "notify rate limits should be positive");
    failures += expect(hkcf::security::isAllowedFallbackExecutablePath(QStringLiteral("/usr/bin/kdeconnectd")),
                       "system KDE Connect daemon should be allowed as an app-id fallback");
    failures += expect(!hkcf::security::isAllowedFallbackExecutablePath(QStringLiteral("kdeconnectd")),
                       "bare executable names should not be allowed as an app-id fallback");
    failures += expect(!hkcf::security::isAllowedFallbackExecutablePath(QStringLiteral("/tmp/kdeconnectd")),
                       "generic D-Bus clients should not be allowed as an app-id fallback");
    failures += expect(hkcf::security::isAllowedFallbackProcess(QStringLiteral("/usr/bin/kdeconnectd"), 1200, 1200, 0),
                       "KDE Connect owner connection should be allowed as an app-id fallback");
    failures += expect(hkcf::security::isAllowedFallbackProcess(QStringLiteral("/usr/bin/kdeconnectd"), 1200, 1100, 1200),
                       "a second D-Bus connection from the KDE Connect owner process should be allowed");
    failures += expect(!hkcf::security::isAllowedFallbackProcess(QStringLiteral("/usr/bin/kdeconnectd"), 1200, 1100, 1100),
                       "a different process running kdeconnectd should not inherit another daemon's trust");
    failures += expect(!hkcf::security::isAllowedFallbackProcess(QStringLiteral("/tmp/kdeconnectd"), 1200, 1200, 1200),
                       "a matching pid should not allow an untrusted executable path");
    failures += expect(!hkcf::security::isAllowedFallbackProcess(QStringLiteral("/usr/bin/kdeconnectd"), 0, 0, 0),
                       "missing process ids should not be allowed");

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
