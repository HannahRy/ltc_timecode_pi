# ltc_timecode_pi

**ALSA-paced, real-time LTC Timecode Generator for Raspberry Pi (and Linux) using [libltc](https://github.com/x42/libltc)**

## Features

- Real-time, sample-accurate Linear Timecode (LTC) audio generation
- Accurate system time sync with adaptive ALSA buffer latency compensation
- Advanced non-linear timing correction for improved frame accuracy
- Memory locking to prevent paging-related timing issues
- Supports all standard SMPTE framerates (24, 25, 29.97, 30, drop-frame and non-drop-frame)
- CPU core pinning for deterministic scheduling
- Real-time priority for glitch-free audio
- Console display of running timecode, live-updated (suppressed when running as a service or with `--quiet`)
- Selectable ALSA audio device
- Can be installed as a systemd service for automatic startup
- Currently tested on Raspberry Pi 2 with onboard audio

## Disclaimer

**This software has not been validated for accuracy or reliability in critical or professional settings.** Based on side-by-side testing with a Blackmagic Studio 4K G2 camera using the camera’s internal NTP time-of-day timecode, ltc_timecode_pi output (on Raspberry Pi 2 with onboard audio) appears to achieve approximately 1–2 frame accuracy; however, it has not been tested beyond this. **The software is provided “as is”, without any warranty—express or implied—including, but not limited to, warranties of merchantability, fitness for a particular purpose, or noninfringement.
Use at your own risk.**

## Build Instructions

First, clone the repository:
```sh
git clone https://github.com/HannahRy/ltc_timecode_pi.git
cd ltc_timecode_pi
```

### Dependencies

- [libltc](https://github.com/x42/libltc) (development headers and library)
- ALSA development headers (`libasound2-dev` on Debian/Raspberry Pi OS)
- Standard Linux build tools (`gcc`, `make`, etc.)

On Raspberry Pi OS/Debian:
```sh
sudo apt-get install libasound2-dev
# For libltc, you may need to build and install from source (https://github.com/x42/libltc)
```

### Build

```sh
make
```

This will produce the `ltc_timecode_pi` executable.

## Usage

```sh
./ltc_timecode_pi [options]
```

- `-q`, `--quiet` : Suppress console timecode output (recommended for service/systemd use)
- `-d`, `--device` : ALSA PCM device string (default: `default`)
- `frame_rate` : One of `24`, `25`, `29.97`, `30`, `29.97df`, `30df` (default: `25`)
- `--config <file>` : Path to config file (default: `/etc/ltc_timecode_pi.conf`)
- `--ntp-server <host>` : Use specified NTP server for time synchronization
- `--ntp-sync-interval <seconds>` : NTP sync interval in seconds (default: 60)
- `--ntp-slew-period <seconds>` : Period over which to gradually adjust time (default: 30)

### List Available ALSA Devices

To see what PCM devices you can use:
```sh
aplay -L
```
Use any of the listed device names with `-d`.

### Examples

Use default audio device at 25 fps:
```sh
./ltc_timecode_pi
```

Use a specific ALSA device (e.g., a USB audio interface) at 29.97 drop-frame:
```sh
./ltc_timecode_pi -d hw:CARD=Device,DEV=0 29.97df
```

Suppress console output (useful for daemon/service/systemd):
```sh
./ltc_timecode_pi --quiet
```

## Configuration File

The configuration file (default: `/etc/ltc_timecode_pi.conf`) allows you to set persistent options. Command-line arguments override config file values.

Example config file:
```
device=hw:CARD=Device,DEV=0         # ALSA device name
framerate=30                        # Frame rate (24, 25, 29.97, 30, 29.97df, 30df)
ntp-server=pool.ntp.org             # NTP server for time synchronization
ntp-sync-interval=60                # NTP sync interval in seconds
ntp-slew-period=30                  # Time adjustment period in seconds
```

- Use `aplay -L` to list available ALSA devices.
- You can specify a different config file with `--config <file>`.

## NTP Time Synchronization

By default, LTC timecode is generated based on the system clock. For more precise and accurate time synchronization, you can specify an NTP server. This is designed to connect to a GPS/PPS NTP server on the local network for low latency.

Enable NTP sync:
```sh
./ltc_timecode_pi --ntp-server pool.ntp.org
```

Configure NTP sync interval and slew period:
```sh
./ltc_timecode_pi --ntp-server pool.ntp.org --ntp-sync-interval 300 --ntp-slew-period 60
```

Or set these in the config file:
```
ntp-server=time.google.com
ntp-sync-interval=300  # sync every 5 minutes
ntp-slew-period=60     # gradually adjust time over 60 seconds
```

- The program will sync with the NTP server at startup and periodically based on the configured interval.
- For best results, use a stratum 1 timeserver or a local NTP server with good accuracy.

## Notes

- For improved real-time performance, you can isolate a CPU core by adding `isolcpus=3` to `/boot/firmware/cmdline.txt` on your Raspberry Pi. This reserves core 3 for real-time tasks and can help reduce audio glitches.
- A USB audio interface is recommended over the built-in Pi audio for best audio quality and output reliability.
- For real-time scheduling, you may need to run as root or give the binary extra capabilities:
  ```sh
  sudo setcap 'cap_sys_nice=eip' ./ltc_timecode_pi
  ```
- If the program cannot set real-time priority, it will print a warning and continue.
- Command-line arguments always override config file values.

## Installing as a systemd Service

You can install and enable `ltc_timecode_pi` as a systemd service using the provided Makefile:

0. **Compile the binary if not already done:**
   ```sh
   make
   ```

1. **Install the binary and service:**
   ```sh
   sudo make install
   ```
   This will:
   - Copy the `ltc_timecode_pi` binary to `/usr/local/bin/`
   - Install the systemd service file to `/etc/systemd/system/ltc_timecode_pi.service`
   - Install a systemd timer to `/etc/systemd/system/ltc_timecode_pi.timer` (for delayed startup, this gives the system time to get the sound card ready)
   - Install the example config file to `/etc/ltc_timecode_pi.conf` (if it doesn't exist)
   - Create a system user `ltc` (if it doesn't exist) and add it to the audio group
   - Reload systemd units

2. **Enable and start the timer for boot autostart:**
   ```sh
   sudo systemctl enable ltc_timecode_pi.timer
   sudo systemctl start ltc_timecode_pi.timer
   ```
   This will start the service with a delay after boot to ensure the sound devices are ready.

3. **Or start the service immediately without the timer:**
   ```sh
   sudo systemctl start ltc_timecode_pi.service
   ```

4. **Check service/timer status:**
   ```sh
   sudo systemctl status ltc_timecode_pi.service
   sudo systemctl status ltc_timecode_pi.timer
   ```

5. **View logs:**
   ```sh
   journalctl -u ltc_timecode_pi
   ```

6. **Uninstall if needed:**
   ```sh
   sudo make uninstall
   ```
   This will stop and disable the service and timer, and remove all installed files (the `ltc` user and config file will not be removed).

You can adjust the configuration at `/etc/ltc_timecode_pi.conf` to set your device, framerate, and NTP server.

## Technical Details: Timing Correction

For an in-depth explanation of the advanced timing correction techniques used to achieve precise LTC output - including hardware optimizations, ALSA buffer compensation, and adaptive mathematical correction; see [docs/TIMING.md](docs/TIMING.md).

## License

See `LICENSE` (if provided). This project uses [libltc](https://github.com/x42/libltc).
