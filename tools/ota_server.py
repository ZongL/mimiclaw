#!/usr/bin/env python3
"""OTA firmware download and local HTTP server setup"""

import os
import sys
import socket
import subprocess
from pathlib import Path

def get_local_ip():
    """Get local IP address"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
    except:
        ip = "127.0.0.1"
    finally:
        s.close()
    return ip

def download_firmware(url, output_path):
    """Download firmware from URL"""
    print(f"Downloading firmware from: {url}")
    try:
        import urllib.request
        import shutil

        with urllib.request.urlopen(url, timeout=30) as response:
            total_size = int(response.headers.get('Content-Length', 0))
            downloaded = 0
            chunk_size = 8192

            with open(output_path, 'wb') as f:
                while True:
                    chunk = response.read(chunk_size)
                    if not chunk:
                        break
                    f.write(chunk)
                    downloaded += len(chunk)
                    if total_size:
                        percent = (downloaded / total_size) * 100
                        bar_len = 40
                        filled = int(bar_len * downloaded / total_size)
                        bar = '█' * filled + '░' * (bar_len - filled)
                        print(f"\r[{bar}] {percent:.1f}% ({downloaded}/{total_size})", end='')

        print(f"\n✓ Firmware downloaded successfully!")
        return True
    except Exception as e:
        print(f"\n✗ Download failed: {e}")
        return False

def main():
    # Configuration
    RELEASETAG = "v0.2.19-test"
    GITHUB_URL = f"https://github.com/ZongL/mimiclaw/releases/download/{RELEASETAG}/mimiclaw-esp32cam-{RELEASETAG}.bin"
    FILENAME = f"mimiclaw-esp32cam-{RELEASETAG}.bin"

    # Get paths
    script_dir = Path(__file__).parent.absolute()
    project_root = script_dir.parent
    build_dir = project_root / "build"

    # Ensure build directory exists
    build_dir.mkdir(exist_ok=True)
    firmware_path = build_dir / FILENAME

    # Download firmware if not exists
    if not firmware_path.exists():
        if not download_firmware(GITHUB_URL, firmware_path):
            sys.exit(1)
    else:
        print(f"✓ Firmware already exists: {firmware_path}")

    # Get local IP
    local_ip = get_local_ip()

    # Print instructions
    print(f"\n{'='*50}")
    print(f"OTA Server Setup")
    print(f"{'='*50}")
    print(f"\nFirmware location: {firmware_path}")
    print(f"Serving from: {build_dir}")
    print(f"Local IP: {local_ip}")
    print(f"Port: 8000")
    print(f"\n{'='*50}")
    print(f"On ESP32, run this command:")
    print(f"  ota_update http://{local_ip}:8000/{FILENAME}")
    print(f"{'='*50}")
    print(f"\nStarting HTTP server...")
    print(f"Press Ctrl+C to stop\n")

    # Change to build directory and start server
    os.chdir(build_dir)
    try:
        subprocess.run([sys.executable, "-m", "http.server", "8000"])
    except KeyboardInterrupt:
        print("\n\nServer stopped.")

if __name__ == "__main__":
    main()
