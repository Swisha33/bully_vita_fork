/* test_open_memo.c -- the open memo never says "missing" about a file that is there
 *
 * Everything this feature can get wrong is silent on the console: a NULL for a
 * file that exists looks like a missing model or a script that never runs. So
 * the tests drive it through a fake filesystem and check every answer it gives
 * against what a real open would have said, including the cases that sank the
 * handle cache: a failed open that was not ENOENT, and a file created while
 * the game runs.
 *
 * The table is shrunk to 64 slots so that running full and probing past
 * forgotten slots are exercised rather than assumed.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#define OPEN_MEMO_SLOTS 64
#define OPEN_MEMO_ARENA 4096
#include "../loader/open_memo.c"

#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

// ---- a filesystem of path strings, and the OS calls the memo makes ----------

#define FS_MAX 512
static char fs_paths[FS_MAX][128];
static int fs_count;
static pthread_mutex_t fs_lock = PTHREAD_MUTEX_INITIALIZER;
static int stat_calls;
static int stat_error_override;   // non-zero: getstat fails with this, whatever the truth

static int fs_has(const char *p) {
  for (int i = 0; i < fs_count; i++)
    if (strcmp(fs_paths[i], p) == 0)
      return 1;
  return 0;
}

static void fs_create(const char *p) {
  pthread_mutex_lock(&fs_lock);
  if (!fs_has(p) && fs_count < FS_MAX)
    snprintf(fs_paths[fs_count++], sizeof(fs_paths[0]), "%s", p);
  pthread_mutex_unlock(&fs_lock);
}

static void fs_reset(void) {
  pthread_mutex_lock(&fs_lock);
  fs_count = 0;
  stat_calls = 0;
  stat_error_override = 0;
  pthread_mutex_unlock(&fs_lock);
}

// Runs inside the next stat, after it has its answer: the moment where a
// writer on another thread can create the file the stat just called missing.
static void (*during_next_stat)(void);

int sceIoGetstat(const char *file, SceIoStat *out) {
  (void)out;
  void (*during)(void) = during_next_stat;
  during_next_stat = NULL;
  pthread_mutex_lock(&fs_lock);
  stat_calls++;
  int res = stat_error_override ? stat_error_override
                                : (fs_has(file) ? 0 : OPEN_MEMO_ENOENT);
  pthread_mutex_unlock(&fs_lock);
  if (during)
    during();
  return res;
}

int sceKernelCreateLwMutex(SceKernelLwMutexWork *work, const char *name, unsigned int attr,
                           int initCount, const SceKernelLwMutexOptParam *opt) {
  pthread_mutex_t *m = malloc(sizeof(*m));
  pthread_mutex_init(m, NULL);
  *(pthread_mutex_t **)work = m;
  return 0;
}
int sceKernelLockLwMutex(SceKernelLwMutexWork *work, int n, unsigned int *timeout) {
  pthread_mutex_lock(*(pthread_mutex_t **)work);
  return 0;
}
int sceKernelUnlockLwMutex(SceKernelLwMutexWork *work, int n) {
  pthread_mutex_unlock(*(pthread_mutex_t **)work);
  return 0;
}
SceUInt64 sceKernelGetProcessTimeWide(void) { return 0; }

static char last_report[512];
int traceLog(char *text, ...) {
  va_list ap;
  va_start(ap, text);
  vsnprintf(last_report, sizeof(last_report), text, ap);
  va_end(ap);
  return 0;
}

// ---- the game's fopen, the way main.c's counted_fopen does it ---------------

static int card_opens;   // opens that reached the card
static __thread int answered_by_memo;

static int game_open(const char *path, const char *mode) {
  answered_by_memo = 0;
  if (open_memo_before_open(path, mode)) {
    answered_by_memo = 1;
    assert(errno == ENOENT && "an answered probe looks like a real ENOENT to the game");
    return 0;
  }
  __sync_fetch_and_add(&card_opens, 1);
  int writes = mode[0] != 'r' || strchr(mode, '+');
  pthread_mutex_lock(&fs_lock);
  int exists = fs_has(path);
  pthread_mutex_unlock(&fs_lock);
  if (writes && !exists && mode[0] != 'r')
    fs_create(path), exists = 1;
  open_memo_record(path, mode, exists);
  return exists;
}

static int game_open(const char *path, const char *mode);
static void save_written_meanwhile(void) { game_open("ux0:data/Bully/BullyFile1", "wb"); }

// The truth, for comparing against.
static int really_there(const char *path) {
  pthread_mutex_lock(&fs_lock);
  int r = fs_has(path);
  pthread_mutex_unlock(&fs_lock);
  return r;
}

static void reset_all(void) {
  memset(slots, 0, sizeof(slots));
  arena_used = 0;
  live_count = 0;
  stat_answered = stat_remembered = stat_not_proven = stat_forgotten = stat_full = 0;
  card_opens = 0;
  write_generation = 0;
  writes_in_flight = 0;
  fs_reset();
}

#define BASE "ux0:data/Bully/"

// ---- the threaded test: answers checked against the truth under contention --

#define THREADS 8
#define ROUNDS 20000
static volatile int wrong_answers;

static void *hammer(void *arg) {
  unsigned seed = (unsigned)(uintptr_t)arg * 2654435761u;
  char path[96];
  for (int i = 0; i < ROUNDS; i++) {
    seed = seed * 1103515245u + 12345u;
    int which = (seed >> 8) % 40;
    snprintf(path, sizeof(path), BASE "probe/%02d.lur", which);
    // One in fifty is a save written by the game; everything else a read.
    const char *mode = ((seed >> 20) % 50 == 0) ? "wb" : "rb";
    // Files are only ever created here, never removed, so a file there before
    // the probe started is there for all of it. The memo saying "missing"
    // about one of those is the bug; the card racing a writer is not.
    int existed = really_there(path);
    int got = game_open(path, mode);
    if (!got && answered_by_memo && existed)
      __sync_fetch_and_add(&wrong_answers, 1);
  }
  return NULL;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);

  // Off: never answers, never stats.
  reset_all();
  assert(!open_memo_before_open(BASE "Scripts/x.lur", "rb") && "disabled answers nothing");
  open_memo_record(BASE "Scripts/x.lur", "rb", 0);
  assert(stat_calls == 0 && "disabled costs nothing");
  printf("off: answers nothing, stats nothing\n");

  open_memo_init(1);
  assert(memo_enabled);

  // A missing file: the first probe reaches the card and is proven once; every
  // repeat after that is answered without either.
  reset_all();
  for (int i = 0; i < 100; i++)
    assert(game_open(BASE "Scripts/missing.lur", "rb") == 0);
  assert(card_opens == 1 && "only the first probe reaches the card");
  assert(stat_calls == 1 && "and it is proven exactly once");
  assert(stat_answered == 99);
  printf("missing: 100 probes, 1 open, 1 stat\n");

  // A failed open that is not ENOENT -- descriptors exhausted, say -- is never
  // remembered. This is the case that handed the game a NULL for a real file.
  reset_all();
  fs_create(BASE "Scripts/real.lur");
  stat_error_override = (int)0x80010018;   // EMFILE
  open_memo_record(BASE "Scripts/real.lur", "rb", 0);
  stat_error_override = 0;
  assert(!open_memo_before_open(BASE "Scripts/real.lur", "rb") && "not proven, not remembered");
  assert(stat_not_proven == 1);
  // Nor is a failed open the stat contradicts outright.
  open_memo_record(BASE "Scripts/real.lur", "rb", 0);
  assert(!open_memo_before_open(BASE "Scripts/real.lur", "rb"));
  printf("unproven failures: never remembered\n");

  // The game creating a file: a write-mode open forgets the path, so the next
  // read reaches the card and finds it.
  reset_all();
  assert(game_open(BASE "BullyFile0", "rb") == 0);
  assert(open_memo_before_open(BASE "BullyFile0", "rb"));
  assert(game_open(BASE "BullyFile0", "wb") == 1);
  assert(!open_memo_before_open(BASE "BullyFile0", "rb") && "a write forgets the path");
  assert(game_open(BASE "BullyFile0", "rb") == 1 && "and the save is readable at once");
  printf("created at runtime: forgotten on the write, readable after\n");

  // The same, interleaved: a read fails, and while its stat is out proving
  // the file missing, a writer creates it. The stale proof must be dropped.
  reset_all();
  during_next_stat = save_written_meanwhile;
  assert(game_open(BASE "BullyFile1", "rb") == 0 && "the read itself ran before the write");
  assert(!open_memo_before_open(BASE "BullyFile1", "rb") && "a proof that a write overtook is not kept");
  assert(game_open(BASE "BullyFile1", "rb") == 1);
  // And a read that fails while a writer is between its two halves.
  reset_all();
  assert(!open_memo_before_open(BASE "BullyFile2", "wb"));   // writer started
  open_memo_record(BASE "BullyFile2", "rb", 0);             // a reader fails meanwhile
  assert(!open_memo_before_open(BASE "BullyFile2", "rb") && "nothing is remembered mid-write");
  fs_create(BASE "BullyFile2");
  open_memo_record(BASE "BullyFile2", "wb", 1);             // writer done
  assert(writes_in_flight == 0);
  printf("races: a write during a proof, and a read during a write, are both safe\n");

  // Only plain reads are answered.
  reset_all();
  assert(game_open(BASE "a.dat", "rb") == 0);
  assert(open_memo_before_open(BASE "a.dat", "r"));
  assert(!open_memo_before_open(BASE "a.dat", "r+"));
  assert(!open_memo_before_open(BASE "a.dat", "rb+"));
  assert(!open_memo_before_open(BASE "a.dat", "wb"));
  assert(!open_memo_before_open(BASE "a.dat", "ab"));
  assert(!open_memo_before_open(BASE "a.dat", ""));
  assert(!open_memo_before_open(NULL, "rb"));
  printf("modes: only read-only opens are answered\n");

  // The whole path is compared, not the hash or a prefix.
  reset_all();
  fs_create(BASE "Scripts/a.lurx");
  assert(game_open(BASE "Scripts/a.lur", "rb") == 0);
  assert(!open_memo_before_open(BASE "Scripts/a.lu", "rb"));
  assert(!open_memo_before_open(BASE "Scripts/a.lurx", "rb"));
  assert(!open_memo_before_open(BASE "scripts/a.lur", "rb"));
  printf("matching: exact paths only\n");

  // Full: past the live limit nothing more is remembered, and those paths
  // still get correct answers from the card.
  reset_all();
  char p[96];
  for (int i = 0; i < 200; i++) {
    snprintf(p, sizeof(p), BASE "full/%03d", i);
    assert(game_open(p, "rb") == 0);
  }
  assert(live_count == OPEN_MEMO_MAX_LIVE && "stops at the limit");
  assert(stat_full > 0);
  for (int i = 0; i < 200; i++) {
    snprintf(p, sizeof(p), BASE "full/%03d", i);
    assert(game_open(p, "rb") == 0);
  }
  // Forgotten slots stay in the chain: drop half, and the rest are still found.
  for (int i = 0; i < OPEN_MEMO_MAX_LIVE; i += 2) {
    snprintf(p, sizeof(p), BASE "full/%03d", i);
    game_open(p, "wb");
  }
  for (int i = 1; i < OPEN_MEMO_MAX_LIVE; i += 2) {
    snprintf(p, sizeof(p), BASE "full/%03d", i);
    assert(open_memo_before_open(p, "rb") && "a forgotten neighbour does not hide this one");
  }
  for (int i = 0; i < OPEN_MEMO_MAX_LIVE; i += 2) {
    snprintf(p, sizeof(p), BASE "full/%03d", i);
    assert(game_open(p, "rb") == 1 && "every created file opens");
  }
  printf("full: stops remembering, keeps answering correctly\n");

  // Eight threads, reads and saves interleaved: never a NULL for a file that is there.
  reset_all();
  pthread_t th[THREADS];
  for (int i = 0; i < THREADS; i++)
    pthread_create(&th[i], NULL, hammer, (void *)(uintptr_t)(i + 1));
  for (int i = 0; i < THREADS; i++)
    pthread_join(th[i], NULL);
  assert(wrong_answers == 0 && "no thread was ever told a real file is missing");
  assert(stat_answered > 0 && "and the memo did answer");
  printf("threads: %d probes, %u answered, %d reached the card, 0 wrong\n",
         THREADS * ROUNDS, (unsigned)stat_answered, card_opens);

  open_memo_report();
  assert(strstr(last_report, "probes answered") && "the heartbeat line is written");
  printf("report: %s", last_report);

  printf("open memo: all passed\n");
  return 0;
}
