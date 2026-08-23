CC := gcc
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O2

all: can_transmitter can_dashboard

can_transmitter: can_transmitter.c
	$(CC) $(CFLAGS) can_transmitter.c -lm -o can_transmitter

can_dashboard: can_dashboard.c
	$(CC) $(CFLAGS) can_dashboard.c -o can_dashboard

clean:
	rm -f can_transmitter can_dashboard
