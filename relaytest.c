// Note: this is a working example but is here to use as a template 
//  It is not used in PgTgPiGpio.

// RPi4 relay sequencer — libgpiod v2 API
// Pins 31,33,35,37 = BCM GPIO 6,13,19,26 = gpiochip0 offsets 6,13,19,26
// Active-high relay modules (transistor buffer): HIGH = relay ON, LOW = relay OFF
// Cycle: each relay ON in sequence (500ms apart), then OFF in sequence (500ms apart)
// Compile: cc relaytest.c -lgpiod -o relaytest  (Debian Trixie, libgpiod >= 2.0)
// Cross-compile: see CMakeLists.txt and cmake/aarch64-rpi4-toolchain.cmake

/* Re-enable default glibc extensions (usleep etc.) suppressed by -std=c11.
   usleep() was removed from POSIX.1-2008 so _POSIX_C_SOURCE won't expose it;
   _DEFAULT_SOURCE is the correct way to get it on Linux/glibc. */
#define _DEFAULT_SOURCE

#include <gpiod.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define GPIO_CHIP   "/dev/gpiochip0"
#define CONSUMER    "relay-seq"
#define STEP_US     500000   /* 500 ms between steps */
#define NUM_RELAYS  4

/* Physical pins 31,33,35,37 -> BCM GPIO 6,13,19,26 */
static const unsigned int offsets[NUM_RELAYS] = {6, 13, 19, 26};

/* Physical pin numbers for display only */
static const unsigned int phys_pins[NUM_RELAYS] = {31, 33, 35, 37};

int main(void)
{
    struct gpiod_chip           *chip     = NULL;
    struct gpiod_line_settings  *settings = NULL;
    struct gpiod_line_config    *line_cfg = NULL;
    struct gpiod_request_config *req_cfg  = NULL;
    struct gpiod_line_request   *request  = NULL;
    int ret = EXIT_FAILURE;

    printf("Opening %s ...\n", GPIO_CHIP);
    chip = gpiod_chip_open(GPIO_CHIP);
    if (!chip) { perror("gpiod_chip_open"); goto cleanup; }
    printf("  chip opened OK\n");

    settings = gpiod_line_settings_new();
    if (!settings) { perror("gpiod_line_settings_new"); goto cleanup; }
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
    /* Active-high: LOW = relay off. Start all relays de-energized. */
    gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_INACTIVE);

    line_cfg = gpiod_line_config_new();
    if (!line_cfg) { perror("gpiod_line_config_new"); goto cleanup; }
    if (gpiod_line_config_add_line_settings(line_cfg, offsets, NUM_RELAYS, settings) < 0) {
        perror("gpiod_line_config_add_line_settings"); goto cleanup;
    }

    req_cfg = gpiod_request_config_new();
    if (!req_cfg) { perror("gpiod_request_config_new"); goto cleanup; }
    gpiod_request_config_set_consumer(req_cfg, CONSUMER);

    printf("Requesting lines: offsets {%u, %u, %u, %u} (physical pins %u,%u,%u,%u) ...\n",
           offsets[0], offsets[1], offsets[2], offsets[3],
           phys_pins[0], phys_pins[1], phys_pins[2], phys_pins[3]);
    request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
    if (!request) { perror("gpiod_chip_request_lines"); goto cleanup; }
    printf("  lines requested OK — starting sequence (Ctrl-C to stop)\n\n");

    int cycle = 0;
    while (1) {
        printf("--- Cycle %d ---\n", ++cycle);

        /* Energize relays one at a time (HIGH = ON for active-high modules) */
        for (int i = 0; i < NUM_RELAYS; i++) {
            printf("  Relay %d (pin %u, offset %u): ON  (HIGH)\n",
                   i + 1, phys_pins[i], offsets[i]);
            fflush(stdout);
            gpiod_line_request_set_value(request, offsets[i], GPIOD_LINE_VALUE_ACTIVE);
            usleep(STEP_US);
        }

        /* De-energize relays one at a time (LOW = OFF) */
        for (int i = 0; i < NUM_RELAYS; i++) {
            printf("  Relay %d (pin %u, offset %u): OFF (LOW)\n",
                   i + 1, phys_pins[i], offsets[i]);
            fflush(stdout);
            gpiod_line_request_set_value(request, offsets[i], GPIOD_LINE_VALUE_INACTIVE);
            usleep(STEP_US);
        }
    }
    ret = EXIT_SUCCESS;

cleanup:
    if (request)  gpiod_line_request_release(request);
    if (req_cfg)  gpiod_request_config_free(req_cfg);
    if (line_cfg) gpiod_line_config_free(line_cfg);
    if (settings) gpiod_line_settings_free(settings);
    if (chip)     gpiod_chip_close(chip);
    return ret;
}
