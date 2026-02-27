// PgTgPiGpio — TCP/JSON GPIO relay control service
// Target: Raspberry Pi 4, Debian Trixie, libgpiod >= 2.0
//
// Config file (key = value, # comments):
//   port = 5555
//   gpio_chip = /dev/gpiochip0
//   output1 = 6
//   output2 = 13
//   ...
//
// JSON API (newline-delimited, connection stays open):
//   {"cmd":"set","output":"output1","value":1}      <- static ON
//   {"cmd":"set","output":"output1","value":0}      <- static OFF (cancels timer)
//   {"cmd":"set","output":"output1","value":30}     <- timed ON, 30 s countdown
//   {"cmd":"read"}                                  <- read all output states
//
// Timer behaviour:
//   value 0 or 1  — static; cancels any running timer for that output
//   value 2–999   — drives output ON; auto-forces OFF after that many seconds
//                   if a new value >=2 arrives before expiry, timer resets
//   On expiry the server sends {"status":"ok","output":"<name>","value":0}
//   on the open connection if a client is connected
//
// Compile: cc PgTgPiGpio.c -lgpiod -o PgTgPiGpio
// Cross-compile: see CMakeLists.txt and cmake/aarch64-rpi4-toolchain.cmake

/* Re-enable default glibc extensions (sigaction, usleep, etc.)
   suppressed by -std=c11.  _DEFAULT_SOURCE is the correct macro on
   Linux/glibc — matches the approach used in relaytest.c. */
#define _DEFAULT_SOURCE

#include <gpiod.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define MAX_OUTPUTS      16
#define MAX_NAME_LEN     32
#define MAX_PATH_LEN    128
#define RECV_BUF_SIZE   4096
#define SEND_BUF_SIZE    512
#define DEFAULT_PORT     5555
#define DEFAULT_CHIP     "/dev/gpiochip0"
#define LOCAL_CONF          "./PgTgPiGpio.conf"
#define SYSTEM_CONF         "/etc/PgTgPiGpio/PgTgPiGpio.conf"
#define CONSUMER_NAME    "PgTgPiGpio"
#define TIMER_MAX_SECS   999   /* values 2-999 are treated as countdown seconds */

/* ------------------------------------------------------------------ */
/* Data structures                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    char         name[MAX_NAME_LEN];   /* "output1" .. "output16"      */
    unsigned int offset;               /* GPIO chip offset (BCM number) */
} OutputDef;

typedef struct {
    int      port;
    char     gpio_chip[MAX_PATH_LEN];
    OutputDef outputs[MAX_OUTPUTS];
    int      num_outputs;
} AppConfig;

typedef struct {
    int    active;      /* 1 if counting down           */
    time_t expire_at;   /* absolute epoch when it fires */
} OutputTimer;

typedef struct {
    struct gpiod_chip           *chip;
    struct gpiod_line_settings  *settings;
    struct gpiod_line_config    *line_cfg;
    struct gpiod_request_config *req_cfg;
    struct gpiod_line_request   *request;
    const AppConfig             *cfg;                  /* back-pointer for offset lookup */
    OutputTimer                  timers[MAX_OUTPUTS];  /* per-output countdown timers    */
} GpioCtx;

typedef struct {
    char cmd[64];     /* "set" or "read"                  */
    char output[64];  /* "output1" etc.                   */
    int  value;       /* 0 or 1; -1 if absent             */
} ParsedCommand;

/* ------------------------------------------------------------------ */
/* Global signal flag                                                  */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_shutdown = 0;

/* ------------------------------------------------------------------ */
/* Utility                                                             */
/* ------------------------------------------------------------------ */

/* Trim leading and trailing whitespace in-place; returns trimmed pointer. */
static char *str_trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return s;
}

/* ------------------------------------------------------------------ */
/* Config parser                                                       */
/* ------------------------------------------------------------------ */

static void config_default(AppConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->port = DEFAULT_PORT;
    strncpy(cfg->gpio_chip, DEFAULT_CHIP, MAX_PATH_LEN - 1);
}

/* Parse one key=value line into cfg.  Returns 0 on success, -1 on fatal error. */
static int config_parse_line(char *line, AppConfig *cfg)
{
    /* Skip blank lines and comments */
    char *p = str_trim(line);
    if (*p == '\0' || *p == '#') return 0;

    char *eq = strchr(p, '=');
    if (!eq) {
        fprintf(stderr, "PgTgPiGpio: config: ignoring malformed line: %s\n", p);
        return 0;
    }

    *eq = '\0';
    char *key = str_trim(p);
    char *val = str_trim(eq + 1);

    if (strcmp(key, "port") == 0) {
        int port = atoi(val);
        if (port < 1 || port > 65535) {
            fprintf(stderr, "PgTgPiGpio: config: invalid port %d\n", port);
            return -1;
        }
        cfg->port = port;
    } else if (strcmp(key, "gpio_chip") == 0) {
        strncpy(cfg->gpio_chip, val, MAX_PATH_LEN - 1);
    } else if (strncmp(key, "output", 6) == 0) {
        /* output<N> = <gpio_offset> */
        const char *num_str = key + 6;
        if (*num_str == '\0') {
            fprintf(stderr, "PgTgPiGpio: config: 'output' missing index\n");
            return 0;
        }
        if (cfg->num_outputs >= MAX_OUTPUTS) {
            fprintf(stderr, "PgTgPiGpio: config: too many outputs (max %d)\n", MAX_OUTPUTS);
            return 0;
        }
        OutputDef *od = &cfg->outputs[cfg->num_outputs];
        snprintf(od->name, MAX_NAME_LEN, "output%s", num_str);
        od->offset = (unsigned int)atoi(val);
        cfg->num_outputs++;
    } else {
        fprintf(stderr, "PgTgPiGpio: config: unknown key '%s', ignored\n", key);
    }
    return 0;
}

/* Load config from file at path.  Returns 0 on success, -1 on error. */
static int config_load(const char *path, AppConfig *cfg)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        line[strcspn(line, "\r\n")] = '\0';
        if (config_parse_line(line, cfg) < 0) {
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* GPIO management                                                     */
/* ------------------------------------------------------------------ */

/* Return index of output named 'name', or -1 if not found. */
static int output_index_by_name(const AppConfig *cfg, const char *name)
{
    for (int i = 0; i < cfg->num_outputs; i++) {
        if (strcmp(cfg->outputs[i].name, name) == 0)
            return i;
    }
    return -1;
}

/* Initialize GPIO: open chip, configure all outputs, request lines.
   Mirrors the init sequence from relaytest.c exactly.
   Returns 0 on success, -1 on failure (partial resources freed). */
static int gpio_init(GpioCtx *ctx, const AppConfig *cfg)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->cfg = cfg;

    /* Build a flat offsets array from config */
    unsigned int offsets[MAX_OUTPUTS];
    for (int i = 0; i < cfg->num_outputs; i++)
        offsets[i] = cfg->outputs[i].offset;

    fprintf(stderr, "PgTgPiGpio: opening %s ...\n", cfg->gpio_chip);
    ctx->chip = gpiod_chip_open(cfg->gpio_chip);
    if (!ctx->chip) { perror("gpiod_chip_open"); goto fail; }

    ctx->settings = gpiod_line_settings_new();
    if (!ctx->settings) { perror("gpiod_line_settings_new"); goto fail; }
    gpiod_line_settings_set_direction(ctx->settings, GPIOD_LINE_DIRECTION_OUTPUT);
    /* All relays start de-energized (INACTIVE = LOW for active-high modules) */
    gpiod_line_settings_set_output_value(ctx->settings, GPIOD_LINE_VALUE_INACTIVE);

    ctx->line_cfg = gpiod_line_config_new();
    if (!ctx->line_cfg) { perror("gpiod_line_config_new"); goto fail; }
    if (gpiod_line_config_add_line_settings(ctx->line_cfg, offsets,
                                             (size_t)cfg->num_outputs,
                                             ctx->settings) < 0) {
        perror("gpiod_line_config_add_line_settings");
        goto fail;
    }

    ctx->req_cfg = gpiod_request_config_new();
    if (!ctx->req_cfg) { perror("gpiod_request_config_new"); goto fail; }
    gpiod_request_config_set_consumer(ctx->req_cfg, CONSUMER_NAME);

    ctx->request = gpiod_chip_request_lines(ctx->chip, ctx->req_cfg, ctx->line_cfg);
    if (!ctx->request) { perror("gpiod_chip_request_lines"); goto fail; }

    fprintf(stderr, "PgTgPiGpio: %d GPIO line(s) requested OK\n", cfg->num_outputs);
    return 0;

fail:
    /* Cleanup whatever was partially allocated (mirrors relaytest.c cleanup) */
    if (ctx->request)  gpiod_line_request_release(ctx->request);
    if (ctx->req_cfg)  gpiod_request_config_free(ctx->req_cfg);
    if (ctx->line_cfg) gpiod_line_config_free(ctx->line_cfg);
    if (ctx->settings) gpiod_line_settings_free(ctx->settings);
    if (ctx->chip)     gpiod_chip_close(ctx->chip);
    memset(ctx, 0, sizeof(*ctx));
    return -1;
}

/* Release all GPIO resources.  Safe to call with a partially-initialized ctx. */
static void gpio_cleanup(GpioCtx *ctx)
{
    if (ctx->request)  gpiod_line_request_release(ctx->request);
    if (ctx->req_cfg)  gpiod_request_config_free(ctx->req_cfg);
    if (ctx->line_cfg) gpiod_line_config_free(ctx->line_cfg);
    if (ctx->settings) gpiod_line_settings_free(ctx->settings);
    if (ctx->chip)     gpiod_chip_close(ctx->chip);
    memset(ctx, 0, sizeof(*ctx));
}

/* Set named output to value (0=off/INACTIVE, 1=on/ACTIVE).
   Returns 0 on success, -1 if name not found. */
static int gpio_set(GpioCtx *ctx, const char *name, int value)
{
    int idx = output_index_by_name(ctx->cfg, name);
    if (idx < 0) return -1;

    enum gpiod_line_value gv = (value == 1) ? GPIOD_LINE_VALUE_ACTIVE
                                             : GPIOD_LINE_VALUE_INACTIVE;
    gpiod_line_request_set_value(ctx->request,
                                 ctx->cfg->outputs[idx].offset, gv);
    return 0;
}

/* Get current value of named output.
   Returns 0 or 1 on success, -1 if name not found or API error. */
static int gpio_get(GpioCtx *ctx, const char *name)
{
    int idx = output_index_by_name(ctx->cfg, name);
    if (idx < 0) return -1;

    enum gpiod_line_value gv =
        gpiod_line_request_get_value(ctx->request,
                                     ctx->cfg->outputs[idx].offset);
    if (gv == GPIOD_LINE_VALUE_ACTIVE)   return 1;
    if (gv == GPIOD_LINE_VALUE_INACTIVE) return 0;
    return -1;  /* GPIOD_LINE_VALUE_ERROR */
}

/* ------------------------------------------------------------------ */
/* Countdown timers                                                    */
/* ------------------------------------------------------------------ */

/* Start (or restart) a countdown timer for output[idx].
   The output should already be driven ON before calling this. */
static void timer_start(GpioCtx *ctx, int idx, int seconds)
{
    ctx->timers[idx].active    = 1;
    ctx->timers[idx].expire_at = time(NULL) + (time_t)seconds;
    fprintf(stderr, "PgTgPiGpio: timer set: %s expires in %ds\n",
            ctx->cfg->outputs[idx].name, seconds);
}

/* Cancel any active timer for output[idx]. */
static void timer_cancel(GpioCtx *ctx, int idx)
{
    if (ctx->timers[idx].active) {
        ctx->timers[idx].active = 0;
        fprintf(stderr, "PgTgPiGpio: timer cancelled: %s\n",
                ctx->cfg->outputs[idx].name);
    }
}

/* Check all timers; force any expired output OFF and, if a client is
   connected (client_fd >= 0), send the expiry notification on that fd. */
static void timer_check_all(GpioCtx *ctx, int client_fd)
{
    time_t now = time(NULL);
    for (int i = 0; i < ctx->cfg->num_outputs; i++) {
        if (!ctx->timers[i].active) continue;
        if (now < ctx->timers[i].expire_at) continue;

        ctx->timers[i].active = 0;
        gpiod_line_request_set_value(ctx->request,
                                     ctx->cfg->outputs[i].offset,
                                     GPIOD_LINE_VALUE_INACTIVE);
        fprintf(stderr, "PgTgPiGpio: timer expired: %s -> OFF\n",
                ctx->cfg->outputs[i].name);

        if (client_fd >= 0) {
            char resp[SEND_BUF_SIZE];
            snprintf(resp, sizeof(resp),
                     "{\"status\":\"ok\",\"output\":\"%s\",\"value\":0}\n",
                     ctx->cfg->outputs[i].name);
            send(client_fd, resp, strlen(resp), 0);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Minimal JSON parser                                                 */
/* ------------------------------------------------------------------ */

/* Extract string value for 'key' from json into buf[bufsz].
   Looks for: "key" : "value"
   Returns 0 on success, -1 if key absent or value not a string. */
static int json_extract_string(const char *json, const char *key,
                                char *buf, size_t bufsz)
{
    /* Build search pattern: "key" */
    char pattern[MAX_NAME_LEN + 3];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);

    /* Skip whitespace and colon */
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ':') return -1;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return -1;
    p++;

    /* Copy until closing quote or buffer full */
    size_t i = 0;
    while (*p && *p != '"' && i < bufsz - 1)
        buf[i++] = *p++;
    buf[i] = '\0';
    return (*p == '"') ? 0 : -1;
}

/* Extract integer value for 'key' from json into *out_val.
   Looks for: "key" : <integer>
   Returns 0 on success, -1 if key absent. */
static int json_extract_int(const char *json, const char *key, int *out_val)
{
    char pattern[MAX_NAME_LEN + 3];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ':') return -1;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;

    if (*p != '-' && !isdigit((unsigned char)*p)) return -1;

    char *end;
    *out_val = (int)strtol(p, &end, 10);
    return (end != p) ? 0 : -1;
}

/* Parse a JSON command line into *out.
   Returns 0 if 'cmd' field was found, -1 otherwise. */
static int json_parse_command(const char *line, ParsedCommand *out)
{
    if (json_extract_string(line, "cmd", out->cmd, sizeof(out->cmd)) < 0)
        return -1;
    /* output and value are optional (not present in "read" command) */
    json_extract_string(line, "output", out->output, sizeof(out->output));
    json_extract_int(line, "value", &out->value);
    return 0;
}

/* ------------------------------------------------------------------ */
/* JSON builders (snprintf-based, no heap allocation)                  */
/* ------------------------------------------------------------------ */

/* {"status":"error","message":"<msg>"}\n */
static int json_build_error(char *buf, size_t bufsz, const char *message)
{
    return snprintf(buf, bufsz,
                    "{\"status\":\"error\",\"message\":\"%s\"}\n", message);
}

/* {"status":"ok","output":"<name>","value":<v>}\n */
static int json_build_ok_set(char *buf, size_t bufsz,
                              const char *output_name, int value)
{
    return snprintf(buf, bufsz,
                    "{\"status\":\"ok\",\"output\":\"%s\",\"value\":%d}\n",
                    output_name, value);
}

/* {"status":"ok","outputs":{"output1":v1,"output2":v2,...}}\n */
static int json_build_ok_read(char *buf, size_t bufsz,
                               const AppConfig *cfg, GpioCtx *gpio)
{
    int n = snprintf(buf, bufsz, "{\"status\":\"ok\",\"outputs\":{");
    if (n < 0 || (size_t)n >= bufsz) return n;

    size_t pos = (size_t)n;
    for (int i = 0; i < cfg->num_outputs; i++) {
        int v = gpio_get(gpio, cfg->outputs[i].name);
        if (v < 0) v = 0;  /* treat error as off */

        int written = snprintf(buf + pos, bufsz - pos,
                                "%s\"%s\":%d",
                                (i == 0) ? "" : ",",
                                cfg->outputs[i].name, v);
        if (written < 0 || pos + (size_t)written >= bufsz) break;
        pos += (size_t)written;
    }

    int tail = snprintf(buf + pos, bufsz - pos, "}}\n");
    if (tail > 0) pos += (size_t)tail;
    return (int)pos;
}

/* ------------------------------------------------------------------ */
/* Command dispatchers                                                 */
/* ------------------------------------------------------------------ */

static void cmd_set(int client_fd, GpioCtx *gpio,
                    const AppConfig *cfg, const ParsedCommand *cmd)
{
    char resp[SEND_BUF_SIZE];

    if (cmd->output[0] == '\0') {
        json_build_error(resp, sizeof(resp), "missing output field");
        send(client_fd, resp, strlen(resp), 0);
        return;
    }
    if (cmd->value < 0 || cmd->value > TIMER_MAX_SECS) {
        json_build_error(resp, sizeof(resp),
                         "value must be 0, 1, or 2-999 (timed ON seconds)");
        send(client_fd, resp, strlen(resp), 0);
        return;
    }
    int idx = output_index_by_name(cfg, cmd->output);
    if (idx < 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "unknown output: %s", cmd->output);
        json_build_error(resp, sizeof(resp), msg);
        send(client_fd, resp, strlen(resp), 0);
        return;
    }

    if (cmd->value >= 2) {
        /* Timer mode: start (or reset) countdown.
           Only drive ON if the output isn't already active — avoids
           toggling a channel that was previously set ON with value=1. */
        if (gpio_get(gpio, cmd->output) != 1)
            gpio_set(gpio, cmd->output, 1);
        timer_start(gpio, idx, cmd->value);
    } else {
        /* Static mode: 0 or 1 — cancel any running timer, set directly */
        timer_cancel(gpio, idx);
        gpio_set(gpio, cmd->output, cmd->value);
        fprintf(stderr, "PgTgPiGpio: set %s = %d\n", cmd->output, cmd->value);
    }

    json_build_ok_set(resp, sizeof(resp), cmd->output, cmd->value);
    send(client_fd, resp, strlen(resp), 0);
}

static void cmd_read(int client_fd, GpioCtx *gpio, const AppConfig *cfg)
{
    char resp[SEND_BUF_SIZE];
    json_build_ok_read(resp, sizeof(resp), cfg, gpio);
    send(client_fd, resp, strlen(resp), 0);
}

/* ------------------------------------------------------------------ */
/* TCP server                                                          */
/* ------------------------------------------------------------------ */

/* Read from client_fd until '\n', filling buf[bufsz].
   Returns byte count on success, 0 on disconnect, -1 on overflow. */
static int recv_line(int client_fd, char *buf, size_t bufsz)
{
    size_t len = 0;
    while (len < bufsz - 1) {
        int n = (int)recv(client_fd, buf + len, 1, 0);
        if (n <= 0) return 0;
        len++;
        if (buf[len - 1] == '\n') break;
    }
    buf[len] = '\0';
    if (len == bufsz - 1 && buf[len - 1] != '\n') return -1;
    return (int)len;
}

/* Read and process one command from client_fd.
   Returns 1 if a command was handled (connection stays open),
           0 if the client disconnected cleanly,
          -1 if the receive buffer overflowed (connection should be dropped). */
static int handle_command(int client_fd, GpioCtx *gpio, const AppConfig *cfg)
{
    char buf[RECV_BUF_SIZE];
    char resp[SEND_BUF_SIZE];

    int n = recv_line(client_fd, buf, sizeof(buf));
    if (n <= 0) {
        if (n < 0) {
            json_build_error(resp, sizeof(resp), "command too long");
            send(client_fd, resp, strlen(resp), 0);
        }
        return n;
    }

    ParsedCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.value = -1;

    if (json_parse_command(buf, &cmd) < 0) {
        json_build_error(resp, sizeof(resp), "invalid JSON: missing cmd");
        send(client_fd, resp, strlen(resp), 0);
        return 1;
    }

    if (strcmp(cmd.cmd, "set") == 0) {
        cmd_set(client_fd, gpio, cfg, &cmd);
    } else if (strcmp(cmd.cmd, "read") == 0) {
        cmd_read(client_fd, gpio, cfg);
    } else {
        char msg[80];
        snprintf(msg, sizeof(msg), "unknown command: %s", cmd.cmd);
        json_build_error(resp, sizeof(resp), msg);
        send(client_fd, resp, strlen(resp), 0);
    }
    return 1;
}

/* Create and bind the TCP listen socket.
   Returns fd on success, -1 on error. */
static int tcp_server_create(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, 4) < 0) {
        perror("listen"); close(fd); return -1;
    }
    return fd;
}

/* Main accept loop.  Keeps one client connection open persistently.
   Returns when g_shutdown is set. */
static void tcp_server_loop(int listen_fd, GpioCtx *gpio, const AppConfig *cfg)
{
    fprintf(stderr, "PgTgPiGpio: listening on port %d\n", cfg->port);

    int client_fd = -1;

    while (!g_shutdown) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        int maxfd = listen_fd;

        if (client_fd >= 0) {
            FD_SET(client_fd, &rfds);
            if (client_fd > maxfd) maxfd = client_fd;
        }

        /* 1-second timeout drives timer expiry checks */
        struct timeval tv;
        tv.tv_sec  = 1;
        tv.tv_usec = 0;

        int ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;  /* signal — recheck g_shutdown */
            perror("select");
            break;
        }

        /* Check timers on every wakeup (timeout or data) */
        timer_check_all(gpio, client_fd);

        /* New incoming connection */
        if (FD_ISSET(listen_fd, &rfds)) {
            int new_fd = accept(listen_fd, NULL, NULL);
            if (new_fd < 0) {
                if (errno != EINTR) perror("accept");
            } else {
                if (client_fd >= 0) {
                    /* Replace previous client with the new one */
                    fprintf(stderr, "PgTgPiGpio: new connection, replacing previous client\n");
                    close(client_fd);
                }
                client_fd = new_fd;
                fprintf(stderr, "PgTgPiGpio: client connected\n");
            }
        }

        /* Data (or disconnect) from the connected client */
        if (client_fd >= 0 && FD_ISSET(client_fd, &rfds)) {
            if (handle_command(client_fd, gpio, cfg) <= 0) {
                close(client_fd);
                client_fd = -1;
                fprintf(stderr, "PgTgPiGpio: client disconnected\n");
            }
        }
    }

    if (client_fd >= 0) close(client_fd);
    close(listen_fd);
    fprintf(stderr, "PgTgPiGpio: server loop exited\n");
}

/* ------------------------------------------------------------------ */
/* Signal handler                                                      */
/* ------------------------------------------------------------------ */

static void signal_handler(int sig)
{
    (void)sig;
    g_shutdown = 1;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    AppConfig cfg;
    config_default(&cfg);

    /* Locate config file.
       Search order:
         1. argv[1]                              — explicit override
         2. ./PgTgPiGpio.conf                    — development / local config
         3. /etc/PgTgPiGpio/PgTgPiGpio.conf     — system config (created by postinst) */
    const char *conf_path = NULL;
    if (argc > 1) {
        conf_path = argv[1];
    } else if (access(LOCAL_CONF, R_OK) == 0) {
        conf_path = LOCAL_CONF;
    } else if (access(SYSTEM_CONF, R_OK) == 0) {
        conf_path = SYSTEM_CONF;
    } else {
        fprintf(stderr,
                "PgTgPiGpio: no config file found\n"
                "  tried: %s\n"
                "  tried: %s\n"
                "  usage: %s [config_file]\n",
                LOCAL_CONF, SYSTEM_CONF, argv[0]);
        return EXIT_FAILURE;
    }

    fprintf(stderr, "PgTgPiGpio: loading config from %s\n", conf_path);
    if (config_load(conf_path, &cfg) < 0)
        return EXIT_FAILURE;

    if (cfg.num_outputs == 0) {
        fprintf(stderr, "PgTgPiGpio: no outputs configured — nothing to do\n");
        return EXIT_FAILURE;
    }

    fprintf(stderr, "PgTgPiGpio: %d output(s) configured, port %d\n",
            cfg.num_outputs, cfg.port);

    /* Register signal handlers — no SA_RESTART so select() returns EINTR */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Initialize GPIO */
    GpioCtx gpio;
    if (gpio_init(&gpio, &cfg) < 0)
        return EXIT_FAILURE;

    /* Create TCP listener */
    int listen_fd = tcp_server_create(cfg.port);
    if (listen_fd < 0) {
        gpio_cleanup(&gpio);
        return EXIT_FAILURE;
    }

    /* Run until SIGINT/SIGTERM */
    tcp_server_loop(listen_fd, &gpio, &cfg);

    /* Clean shutdown */
    fprintf(stderr, "PgTgPiGpio: shutting down, releasing GPIO lines\n");
    gpio_cleanup(&gpio);
    return EXIT_SUCCESS;
}
