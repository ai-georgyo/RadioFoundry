// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The boot count under entropy_nonce's counter (entropy.h, docs/spec/crypto.md 8.3), kept in the settings store
// (settings_boot_count, docs/spec/settings-store.md 3). At start-up it is read, bumped and saved at once, before the
// radio core draws a nonce, and the pool is started on it. When the counter reaches its last quarter the radio core's
// tick stores the next count, and once the store has it the counter moves on to it. Plain functions over the
// settings store: no RTOS call of their own. Built with CONFIG_ENTROPY.
#ifndef RFOUNDRY_ENTROPY_BOOT_H
#define RFOUNDRY_ENTROPY_BOOT_H

#include <stdbool.h>
#include <stdint.h>
#include "../../hal/hal_system.h"

// Start-up, the task that saves the settings (app.c's start task), after settings_init and before the radio core
// starts: the stored count + 1 into the settings, saved there and then where the store can write (else by the next
// save the store makes: entropy_boot_poll sees it), then entropy_init with the board's system services (its hardware
// generator) and the chip's unique ID. sys may be NULL
void entropy_boot_start(const hal_system_t *sys, uint32_t now_ms);
// The radio core's task, on its tick: once the store has saved a count it didn't have at start-up (or the next
// one), entropy_rebase with it; when entropy_nonce_want_bump asks for it, the next count into the settings, saved by
// the store's next save. No flash write here: the settings task writes
void entropy_boot_poll(uint32_t now_ms);
// The count in force and whether the store has it (the debug link)
uint32_t entropy_boot_count(void);
bool entropy_boot_saved(void);

#endif
