// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "localsend.h"
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int number(const char *s, int max)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    return errno || !*s || *end || v < 1 || v > max ? -1 : (int)v;
}

int main(int argc, char **argv)
{
    const char *alias = NULL, *to = NULL, *pin = NULL, *dir = NULL;
    int port = 53317, multicast_port = 0, seconds = 3, ask = 0, no_pin = 0, opt;
    static const struct option opts[] = {
        {"alias", required_argument, NULL, 'a'}, {"to", required_argument, NULL, 't'},
        {"pin", required_argument, NULL, 'p'}, {"dir", required_argument, NULL, 'd'},
        {"port", required_argument, NULL, 'P'}, {"seconds", required_argument, NULL, 's'},
        {"ask", no_argument, NULL, 'A'}, {"multicast-port", required_argument, NULL, 'M'},
        {"no-pin", no_argument, NULL, 'N'},
        {NULL, 0, NULL, 0}
    };
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    if (argc < 2) return ls_error("protocol", "expected scan, send, receive or serve");
    const char *command = argv[1];
    optind = 2;
    opterr = 0;
    while ((opt = getopt_long(argc, argv, "+", opts, NULL)) != -1) {
        switch (opt) {
        case 'a': alias = optarg; break;
        case 't': to = optarg; break;
        case 'p': pin = optarg; break;
        case 'd': dir = optarg; break;
        case 'P': port = number(optarg, 65535); break;
        case 's': seconds = !strcmp(optarg, "0") ? 0 : number(optarg, 3600); break;
        case 'A': ask = 1; break;
        case 'N': no_pin = 1; break;
        case 'M': multicast_port = number(optarg, 65535); break;
        default: return ls_error("protocol", "invalid option");
        }
    }
    if (port < 1 || multicast_port < 0 || seconds < 0) return ls_error("protocol", "invalid port or duration");
    /* Ours (receive) is six digits; a receiver's (send) is whatever it chose. */
    if (pin && (!strcmp(command, "receive") || !strcmp(command, "serve"))) {
        if (strlen(pin) != 6) return ls_error("pin", "PIN must contain six digits");
        for (const char *p = pin; *p; ++p)
            if (!isdigit((unsigned char)*p)) return ls_error("pin", "PIN must contain six digits");
    } else if (pin) {
        size_t n = strlen(pin);
        if (!n || n > 64) return ls_error("pin", "PIN must be 1 to 64 characters");
        for (const unsigned char *p = (const unsigned char *)pin; *p; ++p)
            if (*p < 32 || *p == 127) return ls_error("pin", "PIN has a control character");
    }
    if (strcmp(command, "scan") && strcmp(command, "send") && strcmp(command, "receive") && strcmp(command, "serve"))
        return ls_error("protocol", "unknown command");
    if ((!strcmp(command, "send") && (!to || optind == argc || dir || ask)) ||
        (!strcmp(command, "serve") && (!pin || no_pin || to || dir || ask || optind == argc)) ||
        (!strcmp(command, "receive") && (!dir || !pin == !no_pin || optind != argc || to)) ||
        (!strcmp(command, "scan") && (optind != argc || to || dir || ask || pin)))
        return ls_error("protocol", "invalid arguments for command");
    LsIdentity id = {0};
    char err[512] = "";
    if (ls_identity_init(&id, alias, port, err, sizeof(err))) return ls_error("io", err);
    id.multicast_port = multicast_port;
    if (!strcmp(command, "serve")) {
        id.download = 1;
        if (ls_random(id.fingerprint, 32)) { ls_identity_free(&id); return ls_error("io", "cannot generate HTTP identity"); }
    }
    int result;
    if (!strcmp(command, "send")) result = ls_send(&id, to, pin, argv + optind, argc - optind);
    else if (!strcmp(command, "receive")) result = ls_receive(&id, dir, pin, ask);
    else if (!strcmp(command, "serve")) result = ls_serve(&id, pin, argv + optind, argc - optind);
    else {
        result = ls_discover(&id, seconds, 1, NULL, NULL, err, sizeof(err));
        if (result < 0) result = ls_error("network", err);
        else { puts("done"); result = 0; }
    }
    ls_identity_free(&id);
    return result;
}
