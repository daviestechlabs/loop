#define _POSIX_C_SOURCE 200809L

#include "vbus.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static vbus_stop_flag g_stop;

static void on_sig(int sig) {
    (void)sig;
    atomic_store_explicit(&g_stop, 1, memory_order_relaxed);
}

int main(int argc, char **argv) {
    const char *path = vbus_default_path();
    if (argc > 1 && argv[1][0]) path = argv[1];
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    signal(SIGPIPE, SIG_IGN);
    fprintf(stderr, "vbus broker listening on %s (pure-C, no NATS)\n", path);
    if (vbus_broker_run(path, &g_stop) != 0) {
        perror("vbus_broker_run");
        return 1;
    }
    return 0;
}
