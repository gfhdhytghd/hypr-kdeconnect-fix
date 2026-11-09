#pragma once

#include "key_resolver.hpp"

#include <QHash>
#include <QSet>
#include <QSharedPointer>

namespace hkcf {

class WaylandInput;

// Notify sessions and EIS devices share a single Wayland input device.
class InputState {
  public:
    explicit InputState(WaylandInput& input);
    ~InputState();

    InputState(const InputState&) = delete;
    InputState& operator=(const InputState&) = delete;

    bool keyboardKeycode(const void* owner, std::uint32_t code, bool pressed);
    bool keyboardKeysym(const void* owner, std::uint32_t symbol, bool pressed);
    bool pointerButton(const void* owner, std::uint32_t code, bool pressed);
    void releaseOwner(const void* owner);

  private:
    using PressedInputs = QHash<const void*, QSet<std::uint32_t>>;
    static bool isPressed(const PressedInputs& inputs, std::uint32_t code);
    static bool update(PressedInputs& inputs, const void* owner, std::uint32_t code, bool pressed);

    WaylandInput& m_input;
    PressedInputs m_keys;
    PressedInputs m_buttons;
    // Stable tokens distinguish a keysym's physical keys from raw keycodes.
    QHash<const void*, QHash<std::uint32_t, QSharedPointer<ResolvedKey>>> m_keysyms;
};

} // namespace hkcf
