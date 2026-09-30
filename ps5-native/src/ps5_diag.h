#ifndef PS5_DIAG_H
#define PS5_DIAG_H

/* GbaC0re PS5 native — on-console diagnostics.
 *
 * ps5_diag_log: writes to stderr (the homebrew launcher / deploy tooling
 * streams the app's stdout+stderr back to the host; klogsrv also carries
 * it). Always safe to call.
 *
 * ps5_diag_notify: on-screen system notification ("GbaC0re: <msg>").
 * Backed by the SDK's notification API once wired; until then it degrades
 * to ps5_diag_log so nothing depends on it.
 *
 * Boot/ROM/first-frame breadcrumbs let Ty report exactly how far the app
 * got if something fails on hardware.
 */

void ps5_diag_log(const char *msg);
void ps5_diag_notify(const char *msg);

/* printf-style breadcrumb helper. */
void ps5_diag_logf(const char *fmt, ...);

#endif
