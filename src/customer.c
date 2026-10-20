#include "journal.h"
#include "net.h"
#include "ops.h"
#include "store.h"

#include <limits.h>
#include <string.h>
#include <time.h>

static void view_balance(int uid)
{
    LockSet held;
    User u;
    char bal[32];
    int found;

    /* Through money_lock, so a crashed operation on this account is
     * completed before the balance is shown. */
    if (money_lock(&uid, 1, &held) < 0) {
        net_puts("Internal error: could not lock account.\n");
        return;
    }
    found = rec_read(FILE_USERS, uid, &u) == 1;
    money_unlock(&held);
    if (!found) {
        net_puts("Internal error: account not found.\n");
        return;
    }
    net_printf("Account number: %d\nAccount holder: %s\nBalance: %s\n", u.id, u.name,
               fmt_amount(u.balance, bal, sizeof(bal)));
}

/* Reports a journal_apply failure. Returns 1 if the operation went through. */
static int applied(int r, const char *what)
{
    if (r == JOURNAL_OK)
        return 1;
    if (r == JOURNAL_NOT_COMMITTED)
        net_printf("Internal error: %s failed; nothing was changed.\n", what);
    else
        net_printf("Internal error: %s was recorded and will be completed automatically.\n",
                   what);
    return 0;
}

static void deposit(int uid)
{
    int64_t amount;
    LockSet held;
    JournalEntry e;
    User u;
    char bal[32];

    if (prompt_amount("Amount to deposit: ", &amount) < 0)
        return;
    if (money_lock(&uid, 1, &held) < 0) {
        net_puts("Internal error: could not lock account.\n");
        return;
    }
    if (rec_read(FILE_USERS, uid, &u) != 1 || !u.active) {
        net_puts("Account is not active.\n");
    } else if (u.balance + amount > MAX_BALANCE) {
        net_puts("Deposit refused: balance limit exceeded.\n");
    } else {
        int64_t balance = u.balance + amount;
        memset(&e, 0, sizeof(e));
        journal_set_balance(&e, uid, balance);
        journal_add_txn(&e, uid, TXN_DEPOSIT, amount, balance, 0);
        if (applied(journal_apply(&e), "deposit"))
            net_printf("Deposit successful. New balance: %s\n",
                       fmt_amount(balance, bal, sizeof(bal)));
    }
    money_unlock(&held);
}

static void withdraw(int uid)
{
    int64_t amount;
    LockSet held;
    JournalEntry e;
    User u;
    char bal[32];

    if (prompt_amount("Amount to withdraw: ", &amount) < 0)
        return;
    if (money_lock(&uid, 1, &held) < 0) {
        net_puts("Internal error: could not lock account.\n");
        return;
    }
    if (rec_read(FILE_USERS, uid, &u) != 1 || !u.active) {
        net_puts("Account is not active.\n");
    } else if (u.balance < amount) {
        net_printf("Insufficient balance. Available: %s\n",
                   fmt_amount(u.balance, bal, sizeof(bal)));
    } else {
        int64_t balance = u.balance - amount;
        memset(&e, 0, sizeof(e));
        journal_set_balance(&e, uid, balance);
        journal_add_txn(&e, uid, TXN_WITHDRAW, amount, balance, 0);
        if (applied(journal_apply(&e), "withdrawal"))
            net_printf("Withdrawal successful. New balance: %s\n",
                       fmt_amount(balance, bal, sizeof(bal)));
    }
    money_unlock(&held);
}

static void transfer(int uid)
{
    long to, confirm;
    int64_t amount;
    LockSet held;
    JournalEntry e;
    User src, dst;
    char bal[32], amt[32];

    if (prompt_int("Destination account number: ", 1, INT_MAX, &to) < 0)
        return;
    if (to == uid) {
        net_puts("Cannot transfer to your own account.\n");
        return;
    }
    if (get_user((int)to, &dst) != 1 || dst.role != ROLE_CUSTOMER) {
        net_puts("Destination account does not exist.\n");
        return;
    }
    if (!dst.active) {
        net_printf("Destination account is %s.\n", account_status(&dst));
        return;
    }
    if (prompt_amount("Amount to transfer: ", &amount) < 0)
        return;
    /* Show who the money goes to, so a mistyped account number is caught. */
    net_printf("Transfer %s to %s (account #%ld)?\n", fmt_amount(amount, amt, sizeof(amt)),
               dst.name, to);
    if (prompt_int("1. Confirm  2. Cancel\nOption: ", 1, 2, &confirm) < 0 || confirm != 1) {
        net_puts("Transfer cancelled.\n");
        return;
    }

    /* money_lock takes both accounts in ascending id order, so opposite
     * transfers cannot deadlock; the journal makes the four writes atomic.
     * Everything checked above is checked again under the locks. */
    int ids[2] = {uid, (int)to};
    if (money_lock(ids, 2, &held) < 0) {
        net_puts("Internal error: could not lock accounts.\n");
        return;
    }

    if (rec_read(FILE_USERS, uid, &src) != 1 || !src.active) {
        net_puts("Your account is not active.\n");
    } else if (rec_read(FILE_USERS, (int)to, &dst) != 1 || dst.role != ROLE_CUSTOMER) {
        net_puts("Destination account does not exist.\n");
    } else if (!dst.active) {
        net_printf("Destination account is %s.\n", account_status(&dst));
    } else if (src.balance < amount) {
        net_printf("Insufficient balance. Available: %s\n",
                   fmt_amount(src.balance, bal, sizeof(bal)));
    } else if (dst.balance + amount > MAX_BALANCE) {
        net_puts("Transfer refused: destination balance limit exceeded.\n");
    } else {
        int64_t src_balance = src.balance - amount, dst_balance = dst.balance + amount;
        memset(&e, 0, sizeof(e));
        journal_set_balance(&e, uid, src_balance);
        journal_set_balance(&e, (int)to, dst_balance);
        journal_add_txn(&e, uid, TXN_TRANSFER_OUT, amount, src_balance, (int)to);
        journal_add_txn(&e, (int)to, TXN_TRANSFER_IN, amount, dst_balance, uid);
        if (applied(journal_apply(&e), "transfer"))
            net_printf("Transferred to account %ld. New balance: %s\n", to,
                       fmt_amount(src_balance, bal, sizeof(bal)));
    }
    money_unlock(&held);
}

static void apply_loan(int uid)
{
    Loan l;
    long tenure;
    int64_t debt;
    char max[32];

    memset(&l, 0, sizeof(l));
    if (prompt_amount("Loan amount: ", &l.amount) < 0)
        return;
    if (l.amount > LOAN_MAX_AMOUNT) {
        net_printf("Loans are limited to %s per application.\n",
                   fmt_amount(LOAN_MAX_AMOUNT, max, sizeof(max)));
        return;
    }
    if (prompt_int("Tenure (months, 1-360): ", 1, 360, &tenure) < 0)
        return;
    prompt_line("Purpose: ", l.purpose, sizeof(l.purpose));
    if (l.purpose[0] == '\0') {
        net_puts("Purpose cannot be empty.\n");
        return;
    }
    l.customer_id = uid;
    l.tenure_months = (int32_t)tenure;
    l.status = LOAN_PENDING;
    l.applied_at = (int64_t)time(NULL);

    /* One application under review and no unpaid loan at a time. Checked
     * and appended under one exclusive lock, so two applications sent at
     * once cannot both pass. */
    if (file_lock(FILE_LOANS, F_WRLCK) < 0) {
        net_puts("Internal error: could not lock loans.\n");
        return;
    }
    const char *refusal = NULL;
    int n = rec_count(FILE_LOANS);
    for (int id = 1; id <= n && !refusal; id++) {
        Loan other;
        if (rec_read(FILE_LOANS, id, &other) == 1 && other.customer_id == uid &&
            loan_is_open(&other))
            refusal = "You already have a loan application under review.\n";
    }
    if (!refusal && customer_loan_debt(uid, &debt) < 0)
        refusal = "Internal error: could not check your loans.\n";
    else if (!refusal && debt > 0)
        refusal = "Please repay your current loan before applying for another.\n";
    int id = refusal ? -1 : rec_append_locked(FILE_LOANS, &l);
    file_unlock(FILE_LOANS);
    if (refusal)
        net_puts(refusal);
    else if (id < 0)
        net_puts("Internal error: could not save application.\n");
    else
        net_printf("Loan application submitted. Application ID: %d (status: %s)\n", id,
                   loan_status_name(l.status));
}

/* Repayment position of an approved loan, as one indented line. */
static void print_repayment(const Loan *l)
{
    int64_t owed, inst = loan_installment(l), now = (int64_t)time(NULL);
    char a[32], b[32], c[32], d[32];

    if (loan_outstanding(l, &owed) < 0) {
        net_puts("    (could not read repayments)\n");
        return;
    }
    int64_t months = now > l->decided_at ? (now - l->decided_at) / LOAN_MONTH_SECONDS : 0;
    int64_t due = inst * months < l->amount ? inst * months : l->amount;
    int64_t paid = l->amount - owed, overdue = due > paid ? due - paid : 0;
    net_printf("    Outstanding %s, installment %s/month, overdue %s", fmt_amount(owed, a, sizeof(a)),
               fmt_amount(inst, b, sizeof(b)), fmt_amount(overdue, c, sizeof(c)));
    if (owed > 0)
        net_printf(", next installment due %s",
                   fmt_time(l->decided_at + (months + 1) * LOAN_MONTH_SECONDS, d, sizeof(d)));
    net_puts("\n");
}

static void view_loans(int uid)
{
    Loan l;
    char amt[32], applied[32], decided[32];
    int shown = 0;

    if (file_lock(FILE_LOANS, F_RDLCK) < 0) {
        net_puts("Internal error: could not lock loans.\n");
        return;
    }
    int n = rec_count(FILE_LOANS);
    net_printf("%-6s %16s %-6s %-10s %-19s %-19s %s\n", "LoanID", "Amount", "Months", "Status",
               "Applied", "Decided", "Purpose");
    for (int id = 1; id <= n; id++) {
        if (rec_read(FILE_LOANS, id, &l) != 1 || l.customer_id != uid)
            continue;
        net_printf("%-6d %16s %-6d %-10s %-19s %-19s %s\n", l.id,
                   fmt_amount(l.amount, amt, sizeof(amt)), l.tenure_months,
                   loan_status_name(l.status), fmt_time(l.applied_at, applied, sizeof(applied)),
                   fmt_time(l.decided_at, decided, sizeof(decided)), l.purpose);
        if (l.status == LOAN_APPROVED)
            print_repayment(&l);
        else if (l.status == LOAN_REJECTED && l.note[0])
            net_printf("    Reason: %s\n", l.note);
        shown++;
    }
    file_unlock(FILE_LOANS);
    if (!shown)
        net_puts("(no loan applications)\n");
    else
        net_puts("PENDING, ASSIGNED and PROCESSING applications are under review. "
                 "An APPROVED loan is credited to your balance and repaid with "
                 "'Repay Loan'.\n");
}

static void mark_repaid(Loan *l)
{
    l->status = LOAN_REPAID;
    l->repaid_at = (int64_t)time(NULL);
    if (rec_write(FILE_LOANS, l->id, l) == 0)
        net_printf("Loan %d is now fully repaid.\n", l->id);
    else
        net_puts("Internal error: could not record the loan as repaid.\n");
}

/* Pays amount off loan l from uid's balance. The caller holds the loan's
 * lock and the account (money_lock). */
static void pay_installment(int uid, Loan *l, int64_t amount)
{
    int64_t owed;
    JournalEntry e;
    User u;
    char a[32], b[32], c[32];

    if (loan_outstanding(l, &owed) < 0) {
        net_puts("Internal error: could not read repayments.\n");
        return;
    }
    if (owed == 0) { /* paid before a crash cut the status update short */
        mark_repaid(l);
        return;
    }
    if (amount > owed) {
        net_printf("Only %s is outstanding on loan %d.\n", fmt_amount(owed, a, sizeof(a)),
                   l->id);
        return;
    }
    if (rec_read(FILE_USERS, uid, &u) != 1 || !u.active) {
        net_puts("Account is not active.\n");
        return;
    }
    if (u.balance < amount) {
        net_printf("Insufficient balance. Available: %s\n",
                   fmt_amount(u.balance, a, sizeof(a)));
        return;
    }
    int64_t balance = u.balance - amount;
    memset(&e, 0, sizeof(e));
    journal_set_balance(&e, uid, balance);
    journal_add_txn(&e, uid, TXN_LOAN_REPAY, amount, balance, l->id);
    if (!applied(journal_apply(&e), "repayment"))
        return;
    net_printf("Repaid %s on loan %d. Outstanding: %s. New balance: %s\n",
               fmt_amount(amount, a, sizeof(a)), l->id,
               fmt_amount(owed - amount, b, sizeof(b)), fmt_amount(balance, c, sizeof(c)));
    if (owed == amount)
        mark_repaid(l);
}

/* Lists the customer's approved loans. Returns how many. */
static int list_repayable(int uid)
{
    Loan l;
    int64_t owed;
    char a[32], b[32];
    int shown = 0;

    if (file_lock(FILE_LOANS, F_RDLCK) < 0)
        return 0;
    int n = rec_count(FILE_LOANS);
    for (int id = 1; id <= n; id++) {
        if (rec_read(FILE_LOANS, id, &l) != 1 || l.customer_id != uid ||
            l.status != LOAN_APPROVED || loan_outstanding(&l, &owed) < 0)
            continue;
        net_printf("Loan %d (%s): outstanding %s, installment %s/month\n", l.id, l.purpose,
                   fmt_amount(owed, a, sizeof(a)),
                   fmt_amount(loan_installment(&l), b, sizeof(b)));
        shown++;
    }
    file_unlock(FILE_LOANS);
    return shown;
}

static void repay_loan(int uid)
{
    long id;
    int64_t amount;
    LockSet held;
    Loan l;

    if (!list_repayable(uid)) {
        net_puts("You have no loan to repay.\n");
        return;
    }
    if (prompt_int("Loan ID: ", 1, INT_MAX, &id) < 0 ||
        prompt_amount("Amount to repay: ", &amount) < 0)
        return;
    /* Lock order: loan, then account (money_lock), as everywhere else. */
    if (rec_lock(FILE_LOANS, (int)id, F_WRLCK) < 0) {
        net_puts("Internal error: could not lock loan.\n");
        return;
    }
    if (rec_read(FILE_LOANS, (int)id, &l) != 1 || l.customer_id != uid) {
        net_puts("No such loan of yours.\n");
    } else if (l.status != LOAN_APPROVED) {
        net_printf("Loan %ld is %s; only approved loans are repaid.\n", id,
                   loan_status_name(l.status));
    } else if (money_lock(&uid, 1, &held) < 0) {
        net_puts("Internal error: could not lock account.\n");
    } else {
        pay_installment(uid, &l, amount);
        money_unlock(&held);
    }
    rec_unlock(FILE_LOANS, (int)id);
}
static void add_feedback(int uid)
{
    Feedback f;

    memset(&f, 0, sizeof(f));
    prompt_line("Your feedback: ", f.text, sizeof(f.text));
    if (f.text[0] == '\0') {
        net_puts("Feedback cannot be empty.\n");
        return;
    }
    f.customer_id = uid;
    f.timestamp = (int64_t)time(NULL);
    if (rec_append(FILE_FEEDBACK, &f) < 0)
        net_puts("Internal error: could not save feedback.\n");
    else
        net_puts("Thank you, your feedback was recorded.\n");
}

static void view_feedback(int uid)
{
    Feedback f;
    char when[32];
    int shown = 0;

    if (file_lock(FILE_FEEDBACK, F_RDLCK) < 0) {
        net_puts("Internal error: could not lock feedback.\n");
        return;
    }
    int n = rec_count(FILE_FEEDBACK);
    for (int id = 1; id <= n; id++) {
        if (rec_read(FILE_FEEDBACK, id, &f) != 1 || f.customer_id != uid)
            continue;
        net_printf("[%d] %s %s\n    %s\n", f.id, fmt_time(f.timestamp, when, sizeof(when)),
                   f.replied_at ? "(replied)" : f.reviewed ? "(read by the bank)" : "(not yet read)",
                   f.text);
        if (f.replied_at)
            net_printf("    Bank's reply (%s): %s\n", fmt_time(f.replied_at, when, sizeof(when)),
                       f.reply);
        shown++;
    }
    file_unlock(FILE_FEEDBACK);
    if (!shown)
        net_puts("(no feedback)\n");
}

void customer_menu(int uid)
{
    User me;
    long choice;

    for (;;) {
        if (session_still_valid(uid, ROLE_CUSTOMER, &me) < 0)
            return;
        net_printf("\n===== Customer Menu (%s, Account #%d) =====\n"
                   " 1. View Account Balance\n"
                   " 2. Deposit Money\n"
                   " 3. Withdraw Money\n"
                   " 4. Transfer Funds\n"
                   " 5. Apply for a Loan\n"
                   " 6. View My Loans\n"
                   " 7. Repay Loan\n"
                   " 8. Change Password\n"
                   " 9. Add Feedback\n"
                   "10. View My Feedback\n"
                   "11. View Transaction History\n"
                   "12. Logout\n"
                   "13. Exit\n",
                   me.username, uid);
        if (prompt_int("Choice: ", 1, 13, &choice) < 0)
            continue;
        /* Re-check: the account may have changed while the user was typing. */
        if (session_still_valid(uid, ROLE_CUSTOMER, &me) < 0)
            return;
        switch (choice) {
        case 1: view_balance(uid); break;
        case 2: deposit(uid); break;
        case 3: withdraw(uid); break;
        case 4: transfer(uid); break;
        case 5: apply_loan(uid); break;
        case 6: view_loans(uid); break;
        case 7: repay_loan(uid); break;
        case 8: change_password(uid); break;
        case 9: add_feedback(uid); break;
        case 10: view_feedback(uid); break;
        case 11: print_transactions(uid); break;
        case 12: return;
        case 13: exit_session();
        }
    }
}
