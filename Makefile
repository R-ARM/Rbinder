DESTDIR ?= /
PREFIX ?= /usr/

default:
	$(CROSS_COMPILE)$(CC) main.c -o rbinder $(CFLAGS) -lpthread -Wall -Wextra

install:
	install -m 0644 Rbinder.service $(DESTDIR)/etc/systemd/system/
	install -m 0755 rbinder $(DESTDIR)$(PREFIX)/bin/rbinder
