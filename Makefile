all: send receive relay

send: send.c secret.c
	gcc -o send \
	    send.c \
	    secret.c \
	    $$(pkg-config --cflags --libs openssl)

receive: receive.c secret.c
	gcc -o receive \
	    receive.c \
	    secret.c \
	    $$(pkg-config --cflags --libs openssl)

relay: relay.c
	gcc -o relay -g \
	    -D_GNU_SOURCE \
	    -DNO_USE_SPLICE \
	    -DMAX_CONNECTIONS=10000 \
	    relay.c \
	    -lpthread \
	    $$(pkg-config --cflags --libs openssl)

clean:
	rm -f send receive relay

test:
	@./tests.sh
