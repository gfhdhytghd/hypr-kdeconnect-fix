#pragma once
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

