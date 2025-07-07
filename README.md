# ltc_timecode_pi

**ALSA-paced, real-time LTC Timecode Generator for Raspberry Pi (and Linux) using [libltc](https://github.com/x42/libltc)**

## Features

- Real-time, sample-accurate Linear Timecode (LTC) audio generation
- Accurate system time sync with ALSA buffer latency compensation
- Supports all standard SMPTE framerates (24, 25, 29.97, 30, drop-frame and non-drop-frame)
- Pinned to a CPU core for deterministic scheduling
- Graceful exit on Ctrl+C or SIGTERM
- Real-time priority for glitch-free audio
- Console display of running timecode, live-updated (suppressed when running as a service or with `--quiet`)
- Selectable ALSA audio device via command-line option
- Can be installed as a systemd service for automatic startup
- Tested on Raspberry Pi 2 with onboard audio

## Disclaimer

**This software has not been tested for accuracy or reliability in critical or professional environments. It is provided "as is", without warranty of any kind, express or implied, including but not limited to the warranties of merchantability, fitness for a particular purpose, or noninfringement.  
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
./ltc_timecode_pi [-q] [-d device] [frame_rate] [--config <file>]
```

- `-q`, `--quiet` : Suppress console timecode output (recommended for service/systemd use)
- `-d`, `--device` : ALSA PCM device string (default: `default`)
- `frame_rate` : One of `24`, `25`, `29.97`, `30`, `29.97df`, `30df` (default: `25`)
- `--config <file>` : Path to config file (default: `/etc/ltc_timecode_pi.conf`)

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

## Notes

- For improved real-time performance, you can isolate a CPU core by adding `isolcpus=3` to `/boot/firmware/cmdline.txt` on your Raspberry Pi. This reserves core 3 for real-time tasks and can help reduce audio glitches.
- A USB audio interface is recommended over the built-in Pi audio for best audio quality and output reliability.
- For real-time scheduling, you may need to run as root or give the binary extra capabilities:
  ```sh
  sudo setcap 'cap_sys_nice=eip' ./ltc_timecode_pi
  ```
- If the program cannot set real-time priority, it will print a warning and continue.
- You can set the ALSA device and framerate in a config file (default: `/etc/ltc_timecode_pi.conf`) using key=value format:
  ```
  device=hw:CARD=Device,DEV=0
  framerate=29.97df
  ```
- Use the `--config <file>` argument to specify a different config file.
- Command-line arguments override config file values.

## Installing as a systemd Service

You can install and enable `ltc_timecode_pi` as a systemd service using the provided Makefile:

1. **Install the binary and service:**
   ```sh
   sudo make install
   ```
   This will:
   - Build and copy the `ltc_timecode_pi` binary to `/usr/local/bin/`
   - Install the systemd service file to `/etc/systemd/system/ltc_timecode_pi.service`
   - Create a system user `ltc` (if it does not already exist) for the service to run as
   - Reload systemd units

2. **Enable and start the service:**
   ```sh
   sudo systemctl enable ltc_timecode_pi
   sudo systemctl start ltc_timecode_pi
   ```

3. **Check service status:**
   ```sh
   sudo systemctl status ltc_timecode_pi
   ```

4. **View logs:**
   ```sh
   journalctl -u ltc_timecode_pi
   ```

You can adjust the service file at `/etc/systemd/system/ltc_timecode_pi.service` as needed for your setup (e.g., to specify a config file or device).

## Clean Up

```sh
make clean
```

## License

See `LICENSE` (if provided). This project uses [libltc](https://github.com/x42/libltc).
