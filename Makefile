# Makefile for ltc_timecode_pi

CC=gcc
CFLAGS=-O2 -Wall -pthread
LIBS=-lltc -lasound -lm
TARGET=ltc_timecode_pi
SRC=ltc_timecode_pi.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LIBS)

install: $(TARGET) ltc_timecode_pi.service
	@if ! id -u ltc >/dev/null 2>&1; then \
		echo "Creating system user 'ltc'..."; \
		sudo useradd --system --no-create-home --shell /usr/sbin/nologin ltc; \
	fi
	sudo usermod -aG audio ltc
	install -d /usr/local/bin
	install -m 755 $(TARGET) /usr/local/bin/$(TARGET)
	install -d /etc/systemd/system
	install -m 644 ltc_timecode_pi.service /etc/systemd/system/ltc_timecode_pi.service
	systemctl daemon-reload
	@echo "Installed $(TARGET) to /usr/local/bin and service to /etc/systemd/system/"

clean:
	rm -f $(TARGET) *.o

.PHONY: all clean install