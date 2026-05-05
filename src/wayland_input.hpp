#pragma once

#include "key_resolver.hpp"

#include <cstdint>
#include <optional>

#include <QElapsedTimer>
#include <QHash>
#include <QRect>
#include <QString>

#include <wayland-client.h>

struct zwlr_virtual_pointer_manager_v1;
struct zwlr_virtual_pointer_v1;
struct zwp_virtual_keyboard_manager_v1;
struct zwp_virtual_keyboard_v1;

namespace hkcf {

class WaylandInput {
  public:
    WaylandInput();
    ~WaylandInput();

    WaylandInput(const WaylandInput&) = delete;
    WaylandInput& operator=(const WaylandInput&) = delete;

    [[nodiscard]] bool ensureReady();
    [[nodiscard]] QString lastError() const;

    bool pointerMotion(double dx, double dy);
    bool pointerMotionAbsolute(double x, double y);
    bool pointerButton(std::uint32_t button, bool pressed);
    bool pointerAxis(double dx, double dy);
    bool pointerAxisDiscrete(std::uint32_t axis, int steps);
    bool keyboardKeycode(std::uint32_t keycode, bool pressed);
    bool keyboardKeysym(std::uint32_t keysym, bool pressed);

    static void handleGlobal(void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version);
    static void handleGlobalRemove(void* data, wl_registry* registry, std::uint32_t name);

    static void outputGeometry(void* data,
                               wl_output* output,
                               int32_t x,
                               int32_t y,
                               int32_t physicalWidth,
                               int32_t physicalHeight,
                               int32_t subpixel,
                               const char* make,
                               const char* model,
                               int32_t transform);
    static void outputMode(void* data, wl_output* output, std::uint32_t flags, int32_t width, int32_t height, int32_t refresh);
    static void outputDone(void* data, wl_output* output);
    static void outputScale(void* data, wl_output* output, int32_t factor);
    static void outputName(void* data, wl_output* output, const char* name);
    static void outputDescription(void* data, wl_output* output, const char* description);

  private:
    struct OutputInfo {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    };

    [[nodiscard]] bool connect();
    [[nodiscard]] bool createDevices();
    [[nodiscard]] bool sendKeyboardKeymap();
    [[nodiscard]] std::uint32_t timeMs() const;
    [[nodiscard]] QRect outputBounds() const;
    bool flush();
    void cleanup();
    void setError(const QString& error);

    wl_display* m_display = nullptr;
    wl_registry* m_registry = nullptr;
    wl_seat* m_seat = nullptr;
    wl_output* m_output = nullptr;
    zwlr_virtual_pointer_manager_v1* m_virtualPointerManager = nullptr;
    zwp_virtual_keyboard_manager_v1* m_virtualKeyboardManager = nullptr;
    zwlr_virtual_pointer_v1* m_pointer = nullptr;
    zwp_virtual_keyboard_v1* m_keyboard = nullptr;
    std::uint32_t m_pointerManagerVersion = 1;
    std::uint32_t m_keyboardManagerVersion = 1;
    OutputInfo m_outputInfo;
    QElapsedTimer m_timer;
    QString m_lastError;
    KeyResolver m_keyResolver;
    QHash<std::uint32_t, int> m_keysymShiftCounts;
    int m_shiftedKeysDown = 0;
};

} // namespace hkcf
