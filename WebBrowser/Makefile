CC = gcc
TARGET = simplebrowser
SRC = main.c

PKGS = gtk+-3.0 webkit2gtk-4.1
CFLAGS = `pkg-config --cflags $(PKGS)` -Wall -Wextra -g
LDFLAGS = `pkg-config --libs $(PKGS)`

$(TARGET): $(SRC)
	$(CC) $(SRC) -o $(TARGET) $(CFLAGS) $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: run clean
