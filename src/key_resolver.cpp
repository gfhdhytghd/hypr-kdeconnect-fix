#include "key_resolver.hpp"
#include "security_policy.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <linux/input-event-codes.h>
#include <sstream>
#include <QDebug>

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

std::int64_t KeyResolver::monotonicTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

KeyResolver::KeyResolver(Clock clock)
    : KeyResolver(nullptr, std::move(clock)) {}

KeyResolver::KeyResolver(const xkb_rule_names& names, Clock clock)
    : KeyResolver(&names, std::move(clock)) {}

KeyResolver::KeyResolver(const xkb_rule_names* names, Clock clock)
    : m_context(xkb_context_new(XKB_CONTEXT_NO_FLAGS)), m_clock(std::move(clock)),
      m_budgetTimeMs(m_clock()), m_replacementTokens(security::kKeymapReplacementBurst) {
    if (!m_context)
        return;
    m_keymap.reset(xkb_keymap_new_from_names(m_context.get(), names, XKB_KEYMAP_COMPILE_NO_FLAGS));
    if (!m_keymap)
        return;
    m_shiftIndex = xkb_keymap_mod_get_index(m_keymap.get(), XKB_MOD_NAME_SHIFT);
    m_baseText = keymapText();
    for (auto code = std::max(9u, xkb_keymap_min_keycode(m_keymap.get()));
         code <= std::min(KEY_MAX + 8u, xkb_keymap_max_keycode(m_keymap.get())) && m_slots.size() < kMaxExtraKeysyms; ++code) {
        bool used = false;
        for (xkb_layout_index_t layout = 0; layout < xkb_keymap_num_layouts_for_key(m_keymap.get(), code); ++layout) {
            for (xkb_level_index_t level = 0; level < xkb_keymap_num_levels_for_key(m_keymap.get(), code, layout); ++level) {
                const xkb_keysym_t* syms;
                const int count = xkb_keymap_key_get_syms_by_level(m_keymap.get(), code, layout, level, &syms);
                for (int i = 0; i < count; ++i)
                    used |= syms[i] != XKB_KEY_NoSymbol;
            }
        }
        if (!used) {
            const char* name = xkb_keymap_key_get_name(m_keymap.get(), code);
            m_slots.push_back({code, name ? name : "H" + std::to_string(code), !name});
        }
    }
}

KeyResolver::~KeyResolver() = default;

bool KeyResolver::valid() const {
    return m_context && m_keymap;
}

std::string KeyResolver::keymapText() const {
    if (!valid())
        return {};
    char* text = xkb_keymap_get_as_string(m_keymap.get(), XKB_KEYMAP_FORMAT_TEXT_V1);
    const std::string result = text ? text : "";
    free(text);
    return result;
}

xkb_state* KeyResolver::createState(const std::set<std::uint32_t>& held, xkb_state* previous) const {
    auto* state = valid() ? xkb_state_new(m_keymap.get()) : nullptr;
    if (!state)
        return nullptr;
    for (const auto code : held)
        xkb_state_update_key(state, code + 8, XKB_KEY_DOWN);
    if (previous) {
        xkb_state_update_mask(state, xkb_state_serialize_mods(state, XKB_STATE_MODS_DEPRESSED),
                              xkb_state_serialize_mods(previous, XKB_STATE_MODS_LATCHED),
                              xkb_state_serialize_mods(previous, XKB_STATE_MODS_LOCKED),
                              xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_DEPRESSED),
                              xkb_state_serialize_layout(previous, XKB_STATE_LAYOUT_LATCHED),
                              xkb_state_serialize_layout(previous, XKB_STATE_LAYOUT_LOCKED));
    }
    return state;
}

std::optional<ResolvedKey> KeyResolver::resolveWithMask(xkb_keysym_t keysym, xkb_mod_mask_t mask) const {
    XkbStatePtr state(xkb_state_new(m_keymap.get()));
    if (!state)
        return std::nullopt;
    ResolvedKey result;
    result.modifierMask = mask;
    result.needsShift = m_shiftIndex < 32 && (mask & (1u << m_shiftIndex));
    auto remaining = mask;
    // Discover momentary modifier keys from their actual XKB actions. Never
    // assume that Right Alt selects level 3, or synthesize lock/latch keys.
    for (auto code = xkb_keymap_min_keycode(m_keymap.get()); remaining && code <= xkb_keymap_max_keycode(m_keymap.get()); ++code) {
        XkbStatePtr probe(xkb_state_new(m_keymap.get()));
        if (!probe || code < 9 || code > KEY_MAX + 8u)
            continue;
        xkb_state_update_key(probe.get(), code, XKB_KEY_DOWN);
        const auto mods = xkb_state_serialize_mods(probe.get(), XKB_STATE_MODS_DEPRESSED);
        if (!mods || (mods & ~mask) || !(mods & remaining) ||
            (xkb_state_serialize_mods(probe.get(), XKB_STATE_MODS_LATCHED) ||
             xkb_state_serialize_mods(probe.get(), XKB_STATE_MODS_LOCKED)) ||
            xkb_state_serialize_layout(probe.get(), XKB_STATE_LAYOUT_EFFECTIVE) != 0)
            continue;
        result.modifierKeycodes.push_back(code - 8);
        remaining &= ~mods;
        xkb_state_update_key(state.get(), code, XKB_KEY_DOWN);
    }
    if (remaining)
        return std::nullopt;
    for (auto code = std::max(9u, xkb_keymap_min_keycode(m_keymap.get()));
         code <= std::min(KEY_MAX + 8u, xkb_keymap_max_keycode(m_keymap.get())); ++code) {
        if (xkb_state_key_get_one_sym(state.get(), code) == keysym) {
            result.evdevKeycode = code - 8;
            return result;
        }
    }
    return std::nullopt;
}

std::optional<ResolvedKey> KeyResolver::resolveKeysym(xkb_keysym_t keysym) const {
    if (!valid() || keysym == XKB_KEY_NoSymbol)
        return std::nullopt;
    if (auto key = resolveWithMask(keysym, 0))
        return key;
    const auto shift = m_shiftIndex < 32 ? 1u << m_shiftIndex : 0;
    if (shift) {
        if (auto key = resolveWithMask(keysym, shift))
            return key;
    }
    std::vector<xkb_mod_mask_t> masks;
    for (auto code = xkb_keymap_min_keycode(m_keymap.get()); code <= xkb_keymap_max_keycode(m_keymap.get()); ++code) {
        for (xkb_level_index_t level = 0; level < xkb_keymap_num_levels_for_key(m_keymap.get(), code, 0); ++level) {
            const xkb_keysym_t* syms;
            const int count = xkb_keymap_key_get_syms_by_level(m_keymap.get(), code, 0, level, &syms);
            if (count != 1 || syms[0] != keysym)
                continue;
            std::array<xkb_mod_mask_t, 256> levelMasks{};
            const auto n = xkb_keymap_key_get_mods_for_level(m_keymap.get(), code, 0, level, levelMasks.data(), levelMasks.size());
            masks.insert(masks.end(), levelMasks.begin(), levelMasks.begin() + n);
        }
    }
    std::sort(masks.begin(), masks.end(), [](auto a, auto b) { return std::popcount(a) < std::popcount(b); });
    masks.erase(std::unique(masks.begin(), masks.end()), masks.end());
    for (auto mask : masks) {
        if (auto key = resolveWithMask(keysym, mask))
            return key;
    }
    return std::nullopt;
}

bool KeyResolver::takeReplacementToken() {
    const auto now = std::max(m_budgetTimeMs, m_clock());
    m_replacementTokens = std::min(double(security::kKeymapReplacementBurst),
        m_replacementTokens + (now - m_budgetTimeMs) * (security::kKeymapReplacementsPerSecond / 1000.0));
    m_budgetTimeMs = now;
    if (m_replacementTokens >= 1.0) {
        m_replacementTokens -= 1.0;
        return true;
    }
    if (!m_lastBudgetWarningMs || now - *m_lastBudgetWarningMs >= security::kKeymapWarningIntervalMs) {
        qWarning() << "keymap replacement rate limit reached; dropping unmapped symbol";
        m_lastBudgetWarningMs = now;
    }
    return false;
}

std::optional<ResolvedKey> KeyResolver::resolveOrExtend(xkb_keysym_t keysym, const std::set<std::uint32_t>& held) {
    if (!valid() || !keysym || keysym > XKB_KEYSYM_MAX)
        return std::nullopt;
    if (auto key = resolveKeysym(keysym))
        return key;
    for (std::size_t i = 0; i < m_slots.size(); ++i) {
        const auto index = (m_nextSlot + i) % m_slots.size();
        if (held.contains(m_slots[index].code - 8))
            continue;
        // Charge compilation attempts, never lookups of already mapped symbols.
        if (!takeReplacementToken())
            return std::nullopt;
        auto candidateSlots = m_slots;
        candidateSlots[index].symbol = keysym;
        std::string text = m_baseText;
        std::string names, symbols;
        for (const auto& slot : candidateSlots) {
            if (!slot.symbol)
                continue;
            if (slot.needsName)
                names += "    <" + slot.name + "> = " + std::to_string(slot.code) + ";\n";
            std::ostringstream entry;
            entry << "    replace key <" << slot.name << "> { type=\"ONE_LEVEL\", [ 0x" << std::hex << slot.symbol << " ] };\n";
            symbols += entry.str();
        }
        const auto codesEnd = text.find("\n};", text.find("xkb_keycodes"));
        if (codesEnd == std::string::npos)
            return std::nullopt;
        text.insert(codesEnd, "\n" + names);
        const auto symbolsEnd = text.find("\n};", text.find("xkb_symbols"));
        if (symbolsEnd == std::string::npos)
            return std::nullopt;
        text.insert(symbolsEnd, "\n" + symbols);
        XkbKeymapPtr candidate(xkb_keymap_new_from_string(m_context.get(), text.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS));
        if (!candidate)
            return std::nullopt;
        const xkb_keysym_t* syms;
        if (xkb_keymap_key_get_syms_by_level(candidate.get(), candidateSlots[index].code, 0, 0, &syms) != 1 || syms[0] != keysym)
            return std::nullopt;
        m_keymap = std::move(candidate);
        m_slots = std::move(candidateSlots);
        m_nextSlot = (index + 1) % m_slots.size();
        ++m_revision;
        return ResolvedKey{.evdevKeycode = m_slots[index].code - 8, .modifierKeycodes = {}};
    }
    return std::nullopt;
}

} // namespace hkcf
