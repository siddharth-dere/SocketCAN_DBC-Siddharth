/*
 * can_transmitter.c
 * Linux SocketCAN telemetry generator for vcan0.
 *
 * Build:
 *   gcc -std=c11 -Wall -Wextra -Wpedantic -O2 can_transmitter.c -lm -o can_transmitter
 */

#define _GNU_SOURCE

#include <errno.h>
#include <math.h>
#include <net/if.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <linux/can.h>
#include <linux/can/raw.h>

#define CAN_IFACE "vcan0"
#define TX_PERIOD_MS 100

static volatile sig_atomic_t keep_running = 1;

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

static uint16_t clamp_u16_from_double(double value, uint16_t minimum, uint16_t maximum)
{
    if (value <= (double)minimum) {
        return minimum;
    }
    if (value >= (double)maximum) {
        return maximum;
    }
    return (uint16_t)llround(value);
}

static uint8_t clamp_u8_from_double(double value, uint8_t minimum, uint8_t maximum)
{
    if (value <= (double)minimum) {
        return minimum;
    }
    if (value >= (double)maximum) {
        return maximum;
    }
    return (uint8_t)llround(value);
}

static void put_le16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value & 0xFFU);
    destination[1] = (uint8_t)((value >> 8U) & 0xFFU);
}

static uint16_t encode_speed(double kmh)
{
    const double factor = 0.01;
    return clamp_u16_from_double(kmh / factor, 0U, 12000U);
}

static uint16_t encode_rpm(double rpm)
{
    return clamp_u16_from_double(rpm, 800U, 5000U);
}

static uint16_t encode_coolant(double celsius)
{
    const double factor = 0.1;
    const double offset = -40.0;
    return clamp_u16_from_double((celsius - offset) / factor, 0U, 1600U);
}

static uint8_t encode_fuel(double percent)
{
    const double factor = 0.5;
    return clamp_u8_from_double(percent / factor, 0U, 200U);
}

static uint16_t encode_battery(double volts)
{
    const double factor = 0.01;
    return clamp_u16_from_double(volts / factor, 1100U, 1500U);
}

static uint8_t encode_ambient(double celsius)
{
    const double offset = -40.0;
    return clamp_u8_from_double(celsius - offset, 0U, 140U);
}

static int transmit_frame(int socket_fd, canid_t identifier, const uint8_t data[8])
{
    struct can_frame frame;
    memset(&frame, 0, sizeof(frame));

    frame.can_id = identifier;
    frame.can_dlc = 8;
    memcpy(frame.data, data, sizeof(frame.data));

    ssize_t sent = write(socket_fd, &frame, sizeof(frame));
    if (sent != (ssize_t)sizeof(frame)) {
        if (sent < 0) {
            fprintf(stderr, "write(ID 0x%03X): %s\n", identifier, strerror(errno));
        } else {
            fprintf(stderr, "Partial CAN write for ID 0x%03X\n", identifier);
        }
        return -1;
    }

    return 0;
}

static void build_vehicle_status(uint8_t data[8], double speed, double rpm)
{
    memset(data, 0, 8);
    put_le16(&data[0], encode_speed(speed));
    put_le16(&data[2], encode_rpm(rpm));
}

static void build_thermal_fuel(uint8_t data[8], double coolant, double fuel)
{
    memset(data, 0, 8);
    put_le16(&data[0], encode_coolant(coolant));
    data[2] = encode_fuel(fuel);
}

static void build_power_status(uint8_t data[8], double battery, double ambient)
{
    memset(data, 0, 8);
    put_le16(&data[0], encode_battery(battery));
    data[2] = encode_ambient(ambient);
}

static double now_seconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0.0;
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void sleep_milliseconds(long milliseconds)
{
    struct timespec request;
    request.tv_sec = milliseconds / 1000L;
    request.tv_nsec = (milliseconds % 1000L) * 1000000L;

    while (nanosleep(&request, &request) != 0 && errno == EINTR && keep_running) {
    }
}

int main(void)
{
    signal(SIGINT, stop_program);
    signal(SIGTERM, stop_program);

    int socket_fd = open_can_socket(CAN_IFACE);
    if (socket_fd < 0) {
        return EXIT_FAILURE;
    }

    printf("CAN transmitter started on %s\n", CAN_IFACE);
    printf("IDs: 0x100 VehicleStatus, 0x101 ThermalFuel, 0x102 PowerStatus\n");

    const double start_time = now_seconds();

    while (keep_running) {
        const double t = now_seconds() - start_time;

        double speed = 60.0 + 55.0 * sin(0.18 * t);
        double rpm = 2500.0 + 1600.0 * sin(0.23 * t + 0.8);
        double coolant = 75.0 + 18.0 * sin(0.07 * t + 0.4);
        double fuel = 62.0 - 0.018 * t;
        double battery = 13.4 + 0.7 * sin(0.31 * t);
        double ambient = 28.0 + 12.0 * sin(0.025 * t - 0.6);

        if (fuel < 0.0) {
            fuel = 0.0;
        }

        uint8_t status_data[8];
        uint8_t thermal_data[8];
        uint8_t power_data[8];

        build_vehicle_status(status_data, speed, rpm);
        build_thermal_fuel(thermal_data, coolant, fuel);
        build_power_status(power_data, battery, ambient);

        if (transmit_frame(socket_fd, 0x100U, status_data) != 0 ||
            transmit_frame(socket_fd, 0x101U, thermal_data) != 0 ||
            transmit_frame(socket_fd, 0x102U, power_data) != 0) {
            close(socket_fd);
            return EXIT_FAILURE;
        }

        printf(
            "\rSpeed %6.2f km/h | RPM %6.0f | Coolant %6.1f C | Fuel %5.1f %% | "
            "Battery %4.2f V | Ambient %5.1f C",
            speed, rpm, coolant, fuel, battery, ambient
        );
        fflush(stdout);

        sleep_milliseconds(TX_PERIOD_MS);
    }

    printf("\nTransmitter stopped.\n");
    close(socket_fd);
    return EXIT_SUCCESS;
}
