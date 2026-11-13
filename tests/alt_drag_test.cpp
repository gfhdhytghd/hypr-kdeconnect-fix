// Exercise production libei -> EIS -> InputState -> WaylandInput on private sockets.
#include "eis_input_bridge.hpp"
#include "wayland_test_harness.hpp"
#include <libei.h>
#include <memory>
#include <string_view>
#include <utility>

bool failNextFlush = false;
int injectedFailures = 0;
extern "C" int __real_wl_display_flush(wl_display*);
extern "C" int __wrap_wl_display_flush(wl_display* display) {
    if (std::exchange(failNextFlush, false)) {
        ++injectedFailures;
        errno = EAGAIN;
        return -1;
    }
    return __real_wl_display_flush(display);
}

namespace {
template<typename Condition, typename Pump>
void waitFor(Condition condition, Pump pump) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 3000) {
        pump();
        QThread::msleep(1);
    }
    check(condition(), "private input barrier");
}

struct PrivateWayland {
    wl_display* server = wl_display_create();
    int control[2]{};
    wl_event_source* stopSource;
    std::thread thread;
    PrivateWayland() {
        int sockets[2];
        check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0, "Wayland socketpair");
        check(pipe(control) == 0, "server control pipe");
        wl_global_create(server, &wl_seat_interface, 9, nullptr, bindSeat);
        wl_global_create(server, &zwp_virtual_keyboard_manager_v1_interface, 1, nullptr, bindKeyboard);
        wl_global_create(server, &zwlr_virtual_pointer_manager_v1_interface, 2, nullptr, bindPointer);
        check(wl_client_create(server, sockets[0]), "private Wayland client");
        stopSource = wl_event_loop_add_fd(wl_display_get_event_loop(server), control[0], WL_EVENT_READABLE, stopServer, server);
        setenv("WAYLAND_SOCKET", std::to_string(sockets[1]).c_str(), 1);
        thread = std::thread([&] { wl_display_run(server); });
    }
    ~PrivateWayland() {
        const auto ignored = write(control[1], "q", 1);
        (void)ignored;
        thread.join();
        wl_event_source_remove(stopSource);
        wl_display_destroy_clients(server);
        wl_display_destroy(server);
        if (capture.state) xkb_state_unref(capture.state);
        close(control[0]);
        close(control[1]);
    }
};

struct Sender {
    hkcf::EisInputBridge& bridge;
    QString id;
    ei* client = ei_new_sender(nullptr);
    ei_device* keyboard = nullptr;
    ei_device* pointer = nullptr;
    bool pong = false;
    bool disconnected = false;
    std::string keymapText;
    Sender(hkcf::EisInputBridge& bridge, const QString& id, bool lookup) : bridge(bridge), id(id) {
        auto fd = bridge.addClient(id, 3, nullptr, lookup);
        check(fd.has_value(), "EIS session fd");
        check(ei_setup_backend_fd(client, *fd) == 0, "sender backend");
        waitFor([&] { return keyboard && pointer; }, [&] { pump(); });
    }
    ~Sender() {
        bridge.removeClient(id);
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
            case EI_EVENT_SEAT_ADDED:
                ei_seat_bind_capabilities(ei_event_get_seat(event), EI_DEVICE_CAP_KEYBOARD,
                    EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_BUTTON, nullptr);
                break;
            case EI_EVENT_DEVICE_ADDED:
                if (ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD)) {
                    auto* map = ei_device_keyboard_get_keymap(device);
                    check(map && ei_keymap_get_type(map) == EI_KEYMAP_TYPE_XKB, "sender XKB keymap");
                    keymapText.resize(ei_keymap_get_size(map));
                    check(pread(ei_keymap_get_fd(map), keymapText.data(), keymapText.size(), 0) ==
                          static_cast<ssize_t>(keymapText.size()), "read sender keymap");
                }
                break;
            case EI_EVENT_DEVICE_RESUMED:
                if (ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD) && !keyboard)
                    keyboard = ei_device_ref(device);
                if (ei_device_has_capability(device, EI_DEVICE_CAP_POINTER) && !pointer)
                    pointer = ei_device_ref(device);
                ei_device_start_emulating(device, 1);
                break;
            case EI_EVENT_PONG: pong = true; break;
            case EI_EVENT_DISCONNECT: disconnected = true; break;
            default: break;
            }
            ei_event_unref(event);
        }
    }
    void sync() {
        pong = false;
        auto* ping = ei_new_ping(client);
        ei_ping(ping);
        waitFor([&] { return pong; }, [&] { pump(); });
        ei_ping_unref(ping);
        check(!disconnected, "sender remains connected");
    }
    void key(uint32_t code, bool down) {
        ei_device_keyboard_key(keyboard, code, down);
        ei_device_frame(keyboard, ei_now(client));
    }
    void button(bool down) {
        ei_device_button_button(pointer, BTN_LEFT, down);
        ei_device_frame(pointer, ei_now(client));
    }
    void motion() {
        ei_device_pointer_motion(pointer, 80, 40);
        ei_device_frame(pointer, ei_now(client));
    }
};

void run(hkcf::WaylandInput& input, bool lookup, std::string_view scenario, bool nested) {
    hkcf::InputState inputs(input);
    hkcf::EisInputBridge bridge(input, inputs);
    Sender sender(bridge, lookup ? "lookup" : "layout", lookup);
    auto* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    auto* map = xkb_keymap_new_from_string(context, sender.keymapText.c_str(),
        XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    check(map, "advertised map compiles");
    const xkb_keysym_t* syms;
    const int nsyms = xkb_keymap_key_get_syms_by_level(map, KEY_LEFTALT + 8, 0, 0, &syms);
    check(lookup ? (nsyms == 0 || syms[0] == XKB_KEY_NoSymbol) : (nsyms == 1 && syms[0] == XKB_KEY_Alt_L),
          "lookup reserves raw Alt without a symbol; real layout advertises Alt_L");
    xkb_keymap_unref(map);
    xkb_context_unref(context);

    size_t start = 0;
    if (!nested) {
        std::lock_guard lock(capture.mutex);
        start = capture.pointerSamples.size();
    }
    if (scenario == "press-backpressure") failNextFlush = true;
    sender.key(KEY_LEFTALT, true);
    if (scenario.ends_with("backpressure")) sender.sync();
    if (scenario == "release-backpressure" || scenario == "scoped-packet") {
        if (scenario == "scoped-packet") {
            // KDE Connect's alt+key packet releases Alt before singlehold/motion.
            sender.key(KEY_A, true);
            sender.key(KEY_A, false);
        }
        if (scenario == "release-backpressure") failNextFlush = true;
        sender.key(KEY_LEFTALT, false);
        if (scenario == "release-backpressure") sender.sync();
    }
    // The normal and scoped-packet cases queue keyboard and pointer frames in
    // one batch, with no ping or compositor roundtrip between the devices.
    sender.button(true);
    sender.motion();
    sender.button(false);
    sender.key(KEY_LEFTALT, false);
    sender.sync();
    check(!input.isKeycodePressed(KEY_LEFTALT), "logical Alt released");
    check(injectedFailures == (scenario.ends_with("backpressure") ? (lookup ? 2 : 1) : 0), "requested flush fault occurred");
    if (!nested) {
        waitFor([&] {
            std::lock_guard lock(capture.mutex);
            return capture.pointerSamples.size() >= start + 3;
        }, [] {});
        // A final marker after Alt-up orders all preceding requests.
        check(input.pointerMotion(0, 0), "final Wayland marker");
        waitFor([&] {
            std::lock_guard lock(capture.mutex);
            return capture.pointerSamples.size() == start + 4;
        }, [] {});
        std::lock_guard lock(capture.mutex);
        check(capture.pointerSamples.size() == start + 4, "one button/motion/release and marker");
        const bool expectedAlt = scenario != "release-backpressure" && scenario != "scoped-packet";
        for (size_t i = 0; i < 3; ++i) {
            const auto& sample = capture.pointerSamples[start + i];
            check(sample.motion == (i == 1), "button/motion/release wire order");
            if (!sample.motion)
                check(sample.code == BTN_LEFT && sample.state == (i == 0), "left button state");
            check(sample.alt == expectedAlt, expectedAlt ? "Alt must reach pointer button/motion before release" : "released Alt must not reach pointer input");
        }
        check(!capture.mapError && !altActive(), "compositor XKB Alt released after drag");
    }
    std::cout << (lookup ? "lookup" : "real-layout") << " " << scenario << " passed\n";
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    setenv("XKB_DEFAULT_LAYOUT", "us", 1);
    setenv("XKB_DEFAULT_VARIANT", "", 1);
    setenv("XKB_DEFAULT_OPTIONS", "", 1);
    const std::string_view scenario = argc > 1 ? argv[1] : "normal";
    const bool nested = argc > 2 && std::string_view(argv[2]) == "--nested";
    try {
        std::unique_ptr<PrivateWayland> server;
        if (!nested) server = std::make_unique<PrivateWayland>();
        hkcf::WaylandInput input;
        check(input.ensureReady(), "Wayland input ready");
        if (nested)
            run(input, argc > 3 && std::string_view(argv[3]) == "lookup", scenario, true);
        else {
            run(input, false, scenario, false);
            run(input, true, scenario, false);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
