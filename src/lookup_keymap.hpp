#pragma once

#include <QHash>
#include <QSet>
#include <cstdint>
#include <optional>
#include <string>
#include <xkbcommon/xkbcommon.h>

namespace hkcf {

// Immutable for the lifetime of one EIS keyboard. Keys are evdev (XKB - 8).
struct LookupKeymap {
    std::string text;
    QHash<std::uint32_t, xkb_keysym_t> symbols;
    QSet<std::uint32_t> baseKeycodes;
    QSet<std::uint32_t> rawKeycodes;
    std::optional<xkb_keysym_t> firstOmittedKeysym;
    std::uint32_t firstOmittedCodepoint = 0;
};

[[nodiscard]] std::optional<LookupKeymap> makeLookupKeymap(xkb_keymap* base);

} // namespace hkcf
