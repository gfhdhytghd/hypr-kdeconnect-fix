#include "key_resolver.hpp"
#include "security_policy.hpp"

#include <QDebug>
#include <cstdlib>
#include <iostream>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

int budgetWarnings = 0;
int heldWarnings = 0;
void countBudgetWarnings(QtMsgType type, const QMessageLogContext&, const QString& message) {
    if (type == QtWarningMsg && message.contains("keymap replacement rate limit"))
        ++budgetWarnings;
    if (type == QtWarningMsg && message.contains("keymap replacement blocked by held"))
        ++heldWarnings;
}

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
    std::int64_t now = 0;
    hkcf::KeyResolver resolver(names, [&] { return now; });
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
    std::int64_t gateNow = 0;
    auto groupNames = names;
    groupNames.layout = "us,pl,de";
    hkcf::KeyResolver gated(groupNames, [&] { return gateNow; });
    auto* current = gated.createState();
    const auto shiftMask = 1u << xkb_keymap_mod_get_index(gated.keymap(), XKB_MOD_NAME_SHIFT);
    const auto gateRevision = gated.revision();
    const auto heldHandler = qInstallMessageHandler(countBudgetWarnings);
    for (const auto code : {KEY_LEFTSHIFT, KEY_A, KEY_CAPSLOCK}) {
        for (int i = 0; i < 65; ++i)
            failures += expect(!gated.resolveOrExtend(XKB_KEY_Greek_alpha, {static_cast<std::uint32_t>(code)}, current),
                               "any held key refuses replacement");
    }
    failures += expect(!gated.resolveOrExtend(XKB_KEY_Greek_alpha, {KEY_LEFTSHIFT}),
                       "held key refuses replacement even without current state");
    failures += expect(gated.resolveOrExtend(XKB_KEY_a, {KEY_LEFTSHIFT}, current).has_value(),
                       "base symbols resolve even when replacement is blocked");
    // Each nonzero component must block independently, even without held keys.
    for (int component = 0; component < 4; ++component) {
        xkb_state_update_mask(current, component == 0 ? shiftMask : 0, component == 1 ? shiftMask : 0, 0,
                              component == 2 ? 1 : 0, component == 3 ? 1 : 0, 0);
        for (int i = 0; i < 65; ++i)
            failures += expect(!gated.resolveOrExtend(XKB_KEY_Greek_alpha, {}, current),
                               "depressed or latched modifiers/layout refuse replacement without held keys");
        failures += expect(gated.resolveOrExtend(XKB_KEY_a, {}, current).has_value(),
                           "base symbols resolve with nontrivial current state");
    }
    failures += expect(gated.revision() == gateRevision && gated.keymap() == xkb_state_get_keymap(current) &&
                       !gated.resolveKeysym(XKB_KEY_Greek_alpha), "refusal leaves keymap and revision unchanged");
    failures += expect(heldWarnings == 1, "state refusal warnings are rate limited");
    gateNow += hkcf::security::kKeymapWarningIntervalMs;
    failures += expect(!gated.resolveOrExtend(XKB_KEY_Greek_alpha, {}, current), "latch still blocks after warning interval");
    failures += expect(heldWarnings == 2, "state refusal warning resumes after its interval");
    xkb_state_update_mask(current, 0, 0, 0, 0, 0, 0);
    // Keep time fixed: all refused attempts must leave the full burst available.
    for (int i = 0; i < hkcf::security::kKeymapReplacementBurst; ++i)
        failures += expect(gated.resolveOrExtend(xkb_utf32_to_keysym(0x4e00 + i), {}, current).has_value(),
                           "refused attempts consume no replacement budget");
    failures += expect(!gated.resolveOrExtend(XKB_KEY_Greek_alpha, {}, current), "accepted compiles consume the full burst");
    qInstallMessageHandler(heldHandler);
    xkb_state_unref(current);

    hkcf::KeyResolver caps(names);
    auto* capsState = caps.createState();
    xkb_state_update_key(capsState, KEY_CAPSLOCK + 8, XKB_KEY_DOWN);
    xkb_state_update_key(capsState, KEY_CAPSLOCK + 8, XKB_KEY_UP);
    failures += expect(xkb_state_mod_name_is_active(capsState, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED),
                       "Caps Lock starts on");
    failures += expect(caps.resolveOrExtend(XKB_KEY_Greek_alpha, {}, capsState).has_value(),
                       "locked Caps Lock with nothing held permits replacement");
    auto* capsAfter = caps.createState(capsState);
    failures += expect(xkb_state_mod_name_is_active(capsAfter, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED),
                       "replacement preserves Caps Lock on");
    xkb_state_update_key(capsAfter, KEY_CAPSLOCK + 8, XKB_KEY_DOWN);
    xkb_state_update_key(capsAfter, KEY_CAPSLOCK + 8, XKB_KEY_UP);
    failures += expect(!xkb_state_mod_name_is_active(capsAfter, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED),
                       "later Caps Lock press and release unlocks the replacement state");
    xkb_state_unref(capsState);
    xkb_state_unref(capsAfter);

    hkcf::KeyResolver groups(groupNames);
    auto* lockedGroup = groups.createState();
    xkb_state_update_mask(lockedGroup, 0, 0, 0, 0, 0, 1);
    failures += expect(groups.resolveOrExtend(XKB_KEY_Greek_alpha, {}, lockedGroup).has_value(),
                       "locked layout permits replacement");
    auto* afterGroup = groups.createState(lockedGroup);
    failures += expect(xkb_state_serialize_layout(afterGroup, XKB_STATE_LAYOUT_LOCKED) == 1 &&
                       xkb_state_serialize_layout(afterGroup, XKB_STATE_LAYOUT_DEPRESSED) == 0 &&
                       xkb_state_serialize_layout(afterGroup, XKB_STATE_LAYOUT_LATCHED) == 0,
                       "replacement retains only locked layout");
    xkb_state_unref(lockedGroup);
    xkb_state_unref(afterGroup);

    std::set<std::uint32_t> usedCodes;
    for (std::size_t i = 0; i < hkcf::KeyResolver::kMaxExtraKeysyms + 5; ++i) {
        now += 1000;
        const auto key = resolver.resolveOrExtend(0x01000400 + i);
        failures += expect(key.has_value(), "released slots permit bounded cache replacement");
        if (key) usedCodes.insert(key->evdevKeycode);
    }
    failures += expect(!usedCodes.empty() && usedCodes.size() <= hkcf::KeyResolver::kMaxExtraKeysyms,
                       "cache reuses at most 64 slots");
    failures += expect(!resolver.resolveKeysym(XKB_KEY_aogonek), "cache evicts old unheld symbols");
    failures += expect(resolver.resolveOrExtend(XKB_KEYSYM_MAX).has_value(), "accept the full allowed keysym range");
    failures += expect(!resolver.resolveOrExtend(0) && !resolver.resolveOrExtend(XKB_KEYSYM_MAX + 1u), "reject invalid keysyms");

    hkcf::KeyResolver rollback(names);
    rollback.rollback();
    failures += expect(rollback.revision() == 0, "rollback without a replacement is harmless");
    const auto first = rollback.resolveOrExtend(XKB_KEY_Greek_alpha);
    const auto savedText = rollback.keymapText();
    auto* savedMap = xkb_keymap_ref(rollback.keymap());
    const auto savedRevision = rollback.revision();
    const auto failed = rollback.resolveOrExtend(XKB_KEY_Cyrillic_ya);
    failures += expect(first && failed && rollback.revision() == savedRevision + 1, "replacement advances revision");
    rollback.rollback();
    failures += expect(rollback.keymap() == savedMap && rollback.keymapText() == savedText &&
                       rollback.revision() == savedRevision && !rollback.resolveKeysym(XKB_KEY_Cyrillic_ya) &&
                       rollback.resolveKeysym(XKB_KEY_Greek_alpha).has_value(),
                       "rollback restores previous keymap, slots and revision");
    rollback.rollback();
    failures += expect(rollback.revision() == savedRevision, "rollback is one step only");
    const auto retry = rollback.resolveOrExtend(XKB_KEY_Cyrillic_ya);
    failures += expect(failed && retry && failed->evdevKeycode == retry->evdevKeycode &&
                       rollback.revision() == savedRevision + 1, "retry reuses restored next slot");
    xkb_keymap_unref(savedMap);

    hkcf::KeyResolver longSession(names);
    auto* sessionState = longSession.createState();
    for (int i = 0; i < 10000; ++i) {
        xkb_state_update_key(sessionState, KEY_LEFTSHIFT + 8, XKB_KEY_DOWN);
        xkb_state_update_key(sessionState, KEY_A + 8, XKB_KEY_DOWN);
        xkb_state_update_key(sessionState, KEY_A + 8, XKB_KEY_UP);
        xkb_state_update_key(sessionState, KEY_LEFTSHIFT + 8, XKB_KEY_UP);
    }
    failures += expect(longSession.resolveOrExtend(XKB_KEY_Greek_alpha, {}, sessionState).has_value(),
                       "many balanced key events still permit replacement");
    xkb_state_unref(sessionState);

    budgetWarnings = 0;
    const auto previousHandler = qInstallMessageHandler(countBudgetWarnings);
    now = 0;
    hkcf::KeyResolver limited(names, [&] { return now; });
    int accepted = 0;
    for (int i = 0; i < 65; ++i)
        accepted += limited.resolveOrExtend(xkb_utf32_to_keysym(0x4e00 + i)).has_value();
    failures += expect(accepted == hkcf::security::kKeymapReplacementBurst &&
                       limited.revision() == static_cast<unsigned>(accepted),
                       "65 absent Han symbols at one timestamp compile only the bounded burst");
    const auto lastMapped = xkb_utf32_to_keysym(0x4e00 + accepted - 1);
    for (int i = 0; i < 65; ++i)
        failures += expect(limited.resolveOrExtend(lastMapped).has_value(), "mapped slot works with exhausted budget");
    now = 100;
    for (int i = 0; i < 3; ++i)
        failures += expect(limited.resolveOrExtend(0x01006000 + i).has_value(), "100 ms refills three replacement tokens");
    failures += expect(!limited.resolveOrExtend(0x01006003), "mapped lookups neither consume nor refill budget");
    failures += expect(budgetWarnings == 1, "exhaustion warnings are rate limited within one second");
    now = 10000;
    accepted = 0;
    for (int i = 0; i < 65; ++i)
        accepted += limited.resolveOrExtend(0x01007000 + i).has_value();
    failures += expect(accepted == hkcf::security::kKeymapReplacementBurst, "idle refill is capped at the burst");

    failures += expect(budgetWarnings == 2, "exhaustion warning resumes after its interval");
    hkcf::KeyResolver charged(names, [&] { return now; });
    for (int i = 0; i < hkcf::security::kKeymapReplacementBurst; ++i) {
        failures += expect(charged.resolveOrExtend(XKB_KEY_Greek_alpha).has_value(), "compile before rollback uses one token");
        charged.rollback();
        failures += expect(charged.revision() == 0 && !charged.resolveKeysym(XKB_KEY_Greek_alpha),
                           "rollback of first replacement restores the base revision and slots");
    }
    failures += expect(!charged.resolveOrExtend(XKB_KEY_Greek_alpha), "rollback does not refund compile tokens");
    qInstallMessageHandler(previousHandler);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
