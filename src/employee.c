#include "journal.h"
#include "net.h"
#include "ops.h"
#include "store.h"

#include <limits.h>
#include <string.h>
#include <time.h>

static void modify_customer(void)
{
    long id;

    list_users(1u << ROLE_CUSTOMER);
    if (prompt_int("Customer account number: ", 1, INT_MAX, &id) < 0)
        return;
    modify_user_interactive((int)id, 1u << ROLE_CUSTOMER);
}

/* Tells the employee that a manager moved application l away from them. */
static void print_taken_away(const Loan *l)
{
    if (l->employee_id)
        net_printf("Application %d was reassigned by a manager to employee #%d.\n", l->id,
                   l->employee_id);
    else
        net_printf("Application %d was returned by a manager to the pending queue.\n", l->id);
}

/* The message for a loan that is not (or no longer) assigned to uid. */
static void not_assigned(const Loan *l, int found, int uid)
{
    if (found && l->prev_employee_id == uid && l->employee_id != uid && loan_is_open(l))
        print_taken_away(l);
    else
        net_puts("No such application assigned to you.\n");
}

/* Separation of duties: the employee who opened a customer's account does
 * not process that customer's loans. Returns -1 (message sent) if c is one
 * of uid's customers. */
static int check_not_own_customer(const User *c, int uid)
{
    if (c->created_by != uid)
        return 0;
    net_puts("You opened this customer's account, so you may not process their loan.\n"
             "Ask a manager to assign it to another employee.\n");
    return -1;
}

static void view_assigned(int uid)
{
    Loan l;
    int shown = 0;

    if (file_lock(FILE_LOANS, F_RDLCK) < 0) {
        net_puts("Internal error: could not lock loans.\n");
        return;
    }
    int n = rec_count(FILE_LOANS);
    print_loan_header();
    for (int id = 1; id <= n; id++) {
        if (rec_read(FILE_LOANS, id, &l) == 1 && l.employee_id == uid) {
            print_loan(&l);
            shown++;
        }
    }
    if (!shown)
        net_puts("(no loan applications assigned to you)\n");
    for (int id = 1; id <= n; id++) {
        if (rec_read(FILE_LOANS, id, &l) == 1 && l.prev_employee_id == uid &&
            l.employee_id != uid && loan_is_open(&l))
            print_taken_away(&l);
    }
    file_unlock(FILE_LOANS);
}

/* Takes an assigned application into processing and shows the applicant. */
static void process_loan(int uid)
{
    long id;
    Loan l;
    User c;
    char bal[32], debt_s[32];
    int64_t debt;
    int ok = 0, found;

    if (prompt_int("Loan application ID: ", 1, INT_MAX, &id) < 0)
        return;
    if (rec_lock(FILE_LOANS, (int)id, F_WRLCK) < 0) {
        net_puts("Internal error: could not lock application.\n");
        return;
    }
    found = rec_read(FILE_LOANS, (int)id, &l) == 1;
    if (!found || l.employee_id != uid) {
        not_assigned(&l, found, uid);
    } else if (get_user(l.customer_id, &c) == 1 && check_not_own_customer(&c, uid) < 0) {
        /* message sent */
    } else if (l.status != LOAN_ASSIGNED && l.status != LOAN_PROCESSING) {
        net_printf("Application is already %s.\n", loan_status_name(l.status));
    } else {
        l.status = LOAN_PROCESSING;
        ok = rec_write(FILE_LOANS, (int)id, &l) == 0;
        if (!ok)
            net_puts("Internal error: could not save.\n");
    }
    rec_unlock(FILE_LOANS, (int)id);
    if (!ok)
        return;

    net_puts("Application is now being processed by you:\n");
    print_loan_header();
    print_loan(&l);
    if (get_user(l.customer_id, &c) == 1)
        net_printf("Applicant: %s (%s), phone %s, %s, current balance %s\n", c.name,
                   c.username, c.phone, account_status(&c),
                   fmt_amount(c.balance, bal, sizeof(bal)));
    if (customer_loan_debt(l.customer_id, &debt) == 0)
        net_printf("Applicant's unpaid loans: %s\n", fmt_amount(debt, debt_s, sizeof(debt_s)));
    net_puts("Use 'Approve/Reject Loans' to record the decision.\n");
}

/* Tells the employee why a loan can't be decided, or returns 0 if it can. */
static int check_decidable(const Loan *l, int found, int uid)
{
    if (!found || l->employee_id != uid) {
        not_assigned(l, found, uid);
        return -1;
    }
    if (l->status == LOAN_ASSIGNED) {
        net_puts("Process the application first (Process Loan Applications).\n");
        return -1;
    }
    if (l->status != LOAN_PROCESSING) {
        net_printf("Application is already %s.\n", loan_status_name(l->status));
        return -1;
    }
    return 0;
}

/* Pays an approved loan into the customer's account. The caller holds the
 * loan's lock and the customer's account (money_lock). Returns 0 if paid or
 * committed to the journal, -1 if not (message sent). */
static int credit_loan(const Loan *l)
{
    User c;
    JournalEntry e;
    char bal[32], amt[32];

    if (rec_read(FILE_USERS, l->customer_id, &c) != 1 || c.role != ROLE_CUSTOMER) {
        net_puts("The applicant's account no longer exists; cannot approve.\n");
        return -1;
    }
    if (!c.active) {
        net_puts("The applicant's account is deactivated; cannot pay out the loan.\n");
        return -1;
    }
    if (c.balance + l->amount > MAX_BALANCE) {
        net_puts("Paying out the loan would exceed the account's balance limit.\n");
        return -1;
    }
    int64_t balance = c.balance + l->amount;
    memset(&e, 0, sizeof(e));
    journal_set_balance(&e, l->customer_id, balance);
    journal_add_txn(&e, l->customer_id, TXN_LOAN_CREDIT, l->amount, balance, l->id);
    switch (journal_apply(&e)) {
    case JOURNAL_OK:
        net_printf("%s credited to account #%d. New balance: %s\n",
                   fmt_amount(l->amount, amt, sizeof(amt)), l->customer_id,
                   fmt_amount(balance, bal, sizeof(bal)));
        return 0;
    case JOURNAL_INCOMPLETE:
        net_puts("Internal error: the payment was recorded and will be completed "
                 "automatically.\n");
        return 0;
    default:
        net_puts("Internal error: could not pay out the loan; nothing was changed.\n");
        return -1;
    }
}

static void decide_loan(int uid)
{
    long id, decision;
    Loan l;
    LockSet held;
    User c = {0};
    int64_t credited, debt;
    char reason[NOTE_LEN] = "", debt_s[32];
    int found;

    if (prompt_int("Loan application ID: ", 1, INT_MAX, &id) < 0)
        return;
    /* Check first, so the employee isn't asked to decide a loan that can't
     * be decided. Checked again under the lock below. */
    if (rec_lock(FILE_LOANS, (int)id, F_RDLCK) < 0) {
        net_puts("Internal error: could not lock application.\n");
        return;
    }
    found = rec_read(FILE_LOANS, (int)id, &l) == 1;
    rec_unlock(FILE_LOANS, (int)id);
    if (check_decidable(&l, found, uid) < 0)
        return;
    if (get_user(l.customer_id, &c) == 1 && check_not_own_customer(&c, uid) < 0)
        return;
    print_loan_header();
    print_loan(&l);
    if (c.id == l.customer_id)
        net_printf("Applicant: %s (account #%d)\n", c.name, c.id);
    if (prompt_int("1. Approve (credits the amount to the applicant)  2. Reject\nDecision: ", 1, 2,
                   &decision) < 0)
        return;
    if (decision == 2 && prompt_reason("Reason for rejection (shown to the customer): ", reason,
                                       sizeof(reason)) < 0)
        return;

    /* Lock order: loan, then account (money_lock), as everywhere else. */
    if (rec_lock(FILE_LOANS, (int)id, F_WRLCK) < 0) {
        net_puts("Internal error: could not lock application.\n");
        return;
    }
    found = rec_read(FILE_LOANS, (int)id, &l) == 1;
    if (check_decidable(&l, found, uid) < 0) {
        rec_unlock(FILE_LOANS, (int)id);
        return;
    }
    /* money_lock also completes a payment a crashed handler left half done,
     * so loan_credited() sees it. */
    if (money_lock(&l.customer_id, 1, &held) < 0) {
        net_puts("Internal error: could not lock the applicant's account.\n");
        rec_unlock(FILE_LOANS, (int)id);
        return;
    }
    /* The history row is the proof of payment: after a crash between the
     * payment and the status update the loan is still PROCESSING, but must
     * not be paid again. The account lock makes both sums exact. */
    int new_status = 0;
    if (loan_txn_total(l.customer_id, l.id, TXN_LOAN_CREDIT, &credited) < 0 ||
        customer_loan_debt(l.customer_id, &debt) < 0) {
        net_puts("Internal error: could not check earlier payments.\n");
    } else if (credited) {
        net_puts("This loan was already paid out; recording it as approved.\n");
        new_status = LOAN_APPROVED;
    } else if (decision == 2) {
        new_status = LOAN_REJECTED;
    } else if (debt > 0) {
        net_printf("The applicant still owes %s on an earlier loan; it must be repaid first.\n",
                   fmt_amount(debt, debt_s, sizeof(debt_s)));
    } else if (credit_loan(&l) == 0) {
        new_status = LOAN_APPROVED;
    }
    if (new_status) {
        if (new_status == LOAN_REJECTED)
            strcpy(l.note, reason);
        l.status = new_status;
        l.decided_at = (int64_t)time(NULL);
        if (rec_write(FILE_LOANS, (int)id, &l) == 0)
            net_printf("Application %ld %s.\n", id, loan_status_name(l.status));
        else
            net_puts("Internal error: could not save.\n");
    }
    money_unlock(&held);
    rec_unlock(FILE_LOANS, (int)id);
}

static void passbook(void)
{
    long id;
    User c;
    int64_t debt;
    char bal[32];

    list_users(1u << ROLE_CUSTOMER);
    if (prompt_int("Customer account number: ", 1, INT_MAX, &id) < 0)
        return;
    if (get_user((int)id, &c) != 1 || c.role != ROLE_CUSTOMER) {
        net_puts("No such customer account.\n");
        return;
    }
    net_printf("Passbook of account %d - %s (%s), %s\nCurrent balance: %s\n", c.id, c.name,
               c.username, account_status(&c), fmt_amount(c.balance, bal, sizeof(bal)));
    if (customer_loan_debt(c.id, &debt) == 0 && debt > 0)
        net_printf("Unpaid loans: %s\n", fmt_amount(debt, bal, sizeof(bal)));
    if (c.status_note[0])
        net_printf("Note: %s\n", c.status_note);
    print_transactions(c.id);
}

/* Pays out the remaining balance and closes the account for good. A
 * deactivated (frozen) account cannot be closed, nor one with unpaid loans. */
static void close_account(void)
{
    long id, confirm;
    User c;
    LockSet held;
    JournalEntry e;
    int64_t debt, payout = 0;
    char reason[NOTE_LEN], a[32];
    int closed = 0;

    list_users(1u << ROLE_CUSTOMER);
    if (prompt_int("Customer account number: ", 1, INT_MAX, &id) < 0)
        return;
    int cid = (int)id;
    if (get_user(cid, &c) != 1 || c.role != ROLE_CUSTOMER) {
        net_puts("No such customer account.\n");
        return;
    }
    if (c.closed_at) {
        net_puts("This account is already closed.\n");
        return;
    }
    if (!c.active) {
        net_puts("This account is deactivated by a manager; it must be reactivated before it "
                 "can be closed.\n");
        return;
    }
    if (customer_loan_debt(cid, &debt) < 0) {
        net_puts("Internal error: could not check the customer's loans.\n");
        return;
    }
    if (debt > 0) {
        net_printf("The customer still owes %s on loans; it must be repaid before closing.\n",
                   fmt_amount(debt, a, sizeof(a)));
        return;
    }
    net_printf("Closing account #%d (%s). The balance of %s will be paid out to the customer, "
               "and open loan applications rejected.\n",
               c.id, c.name, fmt_amount(c.balance, a, sizeof(a)));
    if (prompt_reason("Reason for closing: ", reason, sizeof(reason)) < 0)
        return;
    if (prompt_int("1. Close the account  2. Cancel\nOption: ", 1, 2, &confirm) < 0 ||
        confirm != 1) {
        net_puts("Cancelled.\n");
        return;
    }

    /* Checked again under the account lock, which every loan payment and
     * repayment also takes, so the debt cannot change underneath. */
    if (money_lock(&cid, 1, &held) < 0) {
        net_puts("Internal error: could not lock account.\n");
        return;
    }
    if (rec_read(FILE_USERS, cid, &c) != 1 || !c.active || c.closed_at) {
        net_puts("The account changed meanwhile; nothing was done.\n");
    } else if (customer_loan_debt(cid, &debt) < 0 || debt > 0) {
        net_puts("The customer has unpaid loans; nothing was done.\n");
    } else {
        int r = JOURNAL_OK;
        if (c.balance > 0) {
            payout = c.balance;
            memset(&e, 0, sizeof(e));
            journal_set_balance(&e, cid, 0);
            journal_add_txn(&e, cid, TXN_CLOSURE, payout, 0, 0);
            r = journal_apply(&e);
        }
        if (r == JOURNAL_NOT_COMMITTED) {
            net_puts("Internal error: could not pay out the balance; nothing was done.\n");
        } else if (r == JOURNAL_INCOMPLETE) {
            net_puts("Internal error: the payout was recorded and will be completed "
                     "automatically. Close the account again afterwards.\n");
        } else if (rec_read(FILE_USERS, cid, &c) == 1) {
            c.active = 0;
            c.closed_at = (int64_t)time(NULL);
            strcpy(c.status_note, reason);
            closed = rec_write(FILE_USERS, cid, &c) == 0;
            if (closed)
                net_printf("Account #%d closed. %s paid out to the customer.\n", cid,
                           fmt_amount(payout, a, sizeof(a)));
            else
                net_puts("Internal error: could not save.\n");
        }
    }
    money_unlock(&held);
    if (closed) {
        int n = reject_open_loans(cid, "Account closed");
        if (n < 0)
            net_puts("Internal error: could not reject open loan applications.\n");
        else if (n > 0)
            net_printf("%d open loan application(s) rejected.\n", n);
    }
}

void employee_menu(int uid)
{
    User me;
    long choice;

    for (;;) {
        if (session_still_valid(uid, ROLE_EMPLOYEE, &me) < 0)
            return;
        net_printf("\n===== Employee Menu (%s) =====\n"
                   " 1. Add New Customer\n"
                   " 2. Modify Customer Details\n"
                   " 3. Process Loan Applications\n"
                   " 4. Approve/Reject Loans\n"
                   " 5. View Assigned Loan Applications\n"
                   " 6. View Customer Transactions (Passbook)\n"
                   " 7. Close Customer Account\n"
                   " 8. Change Password\n"
                   " 9. Logout\n"
                   "10. Exit\n",
                   me.username);
        if (prompt_int("Choice: ", 1, 10, &choice) < 0)
            continue;
        /* Re-check: the account may have changed while the user was typing. */
        if (session_still_valid(uid, ROLE_EMPLOYEE, &me) < 0)
            return;
        switch (choice) {
        case 1: create_user_interactive(ROLE_CUSTOMER, uid); break;
        case 2: modify_customer(); break;
        case 3: process_loan(uid); break;
        case 4: decide_loan(uid); break;
        case 5: view_assigned(uid); break;
        case 6: passbook(); break;
        case 7: close_account(); break;
        case 8: change_password(uid); break;
        case 9: return;
        case 10: exit_session();
        }
    }
}
