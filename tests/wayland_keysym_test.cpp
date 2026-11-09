// Real WaylandInput against an in-process isolated protocol server.
#include "input_state.hpp"
#include "virtual-keyboard-unstable-v1-server-protocol.h"
#include "wayland_input.hpp"
#include "wlr-virtual-pointer-unstable-v1-server-protocol.h"
#include <QCoreApplication>
#include <QThread>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <linux/input-event-codes.h>
#include <mutex>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
struct Key {
  uint32_t code, state;
};
struct Capture {
  std::mutex mutex;
  std::vector<Key> keys;
  std::vector<uint32_t> mods;
  std::vector<xkb_keysym_t> symbols;
  xkb_state* state = nullptr;
  int maps = 0;
  bool mapError = false;
  uint32_t x = 0, y = 0, w = 0, h = 0;
  std::atomic<int> frames = 0;
};
Capture capture;
bool failKeymapWrite = false;
extern "C" int __real_ftruncate(int, off_t);
extern "C" int __wrap_ftruncate(int fd, off_t size) {
  if (failKeymapWrite) {
    errno = ENOSPC;
    return -1;
  }
  return __real_ftruncate(fd, size);
}
void destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
void keyboardMap(wl_client *, wl_resource *, uint32_t, int32_t fd, uint32_t size) {
  std::string text(size, '\0');
  const auto count = pread(fd, text.data(), size, 0);
  close(fd);
  auto* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  auto* map = count == size ? xkb_keymap_new_from_string(context, text.c_str(),
      XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS) : nullptr;
  std::lock_guard lock(capture.mutex);
  ++capture.maps;
  if (capture.state) xkb_state_unref(capture.state);
  capture.state = map ? xkb_state_new(map) : nullptr;
  capture.mapError |= !capture.state;
  if (map) xkb_keymap_unref(map);
  xkb_context_unref(context);
}
void keyboardKey(wl_client *, wl_resource *, uint32_t, uint32_t key,
                 uint32_t state) {
  std::lock_guard lock(capture.mutex);
  capture.keys.push_back({key, state});
  capture.symbols.push_back(capture.state ? xkb_state_key_get_one_sym(capture.state, key + 8) : XKB_KEY_NoSymbol);
}
void keyboardMods(wl_client *, wl_resource *, uint32_t d, uint32_t latched, uint32_t locked,
                  uint32_t group) {
  std::lock_guard lock(capture.mutex);
  capture.mods.push_back(d);
  if (capture.state) xkb_state_update_mask(capture.state, d, latched, locked, 0, 0, group);
}
const struct zwp_virtual_keyboard_v1_interface keyboardImpl = {
    keyboardMap, keyboardKey, keyboardMods, destroy};
void newKeyboard(wl_client *c, wl_resource *, wl_resource *, uint32_t id) {
  auto *r = wl_resource_create(c, &zwp_virtual_keyboard_v1_interface, 1, id);
  wl_resource_set_implementation(r, &keyboardImpl, nullptr, nullptr);
}
const struct zwp_virtual_keyboard_manager_v1_interface keyboardManagerImpl = {
    newKeyboard};
void bindKeyboard(wl_client *c, void *, uint32_t v, uint32_t id) {
  auto *r =
      wl_resource_create(c, &zwp_virtual_keyboard_manager_v1_interface, v, id);
  wl_resource_set_implementation(r, &keyboardManagerImpl, nullptr, nullptr);
}
void motion(wl_client *, wl_resource *, uint32_t, wl_fixed_t, wl_fixed_t) {}
void absolute(wl_client *, wl_resource *, uint32_t, uint32_t x, uint32_t y,
              uint32_t w, uint32_t h) {
  std::lock_guard lock(capture.mutex);
  capture.x = x;
  capture.y = y;
  capture.w = w;
  capture.h = h;
}
void button(wl_client *, wl_resource *, uint32_t, uint32_t, uint32_t) {}
void axis(wl_client *, wl_resource *, uint32_t, uint32_t, wl_fixed_t) {}
void frame(wl_client *, wl_resource *) { ++capture.frames; }
void source(wl_client *, wl_resource *, uint32_t) {}
void stop(wl_client *, wl_resource *, uint32_t, uint32_t) {}
void discrete(wl_client *, wl_resource *, uint32_t, uint32_t, wl_fixed_t,
              int32_t) {}
const struct zwlr_virtual_pointer_v1_interface pointerImpl = {
    motion, absolute, button, axis, frame, source, stop, discrete, destroy};
void newPointer(wl_client *c, wl_resource *, wl_resource *, uint32_t id) {
  auto *r = wl_resource_create(c, &zwlr_virtual_pointer_v1_interface, 2, id);
  wl_resource_set_implementation(r, &pointerImpl, nullptr, nullptr);
}
void newPointerOutput(wl_client *c, wl_resource *r, wl_resource *seat,
                      wl_resource *, uint32_t id) {
  newPointer(c, r, seat, id);
}
const struct zwlr_virtual_pointer_manager_v1_interface pointerManagerImpl = {
    newPointer, destroy, newPointerOutput};
void bindPointer(wl_client *c, void *, uint32_t v, uint32_t id) {
  auto *r =
      wl_resource_create(c, &zwlr_virtual_pointer_manager_v1_interface, v, id);
  wl_resource_set_implementation(r, &pointerManagerImpl, nullptr, nullptr);
}
void bindSeat(wl_client *c, void *, uint32_t v, uint32_t id) {
  auto *r = wl_resource_create(c, &wl_seat_interface, v, id);
  wl_resource_set_implementation(r, nullptr, nullptr, nullptr);
  wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_POINTER |
                                   WL_SEAT_CAPABILITY_KEYBOARD);
}

void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int stopServer(int fd, uint32_t, void *data) {
  char value;
  if (read(fd, &value, 1) == 1)
    wl_display_terminate(static_cast<wl_display *>(data));
  return 0;
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
      check(state.keyboardKeysym(&owner, 'A', true), "Shift held before replacement");
      check(state.keyboardKeysym(&owner, XKB_KEY_Greek_alpha, true), "dynamic Unicode press");
      sync();
      int maps;
      {
        std::lock_guard lock(capture.mutex);
        maps = capture.maps;
        check(!capture.mapError && maps == 2, "dynamic keymap compiles and uploads once");
        check(capture.symbols.back() == XKB_KEY_Greek_alpha, "uploaded keymap maps Unicode");
        check(capture.mods.back() != 0, "replacement preserves held Shift");
      }
      check(state.keyboardKeysym(&owner, XKB_KEY_Greek_alpha, true), "dynamic Unicode repeat");
      check(state.keyboardKeysym(&owner, XKB_KEY_Greek_alpha, false), "dynamic Unicode release");
      state.releaseOwner(&owner);
      released();
      {
        std::lock_guard lock(capture.mutex);
        check(capture.maps == maps, "mapped Unicode repeat and release do not upload");
      }
    }
    {
      hkcf::InputState state(input);
      int owner, otherOwner;
      check(state.keyboardKeycode(&owner, KEY_LEFTSHIFT, true), "Shift before failed map upload");
      check(input.resolveKeysym(XKB_KEY_Cyrillic_ya).has_value(), "pending keymap revision");
      sync();
      int maps;
      std::size_t before;
      {
        std::lock_guard lock(capture.mutex);
        maps = capture.maps;
        before = capture.keys.size();
      }
      failKeymapWrite = true;
      check(!state.keyboardKeycode(&owner, KEY_B, true), "pending map failure rejects press");
      check(!state.keyboardKeysym(&owner, XKB_KEY_Aogonek, true), "pending map failure rejects symbol press");
      check(!input.keyboardKeysym('Z', true), "pending map failure rejects direct symbol press");
      check(state.keyboardKeycode(&owner, KEY_LEFTSHIFT, false), "failed map upload must allow Shift release");
      released();
      {
        std::lock_guard lock(capture.mutex);
        check(capture.maps == maps && capture.keys.size() == before + 1 &&
              capture.keys.back().code == KEY_LEFTSHIFT && capture.keys.back().state == 0,
              "failed upload sends only the held key release using the old map");
      }
      // Leave the failed owner's logical lifetime active during these retries.
      failKeymapWrite = false;
      check(state.keyboardKeycode(&otherOwner, KEY_B, true), "another owner retries failed press");
      check(input.keyboardKeysym('Z', true), "retry direct symbol with Shift");
      sync();
      {
        std::lock_guard lock(capture.mutex);
        check(capture.maps == maps + 1, "release retained the pending revision for retry");
        check(capture.keys[before + 1].code == KEY_B && capture.keys[before + 1].state == 1,
              "failed press did not leave a phantom owner suppressing the next press");
        check(capture.symbols.back() == XKB_KEY_Z, "direct failed symbol retry restores Shift");
      }
      check(input.keyboardKeysym('Z', false), "release retried direct symbol");
      check(state.keyboardKeysym(&owner, XKB_KEY_Aogonek, true), "retry failed symbol with all modifiers");
      sync();
      {
        std::lock_guard lock(capture.mutex);
        check(capture.symbols.back() == XKB_KEY_Aogonek, "failed symbol retry restores both modifiers");
      }
      state.releaseOwner(&owner);
      state.releaseOwner(&otherOwner);
      released();
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
