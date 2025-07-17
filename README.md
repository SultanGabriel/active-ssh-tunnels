# Chief Tunnel Officer - SSH Tunnel Manager

Ein state-of-the-art C-Programm für persistentes SSH-Tunnel-Management.

## Features

🚀 **Robust & Persistent**
- Automatische Wiederverbindung nach Netzfehlern
- Multi-threaded: Jeder Tunnel läuft in eigenem Thread
- Konfigurierbare Retry-Delays
- Vollständige Fehlerbehandlung

📊 **Monitoring & Logging**
- Live-Status aller Tunnels
- Per-Tunnel-Logging mit Timestamps
- Restart-Counter und Statistiken
- Interaktive CLI mit Echzeit-Updates

⚙️ **Flexibel & Erweiterbar**
- JSON-basierte Konfiguration
- **Dynamisches Hinzufügen/Entfernen von Tunnels zur Laufzeit**
- **Individuelle Tunnel-Steuerung (start/stop/reset by name)**
- **Automatische Konfigurationsspeicherung**
- Systemd-Integration
- Plattformübergreifend (Linux/macOS/Windows)

🛡️ **Production-Ready**
- Saubere Signal-Behandlung
- Memory-Management
- Keine Fork-Bombs oder Zombie-Prozesse
- KISS-Prinzip mit robustem Design

## Quick Start

```bash
# Setup (lädt cJSON und erstellt Beispiel-Config)
make setup

# Build
make

# Run
make run
```

## Installation

### Abhängigkeiten

**Linux (Ubuntu/Debian):**
```bash
sudo apt-get install build-essential libncurses5-dev
```

**Linux (CentOS/RHEL):**
```bash
sudo yum install gcc ncurses-devel
```

**macOS:**
```bash
brew install ncurses
```

**Windows (MinGW/MSYS2):**
```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-ncurses
```

### Bauen

```bash
# Komplettes Setup
make setup

# Nur kompilieren
make

# Debug-Version
make debug

# Release-Version
make release
```

## Konfiguration

Bearbeite `config.json`:

```json
{
  "tunnels": [
    {
      "name": "db-prod",
      "host": "db.example.com",
      "port": 22,
      "user": "admin",
      "local_port": 3307,
      "remote_host": "127.0.0.1",
      "remote_port": 3306,
      "reconnect_delay": 5
    }
  ]
}
```

### Parameter

- `name`: Eindeutiger Tunnel-Name
- `host`: SSH-Server-Hostname
- `port`: SSH-Server-Port (meist 22)
- `user`: SSH-Username
- `local_port`: Lokaler Port für Tunnel
- `remote_host`: Ziel-Host (meist 127.0.0.1)
- `remote_port`: Ziel-Port
- `reconnect_delay`: Wartezeit zwischen Reconnects (Sekunden)

## Verwendung

### Interaktive CLI

```bash
./tunnel_manager

# Verfügbare Befehle:
tunnel> status         # Zeige Tunnel-Status
tunnel> start          # Starte alle Tunnels
tunnel> start db-prod  # Starte spezifischen Tunnel
tunnel> stop           # Stoppe alle Tunnels  
tunnel> stop web-dev   # Stoppe spezifischen Tunnel
tunnel> reset api-test # Restarte Tunnel (Reset Counter)
tunnel> add            # Neuen Tunnel interaktiv hinzufügen
tunnel> watch          # Live-Updates alle 2 Sekunden
tunnel> quit           # Programm beenden
tunnel> help           # Hilfe anzeigen
```

### Dynamische Tunnel-Verwaltung

**Neuen Tunnel hinzufügen:**
```bash
tunnel> add
Tunnel name: api-staging
SSH user: deploy
SSH host: staging.example.com
SSH port: 22
Local port: 9090
Remote host: localhost
Remote port: 3000
Reconnect delay (s) [5]: 3
Start tunnel now? [y/N]: y
```

**Individuelle Tunnel-Steuerung:**
```bash
tunnel> start db-prod     # Nur den DB-Tunnel starten
tunnel> stop web-staging  # Nur den Web-Tunnel stoppen
tunnel> reset cache-redis # Tunnel neustarten (Counter wird zurückgesetzt)
```

**Live-Monitoring:**
```bash
tunnel> watch             # Bildschirm wird alle 2s aktualisiert
# Ctrl+C zum Beenden des Watch-Modus
```

### Systemd Service (Linux)

```bash
# Service-Datei erstellen
make install-service

# Service installieren
sudo cp tunnel-manager.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable tunnel-manager
sudo systemctl start tunnel-manager

# Status prüfen
sudo systemctl status tunnel-manager
```

## Logs

Logs werden automatisch in `logs/` erstellt:
- `logs/db-prod.log`
- `logs/web-staging.log`
- etc.

Jeder Log enthält:
- Timestamps
- Restart-Counter
- SSH-Kommandos
- Fehler und Status-Updates

## Make-Targets

```bash
make setup          # Erstes Setup (cJSON + Config)
make                # Standard-Build
make test           # Unit Tests kompilieren
make test-run       # Unit Tests ausführen
make clean          # Build-Dateien löschen
make distclean      # Alles löschen (inkl. cJSON, Config)
make debug          # Debug-Build
make release        # Optimierter Release-Build
make run            # Build + Run
make config         # Beispiel-Config erstellen
make install-deps   # Dependency-Hilfe
make install-service # Systemd-Service erstellen
make help           # Hilfe anzeigen
```

## Architektur

```
main.c
├── tunnel_manager_t     # Haupt-Manager-Struktur
├── tunnel_t            # Pro-Tunnel-Datenstruktur
├── tunnel_worker()     # Worker-Thread pro Tunnel
├── load_config()       # JSON-Config-Parser
├── interactive_mode()  # CLI-Interface
└── signal_handler()    # Clean Shutdown
```

### Thread-Modell

- **Main Thread**: CLI und Koordination
- **Worker Threads**: Ein Thread pro Tunnel
- **Mutex-Protection**: Thread-sichere Status-Updates
- **Clean Shutdown**: Signalbasiertes Beenden

### Status-Management

```c
typedef enum {
    TUNNEL_STOPPED = 0,
    TUNNEL_STARTING,
    TUNNEL_RUNNING,
    TUNNEL_ERROR,
    TUNNEL_RECONNECTING
} tunnel_status_t;
```

## Beispiel-Session

```bash
$ ./tunnel_manager
Chief Tunnel Officer - SSH Tunnel Manager v1.0
================================================

✅ Loaded 3 tunnels successfully
🚀 Auto-starting all tunnels...

=== Interactive Command Mode ===
Commands: status, start [name], stop [name], reset <name>, add, watch, quit, help

tunnel> add
📝 Adding new tunnel - Interactive Setup
─────────────────────────────────────────

Tunnel name: api-prod
SSH user: admin
SSH host: api.example.com
SSH port: 22
Local port: 4000
Remote host: localhost
Remote port: 8080
Reconnect delay (s) [5]: 
Start tunnel now? [y/N]: y
🚀 Started tunnel 'api-prod'
💾 Configuration saved to config.json

tunnel> status
🔗 db-prod admin@db.example.com:22 ➔ localhost:3307 ➔ 127.0.0.1:3306
   Status: RUNNING | Restarts: 1 | Delay: 5s | Last: 15s ago

🔗 api-prod admin@api.example.com:22 ➔ localhost:4000 ➔ localhost:8080
   Status: RUNNING | Restarts: 1 | Delay: 5s | Last: 2s ago

tunnel> reset db-prod
🔄 Reset tunnel 'db-prod'

tunnel> quit
👋 Chief Tunnel Officer signing off. All tunnels terminated.
```

## SSH-Konfiguration

Für optimale Performance empfohlene SSH-Config (`~/.ssh/config`):

```
Host *
    ServerAliveInterval 30
    ServerAliveCountMax 3
    ConnectTimeout 10
    TCPKeepAlive yes
    Compression yes
```

## Troubleshooting

### Häufige Probleme

**SSH-Authentifizierung fehlgeschlagen:**
- SSH-Keys korrekt eingerichtet?
- `ssh-agent` läuft?
- Test mit `ssh user@host`

**Port bereits belegt:**
- `netstat -tlnp | grep :PORT` prüfen
- Anderen lokalen Port wählen

**Tunnel bricht ab:**
- SSH-Server-Logs prüfen
- Firewall-Regeln überprüfen
- `reconnect_delay` erhöhen

### Debug-Modus

```bash
make debug
./tunnel_manager

# Logs überwachen
tail -f logs/*.log
```

## Performance

**Optimierungen:**
- Minimaler Memory-Footprint
- Efficient Polling mit `select()`
- Keine unnötigen Syscalls
- Clean Thread-Management

**Limits:**
- Max 32 Tunnels (anpassbar via `MAX_TUNNELS`)
- Abhängig von System-Limits (ulimit)

## Lizenz

MIT License - Freie Verwendung für alle Chief Tunnel Officers.

## Beitragen

1. Fork the repo
2. Create feature branch
3. Commit changes
4. Push to branch
5. Create Pull Request

---

**Built by and for Chief Tunnel Officers.**  
*"Das ist die Referenz für SSH-Tunnel-Daemons im C-Universum."*
