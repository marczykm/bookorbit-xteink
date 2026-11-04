# BookOrbit simulator

The firmware built for macOS or Linux. The e-ink panel is shown in an SDL2 window, so screens and the reader can be tried
without flashing a device. It works like the [CrossPoint simulator](https://github.com/uxjulia/crosspoint-simulator): the
real `src/` and the FreeInk book, font and UI libraries are compiled natively, and only the hardware layer is replaced by
`sim/include` and `sim/src`.

## Setup

```sh
brew install sdl2            # macOS (libcurl comes with the system)
sudo apt install libsdl2-dev libcurl4-openssl-dev   # Debian/Ubuntu
```

## Run

```sh
pio run -e simulator
.pio/build/simulator/program            # Xteink X4 (480x800)
.pio/build/simulator/program --x3       # Xteink X3 (528x792)
```

Options:

| Option | Meaning |
| --- | --- |
| `--x3` | Xteink X3 panel (default: X4) |
| `--sd DIR` | folder used as the SD card (default: `sim/sdcard`) |
| `--slow` | wait as long as the real panel takes to refresh |
| `--scale N` | enlarge the window N times |

## Keys

| Key | Device button |
| --- | --- |
| Esc or Backspace | Back |
| Enter or Space | OK |
| arrows | Left, Right, Up, Down |
| P | Power (hold for a long press; wakes from sleep) |

Simulator controls: **R** resets the device, **S** saves a screenshot to `sim/screenshots`, **W** takes Wi-Fi away (to test
the offline paths), **F** turns the panel refresh times on and off, **+** and **-** change the battery level. The window
title shows the device, battery, Wi-Fi and sleep state.

## How it works

- **SD card:** a folder. Put `bookorbit.json` there (or go through the setup wizard), and EPUBs in `BookOrbit/`.
- **Network:** Wi-Fi always "connects". A scan lists the configured network and a few made-up ones. HTTP requests go
  through libcurl, so the simulator talks to your real BookOrbit server.
- **Restart and sleep:** the firmware runs in a child process. `ESP.restart()` (opening and closing a book) and waking
  from deep sleep start it again, as on the device. `RTC_NOINIT_ATTR` variables are carried over.
- **Serial:** the firmware's log goes to the terminal. The simulator also opens a pseudo-terminal, printed at start
  (`[sim] serial port: /dev/ttys…`). The serial test hooks and `tools/drive.py` work with it as with the device, for
  example:

  ```sh
  python tools/drive.py /dev/ttys004 /tmp/shots wait:1 shot o wait:2 shot
  ```

- **Not simulated:** installing updates (Settings > Update ends with an error), heap limits (allocations are the host's),
  panel ghosting and grey levels.

On macOS, if the build fails with "You have not agreed to the Xcode license agreements", either run
`sudo xcodebuild -license` once or build with the Command Line Tools:
`DEVELOPER_DIR=/Library/Developer/CommandLineTools pio run -e simulator`.
