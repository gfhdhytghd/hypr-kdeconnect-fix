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
    hkcf::KeyResolver resolver;
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

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
