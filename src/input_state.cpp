#include "input_state.hpp"

#include "security_policy.hpp"
#include "wayland_input.hpp"

#include <algorithm>
#include <linux/input-event-codes.h>

namespace hkcf {

InputState::InputState(WaylandInput& input)
    : m_input(input) {
}

InputState::~InputState() {
    const auto symbolOwners = m_keysyms.keys();
    for (const auto* owner : symbolOwners)
        releaseOwner(owner);
    const auto keyOwners = m_keys.keys();
    for (const auto* owner : keyOwners)
        releaseOwner(owner);
    const auto buttonOwners = m_buttons.keys();
    for (const auto* owner : buttonOwners)
        releaseOwner(owner);
}

bool InputState::isPressed(const PressedInputs& inputs, std::uint32_t code) {
    return std::any_of(inputs.cbegin(), inputs.cend(), [code](const auto& held) { return held.contains(code); });
}

bool InputState::update(PressedInputs& inputs, const void* owner, std::uint32_t code, bool pressed) {
    const bool wasPressed = isPressed(inputs, code);
    if (pressed) {
        inputs[owner].insert(code);
    } else {
        auto it = inputs.find(owner);
        if (it != inputs.end()) {
            it->remove(code);
            if (it->isEmpty())
                inputs.erase(it);
        }
    }
    return wasPressed != isPressed(inputs, code);
}

bool InputState::keyboardKeycode(const void* owner, std::uint32_t code, bool pressed) {
    if (!security::isAllowedKeyboardKeycode(code))
        return false;
    const bool repeated = pressed && m_keys.value(owner).contains(code);
    if (!update(m_keys, owner, code, pressed) && !repeated)
        return true;
    const bool ok = m_input.keyboardKeycode(code, pressed, repeated);
    // A failure before enqueueing a press must not suppress another owner's
    // next press. Keep holds for requests that were queued before flush failed.
    if (!ok && pressed && !repeated && !m_input.isKeycodePressed(code))
        update(m_keys, owner, code, false);
    return ok;
}

bool InputState::pointerButton(const void* owner, std::uint32_t code, bool pressed) {
    if (!security::isAllowedPointerButton(code))
        return false;
    if (!update(m_buttons, owner, code, pressed))
        return true;
    return m_input.pointerButton(code, pressed);
}

bool InputState::keyboardKeysym(const void* owner, std::uint32_t symbol, bool pressed) {
    if (!security::isAllowedKeysym(symbol))
        return false;
    auto symbols = m_keysyms.find(owner);
    if (!pressed && (symbols == m_keysyms.end() || !symbols->contains(symbol)))
        return true;
    QSharedPointer<ResolvedKey> key;
    if (symbols != m_keysyms.end())
        key = symbols->value(symbol);
    const bool repeated = !key.isNull();
    if (!key) {
        const auto resolved = m_input.resolveKeysym(symbol);
        if (!resolved)
            return false;
        key = QSharedPointer<ResolvedKey>::create(*resolved);
        m_keysyms[owner].insert(symbol, key);
    }
    if (pressed) {
        const auto cancelPress = [&] {
            releaseOwner(key.data());
            auto it = m_keysyms.find(owner);
            it->remove(symbol);
            if (it->isEmpty())
                m_keysyms.erase(it);
            return false;
        };
        if (!repeated) {
            for (const auto code : key->modifierKeycodes) {
                if (!keyboardKeycode(key.data(), code, true))
                    return cancelPress();
            }
        }
        if (!keyboardKeycode(key.data(), key->evdevKeycode, true))
            return repeated ? false : cancelPress();
        return true;
    }
    const bool ok = keyboardKeycode(key.data(), key->evdevKeycode, false);
    bool modifiersOk = true;
    for (auto it = key->modifierKeycodes.rbegin(); it != key->modifierKeycodes.rend(); ++it)
        modifiersOk = keyboardKeycode(key.data(), *it, false) && modifiersOk;
    symbols = m_keysyms.find(owner);
    symbols->remove(symbol);
    if (symbols->isEmpty())
        m_keysyms.erase(symbols);
    return ok && modifiersOk;
}

bool InputState::keyboardText(std::uint32_t symbol) {
    if (!security::isAllowedKeysym(symbol))
        return false;
    const auto key = m_input.resolveKeysym(symbol);
    if (!key)
        return false;
    const bool held = isPressed(m_keys, key->evdevKeycode);
    int owner;
    const bool pressed = keyboardKeysym(&owner, symbol, true);
    // Hyprland drops duplicate presses. Retrigger the shared key while the
    // symbol's modifiers are applied, without changing its logical XKB hold.
    bool retriggered = true;
    if (pressed && held) {
        const bool released = m_input.keyboardKeycode(key->evdevKeycode, false, true);
        retriggered = m_input.keyboardKeycode(key->evdevKeycode, true, true) && released;
    }
    const bool released = keyboardKeysym(&owner, symbol, false);
    return pressed && retriggered && released;
}

void InputState::releaseOwner(const void* owner) {
    const auto symbols = m_keysyms.take(owner);
    for (const auto& key : symbols)
        releaseOwner(key.data());
    for (const auto code : m_keys.take(owner)) {
        if (!isPressed(m_keys, code))
            m_input.keyboardKeycode(code, false);
    }
    for (const auto code : m_buttons.take(owner)) {
        if (!isPressed(m_buttons, code))
            m_input.pointerButton(code, false);
    }
}

} // namespace hkcf
