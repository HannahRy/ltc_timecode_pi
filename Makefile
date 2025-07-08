# Makefile for ltc_timecode_pi

CC=gcc
CFLAGS=-Wall -O2 -D_GNU_SOURCE
LDFLAGS=-pthread -lltc -lasound -lm

TARGET=ltc_timecode_pi
SOURCES=ltc_timecode_pi.c ltc_timecode.c ltc_ntp.c ltc_config.c
HEADERS=ltc_common.h ltc_ntp.h ltc_config.h

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(SOURCES) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	# Create ltc user if it doesn't exist
	@echo "Checking for user 'ltc'..."
	@if ! id -u ltc >/dev/null 2>&1; then \
		echo "Creating user 'ltc'..."; \
		useradd -r -s /bin/false ltc; \
		usermod -a -G audio ltc; \
		echo "User 'ltc' created and added to 'audio' group"; \
	else \
		echo "User 'ltc' already exists"; \
		if ! groups ltc | grep -q '\baudio\b'; then \
			usermod -a -G audio ltc; \
			echo "Added user 'ltc' to 'audio' group"; \
		fi; \
	fi

	# Install binary
	install -m 755 $(TARGET) /usr/local/bin
	@echo "Installed binary to /usr/local/bin/$(TARGET)"

	# Install example config file if not exists
	@if [ ! -f /etc/ltc_timecode_pi.conf ] && [ -f ltc_timecode_pi.conf.example ]; then \
		install -m 644 ltc_timecode_pi.conf.example /etc/ltc_timecode_pi.conf; \
		echo "Installed example config file to /etc/ltc_timecode_pi.conf"; \
	fi

	# Install systemd service and timer files
	install -m 644 ltc_timecode_pi.service /etc/systemd/system/
	install -m 644 ltc_timecode_pi.timer /etc/systemd/system/
	@echo "Installed systemd service and timer files"

	# Reload systemd
	systemctl daemon-reload
	@echo
	@echo "Installation complete!"
	@echo "================================"
	@echo "Edit the config file at /etc/ltc_timecode_pi.conf to set your device and NTP server."
	@echo
	@echo "To enable and start the timer (for boot autostart with delay):"
	@echo "  sudo systemctl enable ltc_timecode_pi.timer"
	@echo "  sudo systemctl start ltc_timecode_pi.timer"
	@echo
	@echo "To start the service immediately:"
	@echo "  sudo systemctl start ltc_timecode_pi.service"
	@echo
	@echo "To check status:"
	@echo "  sudo systemctl status ltc_timecode_pi.service"
	@echo "  sudo systemctl status ltc_timecode_pi.timer"
	@echo "================================"

uninstall:
	systemctl stop ltc_timecode_pi.service || true
	systemctl disable ltc_timecode_pi.service || true
	systemctl stop ltc_timecode_pi.timer || true
	systemctl disable ltc_timecode_pi.timer || true
	rm -f /etc/systemd/system/ltc_timecode_pi.service
	rm -f /etc/systemd/system/ltc_timecode_pi.timer
	rm -f /usr/local/bin/$(TARGET)
	systemctl daemon-reload
	@echo "Uninstalled $(TARGET)"
	@echo "Note: User 'ltc' and config file were not removed"

.PHONY: all clean install uninstall