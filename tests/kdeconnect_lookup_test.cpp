// Production EIS -> InputState -> WaylandInput, using only private sockets.
#include "eis_input_bridge.hpp"
#include "security_policy.hpp"
#include "wayland_test_harness.hpp"
#include <libei.h>
#include <iomanip>
#include <QChar>
#include <QLoggingCategory>
#include <set>

namespace {
QString startupMessage;
int startupMessages = 0;
void startupLogger(QtMsgType type, const QMessageLogContext&, const QString& message) {
    if (type == QtInfoMsg && message.startsWith("EIS lookup keymap:")) {
        ++startupMessages;
        startupMessage = message;
    }
}

struct IsolatedWayland {
    wl_display* server = wl_display_create();
    int control[2]{};
    wl_event_source* stopSource = nullptr;
    std::thread thread;
    IsolatedWayland() {
        int sockets[2];
        check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0, "Wayland socketpair");
        check(pipe(control) == 0, "control pipe");
        wl_global_create(server, &wl_seat_interface, 9, nullptr, bindSeat);
        wl_global_create(server, &zwp_virtual_keyboard_manager_v1_interface, 1, nullptr, bindKeyboard);
        wl_global_create(server, &zwlr_virtual_pointer_manager_v1_interface, 2, nullptr, bindPointer);
        check(wl_client_create(server, sockets[0]), "private Wayland client");
        stopSource = wl_event_loop_add_fd(wl_display_get_event_loop(server), control[0],
                                          WL_EVENT_READABLE, stopServer, server);
        setenv("WAYLAND_SOCKET", std::to_string(sockets[1]).c_str(), 1);
        thread = std::thread([&] { wl_display_run(server); });
    }
    ~IsolatedWayland() {
        check(write(control[1], "q", 1) == 1, "stop server");
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
    ei_device* text = nullptr;
    xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_keymap* map = nullptr;
    xkb_state* state = nullptr;
    bool pong = false;
    bool disconnected = false;
    bool bindText;
    std::string keymapText;
    double compileMs = 0;

    Sender(hkcf::EisInputBridge& bridge, const QString& id, bool lookup,
           const char* name = nullptr, bool bindText = false)
        : bridge(bridge), id(id), bindText(bindText) {
        const auto fd = bridge.addClient(id, 1, nullptr, lookup);
        check(fd.has_value(), "EIS session fd");
        // KDE Connect 26.04/26.08 do not call ei_configure_name.
        if (name) ei_configure_name(client, name);
        check(ei_setup_backend_fd(client, *fd) == 0, "libei backend");
        waitFor([&] { return keyboard && (!bindText || text); });
        check(map && state, "received keyboard map");
    }
    ~Sender() {
        bridge.removeClient(id);
        if (keyboard) ei_device_unref(keyboard);
        if (text) ei_device_unref(text);
        ei_unref(client);
        if (state) xkb_state_unref(state);
        if (map) xkb_keymap_unref(map);
        xkb_context_unref(context);
    }
    void pump() {
        QCoreApplication::processEvents();
        ei_dispatch(client);
        while (auto* event = ei_get_event(client)) {
            auto* device = ei_event_get_device(event);
            switch (ei_event_get_type(event)) {
            case EI_EVENT_SEAT_ADDED:
                if (bindText)
                    ei_seat_bind_capabilities(ei_event_get_seat(event), EI_DEVICE_CAP_KEYBOARD, EI_DEVICE_CAP_TEXT, nullptr);
                else
                    ei_seat_bind_capabilities(ei_event_get_seat(event), EI_DEVICE_CAP_KEYBOARD,
                        EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_POINTER_ABSOLUTE, EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_SCROLL, nullptr);
                break;
            case EI_EVENT_DEVICE_ADDED:
                if (ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD) && !map) {
                    auto* keymap = ei_device_keyboard_get_keymap(device);
                    check(keymap && ei_keymap_get_type(keymap) == EI_KEYMAP_TYPE_XKB, "XKB device keymap");
                    keymapText.resize(ei_keymap_get_size(keymap));
                    check(pread(ei_keymap_get_fd(keymap), keymapText.data(), keymapText.size(), 0) ==
                          static_cast<ssize_t>(keymapText.size()), "read EIS keymap");
                    QElapsedTimer timer;
                    timer.start();
                    map = xkb_keymap_new_from_string(context, keymapText.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
                    compileMs = timer.nsecsElapsed() / 1e6;
                    state = map ? xkb_state_new(map) : nullptr;
                }
                break;
            case EI_EVENT_DEVICE_RESUMED:
                if (ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD) && !keyboard)
                    keyboard = ei_device_ref(device);
                if (ei_device_has_capability(device, EI_DEVICE_CAP_TEXT) && !text)
                    text = ei_device_ref(device);
                ei_device_start_emulating(device, 1);
                break;
            case EI_EVENT_PONG: pong = true; break;
            case EI_EVENT_DISCONNECT: disconnected = true; break;
            default: break;
            }
            ei_event_unref(event);
        }
    }
    template<typename Condition> void waitFor(Condition condition) {
        QElapsedTimer timer;
        timer.start();
        while (!condition() && timer.elapsed() < 3000) {
            pump();
            QThread::msleep(1);
        }
        check(condition(), "libei/EIS barrier");
    }
    void sync() {
        pong = false;
        auto* ping = ei_new_ping(client);
        ei_ping(ping);
        waitFor([&] { return pong; });
        ei_ping_unref(ping);
        check(!disconnected, "sender stays connected");
    }
    // Exact KDE Connect 26.08 scan: effective group, every level, code < max.
    std::optional<uint32_t> keycodeFromKeysym(xkb_keysym_t symbol) {
        const auto layout = xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_EFFECTIVE);
        const auto max = xkb_keymap_max_keycode(map);
        for (auto code = xkb_keymap_min_keycode(map); code < max; ++code) {
            const auto levels = xkb_keymap_num_levels_for_key(map, code, layout);
            for (xkb_level_index_t level = 0; level < levels; ++level) {
                const xkb_keysym_t* syms;
                const auto count = xkb_keymap_key_get_syms_by_level(map, code, layout, level, &syms);
                for (int i = 0; i < count; ++i)
                    if (syms[i] == symbol) return code - 8;
            }
        }
        return std::nullopt;
    }
    void key(uint32_t code, bool pressed) {
        ei_device_keyboard_key(keyboard, code, pressed);
        ei_device_frame(keyboard, ei_now(client));
    }
};

bool modifier(uint32_t code, xkb_keysym_t symbol) {
    // XKB may synthesize a level selector on a virtual modifier key rather
    // than the physical Right Alt code. Classify that symbol too.
    if ((symbol >= XKB_KEY_Shift_L && symbol <= XKB_KEY_Hyper_R) ||
        symbol == XKB_KEY_ISO_Level3_Shift || symbol == XKB_KEY_ISO_Level5_Shift || symbol == XKB_KEY_Mode_switch)
        return true;
    return code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT || code == KEY_LEFTCTRL ||
           code == KEY_RIGHTCTRL || code == KEY_LEFTALT || code == KEY_RIGHTALT ||
           code == KEY_LEFTMETA || code == KEY_RIGHTMETA;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const char* layout = argc > 1 ? argv[1] : "us";
    setenv("XKB_DEFAULT_LAYOUT", layout, 1);
    setenv("XKB_DEFAULT_VARIANT", "", 1);
    setenv("XKB_DEFAULT_OPTIONS", "", 1);
    IsolatedWayland server;
    try {
        hkcf::WaylandInput input;
        check(input.ensureReady(), "isolated Wayland input ready");
        hkcf::InputState inputs(input);
        qint64 rateTime = 0;
        QLoggingCategory::setFilterRules(QStringLiteral("default.info=true"));
        const auto previousLogger = qInstallMessageHandler(startupLogger);
        hkcf::EisInputBridge bridge(input, inputs, nullptr, [&] { return rateTime; });
        qInstallMessageHandler(previousLogger);
        check(startupMessages == 1 && startupMessage.contains("first omitted code point:"), "one startup repertoire summary");
        std::cout << startupMessage.toStdString() << '\n';
        const auto sync = [&] {
            const int before = capture.frames;
            check(input.pointerMotion(0, 0), "Wayland barrier motion");
            QElapsedTimer timer;
            timer.start();
            while (capture.frames == before && timer.elapsed() < 3000) QThread::msleep(1);
            check(capture.frames > before, "Wayland barrier");
        };
        const auto released = [&] {
            sync();
            for (uint32_t code = 1; code <= KEY_MAX; ++code)
                check(!input.isKeycodePressed(code), "no held physical keys");
            std::lock_guard lock(capture.mutex);
            check(!capture.mapError && capture.mods.back() == 0, "no stuck modifiers");
        };
        const auto before = [&] {
            sync();
            std::lock_guard lock(capture.mutex);
            return capture.keys.size();
        };
        const auto assertSymbols = [&](size_t start, const std::vector<xkb_keysym_t>& expected) {
            sync();
            std::vector<xkb_keysym_t> actual;
            std::lock_guard lock(capture.mutex);
            for (size_t i = start; i < capture.keys.size(); ++i)
                if (capture.keys[i].state && !modifier(capture.keys[i].code, capture.symbols[i])) actual.push_back(capture.symbols[i]);
            if (actual != expected) {
                std::cerr << "expected:";
                for (auto sym : expected) std::cerr << " 0x" << std::hex << sym;
                std::cerr << " actual:";
                for (auto sym : actual) std::cerr << " 0x" << std::hex << sym;
                std::cerr << std::dec << '\n';
            }
            check(actual == expected, "injected keysyms equal input");
        };

        Sender kde(bridge, "kde", hkcf::security::usesLookupKeyboard("org.kde.kdeconnect.daemon"));
        auto* base = xkb_keymap_new_from_names(kde.context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
        check(base, "reference layout");
        QSet<uint32_t> baseCodes;
        for (auto code = xkb_keymap_min_keycode(base); code <= xkb_keymap_max_keycode(base); ++code) {
            const xkb_keysym_t* first;
            const auto count = xkb_keymap_key_get_syms_by_level(base, code, 0, 0, &first);
            if (count && xkb_keysym_to_utf32(first[0])) baseCodes.insert(code - 8);
            for (xkb_layout_index_t group = 0; group < xkb_keymap_num_layouts_for_key(base, code); ++group) {
                for (xkb_level_index_t level = 0; level < xkb_keymap_num_levels_for_key(base, code, group); ++level) {
                    const xkb_keysym_t* syms;
                    const auto n = xkb_keymap_key_get_syms_by_level(base, code, group, level, &syms);
                    for (int i = 0; i < n; ++i) {
                        const auto cp = xkb_keysym_to_utf32(syms[i]);
                        if (cp && QChar::category(cp) != QChar::Other_NotAssigned)
                            check(kde.keycodeFromKeysym(xkb_utf32_to_keysym(cp)).has_value(), "every higher layout character is discoverable");
                    }
                }
            }
        }
        check(!kde.keycodeFromKeysym(XKB_KEY_F1) && !kde.keycodeFromKeysym(XKB_KEY_XF86AudioMute),
              "non-printable base action symbols are not advertised");
        size_t extraCount = 0;
        std::set<xkb_keysym_t> advertised;
        for (auto code = xkb_keymap_min_keycode(kde.map); code <= xkb_keymap_max_keycode(kde.map); ++code) {
            check(code >= 8 && code - 8 < KEY_CNT, "every advertised evdev code is below 768");
            check(xkb_keymap_num_levels_for_key(kde.map, code, 0) <= 1, "lookup map is single-level");
            const xkb_keysym_t* syms;
            const auto n = xkb_keymap_key_get_syms_by_level(kde.map, code, 0, 0, &syms);
            if (n && syms[0] != XKB_KEY_NoSymbol) {
                check(advertised.insert(syms[0]).second || baseCodes.contains(code - 8), "extras are deduplicated");
                if (!baseCodes.contains(code - 8)) ++extraCount;
            }
            if (baseCodes.contains(code - 8)) {
                const xkb_keysym_t* original;
                const auto count = xkb_keymap_key_get_syms_by_level(base, code, 0, 0, &original);
                check(count ? n && syms[0] == original[0] : !n, "base keys preserve original level zero");
            }
        }
        const xkb_keysym_t* last;
        check(xkb_keymap_key_get_syms_by_level(kde.map, xkb_keymap_max_keycode(kde.map), 0, 0, &last) == 0,
              "highest keycode is an unused sentinel");
        xkb_keymap_unref(base);
        double total = 0, maximum = 0;
        for (int i = 0; i < 25; ++i) {
            QElapsedTimer timer;
            timer.start();
            auto* compiled = xkb_keymap_new_from_string(kde.context, kde.keymapText.c_str(),
                XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
            const double ms = timer.nsecsElapsed() / 1e6;
            check(compiled, "lookup recompilation");
            xkb_keymap_unref(compiled);
            total += ms;
            maximum = std::max(maximum, ms);
        }
        std::cout << std::fixed << std::setprecision(3) << "layout=" << layout << " keymap_bytes=" << kde.keymapText.size()
                  << " extras=" << extraCount << " initial_compile_ms=" << kde.compileMs
                  << " compile_25_mean_ms=" << total / 25 << " max_ms=" << maximum << '\n';
        const auto type = [&](const QString& text) {
            for (auto point : text.toUcs4()) {
                const auto symbol = xkb_utf32_to_keysym(point);
                const auto code = kde.keycodeFromKeysym(symbol);
                if (!code) std::cerr << "missing U+" << std::hex << point << " symbol=0x" << symbol << std::dec << '\n';
                check(code.has_value(), "KDE Connect scan finds every character");
                const auto start = before();
                kde.key(*code, true);
                kde.key(*code, false);
                kde.sync();
                assertSymbols(start, {symbol});
                released();
            }
        };
        type(QString::fromUtf8("zażółć gęślą jaźń ZAŻÓŁĆ Hello World!"));
        type(QString::fromUtf8("αЯ€—șțơưƠƯ")); // Dynamic fallback and preferred Latin-B characters.
        for (const auto& bounds : {std::pair{0x00a0u, 0x017fu}, std::pair{0x2010u, 0x2027u},
                                   std::pair{0x2030u, 0x203au}, std::pair{0x0400u, 0x045fu},
                                   std::pair{0x0386u, 0x03ceu}, std::pair{0x20a0u, 0x20c0u},
                                   std::pair{0x0218u, 0x021bu}}) {
            for (auto cp = bounds.first; cp <= bounds.second; ++cp) {
                if (QChar::category(cp) != QChar::Other_NotAssigned)
                    check(kde.keycodeFromKeysym(xkb_utf32_to_keysym(cp)).has_value(), "priority blocks fit before Latin-B truncation");
            }
        }

        // KDE Connect drops unknown characters locally without sending a key.
        const auto dropped = before();
        check(!kde.keycodeFromKeysym(xkb_utf32_to_keysym(0x024f)), "truncated Latin-B suffix is absent");
        check(!kde.keycodeFromKeysym(xkb_utf32_to_keysym(0x1f600)), "outside-repertoire emoji is dropped");
        check(!kde.keycodeFromKeysym(xkb_utf32_to_keysym(0x038b)), "unassigned code point is absent");
        kde.sync();
        assertSymbols(dropped, {});
        type("A"); // No disconnect or state corruption after an unknown symbol.

        const auto upper = kde.keycodeFromKeysym('A');
        check(upper && !baseCodes.contains(*upper), "uppercase uses a free synthetic code below 768");
        kde.key(*upper, true);
        kde.sync();
        check(input.isKeycodePressed(KEY_A) && input.isKeycodePressed(KEY_LEFTSHIFT), "synthetic press holds Shift and A");
        kde.key(*upper, false);
        kde.sync();
        released();

        for (bool releaseRawFirst : {false, true}) {
            kde.key(KEY_A, true);
            kde.key(*upper, true);
            kde.sync();
            kde.key(releaseRawFirst ? KEY_A : *upper, false);
            kde.sync();
            check(input.isKeycodePressed(KEY_A), "remaining raw/synthetic owner retains physical A");
            kde.key(releaseRawFirst ? *upper : KEY_A, false);
            kde.sync();
            released();
        }
        kde.key(*upper, true);
        kde.sync();
        ei_device_stop_emulating(kde.keyboard);
        kde.sync();
        released();
        ei_device_start_emulating(kde.keyboard, 2);
        kde.key(*upper, false); // libei retains its sender state across stops.
        kde.sync();
        released();
        kde.key(*upper, true);
        kde.sync();
        auto* seat = ei_seat_ref(ei_device_get_seat(kde.keyboard));
        ei_device_close(kde.keyboard);
        kde.keyboard = ei_device_unref(kde.keyboard);
        kde.waitFor([&] { return !input.isKeycodePressed(KEY_A); });
        released();
        ei_seat_request_device_with_capabilities(seat, EI_DEVICE_CAP_KEYBOARD, nullptr);
        kde.waitFor([&] { return kde.keyboard != nullptr; });
        ei_seat_unref(seat);
        kde.key(*upper, true);
        kde.key(*upper, false);
        kde.sync();
        released();

        const auto ignored = before();
        kde.key(xkb_keymap_max_keycode(kde.map) - 8, true);
        kde.key(xkb_keymap_max_keycode(kde.map) - 8, false);
        kde.sync();
        assertSymbols(ignored, {});
        released();

        // Real-layout clients get exactly the original map, even with a KDE-like name.
        for (const char* name : {"deskflow client", "kdeconnectd"}) {
            Sender real(bridge, QString::fromLatin1(name), false, name);
            auto* base = xkb_keymap_new_from_names(real.context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
            char* original = xkb_keymap_get_as_string(base, XKB_KEYMAP_FORMAT_TEXT_V1);
            check(real.keymapText == std::string(original, std::strlen(original) + 1), "Deskflow/unknown keymap unchanged byte-for-byte");
            free(original);
            xkb_keymap_unref(base);
            check(real.keycodeFromKeysym('A') == KEY_A, "Deskflow retains higher keymap levels");
            const auto control = before();
            real.key(*real.keycodeFromKeysym('A'), true);
            real.key(*real.keycodeFromKeysym('A'), false);
            real.sync();
            assertSymbols(control, {'a'}); // Baseline KDE bare-keycode failure.
            const auto start = before();
            real.key(KEY_LEFTSHIFT, true);
            real.key(KEY_A, true);
            real.key(KEY_A, false);
            real.key(KEY_LEFTSHIFT, false);
            real.sync();
            assertSymbols(start, {'A'});
            released();
        }
        // KDE Connect's explicit modifier/special-key keycodes keep the raw path.
        for (const auto mod : {KEY_LEFTSHIFT, KEY_LEFTCTRL, KEY_LEFTALT}) {
            kde.key(mod, true);
            kde.key(KEY_A, true);
            kde.sync();
            check(input.isKeycodePressed(mod) && input.isKeycodePressed(KEY_A), "raw shortcut retained");
            kde.key(KEY_A, false);
            kde.key(mod, false);
            kde.sync();
            released();
        }
        // KDE Connect 26.04/26.08 SpecialKeysMap, in upstream table order.
        constexpr uint32_t specialKeys[] = {
            KEY_BACKSPACE, KEY_TAB, KEY_LINEFEED, KEY_LEFT, KEY_UP, KEY_RIGHT, KEY_DOWN,
            KEY_PAGEUP, KEY_PAGEDOWN, KEY_HOME, KEY_END, KEY_ENTER, KEY_DELETE, KEY_ESC,
            KEY_SYSRQ, KEY_SCROLLLOCK, KEY_LEFTCTRL, KEY_LEFTALT, KEY_LEFTSHIFT, KEY_LEFTMETA,
            KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
        };
        for (const auto code : specialKeys) {
            const auto start = before();
            kde.key(code, true);
            kde.key(code, false);
            kde.sync();
            sync();
            {
                std::lock_guard lock(capture.mutex);
                check(capture.keys.size() == start + 2 && capture.keys[start].code == code &&
                      capture.keys[start].state == 1 && capture.keys[start + 1].code == code &&
                      capture.keys[start + 1].state == 0, "every SpecialKeysMap entry takes the raw keycode path");
            }
            released();
        }
        for (const auto code : {KEY_RIGHTCTRL, KEY_RIGHTALT, KEY_RIGHTSHIFT, KEY_RIGHTMETA,
                                KEY_CAPSLOCK, KEY_NUMLOCK, KEY_COMPOSE, KEY_FN}) {
            const auto start = before();
            const size_t pairs = code == KEY_CAPSLOCK || code == KEY_NUMLOCK ? 2 : 1;
            for (size_t i = 0; i < pairs; ++i) {
                kde.key(code, true);
                kde.key(code, false);
            }
            kde.sync();
            sync();
            {
                std::lock_guard lock(capture.mutex);
                check(capture.keys.size() == start + pairs * 2, "reserved modifiers forward exactly one raw pair per request");
                for (size_t i = 0; i < pairs * 2; ++i)
                    check(capture.keys[start + i].code == uint32_t(code) && capture.keys[start + i].state == (i % 2 == 0),
                          "other modifiers also take the raw path");
            }
            released();
        }

        // Exhaust a fixed-time budget while another synthetic key is held.
        // Its release must still balance both the real key and Shift.
        Sender limited(bridge, "limited", true);
        const auto limitedA = limited.keycodeFromKeysym('A');
        const auto limitedB = limited.keycodeFromKeysym('B');
        check(limitedA && limitedB, "budget test symbols");
        limited.key(*limitedA, true);
        for (int i = 1; i < hkcf::security::kMaxNotifyEventsPerWindow; ++i) {
            limited.key(*limitedB, true);
            limited.key(*limitedB, false);
            if (i % 32 == 0) { limited.sync(); sync(); }
        }
        limited.sync();
        const auto exhausted = before();
        limited.key(*limitedB, true);
        limited.key(*limitedB, false);
        limited.sync();
        assertSymbols(exhausted, {});
        limited.key(*limitedA, false);
        limited.sync();
        released();
        rateTime += hkcf::security::kNotifyRateWindowMs;
        const auto refilled = before();
        limited.key(*limitedB, true);
        limited.key(*limitedB, false);
        limited.sync();
        assertSymbols(refilled, {'B'});
        released();

        // Shared physical holds survive lookup-owner revocation.
        Sender other(bridge, "other", false, "deskflow client");
        other.key(KEY_LEFTSHIFT, true);
        other.sync();
        kde.key(*upper, true);
        kde.sync();
        bridge.removeClient("kde");
        sync();
        check(input.isKeycodePressed(KEY_LEFTSHIFT) && !input.isKeycodePressed(KEY_A), "close releases only lookup owner's holds");
        other.key(KEY_LEFTSHIFT, false);
        other.sync();
        released();

        Sender modern(bridge, "modern", true, nullptr, true);
        const auto start = before();
        ei_device_text_keysym(modern.text, 'A', true);
        ei_device_frame(modern.text, ei_now(modern.client));
        ei_device_text_keysym(modern.text, 'A', false);
        ei_device_frame(modern.text, ei_now(modern.client));
        modern.sync();
        assertSymbols(start, {'A'});
        released();
        std::cout << "KDE lookup, Deskflow/unknown map, shortcuts, owner cleanup and text-device checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
