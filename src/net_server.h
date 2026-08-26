#ifndef NET_SERVER_H
#define NET_SERVER_H

/* Start central log server listener thread. */
int net_server_start(void);

/* Stop central log server listener thread. */
void net_server_stop(void);

/* Return non-zero when listener thread is active. */
int net_server_is_running(void);

#endif
