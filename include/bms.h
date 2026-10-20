/* Record layouts and constants shared by the server, client and init tool. */
#ifndef BMS_H
#define BMS_H

#include <stdint.h>

#define USERNAME_LEN 32
#define PWHASH_LEN 128
#define NAME_LEN 64
#define PHONE_LEN 16
#define ADDRESS_LEN 128
#define PURPOSE_LEN 128
#define FEEDBACK_LEN 256
#define NOTE_LEN 128
#define PASSWORD_MAX 64

/* Largest amount accepted in one operation / held in an account (paise). */
#define MAX_AMOUNT 100000000000LL /* 1,00,00,00,000.00 */
#define MAX_BALANCE 900000000000000LL

/* Loan rules. Loans carry no interest: the customer repays the principal in
 * monthly installments of amount / tenure (rounded up). */
#define LOAN_MAX_AMOUNT 1000000000LL /* 1,00,00,000.00 per application */
#define LOAN_MONTH_SECONDS (30 * 24 * 3600)

/* After this many consecutive wrong passwords the account cannot log in for
 * LOGIN_LOCK_SECONDS (an administrator's password reset lifts it). One
 * connection is dropped after LOGIN_MAX_TRIES_PER_CONN failed logins. */
#define LOGIN_MAX_FAILURES 5
#define LOGIN_LOCK_SECONDS (15 * 60)
#define LOGIN_MAX_TRIES_PER_CONN 3

/* In-band byte the server sends before a password prompt: the client turns
 * off terminal echo until the next line is entered. */
#define NOECHO_MARK '\x01'

enum role {
    ROLE_CUSTOMER = 1,
    ROLE_EMPLOYEE = 2,
    ROLE_MANAGER = 3,
    ROLE_ADMIN = 4,
};

enum txn_type {
    TXN_DEPOSIT = 1,
    TXN_WITHDRAW = 2,
    TXN_TRANSFER_OUT = 3,
    TXN_TRANSFER_IN = 4,
    TXN_LOAN_CREDIT = 5, /* approved loan paid in; counterparty_id = loan id */
    TXN_LOAN_REPAY = 6,  /* installment paid back; counterparty_id = loan id */
    TXN_CLOSURE = 7,     /* balance paid out when the account was closed */
};

enum loan_status {
    LOAN_PENDING = 1,    /* applied, not yet assigned by a manager */
    LOAN_ASSIGNED = 2,   /* assigned to an employee */
    LOAN_PROCESSING = 3, /* employee has taken it up */
    LOAN_APPROVED = 4,
    LOAN_REJECTED = 5,
    LOAN_REPAID = 6, /* approved and fully paid back */
};

/* Every record starts with its id; id N lives at offset (N-1)*sizeof(rec). */
typedef struct {
    int32_t id;
    int32_t role;
    int32_t active;        /* 0 = deactivated, or closed (closed_at set) */
    int32_t failed_logins; /* consecutive wrong passwords */
    int64_t balance;       /* paise, customers only */
    int64_t locked_until;  /* logins refused until then, 0 = not locked */
    int64_t closed_at;     /* customers: when the account was closed, 0 = open */
    int32_t created_by;    /* staff member who opened the account, 0 = unknown */
    int32_t reserved;
    char username[USERNAME_LEN];
    char pwhash[PWHASH_LEN];
    char name[NAME_LEN];
    char phone[PHONE_LEN];
    char address[ADDRESS_LEN];
    char status_note[NOTE_LEN]; /* why it was deactivated or closed */
} User;

typedef struct {
    int32_t id;
    int32_t customer_id;
    int32_t type;
    int32_t counterparty_id; /* other account for transfers, else 0 */
    int64_t amount;
    int64_t balance_after;
    int64_t timestamp;
} Transaction;

typedef struct {
    int32_t id;
    int32_t customer_id;
    int32_t status;
    int32_t employee_id; /* 0 = unassigned */
    int64_t amount;
    int32_t tenure_months;
    int32_t prev_employee_id; /* employee it was last taken away from, 0 = none */
    int64_t applied_at;
    int64_t decided_at;
    int64_t repaid_at;
    char purpose[PURPOSE_LEN];
    char note[NOTE_LEN]; /* reason given for a rejection */
} Loan;

typedef struct {
    int32_t id;
    int32_t customer_id;
    int32_t reviewed;
    int32_t reviewer_id; /* manager who reviewed or replied */
    int64_t timestamp;
    int64_t replied_at; /* 0 = no reply */
    char text[FEEDBACK_LEN];
    char reply[FEEDBACK_LEN];
} Feedback;

/* Write-ahead journal entry for one balance-changing operation (journal.c).
 * Slot N lives at offset (N-1)*sizeof(JournalEntry); state 0 = free slot. */
#define JOURNAL_MAX_ACCOUNTS 2
#define JOURNAL_MAX_TXNS 2

enum journal_state {
    JOURNAL_FREE = 0,
    JOURNAL_COMMITTED = 1, /* durable; account and log writes may be partial */
};

typedef struct {
    int32_t id;
    int32_t state;
    int32_t naccounts;
    int32_t ntxns;
    int32_t account_id[JOURNAL_MAX_ACCOUNTS];
    int64_t balance[JOURNAL_MAX_ACCOUNTS];    /* new absolute balances */
    Transaction txn[JOURNAL_MAX_TXNS]; /* log rows, ids reserved before commit */
} JournalEntry;

const char *role_name(int role);
const char *txn_type_name(int type);
const char *loan_status_name(int status);
/* "active", "deactivated" or "closed". */
const char *account_status(const User *u);

#endif
