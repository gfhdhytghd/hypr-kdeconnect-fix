// Control test without the production bridge: locate duplicate filtering in libeis.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QVector>
#include <libei.h>
#include <libeis.h>
#include <linux/input-event-codes.h>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

struct DirectConnection {
    eis* server = eis_new(nullptr);
    ei* client = ei_new_sender(nullptr);
    eis_seat* seat = nullptr;
    eis_device* serverKeyboard = nullptr;
    ei_device* keyboard = nullptr;
    bool pong = false;
    QVector<QPair<uint32_t, bool>> events;

    DirectConnection() {
        check(server && client, "contexts");
        check(eis_setup_backend_fd(server) == 0, "server fd backend");
        const int fd = eis_backend_fd_add_client(server);
        check(fd >= 0 && ei_setup_backend_fd(client, fd) == 0, "client fd backend");
    }
    ~DirectConnection() {
        if (keyboard) ei_device_unref(keyboard);
        ei_unref(client);
        if (serverKeyboard) eis_device_unref(serverKeyboard);
        if (seat) eis_seat_unref(seat);
        eis_unref(server);
    }
    void pump() {
        eis_dispatch(server);
        while (auto* event = eis_get_event(server)) {
            switch (eis_event_get_type(event)) {
            case EIS_EVENT_CLIENT_CONNECT: {
                auto* sender = eis_event_get_client(event);
                check(eis_client_get_name(sender) == nullptr, "unconfigured libei client has no name (KDE Connect <=26.08)");
                eis_client_connect(sender);
                seat = eis_client_new_seat(sender, "repeat-control");
                eis_seat_configure_capability(seat, EIS_DEVICE_CAP_KEYBOARD);
                eis_seat_add(seat);
                break;
            }
            case EIS_EVENT_SEAT_BIND:
                if (!serverKeyboard) {
                    serverKeyboard = eis_seat_new_device(seat);
                    eis_device_configure_type(serverKeyboard, EIS_DEVICE_TYPE_VIRTUAL);
                    eis_device_configure_name(serverKeyboard, "control keyboard");
                    eis_device_configure_capability(serverKeyboard, EIS_DEVICE_CAP_KEYBOARD);
                    eis_device_add(serverKeyboard);
                    eis_device_resume(serverKeyboard);
                }
                break;
            case EIS_EVENT_KEYBOARD_KEY:
                events.append({eis_event_keyboard_get_key(event), eis_event_keyboard_get_key_is_press(event)});
                break;
            default: break;
            }
            // Unref of EIS_EVENT_SYNC sends the corresponding client pong.
            eis_event_unref(event);
        }
        ei_dispatch(client);
        while (auto* event = ei_get_event(client)) {
            switch (ei_event_get_type(event)) {
            case EI_EVENT_SEAT_ADDED:
                ei_seat_bind_capabilities(ei_event_get_seat(event), EI_DEVICE_CAP_KEYBOARD, nullptr);
                break;
            case EI_EVENT_DEVICE_RESUMED:
                if (!keyboard)
                    keyboard = ei_device_ref(ei_event_get_device(event));
                ei_device_start_emulating(keyboard, 1);
                break;
            case EI_EVENT_PONG: pong = true; break;
            case EI_EVENT_DISCONNECT: throw std::runtime_error("unexpected disconnect");
            default: break;
            }
            ei_event_unref(event);
        }
    }
    template<typename Condition>
    void waitFor(Condition condition) {
        QElapsedTimer timer;
        timer.start();
        while (!condition() && timer.elapsed() < 3000) {
            pump();
            QThread::msleep(1);
        }
        check(condition(), "direct libei/libeis round trip");
    }
    void sync() {
        pong = false;
        auto* ping = ei_new_ping(client);
        ei_ping(ping);
        waitFor([&] { return pong; });
        ei_ping_unref(ping);
    }
    void key(bool pressed) {
        ei_device_keyboard_key(keyboard, KEY_A, pressed);
        // One key request per frame, respecting the protocol contract.
        ei_device_frame(keyboard, ei_now(client));
    }
};
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        DirectConnection direct;
        direct.waitFor([&] { return direct.keyboard != nullptr; });
        direct.key(true);
        direct.key(true);
        direct.key(true);
        direct.key(false);
        direct.sync();
        check(direct.events == QVector<QPair<uint32_t, bool>>{{KEY_A, true}, {KEY_A, false}},
              "libeis itself filters repeated press before any bridge code");
        direct.events.clear();
        for (int i = 0; i < 3; ++i) {
            direct.key(true);
            direct.key(false);
        }
        direct.sync();
        check(direct.events.size() == 6, "balanced repeat pairs are delivered");
        for (int i = 0; i < 6; ++i)
            check(direct.events[i] == qMakePair(uint32_t(KEY_A), i % 2 == 0), "balanced pair ordering");
        std::cout << "Direct libei/libeis: down/down/down/up -> down/up; balanced pairs preserved\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
