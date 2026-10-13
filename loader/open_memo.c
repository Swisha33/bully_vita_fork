/* open_memo.c -- answer the game's repeat probes for files that are not there
 *
 * What the earlier rounds measured, in one session of ordinary play:
 *
 *   17620 opens, about 11900 of them for files that do not exist
 *   a failed open costs what a successful one does, about 3.5 ms
 *
 * So roughly forty seconds a session go to the card saying "no" to questions
 * it has already answered. The handle cache went after the other third -- the
 * opens that succeed -- and lost, because a first open tripled in cost
 * whenever it held descriptors, for reasons that were never found.
 *
 * This holds no descriptors at all. It remembers paths that were proven
 * missing, and answers a repeat read-only open of one with NULL without
 * asking the card. That is the whole feature. What makes it safe:
 *
 * - A failed open is not proof. It can also mean the descriptor table is full,
 *   which is exactly how an earlier round handed the game a NULL for a file
 *   that existed. So a failed read is followed by one sceIoGetstat, and the
 *   path is remembered only if that says ENOENT. Any other answer, including
 *   success, remembers nothing. That stat is paid once per distinct missing
 *   path, not once per probe.
 *
 * - The memo can only go stale if a file appears while the game runs. The game
 *   imports no rename or link, and the only way it creates a file is an open
 *   that can write -- which makes the memo forget that path, whether the open
 *   worked or not. A read that succeeds also forgets it. Mods are copied to the
 *   card with the game closed, and the memo starts empty every boot.
 *
 * - Full means pass-through. When the table or its string arena runs out, new
 *   paths are simply not remembered and go to the card as before.
 *
 * Off unless ux0:data/Bully/open_memo exists, so one build can be measured
 * both ways on the same route before anyone decides it should ship on.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "open_memo.h"

int traceLog(char *text, ...);

// sceIoGetstat's "no such file": the SCE errno space with ENOENT in it.
#ifndef OPEN_MEMO_ENOENT
#define OPEN_MEMO_ENOENT ((int)0x80010002)
#endif

// Power of two. Eight thousand slots at sixteen bytes is 128 KB, and the arena
// 512 KB, against roughly 140 MB of heap the loader carves out and never uses.
#ifndef OPEN_MEMO_SLOTS
#define OPEN_MEMO_SLOTS 8192
#endif
#ifndef OPEN_MEMO_ARENA
#define OPEN_MEMO_ARENA (512 * 1024)
#endif
// Kept below the slot count so a probe sequence always finds a free slot or a
// match quickly; past this the memo stops remembering rather than slowing down.
#define OPEN_MEMO_MAX_LIVE (OPEN_MEMO_SLOTS * 3 / 4)

typedef struct {
  uint32_t hash;    // 0 = never used
  uint16_t len;
  uint8_t  live;    // 0 = forgotten (a tombstone, so probe chains stay intact)
  uint8_t  pad;
  const char *path;
} MemoSlot;

static MemoSlot slots[OPEN_MEMO_SLOTS];
static char *arena;
static uint32_t arena_used;
static uint32_t live_count;

static SceKernelLwMutexWork memo_lock;
static int memo_enabled;

// Cumulative, like the census: the question is how many probes in a session
// never reached the card.
static uint32_t stat_answered;     // repeat probes answered from the memo
static uint32_t stat_remembered;   // distinct paths proven missing
static uint32_t stat_not_proven;   // failed opens whose stat did not say ENOENT
static uint32_t stat_forgotten;    // paths dropped because they may exist now
static uint32_t stat_full;         // paths not remembered for lack of room
static uint64_t stat_proof_us;     // time spent in the proving stats

// Bumped by every open that could create a file. A failed read proves its path
// missing with a stat taken outside the lock; if a writing open ran in the
// meantime, that proof may already be out of date, so it is thrown away.
static uint32_t write_generation;
// Writing opens between their before_open and their record. While any is in
// flight a file may be appearing, so no failed read may be remembered.
static uint32_t writes_in_flight;

void open_memo_init(int enabled) {
  if (!enabled)
    return;
  arena = malloc(OPEN_MEMO_ARENA);
  if (!arena)
    return;
  if (sceKernelCreateLwMutex(&memo_lock, "bully open memo", 0, 0, NULL) < 0) {
    free(arena);
    arena = NULL;
    return;
  }
  memo_enabled = 1;
}

// Only a mode that can do nothing but read may be answered or remembered.
// Anything with w, a or + can create the file, so it is never answered from
// the memo and always clears the path from it.
static int read_only(const char *mode) {
  if (!mode || mode[0] != 'r')
    return 0;
  for (const char *p = mode; *p; p++)
    if (*p == '+' || *p == 'w' || *p == 'a')
      return 0;
  return 1;
}

static uint32_t path_hash(const char *path, size_t *len_out) {
  uint32_t h = 0x811c9dc5;
  size_t len = 0;
  for (const char *p = path; *p; p++, len++) {
    h ^= (uint8_t)*p;
    h *= 0x01000193;
  }
  *len_out = len;
  return h ? h : 1;
}

// The slot holding this path, live or forgotten, or -1. Compares the whole
// string: a hash match alone would answer one file's absence for another.
static int find(const char *path, uint32_t hash, size_t len) {
  uint32_t mask = OPEN_MEMO_SLOTS - 1;
  for (uint32_t i = 0, at = hash & mask; i < OPEN_MEMO_SLOTS; i++, at = (at + 1) & mask) {
    MemoSlot *s = &slots[at];
    if (s->hash == 0)
      return -1;
    if (s->hash == hash && s->len == len && memcmp(s->path, path, len) == 0)
      return (int)at;
  }
  return -1;
}

static void remember(const char *path, uint32_t hash, size_t len) {
  int at = find(path, hash, len);
  if (at >= 0) {
    if (!slots[at].live) {
      slots[at].live = 1;
      live_count++;
      stat_remembered++;
    }
    return;
  }
  if (live_count >= OPEN_MEMO_MAX_LIVE || len > 0xffff || arena_used + len + 1 > OPEN_MEMO_ARENA) {
    stat_full++;
    return;
  }
  uint32_t mask = OPEN_MEMO_SLOTS - 1;
  for (uint32_t i = 0, pos = hash & mask; i < OPEN_MEMO_SLOTS; i++, pos = (pos + 1) & mask) {
    MemoSlot *s = &slots[pos];
    if (s->hash != 0)
      continue;
    char *copy = arena + arena_used;
    memcpy(copy, path, len + 1);
    arena_used += (uint32_t)(len + 1);
    s->path = copy;
    s->len = (uint16_t)len;
    s->live = 1;
    s->hash = hash;   // last, so a slot is never seen with a hash and no path
    live_count++;
    stat_remembered++;
    return;
  }
  stat_full++;
}

static void forget(const char *path, uint32_t hash, size_t len) {
  int at = find(path, hash, len);
  if (at >= 0 && slots[at].live) {
    slots[at].live = 0;
    live_count--;
    stat_forgotten++;
  }
}

int open_memo_before_open(const char *path, const char *mode) {
  if (!memo_enabled || !path)
    return 0;
  size_t len;
  uint32_t hash = path_hash(path, &len);
  if (!read_only(mode)) {
    // Forgotten before the file can exist, and again after (in record), so no
    // window opens in which the memo calls a file missing that the card has.
    sceKernelLockLwMutex(&memo_lock, 1, NULL);
    write_generation++;
    writes_in_flight++;
    forget(path, hash, len);
    sceKernelUnlockLwMutex(&memo_lock, 1);
    return 0;
  }
  sceKernelLockLwMutex(&memo_lock, 1, NULL);
  int at = find(path, hash, len);
  int missing = at >= 0 && slots[at].live;
  if (missing)
    stat_answered++;
  sceKernelUnlockLwMutex(&memo_lock, 1);
  if (missing)
    errno = ENOENT;
  return missing;
}

void open_memo_record(const char *path, const char *mode, int opened) {
  if (!memo_enabled || !path)
    return;
  size_t len;
  uint32_t hash = path_hash(path, &len);

  if (opened || !read_only(mode)) {
    sceKernelLockLwMutex(&memo_lock, 1, NULL);
    if (!read_only(mode)) {
      write_generation++;
      if (writes_in_flight)
        writes_in_flight--;
    }
    forget(path, hash, len);
    sceKernelUnlockLwMutex(&memo_lock, 1);
    return;
  }

  // A failed read: prove it before believing it. Outside the lock, because the
  // stat is the slow part and nothing it learns depends on the table.
  sceKernelLockLwMutex(&memo_lock, 1, NULL);
  uint32_t generation = write_generation;
  int writing = writes_in_flight != 0;
  sceKernelUnlockLwMutex(&memo_lock, 1);
  SceUInt64 t0 = sceKernelGetProcessTimeWide();
  SceIoStat st;
  int res = sceIoGetstat(path, &st);
  SceUInt64 took = sceKernelGetProcessTimeWide() - t0;

  sceKernelLockLwMutex(&memo_lock, 1, NULL);
  stat_proof_us += took;
  if (res == OPEN_MEMO_ENOENT && !writing && generation == write_generation)
    remember(path, hash, len);
  else
    stat_not_proven++;
  sceKernelUnlockLwMutex(&memo_lock, 1);
}

void open_memo_report(void) {
  if (!memo_enabled)
    return;
  sceKernelLockLwMutex(&memo_lock, 1, NULL);
  uint32_t answered = stat_answered, remembered = stat_remembered, live = live_count;
  uint32_t not_proven = stat_not_proven, forgotten = stat_forgotten, full = stat_full;
  uint32_t proof_ms = (uint32_t)(stat_proof_us / 1000);
  uint32_t used_kb = arena_used / 1024;
  sceKernelUnlockLwMutex(&memo_lock, 1);

  traceLog("open memo: %u probes answered, %u paths proven missing (%u held, %u KB), "
           "%u ms proving | %u failed opens not ENOENT, %u forgotten, %u not kept (full)\n",
           (unsigned)answered, (unsigned)remembered, (unsigned)live, (unsigned)used_kb,
           (unsigned)proof_ms, (unsigned)not_proven, (unsigned)forgotten, (unsigned)full);
}
