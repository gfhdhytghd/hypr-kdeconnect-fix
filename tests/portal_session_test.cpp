// Real portal and EIS bridge on a private bus, with a recording Wayland sink.
#include "portal_backend.hpp"
#include "security_policy.hpp"

#include <QCoreApplication>
#include <QDBusPendingCallWatcher>
#include <QDBusUnixFileDescriptor>
#include <QEventLoop>
#include <QThread>
#include <QSet>
#include <libei.h>
#include <linux/input-event-codes.h>
#include <unistd.h>
#include <poll.h>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <cstdlib>
#include <xkbcommon/xkbcommon.h>

namespace {
QSet<uint32_t> keys, buttons;
int inputEvents = 0;
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template<typename Condition, typename Pump>
void waitFor(Condition condition, Pump pump, const char* message) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 3000) {
        pump();
        QThread::msleep(1);
    }
    check(condition(), message);
}
}

namespace hkcf {
WaylandInput::WaylandInput() = default;
WaylandInput::~WaylandInput() = default;
std::optional<ResolvedKey> WaylandInput::resolveKeysym(std::uint32_t symbol) {
    return m_keyResolver.resolveOrExtend(symbol, m_pressedKeycodes, m_xkbState);
}
bool WaylandInput::ensureReady() { return true; }
QString WaylandInput::lastError() const { return {}; }
QRect WaylandInput::logicalBounds() const { return {0, 0, 1920, 1080}; }
bool WaylandInput::pointerMotion(double, double) { ++inputEvents; return true; }
bool WaylandInput::pointerMotionAbsolute(double, double) { ++inputEvents; return true; }
bool WaylandInput::pointerButton(uint32_t code, bool pressed) {
    ++inputEvents;
    if (pressed) buttons.insert(code); else buttons.remove(code);
    return true;
}
bool WaylandInput::pointerAxis(double, double) { ++inputEvents; return true; }
bool WaylandInput::pointerAxisDiscrete(uint32_t, int) { ++inputEvents; return true; }
bool WaylandInput::keyboardKeycode(uint32_t code, bool pressed, bool) {
    ++inputEvents;
    if (pressed) keys.insert(code); else keys.remove(code);
    return true;
}
bool WaylandInput::keyboardKeysym(uint32_t, bool) { ++inputEvents; return true; }
}

namespace {
const QString desktop = QStringLiteral("/org/freedesktop/portal/desktop");
const QString remote = QStringLiteral("org.freedesktop.impl.portal.RemoteDesktop");
const QString service = QStringLiteral("org.freedesktop.impl.portal.desktop.test");

struct EisClient {
    ei* client = ei_new_sender(nullptr);
    ei_device* keyboard = nullptr;
    ei_device* pointer = nullptr;
    bool disconnected = false;
    uint32_t capabilities = 0;
    bool seatSeen = false;
    bool textCapability = false;
    std::string keymapText;

    explicit EisClient(int fd) {
        check(ei_setup_backend_fd(client, fd) == 0, "EIS fd setup");
    }
    ~EisClient() {
        if (keyboard) ei_device_unref(keyboard);
        if (pointer) ei_device_unref(pointer);
        ei_unref(client);
    }
    void pump() {
        QCoreApplication::processEvents();
        ei_dispatch(client);
        while (auto* event = ei_get_event(client)) {
            auto* device = ei_event_get_device(event);
            switch (ei_event_get_type(event)) {
            case EI_EVENT_DEVICE_ADDED:
                if (ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD)) {
                    auto* map = ei_device_keyboard_get_keymap(device);
                    check(map && ei_keymap_get_type(map) == EI_KEYMAP_TYPE_XKB, "portal keyboard map");
                    keymapText.resize(ei_keymap_get_size(map));
                    check(pread(ei_keymap_get_fd(map), keymapText.data(), keymapText.size(), 0) ==
                          static_cast<ssize_t>(keymapText.size()), "read portal keyboard map");
                }
                break;
            case EI_EVENT_SEAT_ADDED:
                seatSeen = true;
                textCapability = ei_seat_has_capability(ei_event_get_seat(event), EI_DEVICE_CAP_TEXT);
                if (ei_seat_has_capability(ei_event_get_seat(event), EI_DEVICE_CAP_KEYBOARD)) capabilities |= 1;
                for (auto cap : {EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_POINTER_ABSOLUTE, EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_SCROLL})
                    if (ei_seat_has_capability(ei_event_get_seat(event), cap)) capabilities |= 2;
                ei_seat_bind_capabilities(ei_event_get_seat(event), EI_DEVICE_CAP_KEYBOARD,
                    EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_POINTER_ABSOLUTE,
                    EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_SCROLL, nullptr);
                break;
            case EI_EVENT_DEVICE_RESUMED:
                if (ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD) && !keyboard)
                    keyboard = ei_device_ref(device);
                if (ei_device_has_capability(device, EI_DEVICE_CAP_POINTER) && !pointer)
                    pointer = ei_device_ref(device);
                ei_device_start_emulating(device, 1);
                break;
            case EI_EVENT_DISCONNECT: disconnected = true; break;
            default: break;
            }
            ei_event_unref(event);
        }
    }
    void discover(uint32_t types = 3) {
        waitFor([&] { return seatSeen && (!(types & 1) || keyboard) && (!(types & 2) || pointer); },
                [&] { pump(); }, "EIS discovery");
    }
    void key(uint32_t code, bool pressed) {
        ei_device_keyboard_key(keyboard, code, pressed);
        ei_device_frame(keyboard, ei_now(client));
    }
    void button(uint32_t code, bool pressed) {
        ei_device_button_button(pointer, code, pressed);
        ei_device_frame(pointer, ei_now(client));
    }
};

struct Portal {
    QDBusConnection backendBus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "backend");
    QDBusConnection frontend = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "frontend");
    QDBusConnection owner = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "owner");
    hkcf::PortalBackend backend;
    int serial = 0;

    Portal() {
        check(frontend.registerService("org.freedesktop.portal.Desktop"), "private frontend service");
        check(backendBus.registerService(service), "private backend service");
        check(backendBus.registerVirtualObject(desktop, &backend, QDBusConnection::SubPath), "export backend");
    }
    QDBusMessage call(const QString& path, const QString& interface, const QString& method, const QList<QVariant>& args = {}) {
        auto message = QDBusMessage::createMethodCall(service, path, interface, method);
        message.setArguments(args);
        QDBusPendingCallWatcher watcher(frontend.asyncCall(message, 3000));
        QEventLoop loop;
        QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
        if (!watcher.isFinished()) loop.exec();
        const auto reply = watcher.reply();
        return reply;
    }
    static void success(const QDBusMessage& reply) {
        check(reply.type() != QDBusMessage::ErrorMessage, qPrintable(reply.errorMessage()));
        check(!reply.arguments().isEmpty() && reply.arguments().first().toUInt() == 0, "portal success");
    }
    QString create(uint32_t types = 3, const QString& app = QStringLiteral("org.kde.kdeconnect")) {
        QString sender = owner.baseService().mid(1);
        sender.replace('.', '_');
        const QString path = desktop + "/session/" + sender + "/test" + QString::number(++serial);
        const auto request = QVariant::fromValue(QDBusObjectPath(desktop + "/request/" + sender + "/test"));
        const auto session = QVariant::fromValue(QDBusObjectPath(path));
        success(call(desktop, remote, "CreateSession", {request, session, app, QVariantMap{}}));
        success(call(desktop, remote, "SelectDevices", {request, session, app, QVariantMap{{"types", types}}}));
        success(call(desktop, remote, "Start", {request, session, app, QString(), QVariantMap{}}));
        return path;
    }
    QDBusMessage notify(const QString& path, const QString& method, const QList<QVariant>& values) {
        QList<QVariant> args{QVariant::fromValue(QDBusObjectPath(path)), QVariantMap{}};
        args.append(values);
        return call(desktop, remote, method, args);
    }
    int connectEis(const QString& path, const QString& app = QStringLiteral("org.kde.kdeconnect")) {
        const auto reply = call(desktop, remote, "ConnectToEIS",
            {QVariant::fromValue(QDBusObjectPath(path)), app, QVariantMap{}});
        check(reply.type() != QDBusMessage::ErrorMessage, "ConnectToEIS");
        const auto fd = qvariant_cast<QDBusUnixFileDescriptor>(reply.arguments().first());
        check(fd.isValid(), "valid EIS fd");
        return dup(fd.fileDescriptor());
    }
    void close(const QString& path) {
        check(call(path, "org.freedesktop.impl.portal.Session", "Close").type() != QDBusMessage::ErrorMessage, "Session.Close");
    }
};

void testKeymapSelection(Portal& portal) {
    auto* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    auto* base = xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    check(base, "reference real-layout keymap");
    char* original = xkb_keymap_get_as_string(base, XKB_KEYMAP_FORMAT_TEXT_V1);
    check(original, "serialized real-layout keymap");
    for (const auto& app : {QStringLiteral("org.kde.kdeconnect"), QStringLiteral("org.deskflow.deskflow"), QStringLiteral("org.keepassxc.KeePassXC")}) {
        const auto path = portal.create(1, app);
        EisClient client(portal.connectEis(path, app));
        client.discover(1);
        if (app != QStringLiteral("org.kde.kdeconnect")) {
            check(client.keymapText == std::string(original, strlen(original) + 1), "Deskflow/KeePassXC portal app receives unchanged map");
        } else {
            auto* lookup = xkb_keymap_new_from_string(context, client.keymapText.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
            check(lookup, "KDE portal lookup map compiles");
            const xkb_keysym_t* syms;
            check(xkb_keymap_num_levels_for_key(lookup, KEY_A + 8, 0) == 1 &&
                  xkb_keymap_key_get_syms_by_level(lookup, KEY_A + 8, 0, 0, &syms) == 1 && syms[0] == 'a',
                  "KDE portal app receives single-level base keys");
            xkb_keymap_unref(lookup);
        }
        portal.close(path);
    }
    free(original);
    xkb_keymap_unref(base);
    xkb_context_unref(context);
}

void testRevocation(Portal& portal) {
    // One session's revocation must leave another session operational.
    const auto otherPath = portal.create();
    EisClient other(portal.connectEis(otherPath));
    other.discover();
    const auto path = portal.create();
    EisClient client(portal.connectEis(path));
    client.discover();
    client.key(KEY_LEFTCTRL, true);
    client.button(BTN_LEFT, true);
    waitFor([&] { return keys.contains(KEY_LEFTCTRL) && buttons.contains(BTN_LEFT); }, [&] { client.pump(); }, "held input");
    portal.close(path);
    check(keys.isEmpty() && buttons.isEmpty(), "Close releases held EIS input before reply");
    const auto before = inputEvents;
    client.key(KEY_A, true);
    waitFor([&] { return client.disconnected; }, [&] { client.pump(); }, "old EIS fd disconnected");
    check(inputEvents == before && keys.isEmpty() && buttons.isEmpty(), "closed fd cannot inject");
    other.key(KEY_B, true);
    waitFor([&] { return keys.contains(KEY_B); }, [&] { other.pump(); }, "other session remains usable");
    portal.close(otherPath);
    check(keys.isEmpty(), "other session cleanup");

    const auto pendingPath = portal.create();
    const int pendingFd = portal.connectEis(pendingPath);
    portal.close(pendingPath);
    pollfd pending{pendingFd, POLLIN, 0};
    check(poll(&pending, 1, 1000) == 1 && (pending.revents & POLLHUP), "pending handshake fd closed");
    ::close(pendingFd);
}

void testNotifyOwnership(Portal& portal) {
    const auto notify = [&](const QString& path, const QString& method, uint32_t code, bool pressed) {
        check(portal.notify(path, method, {int(code), uint32_t(pressed)}).type() != QDBusMessage::ErrorMessage,
              "Notify input accepted");
    };
    const QString key = QStringLiteral("NotifyKeyboardKeycode");
    const QString button = QStringLiteral("NotifyPointerButton");
    const auto a = portal.create(), b = portal.create();
    notify(a, key, KEY_A, true);
    notify(a, button, BTN_LEFT, true);
    const auto before = inputEvents;
    notify(b, key, KEY_A, false);
    notify(b, button, BTN_LEFT, false);
    check(inputEvents == before && keys.contains(KEY_A) && buttons.contains(BTN_LEFT), "unmatched release cannot affect another session");
    notify(b, key, KEY_A, true);
    notify(b, button, BTN_LEFT, true);
    check(inputEvents == before, "shared Notify presses aggregate");
    portal.close(a);
    check(keys.contains(KEY_A) && buttons.contains(BTN_LEFT), "Close preserves other Notify holders");
    portal.close(b);
    check(keys.isEmpty() && buttons.isEmpty(), "last Notify Close releases held input");

    for (bool closeEisFirst : {false, true}) {
        const auto notifyPath = portal.create(), eisPath = portal.create();
        EisClient client(portal.connectEis(eisPath));
        client.discover();
        notify(notifyPath, key, KEY_LEFTCTRL, true);
        notify(notifyPath, button, BTN_LEFT, true);
        client.key(KEY_LEFTCTRL, true);
        client.key(KEY_B, true); // A distinct key is a barrier for the shared press.
        client.button(BTN_LEFT, true);
        waitFor([&] { return keys.contains(KEY_B); }, [&] { client.pump(); }, "mixed ownership input");
        // A D-Bus round trip allows all already-readable EIS frames to drain.
        check(portal.call(desktop, "org.freedesktop.DBus.Peer", "Ping").type() != QDBusMessage::ErrorMessage, "input barrier");
        portal.close(closeEisFirst ? eisPath : notifyPath);
        check(keys.contains(KEY_LEFTCTRL) && buttons.contains(BTN_LEFT), "mixed Close preserves remaining owner");
        portal.close(closeEisFirst ? notifyPath : eisPath);
        check(keys.isEmpty() && buttons.isEmpty(), "mixed Close releases last owner");
    }

    const auto symbolPath = portal.create(), rawPath = portal.create();
    notify(rawPath, key, KEY_LEFTSHIFT, true);
    notify(rawPath, key, KEY_A, true);
    notify(symbolPath, "NotifyKeyboardKeysym", 'A', true);
    notify(symbolPath, "NotifyKeyboardKeysym", 'A', true);
    portal.close(symbolPath);
    check(keys.contains(KEY_LEFTSHIFT) && keys.contains(KEY_A), "keysym cleanup preserves raw keycode holders");
    portal.close(rawPath);
    check(keys.isEmpty(), "raw and keysym ownership cleanup");
    const auto shifted = portal.create();
    notify(shifted, "NotifyKeyboardKeysym", 'B', true);
    check(keys.contains(KEY_B) && keys.contains(KEY_LEFTSHIFT), "keysym owns physical key and synthetic shift");
    portal.close(shifted);
    check(keys.isEmpty(), "Notify Close releases synthesized Shift");
}

void testRateLimitReleases(Portal& portal) {
    const auto path = portal.create();
    const auto notify = [&](const QString& method, int code, uint32_t state) {
        return portal.notify(path, method, {code, state});
    };
    const auto accepted = [](const QDBusMessage& reply) {
        check(reply.type() != QDBusMessage::ErrorMessage, "release must bypass saturated rate limit");
    };
    accepted(notify("NotifyKeyboardKeycode", KEY_LEFTCTRL, 1));
    accepted(notify("NotifyKeyboardKeysym", 'A', 1));
    accepted(notify("NotifyPointerButton", BTN_LEFT, 1));
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < hkcf::security::kMaxNotifyEventsPerWindow; ++i) {
        auto message = QDBusMessage::createMethodCall(service, desktop, remote, "NotifyPointerMotion");
        message.setArguments({QVariant::fromValue(QDBusObjectPath(path)), QVariantMap{}, 0.0, 0.0});
        portal.frontend.asyncCall(message, 3000);
    }
    const auto limited = portal.notify(path, "NotifyPointerMotion", {0.0, 0.0});
    check(limited.errorName() == "org.freedesktop.DBus.Error.LimitsExceeded", "motion saturates rate limit");
    accepted(notify("NotifyKeyboardKeycode", KEY_LEFTCTRL, 0));
    accepted(notify("NotifyKeyboardKeysym", 'A', 0));
    accepted(notify("NotifyPointerButton", BTN_LEFT, 0));
    check(keys.isEmpty() && buttons.isEmpty(), "all releases clear held state while rate limited");
    check(notify("NotifyPointerButton", -1, 0).type() == QDBusMessage::ErrorMessage, "release bypass still validates input");
    check(portal.notify(path, "NotifyPointerMotion", {0.0, 0.0}).errorName() == "org.freedesktop.DBus.Error.LimitsExceeded",
          "releases do not reopen motion budget");
    check(timer.elapsed() < hkcf::security::kNotifyRateWindowMs, "saturation test stays inside rate window");
    portal.close(path);
}

void testDeviceGrants(Portal& portal) {
    for (uint32_t types : {1u, 2u}) {
        const auto path = portal.create(types);
        EisClient client(portal.connectEis(path));
        client.discover(types);
        check(client.capabilities == types, "EIS only announces granted capabilities");
        check(client.textCapability == bool(types & 1), "text seat capability follows keyboard grant");
        check(bool(client.keyboard) == bool(types & 1) && bool(client.pointer) == bool(types & 2),
              "ungranted devices are not created even when requested");
        const auto before = inputEvents;
        if (types == 1) {
            client.key(KEY_A, true);
            waitFor([&] { return keys.contains(KEY_A); }, [&] { client.pump(); }, "granted keyboard input");
            check(buttons.isEmpty() && inputEvents == before + 1, "keyboard-only session cannot inject pointer input");
        } else {
            client.button(BTN_LEFT, true);
            waitFor([&] { return buttons.contains(BTN_LEFT); }, [&] { client.pump(); }, "granted pointer input");
            check(keys.isEmpty() && inputEvents == before + 1, "pointer-only session cannot inject keyboard input");
        }
        const auto select = portal.call(desktop, remote, "SelectDevices", {
            QVariant::fromValue(QDBusObjectPath(desktop + "/request/1_999/change")),
            QVariant::fromValue(QDBusObjectPath(path)), QString("org.kde.kdeconnect"), QVariantMap{{"types", 3u}}});
        check(select.type() == QDBusMessage::ErrorMessage, "cannot change grants after Start");
        portal.close(path);
        check(keys.isEmpty() && buttons.isEmpty(), "restricted session cleanup");
    }
}

void testOwnerLoss(Portal& portal) {
    const auto path = portal.create();
    EisClient client(portal.connectEis(path));
    client.discover();
    check(portal.notify(path, "NotifyKeyboardKeysym", {int('C'), 1u}).type() != QDBusMessage::ErrorMessage, "Notify keysym before owner loss");
    check(portal.notify(path, "NotifyPointerButton", {int(BTN_RIGHT), 1u}).type() != QDBusMessage::ErrorMessage, "Notify button before owner loss");
    client.key(KEY_LEFTSHIFT, true);
    waitFor([&] { return keys.contains(KEY_LEFTSHIFT); }, [&] { client.pump(); }, "owner holds shift");
    portal.owner = QDBusConnection("disconnected");
    QDBusConnection::disconnectFromBus("owner");
    waitFor([&] { return client.disconnected; }, [&] { client.pump(); }, "originating owner disconnect revokes EIS");
    check(keys.isEmpty() && buttons.isEmpty(), "owner loss releases EIS and Notify input");
    portal.owner = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "owner2");

    const auto frontendPath = portal.create();
    EisClient frontendClient(portal.connectEis(frontendPath));
    frontendClient.discover();
    check(portal.notify(frontendPath, "NotifyKeyboardKeycode", {int(KEY_LEFTALT), 1u}).type() != QDBusMessage::ErrorMessage, "Notify key before frontend loss");
    frontendClient.button(BTN_LEFT, true);
    waitFor([&] { return buttons.contains(BTN_LEFT); }, [&] { frontendClient.pump(); }, "frontend holds button");
    check(portal.frontend.unregisterService("org.freedesktop.portal.Desktop"), "frontend loses service");
    waitFor([&] { return frontendClient.disconnected; }, [&] { frontendClient.pump(); }, "frontend owner loss revokes EIS");
    check(buttons.isEmpty() && keys.isEmpty(), "frontend loss releases EIS and Notify input");
    check(portal.frontend.registerService("org.freedesktop.portal.Desktop"), "restore private frontend");
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        check(qEnvironmentVariableIsSet("DBUS_SESSION_BUS_ADDRESS"), "run on a private dbus-run-session bus");
        Portal portal;
        testKeymapSelection(portal);
        testRevocation(portal);
        testNotifyOwnership(portal);
        testRateLimitReleases(portal);
        testDeviceGrants(portal);
        testOwnerLoss(portal);
        std::cout << "Portal session EIS lifetime tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
