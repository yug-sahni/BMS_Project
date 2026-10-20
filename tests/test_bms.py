#!/usr/bin/env python3
"""Integration tests for the Banking Management System.

Each test starts a fresh bms_server on a free port with a temporary data
directory (initialised by bms_init) and drives it over real TCP connections.
Run with:  make test   (or: python3 tests/test_bms.py)
"""
import os
import pty
import re
import select
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import termios
import threading
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "bin")
ADMIN_USER, ADMIN_PW = "admin", "admin-pw"
CHOICE = "Choice: "


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Conn:
    """One client connection to the server, driven line by line."""

    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=20)
        self.buf = ""
        self.expect(CHOICE)

    def expect(self, text):
        while text not in self.buf:
            data = self.sock.recv(65536)
            if not data:
                raise ConnectionError(f"closed while waiting for {text!r}; got {self.buf!r}")
            self.buf += data.decode().replace("\x01", "")
        idx = self.buf.index(text) + len(text)
        out, self.buf = self.buf[:idx], self.buf[idx:]
        return out

    def send(self, *lines):
        self.sock.sendall("".join(f"{l}\n" for l in lines).encode())

    def do(self, *lines, until=CHOICE):
        """Sends lines and returns everything up to the next `until`."""
        self.send(*lines)
        return self.expect(until)

    def login(self, user, pw):
        return self.do("1", user, pw)

    def read_to_close(self):
        while True:
            data = self.sock.recv(65536)
            if not data:
                return self.buf
            self.buf += data.decode()

    def close(self):
        self.sock.close()


class BMSTestCase(unittest.TestCase):
    server_log = None  # path that receives the server's stderr, if set

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="bms-test-")
        self.data = os.path.join(self.dir, "data")
        self.port = free_port()
        env = dict(os.environ, BMS_DATA_DIR=self.data, BMS_PORT=str(self.port),
                   BMS_ADMIN_USERNAME=ADMIN_USER, BMS_ADMIN_PASSWORD=ADMIN_PW)
        subprocess.run([os.path.join(BIN, "bms_init")], env=env, check=True,
                       stdout=subprocess.DEVNULL)
        self.env = env
        self.conns = []
        self.addCleanup(self.cleanup)
        self.start_server()

    def start_server(self):
        err = open(self.server_log, "ab") if self.server_log else subprocess.DEVNULL
        self.server = subprocess.Popen([os.path.join(BIN, "bms_server")], env=self.env,
                                       stdout=subprocess.DEVNULL, stderr=err)
        if self.server_log:
            err.close()
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("server did not start")

    def stop_server(self):
        self.server.terminate()
        self.server.wait(timeout=5)

    def cleanup(self):
        for c in self.conns:
            try:
                c.close()
            except OSError:
                pass
        self.stop_server()
        shutil.rmtree(self.dir, ignore_errors=True)

    # ---- helpers -------------------------------------------------------
    def connect(self):
        c = Conn(self.port)
        self.conns.append(c)
        return c

    def session(self, user, pw):
        c = self.connect()
        out = c.login(user, pw)
        self.assertIn("Welcome", out, out)
        return c

    @staticmethod
    def new_id(out):
        m = re.search(r"(?:ID|Account number): (\d+)", out)
        assert m, out
        return int(m.group(1))

    def add_employee(self, username, pw="emp-pw", manager=False):
        a = self.session(ADMIN_USER, ADMIN_PW)
        eid = self.new_id(a.do("1", username, pw, f"Name {username}", "9000000000", "Addr"))
        if manager:
            out = a.do("3", str(eid), "2")
            self.assertIn("is now Manager", out)
        a.do("8", until="Goodbye.")
        return eid

    def add_customers(self, emp, names, pw="cust-pw"):
        e = self.session(emp, "emp-pw")
        ids = []
        for n in names:
            out = e.do("1", n, pw, f"Customer {n}", "+919999999999", "Somewhere")
            ids.append(self.new_id(out))
        e.do("10", until="Goodbye.")
        return ids

    def balance(self, conn):
        out = conn.do("1")
        return re.search(r"Balance: (-?[\d.]+)", out).group(1)


class LoginTests(BMSTestCase):
    def test_login_failures_and_success(self):
        c = self.connect()
        self.assertIn("Invalid username or password", c.login(ADMIN_USER, "wrong"))
        self.assertIn("Invalid username or password", c.login("nobody", "x"))
        out = c.login(ADMIN_USER, ADMIN_PW)
        self.assertIn("Logged in as Administrator", out)
        self.assertIn("Administrator Menu", out)
        out = c.do("7")  # logout returns to the login screen
        self.assertIn("Logged out.", out)
        self.assertIn("1. Login", out)
        c.send("2")
        self.assertIn("Goodbye.", c.read_to_close())

    def test_invalid_menu_input(self):
        c = self.connect()
        self.assertIn("Invalid input", c.do("abc"))
        self.assertIn("Invalid input", c.do("7"))

    def test_one_session_per_user(self):
        self.add_employee("emp")
        a = self.session("emp", "emp-pw")
        b = self.connect()
        self.assertIn("already logged in", b.login("emp", "emp-pw"))
        a.do("9")  # logout frees the session
        self.assertIn("Welcome", b.login("emp", "emp-pw"))
        # A dropped connection also frees the session.
        b.close()
        time.sleep(0.3)
        c = self.connect()
        self.assertIn("Welcome", c.login("emp", "emp-pw"))

    def test_concurrent_login_race(self):
        self.add_employee("emp")
        results = []
        conns = [self.connect() for _ in range(12)]
        barrier = threading.Barrier(len(conns))

        def go(c):
            barrier.wait()
            results.append(c.login("emp", "emp-pw"))

        threads = [threading.Thread(target=go, args=(c,)) for c in conns]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(sum("Welcome" in r for r in results), 1)
        self.assertEqual(sum("already logged in" in r for r in results), len(conns) - 1)

    def test_data_persists_across_restart(self):
        self.add_employee("emp")
        (cid,) = self.add_customers("emp", ["alice"])
        c = self.session("alice", "cust-pw")
        c.do("2", "150.50")
        c.do("13", until="Goodbye.")
        self.stop_server()
        self.start_server()
        c = self.session("alice", "cust-pw")
        self.assertEqual(self.balance(c), "150.50")


class CustomerTests(BMSTestCase):
    def setUp(self):
        super().setUp()
        self.add_employee("emp")
        self.alice, self.bob = self.add_customers("emp", ["alice", "bob"])

    def test_deposit_withdraw_balance(self):
        c = self.session("alice", "cust-pw")
        self.assertEqual(self.balance(c), "0.00")
        self.assertIn("New balance: 1000.00", c.do("2", "1000"))
        self.assertIn("New balance: 749.75", c.do("3", "250.25"))
        self.assertIn("Insufficient balance", c.do("3", "1000"))
        for bad in ["-5", "0", "abc", "1.234", "99999999999"]:
            self.assertIn("Invalid amount", c.do("2", bad), bad)
        self.assertEqual(self.balance(c), "749.75")

    def test_transfer(self):
        a = self.session("alice", "cust-pw")
        a.do("2", "500")
        self.assertIn("own account", a.do("4", str(self.alice)))
        self.assertIn("does not exist", a.do("4", "999"))
        self.assertIn("does not exist", a.do("4", "1"))  # id 1 is the admin
        self.assertIn("Insufficient", a.do("4", str(self.bob), "600", "1"))
        out = a.do("4", str(self.bob), "120", until="Option: ")
        self.assertIn(f"Transfer 120.00 to Customer bob (account #{self.bob})?", out)
        self.assertIn("Transfer cancelled", a.do("2"))
        self.assertIn("Transfer cancelled", a.do("4", str(self.bob), "120", "x"))
        self.assertEqual(self.balance(a), "500.00")
        self.assertIn("New balance: 380.00", a.do("4", str(self.bob), "120", "1"))
        b = self.session("bob", "cust-pw")
        self.assertEqual(self.balance(b), "120.00")

    def test_account_number_shown(self):
        out = self.connect().login("alice", "cust-pw")
        self.assertIn(f"Logged in as Customer, account number {self.alice}.", out)
        self.assertIn(f"Customer Menu (alice, Account #{self.alice})", out)
        e = self.session("emp", "emp-pw")
        out = e.do("1", "carol", "pw", "Carol", "123", "Addr")
        self.assertRegex(out, r"Customer created\. Account number: \d+, username: carol")

    def test_transaction_history(self):
        a = self.session("alice", "cust-pw")
        a.do("2", "500")
        a.do("3", "100")
        a.do("4", str(self.bob), "50", "1")
        out = a.do("11")
        self.assertRegex(out, r"DEPOSIT\s+500.00\s+500.00")
        self.assertRegex(out, r"WITHDRAW\s+100.00\s+400.00")
        self.assertRegex(out, rf"TRANSFER OUT\s+50.00\s+350.00 #{self.bob}")
        b = self.session("bob", "cust-pw")
        out = b.do("11")
        self.assertRegex(out, rf"TRANSFER IN\s+50.00\s+50.00 #{self.alice}")
        self.assertNotIn("DEPOSIT", out)

    def test_change_password(self):
        a = self.session("alice", "cust-pw")
        self.assertIn("incorrect", a.do("8", "wrong", "new-pw", "new-pw"))
        self.assertIn("do not match", a.do("8", "cust-pw", "new-pw", "other"))
        self.assertIn("Password changed", a.do("8", "cust-pw", "new-pw", "new-pw"))
        a.do("12")
        self.assertIn("Invalid", a.login("alice", "cust-pw"))
        self.assertIn("Welcome", a.login("alice", "new-pw"))

    def test_apply_loan_and_feedback(self):
        a = self.session("alice", "cust-pw")
        out = a.do("5", "50000", "24", "Home repair")
        self.assertIn("Application ID: 1 (status: PENDING)", out)
        self.assertIn("Invalid input", a.do("5", "50000", "0"))
        self.assertIn("Purpose cannot be empty", a.do("5", "100", "12", ""))
        self.assertIn("limited to 10000000.00 per application", a.do("5", "10000000.01"))
        self.assertIn("already have a loan application under review",
                      a.do("5", "100", "12", "Second loan"))
        self.assertIn("feedback was recorded", a.do("9", "Great service"))
        self.assertIn("cannot be empty", a.do("9", ""))


class StaffTests(BMSTestCase):
    def setUp(self):
        super().setUp()
        self.emp1 = self.add_employee("emp1")
        self.emp2 = self.add_employee("emp2")
        self.mgr = self.add_employee("mgr", manager=True)
        # A separate employee opens the accounts: whoever opened a customer's
        # account may not handle that customer's loans.
        self.clerk = self.add_employee("clerk")
        self.alice, self.bob = self.add_customers("clerk", ["alice", "bob"])

    def apply(self, user, amount, months, purpose):
        c = self.session(user, "cust-pw")
        out = c.do("5", amount, months, purpose)
        self.assertIn("Loan application submitted", out)
        c.do("13", until="Goodbye.")
        return self.new_id(out)

    def test_employee_add_customer_validation(self):
        e = self.session("emp1", "emp-pw")
        self.assertIn("already taken", e.do("1", "alice", "pw", "N", "123", "A"))
        self.assertIn("Invalid username", e.do("1", "bad name"))
        self.assertIn("Invalid phone", e.do("1", "carol", "pw", "Carol", "12ab"))

    def test_employee_modify_customer_and_passbook(self):
        a = self.session("alice", "cust-pw")
        a.do("2", "300")
        a.do("3", "45.5")
        e = self.session("emp1", "emp-pw")
        out = e.do("2", str(self.alice), "Alice New", "", "New Address")
        self.assertIn("Details updated", out)
        out = e.do("2", str(self.emp2))  # employees cannot modify staff
        self.assertIn("No such user", out)
        out = e.do("6", until="Customer account number: ")
        self.assertRegex(out, rf"#{self.alice} +alice +Alice New +Customer +active")
        self.assertNotIn("emp2", out)  # staff are not listed as customers
        out = e.do(str(self.alice))
        self.assertIn("Alice New", out)
        self.assertIn("Current balance: 254.50", out)
        self.assertRegex(out, r"DEPOSIT\s+300.00\s+300.00")
        self.assertRegex(out, r"WITHDRAW\s+45.50\s+254.50")
        # Customer balance untouched by the detail edit.
        self.assertEqual(self.balance(a), "254.50")

    def test_loan_workflow(self):
        self.apply("alice", "100000", "12", "Car")
        self.apply("bob", "2000", "6", "Laptop")
        m = self.session("mgr", "emp-pw")
        out = m.do("2", "1", str(self.emp1))
        self.assertIn("Car", out)  # open applications listed
        self.assertIn(f"#{self.emp1}", out)  # employees listed
        self.assertIn("assigned to employee", out)
        self.assertIn("No such active employee", m.do("2", "2", str(self.mgr)))
        m.do("2", "2", str(self.emp2))

        e1 = self.session("emp1", "emp-pw")
        out = e1.do("5")
        self.assertIn("ASSIGNED", out)
        self.assertNotIn("Laptop", out)
        out = e1.do("4", "1")  # refused before the decision is asked
        self.assertIn("Process the application first", out)
        self.assertNotIn("Decision", out)
        self.assertIn("No such application assigned to you", e1.do("3", "2"))
        out = e1.do("3", "1")
        self.assertIn("PROCESSING", out)
        self.assertIn("Applicant: Customer alice", out)
        self.assertIn("Applicant's unpaid loans: 0.00", out)
        self.assertIn("Application 1 APPROVED", e1.do("4", "1", "1"))
        self.assertIn("already APPROVED", e1.do("4", "1"))
        self.assertIn("No such application assigned to you", e1.do("4", "99"))

        e2 = self.session("emp2", "emp-pw")
        e2.do("3", "2")
        self.assertIn("A reason is required", e2.do("4", "2", "2", ""))
        self.assertIn("Application 2 REJECTED", e2.do("4", "2", "2", "Income too low"))
        self.assertIn("already REJECTED", m.do("2", "2", str(self.emp1)))
        b = self.session("bob", "cust-pw")
        out = b.do("6")
        self.assertRegex(out, r"(?m)^2 +2000\.00 +6 +REJECTED ")
        self.assertIn("Reason: Income too low", out)

    def test_loan_approval_credits_customer(self):
        a = self.session("alice", "cust-pw")
        self.assertIn("(no loan applications)", a.do("6"))
        a.do("5", "25000.50", "12", "Car")
        m = self.session("mgr", "emp-pw")
        m.do("2", "1", str(self.emp1))
        out = a.do("6")
        self.assertRegex(out, r"(?m)^1 +25000\.50 +12 +ASSIGNED .*Car$")
        self.assertIn("under review", out)

        e1 = self.session("emp1", "emp-pw")
        e1.do("3", "1")
        out = e1.do("4", "1", until="Decision: ")
        self.assertIn("Car", out)  # the application is shown before deciding
        self.assertIn(f"Applicant: Customer alice (account #{self.alice})", out)
        out = e1.do("1")
        self.assertIn(f"25000.50 credited to account #{self.alice}. New balance: 25000.50", out)
        self.assertIn("Application 1 APPROVED", out)
        self.assertEqual(self.balance(a), "25000.50")
        out = a.do("11")
        self.assertRegex(out, r"LOAN CREDIT\s+25000.50\s+25000.50 loan 1")
        out = a.do("6")
        self.assertRegex(out, r"(?m)^1 +25000\.50 +12 +APPROVED +\S+ \S+ +\d{4}-\S+ \S+ +Car$")
        self.assertRegex(out, r"Outstanding 25000\.50, installment 2083\.38/month, overdue 0\.00, "
                              r"next installment due \d{4}-")
        # No second loan while the first is unpaid.
        self.assertIn("repay your current loan", a.do("5", "100", "6", "Books"))
        out = e1.do("6", str(self.alice))
        self.assertIn("Unpaid loans: 25000.50", out)

    def test_loan_repayment(self):
        self.apply("alice", "1000", "4", "Bike")
        m = self.session("mgr", "emp-pw")
        m.do("2", "1", str(self.emp1))
        e1 = self.session("emp1", "emp-pw")
        e1.do("3", "1")
        e1.do("4", "1", "1")
        a = self.session("alice", "cust-pw")
        self.assertIn("No such loan of yours", a.do("7", "5", "10"))
        a.do("3", "700")  # spend most of the loan
        out = a.do("7", "1", "250")
        self.assertIn("Loan 1 (Bike): outstanding 1000.00, installment 250.00/month", out)
        self.assertIn("Repaid 250.00 on loan 1. Outstanding: 750.00. New balance: 50.00", out)
        self.assertIn("Insufficient balance", a.do("7", "1", "500"))
        self.assertIn("Only 750.00 is outstanding", a.do("7", "1", "800"))
        a.do("2", "1000")
        out = a.do("7", "1", "750")
        self.assertIn("Outstanding: 0.00. New balance: 300.00", out)
        self.assertIn("Loan 1 is now fully repaid", out)
        self.assertIn("You have no loan to repay", a.do("7"))
        out = a.do("6")
        self.assertRegex(out, r"(?m)^1 +1000\.00 +4 +REPAID ")
        out = a.do("11")
        self.assertRegex(out, r"LOAN REPAY\s+250\.00\s+50\.00 loan 1")
        self.assertRegex(out, r"LOAN REPAY\s+750\.00\s+300\.00 loan 1")
        # Repaid: a new application is accepted again.
        self.assertIn("Loan application submitted", a.do("5", "100", "6", "Books"))

    def test_separation_of_duties(self):
        self.apply("alice", "1000", "12", "Bike")
        m = self.session("mgr", "emp-pw")
        out = m.do("2", "1", str(self.clerk))
        self.assertIn("opened this customer's account", out)
        # Assigned before the rule (or by mistake): the clerk still cannot act.
        with open(os.path.join(self.data, "loans.dat"), "r+b") as f:
            f.seek(8)
            f.write(struct.pack("<ii", 2, self.clerk))  # ASSIGNED to the clerk
        c = self.session("clerk", "emp-pw")
        self.assertIn("opened this customer's account", c.do("3", "1"))

    def test_reassignment_tells_first_employee(self):
        self.apply("alice", "1000", "12", "Bike")
        m = self.session("mgr", "emp-pw")
        m.do("2", "1", str(self.emp1))
        self.assertIn("already assigned to employee", m.do("2", "1", str(self.emp1)))
        e1 = self.session("emp1", "emp-pw")
        e1.do("3", "1")
        out = m.do("2", "1", str(self.emp2), until="Option: ")
        self.assertIn(f"Application 1 is PROCESSING with employee #{self.emp1}", out)
        self.assertIn("Cancelled", m.do("2"))
        self.assertIn("assigned to employee", m.do("2", "1", str(self.emp2), "1"))
        msg = f"Application 1 was reassigned by a manager to employee #{self.emp2}"
        self.assertIn(msg, e1.do("5"))
        self.assertIn(msg, e1.do("4", "1"))

    def test_manager_activate_deactivate(self):
        a = self.session("alice", "cust-pw")
        a.do("2", "100")
        b = self.session("bob", "cust-pw")
        b.do("5", "500", "6", "Phone")
        b.do("13", until="Goodbye.")
        m = self.session("mgr", "emp-pw")
        self.assertIn("is already ACTIVE", m.do("1", str(self.bob), "1"))
        out = m.do("1", str(self.bob), "2", until="Reason for deactivating: ")
        self.assertIn("1 open loan application(s) will be rejected", out)
        self.assertIn("A reason is required", m.do(""))
        self.assertIn("Cancelled", m.do("1", str(self.bob), "2", "Fraud check", "2"))
        out = m.do("1", str(self.bob), "2", "Fraud check", "1")
        self.assertIn("is now DEACTIVATED", out)
        self.assertIn("1 open loan application(s) rejected", out)
        self.assertIn("No such customer", m.do("1", str(self.emp1)))
        self.assertIn("is deactivated", a.do("4", str(self.bob)))
        self.assertIn("deactivated", self.connect().login("bob", "cust-pw"))
        # Deactivating a logged-in customer logs them out at the next action.
        out = m.do("1", str(self.alice), "2", until="Reason for deactivating: ")
        self.assertIn("balance of 100.00 stays frozen", out)
        m.do("Audit", "1")
        out = a.do("1", until="2. Exit\n")
        self.assertIn("deactivated or its role changed", out)
        a.expect(CHOICE)
        out = m.do("1", until="Customer account number: ")
        self.assertRegex(out, rf"#{self.alice} +alice .* deactivated")
        self.assertRegex(out, rf"#{self.bob} +bob .* deactivated")
        out = m.do(str(self.alice), "1")
        self.assertIn("Deactivated because: Audit", out)
        self.assertIn("Welcome", a.login("alice", "cust-pw"))
        out = self.session("emp1", "emp-pw").do("6", str(self.bob))
        self.assertIn("Note: Fraud check", out)

    def test_manager_review_feedback(self):
        a = self.session("alice", "cust-pw")
        a.do("9", "Slow queue at branch")
        a.do("9", "Thanks for the help")
        self.assertIn("(not yet read)", a.do("10"))
        m = self.session("mgr", "emp-pw")
        out = m.do("3", until="(0 to go back): ")
        self.assertIn("Slow queue at branch", out)
        self.assertIn("(NEW)", out)
        self.assertIn("marked as reviewed", m.do("2", "1"))
        self.assertIn("Reply cannot be empty", m.do("3", "1", "2", ""))
        self.assertIn("answered", m.do("3", "1", "2", "Sorry, we are adding a counter."))
        out = m.do("3", until="(0 to go back): ")
        self.assertIn("(replied)", out)
        self.assertIn("(reviewed)", out)
        self.assertIn(f"Reply by manager #{self.mgr}: Sorry, we are adding a counter.", out)
        m.do("0")
        out = a.do("10")
        self.assertIn("Bank's reply", out)
        self.assertIn("Sorry, we are adding a counter.", out)
        self.assertIn("(read by the bank)", out)

    def test_close_account(self):
        a = self.session("alice", "cust-pw")
        a.do("2", "500")
        e = self.session("emp1", "emp-pw")
        out = e.do("7", str(self.alice), until="Reason for closing: ")
        self.assertIn("balance of 500.00 will be paid out", out)
        self.assertIn("Cancelled", e.do("Moving abroad", "2"))
        out = e.do("7", str(self.alice), "Moving abroad", "1")
        self.assertIn(f"Account #{self.alice} closed. 500.00 paid out", out)
        self.assertIn("already closed", e.do("7", str(self.alice)))
        out = e.do("6", str(self.alice))
        self.assertIn("closed", out)
        self.assertRegex(out, r"CLOSURE PAYOUT\s+500\.00\s+0\.00")
        self.assertIn("Current balance: 0.00", out)
        self.assertIn("deactivated or its role changed", a.do("1", until="2. Exit\n"))
        self.assertIn("This account was closed on", self.connect().login("alice", "cust-pw"))
        b = self.session("bob", "cust-pw")
        self.assertIn("Destination account is closed", b.do("4", str(self.alice)))
        m = self.session("mgr", "emp-pw")
        self.assertIn("cannot be reactivated", m.do("1", str(self.alice)))
        # A frozen account cannot be closed (and so paid out) by an employee.
        m.do("1", str(self.bob), "2", "Fraud check", "1")
        self.assertIn("must be reactivated", e.do("7", str(self.bob)))

    def test_cannot_close_with_unpaid_loan(self):
        self.apply("alice", "1000", "12", "Bike")
        m = self.session("mgr", "emp-pw")
        m.do("2", "1", str(self.emp1))
        e1 = self.session("emp1", "emp-pw")
        e1.do("3", "1")
        e1.do("4", "1", "1")
        self.assertIn("still owes 1000.00 on loans", e1.do("7", str(self.alice)))

    def test_admin_modify_and_roles(self):
        ad = self.session(ADMIN_USER, ADMIN_PW)
        out = ad.do("2", until="User ID: ")
        for name in ("alice", "emp1", "emp2", "mgr", ADMIN_USER):
            self.assertIn(name, out)
        self.assertIn("Details updated", ad.do(str(self.emp1), "Renamed Emp", "", ""))
        self.assertIn("Details updated", ad.do("2", str(self.alice), "", "12345", ""))
        # The administrator can edit their own details too.
        self.assertIn("Details updated", ad.do("2", "1", "Head Admin", "5550000", "HQ"))
        self.assertIn("No such employee", ad.do("3", str(self.alice)))
        out = ad.do("3", until="Employee ID: ")
        self.assertRegex(out, rf"#{self.emp1} +emp1 .*Employee")
        self.assertRegex(out, rf"#{self.mgr} +mgr .*Manager")
        self.assertNotIn("alice", out)  # customers are not staff
        ad.do("0")
        self.assertIn("is now Manager", ad.do("3", str(self.emp2), "2"))
        out = self.connect().login("emp2", "emp-pw")
        self.assertIn("Manager Menu", out)
        self.assertIn("is now Employee", ad.do("3", str(self.mgr), "1"))
        out = self.connect().login("mgr", "emp-pw")
        self.assertIn("Employee Menu", out)
        out = ad.do("2", str(self.emp1), "", "", "")
        self.assertIn("Name: Renamed Emp", out)
        out = ad.do("2", str(self.alice), "", "", "")
        self.assertIn("Phone: 12345", out)
        out = ad.do("2", "1", "", "", "")
        self.assertIn("Name: Head Admin", out)

    def test_admin_reset_password(self):
        ad = self.session(ADMIN_USER, ADMIN_PW)
        out = ad.do("5", until="User ID: ")
        for name in ("alice", "emp1", "mgr"):
            self.assertIn(name, out)
        self.assertIn("No such customer", ad.do("1"))  # not the admin itself
        self.assertIn("do not match", ad.do("5", str(self.alice), "fresh-pw", "other"))
        self.assertIn("Invalid password", ad.do("5", str(self.alice), "", ""))
        self.assertIn("(alice) has been reset", ad.do("5", str(self.alice), "fresh-pw", "fresh-pw"))
        self.assertIn("(emp1) has been reset", ad.do("5", str(self.emp1), "emp-new", "emp-new"))
        c = self.connect()
        self.assertIn("Invalid username or password", c.login("alice", "cust-pw"))
        self.assertIn("Customer Menu", c.login("alice", "fresh-pw"))
        self.assertIn("Employee Menu", self.connect().login("emp1", "emp-new"))

    def test_staff_deactivation(self):
        self.apply("alice", "1000", "12", "Bike")
        m = self.session("mgr", "emp-pw")
        m.do("2", "1", str(self.emp1))
        e1 = self.session("emp1", "emp-pw")
        e1.do("3", "1")
        ad = self.session(ADMIN_USER, ADMIN_PW)
        out = ad.do("4", until="Employee ID: ")
        self.assertIn("emp1", out)
        self.assertNotIn("alice", out)
        self.assertIn("No such employee", ad.do(str(self.alice)))
        self.assertIn("already ACTIVE", ad.do("4", str(self.emp1), "1"))
        out = ad.do("4", str(self.emp1), "2", "Left the bank", "1")
        self.assertIn("is now DEACTIVATED", out)
        self.assertIn("1 open loan application(s) returned to the pending queue", out)
        self.assertIn("deactivated or its role changed", e1.do("5", until="2. Exit\n"))
        e1.expect(CHOICE)
        self.assertIn("deactivated", e1.login("emp1", "emp-pw"))
        self.assertIn("No such active employee", m.do("2", "1", str(self.emp1)))
        self.assertIn("assigned to employee", m.do("2", "1", str(self.emp2)))
        out = ad.do("4", str(self.emp1), "1")
        self.assertIn("Deactivated because: Left the bank", out)
        self.assertIn("is now ACTIVE", out)
        self.assertIn("Employee Menu", e1.login("emp1", "emp-pw"))

    def test_promoting_employee_returns_open_loans(self):
        self.apply("alice", "1000", "12", "Bike")
        self.apply("bob", "2000", "12", "Books")
        m = self.session("mgr", "emp-pw")
        m.do("2", "1", str(self.emp1))
        m.do("2", "2", str(self.emp1))
        e1 = self.session("emp1", "emp-pw")
        e1.do("3", "1")  # loan 1 left in PROCESSING
        e1.do("3", "2")
        e1.do("4", "2", "1")  # loan 2 decided: stays APPROVED

        ad = self.session(ADMIN_USER, ADMIN_PW)
        out = ad.do("3", str(self.emp1), "2")
        self.assertIn("is now Manager", out)
        self.assertIn("1 open loan application(s) returned to the pending queue", out)

        out = m.do("2", until="Loan application ID: ")
        self.assertRegex(out, r"(?m)^1 +#\d+ +1000\.00 +12 +PENDING +- +Bike$")
        self.assertNotIn("Books", out)
        self.assertIn("No such active employee", m.do("1", str(self.emp1)))
        self.assertIn("assigned to employee", m.do("2", "1", str(self.emp2)))
        e2 = self.session("emp2", "emp-pw")
        e2.do("3", "1")
        self.assertIn("Application 1 APPROVED", e2.do("4", "1", "1"))


class LoginLockTests(BMSTestCase):
    def test_lockout_after_failed_logins(self):
        self.add_employee("emp")
        c = self.connect()
        self.assertIn("Invalid", c.login("emp", "x"))
        self.assertIn("Invalid", c.login("emp", "x"))
        c.send("1", "emp", "x")
        self.assertIn("Too many failed logins on this connection", c.read_to_close())
        c = self.connect()
        self.assertIn("Invalid", c.login("emp", "x"))
        self.assertIn("locked for 15 minutes", c.login("emp", "x"))  # 5th failure
        # Even the right password is refused while locked.
        self.assertIn("locked until", self.connect().login("emp", "emp-pw"))
        ad = self.session(ADMIN_USER, ADMIN_PW)
        out = ad.do("5", "2", "new-pw", "new-pw")
        self.assertIn("login lock lifted", out)
        self.assertIn("Employee Menu", self.connect().login("emp", "new-pw"))

    def test_success_resets_failure_count(self):
        self.add_employee("emp")
        for _ in range(3):
            c = self.connect()
            self.assertIn("Invalid", c.login("emp", "x"))
            self.assertIn("Invalid", c.login("emp", "x"))
            out = c.login("emp", "emp-pw")
            self.assertIn("Welcome", out)
            c.do("10", until="Goodbye.")


class MigrationTests(BMSTestCase):
    USER_V2 = struct.Struct("<iiiiqqqii32s128s64s16s128s128s")
    USER_V1 = struct.Struct("<iiiiq32s128s64s16s128s")

    def test_migrates_format1_data(self):
        self.add_employee("emp")
        (cid,) = self.add_customers("emp", ["alice"])
        a = self.session("alice", "cust-pw")
        a.do("2", "75")
        a.do("9", "Old feedback")
        a.do("13", until="Goodbye.")
        self.stop_server()
        # Rewrite the users and feedback files in the format-1 layout.
        path = os.path.join(self.data, "users.dat")
        with open(path, "rb") as f:
            data = f.read()
        old = b""
        for off in range(0, len(data), self.USER_V2.size):
            u = self.USER_V2.unpack_from(data, off)
            old += self.USER_V1.pack(u[0], u[1], u[2], 0, u[4], *u[9:14])
        with open(path, "wb") as f:
            f.write(old)
        path = os.path.join(self.data, "feedback.dat")
        with open(path, "rb") as f:
            fb = f.read()
        with open(path, "wb") as f:
            f.write(fb[:24] + fb[32:32 + 256])
        os.remove(os.path.join(self.data, "format"))

        r = subprocess.run([os.path.join(BIN, "bms_server")], env=self.env,
                           capture_output=True, text=True, timeout=10)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("run bin/bms_migrate", r.stderr)
        r = subprocess.run([os.path.join(BIN, "bms_migrate")], env=self.env,
                           capture_output=True, text=True, check=True)
        self.assertIn("users.dat: 3 record(s) converted", r.stdout)
        self.start_server()
        a = self.session("alice", "cust-pw")
        self.assertEqual(self.balance(a), "75.00")
        self.assertIn("Old feedback", a.do("10"))
        self.assertEqual(os.stat(os.path.join(self.data, "users.dat")).st_mode & 0o777, 0o600)


class ConcurrencyTests(BMSTestCase):
    def setUp(self):
        super().setUp()
        self.add_employee("emp")

    def test_concurrent_transfers_conserve_money(self):
        names = [f"c{i}" for i in range(8)]
        ids = self.add_customers("emp", names + ["hub"])
        hub_id = ids[-1]
        conns = {n: self.session(n, "cust-pw") for n in names + ["hub"]}
        for n in names:
            conns[n].do("2", "1000")
        conns["hub"].do("2", "1000")
        rounds = 25
        errors = []

        def sender(i, n):
            try:
                peer = ids[(i + 1) % len(names)]
                for _ in range(rounds):
                    self.assertIn("Transferred", conns[n].do("4", str(hub_id), "1", "1"))
                    # Opposite-direction transfers between neighbours exercise the
                    # ordered locking that prevents deadlock.
                    conns[n].do("4", str(peer), "2", "1")
            except Exception as ex:  # surface failures from threads
                errors.append(ex)

        def hub():
            try:
                for _ in range(rounds):
                    conns["hub"].do("3", "1")
            except Exception as ex:
                errors.append(ex)

        threads = [threading.Thread(target=sender, args=(i, n)) for i, n in enumerate(names)]
        threads.append(threading.Thread(target=hub))
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=120)
        self.assertFalse(errors, errors)

        total = sum(float(self.balance(conns[n])) for n in names + ["hub"])
        self.assertAlmostEqual(total, 9000 - rounds * 1, places=2)
        self.assertEqual(float(self.balance(conns["hub"])), 1000 + len(names) * rounds - rounds)
        for n in names:
            self.assertEqual(float(self.balance(conns[n])), 1000 - rounds)
        # Each account's history must end at its current balance.
        e = self.session("emp", "emp-pw")
        for n, cid in zip(names + ["hub"], ids):
            out = e.do("6", str(cid))
            rows = re.findall(r"^\d+\s+\S+ \S+\s+[A-Z ]+?\s+([\d.]+)\s+([\d.]+)", out, re.M)
            current = re.search(r"Current balance: ([\d.]+)", out).group(1)
            self.assertEqual(rows[-1][1], current, n)

    def test_concurrent_duplicate_username(self):
        self.add_employee("emp2")
        e1, e2 = self.session("emp", "emp-pw"), self.session("emp2", "emp-pw")
        results = []
        barrier = threading.Barrier(2)

        def go(c):
            barrier.wait()
            results.append(c.do("1", "same", "pw", "N", "1", "A"))

        ts = [threading.Thread(target=go, args=(c,)) for c in (e1, e2)]
        for t in ts:
            t.start()
        for t in ts:
            t.join()
        self.assertEqual(sum("Customer created" in r for r in results), 1)
        self.assertEqual(sum("already taken" in r for r in results), 1)

    def test_many_concurrent_clients(self):
        names = [f"u{i}" for i in range(20)]
        self.add_customers("emp", names)
        errors = []

        def go(n):
            try:
                c = Conn(self.port)
                c.login(n, "cust-pw")
                for _ in range(10):
                    c.do("2", "10")
                assert c.do("1").count("Balance: 100.00") == 1
                c.send("13")
                c.read_to_close()
                c.close()
            except Exception as ex:
                errors.append((n, ex))

        ts = [threading.Thread(target=go, args=(n,)) for n in names]
        for t in ts:
            t.start()
        for t in ts:
            t.join(timeout=60)
        self.assertFalse(errors, errors)


class RobustnessTests(BMSTestCase):
    def setUp(self):
        super().setUp()
        self.add_employee("emp")
        self.alice, self.bob = self.add_customers("emp", ["alice", "bob"])

    def test_stalled_reader_does_not_block_others(self):
        # Give alice a history far larger than the socket buffers can hold.
        path = os.path.join(self.data, "transactions.dat")
        rec = struct.Struct("<iiiiqqq")  # Transaction
        first = os.path.getsize(path) // rec.size + 1
        with open(path, "ab") as f:
            f.write(b"".join(rec.pack(first + i, self.alice, 1, 0, 100, 100 * (i + 1),
                                      1700000000) for i in range(300000)))
        stalled = self.session("alice", "cust-pw")
        stalled.send("11")  # request the history, then never read it
        time.sleep(1)
        b = self.session("bob", "cust-pw")
        b.sock.settimeout(5)
        self.assertIn("Deposit successful", b.do("2", "10"))
        self.assertIn("Withdrawal successful", b.do("3", "5"))

    def test_shutdown_disconnects_clients(self):
        a = self.session("alice", "cust-pw")
        a.sock.settimeout(10)
        self.server.terminate()
        self.server.wait(timeout=10)
        self.assertIn("shutting down", a.read_to_close())
        self.start_server()
        self.assertIn("Welcome", self.connect().login("alice", "cust-pw"))


class DemoModeTests(BMSTestCase):
    """BMS_DEMO_DELAY (see journal.h) holds a balance change's locks for a few
    seconds, so a concurrent operation on the same account visibly waits."""

    def setUp(self):
        super().setUp()
        self.add_employee("emp")
        self.alice, self.bob = self.add_customers("emp", ["alice", "bob"])
        a = self.session("alice", "cust-pw")
        a.do("2", "500")
        a.do("13", until="Goodbye.")
        self.stop_server()
        self.server_log = os.path.join(self.dir, "server.log")
        self.env["BMS_DEMO_DELAY"] = "2"
        self.start_server()

    def test_balance_check_waits_for_transfer(self):
        a, b = self.session("alice", "cust-pw"), self.session("bob", "cust-pw")
        a.send("4", str(self.bob), "100", "1")
        time.sleep(0.5)  # the transfer now holds both accounts
        start = time.time()
        # Bob never sees a half-done transfer: he waits, then sees all of it.
        self.assertEqual(self.balance(b), "100.00")
        self.assertGreater(time.time() - start, 1.0)
        self.assertIn("New balance: 400.00", a.expect(CHOICE))
        with open(self.server_log) as f:
            log = f.read()
        self.assertIn("Demo mode", log)
        self.assertIn(f"holding accounts #{self.alice} and #{self.bob} for 2 s", log)
        self.assertIn(f"waiting for a read lock on users.dat record #{self.bob}", log)
        self.assertRegex(log, rf"got the read lock on users.dat record #{self.bob} after 1\.\d s")


class CrashRecoveryTests(BMSTestCase):
    """A handler is killed mid-transfer (BMS_FAULT, see journal.h); the
    journal must leave the transfer either fully done or not done at all."""

    USER = struct.Struct("<iiiiq")  # User prefix up to balance; sizeof(User) = 544
    TXN = struct.Struct("<iiiiqqq")  # Transaction
    JOURNAL_HEAD = struct.Struct("<iiiiiiqq")  # JournalEntry up to txn[]

    def setUp(self):
        super().setUp()
        self.emp = self.add_employee("emp")
        self.add_employee("clerk")  # opens the accounts, so emp may handle their loans
        self.alice, self.bob = self.add_customers("clerk", ["alice", "bob"])
        a = self.session("alice", "cust-pw")
        a.do("2", "500")
        a.do("13", until="Goodbye.")

    def restart_with_fault(self, point):
        self.stop_server()
        self.env["BMS_FAULT"] = point
        self.start_server()

    def crash_transfer(self, conn, amount="100"):
        conn.send("4", str(self.bob), amount, "1")
        try:
            out = conn.read_to_close()
        except ConnectionResetError:
            out = ""
        self.assertNotIn("Transferred", out)

    def passbook(self, cid):
        e = self.session("emp", "emp-pw")
        out = e.do("6", str(cid))
        e.do("10", until="Goodbye.")
        return out

    def assert_transfer_done(self):
        out = self.passbook(self.alice)
        self.assertIn("Current balance: 400.00", out)
        self.assertRegex(out, rf"TRANSFER OUT\s+100.00\s+400.00 #{self.bob}")
        out = self.passbook(self.bob)
        self.assertIn("Current balance: 100.00", out)
        self.assertRegex(out, rf"TRANSFER IN\s+100.00\s+100.00 #{self.alice}")
        self.assertEqual(out.count("TRANSFER IN"), 1)
        with open(os.path.join(self.data, "journal.dat"), "rb") as f:
            data = f.read()
        for off in range(0, len(data), 120):
            self.assertEqual(self.JOURNAL_HEAD.unpack_from(data, off)[1], 0, "entry not freed")

    def test_crash_before_commit_changes_nothing(self):
        self.restart_with_fault("reserved")
        self.crash_transfer(self.session("alice", "cust-pw"))
        out = self.passbook(self.alice)
        self.assertIn("Current balance: 500.00", out)
        self.assertNotIn("TRANSFER", out)
        out = self.passbook(self.bob)
        self.assertIn("Current balance: 0.00", out)
        self.assertIn("(no transactions)", out)

    def test_crash_after_commit_completed_by_server(self):
        self.restart_with_fault("committed")
        self.crash_transfer(self.session("alice", "cust-pw"))
        # The server replays the journal when it reaps the crashed handler.
        deadline = time.time() + 5
        while "Current balance: 100.00" not in self.passbook(self.bob):
            self.assertLess(time.time(), deadline, "transfer never completed")
            time.sleep(0.1)
        self.assert_transfer_done()

    def test_crash_mid_apply_completed_by_next_access(self):
        self.restart_with_fault("applied-one")
        a = self.session("alice", "cust-pw")
        b = self.session("bob", "cust-pw")
        # Freeze the server parent so only the next account access can fix it.
        os.kill(self.server.pid, signal.SIGSTOP)
        self.addCleanup(os.kill, self.server.pid, signal.SIGCONT)
        self.crash_transfer(a)  # dies after debiting alice, before crediting bob
        # Bob's balance check locks only bob; the redo must also take alice.
        self.assertEqual(self.balance(b), "100.00")
        # The parent now reaps the crash and replays: nothing may apply twice.
        os.kill(self.server.pid, signal.SIGCONT)
        time.sleep(0.5)
        self.assertEqual(self.balance(b), "100.00")
        self.assert_transfer_done()

    def test_crash_during_loan_payout_pays_once(self):
        self.add_employee("mgr", manager=True)
        a = self.session("alice", "cust-pw")
        a.do("5", "1000", "12", "Bike")
        a.do("13", until="Goodbye.")
        m = self.session("mgr", "emp-pw")
        m.do("2", "1", str(self.emp))
        self.restart_with_fault("committed")
        e = self.session("emp", "emp-pw")
        e.do("3", "1")
        e.send("4", "1", "1")  # approve: dies after committing the payment
        try:
            self.assertNotIn("APPROVED", e.read_to_close())
        except ConnectionResetError:
            pass
        # Paid (by recovery) but the loan is still PROCESSING. Deciding again
        # (even "reject") must record it as approved without paying twice.
        # The crashed handler holds the session until it has fully exited.
        deadline = time.time() + 5
        while "Welcome" not in (out := (e := self.connect()).login("emp", "emp-pw")):
            self.assertLess(time.time(), deadline, out)
            time.sleep(0.1)
        out = e.do("4", "1", "2", "Changed my mind")
        self.assertIn("already paid out", out)
        self.assertIn("Application 1 APPROVED", out)
        e.do("10", until="Goodbye.")
        out = self.passbook(self.alice)
        self.assertIn("Current balance: 1500.00", out)
        self.assertEqual(out.count("LOAN CREDIT"), 1)

    def test_crash_during_repayment_completes_once(self):
        self.add_employee("mgr", manager=True)
        a = self.session("alice", "cust-pw")
        a.do("5", "100", "1", "Fees")
        m = self.session("mgr", "emp-pw")
        m.do("2", "1", str(self.emp))
        e = self.session("emp", "emp-pw")
        e.do("3", "1")
        e.do("4", "1", "1")
        a.do("13", until="Goodbye.")
        self.restart_with_fault("committed")
        a = self.session("alice", "cust-pw")
        a.send("7", "1", "100")  # dies after committing the repayment
        try:
            self.assertNotIn("Repaid", a.read_to_close())
        except ConnectionResetError:
            pass
        # Paid (by recovery) but the loan is still APPROVED: the next attempt
        # records it as repaid without taking the money again.
        deadline = time.time() + 5
        while "Welcome" not in (out := (a := self.connect()).login("alice", "cust-pw")):
            self.assertLess(time.time(), deadline, out)
            time.sleep(0.1)
        out = a.do("7", "1", "100")
        self.assertIn("Loan 1 is now fully repaid", out)
        self.assertNotIn("Repaid 100.00", out)
        self.assertEqual(self.balance(a), "500.00")
        self.assertEqual(a.do("11").count("LOAN REPAY"), 1)

    def test_startup_recovery(self):
        # Leave the files as if the whole server died right after debiting
        # alice: journal entry committed, bob not credited, log rows reserved.
        self.stop_server()
        users = os.path.join(self.data, "users.dat")
        with open(users, "r+b") as f:
            f.seek((self.alice - 1) * 544 + 16)
            f.write(struct.pack("<q", 40000))
        txns = os.path.join(self.data, "transactions.dat")
        first = os.path.getsize(txns) // self.TXN.size + 1
        rows = [(first, self.alice, 3, self.bob, 10000, 40000, 1700000000),
                (first + 1, self.bob, 4, self.alice, 10000, 10000, 1700000000)]
        with open(txns, "ab") as f:
            f.write(self.TXN.pack(first, 0, 0, 0, 0, 0, 0))
            f.write(self.TXN.pack(first + 1, 0, 0, 0, 0, 0, 0))
        with open(os.path.join(self.data, "journal.dat"), "wb") as f:
            f.write(self.JOURNAL_HEAD.pack(1, 1, 2, 2, self.alice, self.bob, 40000, 10000))
            f.write(b"".join(self.TXN.pack(*r) for r in rows))
        self.start_server()
        self.assert_transfer_done()


class ClientTests(BMSTestCase):
    def test_client_restores_echo_on_signals(self):
        master, slave = pty.openpty()
        self.addCleanup(os.close, master)
        self.addCleanup(os.close, slave)
        p = subprocess.Popen([os.path.join(BIN, "bms_client"), "127.0.0.1", str(self.port)],
                             stdin=slave, stdout=slave, stderr=slave)
        self.addCleanup(lambda: p.poll() is None and p.kill())
        out = b""

        def expect(text):
            nonlocal out
            deadline = time.time() + 10
            while text not in out:
                if time.time() > deadline:
                    self.fail(f"timed out waiting for {text!r}; got {out!r}")
                if select.select([master], [], [], 0.2)[0]:
                    out += os.read(master, 4096)
            out = out[out.index(text) + len(text):]

        def wait_echo(on):
            deadline = time.time() + 5
            while bool(termios.tcgetattr(slave)[3] & termios.ECHO) != on:
                if time.time() > deadline:
                    self.fail(f"terminal echo never turned {'on' if on else 'off'}")
                time.sleep(0.05)

        expect(b"Choice: ")
        os.write(master, b"1\n")
        expect(b"Username: ")
        os.write(master, b"admin\n")
        expect(b"Password: ")
        wait_echo(False)
        p.send_signal(signal.SIGTSTP)  # Ctrl+Z at the password prompt
        wait_echo(True)
        p.send_signal(signal.SIGCONT)  # fg: back to the hidden prompt
        wait_echo(False)
        p.send_signal(signal.SIGINT)  # Ctrl+C
        p.wait(timeout=5)
        wait_echo(True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
