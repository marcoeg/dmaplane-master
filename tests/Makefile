CC := gcc
CFLAGS := -Wall -Wextra -O2 -I../include

TESTS := test_phase1_driver

all: $(TESTS)

test_phase1_driver: test_phase1_driver.c
	$(CC) $(CFLAGS) -o $@ $< -lpthread

clean:
	rm -f $(TESTS)
