/* open_memo.h -- answer the game's repeat probes for files that are not there
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __OPEN_MEMO_H__
#define __OPEN_MEMO_H__

// Turns the memo on or off. Call once, before the game can open anything.
void open_memo_init(int enabled);

// Call before every open. 1 if this is a read of a path already proven
// missing, so it can fail without touching the card; 0 means do the real open.
// An open that could write makes the memo forget the path here, before the
// file can exist.
int open_memo_before_open(const char *path, const char *mode);

// Tells the memo how a real open went. A read that failed is checked once with
// a stat and remembered only if the file is really absent; anything that could
// write, or any open that succeeded, makes the memo forget the path.
void open_memo_record(const char *path, const char *mode, int opened);

// The heartbeat line: probes answered, paths remembered, how long the proofs took.
void open_memo_report(void);

#endif
