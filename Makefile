all: send receive relay

# debian stretch puts sys in /usr/include/x86_64-linux-gnu
CFLAGS = -g -std=c99 -D_GNU_SOURCE -rdynamic \
	-I/usr/include/x86_64-linux-gnu \
	-L/usr/lib/x86_64-linux-gnu

ifeq "$(HAS_LTO)" "1"
	CFLAGS += -flto
endif

send: send.c secret.c
	gcc -o send \
	    $(CFLAGS) \
	    send.c \
	    secret.c \
	    $$(pkg-config --cflags --libs openssl)

receive: receive.c secret.c
	gcc -o receive \
	    $(CFLAGS) \
	    receive.c \
	    secret.c \
	    $$(pkg-config --cflags --libs openssl)

relay: relay.c
	gcc -o relay -g \
	    $(CFLAGS) \
	    -DMAX_CONNECTIONS=10000 \
	    relay.c \
	    -lpthread \
	    $$(pkg-config --cflags --libs openssl)

clean:
	rm -f send receive relay

test:
	@./tests.sh
