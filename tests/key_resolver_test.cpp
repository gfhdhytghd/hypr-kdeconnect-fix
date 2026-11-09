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

std::array<std::uint32_t, 6> components(xkb_state* state) {
    return {xkb_state_serialize_mods(state, XKB_STATE_MODS_DEPRESSED),
            xkb_state_serialize_mods(state, XKB_STATE_MODS_LATCHED),
            xkb_state_serialize_mods(state, XKB_STATE_MODS_LOCKED),
            xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_DEPRESSED),
            xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_LATCHED),
            xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_LOCKED)};
}

void apply(xkb_state* state, hkcf::XkbStateHistory& history, std::uint32_t code, xkb_key_direction direction) {
    xkb_state_update_key(state, code + 8, direction);
    history.record(code, direction);
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
    auto* oldState = resolver.createState({KEY_LEFTSHIFT, KEY_RIGHTSHIFT});
    hkcf::XkbStateHistory shiftHistory;
    shiftHistory.reset({KEY_LEFTSHIFT, KEY_RIGHTSHIFT});
    apply(oldState, shiftHistory, KEY_CAPSLOCK, XKB_KEY_DOWN);
    apply(oldState, shiftHistory, KEY_CAPSLOCK, XKB_KEY_UP);
    const auto oldLocks = xkb_state_serialize_mods(oldState, XKB_STATE_MODS_LOCKED);
    failures += expect(resolver.resolveOrExtend(XKB_KEY_eogonek, {KEY_LEFTSHIFT, KEY_RIGHTSHIFT}, oldState, &shiftHistory).has_value(),
                       "extend while momentary modifiers are held");
    auto* newState = resolver.createState({KEY_LEFTSHIFT, KEY_RIGHTSHIFT}, oldState);
    failures += expect(xkb_state_serialize_mods(newState, XKB_STATE_MODS_DEPRESSED) != 0 &&
                       xkb_state_serialize_mods(newState, XKB_STATE_MODS_LOCKED) == oldLocks,
                       "replacement preserves held modifiers and Caps Lock");
    failures += expect(components(oldState) == components(newState), "held Shift replacement equals the real current state");
    xkb_state_update_key(oldState, KEY_LEFTSHIFT + 8, XKB_KEY_UP);
    xkb_state_update_key(newState, KEY_LEFTSHIFT + 8, XKB_KEY_UP);
    failures += expect(components(oldState) == components(newState), "first Shift release matches the original");
    failures += expect(xkb_state_serialize_mods(newState, XKB_STATE_MODS_DEPRESSED) != 0, "replacement keeps both Shift reference counts");
    xkb_state_update_key(oldState, KEY_RIGHTSHIFT + 8, XKB_KEY_UP);
    xkb_state_update_key(newState, KEY_RIGHTSHIFT + 8, XKB_KEY_UP);
    failures += expect(components(oldState) == components(newState), "last Shift release matches the original");
    failures += expect(xkb_state_serialize_mods(newState, XKB_STATE_MODS_DEPRESSED) == 0, "replacement permits balanced modifier releases");
    xkb_state_unref(oldState);
    xkb_state_unref(newState);

    {
        hkcf::KeyResolver shift(names);
        hkcf::XkbStateHistory history;
        auto* current = shift.createState({}, nullptr, &history);
        apply(current, history, KEY_LEFTSHIFT, XKB_KEY_DOWN);
        failures += expect(shift.resolveOrExtend(XKB_KEY_Greek_alpha, {KEY_LEFTSHIFT}, current, &history).has_value(),
                           "held plain Shift allows replacement");
        auto* rebuilt = shift.createState(history.heldKeys(), current);
        failures += expect(components(current) == components(rebuilt), "plain Shift rebuild equals original");
        xkb_state_update_key(current, KEY_LEFTSHIFT + 8, XKB_KEY_UP);
        xkb_state_update_key(rebuilt, KEY_LEFTSHIFT + 8, XKB_KEY_UP);
        failures += expect(components(current) == components(rebuilt), "plain Shift release equals original");
        xkb_state_unref(current);
        xkb_state_unref(rebuilt);
    }

    std::int64_t capsNow = 0;
    hkcf::KeyResolver caps(names, [&] { return capsNow; });
    auto* capsState = caps.createState({});
    hkcf::XkbStateHistory capsHistory;
    capsHistory.reset({});
    apply(capsState, capsHistory, KEY_CAPSLOCK, XKB_KEY_DOWN);
    apply(capsState, capsHistory, KEY_CAPSLOCK, XKB_KEY_UP);
    failures += expect(xkb_state_mod_name_is_active(capsState, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED),
                       "Caps Lock starts on");
    apply(capsState, capsHistory, KEY_CAPSLOCK, XKB_KEY_DOWN);
    const auto capsRevision = caps.revision();
    const auto heldHandler = qInstallMessageHandler(countBudgetWarnings);
    for (int i = 0; i < 65; ++i)
        failures += expect(!caps.resolveOrExtend(XKB_KEY_Greek_alpha, {KEY_CAPSLOCK}, capsState, &capsHistory),
                           "held Caps Lock refuses extension");
    failures += expect(caps.revision() == capsRevision && caps.keymap() == xkb_state_get_keymap(capsState) &&
                       !caps.resolveKeysym(XKB_KEY_Greek_alpha), "refusal leaves keymap and revision unchanged");
    failures += expect(caps.resolveOrExtend(XKB_KEY_a, {KEY_CAPSLOCK}, capsState, &capsHistory).has_value(),
                       "mapped symbols still resolve while Caps Lock is held");
    failures += expect(heldWarnings == 1, "held-key warnings are rate limited");
    capsNow += hkcf::security::kKeymapWarningIntervalMs;
    for (int i = 0; i < 65; ++i)
        failures += expect(!caps.resolveOrExtend(XKB_KEY_Greek_alpha, {KEY_CAPSLOCK}, capsState, &capsHistory),
                           "held Caps Lock still refuses extension after warning interval");
    failures += expect(heldWarnings == 2, "held-key warning resumes after its interval");
    apply(capsState, capsHistory, KEY_CAPSLOCK, XKB_KEY_UP);
    failures += expect(!xkb_state_mod_name_is_active(capsState, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED),
                       "original Caps Lock release turns it off after refused extension");
    // Keep time fixed: refused attempts must leave the entire burst available.
    failures += expect(caps.resolveOrExtend(XKB_KEY_Greek_alpha, {}, capsState, &capsHistory).has_value(), "released Caps Lock permits the refused symbol");
    for (int i = 1; i < hkcf::security::kKeymapReplacementBurst; ++i)
        failures += expect(caps.resolveOrExtend(xkb_utf32_to_keysym(0x4e00 + i), {}, capsState, &capsHistory).has_value(),
                           "release permits extension with the full replacement budget");
    auto* capsAfter = caps.createState({}, capsState);
    failures += expect(!xkb_state_mod_name_is_active(capsAfter, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED),
                       "replacement after release preserves Caps Lock off");
    qInstallMessageHandler(heldHandler);
    xkb_state_unref(capsState);
    xkb_state_unref(capsAfter);

    for (bool shiftedCaps : {true, false}) {
        auto actionNames = names;
        actionNames.layout = shiftedCaps ? "us" : "us,pl";
        actionNames.options = shiftedCaps ? "caps:escape_shifted_capslock" : "grp:alt_shift_toggle";
        std::int64_t actionNow = 0;
        hkcf::KeyResolver actions(actionNames, [&] { return actionNow; });
        auto* current = actions.createState({});
        hkcf::XkbStateHistory history;
        history.reset({});
        if (shiftedCaps) {
            apply(current, history, KEY_LEFTSHIFT, XKB_KEY_DOWN);
            apply(current, history, KEY_CAPSLOCK, XKB_KEY_DOWN);
            apply(current, history, KEY_CAPSLOCK, XKB_KEY_UP);
            apply(current, history, KEY_CAPSLOCK, XKB_KEY_DOWN);
        } else {
            // Press order matters: Alt precedes Shift although its evdev code is larger.
            apply(current, history, KEY_LEFTALT, XKB_KEY_DOWN);
            apply(current, history, KEY_LEFTSHIFT, XKB_KEY_DOWN);
        }
        auto* replay = history.replay(xkb_state_get_keymap(current));
        auto* rebuilt = actions.createState(history.heldKeys(), current);
        failures += expect(replay && components(replay) == components(current), "history reproduces current components");
        if (shiftedCaps) {
            failures += expect(components(current) == components(rebuilt), "shifted Caps mismatch only appears on release");
            xkb_state_update_key(rebuilt, KEY_CAPSLOCK + 8, XKB_KEY_UP);
            xkb_state_update_key(replay, KEY_CAPSLOCK + 8, XKB_KEY_UP);
            failures += expect(components(replay) != components(rebuilt), "shifted Caps release exposes hidden lock action");
        } else {
            auto* sorted = actions.createState({KEY_LEFTSHIFT, KEY_LEFTALT}, current);
            failures += expect(components(current) != components(sorted), "old sorted Alt-Shift replay changes depressed modifiers");
            failures += expect(components(current) == components(rebuilt), "actual Alt-Shift press order preserves all components");
            xkb_state_unref(sorted);
        }
        const auto heldKeys = history.heldKeys();
        const std::set<std::uint32_t> heldSet(heldKeys.begin(), heldKeys.end());
        const auto before = components(current);
        if (shiftedCaps) {
            for (int i = 0; i < 3; ++i)
                failures += expect(!actions.resolveOrExtend(XKB_KEY_Greek_alpha, heldSet, current, &history),
                                   "modifier-selected Caps action refuses replacement");
            failures += expect(actions.revision() == 0 && actions.keymap() == xkb_state_get_keymap(current) &&
                               components(current) == before, "refusal leaves the real state and map untouched");
        } else {
            failures += expect(actions.resolveOrExtend(XKB_KEY_Greek_alpha, heldSet, current, &history).has_value(),
                               "equivalent Alt-Shift replay permits replacement");
            xkb_state_unref(rebuilt);
            rebuilt = actions.createState(heldKeys, current);
        }
        xkb_state_unref(replay);
        replay = history.replay(xkb_state_get_keymap(current));
        const std::vector<std::uint32_t> releases = shiftedCaps ? std::vector<std::uint32_t>{KEY_CAPSLOCK, KEY_LEFTSHIFT}
                                                               : std::vector<std::uint32_t>{KEY_LEFTSHIFT, KEY_LEFTALT};
        for (auto code : releases) {
            apply(current, history, code, XKB_KEY_UP);
            xkb_state_update_key(replay, code + 8, XKB_KEY_UP);
            failures += expect(components(replay) == components(current), "exact history retains release-time action filters");
            if (!shiftedCaps) {
                xkb_state_update_key(rebuilt, code + 8, XKB_KEY_UP);
                failures += expect(components(rebuilt) == components(current), "Alt-Shift replacement releases match original");
            }
        }
        if (shiftedCaps)
            failures += expect(xkb_state_serialize_mods(current, XKB_STATE_MODS_LOCKED) == 0,
                               "shifted Caps release leaves the original lock off");
        for (int i = shiftedCaps ? 0 : 1; i < hkcf::security::kKeymapReplacementBurst; ++i)
            failures += expect(actions.resolveOrExtend(xkb_utf32_to_keysym(0x4e00 + i), {}, current, &history).has_value(),
                               "unsafe replacements consume no burst budget");
        xkb_state_unref(current);
        xkb_state_unref(replay);
        xkb_state_unref(rebuilt);
    }

    {
        hkcf::KeyResolver replayResolver(names);
        auto* current = replayResolver.createState({});
        hkcf::XkbStateHistory history;
        history.reset({});
        apply(current, history, KEY_CAPSLOCK, XKB_KEY_DOWN);
        apply(current, history, KEY_CAPSLOCK, XKB_KEY_UP);
        apply(current, history, KEY_LEFTSHIFT, XKB_KEY_DOWN);
        const auto uploadHeld = history.heldKeys();
        auto* uploaded = replayResolver.createState(uploadHeld, current, &history);
        xkb_state_unref(current);
        current = uploaded;
        apply(current, history, KEY_LEFTSHIFT, XKB_KEY_UP);
        auto* replay = history.replay(replayResolver.keymap());
        failures += expect(replay && components(replay) == components(current), "upload replay includes the exact mask operation");
        apply(current, history, KEY_CAPSLOCK, XKB_KEY_DOWN);
        xkb_state_update_key(replay, KEY_CAPSLOCK + 8, XKB_KEY_DOWN);
        apply(current, history, KEY_CAPSLOCK, XKB_KEY_UP);
        xkb_state_update_key(replay, KEY_CAPSLOCK + 8, XKB_KEY_UP);
        failures += expect(components(replay) == components(current), "post-upload replay retains lock release behavior");
        xkb_state_unref(replay);
        for (std::size_t i = 0; i <= hkcf::XkbStateHistory::kMaxEvents; ++i)
            apply(current, history, KEY_A, i % 2 ? XKB_KEY_UP : XKB_KEY_DOWN);
        const auto heldKeys = history.heldKeys();
        const std::set<std::uint32_t> heldSet(heldKeys.begin(), heldKeys.end());
        failures += expect(!history.replay(replayResolver.keymap()), "bounded history refuses an incomplete replay");
        failures += expect(!replayResolver.resolveOrExtend(XKB_KEY_Greek_alpha, heldSet, current, &history),
                           "history overflow blocks replacement");
        failures += expect(!replayResolver.resolveOrExtend(XKB_KEY_Greek_alpha, heldSet, current),
                           "current state without history blocks replacement");
        uploaded = replayResolver.createState(heldKeys, current, &history);
        xkb_state_unref(current);
        current = uploaded;
        failures += expect(replayResolver.resolveOrExtend(XKB_KEY_Greek_alpha, heldSet, current, &history).has_value(),
                           "upload resets bounded history and permits equivalent replacement");
        xkb_state_unref(current);
    }

    std::set<std::uint32_t> pinned;
    if (dynamic) pinned.insert(dynamic->evdevKeycode);
    if (const auto key = resolver.resolveKeysym(XKB_KEY_eogonek)) pinned.insert(key->evdevKeycode);
    for (std::size_t i = 0; i < hkcf::KeyResolver::kMaxExtraKeysyms + 5; ++i) {
        now += 1000;
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

    auto groupNames = names;
    groupNames.layout = "us,pl";
    groupNames.options = "grp:menu_switch";
    hkcf::KeyResolver groups(groupNames);
    // XKB <MENU> aliases <COMP>, evdev KEY_COMPOSE (not KEY_MENU).
    auto* beforeGroup = groups.createState({KEY_COMPOSE});
    failures += expect(xkb_state_serialize_layout(beforeGroup, XKB_STATE_LAYOUT_DEPRESSED) == 1,
                       "Menu temporarily selects the second group");
    hkcf::XkbStateHistory groupHistory;
    groupHistory.reset({KEY_COMPOSE});
    failures += expect(groups.resolveOrExtend(XKB_KEY_Greek_alpha, {KEY_COMPOSE}, beforeGroup, &groupHistory).has_value(),
                       "equivalent momentary group action permits replacement");
    xkb_state_update_key(beforeGroup, KEY_COMPOSE + 8, XKB_KEY_UP);
    failures += expect(groups.resolveOrExtend(XKB_KEY_Greek_alpha).has_value(), "extend after Menu release");
    auto* afterGroup = groups.createState({}, beforeGroup);
    failures += expect(xkb_state_serialize_layout(afterGroup, XKB_STATE_LAYOUT_EFFECTIVE) == 0,
                       "replacement after Menu release retains the first group");
    xkb_state_unref(beforeGroup);
    xkb_state_unref(afterGroup);

    groupNames.layout = "us,pl,de";
    hkcf::KeyResolver components(groupNames);
    auto* beforeComponents = components.createState({});
    const auto shiftMask = 1u << xkb_keymap_mod_get_index(components.keymap(), XKB_MOD_NAME_SHIFT);
    const auto capsMask = 1u << xkb_keymap_mod_get_index(components.keymap(), XKB_MOD_NAME_CAPS);
    xkb_state_update_mask(beforeComponents, 0, shiftMask, capsMask, 0, 1, 1);
    failures += expect(components.resolveOrExtend(XKB_KEY_Greek_alpha).has_value(), "extend with latches and locks");
    auto* afterComponents = components.createState({KEY_LEFTCTRL}, beforeComponents);
    failures += expect(xkb_state_serialize_layout(afterComponents, XKB_STATE_LAYOUT_LATCHED) == 1 &&
                       xkb_state_serialize_layout(afterComponents, XKB_STATE_LAYOUT_LOCKED) == 1 &&
                       xkb_state_serialize_mods(afterComponents, XKB_STATE_MODS_LATCHED) == shiftMask &&
                       xkb_state_serialize_mods(afterComponents, XKB_STATE_MODS_LOCKED) == capsMask &&
                       xkb_state_mod_name_is_active(afterComponents, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_DEPRESSED),
                       "replacement preserves separate layout/modifier latches and locks, replaying depressed keys");
    xkb_state_unref(beforeComponents);
    xkb_state_unref(afterComponents);

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
    qInstallMessageHandler(previousHandler);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
