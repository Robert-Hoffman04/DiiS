/*
    harness_config.h - one place for the harness transport endpoint.

    The Dolphin-vs-real-hardware cutover (plan §6) is a one-line change here (or
    an override on the build command line):

        make TESTDEFS='-DDESMUME_HARNESS -DHARNESS_HOST=\"192.168.1.50\"'

    HARNESS_HOST may be a dotted-quad or a hostname. Against Dolphin with Wii
    network passthrough it is the host machine's address (LAN IP is safest;
    127.0.0.1 usually works too). Port 4300 is wii_control.py's default and is
    deliberately distinct from wiiload's 4299.
*/
#ifndef HARNESS_CONFIG_H
#define HARNESS_CONFIG_H

#ifndef HARNESS_HOST
#define HARNESS_HOST "127.0.0.1"
#endif

#ifndef HARNESS_PORT
#define HARNESS_PORT 4300
#endif

// Attempts (250ms apart) to connect before the harness gives up and the app
// continues normally without it.
#ifndef HARNESS_CONNECT_RETRIES
#define HARNESS_CONNECT_RETRIES 12
#endif

#endif // HARNESS_CONFIG_H
