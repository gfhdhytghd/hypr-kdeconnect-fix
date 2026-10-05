#include "eis_input_bridge.hpp"
#include "wayland_input.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QVector>
#include <QSet>
#include <libei.h>
#include <linux/input-event-codes.h>
#include <stdexcept>
#include <iostream>

namespace {
QVector<QPair<uint32_t, int>> scrolls;
QSet<uint32_t> keys;
QSet<uint32_t> buttons;
double absoluteX = -1, absoluteY = -1;
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

// Link the real EIS bridge to a recording Wayland sink. Never connects to a
// compositor or injects input into the user's desktop.
namespace hkcf {
WaylandInput::WaylandInput() = default;
WaylandInput::~WaylandInput() = default;
QRect WaylandInput::logicalBounds() const { return {0, 0, 1920, 1080}; }
bool WaylandInput::pointerMotion(double, double) { return true; }
bool WaylandInput::pointerMotionAbsolute(double x, double y) { absoluteX = x; absoluteY = y; return true; }
bool WaylandInput::pointerButton(uint32_t button, bool pressed) {
    if (pressed) buttons.insert(button); else buttons.remove(button);
    return true;
}
bool WaylandInput::pointerAxisDiscrete(uint32_t axis, int steps) { scrolls.append({axis, steps}); return true; }
bool WaylandInput::keyboardKeycode(uint32_t key, bool pressed) {
    if (pressed) keys.insert(key); else keys.remove(key);
    return true;
}
}

void testEis() {
    hkcf::WaylandInput input;
    hkcf::EisInputBridge bridge(input);
    QString error;
    auto fd = bridge.addClient(&error);
    check(fd.has_value(), "create EIS client");
    auto* client = ei_new_sender(nullptr);
    ei_configure_name(client, "deskflow-input-test");
    check(ei_setup_backend_fd(client, *fd) == 0, "connect libei sender");
    ei_device* keyboard = nullptr;
    ei_device* pointer = nullptr;
    ei_device* absolute = nullptr;
    bool disconnected = false;
    const auto pump = [&] {
        QCoreApplication::processEvents();
        if (!client) return;
        ei_dispatch(client);
        while (auto* event = ei_get_event(client)) {
            auto* device = ei_event_get_device(event);
            switch (ei_event_get_type(event)) {
            case EI_EVENT_SEAT_ADDED:
                ei_seat_bind_capabilities(ei_event_get_seat(event), EI_DEVICE_CAP_KEYBOARD,
                    EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_POINTER_ABSOLUTE,
                    EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_SCROLL, nullptr);
                break;
            case EI_EVENT_DEVICE_RESUMED:
                if (ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD) && !keyboard)
                    keyboard = ei_device_ref(device);
                if (ei_device_has_capability(device, EI_DEVICE_CAP_POINTER) && !pointer)
                    pointer = ei_device_ref(device);
                if (ei_device_has_capability(device, EI_DEVICE_CAP_POINTER_ABSOLUTE) && !absolute)
                    absolute = ei_device_ref(device);
                ei_device_start_emulating(device, 1);
                break;
            case EI_EVENT_DISCONNECT: disconnected = true; break;
            default: break;
            }
            ei_event_unref(event);
        }
    };
    const auto waitFor = [&](auto condition, const char* message) {
        QElapsedTimer timer; timer.start();
        while (!condition() && timer.elapsed() < 3000) { pump(); QThread::msleep(1); }
        check(condition() && !disconnected, message);
    };
    waitFor([&] { return keyboard && pointer && absolute; }, "EIS device discovery");
    check(ei_device_has_capability(absolute, EI_DEVICE_CAP_BUTTON), "absolute pointer buttons");
    check(ei_device_has_capability(absolute, EI_DEVICE_CAP_SCROLL), "absolute pointer scroll");
    auto* region = ei_device_get_region(absolute, 0);
    check(region && ei_region_get_physical_scale(region) == 1.0, "absolute physical scale");
    ei_device_pointer_motion_absolute(absolute, 400, 300);
    ei_device_frame(absolute, ei_now(client));
    waitFor([&] { return absoluteX == 400 && absoluteY == 300; }, "absolute motion forwarding");
    const auto wheel = [&](int x, int y, uint32_t axis, int steps) {
        const auto before = scrolls.size();
        ei_device_scroll_discrete(pointer, x, y);
        ei_device_frame(pointer, ei_now(client));
        waitFor([&] { return scrolls.size() > before; }, "scroll forwarding");
        check(scrolls.size() == before + 1 && scrolls.last() == qMakePair(axis, steps), "v120 scroll scale and direction");
    };
    wheel(0, 120, 0, 1);
    wheel(0, -120, 0, -1);
    wheel(240, 0, 1, 2);
    wheel(-120, 0, 1, -1);
    ei_device_scroll_discrete(pointer, 0, 60);
    ei_device_frame(pointer, ei_now(client));
    wheel(0, 60, 0, 1);
    ei_device_scroll_discrete(pointer, 0, 90);
    ei_device_frame(pointer, ei_now(client));
    wheel(0, -120, 0, -1);
    ei_device_scroll_discrete(pointer, 0, -90);
    ei_device_frame(pointer, ei_now(client));
    wheel(0, 120, 0, 1);
    ei_device_keyboard_key(keyboard, KEY_LEFTSHIFT, true);
    ei_device_keyboard_key(keyboard, KEY_A, true);
    ei_device_frame(keyboard, ei_now(client));
    ei_device_button_button(pointer, BTN_LEFT, true);
    ei_device_frame(pointer, ei_now(client));
    waitFor([&] { return keys.contains(KEY_LEFTSHIFT) && keys.contains(KEY_A) && buttons.contains(BTN_LEFT); }, "keys and button forwarding");
    ei_device_stop_emulating(keyboard);
    ei_device_stop_emulating(pointer);
    waitFor([&] { return keys.isEmpty() && buttons.isEmpty(); }, "stop releases held input");
    ei_device_start_emulating(keyboard, 2);
    ei_device_keyboard_key(keyboard, KEY_LEFTCTRL, true);
    ei_device_frame(keyboard, ei_now(client));
    waitFor([&] { return keys.contains(KEY_LEFTCTRL); }, "second emulation");
    auto* seat = ei_seat_ref(ei_device_get_seat(keyboard));
    ei_device_close(keyboard);
    keyboard = ei_device_unref(keyboard);
    waitFor([&] { return keys.isEmpty(); }, "device close releases held key");
    ei_seat_request_device_with_capabilities(seat, EI_DEVICE_CAP_KEYBOARD, nullptr);
    waitFor([&] { return keyboard != nullptr; }, "libei 1.6 keyboard recreation");
    ei_device_keyboard_key(keyboard, KEY_LEFTALT, true);
    ei_device_frame(keyboard, ei_now(client));
    waitFor([&] { return keys.contains(KEY_LEFTALT); }, "recreated keyboard input");
    ei_seat_unref(seat);
    ei_device_unref(keyboard); ei_device_unref(pointer); ei_device_unref(absolute);
    client = ei_unref(client);
    waitFor([&] { return keys.isEmpty(); }, "disconnect releases held key");
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        testEis();
        std::cout << "Deskflow EIS discovery, absolute motion, scroll, keys and cleanup passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
