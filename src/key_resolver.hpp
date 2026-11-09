#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <xkbcommon/xkbcommon.h>

namespace hkcf {

struct ResolvedKey {
    std::uint32_t evdevKeycode = 0;
    bool needsShift = false;
    xkb_mod_mask_t modifierMask = 0;
    std::vector<std::uint32_t> modifierKeycodes;
};

class KeyResolver {
  public:
    using Clock = std::function<std::int64_t()>;
    explicit KeyResolver(Clock clock = monotonicTimeMs);
    explicit KeyResolver(const xkb_rule_names& names, Clock clock = monotonicTimeMs);
    ~KeyResolver();

    KeyResolver(const KeyResolver&) = delete;
    KeyResolver& operator=(const KeyResolver&) = delete;

    [[nodiscard]] bool valid() const;
    [[nodiscard]] std::optional<ResolvedKey> resolveKeysym(xkb_keysym_t keysym) const;
    // Held evdev codes cannot be reassigned, including holds from raw input.
    [[nodiscard]] std::optional<ResolvedKey> resolveOrExtend(xkb_keysym_t keysym, const std::set<std::uint32_t>& held = {});
    [[nodiscard]] xkb_keymap* keymap() const { return m_keymap.get(); }
    [[nodiscard]] std::string keymapText() const;
    // Caller owns the returned state. No display connection is needed.
    [[nodiscard]] xkb_state* createState(const std::set<std::uint32_t>& held, xkb_state* previous = nullptr) const;
    [[nodiscard]] std::uint64_t revision() const { return m_revision; }
    static constexpr std::size_t kMaxExtraKeysyms = 64;

  private:
    static std::int64_t monotonicTimeMs();
    explicit KeyResolver(const xkb_rule_names* names, Clock clock);
    bool takeReplacementToken();
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
    struct Slot {
        xkb_keycode_t code;
        std::string name;
        bool needsName;
        xkb_keysym_t symbol = XKB_KEY_NoSymbol;
    };
    [[nodiscard]] std::optional<ResolvedKey> resolveWithMask(xkb_keysym_t keysym, xkb_mod_mask_t mask) const;

    XkbContextPtr m_context;
    XkbKeymapPtr m_keymap;
    xkb_mod_index_t m_shiftIndex = XKB_MOD_INVALID;
    std::string m_baseText;
    std::vector<Slot> m_slots;
    std::size_t m_nextSlot = 0;
    std::uint64_t m_revision = 0;
    Clock m_clock;
    std::int64_t m_budgetTimeMs;
    double m_replacementTokens;
    std::optional<std::int64_t> m_lastBudgetWarningMs;
};

} // namespace hkcf
