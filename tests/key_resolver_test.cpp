#include "key_resolver.hpp"

#include <cstdlib>
#include <iostream>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

int expect(bool condition, const char* message) {
    if (condition)
        return 0;
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

} // namespace

int main() {
    // Keep this test independent of XKB_DEFAULT_* and the host's keyboard
    // configuration. The production constructor intentionally uses those
    // defaults, while these expected evdev positions describe a US keymap.
    const xkb_rule_names names{
        .rules = "evdev",
        .model = "pc105",
        .layout = "us",
        .variant = "",
        .options = "",
    };
    hkcf::KeyResolver resolver(names);
    int failures = 0;

    failures += expect(resolver.valid(), "resolver should initialize");

    const auto lowerA = resolver.resolveKeysym(XKB_KEY_a);
    failures += expect(lowerA.has_value(), "lowercase a should resolve");
    if (lowerA)
        failures += expect(lowerA->evdevKeycode == KEY_A && !lowerA->needsShift, "lowercase a should map to KEY_A without shift");

    const auto upperA = resolver.resolveKeysym(XKB_KEY_A);
    failures += expect(upperA.has_value(), "uppercase A should resolve");
    if (upperA)
        failures += expect(upperA->evdevKeycode == KEY_A && upperA->needsShift, "uppercase A should map to KEY_A with shift");

    const auto exclamation = resolver.resolveKeysym(XKB_KEY_exclam);
    failures += expect(exclamation.has_value(), "exclamation mark should resolve");
    if (exclamation)
        failures += expect(exclamation->needsShift, "exclamation mark should need shift on the default keymap");

    auto polishNames = names;
    polishNames.layout = "pl";
    hkcf::KeyResolver polish(polishNames);
    const auto ogonek = polish.resolveKeysym(XKB_KEY_aogonek);
    failures += expect(ogonek && ogonek->evdevKeycode == KEY_A && !ogonek->needsShift &&
                       ogonek->modifierMask && !ogonek->modifierKeycodes.empty(), "Polish aogonek needs level 3 on KEY_A");
    const auto upperOgonek = polish.resolveKeysym(XKB_KEY_Aogonek);
    failures += expect(upperOgonek && upperOgonek->evdevKeycode == KEY_A && upperOgonek->needsShift &&
                       upperOgonek->modifierKeycodes.size() == 2, "Polish Aogonek needs Shift and level 3");
    for (const xkb_keysym_t symbol : {XKB_KEY_aogonek, XKB_KEY_Aogonek}) {
        const auto key = polish.resolveKeysym(symbol);
        auto* state = xkb_state_new(polish.keymap());
        if (key) {
            for (const auto code : key->modifierKeycodes)
                xkb_state_update_key(state, code + 8, XKB_KEY_DOWN);
            failures += expect(xkb_state_key_get_one_sym(state, key->evdevKeycode + 8) == symbol,
                               "discovered modifier keys actually select the requested Polish symbol");
        }
        xkb_state_unref(state);
    }
    // Move level 3 away from Right Alt. Resolution must follow XKB actions.
    polishNames.options = "lv3:lalt_switch";
    hkcf::KeyResolver leftAlt(polishNames);
    const auto moved = leftAlt.resolveKeysym(XKB_KEY_aogonek);
    failures += expect(moved && moved->modifierKeycodes == std::vector<std::uint32_t>{KEY_LEFTALT},
                       "level 3 follows the configured modifier key");

    failures += expect(!resolver.resolveKeysym(XKB_KEY_aogonek), "US has no aogonek before extension");
    auto* base = xkb_keymap_ref(resolver.keymap());
    const auto dynamic = resolver.resolveOrExtend(XKB_KEY_aogonek);
    if (dynamic) {
        const auto code = dynamic->evdevKeycode + 8;
        const xkb_keysym_t* symbols;
        const auto count = xkb_keymap_key_get_syms_by_level(base, code, 0, 0, &symbols);
        failures += expect(code >= xkb_keymap_min_keycode(base) && code <= xkb_keymap_max_keycode(base) &&
                           (count == 0 || (count == 1 && symbols[0] == XKB_KEY_NoSymbol)),
                           "dynamic slot is an unused keycode inside the original keymap range");
    }
    xkb_keymap_unref(base);
    failures += expect(dynamic && dynamic->modifierKeycodes.empty(), "dynamic aogonek needs no modifiers");
    const auto revision = resolver.revision();
    const auto reused = resolver.resolveOrExtend(XKB_KEY_aogonek);
    failures += expect(dynamic && reused && dynamic->evdevKeycode == reused->evdevKeycode && resolver.revision() == revision,
                       "repeat resolution reuses slot without keymap replacement");
    auto* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    auto* compiled = xkb_keymap_new_from_string(context, resolver.keymapText().c_str(), XKB_KEYMAP_FORMAT_TEXT_V1,
                                               XKB_KEYMAP_COMPILE_NO_FLAGS);
    failures += expect(compiled, "extended serialized keymap compiles");
    if (compiled && dynamic) {
        auto* state = xkb_state_new(compiled);
        failures += expect(xkb_state_key_get_one_sym(state, dynamic->evdevKeycode + 8) == XKB_KEY_aogonek,
                           "extended keymap maps the allocated keycode to aogonek");
        xkb_state_unref(state);
    }
    if (compiled) xkb_keymap_unref(compiled);
    xkb_context_unref(context);
    auto* oldState = resolver.createState({KEY_LEFTSHIFT, KEY_RIGHTSHIFT});
    xkb_state_update_key(oldState, KEY_CAPSLOCK + 8, XKB_KEY_DOWN);
    xkb_state_update_key(oldState, KEY_CAPSLOCK + 8, XKB_KEY_UP);
    const auto oldLocks = xkb_state_serialize_mods(oldState, XKB_STATE_MODS_LOCKED);
    failures += expect(resolver.resolveOrExtend(XKB_KEY_eogonek).has_value(), "extend while modifiers are held");
    auto* newState = resolver.createState({KEY_LEFTSHIFT, KEY_RIGHTSHIFT}, oldState);
    failures += expect(xkb_state_serialize_mods(newState, XKB_STATE_MODS_DEPRESSED) != 0 &&
                       xkb_state_serialize_mods(newState, XKB_STATE_MODS_LOCKED) == oldLocks,
                       "replacement preserves held modifiers and Caps Lock");
    xkb_state_update_key(newState, KEY_LEFTSHIFT + 8, XKB_KEY_UP);
    failures += expect(xkb_state_serialize_mods(newState, XKB_STATE_MODS_DEPRESSED) != 0, "replacement keeps both Shift reference counts");
    xkb_state_update_key(newState, KEY_RIGHTSHIFT + 8, XKB_KEY_UP);
    failures += expect(xkb_state_serialize_mods(newState, XKB_STATE_MODS_DEPRESSED) == 0, "replacement permits balanced modifier releases");
    xkb_state_unref(oldState);
    xkb_state_unref(newState);
    std::set<std::uint32_t> pinned;
    if (dynamic) pinned.insert(dynamic->evdevKeycode);
    if (const auto key = resolver.resolveKeysym(XKB_KEY_eogonek)) pinned.insert(key->evdevKeycode);
    for (std::size_t i = 0; i < hkcf::KeyResolver::kMaxExtraKeysyms + 5; ++i) {
        const auto key = resolver.resolveOrExtend(0x01000400 + i, pinned);
        if (key) pinned.insert(key->evdevKeycode);
    }
    failures += expect(pinned.size() <= hkcf::KeyResolver::kMaxExtraKeysyms, "cache has at most 64 slots");
    const auto fullRevision = resolver.revision();
    failures += expect(!resolver.resolveOrExtend(0x01002001, pinned) && resolver.revision() == fullRevision,
                       "all held slots refuse eviction without changing the keymap");
    failures += expect(resolver.resolveKeysym(XKB_KEY_aogonek).has_value(), "held aogonek survives cache pressure");
    failures += expect(resolver.resolveOrExtend(0x01002001).has_value(), "released slot can be reused");
    failures += expect(resolver.resolveOrExtend(XKB_KEYSYM_MAX).has_value(), "accept the full allowed keysym range");
    failures += expect(!resolver.resolveOrExtend(0) && !resolver.resolveOrExtend(XKB_KEYSYM_MAX + 1u), "reject invalid keysyms");

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
