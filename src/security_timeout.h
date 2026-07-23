/*
 * Single source of truth for the post-connect encryption watchdog window.
 *
 * One 60 s window for both pairing and locked mode — see the comment above
 * SECURITY_TIMEOUT in main.c (dialog-abort risk, bond-loss recovery, accept
 * list as the real gate).
 */
#ifndef SECURITY_TIMEOUT_H_
#define SECURITY_TIMEOUT_H_

#define SECURITY_TIMEOUT_MS 60000

#endif /* SECURITY_TIMEOUT_H_ */
