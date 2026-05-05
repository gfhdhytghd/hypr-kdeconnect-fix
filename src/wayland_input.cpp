#include "wayland_input.hpp"

#include "security_policy.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <unistd.h>

#include <QDebug>

#include <wayland-client-protocol.h>
#include <virtual-keyboard-unstable-v1-client-protocol.h>
#include <wlr-virtual-pointer-unstable-v1-client-protocol.h>
#include <xkbcommon/xkbcommon.h>

namespace {

constexpr std::uint32_t kPointerButtonReleased = 0;
constexpr std::uint32_t kPointerButtonPressed = 1;
constexpr std::uint32_t kKeyboardKeyReleased = 0;
constexpr std::uint32_t kKeyboardKeyPressed = 1;
constexpr std::uint32_t kKeyboardKeymapFormatXkbV1 = 1;
constexpr std::uint32_t kPointerAxisVertical = 0;
constexpr std::uint32_t kPointerAxisHorizontal = 1;
constexpr std::uint32_t kPointerAxisSourceWheel = 0;
constexpr double kDiscreteStep = 15.0;

} // namespace

namespace hkcf {

namespace {

const wl_registry_listener kRegistryListener = {
    .global = WaylandInput::handleGlobal,
    .global_remove = WaylandInput::handleGlobalRemove,
};

const wl_output_listener kOutputListener = {
    .geometry = WaylandInput::outputGeometry,
    .mode = WaylandInput::outputMode,
    .done = WaylandInput::outputDone,
    .scale = WaylandInput::outputScale,
    .name = WaylandInput::outputName,
    .description = WaylandInput::outputDescription,
};

std::optional<int> createMemfd(const char* name) {
#ifdef MFD_CLOEXEC
    {
        const int fd = memfd_create(name, MFD_CLOEXEC);
        if (fd >= 0)
            return fd;
    }
#endif

    char path[] = "/tmp/hypr-kdeconnect-keymap.XXXXXX";
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

double normalizedAxisDelta(double value) {
    if (value == 0.0)
        return 0.0;
    if (std::abs(value) < kDiscreteStep)
        return std::copysign(kDiscreteStep, value);
    return value;
}

} // namespace

WaylandInput::WaylandInput() {
    m_timer.start();
}

WaylandInput::~WaylandInput() {
    cleanup();
}

bool WaylandInput::ensureReady() {
    if (m_pointer && m_keyboard)
        return true;

    if (!m_display && !connect())
        return false;

    return createDevices();
}

QString WaylandInput::lastError() const {
    return m_lastError;
}

bool WaylandInput::connect() {
    m_display = wl_display_connect(nullptr);
    if (!m_display) {
        setError(QStringLiteral("failed to connect to Wayland display"));
        return false;
    }

    m_registry = wl_display_get_registry(m_display);
    if (!m_registry) {
        setError(QStringLiteral("failed to get Wayland registry"));
        cleanup();
        return false;
    }

    wl_registry_add_listener(m_registry, &kRegistryListener, this);
    if (wl_display_roundtrip(m_display) < 0 || wl_display_roundtrip(m_display) < 0) {
        setError(QStringLiteral("failed to read Wayland registry"));
        cleanup();
        return false;
    }

    if (!m_seat) {
        setError(QStringLiteral("Wayland compositor did not expose wl_seat"));
        cleanup();
        return false;
    }

    if (!m_virtualPointerManager) {
        setError(QStringLiteral("Wayland compositor did not expose zwlr_virtual_pointer_manager_v1"));
        cleanup();
        return false;
    }

    if (!m_virtualKeyboardManager) {
        setError(QStringLiteral("Wayland compositor did not expose zwp_virtual_keyboard_manager_v1"));
        cleanup();
        return false;
    }

    return true;
}

bool WaylandInput::createDevices() {
    if (!m_pointer) {
        m_pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(m_virtualPointerManager, m_seat);
    }

    if (!m_keyboard) {
        m_keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(m_virtualKeyboardManager, m_seat);
        if (!m_keyboard) {
            setError(QStringLiteral("failed to create virtual keyboard"));
            return false;
        }
        if (!sendKeyboardKeymap()) {
            zwp_virtual_keyboard_v1_destroy(m_keyboard);
            m_keyboard = nullptr;
            return false;
        }
    }

    if (!m_pointer) {
        setError(QStringLiteral("failed to create virtual pointer"));
        return false;
    }

    zwlr_virtual_pointer_v1_axis_source(m_pointer, kPointerAxisSourceWheel);

    if (!flush())
        return false;
    if (wl_display_roundtrip(m_display) < 0) {
        setError(QStringLiteral("failed to sync Wayland input devices"));
        return false;
    }
    return true;
}

bool WaylandInput::sendKeyboardKeymap() {
    xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!context) {
        setError(QStringLiteral("failed to create xkb context"));
        return false;
    }

    xkb_keymap* keymap = xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) {
        xkb_context_unref(context);
        setError(QStringLiteral("failed to create xkb keymap"));
        return false;
    }

    char* keymapText = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);

    if (!keymapText) {
        setError(QStringLiteral("failed to serialize xkb keymap"));
        return false;
    }

    const std::size_t size = std::strlen(keymapText) + 1;
    const auto fd = createMemfd("hypr-kdeconnect-keymap");
    if (!fd) {
        free(keymapText);
        setError(QStringLiteral("failed to create keymap fd"));
        return false;
    }

    bool ok = true;
    if (ftruncate(*fd, static_cast<off_t>(size)) != 0)
        ok = false;
    else
        ok = writeAll(*fd, keymapText, size);

    free(keymapText);

    if (!ok) {
        const QString error = QStringLiteral("failed to write keymap fd: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        close(*fd);
        setError(error);
        return false;
    }

    zwp_virtual_keyboard_v1_keymap(m_keyboard, kKeyboardKeymapFormatXkbV1, *fd, static_cast<std::uint32_t>(size));
    ok = flush();
    close(*fd);
    return ok;
}

bool WaylandInput::pointerMotion(double dx, double dy) {
    const auto safeDx = security::boundedFinite(dx, security::kMaxPointerDelta);
    const auto safeDy = security::boundedFinite(dy, security::kMaxPointerDelta);
    if (!safeDx || !safeDy) {
        setError(QStringLiteral("invalid pointer motion delta"));
        return false;
    }
    if (!ensureReady())
        return false;
    zwlr_virtual_pointer_v1_motion(m_pointer, timeMs(), wl_fixed_from_double(*safeDx), wl_fixed_from_double(*safeDy));
    zwlr_virtual_pointer_v1_frame(m_pointer);
    return flush();
}

bool WaylandInput::pointerMotionAbsolute(double x, double y) {
    const auto safeX = security::boundedFinite(x, security::kMaxAbsoluteCoordinate);
    const auto safeY = security::boundedFinite(y, security::kMaxAbsoluteCoordinate);
    if (!safeX || !safeY) {
        setError(QStringLiteral("invalid absolute pointer coordinates"));
        return false;
    }
    if (!ensureReady())
        return false;

    const QRect bounds = outputBounds();
    const auto clampedX = static_cast<int>(std::clamp(*safeX - bounds.x(), 0.0, static_cast<double>(std::max(1, bounds.width()))));
    const auto clampedY = static_cast<int>(std::clamp(*safeY - bounds.y(), 0.0, static_cast<double>(std::max(1, bounds.height()))));

    zwlr_virtual_pointer_v1_motion_absolute(m_pointer,
                                            timeMs(),
                                            static_cast<std::uint32_t>(clampedX),
                                            static_cast<std::uint32_t>(clampedY),
                                            static_cast<std::uint32_t>(std::max(1, bounds.width())),
                                            static_cast<std::uint32_t>(std::max(1, bounds.height())));
    zwlr_virtual_pointer_v1_frame(m_pointer);
    return flush();
}

bool WaylandInput::pointerButton(std::uint32_t button, bool pressed) {
    if (!security::isAllowedPointerButton(button)) {
        setError(QStringLiteral("invalid pointer button"));
        return false;
    }
    if (!ensureReady())
        return false;
    zwlr_virtual_pointer_v1_button(m_pointer, timeMs(), button, pressed ? kPointerButtonPressed : kPointerButtonReleased);
    zwlr_virtual_pointer_v1_frame(m_pointer);
    return flush();
}

bool WaylandInput::pointerAxis(double dx, double dy) {
    const auto safeDx = security::boundedFinite(dx, security::kMaxAxisDelta);
    const auto safeDy = security::boundedFinite(dy, security::kMaxAxisDelta);
    if (!safeDx || !safeDy) {
        setError(QStringLiteral("invalid pointer axis delta"));
        return false;
    }
    if (!ensureReady())
        return false;

    const double dxOut = normalizedAxisDelta(*safeDx);
    const double dyOut = normalizedAxisDelta(*safeDy);
    zwlr_virtual_pointer_v1_axis_source(m_pointer, kPointerAxisSourceWheel);
    if (dxOut != 0.0) {
        zwlr_virtual_pointer_v1_axis(m_pointer, timeMs(), kPointerAxisHorizontal, wl_fixed_from_double(dxOut));
    }
    if (dyOut != 0.0) {
        zwlr_virtual_pointer_v1_axis(m_pointer, timeMs(), kPointerAxisVertical, wl_fixed_from_double(dyOut));
    }
    zwlr_virtual_pointer_v1_frame(m_pointer);
    return flush();
}

bool WaylandInput::pointerAxisDiscrete(std::uint32_t axis, int steps) {
    if (!security::isAllowedDiscreteAxis(axis)) {
        setError(QStringLiteral("invalid discrete pointer axis"));
        return false;
    }
    if (!ensureReady())
        return false;

    const int boundedSteps = security::clampDiscreteScrollSteps(steps);
    const auto pointerAxis = axis == 1 ? kPointerAxisHorizontal : kPointerAxisVertical;
    if (boundedSteps == 0) {
        zwlr_virtual_pointer_v1_axis_source(m_pointer, kPointerAxisSourceWheel);
        zwlr_virtual_pointer_v1_axis_stop(m_pointer, timeMs(), pointerAxis);
        zwlr_virtual_pointer_v1_frame(m_pointer);
        return flush();
    }

    const double value = boundedSteps * kDiscreteStep;
    zwlr_virtual_pointer_v1_axis_source(m_pointer, kPointerAxisSourceWheel);
    zwlr_virtual_pointer_v1_axis_discrete(m_pointer, timeMs(), pointerAxis, wl_fixed_from_double(value), boundedSteps);
    zwlr_virtual_pointer_v1_frame(m_pointer);
    return flush();
}

bool WaylandInput::keyboardKeycode(std::uint32_t keycode, bool pressed) {
    if (!security::isAllowedKeyboardKeycode(keycode)) {
        setError(QStringLiteral("invalid keyboard keycode"));
        return false;
    }
    if (!ensureReady())
        return false;

    zwp_virtual_keyboard_v1_key(m_keyboard, timeMs(), keycode, pressed ? kKeyboardKeyPressed : kKeyboardKeyReleased);
    return flush();
}

bool WaylandInput::keyboardKeysym(std::uint32_t keysym, bool pressed) {
    if (!security::isAllowedKeysym(keysym)) {
        setError(QStringLiteral("invalid keyboard keysym"));
        return false;
    }
    const auto resolved = m_keyResolver.resolveKeysym(static_cast<xkb_keysym_t>(keysym));
    if (!resolved) {
        qWarning() << "failed to resolve keysym" << keysym;
        return false;
    }

    if (pressed) {
        const bool shouldPressShift = resolved->needsShift && m_shiftedKeysDown == 0;
        if (shouldPressShift && !keyboardKeycode(KEY_LEFTSHIFT, true))
            return false;
        if (!keyboardKeycode(resolved->evdevKeycode, true)) {
            if (shouldPressShift)
                keyboardKeycode(KEY_LEFTSHIFT, false);
            return false;
        }
        if (resolved->needsShift) {
            ++m_shiftedKeysDown;
            m_keysymShiftCounts.insert(keysym, m_keysymShiftCounts.value(keysym) + 1);
        }
        return true;
    }

    const bool ok = keyboardKeycode(resolved->evdevKeycode, false);
    bool shiftOk = true;
    auto shifted = m_keysymShiftCounts.find(keysym);
    if (shifted != m_keysymShiftCounts.end()) {
        --m_shiftedKeysDown;
        if (*shifted <= 1)
            m_keysymShiftCounts.erase(shifted);
        else
            --(*shifted);
        if (m_shiftedKeysDown <= 0) {
            m_shiftedKeysDown = 0;
            shiftOk = keyboardKeycode(KEY_LEFTSHIFT, false);
        }
    }
    return ok && shiftOk;
}

QRect WaylandInput::logicalBounds() const {
    return outputBounds();
}

std::uint32_t WaylandInput::timeMs() const {
    return static_cast<std::uint32_t>(m_timer.elapsed());
}

QRect WaylandInput::outputBounds() const {
    if (m_outputInfo.width > 0 && m_outputInfo.height > 0)
        return {m_outputInfo.x, m_outputInfo.y, m_outputInfo.width, m_outputInfo.height};
    return {0, 0, 1920, 1080};
}

bool WaylandInput::flush() {
    if (!m_display)
        return false;

    wl_display_dispatch_pending(m_display);
    if (wl_display_flush(m_display) < 0) {
        setError(QStringLiteral("failed to flush Wayland events: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
        return false;
    }
    wl_display_dispatch_pending(m_display);
    return true;
}

void WaylandInput::cleanup() {
    if (m_keyboard) {
        zwp_virtual_keyboard_v1_destroy(m_keyboard);
        m_keyboard = nullptr;
    }
    if (m_pointer) {
        zwlr_virtual_pointer_v1_destroy(m_pointer);
        m_pointer = nullptr;
    }
    if (m_virtualKeyboardManager) {
        zwp_virtual_keyboard_manager_v1_destroy(m_virtualKeyboardManager);
        m_virtualKeyboardManager = nullptr;
    }
    if (m_virtualPointerManager) {
        zwlr_virtual_pointer_manager_v1_destroy(m_virtualPointerManager);
        m_virtualPointerManager = nullptr;
    }
    if (m_output) {
        wl_output_destroy(m_output);
        m_output = nullptr;
    }
    if (m_seat) {
        wl_seat_destroy(m_seat);
        m_seat = nullptr;
    }
    if (m_registry) {
        wl_registry_destroy(m_registry);
        m_registry = nullptr;
    }
    if (m_display) {
        wl_display_disconnect(m_display);
        m_display = nullptr;
    }
}

void WaylandInput::setError(const QString& error) {
    m_lastError = error;
    qWarning() << error;
}

void WaylandInput::handleGlobal(void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version) {
    auto* self = static_cast<WaylandInput*>(data);
    const QString iface = QString::fromLatin1(interface);

    if (iface == QStringLiteral("wl_seat") && !self->m_seat) {
        self->m_seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, std::min<std::uint32_t>(version, 9)));
    } else if (iface == QStringLiteral("zwlr_virtual_pointer_manager_v1") && !self->m_virtualPointerManager) {
        self->m_pointerManagerVersion = std::min<std::uint32_t>(version, 2);
        self->m_virtualPointerManager =
            static_cast<zwlr_virtual_pointer_manager_v1*>(wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, self->m_pointerManagerVersion));
    } else if (iface == QStringLiteral("zwp_virtual_keyboard_manager_v1") && !self->m_virtualKeyboardManager) {
        self->m_keyboardManagerVersion = std::min<std::uint32_t>(version, 1);
        self->m_virtualKeyboardManager =
            static_cast<zwp_virtual_keyboard_manager_v1*>(wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, self->m_keyboardManagerVersion));
    } else if (iface == QStringLiteral("wl_output") && !self->m_output) {
        self->m_output = static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, std::min<std::uint32_t>(version, 4)));
        if (self->m_output)
            wl_output_add_listener(self->m_output, &kOutputListener, self);
    }
}

void WaylandInput::handleGlobalRemove(void*, wl_registry*, std::uint32_t) {
}

void WaylandInput::outputGeometry(void* data,
                                  wl_output*,
                                  int32_t x,
                                  int32_t y,
                                  int32_t,
                                  int32_t,
                                  int32_t,
                                  const char*,
                                  const char*,
                                  int32_t) {
    auto* self = static_cast<WaylandInput*>(data);
    self->m_outputInfo.x = x;
    self->m_outputInfo.y = y;
}

void WaylandInput::outputMode(void* data, wl_output*, std::uint32_t flags, int32_t width, int32_t height, int32_t) {
    if (!(flags & WL_OUTPUT_MODE_CURRENT))
        return;
    auto* self = static_cast<WaylandInput*>(data);
    self->m_outputInfo.width = width;
    self->m_outputInfo.height = height;
}

void WaylandInput::outputDone(void*, wl_output*) {
}

void WaylandInput::outputScale(void*, wl_output*, int32_t) {
}

void WaylandInput::outputName(void*, wl_output*, const char*) {
}

void WaylandInput::outputDescription(void*, wl_output*, const char*) {
}

} // namespace hkcf
