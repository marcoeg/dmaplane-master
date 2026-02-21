CC := gcc
CFLAGS := -Wall -Wextra -O2 -I../include

TESTS := test_phase1_driver test_phase2_dma

all: $(TESTS)

test_phase1_driver: test_phase1_driver.c
	$(CC) $(CFLAGS) -o $@ $< -lpthread

test_phase2_dma: test_phase2_dma.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TESTS)
