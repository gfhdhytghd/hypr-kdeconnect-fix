#include "key_resolver.hpp"

namespace hkcf {

void KeyResolver::XkbContextDeleter::operator()(xkb_context* context) const {
    if (context)
        xkb_context_unref(context);
}

void KeyResolver::XkbKeymapDeleter::operator()(xkb_keymap* keymap) const {
    if (keymap)
        xkb_keymap_unref(keymap);
}

void KeyResolver::XkbStateDeleter::operator()(xkb_state* state) const {
    if (state)
        xkb_state_unref(state);
}

KeyResolver::KeyResolver()
    : m_context(xkb_context_new(XKB_CONTEXT_NO_FLAGS)) {
    if (!m_context)
        return;

    m_keymap.reset(xkb_keymap_new_from_names(m_context.get(), nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS));
    if (!m_keymap)
        return;

    m_shiftIndex = xkb_keymap_mod_get_index(m_keymap.get(), XKB_MOD_NAME_SHIFT);
}

KeyResolver::~KeyResolver() = default;

bool KeyResolver::valid() const {
    return m_context && m_keymap;
}

std::optional<ResolvedKey> KeyResolver::resolveKeysym(xkb_keysym_t keysym) const {
    if (!valid() || keysym == XKB_KEY_NoSymbol)
        return std::nullopt;

    if (auto key = resolveWithMask(keysym, 0, false))
        return key;

    if (m_shiftIndex != XKB_MOD_INVALID && m_shiftIndex < 32)
        return resolveWithMask(keysym, 1u << m_shiftIndex, true);

    return std::nullopt;
}

std::optional<ResolvedKey> KeyResolver::resolveWithMask(xkb_keysym_t keysym, std::uint32_t depressedMask, bool needsShift) const {
    XkbStatePtr state(xkb_state_new(m_keymap.get()));
    if (!state)
        return std::nullopt;

    xkb_state_update_mask(state.get(), depressedMask, 0, 0, 0, 0, 0);

    const auto min = xkb_keymap_min_keycode(m_keymap.get());
    const auto max = xkb_keymap_max_keycode(m_keymap.get());

    for (xkb_keycode_t keycode = min; keycode <= max; ++keycode) {
        if (xkb_state_key_get_one_sym(state.get(), keycode) == keysym) {
            if (keycode < 8)
                continue;
            return ResolvedKey{.evdevKeycode = static_cast<std::uint32_t>(keycode - 8), .needsShift = needsShift};
        }
    }

    return std::nullopt;
}

} // namespace hkcf
