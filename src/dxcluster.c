#include "dxcluster.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdarg.h>
#include <strings.h>
#include <sys/select.h>
#include <unistd.h>

#include "config.h"

#define DXCLUSTER_CONNECT_TIMEOUT_SEC 30
#define DXCLUSTER_RECONNECT_DELAY_SEC 120

Spot spots[MAX_SPOTS];
int spot_count = 0;
int spot_start = 0;
char dxcluster_status[128] = "DXCluster idle";
pthread_mutex_t dxcluster_mutex = PTHREAD_MUTEX_INITIALIZER;

static int worker_started = 0;
static int retry_requested = 0;
static int stop_requested = 0;
static pthread_t thread_id;
static int active_socket = -1;
static pthread_mutex_t worker_mutex = PTHREAD_MUTEX_INITIALIZER;
struct sockaddr_in addr;

static void dxcluster_close_socket(void *arg);

/*
 * Safely update the active DXCluster socket descriptor.
 *
 * @param sock Socket descriptor or -1 when disconnected.
 * @return Nothing.
 */
static void dxcluster_set_active_socket(int sock) {
  pthread_mutex_lock(&worker_mutex);
  active_socket = sock;
  pthread_mutex_unlock(&worker_mutex);
}

/*
 * Return whether a command text already starts with DX.
 *
 * @param text Input command text.
 * @return true when text starts with "dx" (case-insensitive).
 */
static bool dxcluster_is_dx_command(const char *text) {
  if (!text)
    return false;

  while (*text && isspace((unsigned char)*text))
    text++;

  if (strncasecmp(text, "dx", 2) != 0)
    return false;

  return text[2] == 0 || isspace((unsigned char)text[2]);
}

/*
 * Print a DXCluster debug line when --debug mode is enabled.
 *
 * @param fmt printf-style format string.
 * @param ... Format arguments.
 * @return Nothing.
 */
static void dxcluster_debug_log(const char *fmt, ...) {
  if (!app_debug_enabled || !fmt)
    return;

  time_t now = time(NULL);
  struct tm local_tm;
  localtime_r(&now, &local_tm);

  char ts[32];
  strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &local_tm);

  fprintf(stderr, "[debug][dxcluster][%s] ", ts);

  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);

  fputc('\n', stderr);
}

/* ------------------------------------------------ */

/*
 * Close both ends of the stop pipe.
 *
 * @return Nothing.
 */
static void dxcluster_close_socket(void *arg) {
  int *sock = (int *)arg;
  if (!sock || *sock < 0)
    return;

  close(*sock);
  *sock = -1;
}

/*
 * Wait for socket readiness or a stop request.
 *
 * @param sock Socket descriptor to monitor.
 * @param want_write Nonzero to wait for writability, zero to wait for read.
 * @param timeout_sec Timeout in seconds, or negative to wait indefinitely.
 * @return 1 if the socket is ready, 0 on timeout, or -1 on stop/error.
 */
static int dxcluster_wait_for_socket(int sock, int want_write, int timeout_sec) {
  if (sock < 0) {
    errno = EINVAL;
    return -1;
  }

  fd_set rfds;
  fd_set wfds;

  FD_ZERO(&rfds);
  FD_ZERO(&wfds);

  if (want_write)
    FD_SET(sock, &wfds);
  else
    FD_SET(sock, &rfds);

  int maxfd = sock;

  struct timeval tv;
  struct timeval *ptv = NULL;
  if (timeout_sec >= 0) {
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    ptv = &tv;
  }

  int rc = select(maxfd + 1, want_write ? NULL : &rfds,
                  want_write ? &wfds : NULL, NULL, ptv);
  if (rc <= 0)
    return rc;

  return want_write ? FD_ISSET(sock, &wfds) : FD_ISSET(sock, &rfds);
}

/*
 * Sleep for a period before the next reconnect attempt.
 *
 * @param timeout_sec Timeout in seconds.
 * @return Nothing.
 */
static void dxcluster_sleep_seconds(int timeout_sec) {
  if (timeout_sec > 0)
    sleep((unsigned int)timeout_sec);
}

/*
 * Check whether a line looks like a DX de spot.
 *
 * @param line Input text.
 * @return true if the line starts with DX de after whitespace, otherwise false.
 */
static bool dxcluster_line_is_dx_de(const char *line) {
  if (!line)
    return false;

  while (*line && isspace((unsigned char)*line))
    line++;

  return strncasecmp(line, "DX de", 5) == 0;
}

/*
 * Parse and store a spot line in the circular buffer.
 *
 * @param line Raw cluster line.
 * @return Nothing.
 */
static void add_spot(const char *line) {
  if (!line || !line[0])
    return;

  char copy[512];
  strncpy(copy, line, sizeof(copy) - 1);
  copy[sizeof(copy) - 1] = 0;

  size_t len = strlen(copy);
  while (len > 0 && (copy[len - 1] == '\r' || copy[len - 1] == '\n' ||
                     copy[len - 1] == ' ' || copy[len - 1] == '\t'))
    copy[--len] = 0;

  pthread_mutex_lock(&dxcluster_mutex);

  int index;
  if (spot_count < MAX_SPOTS)
    index = spot_count++;
  else {
    index = spot_start;
    spot_start = (spot_start + 1) % MAX_SPOTS;
  }

  Spot *s = &spots[index];
  memset(s, 0, sizeof(*s));

  char *save = NULL;
  char *tokens[8] = {0};
  int count = 0;

  for (char *tok = strtok_r(copy, " \t", &save); tok && count < 8;
       tok = strtok_r(NULL, " \t", &save)) {
    tokens[count++] = tok;
  }

  int skip = 0;
  if (count >= 2 && strcmp(tokens[0], "DX") == 0 &&
      strcmp(tokens[1], "de") == 0)
    skip = 2;
  else if (count >= 1 && strcmp(tokens[0], "Spot") == 0)
    skip = 1;

  if (count > skip + 2) {
    strncpy(s->time, tokens[skip], sizeof(s->time) - 1);
    strncpy(s->freq, tokens[skip + 1], sizeof(s->freq) - 1);
    strncpy(s->call, tokens[skip + 2], sizeof(s->call) - 1);
  } else {
    strncpy(s->time, "?", sizeof(s->time) - 1);
    strncpy(s->freq, "?", sizeof(s->freq) - 1);
    strncpy(s->call, line, sizeof(s->call) - 1);
  }

  if (count > skip + 3) {
    size_t offset = 0;
    for (int i = skip + 3; i < count; i++) {
      if (offset < sizeof(s->comment) - 1) {
        offset += snprintf(s->comment + offset, sizeof(s->comment) - offset,
                           "%s%s", i == skip + 3 ? "" : " ", tokens[i]);
      }
    }
  } else {
    strncpy(s->comment, copy, sizeof(s->comment) - 1);
  }

  pthread_mutex_unlock(&dxcluster_mutex);
}

/*
 * Update the shared DXCluster status string.
 *
 * @param text Status message to store.
 * @return Nothing.
 */
void dxcluster_set_status(const char *text) {
  if (!text)
    return;

  char tmp[128];
  snprintf(tmp, sizeof(tmp), "%s", text);

  const size_t used = strnlen(tmp, sizeof(tmp));
  if (used < sizeof(tmp) - 1 && config.dxc_host[0]) {
    const int port_len = snprintf(NULL, 0, "%d", config.dxc_port);
    if (port_len > 0) {
      const size_t avail = sizeof(tmp) - used - 1;
      const size_t fixed = 4 + (size_t)port_len; /* " (" + ":" + ")" */

      if (avail > fixed) {
        size_t host_max = avail - fixed;
        size_t host_len = strnlen(config.dxc_host, sizeof(config.dxc_host));
        if (host_len > host_max)
          host_len = host_max;

        snprintf(tmp + used, sizeof(tmp) - used, " (%.*s:%d)", (int)host_len,
                 config.dxc_host, config.dxc_port);
      }
    }
  }

  pthread_mutex_lock(&dxcluster_mutex);
  snprintf(dxcluster_status, sizeof(dxcluster_status), "%s", tmp);
  pthread_mutex_unlock(&dxcluster_mutex);
}

/*
 * Send the initial DXCluster command sequence after login.
 *
 * @param sock Connected socket descriptor.
 * @return 0 on success, or -1 on failure.
 */
static int send_cluster_commands(int sock) {
  const char *cmds[] = {"set prompt off\r\n", "set/terse\r\n", "dx\r\n"};

  for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
    if (send(sock, cmds[i], strlen(cmds[i]), 0) < 0)
      return -1;

    usleep(200000);
  }

  return 0;
}

/*
 * Send the configured login callsign to the DXCluster server.
 *
 * @param sock Connected socket descriptor.
 * @return 0 on success, or -1 on failure.
 */
static int send_cluster_login_call(int sock) {
  if (!config.dxc_call[0])
    return 0;

  char login_cmd[64];
  snprintf(login_cmd, sizeof(login_cmd), "%s\r\n", config.dxc_call);

  if (send(sock, login_cmd, strlen(login_cmd), 0) < 0)
    return -1;

  usleep(200000);
  return 0;
}

/*
 * Detect common login prompts in incoming cluster text.
 *
 * @param text Input buffer to inspect.
 * @return true if the text resembles a login prompt, otherwise false.
 */
static bool looks_like_login_prompt(const char *text) {
  if (!text || !text[0])
    return false;

  if (strcasestr(text, "login"))
    return true;

  if (strcasestr(text, "callsign"))
    return true;

  if (strcasestr(text, "call:"))
    return true;

  return false;
}

/* ------------------------------------------------ */

/*
 * Worker thread that manages the DXCluster connection and spot feed.
 *
 * @param arg Unused thread argument.
 * @return NULL when the thread exits.
 */
static void *cluster_thread(void *arg) {
  (void)arg;

  dxcluster_debug_log("worker started (host=%s port=%d call=%s)", config.dxc_host,
                      config.dxc_port,
                      config.dxc_call[0] ? config.dxc_call : "<empty>");

  while (!stop_requested) {
    if (retry_requested) {
      retry_requested = 0;
      dxcluster_set_status("Retrying DXCluster...");
    }

    int sock = -1;

    dxcluster_debug_log("connection attempt started");

    dxcluster_set_status("Connecting...");

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
      dxcluster_set_status("Socket error");
      dxcluster_debug_log("socket() failed: errno=%d (%s)", errno,
                          strerror(errno));
      dxcluster_sleep_seconds(1);
      continue;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0)
      fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    dxcluster_set_status("Resolving host...");

    struct hostent *host = gethostbyname(config.dxc_host);
    if (!host) {
      dxcluster_set_status("DNS failed");
      dxcluster_debug_log("DNS lookup failed for '%s'", config.dxc_host);
      dxcluster_close_socket(&sock);
      dxcluster_sleep_seconds(5);
      continue;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config.dxc_port);
    memcpy(&addr.sin_addr, host->h_addr_list[0], host->h_length);

    dxcluster_set_status("Connecting to host...");
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
      if (errno != EINPROGRESS && errno != EWOULDBLOCK) {
        dxcluster_set_status("Connect failed");
        dxcluster_debug_log("connect() failed immediately: errno=%d (%s)",
                            errno, strerror(errno));
        dxcluster_close_socket(&sock);
        dxcluster_sleep_seconds(1);
        continue;
      }
    }

    int sel;
    do {
      sel = dxcluster_wait_for_socket(sock, 1, DXCLUSTER_CONNECT_TIMEOUT_SEC);
    } while (sel < 0 && errno == EINTR);

    if (sel < 0) {
      dxcluster_debug_log("connect wait failed: errno=%d (%s)", errno,
                          strerror(errno));
      dxcluster_close_socket(&sock);
      dxcluster_sleep_seconds(1);
      continue;
    }

    if (sel == 0) {
      dxcluster_set_status("Connect timeout");
      dxcluster_debug_log("connect timeout after %d seconds",
                          DXCLUSTER_CONNECT_TIMEOUT_SEC);
      dxcluster_close_socket(&sock);
      dxcluster_sleep_seconds(DXCLUSTER_RECONNECT_DELAY_SEC);
      continue;
    }

    int so_error = 0;
    socklen_t len = sizeof(so_error);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
    if (so_error != 0) {
      dxcluster_set_status("Connect failed");
      dxcluster_debug_log("connect completed with SO_ERROR=%d (%s)", so_error,
                          strerror(so_error));
      dxcluster_close_socket(&sock);
      dxcluster_sleep_seconds(1);
      continue;
    }

    dxcluster_set_status("Connected");
    dxcluster_debug_log("TCP connected to %s:%d", config.dxc_host,
                        config.dxc_port);
    dxcluster_set_active_socket(sock);

    bool login_sent = config.dxc_call[0] ? false : true;
    char buf[512];
    char pending[1024] = {0};
    size_t pending_len = 0;
    int login_wait_loops = 0;

    if (login_sent) {
      if (send_cluster_commands(sock) < 0) {
        dxcluster_set_status("Init command send failed");
        dxcluster_debug_log("send_cluster_commands() failed: errno=%d (%s)",
                            errno, strerror(errno));
        dxcluster_close_socket(&sock);
        dxcluster_sleep_seconds(1);
        continue;
      }

      dxcluster_set_status("Connected");
      dxcluster_debug_log("no login call configured, sent init commands");
    } else {
      dxcluster_set_status("Waiting for login prompt...");
      dxcluster_debug_log("waiting for login prompt to send call '%s'",
                          config.dxc_call);
    }

    while (1) {
      if (retry_requested) {
        dxcluster_debug_log("manual retry requested, closing active connection");
        break;
      }

      int n = 0;
      int ready = dxcluster_wait_for_socket(sock, 0, 1);
      if (ready < 0) {
        dxcluster_debug_log("read wait failed: errno=%d (%s)", errno,
                            strerror(errno));
        break;
      }

      n = read(sock, buf, sizeof(buf));
      if (n <= 0) {
        if (n == 0)
          dxcluster_debug_log("server closed the connection");
        else
          dxcluster_debug_log("read() failed: errno=%d (%s)", errno,
                              strerror(errno));
        break;
      }

      if ((size_t)n + pending_len >= sizeof(pending))
        pending_len = 0;

      memcpy(pending + pending_len, buf, (size_t)n);
      pending_len += (size_t)n;
      pending[pending_len] = 0;

      if (!login_sent && looks_like_login_prompt(pending)) {
        dxcluster_debug_log("login prompt detected in buffered data");
        if (send_cluster_login_call(sock) < 0) {
          dxcluster_set_status("Login send failed");
          dxcluster_debug_log("send_cluster_login_call() failed: errno=%d (%s)",
                              errno, strerror(errno));
          break;
        }

        if (send_cluster_commands(sock) < 0) {
          dxcluster_set_status("Command send failed");
          dxcluster_debug_log("send_cluster_commands() after login failed: errno=%d (%s)",
                              errno, strerror(errno));
          break;
        }

        login_sent = true;
        dxcluster_set_status("Logged in");
        dxcluster_debug_log("login sequence sent successfully");
      }

      char *line_start = pending;
      char *newline = NULL;

      while ((newline = strchr(line_start, '\n')) != NULL) {
        *newline = 0;
        char *line = line_start;
        if (*line == '\r')
          line++;

        while (*line && isspace((unsigned char)*line))
          line++;

        if (!login_sent && looks_like_login_prompt(line)) {
          dxcluster_debug_log("login prompt detected in line: '%s'", line);
          if (send_cluster_login_call(sock) < 0) {
            dxcluster_set_status("Login send failed");
            dxcluster_debug_log("send_cluster_login_call() failed: errno=%d (%s)",
                                errno, strerror(errno));
            break;
          }

          if (send_cluster_commands(sock) < 0) {
            dxcluster_set_status("Command send failed");
            dxcluster_debug_log("send_cluster_commands() after line prompt failed: errno=%d (%s)",
                                errno, strerror(errno));
            break;
          }

          login_sent = true;
          dxcluster_set_status("Logged in");
          line_start = newline + 1;
          continue;
        }

        if (line[0] != '\0' && dxcluster_line_is_dx_de(line)) {
          add_spot(line);
          dxcluster_set_status("Spot received");
        }

        line_start = newline + 1;
      }

      if (line_start != pending) {
        pending_len = strlen(line_start);
        memmove(pending, line_start, pending_len + 1);
      }

      if (!login_sent) {
        login_wait_loops++;
        if (login_wait_loops >= 50) {
          dxcluster_debug_log("login prompt timeout, sending fallback login sequence");
          if (send_cluster_login_call(sock) < 0) {
            dxcluster_set_status("Login fallback failed");
            dxcluster_debug_log("fallback send_cluster_login_call() failed: errno=%d (%s)",
                                errno, strerror(errno));
            break;
          }

          if (send_cluster_commands(sock) < 0) {
            dxcluster_set_status("Command send failed");
            dxcluster_debug_log("fallback send_cluster_commands() failed: errno=%d (%s)",
                                errno, strerror(errno));
            break;
          }

          login_sent = true;
          dxcluster_set_status("Login fallback sent");
        }
      }
    }

    dxcluster_close_socket(&sock);
    dxcluster_set_active_socket(-1);

    if (stop_requested) {
      break;
    }

    dxcluster_set_status("Reconnecting...");
    dxcluster_debug_log("connection lost, retrying in 1 second");
    dxcluster_sleep_seconds(1);
  }

  pthread_mutex_lock(&worker_mutex);
  worker_started = 0;
  active_socket = -1;
  pthread_mutex_unlock(&worker_mutex);
  dxcluster_set_status("Disconnected");
  dxcluster_debug_log("worker exiting");
  return NULL;
}

/* ------------------------------------------------ */

/*
 * Start the background DXCluster worker thread.
 *
 * @return 0 on success, or -1 on failure.
 */
int dxcluster_start(void) {
  dxcluster_debug_log("dxcluster_start() called");

  pthread_mutex_lock(&worker_mutex);
  if (worker_started) {
    pthread_mutex_unlock(&worker_mutex);
    return 0;
  }

  stop_requested = 0;
  retry_requested = 0;

  if (pthread_create(&thread_id, NULL, cluster_thread, NULL) != 0) {
    pthread_mutex_unlock(&worker_mutex);
    dxcluster_debug_log("pthread_create() failed: errno=%d (%s)", errno,
                        strerror(errno));
    return -1;
  }
  worker_started = 1;
  pthread_mutex_unlock(&worker_mutex);

  dxcluster_debug_log("dxcluster worker thread created");

  return 0;
}

int dxcluster_connect(void) {
  return dxcluster_start();
}

int dxcluster_retry_connection(void) {
  retry_requested = 1;
  dxcluster_set_status("Retrying DXCluster...");
  return dxcluster_start();
}

int dxcluster_send_spot(const char *spot_text) {
  if (!spot_text) {
    dxcluster_set_status("Spot send failed: empty text");
    return -1;
  }

  while (*spot_text && isspace((unsigned char)*spot_text))
    spot_text++;

  if (!*spot_text) {
    dxcluster_set_status("Spot send failed: empty text");
    return -1;
  }

  int sock = -1;
  pthread_mutex_lock(&worker_mutex);
  sock = active_socket;
  pthread_mutex_unlock(&worker_mutex);

  if (sock < 0) {
    dxcluster_set_status("Spot send failed: not connected");
    return -1;
  }

  char cmd[256];
  if (dxcluster_is_dx_command(spot_text))
    snprintf(cmd, sizeof(cmd), "%s\r\n", spot_text);
  else
    snprintf(cmd, sizeof(cmd), "dx %s\r\n", spot_text);

  if (send(sock, cmd, strlen(cmd), 0) < 0) {
    dxcluster_set_status("Spot send failed");
    dxcluster_debug_log("send(spot) failed: errno=%d (%s)", errno,
                        strerror(errno));
    return -1;
  }

  dxcluster_set_status("Spot sent");
  dxcluster_debug_log("spot command sent: '%s'", cmd);
  return 0;
}

/* ------------------------------------------------ */

/*
 * Stop the background DXCluster worker thread and release its resources.
 *
 * @return Nothing.
 */
void dxcluster_stop(void) {
  pthread_t worker;
  int sock = -1;

  pthread_mutex_lock(&worker_mutex);
  if (!worker_started) {
    pthread_mutex_unlock(&worker_mutex);
    dxcluster_set_status("Disconnected");
    return;
  }

  stop_requested = 1;
  retry_requested = 0;
  worker = thread_id;
  sock = active_socket;
  pthread_mutex_unlock(&worker_mutex);

  if (sock >= 0)
    shutdown(sock, SHUT_RDWR);

  pthread_join(worker, NULL);

  pthread_mutex_lock(&worker_mutex);
  worker_started = 0;
  active_socket = -1;
  pthread_mutex_unlock(&worker_mutex);

  dxcluster_set_status("Disconnected");
  dxcluster_debug_log("dxcluster_stop() completed");
}

void dxcluster_disconnect(void) {
  dxcluster_stop();
}
