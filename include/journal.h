/* Write-ahead journal that makes each balance-changing operation atomic
 * against crashes.
 *
 * A money operation locks its accounts with money_lock(), describes its
 * effect in a JournalEntry (new balances + transaction rows) and calls
 * journal_apply(). The entry is fsynced to journal.dat (the commit point)
 * before any account or log row is written, and cleared once all of them
 * are. If the handler dies in between, the entry stays committed and is
 * redone by whoever next locks one of its accounts, by the server after it
 * reaps the crashed handler, or at server startup.
 *
 * Redo is idempotent: it writes absolute balances (only the balance field,
 * so concurrent detail edits are kept) and log rows at ids reserved before
 * the commit. It is safe because a committed entry's accounts stay locked by
 * its owner until it is cleared, and every balance change goes through
 * money_lock(), which redoes the entries on its accounts before returning. */
#ifndef JOURNAL_H
#define JOURNAL_H

#include "bms.h"

#define MONEY_LOCK_MAX 16

typedef struct {
    int n;
    int ids[MONEY_LOCK_MAX]; /* ascending */
} LockSet;

/* Write-locks the user records ids[0..n-1] in ascending id order (no
 * deadlock between operations) and completes any committed journal entry
 * touching them, locking that entry's other accounts too. Returns 0 with the
 * accounts locked, -1 on error (nothing locked). */
int money_lock(const int *ids, int n, LockSet *held);
void money_unlock(LockSet *held);

/* Building an entry: new balance for an account, and a log row. */
void journal_set_balance(JournalEntry *e, int account_id, int64_t balance);
void journal_add_txn(JournalEntry *e, int customer_id, int type, int64_t amount,
                     int64_t balance_after, int counterparty_id);

enum {
    JOURNAL_OK = 0,
    JOURNAL_NOT_COMMITTED = -1, /* nothing changed */
    JOURNAL_INCOMPLETE = -2,    /* committed: will be completed by recovery */
};

/* Commits and applies e. Caller holds every account in e via money_lock. */
int journal_apply(JournalEntry *e);

/* Completes every committed entry. Used at startup and after a handler
 * crash. Returns the number completed, or -1 on error. */
int journal_recover_all(void);

/* Test hook: if BMS_FAULT names this point, the process kills itself here
 * with SIGKILL, simulating a crash. Points: "reserved", "committed",
 * "applied-one" (see journal_apply). */
void fault_point(const char *name);

#endif
