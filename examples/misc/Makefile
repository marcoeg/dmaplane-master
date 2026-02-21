CC := gcc
CFLAGS := -Wall -Wextra -O2 -I../../include

DEMOS := dma_explorer dma_sweep

all: $(DEMOS)

%: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(DEMOS)
