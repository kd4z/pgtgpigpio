# PgTgPiGpio Daemon

TCP/JSON GPIO relay control service for Raspberry Pi 4 (Debian Trixie, libgpiod ≥ 2.0).

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

---

## Configuration File

Location: `/etc/PgTgPiGpio/PgTgPiGpio.conf` (production) or `./PgTgPiGpio.conf` (development)

```ini
# PgTgPiGpio configuration file
port = 5555
gpio_chip = /dev/gpiochip0

# output<N> = GPIO BCM offset
output1 = 6
output2 = 13
output3 = 19
output4 = 26
```

The live config is created from the package template on first install and is preserved on upgrade and removal. Edit it directly on the Pi; it is never overwritten by `apt`.

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

### 1. Cross-compile (in WSL2)

```bash
./build
# Produces: build-aarch64/PgTgPiGpio (ELF aarch64)
```

### 2. Package

```bash
./package
# Produces: pgtgpigpio_1.0.0_arm64.deb
```

### 3. Deploy to Pi

```bash
./deploy
# SCPs pgtgpigpio_1.0.0_arm64.deb to pi@192.168.111.241:/home/pi/
```

### 4. Install on Pi

```bash
sudo apt install /home/pi/pgtgpigpio_1.0.0_arm64.deb
```

The package installer:
- Creates the `pgtgpigpio` system user and adds it to the `gpio` group
- Copies the factory-default config to `/etc/PgTgPiGpio/PgTgPiGpio.conf` (first install only)
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
```
