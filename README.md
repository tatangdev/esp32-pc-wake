# esp32-pc-wake

wake-on-LAN for my PC, controlled from telegram and running on an ESP32-WROOM-32. it can also check if windows RDP is reachable.

## Setup

1. install [arduino-cli](https://arduino.github.io/arduino-cli/latest/installation/). the core and library versions are pinned in `sketch.yaml`, so the first compile installs them automatically.
2. copy `secrets.example.h` to `secrets.h`, then fill in your wifi SSID/password (2.4 GHz only), BotFather token and your numeric telegram user ID. don't commit `secrets.h`.
3. in `config.h`, the ethernet MAC of the windows PC is already set to `9C:6B:00:FA:74:7C`. change it if you use another machine.
4. (recommended) get the PC's *LAN* IPv4 from `ipconfig` on windows, reserve it in your router, and put it as `PC_LAN_IP` in `config.h`. don't use the tailscale 100.x IP. without this, `/wake` still works but `/status` can't check RDP.
5. compile, upload and open the serial monitor (see below).
6. send `/status` and `/wake` to the bot from your authorized private chat.

## Build and upload

```bash
arduino-cli compile .
arduino-cli board list                      # find the port, usually /dev/ttyUSB0
arduino-cli upload -p /dev/ttyUSB0 .
arduino-cli monitor -p /dev/ttyUSB0 -c baudrate=115200
```

### WSL2

WSL can't see USB devices by default. use [usbipd-win](https://github.com/dorssel/usbipd-win) to attach the ESP32:

```powershell
# in windows powershell (admin)
winget install usbipd
usbipd list                          # find the busid of the ESP32 (CP210x or CH340)
usbipd bind --busid <busid>          # only needed once
usbipd attach --wsl --busid <busid>  # needed again after replugging
```

then in WSL, give your user access to the serial port (log out and in again after this):

```bash
sudo usermod -aG dialout $USER
```

## Behavior

- `/wake`: sends a magic packet. if the RDP IP is set, it checks first if RDP is already reachable, and after sending it watches the boot for up to 90 seconds.
- `/status`: shows ESP32 wifi, uptime, memory, telegram stats, and RDP reachability (if set).
- `/help`: list of commands.
- only one user (`AUTHORIZED_USER_ID`) can use the bot. any other text gets the command list.
- uses telegram long polling (20s), so commands arrive almost instantly without polling all the time.
- only uses outbound HTTPS. no incoming ports, VPN endpoint or the Dokploy VPS needed.

## Limitations

- **not tested on real hardware yet.** it compiles on core 3.3.12 with ArduinoJson 7.4.3, but still need to check first boot, recovery after wifi goes down, and long-run stability on the actual ESP32.
- the telegram reply only means the WoL *packet was sent*, not that the PC really booted. RDP reachable only means the service can be reached, and unreachable doesn't mean the PC is off.
- if home internet, router, wifi or ESP32 power is down, remote commands won't work.
- HTTPS calls are blocking, and a long poll can hold the loop for up to 20s. the watchdog restarts the device if it gets really stuck, and it also restarts if telegram or the clock sync fails for 5 minutes.
- TLS uses `TELEGRAM_CERTIFICATE_ROOT` from the installed UniversalTelegramBot library. update the library if telegram changes its CA chain.
- the getUpdates reply is parsed from the stream with a filter and only 1 update per call, so a big message can't block the bot.
- commands older than 90 seconds are ignored, so old messages from an outage won't run later. the offset is saved before a command runs, so a crash can skip a command but won't run it twice.
- the `.ino` depends on libraries with their own licenses, so check them before sharing a compiled binary.
