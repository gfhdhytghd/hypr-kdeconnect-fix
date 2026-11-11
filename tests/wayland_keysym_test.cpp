#include "wayland_test_harness.hpp"

bool failKeymapWrite = false;
int keymapWriteAttempts = 0;
extern "C" int __real_ftruncate(int, off_t);
extern "C" int __wrap_ftruncate(int fd, off_t size) {
  ++keymapWriteAttempts;
  if (failKeymapWrite) {
    errno = ENOSPC;
    return -1;
  }
  return __real_ftruncate(fd, size);
}
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  setenv("XKB_DEFAULT_LAYOUT", "pl", 1);
  setenv("XKB_DEFAULT_VARIANT", "", 1);
  setenv("XKB_DEFAULT_OPTIONS", "", 1);
  int sockets[2], control[2];
  check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0,
        "socketpair");
  check(pipe(control) == 0, "control pipe");
  auto *server = wl_display_create();
  wl_global_create(server, &wl_seat_interface, 9, nullptr, bindSeat);
  wl_global_create(server, &zwp_virtual_keyboard_manager_v1_interface, 1,
                   nullptr, bindKeyboard);
  wl_global_create(server, &zwlr_virtual_pointer_manager_v1_interface, 2,
                   nullptr, bindPointer);
  wl_client_create(server, sockets[0]);
  auto *stopSource =
      wl_event_loop_add_fd(wl_display_get_event_loop(server), control[0],
                           WL_EVENT_READABLE, stopServer, server);
  setenv("WAYLAND_SOCKET", std::to_string(sockets[1]).c_str(), 1);
  std::thread thread([&] { wl_display_run(server); });
  int result = 0;
  try {
    hkcf::WaylandInput input;
    check(input.ensureReady(), "isolated input ready");
    const auto sync = [&] {
      const int before = capture.frames;
      check(input.pointerMotion(0, 0), "barrier motion");
      QElapsedTimer timer;
      timer.start();
      while (capture.frames == before && timer.elapsed() < 3000)
        QThread::msleep(1);
      check(capture.frames > before, "Wayland event barrier");
    };
    const auto released = [&] {
      sync();
      std::lock_guard lock(capture.mutex);
      check(!capture.mods.empty() && capture.mods.back() == 0,
            "no synthesized Shift remains pressed");
    };
    for (int repeats : {1, 2, 5}) {
      for (int i = 0; i < repeats; ++i)
        check(input.keyboardKeysym('A', true), "uppercase press/repeat");
      check(input.keyboardKeysym('A', false), "uppercase release");
      released();
    }
    check(input.keyboardKeysym('A', true), "first shifted keysym");
    check(input.keyboardKeysym('B', true), "second shifted keysym");
    check(input.keyboardKeysym('A', true), "repeat first shifted keysym");
    check(input.keyboardKeysym('A', false), "release first shifted keysym");
    sync();
    {
      std::lock_guard lock(capture.mutex);
      check(capture.mods.back() != 0, "other shifted keysym retains Shift");
    }
    check(input.keyboardKeysym('B', false), "release final shifted keysym");
    released();
    for (int i = 0; i < 3; ++i) {
      check(input.keyboardKeysym('A', true), "balanced press");
      check(input.keyboardKeysym('A', false), "balanced release");
    }
    released();
    check(input.keyboardKeysym(XKB_KEY_aogonek, true), "AltGr press");
    check(input.keyboardKeysym(XKB_KEY_aogonek, true), "AltGr repeat");
    check(input.keyboardKeysym(XKB_KEY_eogonek, true), "overlapping AltGr press");
    check(input.keyboardKeysym(XKB_KEY_Aogonek, true), "Shift plus AltGr press");
    sync();
    {
      std::lock_guard lock(capture.mutex);
      check(capture.symbols.back() == XKB_KEY_Aogonek, "Shift plus AltGr selects Aogonek");
    }
    check(input.keyboardKeysym(XKB_KEY_Aogonek, false), "Shift plus AltGr release");
    check(input.keyboardKeysym(XKB_KEY_aogonek, false), "first AltGr release");
    sync();
    {
      std::lock_guard lock(capture.mutex);
      check(capture.mods.back() != 0, "remaining AltGr hold retains its modifier");
    }
    check(input.keyboardKeysym(XKB_KEY_eogonek, false), "last AltGr release");
    released();
    {
      hkcf::InputState state(input);
      int owner;
      check(state.keyboardKeycode(&owner, KEY_LEFTSHIFT, true), "Shift held before replacement");
      const auto attempts = keymapWriteAttempts;
      check(!state.keyboardKeysym(&owner, XKB_KEY_Greek_alpha, true), "held Shift refuses dynamic symbol");
      check(keymapWriteAttempts == attempts, "held Shift refusal does not attempt upload");
      state.releaseOwner(&owner);
      released();

      // A long session must not accumulate history that prevents replacement.
      for (int i = 0; i < 1000; ++i) {
        check(input.keyboardKeysym('a', true), "long session press");
        check(input.keyboardKeysym('a', false), "long session release");
        // Let the server drain the socket so flush never sees EAGAIN.
        if (i % 64 == 0)
          sync();
      }
      check(input.keyboardKeycode(KEY_CAPSLOCK, true), "lock Caps Lock press");
      check(input.keyboardKeycode(KEY_CAPSLOCK, false), "lock Caps Lock release");
      check(state.keyboardKeysym(&owner, XKB_KEY_Greek_alpha, true), "dynamic Unicode press with Caps Lock locked");
      sync();
      int maps;
      {
        std::lock_guard lock(capture.mutex);
        maps = capture.maps;
        check(!capture.mapError && maps == 2, "dynamic keymap compiles and uploads once");
        // Locked Caps Lock capitalizes the dynamic symbol, as on a real keyboard.
        check(capture.symbols.back() == XKB_KEY_Greek_ALPHA, "uploaded keymap maps Unicode");
        check(xkb_state_mod_name_is_active(capture.state, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED),
              "replacement preserves locked Caps Lock");
      }
      check(state.keyboardKeysym(&owner, XKB_KEY_Greek_alpha, true), "dynamic Unicode repeat");
      check(state.keyboardKeysym(&owner, XKB_KEY_Greek_alpha, false), "dynamic Unicode release");
      state.releaseOwner(&owner);
      check(input.keyboardKeycode(KEY_CAPSLOCK, true), "unlock Caps Lock press");
      check(input.keyboardKeycode(KEY_CAPSLOCK, false), "unlock Caps Lock release");
      released();
      {
        std::lock_guard lock(capture.mutex);
        check(!xkb_state_mod_name_is_active(capture.state, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED),
              "later Caps Lock press and release unlocks after replacement");
        check(capture.maps == maps, "mapped Unicode repeat and release do not upload");
      }
    }
    {
      hkcf::InputState state(input);
      int owner;
      int maps;
      std::size_t before;
      sync();
      {
        std::lock_guard lock(capture.mutex);
        maps = capture.maps;
        before = capture.keys.size();
      }
      const auto attempts = keymapWriteAttempts;
      failKeymapWrite = true;
      check(!state.keyboardKeysym(&owner, XKB_KEY_Cyrillic_ya, true), "failed upload rejects owned symbol press");
      check(state.keyboardKeysym(&owner, XKB_KEY_Cyrillic_ya, false), "failed press leaves no owned symbol to release");
      state.releaseOwner(&owner);
      released();
      for (std::uint32_t code = 1; code <= KEY_MAX; ++code)
        check(!input.isKeycodePressed(code), "failed upload leaves no key stuck");
      {
        std::lock_guard lock(capture.mutex);
        check(capture.maps == maps && capture.keys.size() == before, "failed upload sends neither map nor keys");
      }
      check(keymapWriteAttempts == attempts + 1, "failure attempts exactly one upload");
      // Presses and releases of mapped/raw keys never retry the failed map.
      check(input.keyboardKeysym('Z', true), "base symbol works while uploads fail");
      check(input.keyboardKeysym('Z', false), "base symbol releases while uploads fail");
      check(input.keyboardKeycode(KEY_B, true), "raw key works while uploads fail");
      check(input.keyboardKeycode(KEY_B, false), "raw key releases while uploads fail");
      check(keymapWriteAttempts == attempts + 1, "presses and releases do not retry uploads");
      failKeymapWrite = false;
      check(state.keyboardKeysym(&owner, XKB_KEY_Cyrillic_ya, true), "retry rolled back symbol");
      sync();
      {
        std::lock_guard lock(capture.mutex);
        check(capture.maps == maps + 1, "retry uploads the remapped symbol once");
        check(capture.symbols.back() == XKB_KEY_Cyrillic_ya, "retry maps the requested symbol");
      }
      check(state.keyboardKeysym(&owner, XKB_KEY_Cyrillic_ya, true), "retry symbol repeat");
      check(state.keyboardKeysym(&owner, XKB_KEY_Cyrillic_ya, false), "retry symbol release");
      state.releaseOwner(&owner);
      check(keymapWriteAttempts == attempts + 2, "retry maps once with no repeat/release upload");
      released();

      failKeymapWrite = true;
      check(!input.keyboardKeysym(XKB_KEY_Greek_beta, true), "failed upload rejects direct symbol press");
      check(input.keyboardKeysym(XKB_KEY_Greek_beta, false), "failed direct press leaves no hold");
      failKeymapWrite = false;
      check(input.keyboardKeysym(XKB_KEY_Greek_beta, true), "retry direct symbol after rollback");
      check(input.keyboardKeysym(XKB_KEY_Greek_beta, false), "release retried direct symbol");
      released();
      for (std::uint32_t code = 1; code <= KEY_MAX; ++code)
        check(!input.isKeycodePressed(code), "retry leaves no key stuck");
    }
    for (bool releaseRawFirst : {false, true}) {
      hkcf::InputState state(input);
      int rawOwner, symbolOwner;
      check(state.keyboardKeycode(&rawOwner, KEY_LEFTSHIFT, true),
            "raw owner Shift");
      check(state.keyboardKeycode(&rawOwner, KEY_A, true), "raw owner A");
      check(state.keyboardKeysym(&symbolOwner, 'A', true),
            "symbol owner uppercase A");
      check(state.keyboardKeysym(&symbolOwner, 'A', true),
            "symbol owner repeat");
      state.releaseOwner(releaseRawFirst ? &rawOwner : &symbolOwner);
      sync();
      {
        std::lock_guard lock(capture.mutex);
        check(capture.mods.back() != 0,
              "remaining owner keeps real XKB Shift depressed");
      }
      state.releaseOwner(releaseRawFirst ? &symbolOwner : &rawOwner);
      released();
    }
    {
      hkcf::InputState state(input);
      int owner;
      std::size_t before;
      {
        std::lock_guard lock(capture.mutex);
        before = capture.keys.size();
      }
      for (int i = 0; i < 3; ++i)
        check(state.keyboardKeysym(&owner, 'A', true),
              "owned uppercase repeat");
      check(state.keyboardKeysym(&owner, 'A', false),
            "owned uppercase release");
      released();
      {
        std::lock_guard lock(capture.mutex);
        const std::vector<Key> expected{{KEY_LEFTSHIFT, 1}, {KEY_A, 1},
                                        {KEY_A, 1},         {KEY_A, 1},
                                        {KEY_A, 0},         {KEY_LEFTSHIFT, 0}};
        check(capture.keys.size() == before + expected.size(),
              "owned keysym repeats reach Wayland");
        for (std::size_t i = 0; i < expected.size(); ++i)
          check(capture.keys[before + i].code == expected[i].code &&
                    capture.keys[before + i].state == expected[i].state,
                "owned keysym repeat event ordering");
      }
    }
    for (uint32_t modifier : {KEY_LEFTSHIFT, KEY_LEFTCTRL, KEY_LEFTALT}) {
      hkcf::InputState state(input);
      int owner;
      for (int i = 0; i < 3; ++i)
        check(state.keyboardKeycode(&owner, modifier, true),
              "owned modifier repeat");
      check(state.keyboardKeycode(&owner, modifier, false),
            "owned modifier release");
      released();
    }
    std::cout << "Shifted keysym repeat and release tests passed on isolated "
                 "Wayland server\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    result = 1;
  }
  check(write(control[1], "q", 1) == 1, "stop server");
  thread.join();
  wl_event_source_remove(stopSource);
  wl_display_destroy_clients(server);
  wl_display_destroy(server);
  if (capture.state) xkb_state_unref(capture.state);
  close(control[0]);
  close(control[1]);
  return result;
}
