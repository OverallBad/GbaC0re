Place your .gba ROM files in this folder.

Then upload them to the PS5 with:
  python3 gba_launcher.py <PS5_IP> --roms . --log

(or use the full path: --roms /path/to/this/folder)

Rules:
- Filenames: A-Z a-z 0-9 . _ - only, max 64 chars, must end in .gba
- Max 32 MB per ROM
- ROMs are uploaded to /temp0/roms/ on the PS5 (wiped on reboot)
