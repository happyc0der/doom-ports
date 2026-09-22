# Host tools.  `make` builds the MTP pusher; needs libmtp (brew install libmtp).
CFLAGS += -O2 $(shell pkg-config --cflags libmtp)
LDLIBS += $(shell pkg-config --libs libmtp)

mtp_push: mtp_push.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f mtp_push

.PHONY: clean
