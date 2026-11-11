#include "lookup_keymap.hpp"

#include <QChar>
#include <cstdlib>
#include <iostream>
#include <linux/input-event-codes.h>
#include <set>
#include <stdexcept>

namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
xkb_keymap* baseMap(xkb_context* context, unsigned count) {
    std::string codes, symbols;
    for (unsigned code = 9; code < 9 + count; ++code) {
        const auto name = "B" + std::to_string(code);
        codes += "<" + name + ">=" + std::to_string(code) + ";";
        symbols += "key <" + name + "> { [a, A] };";
    }
    const auto text = "xkb_keymap { xkb_keycodes { minimum=8; maximum=" + std::to_string(8 + count) +
        ";" + codes + "}; xkb_types { include \"complete\" }; xkb_compatibility {}; xkb_symbols {" + symbols + "}; };";
    return xkb_keymap_new_from_string(context, text.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
}
void test(xkb_context* context, unsigned baseCount) {
    auto* base = baseMap(context, baseCount);
    check(base, "synthetic reference layout compiles");
    const auto lookup = hkcf::makeLookupKeymap(base);
    xkb_keymap_unref(base);
    check(lookup.has_value(), "bounded lookup map");
    auto* compiled = xkb_keymap_new_from_string(context, lookup->text.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    check(compiled, "lookup map compiles");
    std::set<xkb_keysym_t> repertoire;
    for (const auto symbol : lookup->symbols) repertoire.insert(symbol);
    check(lookup->symbols.size() <= KEY_CNT - 2 - baseCount, "base keys and sentinel consume capacity");
    for (auto it = lookup->symbols.cbegin(); it != lookup->symbols.cend(); ++it) {
        check(it.key() < KEY_CNT && !lookup->rawKeycodes.contains(it.key()), "extras occupy only valid, unreserved holes");
        const auto cp = xkb_keysym_to_utf32(it.value());
        check(!cp || QChar::category(cp) != QChar::Other_NotAssigned, "no unassigned extra characters");
        const xkb_keysym_t* syms;
        check(xkb_keymap_key_get_syms_by_level(compiled, it.key() + 8, 0, 0, &syms) == 1 && syms[0] == it.value(),
              "extra level zero agrees with translation table");
    }
    for (auto code : lookup->baseKeycodes) {
        check(code < KEY_CNT, "valid base evdev code");
        const xkb_keysym_t* syms;
        check(xkb_keymap_key_get_syms_by_level(compiled, code + 8, 0, 0, &syms) == 1 && syms[0] == 'a', "base level zero preserved");
        check(xkb_keymap_num_levels_for_key(compiled, code + 8, 0) == 1, "base is single level");
    }
    check(xkb_keymap_max_keycode(compiled) - 8 < KEY_CNT, "sentinel is a valid evdev code");
    const xkb_keysym_t* last;
    check(!xkb_keymap_key_get_syms_by_level(compiled, xkb_keymap_max_keycode(compiled), 0, 0, &last), "sentinel has no symbol");
    if (baseCount == 1) {
        check(!lookup->firstOmittedKeysym, "sparse layout fits the complete chosen repertoire");
        for (const auto cp : {0x017cu, 0x20acu, 0x2014u, 0x0416u, 0x03b1u, 0x0219u, 0x021bu, 0x01a1u, 0x024fu})
            check(repertoire.contains(xkb_utf32_to_keysym(cp)), "all priority blocks fit sparse layout");
        check(!repertoire.contains(0x0100017c), "legacy alias is not duplicated as Unicode");
        check(!repertoire.contains(xkb_utf32_to_keysym(0x038b)), "unassigned Greek code point omitted");
    } else {
        check(lookup->symbols.size() == KEY_CNT - 2 - baseCount, "dense layout stops at capacity");
        check(lookup->firstOmittedKeysym && lookup->firstOmittedCodepoint, "overflow records first omitted point");
        check(repertoire.contains('A'), "layout higher levels have first priority");
        for (unsigned cp = 0xa0; cp <= 0xff; ++cp)
            check(repertoire.contains(xkb_utf32_to_keysym(cp)), "Latin-1 precedes later blocks");
        for (unsigned cp = 0x100; cp < lookup->firstOmittedCodepoint; ++cp)
            check(repertoire.contains(xkb_utf32_to_keysym(cp)), "Latin-A fills in order");
        check(!repertoire.contains(*lookup->firstOmittedKeysym), "first omitted point is absent");
        check(!repertoire.contains(xkb_utf32_to_keysym(0x20ac)), "overflow does not skip ahead to later priority blocks");
    }
    xkb_keymap_unref(compiled);
}
}
int main() {
    auto* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    try {
        check(context, "XKB context");
        test(context, 1);
        test(context, 600);
        xkb_context_unref(context);
        std::cout << "Sparse/dense lookup capacity, priority, canonicalization and sentinel checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        if (context) xkb_context_unref(context);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
