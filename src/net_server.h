#ifndef NET_SERVER_H
#define NET_SERVER_H

typedef struct {
	unsigned long long accepted_connections;
	unsigned long long tls_failures;
	unsigned long long auth_failures;
	unsigned long long requests;
	unsigned long long rate_limit_rejections;
	unsigned long long blacklist_rejections;
	int active_sessions;
	int blacklisted_ips;
} NetServerMetrics;

/* Start central log server listener thread. */
int net_server_start(void);

/* Stop central log server listener thread. */
void net_server_stop(void);

/* Return non-zero when listener thread is active. */
int net_server_is_running(void);

void net_server_get_metrics(NetServerMetrics *out);

#endif
