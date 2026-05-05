#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include <xkbcommon/xkbcommon.h>

namespace hkcf {

struct ResolvedKey {
    std::uint32_t evdevKeycode = 0;
    bool needsShift = false;
};

class KeyResolver {
  public:
    KeyResolver();
    ~KeyResolver();

    KeyResolver(const KeyResolver&) = delete;
    KeyResolver& operator=(const KeyResolver&) = delete;

    [[nodiscard]] bool valid() const;
    [[nodiscard]] std::optional<ResolvedKey> resolveKeysym(xkb_keysym_t keysym) const;

  private:
    struct XkbContextDeleter {
        void operator()(xkb_context* context) const;
    };
    struct XkbKeymapDeleter {
        void operator()(xkb_keymap* keymap) const;
    };
    struct XkbStateDeleter {
        void operator()(xkb_state* state) const;
    };

    using XkbContextPtr = std::unique_ptr<xkb_context, XkbContextDeleter>;
    using XkbKeymapPtr = std::unique_ptr<xkb_keymap, XkbKeymapDeleter>;
    using XkbStatePtr = std::unique_ptr<xkb_state, XkbStateDeleter>;

    [[nodiscard]] std::optional<ResolvedKey> resolveWithMask(xkb_keysym_t keysym, std::uint32_t depressedMask, bool needsShift) const;

    XkbContextPtr m_context;
    XkbKeymapPtr m_keymap;
    xkb_mod_index_t m_shiftIndex = XKB_MOD_INVALID;
};

} // namespace hkcf
