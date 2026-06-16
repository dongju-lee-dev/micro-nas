# Micro NAS (ESP32-C6)

An ESP32-C6 based Network Attached Storage (NAS) solution. This project enables management of multiple SD cards through a web interface, including session management and physical hardware controls for storage mounting.

## Features

### Storage Management
- **Multi-SD Card Support**: Connect and manage multiple SD cards via SPI.
- **Dynamic Mounting**: Automatic mounting of cards to independent paths (e.g., `/storage_0`, `/storage_1`).
- **Hardware Control**: Physical button and LED indicator for manual unmounting before card removal.

### Web Interface and API
- **Web UI**: Browser-based file explorer served from internal flash (SPIFFS).
- **File Operations**: Support for upload, download, rename, delete, and search operations.
- **Session Management**: Token-based authentication with configurable timeouts and password protection.
- **System Monitoring**: Real-time status for CPU usage, heap memory, Wi-Fi signal strength, and storage capacity.

### Networking
- **Wi-Fi Station Mode**: Connects to an existing wireless network.
- **mDNS Support**: Device discovery via `http://micro-nas.local`.
- **Power Management**: Wi-Fi power-saving adjustments based on request activity.

## Configuration

The system is configured via `/flash/setting.txt`. Parameters can be customized without recompiling the firmware:

```ini
# Hardware Pins
STOP=9
STOP_LED=18
SPI_MOSI=22
SPI_MISO=21
SPI_SCLK=20
SPI_CS_0=19
SPI_CS_1=1

# Network Settings
WIFI_SSID=Your_SSID
WIFI_PASSWORD=Your_Password

# Security
PASSWORD=admin_pass
SESSION=5
SESSION_TIME=3600
```

## Technical Stack

- **Platform**: ESP32-C6 (RISC-V)
- **Framework**: ESP-IDF
- **Filesystems**: FATFS (SD cards), SPIFFS (system and web files)
- **Communication**: SPI, HTTP, mDNS
- **Libraries**: cJSON

## Project Structure

- `src/main.c`: Application entry point and configuration management.
- `src/storage/`: SD card hardware abstraction and mount logic.
- `src/web/`: HTTP server implementation and API handlers.
- `data/web/`: Frontend assets (HTML, CSS, JavaScript).
- `partitions.csv`: Partition table defining the SPIFFS area.

## Getting Started

1. **Hardware Setup**: Connect SD card modules according to the pins defined in `setting.txt`.
2. **Flash Filesystem**: Upload the `data/` folder content to the SPIFFS partition.
3. **Build and Flash**: Build the project and upload to the ESP32-C6 using PlatformIO or ESP-IDF tools.
4. **Access**: Navigate to `http://micro-nas.local` in a web browser.
