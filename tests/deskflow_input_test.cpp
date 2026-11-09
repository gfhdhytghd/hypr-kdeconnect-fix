#include "eis_input_bridge.hpp"
#include "input_state.hpp"
#include "security_policy.hpp"
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
QVector<QPair<uint32_t, bool>> keyEvents;
QSet<uint32_t> buttons;
std::int64_t resolverTimeMs = 0;
bool failPress = false;
uint32_t rejectedPressCode = 0;
double absoluteX = -1, absoluteY = -1;
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

// Link the real EIS bridge to a recording Wayland sink. Never connects to a
// compositor or injects input into the user's desktop.
namespace hkcf {
WaylandInput::WaylandInput() : m_keyResolver([] { return resolverTimeMs; }) {}
WaylandInput::~WaylandInput() = default;
QRect WaylandInput::logicalBounds() const { return {0, 0, 1920, 1080}; }
bool WaylandInput::pointerMotion(double, double) { return true; }
bool WaylandInput::pointerMotionAbsolute(double x, double y) { absoluteX = x; absoluteY = y; return true; }
bool WaylandInput::pointerButton(uint32_t button, bool pressed) {
    if (pressed) buttons.insert(button); else buttons.remove(button);
    return true;
}
bool WaylandInput::pointerAxisDiscrete(uint32_t axis, int steps) { scrolls.append({axis, steps}); return true; }
bool WaylandInput::keyboardKeycode(uint32_t key, bool pressed, bool) {
    if (pressed && (failPress || key == rejectedPressCode))
        return false;
    keyEvents.append({key, pressed});
    if (pressed) {
        keys.insert(key);
        m_pressedKeycodes.insert(key);
    } else {
        keys.remove(key);
        m_pressedKeycodes.erase(key);
    }
    return true;
}
}

void testInputState() {
    qputenv("XKB_DEFAULT_LAYOUT", "pl");
    qputenv("XKB_DEFAULT_VARIANT", "");
    qputenv("XKB_DEFAULT_OPTIONS", "");
    hkcf::WaylandInput input;
    hkcf::InputState inputs(input);
    int rawOwner, first, second, third;
    const auto ogonek = input.resolveKeysym(XKB_KEY_aogonek);
    check(ogonek && ogonek->modifierKeycodes.size() == 1, "Polish level-3 modifier");
    const auto level3 = ogonek->modifierKeycodes.front();
    check(inputs.keyboardKeycode(&rawOwner, level3, true), "raw AltGr hold");
    check(inputs.keyboardKeysym(&first, XKB_KEY_aogonek, true), "first AltGr symbol");
    check(inputs.keyboardKeysym(&first, XKB_KEY_aogonek, true), "repeated AltGr symbol");
    check(inputs.keyboardKeysym(&second, XKB_KEY_eogonek, true), "second AltGr symbol");
    check(inputs.keyboardKeysym(&third, XKB_KEY_Aogonek, true), "Shift plus AltGr symbol");
    inputs.releaseOwner(&first);
    check(keys.contains(KEY_A) && keys.contains(level3), "overlapping upper symbol retains physical key and modifier");
    inputs.releaseOwner(&third);
    check(!keys.contains(KEY_A) && !keys.contains(KEY_LEFTSHIFT), "upper symbol releases its key and Shift");
    inputs.releaseOwner(&rawOwner);
    check(keys.contains(level3), "another owner retains AltGr");
    inputs.releaseOwner(&second);
    check(keys.isEmpty(), "last owner releases AltGr");
    check(inputs.keyboardKeysym(&first, XKB_KEY_Greek_alpha, true), "hold dynamic symbol");
    const auto held = input.resolveKeysym(XKB_KEY_Greek_alpha);
    check(held.has_value(), "dynamic symbol mapping");
    for (std::size_t i = 0; i < hkcf::KeyResolver::kMaxExtraKeysyms + 5; ++i) {
        resolverTimeMs += 1000;
        check(inputs.keyboardKeysym(&second, 0x01000400 + i, true), "cache pressure press");
        check(inputs.keyboardKeysym(&second, 0x01000400 + i, false), "cache pressure release");
    }
    check(input.resolveKeysym(XKB_KEY_Greek_alpha)->evdevKeycode == held->evdevKeycode && keys.contains(held->evdevKeycode),
          "held dynamic symbol survives eviction of released slots");
    inputs.releaseOwner(&first);
    check(keys.isEmpty(), "owner cleanup releases dynamic symbol");
    failPress = true;
    check(!inputs.keyboardKeycode(&first, KEY_LEFTSHIFT, true), "failed raw press");
    check(!inputs.keyboardKeysym(&first, XKB_KEY_Aogonek, true), "failed symbol modifier press");
    failPress = false;
    check(inputs.keyboardKeycode(&second, KEY_LEFTSHIFT, true) && keys.contains(KEY_LEFTSHIFT),
          "failed press does not suppress another owner's successful press");
    inputs.releaseOwner(&second);
    check(inputs.keyboardKeysym(&first, XKB_KEY_Aogonek, true) && keys.contains(KEY_LEFTSHIFT) && keys.contains(level3),
          "failed symbol press retries all modifiers");
    inputs.releaseOwner(&first);
    check(keys.isEmpty(), "retried symbol releases all physical holds");
    rejectedPressCode = KEY_A;
    check(!inputs.keyboardKeysym(&first, XKB_KEY_Aogonek, true) && keys.isEmpty(),
          "failed symbol key press releases modifiers already sent");
    rejectedPressCode = 0;
    check(inputs.keyboardKeysym(&first, XKB_KEY_aogonek, true), "held symbol before text");
    keyEvents.clear();
    check(inputs.keyboardText(XKB_KEY_aogonek), "one-shot text retriggers a held symbol");
    check(keyEvents == QVector<QPair<uint32_t, bool>>{{KEY_A, false}, {KEY_A, true}} &&
          keys.contains(KEY_A) && keys.contains(level3) && input.isKeycodePressed(KEY_A),
          "held text releases then presses the key, preserving original key/modifier holds");
    keyEvents.clear();
    check(inputs.keyboardKeysym(&first, XKB_KEY_aogonek, false), "original symbol release after text");
    check(keyEvents == QVector<QPair<uint32_t, bool>>{{KEY_A, false}, {level3, false}} && keys.isEmpty(),
          "original symbol release balances the retriggered key and modifier");
    check(inputs.keyboardKeycode(&rawOwner, KEY_A, true), "raw key held before modified text");
    keyEvents.clear();
    check(inputs.keyboardText(XKB_KEY_aogonek), "text applies AltGr to a held raw key");
    check(keyEvents == QVector<QPair<uint32_t, bool>>{{level3, true}, {KEY_A, false}, {KEY_A, true}, {level3, false}} &&
          keys == QSet<uint32_t>{KEY_A}, "text brackets the retrigger with its own modifier");
    check(inputs.keyboardKeycode(&rawOwner, KEY_A, false) && keys.isEmpty(), "raw release leaves no stuck keys");
}

void testEis() {
    hkcf::WaylandInput input;
    hkcf::InputState inputs(input);
    hkcf::EisInputBridge bridge(input, inputs);
    QString error;
    auto fd = bridge.addClient(QStringLiteral("first"), 3, &error);
    check(fd.has_value(), "create EIS client");
    auto* client = ei_new_sender(nullptr);
    ei_configure_name(client, "deskflow-input-test");
    check(ei_setup_backend_fd(client, *fd) == 0, "connect libei sender");
    ei_device* keyboard = nullptr;
    ei_device* text = nullptr;
    ei_device* pointer = nullptr;
    ei_device* absolute = nullptr;
    ei* secondClient = nullptr;
    ei_device* secondKeyboard = nullptr;
    bool disconnected = false;
    const auto pump = [&] {
        QCoreApplication::processEvents();
        for (auto* activeClient : {client, secondClient}) {
            if (!activeClient) continue;
            ei_dispatch(activeClient);
            while (auto* event = ei_get_event(activeClient)) {
                auto* device = ei_event_get_device(event);
                switch (ei_event_get_type(event)) {
                case EI_EVENT_SEAT_ADDED:
                    ei_seat_bind_capabilities(ei_event_get_seat(event), EI_DEVICE_CAP_KEYBOARD, EI_DEVICE_CAP_TEXT,
                        EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_POINTER_ABSOLUTE,
                        EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_SCROLL, nullptr);
                    break;
                case EI_EVENT_DEVICE_RESUMED:
                    if (activeClient == secondClient) {
                        if (ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD) && !secondKeyboard)
                            secondKeyboard = ei_device_ref(device);
                        ei_device_start_emulating(device, 1);
                        break;
                    }
                    if (ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD) && !keyboard)
                        keyboard = ei_device_ref(device);
                    if (ei_device_has_capability(device, EI_DEVICE_CAP_TEXT) && !text)
                        text = ei_device_ref(device);
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
        }
    };
    const auto waitFor = [&](auto condition, const char* message) {
        QElapsedTimer timer; timer.start();
        while (!condition() && timer.elapsed() < 3000) { pump(); QThread::msleep(1); }
        check(condition() && !disconnected, message);
    };
    waitFor([&] { return keyboard && text && pointer && absolute; }, "EIS device discovery");
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
    // A motion on the same connection is a barrier for all preceding events,
    // including transitions that must not produce an event in the sink.
    double barrierX = 500;
    const auto sync = [&] {
        ++barrierX;
        ei_device_pointer_motion_absolute(absolute, barrierX, 300);
        ei_device_frame(absolute, ei_now(client));
        waitFor([&] { return absoluteX == barrierX; }, "input barrier");
    };
    const auto aogonek = input.resolveKeysym(XKB_KEY_aogonek);
    const auto eogonek = input.resolveKeysym(XKB_KEY_eogonek);
    check(aogonek && eogonek, "text symbols resolve");
    keyEvents.clear();
    ei_device_text_utf8(text, "ąę");
    ei_device_frame(text, ei_now(client));
    sync();
    check(keyEvents == QVector<QPair<uint32_t, bool>>{{aogonek->evdevKeycode, true}, {aogonek->evdevKeycode, false},
                                                    {eogonek->evdevKeycode, true}, {eogonek->evdevKeycode, false}},
          "EIS UTF-8 decodes both Polish code points in order");
    ei_device_text_keysym(text, XKB_KEY_aogonek, true);
    ei_device_frame(text, ei_now(client));
    sync();
    check(keys.contains(aogonek->evdevKeycode), "EIS text keysym press");
    keyEvents.clear();
    ei_device_text_utf8(text, "ą");
    ei_device_frame(text, ei_now(client));
    sync();
    check(keys.contains(aogonek->evdevKeycode), "UTF-8 must preserve a held text keysym");
    check(keyEvents == QVector<QPair<uint32_t, bool>>{{aogonek->evdevKeycode, false}, {aogonek->evdevKeycode, true}},
          "UTF-8 releases then presses the key while the same TEXT_KEYSYM is held");
    keyEvents.clear();
    ei_device_text_keysym(text, XKB_KEY_aogonek, false);
    ei_device_frame(text, ei_now(client));
    sync();
    check(keyEvents == QVector<QPair<uint32_t, bool>>{{aogonek->evdevKeycode, false}} && keys.isEmpty(),
          "original TEXT_KEYSYM release leaves no stuck keys after UTF-8");
    ei_device_text_keysym(text, XKB_KEY_eogonek, true);
    ei_device_frame(text, ei_now(client));
    sync();
    ei_device_stop_emulating(text);
    sync();
    check(keys.isEmpty(), "text stop releases held keysym");
    ei_device_start_emulating(text, 2);

    // A small UTF-8 event must not trigger unbounded keymap compilation.
    resolverTimeMs += 1000;
    QString han;
    for (uint point = 0x4e00; point < 0x4e00 + 65; ++point)
        han += QChar(point);
    const auto utf8 = han.toUtf8();
    check(utf8.size() == 195, "65 Han characters use only 195 UTF-8 bytes");
    keyEvents.clear();
    ei_device_text_utf8(text, utf8.constData());
    ei_device_frame(text, ei_now(client));
    sync();
    check(keyEvents.size() == 2 * hkcf::security::kKeymapReplacementBurst && keys.isEmpty(),
          "EIS text drops over-budget symbols without leaving holds");
    keyEvents.clear();
    const auto mappedHan = QString(QChar(0x4e00)).toUtf8();
    ei_device_text_utf8(text, mappedHan.constData());
    ei_device_frame(text, ei_now(client));
    sync();
    check(keyEvents.size() == 2 && keys.isEmpty(), "mapped EIS text still works after budget exhaustion");

    // Compare with eis-repeat-test's direct libei/libeis control. Repeats use
    // separate frames; libeis filters duplicate states before the bridge sees them.
    keyEvents.clear();
    for (bool pressed : {true, true, true, false}) {
        ei_device_keyboard_key(keyboard, KEY_A, pressed);
        ei_device_frame(keyboard, ei_now(client));
    }
    sync();
    check(keyEvents == QVector<QPair<uint32_t, bool>>{{KEY_A, true}, {KEY_A, false}}, "repeat sequence matches bare libeis");
    keyEvents.clear();
    for (int i = 0; i < 3; ++i) {
        for (bool pressed : {true, false}) {
            ei_device_keyboard_key(keyboard, KEY_A, pressed);
            ei_device_frame(keyboard, ei_now(client));
        }
    }
    sync();
    check(keyEvents.size() == 6 && keys.isEmpty(), "balanced EIS repeat pairs are preserved");
    for (bool cancel : {false, true}) {
        for (bool stopX : {false, true}) {
            ei_device_scroll_discrete(pointer, 60, 60);
            ei_device_frame(pointer, ei_now(client));
            if (cancel)
                ei_device_scroll_cancel(pointer, stopX, !stopX);
            else
                ei_device_scroll_stop(pointer, stopX, !stopX);
            ei_device_frame(pointer, ei_now(client));
            sync();
            auto before = scrolls.size();
            // The other axis retains its half-notch.
            wheel(stopX ? 0 : 60, stopX ? 60 : 0, stopX ? 0 : 1, 1);
            check(scrolls.size() == before + 1, "preserve active axis remainder");
            before = scrolls.size();
            ei_device_scroll_discrete(pointer, stopX ? 60 : 0, stopX ? 0 : 60);
            ei_device_frame(pointer, ei_now(client));
            sync();
            check(scrolls.size() == before, "new gesture must not inherit stopped or cancelled remainder");
            wheel(stopX ? 60 : 0, stopX ? 0 : 60, stopX ? 1 : 0, 1);
        }
    }
    for (bool stop : {false, true}) {
        ei_device_button_button(pointer, BTN_LEFT, true);
        ei_device_frame(pointer, ei_now(client));
        ei_device_button_button(absolute, BTN_LEFT, true);
        ei_device_frame(absolute, ei_now(client));
        sync();
        check(buttons.contains(BTN_LEFT), "shared button press");
        if (stop)
            ei_device_stop_emulating(pointer);
        else {
            ei_device_button_button(pointer, BTN_LEFT, false);
            ei_device_frame(pointer, ei_now(client));
        }
        sync();
        check(buttons.contains(BTN_LEFT), "one device must not release another device's button");
        ei_device_button_button(absolute, BTN_LEFT, false);
        ei_device_frame(absolute, ei_now(client));
        sync();
        check(buttons.isEmpty(), "last holder releases button");
        if (stop) {
            ei_device_start_emulating(pointer, 2);
            // libei retains the sender's button state across emulation stops.
            ei_device_button_button(pointer, BTN_LEFT, false);
            ei_device_frame(pointer, ei_now(client));
            sync();
        }
    }
    const auto secondFd = bridge.addClient(QStringLiteral("second"), 3, &error);
    check(secondFd.has_value(), "create second client");
    secondClient = ei_new_sender(nullptr);
    check(ei_setup_backend_fd(secondClient, *secondFd) == 0, "connect second client");
    waitFor([&] { return secondKeyboard != nullptr; }, "second keyboard discovery");
    ei_device_keyboard_key(secondKeyboard, KEY_LEFTCTRL, true);
    ei_device_keyboard_key(secondKeyboard, KEY_B, true);
    ei_device_frame(secondKeyboard, ei_now(secondClient));
    waitFor([&] { return keys.contains(KEY_B); }, "second client holds control");
    ei_device_keyboard_key(keyboard, KEY_LEFTCTRL, true);
    ei_device_frame(keyboard, ei_now(client));
    sync();
    ei_device_unref(secondKeyboard);
    secondKeyboard = nullptr;
    secondClient = ei_unref(secondClient);
    waitFor([&] { return !keys.contains(KEY_B); }, "second client disconnect processed");
    check(keys.contains(KEY_LEFTCTRL), "disconnect must preserve another client's held modifier");
    ei_device_keyboard_key(keyboard, KEY_LEFTCTRL, false);
    ei_device_frame(keyboard, ei_now(client));
    sync();
    check(keys.isEmpty(), "last holder releases modifier");
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
    // The budget test above filled every dynamic slot; aogonek is remapped to a
    // new slot once the replacement budget has refilled.
    resolverTimeMs += 1000;
    const auto keysBeforeText = keys.size();
    ei_device_text_keysym(text, XKB_KEY_aogonek, true);
    ei_device_frame(text, ei_now(client));
    sync();
    check(keys.size() == keysBeforeText + 1, "text hold before disconnect");
    ei_device_unref(text);
    ei_device_unref(keyboard); ei_device_unref(pointer); ei_device_unref(absolute);
    client = ei_unref(client);
    waitFor([&] { return keys.isEmpty(); }, "disconnect releases held key");
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        if (app.arguments().contains(QStringLiteral("--state-only"))) {
            testInputState();
            std::cout << "InputState AltGr ownership and dynamic keymap cache passed\n";
            return 0;
        }
        qputenv("XKB_DEFAULT_LAYOUT", "us");
        qputenv("XKB_DEFAULT_VARIANT", "");
        qputenv("XKB_DEFAULT_OPTIONS", "");
        testEis();
        std::cout << "Deskflow EIS discovery, absolute motion, scroll, keys and cleanup passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
