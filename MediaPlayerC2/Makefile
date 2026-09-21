CC       = gcc
CFLAGS   = -Wall -Wextra -O2 $(shell pkg-config --cflags gtk+-3.0 gstreamer-1.0)
LDLIBS   = $(shell pkg-config --libs gtk+-3.0 gstreamer-1.0)
TARGET   = mediaplayer

PREFIX  ?= /usr/local
DESTDIR ?=

all: $(TARGET)

$(TARGET): mediaplayer.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	install -Dm644 mediaplayer.desktop $(DESTDIR)$(PREFIX)/share/applications/mediaplayer.desktop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	rm -f $(DESTDIR)$(PREFIX)/share/applications/mediaplayer.desktop

clean:
	rm -f $(TARGET)

.PHONY: all install uninstall clean
