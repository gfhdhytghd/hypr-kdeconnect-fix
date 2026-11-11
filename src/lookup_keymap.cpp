#include "lookup_keymap.hpp"
#include "security_policy.hpp"

#include <QChar>
#include <cstdio>
#include <set>
#include <vector>

namespace hkcf {

std::optional<LookupKeymap> makeLookupKeymap(xkb_keymap* base) {
    if (!base || xkb_keymap_min_keycode(base) < 8 || xkb_keymap_max_keycode(base) > KEY_MAX + 8)
        return std::nullopt;

    LookupKeymap result;
    for (const auto code : security::kKdeConnectRawKeycodes)
        result.rawKeycodes.insert(code);
    std::set<xkb_keysym_t> seen;
    std::vector<xkb_keysym_t> higher;
    std::string codes, symbols;
    const auto append = [&](xkb_keycode_t code, xkb_keysym_t symbol) {
        const auto name = "K" + std::to_string(code);
        codes += "<" + name + "> = " + std::to_string(code) + ";\n";
        char value[16];
        std::snprintf(value, sizeof(value), "0x%08x", symbol);
        symbols += "key <" + name + "> { type=\"ONE_LEVEL\", [ " + value + " ] };\n";
    };
    for (auto code = xkb_keymap_min_keycode(base); code <= xkb_keymap_max_keycode(base); ++code) {
        for (xkb_layout_index_t group = 0; group < xkb_keymap_num_layouts_for_key(base, code); ++group) {
            for (xkb_level_index_t level = 0; level < xkb_keymap_num_levels_for_key(base, code, group); ++level) {
                const xkb_keysym_t* syms;
                const auto count = xkb_keymap_key_get_syms_by_level(base, code, group, level, &syms);
                for (int i = 0; i < count; ++i) {
                    if (syms[i] != XKB_KEY_NoSymbol) {
                        // KDE Connect requests characters, not keypad aliases
                        // or non-printable higher-level action symbols.
                        const auto cp = xkb_keysym_to_utf32(syms[i]);
                        if (cp)
                            higher.push_back(xkb_utf32_to_keysym(cp));
                    }
                }
            }
        }
        const xkb_keysym_t* syms;
        const auto count = xkb_keymap_key_get_syms_by_level(base, code, 0, 0, &syms);
        const auto symbol = count > 0 ? syms[0] : XKB_KEY_NoSymbol;
        // Keep character-bearing base keys. Non-text XF86/media/F-key slots
        // are reclaimed unless the raw-key reservation above protects them.
        if (!xkb_keysym_to_utf32(symbol))
            continue;
        if (code < 9)
            return std::nullopt;
        append(code, symbol);
        result.baseKeycodes.insert(code - 8);
        result.rawKeycodes.insert(code - 8);
        seen.insert(symbol);
    }

    // libeis validates evdev codes, not XKB codes: XKB = evdev + 8.
    // Reserve the highest code for KDE Connect's exclusive-max scan.
    constexpr xkb_keycode_t sentinel = KEY_MAX + 8;
    if (result.rawKeycodes.contains(sentinel - 8))
        return std::nullopt;
    std::vector<xkb_keycode_t> freeCodes;
    for (xkb_keycode_t code = 9; code < sentinel; ++code) {
        if (!result.rawKeycodes.contains(code - 8))
            freeCodes.push_back(code);
    }
    bool full = false;
    const auto place = [&](xkb_keysym_t symbol) {
        if (full || symbol == XKB_KEY_NoSymbol || seen.contains(symbol))
            return;
        const auto cp = xkb_keysym_to_utf32(symbol);
        if (cp && QChar::category(cp) == QChar::Other_NotAssigned)
            return;
        if (!security::isAllowedKeysym(symbol))
            return;
        if (result.symbols.size() == static_cast<qsizetype>(freeCodes.size())) {
            result.firstOmittedKeysym = symbol;
            result.firstOmittedCodepoint = cp;
            full = true;
            return;
        }
        seen.insert(symbol);
        const auto code = freeCodes[result.symbols.size()];
        append(code, symbol);
        result.symbols.insert(code - 8, symbol);
    };
    const auto range = [&](std::uint32_t first, std::uint32_t last) {
        for (auto cp = first; cp <= last && !full; ++cp) {
            if (QChar::category(cp) != QChar::Other_NotAssigned)
                place(xkb_utf32_to_keysym(cp));
        }
    };
    // Priority is intentional. Use the same canonical conversion as KDE
    // Connect; adding Unicode aliases separately would waste scarce keycodes.
    for (const auto symbol : higher)
        place(symbol);
    range(0x00a0, 0x00ff);
    range(0x0100, 0x017f);
    range(0x2010, 0x2027);
    range(0x2030, 0x203a);
    place(xkb_utf32_to_keysym(0x20ac));
    range(0x0400, 0x045f);
    range(0x0386, 0x03ce);
    range(0x20a0, 0x20c0);
    range(0x0218, 0x021b); // Romanian comma-below letters
    for (const auto cp : {0x01a1u, 0x01b0u, 0x01a0u, 0x01afu}) // Vietnamese horn letters
        place(xkb_utf32_to_keysym(cp));
    range(0x0180, 0x024f);

    codes += "<END> = " + std::to_string(sentinel) + ";\n";
    result.text = "xkb_keymap {\nxkb_keycodes { minimum = 8; maximum = " + std::to_string(sentinel) +
        ";\n" + codes + "};\nxkb_types { type \"ONE_LEVEL\" { modifiers=None; map[None]=Level1; }; };\n"
        "xkb_compatibility {};\nxkb_symbols {\n" + symbols + "};\n};\n";
    return result;
}

} // namespace hkcf
