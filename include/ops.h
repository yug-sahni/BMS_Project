/* Operations shared across roles: authentication, user records, transaction
 * log, and the role menus' entry points. */
#ifndef OPS_H
#define OPS_H

#include "bms.h"

#include <stddef.h>
#include <stdint.h>

/* auth.c */
int hash_password(const char *password, char *out, size_t size);
int verify_password(const char *password, const char *hash);

/* common_ops.c */
int valid_username(const char *s);
int valid_password(const char *s);
int valid_phone(const char *s);

/* Returns 1 and fills *out if found, 0 if not, -1 on error. */
int get_user(int id, User *out);
/* Returns the user's id, or 0 if no such username. */
int find_user_by_username(const char *username, User *out);

/* Prompts for username, password, name, phone, address and creates the user
 * with the given role, recording creator (the staff member's id). Returns the
 * new id or -1 (message already sent). */
int create_user_interactive(int role, int creator);

/* Edits name/phone/address of user id, which must have one of the roles in
 * allowed_mask (bit 1<<role). */
void modify_user_interactive(int id, unsigned allowed_mask);

void change_password(int uid);

/* Re-reads the logged-in user. Returns 0 if still active with the expected
 * role, else tells the user and returns -1 (caller logs out). */
int session_still_valid(int uid, int role, User *out);

void print_transactions(int customer_id);

/* Lists users whose role is in role_mask (bit 1<<role) with their IDs. */
void list_users(unsigned role_mask);

void print_loan_header(void);
void print_loan(const Loan *l);

/* Loan balances come from the transaction log, so they can never disagree
 * with the account balances: a loan's LOAN CREDIT and LOAN REPAY rows are
 * written by the journal together with the balance change.
 *
 * Sums the amounts of rows of the given type, filtered by customer and/or
 * loan (0 = any). 0 on success, -1 on error. */
int loan_txn_total(int customer_id, int loan_id, int type, int64_t *sum);
/* Principal still owed on an APPROVED loan (0 for any other status). */
int loan_outstanding(const Loan *l, int64_t *out);
/* Everything the customer still owes across all loans. Exact while the
 * caller holds the customer's account (money_lock). */
int customer_loan_debt(int customer_id, int64_t *out);
/* Monthly installment: amount / tenure, rounded up to the next paisa. */
int64_t loan_installment(const Loan *l);
/* PENDING, ASSIGNED or PROCESSING. */
int loan_is_open(const Loan *l);
/* Rejects every open application of the customer with the given note.
 * Takes the loans file lock, so the caller must hold no loan lock. Returns
 * how many, or -1 on error. */
int reject_open_loans(int customer_id, const char *note);

/* Reads a required free-text reason. 0, or -1 (after telling the user). */
int prompt_reason(const char *label, char *buf, size_t size);

/* Role menus: return when the user logs out. Exit disconnects directly. */
void customer_menu(int uid);
void employee_menu(int uid);
void manager_menu(int uid);
void admin_menu(int uid);

/* Called when the user picks Exit: ends the connection. */
void exit_session(void) __attribute__((noreturn));

#endif
