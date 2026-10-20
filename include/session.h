/* One-session-per-user table kept in System V shared memory and guarded by a
 * System V semaphore, shared by every forked client handler. */
#ifndef SESSION_H
#define SESSION_H

/* Called once by the server parent before forking. 0 on success. */
int session_init(void);

/* Removes the shared memory and semaphore (server shutdown). */
void session_destroy(void);

/* Registers uid for the calling process. Returns 0 on success, 1 if the
 * user already has a live session, -1 on error / table full. */
int session_acquire(int uid);

/* Releases uid if it is held by the calling process. */
void session_release(int uid);

#endif
