/* GbaC0re PS5 native — on-console diagnostics. See ps5_diag.h.
 *
 * Native titles have no console stdio: ps5_diag_log appends to gbac0re.log
 * in the app's working directory (best-effort; fetch it over FTP), and
 * ps5_diag_notify pops a real system notification via
 * sceKernelSendNotificationRequest (3120-byte request, message at offset
 * 45 — the pad_input-example format).
 */

#include "ps5_diag.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

extern int sceKernelSendNotificationRequest(int device, void *request,
                                            int size, int flags);

#define DIAG_LOG_FILE "gbac0re.log"

void ps5_diag_log(const char *msg) {
    FILE *f = fopen(DIAG_LOG_FILE, "a");
    if (f) {
        fputs(msg, f);
        fclose(f);
    }
}

void ps5_diag_logf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ps5_diag_log(buf);
}

void ps5_diag_notify(const char *msg) {
    /* 3120-byte notification request; message text at offset 45. */
    static char req[3120];
    size_t n;

    memset(req, 0, sizeof(req));
    n = strlen(msg);
    if (n > sizeof(req) - 46) n = sizeof(req) - 46;
    memcpy(req + 45, msg, n);

    sceKernelSendNotificationRequest(0, req, sizeof(req), 0);

    /* Notifications are also worth keeping in the log. */
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "[notify] %s\n", msg);
        ps5_diag_log(buf);
    }
}
