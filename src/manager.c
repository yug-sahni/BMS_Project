#include "net.h"
#include "ops.h"
#include "store.h"

#include <limits.h>
#include <string.h>
#include <time.h>

/* Number of open loan applications of the customer, or -1 on error. */
static int count_open_loans(int customer_id)
{
    Loan l;
    int open = 0;

    if (file_lock(FILE_LOANS, F_RDLCK) < 0)
        return -1;
    int n = rec_count(FILE_LOANS);
    for (int id = 1; id <= n; id++) {
        if (rec_read(FILE_LOANS, id, &l) == 1 && l.customer_id == customer_id &&
            loan_is_open(&l))
            open++;
    }
    file_unlock(FILE_LOANS);
    return open;
}

/* Shows what deactivating c means and asks for a reason and confirmation.
 * Returns 0 to go ahead. */
static int confirm_deactivation(const User *c, char *reason, size_t size)
{
    long confirm;
    int64_t debt;
    char a[32];

    net_printf("While deactivated the customer cannot log in, and the balance of %s stays "
               "frozen in the account.\n", fmt_amount(c->balance, a, sizeof(a)));
    if (customer_loan_debt(c->id, &debt) == 0 && debt > 0)
        net_printf("They still owe %s on loans and cannot repay while deactivated.\n",
                   fmt_amount(debt, a, sizeof(a)));
    int open = count_open_loans(c->id);
    if (open > 0)
        net_printf("%d open loan application(s) will be rejected.\n", open);
    if (prompt_reason("Reason for deactivating: ", reason, size) < 0)
        return -1;
    if (prompt_int("1. Deactivate  2. Cancel\nOption: ", 1, 2, &confirm) < 0 || confirm != 1) {
        net_puts("Cancelled.\n");
        return -1;
    }
    return 0;
}

static void set_customer_active(void)
{
    long id, choice;
    User c;
    char reason[NOTE_LEN] = "", when[32], bal[32];
    int deactivated = 0;

    list_users(1u << ROLE_CUSTOMER);
    if (prompt_int("Customer account number: ", 1, INT_MAX, &id) < 0)
        return;
    if (get_user((int)id, &c) != 1 || c.role != ROLE_CUSTOMER) {
        net_puts("No such customer account.\n");
        return;
    }
    if (c.closed_at) {
        net_printf("Account %d was closed on %s and cannot be reactivated.\n", c.id,
                   fmt_time(c.closed_at, when, sizeof(when)));
        return;
    }
    net_printf("Account %d (%s, %s) is currently %s. Balance: %s\n", c.id, c.username, c.name,
               c.active ? "ACTIVE" : "DEACTIVATED", fmt_amount(c.balance, bal, sizeof(bal)));
    if (!c.active && c.status_note[0])
        net_printf("Deactivated because: %s\n", c.status_note);
    if (prompt_int("1. Activate  2. Deactivate\nOption: ", 1, 2, &choice) < 0)
        return;
    if ((choice == 1) == (c.active != 0)) {
        net_printf("Account %d is already %s.\n", c.id, c.active ? "ACTIVE" : "DEACTIVATED");
        return;
    }
    if (choice == 2 && confirm_deactivation(&c, reason, sizeof(reason)) < 0)
        return;

    if (rec_lock(FILE_USERS, (int)id, F_WRLCK) < 0) {
        net_puts("Internal error: could not lock account.\n");
        return;
    }
    if (rec_read(FILE_USERS, (int)id, &c) == 1 && c.role == ROLE_CUSTOMER && !c.closed_at) {
        c.active = choice == 1;
        strcpy(c.status_note, reason);
        if (rec_write(FILE_USERS, (int)id, &c) == 0) {
            net_printf("Account %d is now %s.\n", c.id, c.active ? "ACTIVE" : "DEACTIVATED");
            deactivated = !c.active;
        } else {
            net_puts("Internal error: could not save.\n");
        }
    } else {
        net_puts("The account changed meanwhile; nothing was done.\n");
    }
    rec_unlock(FILE_USERS, (int)id);

    /* After the record is saved: an application made in between is caught
     * here, and no loan can be paid into the account any more. */
    if (deactivated) {
        int n = reject_open_loans((int)id, "Account deactivated by the bank");
        if (n < 0)
            net_puts("Internal error: could not reject open loan applications.\n");
        else if (n > 0)
            net_printf("%d open loan application(s) rejected.\n", n);
    }
}

static void list_open_loans_and_employees(void)
{
    Loan l;
    User u;
    int shown = 0;

    net_puts("Open loan applications:\n");
    if (file_lock(FILE_LOANS, F_RDLCK) == 0) {
        int n = rec_count(FILE_LOANS);
        print_loan_header();
        for (int id = 1; id <= n; id++) {
            if (rec_read(FILE_LOANS, id, &l) == 1 && loan_is_open(&l)) {
                print_loan(&l);
                shown++;
            }
        }
        file_unlock(FILE_LOANS);
    }
    if (!shown)
        net_puts("(none)\n");

    net_puts("Employees:\n");
    if (file_lock(FILE_USERS, F_RDLCK) == 0) {
        int n = rec_count(FILE_USERS);
        for (int id = 1; id <= n; id++) {
            if (rec_read(FILE_USERS, id, &u) == 1 && u.role == ROLE_EMPLOYEE && u.active)
                net_printf("  #%d %s (%s)\n", u.id, u.name, u.username);
        }
        file_unlock(FILE_USERS);
    }
}

/* If loan_id is already with another employee, says so and asks before
 * taking it away from them. Returns 0 to go ahead. */
static int confirm_reassign(int loan_id, int emp_id)
{
    Loan l;
    long confirm;
    int found;

    if (rec_lock(FILE_LOANS, loan_id, F_RDLCK) < 0)
        return 0; /* the locked step below reports it */
    found = rec_read(FILE_LOANS, loan_id, &l) == 1;
    rec_unlock(FILE_LOANS, loan_id);
    if (!found || !loan_is_open(&l) || !l.employee_id || l.employee_id == emp_id)
        return 0;
    net_printf("Application %d is %s with employee #%d. Reassigning takes it away from them.\n",
               loan_id, loan_status_name(l.status), l.employee_id);
    if (prompt_int("1. Reassign  2. Cancel\nOption: ", 1, 2, &confirm) < 0 || confirm != 1) {
        net_puts("Cancelled.\n");
        return -1;
    }
    return 0;
}

static void assign_loan(void)
{
    long loan_id, emp_id;
    Loan l;
    User e, c;

    list_open_loans_and_employees();
    if (prompt_int("Loan application ID: ", 1, INT_MAX, &loan_id) < 0)
        return;
    if (prompt_int("Assign to employee ID: ", 1, INT_MAX, &emp_id) < 0)
        return;
    if (confirm_reassign((int)loan_id, (int)emp_id) < 0)
        return;
    if (rec_lock(FILE_LOANS, (int)loan_id, F_WRLCK) < 0) {
        net_puts("Internal error: could not lock application.\n");
        return;
    }
    /* Checked under the loan lock: a promotion or deactivation that lands
     * after this check waits for the lock and then returns the loan to the
     * queue (admin.c). Lock order: loan, then user records. */
    if (get_user((int)emp_id, &e) != 1 || e.role != ROLE_EMPLOYEE || !e.active) {
        net_puts("No such active employee.\n");
    } else if (rec_read(FILE_LOANS, (int)loan_id, &l) != 1) {
        net_puts("No such loan application.\n");
    } else if (!loan_is_open(&l)) {
        net_printf("Application is already %s.\n", loan_status_name(l.status));
    } else if (l.employee_id == emp_id) {
        net_printf("Application %ld is already assigned to employee #%ld.\n", loan_id, emp_id);
    } else if (get_user(l.customer_id, &c) == 1 && c.created_by == emp_id) {
        net_printf("Employee #%ld opened this customer's account; assign the application to "
                   "another employee.\n", emp_id);
    } else {
        if (l.employee_id)
            l.prev_employee_id = l.employee_id;
        l.employee_id = (int32_t)emp_id;
        l.status = LOAN_ASSIGNED;
        if (rec_write(FILE_LOANS, (int)loan_id, &l) == 0)
            net_printf("Application %ld assigned to employee #%ld (%s).\n", loan_id, emp_id,
                       e.username);
        else
            net_puts("Internal error: could not save.\n");
    }
    rec_unlock(FILE_LOANS, (int)loan_id);
}

static void review_feedback(int uid)
{
    Feedback f;
    char when[32], reply[FEEDBACK_LEN] = "";
    long id, action;
    int shown = 0;

    if (file_lock(FILE_FEEDBACK, F_RDLCK) < 0) {
        net_puts("Internal error: could not lock feedback.\n");
        return;
    }
    int n = rec_count(FILE_FEEDBACK);
    for (int i = 1; i <= n; i++) {
        if (rec_read(FILE_FEEDBACK, i, &f) != 1)
            continue;
        net_printf("[%d] %s customer #%d %s\n    %s\n", f.id,
                   fmt_time(f.timestamp, when, sizeof(when)), f.customer_id,
                   f.replied_at ? "(replied)" : f.reviewed ? "(reviewed)" : "(NEW)", f.text);
        if (f.replied_at)
            net_printf("    Reply by manager #%d: %s\n", f.reviewer_id, f.reply);
        shown++;
    }
    file_unlock(FILE_FEEDBACK);
    if (!shown) {
        net_puts("(no feedback)\n");
        return;
    }
    if (prompt_int("Feedback ID to respond to (0 to go back): ", 0, INT_MAX, &id) < 0 ||
        id == 0)
        return;
    if (id > n) {
        net_puts("No such feedback.\n");
        return;
    }
    if (prompt_int("1. Mark as reviewed  2. Reply to the customer\nOption: ", 1, 2, &action) < 0)
        return;
    if (action == 2) {
        prompt_line("Reply: ", reply, sizeof(reply));
        if (reply[0] == '\0') {
            net_puts("Reply cannot be empty.\n");
            return;
        }
    }
    if (rec_lock(FILE_FEEDBACK, (int)id, F_WRLCK) < 0) {
        net_puts("Internal error: could not lock feedback.\n");
        return;
    }
    if (rec_read(FILE_FEEDBACK, (int)id, &f) != 1) {
        net_puts("No such feedback.\n");
    } else {
        f.reviewed = 1;
        f.reviewer_id = uid;
        if (action == 2) {
            strcpy(f.reply, reply);
            f.replied_at = (int64_t)time(NULL);
        }
        if (rec_write(FILE_FEEDBACK, (int)id, &f) == 0)
            net_printf("Feedback %ld %s.\n", id,
                       action == 2 ? "answered; the customer can read the reply"
                                   : "marked as reviewed");
        else
            net_puts("Internal error: could not save.\n");
    }
    rec_unlock(FILE_FEEDBACK, (int)id);
}

void manager_menu(int uid)
{
    User me;
    long choice;

    for (;;) {
        if (session_still_valid(uid, ROLE_MANAGER, &me) < 0)
            return;
        net_printf("\n===== Manager Menu (%s) =====\n"
                   " 1. Activate/Deactivate Customer Accounts\n"
                   " 2. Assign Loan Application Processes to Employees\n"
                   " 3. Review Customer Feedback\n"
                   " 4. Change Password\n"
                   " 5. Logout\n"
                   " 6. Exit\n",
                   me.username);
        if (prompt_int("Choice: ", 1, 6, &choice) < 0)
            continue;
        /* Re-check: the account may have changed while the user was typing. */
        if (session_still_valid(uid, ROLE_MANAGER, &me) < 0)
            return;
        switch (choice) {
        case 1: set_customer_active(); break;
        case 2: assign_loan(); break;
        case 3: review_feedback(uid); break;
        case 4: change_password(uid); break;
        case 5: return;
        case 6: exit_session();
        }
    }
}
