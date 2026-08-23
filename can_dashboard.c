/*
 * can_dashboard.c
 * Linux SocketCAN decoder and ANSI dashboard for vcan0.
 *
 * Build:
 *   gcc -std=c11 -Wall -Wextra -Wpedantic -O2 can_dashboard.c -o can_dashboard
 */

#define _GNU_SOURCE

#include <errno.h>
#include <net/if.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/can.h>
#include <linux/can/raw.h>

#define CAN_IFACE "vcan0"

static volatile sig_atomic_t keep_running = 1;

typedef struct
{
    double speed_kmh;
    double engine_rpm;
    double coolant_c;
    double fuel_percent;
    double battery_v;
    double ambient_c;
    int got_vehicle;
    int got_thermal;
    int got_power;
} telemetry_t;

static void stop_program(int signal_number)
{
    (void)signal_number;
    keep_running = 0;
}

static int open_can_socket(const char *interface_name)
{
    int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));

    if (snprintf(ifr.ifr_name, IFNAMSIZ, "%s", interface_name) >= IFNAMSIZ) {
        fprintf(stderr, "Interface name is too long: %s\n", interface_name);
        close(fd);
        return -1;
    }

    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        perror("ioctl(SIOCGIFINDEX)");
        close(fd);
        return -1;
    }

    struct sockaddr_can address;
    memset(&address, 0, sizeof(address));
    address.can_family = AF_CAN;
    address.can_ifindex = ifr.ifr_ifindex;

    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    return fd;
}

static uint16_t get_le16(const uint8_t *source)
{
    return (uint16_t)source[0] | ((uint16_t)source[1] << 8U);
}

static double decode_speed(uint16_t raw)
{
    return (double)raw * 0.01;
}

static double decode_rpm(uint16_t raw)
{
    return (double)raw;
}

static double decode_coolant(uint16_t raw)
{
    return (double)raw * 0.1 - 40.0;
}

static double decode_fuel(uint8_t raw)
{
    return (double)raw * 0.5;
}

static double decode_battery(uint16_t raw)
{
    return (double)raw * 0.01;
}

static double decode_ambient(uint8_t raw)
{
    return (double)raw - 40.0;
}

static void update_from_frame(const struct can_frame *frame, telemetry_t *state)
{
    const canid_t id = frame->can_id & CAN_SFF_MASK;

    if (id == 0x100U && frame->can_dlc >= 4U) {
        const uint16_t speed_raw = get_le16(&frame->data[0]);
        const uint16_t rpm_raw = get_le16(&frame->data[2]);

        state->speed_kmh = decode_speed(speed_raw);
        state->engine_rpm = decode_rpm(rpm_raw);
        state->got_vehicle = 1;
    } else if (id == 0x101U && frame->can_dlc >= 3U) {
        const uint16_t coolant_raw = get_le16(&frame->data[0]);
        const uint8_t fuel_raw = frame->data[2];

        state->coolant_c = decode_coolant(coolant_raw);
        state->fuel_percent = decode_fuel(fuel_raw);
        state->got_thermal = 1;
    } else if (id == 0x102U && frame->can_dlc >= 3U) {
        const uint16_t battery_raw = get_le16(&frame->data[0]);
        const uint8_t ambient_raw = frame->data[2];

        state->battery_v = decode_battery(battery_raw);
        state->ambient_c = decode_ambient(ambient_raw);
        state->got_power = 1;
    }
}

static void draw_dashboard(const telemetry_t *state)
{
    printf("\033[2J\033[H");
    printf("==============================================================\n");
    printf("              VIRTUAL VEHICLE CAN DASHBOARD                  \n");
    printf("==============================================================\n");
    printf("  CAN Interface : %-10s\n", CAN_IFACE);
    printf("--------------------------------------------------------------\n");
    printf("  VehicleStatus  [0x100]\n");
    printf("    Vehicle Speed       : %8.2f km/h %s\n",
           state->speed_kmh, state->got_vehicle ? "" : "(waiting)");
    printf("    Engine RPM          : %8.0f rpm\n", state->engine_rpm);
    printf("--------------------------------------------------------------\n");
    printf("  ThermalFuel    [0x101]\n");
    printf("    Coolant Temperature : %8.1f degC %s\n",
           state->coolant_c, state->got_thermal ? "" : "(waiting)");
    printf("    Fuel Level          : %8.1f %%\n", state->fuel_percent);
    printf("--------------------------------------------------------------\n");
    printf("  PowerStatus    [0x102]\n");
    printf("    Battery Voltage     : %8.2f V %s\n",
           state->battery_v, state->got_power ? "" : "(waiting)");
    printf("    Ambient Temperature : %8.1f degC\n", state->ambient_c);
    printf("--------------------------------------------------------------\n");
    printf("  Press Ctrl+C to stop.\n");
    printf("==============================================================\n");
    fflush(stdout);
}

int main(void)
{
    signal(SIGINT, stop_program);
    signal(SIGTERM, stop_program);

    int socket_fd = open_can_socket(CAN_IFACE);
    if (socket_fd < 0) {
        return EXIT_FAILURE;
    }

    telemetry_t state;
    memset(&state, 0, sizeof(state));

    printf("Dashboard listening on %s...\n", CAN_IFACE);

    while (keep_running) {
        struct can_frame frame;
        ssize_t received = read(socket_fd, &frame, sizeof(frame));

        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("read");
            close(socket_fd);
            return EXIT_FAILURE;
        }

        if (received != (ssize_t)sizeof(frame)) {
            fprintf(stderr, "Unexpected CAN frame size: %zd\n", received);
            continue;
        }

        if (frame.can_id & CAN_EFF_FLAG) {
            continue;
        }

        update_from_frame(&frame, &state);
        draw_dashboard(&state);
    }

    close(socket_fd);
    printf("Dashboard stopped.\n");
    return EXIT_SUCCESS;
}
