CC := gcc
SENDER_CFLAGS := -Wall -Wextra -O2 -I../../include
SENDER_LDFLAGS := -lcuda -lcudart -lm

RECEIVER_CFLAGS := -Wall -Wextra -O2 -I../../include
RECEIVER_LDFLAGS := -lm

all: gpu_sender gpu_receiver

gpu_sender: gpu_sender.c
	$(CC) $(SENDER_CFLAGS) -o $@ $< $(SENDER_LDFLAGS)

gpu_receiver: gpu_receiver.c
	$(CC) $(RECEIVER_CFLAGS) -o $@ $< $(RECEIVER_LDFLAGS)

clean:
	rm -f gpu_sender gpu_receiver

.PHONY: all clean
