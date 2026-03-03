# PgTgPiGpio Daemon for PgTgBridge

TCP/JSON GPIO relay control service targeting Raspberry Pi 4 (Debian Trixie, libgpiod ≥ 2.0).
Specifically for use with PgTgBridge built-in plugin PgTg PiGpio.  Can be used as a smart 
wrapper to GPIOD that is accessible using JSON strings such as from Node-Red.

---

## JSON Protocol

Commands are newline-delimited JSON sent over TCP. The connection remains open between commands.

| Direction      | Example |
|----------------|---------|
| Set ON         | `{"cmd":"set","output":"output1","value":1}` |
| Set OFF        | `{"cmd":"set","output":"output2","value":0}` |
| Timed ON (30s) | `{"cmd":"set","output":"output1","value":30}` |
| Read all       | `{"cmd":"read"}` |
| OK (set)       | `{"status":"ok","output":"output1","value":1}` |
| OK (read)      | `{"status":"ok","outputs":{"output1":1,"output2":0}}` |
| Error          | `{"status":"error","message":"unknown output: output5"}` |

### Timer Behaviour

| Value   | Behaviour |
|---------|-----------|
| `0`     | Static OFF — cancels any running timer |
| `1`     | Static ON — cancels any running timer |
| `2–999` | Drives output ON; auto-forces OFF after that many seconds. A new value ≥ 2 before expiry resets the timer. |

On expiry the server sends `{"status":"ok","output":"<name>","value":0}` on the open connection if a client is connected.

### PgTg Bridge Watchdog Integration

When the PgTgBridge GPIO plugin is configured with an **Amp PTT** output, it sends `value: 15`
(not `1`) whenever the amplifier PTT is keyed. This starts a 15-second channel watchdog timer on
the service side. PgTgBridge then re-sends `value: 15` every 10 seconds to keep the watchdog
alive for the duration of the transmission. When PTT is released the PgTgBridge sends `value: 0`.

If the PgTgBridge loses connectivity, the watchdog expires after 15 seconds and the PTT output is
automatically deactivated — preventing a stuck transmitter if the connectivity to the host machine is lost.

If the PgTgBridge service is stopped, PgTg sends `value: 0` to all configured GPIO channels before halting.

---

## Configuration File

Location: `/etc/PgTgPiGpio/PgTgPiGpio.conf` 

```ini
# PgTgPiGpio configuration file
# You may need to change the gpio_chip assignment to suit your particular hardware.
port = 5555
gpio_chip = /dev/gpiochip0

# output<N> = GPIO BCM offset
# These numbers are the GPIO number, not pin numbers on the Raspberry Pi!

output1 = 6
output2 = 13
output3 = 19
output4 = 26
```

The live conf file is created from the package template on first install and is preserved on upgrade and removal. Edit it directly on the Pi; it is never overwritten by `apt`.

---

## Systemd Service

The service runs as the dedicated system user `pgtgpigpio` (member of the `gpio` group) — no root required.

```ini
[Unit]
Description=PgTgPiGpio TCP/JSON GPIO relay control service
After=network.target
Wants=network.target

[Service]
Type=simple
ExecStart=/usr/bin/PgTgPiGpio /etc/PgTgPiGpio/PgTgPiGpio.conf
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal
SyslogIdentifier=PgTgPiGpio
User=pgtgpigpio
Group=gpio

[Install]
WantedBy=multi-user.target
```

The service file is installed and enabled automatically by the Debian package.

---

## Build & Deploy

Two build workflows are available — cross-compile from a WSL2 host, or build natively on the Pi.
If building under WSL2, a deb package is created.

---

### Option A — Native build on the Pi

Prerequisites (install once on the Pi):

```bash
sudo apt install cmake ninja-build pkg-config libgpiod-dev
```

```bash
./build
# Produces: build-native/PgTgPiGpio (you need to setup the cross-compiler yourself)
```

The script verifies all prerequisites and the libgpiod version before building.

You will be responsible for copying the PgTgPiGpio binary to:
`/usr/bin/PgTgPiGpio`

make sure it is marked as executable:
`sudo chmod +x /usr/bin/PgTgPiGpio`

and creating the configuration file here:
 `/etc/PgTgPiGpio/PgTgPiGpio.conf`
 
 
---

### Option B — Cross-compile (in WSL2)

```bash
./build-with-cross-compiler
# Produces: build-aarch64/PgTgPiGpio (ELF aarch64)
```

### 1. Package (after cross-compile in WSL2)

```bash
./package
# Produces: pgtgpigpio_1.0.0_arm64.deb
# Sources binary from build-aarch64/PgTgPiGpio
```

### 2. Deploy to Pi (use after cross-compile and deb package creation)

```bash
./deploy
# SCPs pgtgpigpio_1.0.0_arm64.deb to pi@<yourPiIpaddress>:/home/pi/
```

### 3. Install on Pi

SCP the deb package to `/tmp`

```bash
sudo apt install /tmp/pgtgpigpio_1.0.0_arm64.deb
```

The package installer:
- Creates the `pgtgpigpio` system user and adds it to the `gpio` group
- Copies the factory-default config to `/etc/PgTgPiGpio/PgTgPiGpio.conf` (first install only)
- Copies the daemon binary to `/usr/bin` 
- Installs and starts the systemd service

---

## Upgrade & Removal

```bash
# Upgrade (live config is preserved)
sudo apt install /home/pi/pgtgpigpio_1.0.0_arm64.deb

# Remove (binary and service removed; config kept)
sudo apt remove pgtgpigpio

# Full purge (config and system user also removed)
sudo apt purge pgtgpigpio
```

---

## Testing on the Pi

```bash
# Set output1 ON
echo '{"cmd":"set","output":"output1","value":1}' | nc localhost 5555

# Read all output states
echo '{"cmd":"read"}' | nc localhost 5555

# Set output1 OFF
echo '{"cmd":"set","output":"output1","value":0}' | nc localhost 5555

# Timed ON — output1 goes ON, forces OFF after 30 seconds
echo '{"cmd":"set","output":"output1","value":30}' | nc localhost 5555

# Monitor in journald
journalctl -f -u PgTgPiGpio
```

---

## Service Management

```bash
# Start and follow logs
sudo systemctl start PgTgPiGpio && journalctl -u PgTgPiGpio -f

# Stop (triggers clean GPIO release — verify in journal)
sudo systemctl stop PgTgPiGpio

# Confirm service is running as pgtgpigpio (not root)
ps aux | grep PgTgPiGpio

or to see user and group in list

ps -eo user,group,cmd | grep PgTgPiGpio
```
