#include "eis_input_bridge.hpp"

#include "wayland_input.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <QDebug>
#include <QRect>
#include <QSocketNotifier>

#include <libeis.h>
#include <xkbcommon/xkbcommon.h>

namespace hkcf {

namespace {

struct KeymapFd {
    int fd = -1;
    std::size_t size = 0;
};

std::optional<int> createMemfd(const char* name) {
#ifdef MFD_CLOEXEC
    {
        const int fd = memfd_create(name, MFD_CLOEXEC);
        if (fd >= 0)
            return fd;
    }
#endif

    char path[] = "/tmp/hypr-kdeconnect-eis-keymap.XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0)
        return std::nullopt;
    const int flags = fcntl(fd, F_GETFD);
    if (flags >= 0)
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    unlink(path);
    return fd;
}

bool writeAll(int fd, const char* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t written = write(fd, data + offset, size - offset);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (written == 0)
            return false;
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

std::optional<KeymapFd> createKeymapFd(QString* errorText) {
    xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!context) {
        if (errorText)
            *errorText = QStringLiteral("failed to create xkb context");
        return std::nullopt;
    }

    xkb_keymap* keymap = xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) {
        xkb_context_unref(context);
        if (errorText)
            *errorText = QStringLiteral("failed to create xkb keymap");
        return std::nullopt;
    }

    char* keymapText = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);

    if (!keymapText) {
        if (errorText)
            *errorText = QStringLiteral("failed to serialize xkb keymap");
        return std::nullopt;
    }

    const std::size_t size = std::strlen(keymapText) + 1;
    const auto fd = createMemfd("hypr-kdeconnect-eis-keymap");
    if (!fd) {
        free(keymapText);
        if (errorText)
            *errorText = QStringLiteral("failed to create keymap fd");
        return std::nullopt;
    }

    bool ok = true;
    if (ftruncate(*fd, static_cast<off_t>(size)) != 0)
        ok = false;
    else
        ok = writeAll(*fd, keymapText, size);
    free(keymapText);

    if (!ok) {
        const QString text = QStringLiteral("failed to write keymap fd: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        close(*fd);
        if (errorText)
            *errorText = text;
        return std::nullopt;
    }

    return KeymapFd{*fd, size};
}

void configureRegion(eis_device* device, const QRect& bounds) {
    eis_region* region = eis_device_new_region(device);
    if (!region)
        return;
    eis_region_set_offset(region, static_cast<std::uint32_t>(std::max(0, bounds.x())), static_cast<std::uint32_t>(std::max(0, bounds.y())));
    eis_region_set_size(region,
                        static_cast<std::uint32_t>(std::max(1, bounds.width())),
                        static_cast<std::uint32_t>(std::max(1, bounds.height())));
    eis_region_add(region);
    eis_region_unref(region);
}

int wheelStep(double delta) {
    if (!std::isfinite(delta) || delta == 0.0)
        return 0;
    return delta > 0.0 ? 1 : -1;
}

} // namespace

EisInputBridge::EisInputBridge(WaylandInput& input, QObject* parent)
    : QObject(parent)
    , m_input(input) {
}

EisInputBridge::~EisInputBridge() {
    cleanup();
}

std::optional<int> EisInputBridge::addClient(QString* errorText) {
    if (!ensureContext(errorText))
        return std::nullopt;

    const int fd = eis_backend_fd_add_client(m_eis);
    if (fd < 0) {
        if (errorText)
            *errorText = QStringLiteral("failed to create EIS client fd: %1").arg(QString::fromLocal8Bit(std::strerror(-fd)));
        return std::nullopt;
    }
    return fd;
}

bool EisInputBridge::ensureContext(QString* errorText) {
    if (m_eis)
        return true;

    m_eis = eis_new(this);
    if (!m_eis) {
        if (errorText)
            *errorText = QStringLiteral("failed to create EIS context");
        return false;
    }
    eis_log_set_handler(m_eis, nullptr);
    eis_log_set_priority(m_eis, EIS_LOG_PRIORITY_ERROR);

    const int rc = eis_setup_backend_fd(m_eis);
    if (rc < 0) {
        if (errorText)
            *errorText = QStringLiteral("failed to initialize EIS fd backend: %1").arg(QString::fromLocal8Bit(std::strerror(-rc)));
        cleanup();
        return false;
    }

    auto* notifier = new QSocketNotifier(eis_get_fd(m_eis), QSocketNotifier::Read, this);
    connect(notifier, &QSocketNotifier::activated, this, [this]() {
        dispatch();
    });
    m_notifier = notifier;
    return true;
}

void EisInputBridge::dispatch() {
    if (!m_eis)
        return;
    eis_dispatch(m_eis);
    while (eis_event* event = eis_get_event(m_eis)) {
        handleEvent(event);
        eis_event_unref(event);
    }
}

void EisInputBridge::handleEvent(eis_event* event) {
    switch (eis_event_get_type(event)) {
    case EIS_EVENT_CLIENT_CONNECT:
        handleClientConnect(eis_event_get_client(event));
        break;
    case EIS_EVENT_CLIENT_DISCONNECT:
        handleClientDisconnect(eis_event_get_client(event));
        break;
    case EIS_EVENT_SEAT_BIND:
        handleSeatBind(event);
        break;
    case EIS_EVENT_DEVICE_CLOSED:
        handleDeviceClosed(eis_event_get_device(event));
        break;
    case EIS_EVENT_POINTER_MOTION:
    case EIS_EVENT_POINTER_MOTION_ABSOLUTE:
    case EIS_EVENT_BUTTON_BUTTON:
    case EIS_EVENT_SCROLL_DELTA:
    case EIS_EVENT_SCROLL_DISCRETE:
    case EIS_EVENT_SCROLL_STOP:
    case EIS_EVENT_KEYBOARD_KEY:
        handleInputEvent(event);
        break;
    case EIS_EVENT_SYNC:
    case EIS_EVENT_FRAME:
    case EIS_EVENT_DEVICE_START_EMULATING:
    case EIS_EVENT_DEVICE_STOP_EMULATING:
    case EIS_EVENT_PONG:
    case EIS_EVENT_SCROLL_CANCEL:
    case EIS_EVENT_TOUCH_DOWN:
    case EIS_EVENT_TOUCH_UP:
    case EIS_EVENT_TOUCH_MOTION:
        break;
    }
}

void EisInputBridge::handleClientConnect(eis_client* client) {
    if (!client || !eis_client_is_sender(client)) {
        if (client)
            eis_client_disconnect(client);
        return;
    }

    eis_client_connect(client);

    eis_seat* seat = eis_client_new_seat(client, "default");
    if (!seat)
        return;
    eis_seat_configure_capability(seat, EIS_DEVICE_CAP_KEYBOARD);
    eis_seat_configure_capability(seat, EIS_DEVICE_CAP_POINTER);
    eis_seat_configure_capability(seat, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
    eis_seat_configure_capability(seat, EIS_DEVICE_CAP_BUTTON);
    eis_seat_configure_capability(seat, EIS_DEVICE_CAP_SCROLL);

    auto* state = new SeatState;
    state->client = client;
    m_seats.insert(seat, state);
    eis_seat_add(seat);
}

void EisInputBridge::handleClientDisconnect(eis_client* client) {
    QList<eis_seat*> seats;
    for (auto it = m_seats.cbegin(); it != m_seats.cend(); ++it) {
        if (it.value()->client == client)
            seats.append(it.key());
    }
    for (eis_seat* seat : seats)
        removeSeat(seat);
}

void EisInputBridge::handleSeatBind(eis_event* event) {
    eis_seat* seat = eis_event_get_seat(event);
    SeatState* state = m_seats.value(seat, nullptr);
    if (!state)
        return;

    if (eis_event_seat_has_capability(event, EIS_DEVICE_CAP_KEYBOARD) && !state->keyboard)
        state->keyboard = addKeyboard(seat);
    if ((eis_event_seat_has_capability(event, EIS_DEVICE_CAP_POINTER) || eis_event_seat_has_capability(event, EIS_DEVICE_CAP_BUTTON) ||
         eis_event_seat_has_capability(event, EIS_DEVICE_CAP_SCROLL)) &&
        !state->pointer)
        state->pointer = addPointer(seat);
    if (eis_event_seat_has_capability(event, EIS_DEVICE_CAP_POINTER_ABSOLUTE) && !state->absolutePointer)
        state->absolutePointer = addAbsolutePointer(seat);
}

void EisInputBridge::handleDeviceClosed(eis_device* device) {
    if (!device)
        return;
    eis_seat* seat = eis_device_get_seat(device);
    SeatState* state = m_seats.value(seat, nullptr);
    if (!state)
        return;
    if (state->keyboard == device) {
        eis_device_unref(state->keyboard);
        state->keyboard = nullptr;
    }
    if (state->pointer == device) {
        eis_device_unref(state->pointer);
        state->pointer = nullptr;
    }
    if (state->absolutePointer == device) {
        eis_device_unref(state->absolutePointer);
        state->absolutePointer = nullptr;
    }
}

void EisInputBridge::handleInputEvent(eis_event* event) {
    switch (eis_event_get_type(event)) {
    case EIS_EVENT_POINTER_MOTION:
        m_input.pointerMotion(eis_event_pointer_get_dx(event), eis_event_pointer_get_dy(event));
        break;
    case EIS_EVENT_POINTER_MOTION_ABSOLUTE:
        m_input.pointerMotionAbsolute(eis_event_pointer_get_absolute_x(event), eis_event_pointer_get_absolute_y(event));
        break;
    case EIS_EVENT_BUTTON_BUTTON:
        m_input.pointerButton(eis_event_button_get_button(event), eis_event_button_get_is_press(event));
        break;
    case EIS_EVENT_SCROLL_DELTA:
        if (const int x = wheelStep(eis_event_scroll_get_dx(event)); x != 0)
            m_input.pointerAxisDiscrete(1, x);
        if (const int y = wheelStep(-eis_event_scroll_get_dy(event)); y != 0)
            m_input.pointerAxisDiscrete(0, y);
        break;
    case EIS_EVENT_SCROLL_DISCRETE: {
        const int x = eis_event_scroll_get_discrete_dx(event);
        const int y = eis_event_scroll_get_discrete_dy(event);
        if (x != 0)
            m_input.pointerAxisDiscrete(1, x);
        if (y != 0)
            m_input.pointerAxisDiscrete(0, -y);
        break;
    }
    case EIS_EVENT_SCROLL_STOP:
        if (eis_event_scroll_get_stop_x(event))
            m_input.pointerAxisDiscrete(1, 0);
        if (eis_event_scroll_get_stop_y(event))
            m_input.pointerAxisDiscrete(0, 0);
        break;
    case EIS_EVENT_KEYBOARD_KEY:
        m_input.keyboardKeycode(eis_event_keyboard_get_key(event), eis_event_keyboard_get_key_is_press(event));
        break;
    default:
        break;
    }
}

eis_device* EisInputBridge::addKeyboard(eis_seat* seat) {
    eis_device* device = eis_seat_new_device(seat);
    if (!device)
        return nullptr;
    eis_device_configure_type(device, EIS_DEVICE_TYPE_VIRTUAL);
    eis_device_configure_name(device, "hypr-kdeconnect keyboard");
    eis_device_configure_capability(device, EIS_DEVICE_CAP_KEYBOARD);

    QString errorText;
    const auto fd = createKeymapFd(&errorText);
    if (!fd) {
        qWarning() << "failed to create EIS keyboard keymap:" << errorText;
        eis_device_unref(device);
        return nullptr;
    }
    eis_keymap* keymap = eis_device_new_keymap(device, EIS_KEYMAP_TYPE_XKB, fd->fd, fd->size);
    close(fd->fd);
    if (!keymap) {
        qWarning() << "failed to attach EIS keyboard keymap";
        eis_device_unref(device);
        return nullptr;
    }
    eis_keymap_add(keymap);
    eis_keymap_unref(keymap);

    eis_device_add(device);
    eis_device_resume(device);
    return device;
}

eis_device* EisInputBridge::addPointer(eis_seat* seat) {
    eis_device* device = eis_seat_new_device(seat);
    if (!device)
        return nullptr;
    eis_device_configure_type(device, EIS_DEVICE_TYPE_VIRTUAL);
    eis_device_configure_name(device, "hypr-kdeconnect pointer");
    eis_device_configure_capability(device, EIS_DEVICE_CAP_POINTER);
    eis_device_configure_capability(device, EIS_DEVICE_CAP_BUTTON);
    eis_device_configure_capability(device, EIS_DEVICE_CAP_SCROLL);
    eis_device_add(device);
    eis_device_resume(device);
    return device;
}

eis_device* EisInputBridge::addAbsolutePointer(eis_seat* seat) {
    eis_device* device = eis_seat_new_device(seat);
    if (!device)
        return nullptr;
    eis_device_configure_type(device, EIS_DEVICE_TYPE_VIRTUAL);
    eis_device_configure_name(device, "hypr-kdeconnect absolute pointer");
    eis_device_configure_capability(device, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
    configureRegion(device, m_input.logicalBounds());
    eis_device_add(device);
    eis_device_resume(device);
    return device;
}

void EisInputBridge::removeSeat(eis_seat* seat) {
    SeatState* state = m_seats.take(seat);
    if (!state)
        return;
    if (state->keyboard)
        eis_device_unref(state->keyboard);
    if (state->pointer)
        eis_device_unref(state->pointer);
    if (state->absolutePointer)
        eis_device_unref(state->absolutePointer);
    delete state;
    eis_seat_unref(seat);
}

void EisInputBridge::cleanup() {
    const QList<eis_seat*> seats = m_seats.keys();
    for (eis_seat* seat : seats)
        removeSeat(seat);

    delete m_notifier;
    m_notifier = nullptr;

    if (m_eis) {
        eis_unref(m_eis);
        m_eis = nullptr;
    }
}

} // namespace hkcf
