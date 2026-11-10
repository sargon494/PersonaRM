#!/usr/bin/env python3
"""
PersonaRM - Game Asset Extractor & Validator
Extracts and indexes assets from a user-provided legitimate copy of Persona 2: Eternal Punishment.

Supported disc formats:
  - ISO 9660 (.iso)
  - Raw CD Image (.bin / .img)
"""

import os
import sys
import argparse
import hashlib
import json
from pathlib import Path
from typing import Dict, List, Optional, Tuple

ISO_SECTOR_SIZE = 2048
RAW_SECTOR_SIZE = 2352
PVD_LBA = 16

# Serials from community databases (psxdatacenter / libretro); not verified against media.
# See docs/research/PERSONARM_ERRATA_Y_CORRECCIONES.md (G1/G2).
KNOWN_VERSIONS = {
    "SLUS_011.58": {
        "title": "Persona 2: Eternal Punishment (US) - Disc 1",
        "platform": "PlayStation",
        "serial": "SLUS-01158"
    },
    "SLUS_013.39": {
        "title": "Persona 2: Eternal Punishment (US) - Disc 2",
        "platform": "PlayStation",
        "serial": "SLUS-01339"
    },
    "SLPS_027.85": {
        "title": "Persona 2: Batsu (JP, Deluxe Pack) - Disc 1",
        "platform": "PlayStation",
        "serial": "SLPS-02785"
    },
    "SLPS_027.86": {
        "title": "Persona 2: Batsu (JP, Deluxe Pack) - Disc 2",
        "platform": "PlayStation",
        "serial": "SLPS-02786"
    },
    "SLPS_028.25": {
        "title": "Persona 2: Batsu (JP) - Disc 1",
        "platform": "PlayStation",
        "serial": "SLPS-02825"
    },
    "SLPS_028.26": {
        "title": "Persona 2: Batsu (JP) - Disc 2",
        "platform": "PlayStation",
        "serial": "SLPS-02826"
    },
    "SCPS_454.95": {
        "title": "Persona 2: Batsu (JP, SCPS) - Disc 1",
        "platform": "PlayStation",
        "serial": "SCPS-45495"
    },
    "SCPS_454.96": {
        "title": "Persona 2: Batsu (JP, SCPS) - Disc 2",
        "platform": "PlayStation",
        "serial": "SCPS-45496"
    },
    "SLPM_869.67": {
        "title": "Persona 2: Batsu (JP, Atlus Best Collection)",
        "platform": "PlayStation",
        "serial": "SLPM-86967"
    }
}

class DiscExtractor:
    def __init__(self, disc_path: Path):
        self.disc_path = disc_path
        self.sector_size = ISO_SECTOR_SIZE
        self.files: Dict[str, Tuple[int, int]] = {} # name -> (lba, size)
        self.boot_serial = ""

    def detect_and_open(self) -> bool:
        if not self.disc_path.is_file():
            print(f"[Error] Disc image not found: {self.disc_path}")
            return False

        # Try ISO sector size (2048)
        self.sector_size = ISO_SECTOR_SIZE
        if self._parse_iso():
            return True

        # Try RAW CD sector size (2352)
        self.sector_size = RAW_SECTOR_SIZE
        if self._parse_iso():
            return True

        print("[Error] Failed to recognize standard ISO 9660 filesystem on disc.")
        return False

    def _parse_iso(self) -> bool:
        self.files.clear()
        with open(self.disc_path, "rb") as f:
            f.seek(PVD_LBA * self.sector_size)
            pvd = f.read(self.sector_size)
            if len(pvd) < 6 or pvd[1:6] != b"CD001":
                return False

            root_record = pvd[156:156 + 34]
            root_lba = int.from_bytes(root_record[2:6], "little")
            root_len = int.from_bytes(root_record[10:14], "little")

            self._read_directory(f, root_lba, root_len, "")
            return len(self.files) > 0

    def _read_directory(self, f, lba: int, length: int, prefix: str):
        sectors = (length + self.sector_size - 1) // self.sector_size
        for s in range(sectors):
            f.seek((lba + s) * self.sector_size)
            buf = f.read(self.sector_size)
            pos = 0
            while pos < self.sector_size:
                rec_len = buf[pos]
                if rec_len == 0 or pos + rec_len > self.sector_size:
                    break

                file_lba = int.from_bytes(buf[pos + 2:pos + 6], "little")
                file_size = int.from_bytes(buf[pos + 10:pos + 14], "little")
                flags = buf[pos + 25]
                name_len = buf[pos + 32]
                raw_name = buf[pos + 33:pos + 33 + name_len].decode("ascii", errors="ignore")

                # Remove ISO version tag (;1)
                clean_name = raw_name.split(";")[0]
                is_dir = bool(flags & 0x02)

                if clean_name and clean_name not in ("\x00", "\x01"):
                    rel_path = f"{prefix}/{clean_name}" if prefix else clean_name
                    if is_dir:
                        # Prevent infinite loops in self/parent refs
                        pass
                    else:
                        self.files[rel_path.upper()] = (file_lba, file_size)

                pos += rec_len

    def extract_file(self, filename: str) -> Optional[bytes]:
        entry = self.files.get(filename.upper())
        if not entry:
            return None
        lba, size = entry
        with open(self.disc_path, "rb") as f:
            f.seek(lba * self.sector_size)
            return f.read(size)

    def identify_game(self) -> Dict[str, str]:
        cnf = self.extract_file("SYSTEM.CNF")
        if cnf:
            text = cnf.decode("ascii", errors="ignore")
            for line in text.splitlines():
                if "BOOT" in line:
                    for serial, info in KNOWN_VERSIONS.items():
                        if serial in line.upper():
                            self.boot_serial = serial
                            return info

        for serial, info in KNOWN_VERSIONS.items():
            if serial in self.files:
                self.boot_serial = serial
                return info

        return {"title": "Unknown Persona Disc", "platform": "PlayStation", "serial": "UNKNOWN"}

    def extract_all(self, output_dir: Path) -> int:
        output_dir.mkdir(parents=True, exist_ok=True)
        manifest = {
            "game_info": self.identify_game(),
            "disc_path": str(self.disc_path.name),
            "files": {}
        }

        print(f"\n📦 Extracting {len(self.files)} files to '{output_dir}'...")
        count = 0

        for name, (lba, size) in self.files.items():
            data = self.extract_file(name)
            if data is None:
                continue

            target_path = output_dir / name
            target_path.parent.mkdir(parents=True, exist_ok=True)
            with open(target_path, "wb") as out_f:
                out_f.write(data)

            sha256 = hashlib.sha256(data).hexdigest()
            manifest["files"][name] = {
                "size": size,
                "sha256": sha256
            }
            count += 1
            if count % 10 == 0 or count == len(self.files):
                print(f"  [{count}/{len(self.files)}] {name} ({size} bytes)")

        # Save manifest
        manifest_path = output_dir / "asset_manifest.json"
        with open(manifest_path, "w", encoding="utf-8") as mf:
            json.dump(manifest, mf, indent=2)

        print(f"\n✅ Successfully extracted {count} files.")
        print(f"📄 Manifest written to: {manifest_path}")
        return count

def main():
    parser = argparse.ArgumentParser(description="PersonaRM Asset Extractor & Game Identifier")
    parser.add_argument("disc", type=Path, help="Path to legitimate Persona 2 disc image (.iso / .bin)")
    parser.add_argument("-o", "--output", type=Path, default=Path("assets"), help="Output directory for extracted files")
    parser.add_argument("--info-only", action="store_true", help="Inspect disc without extracting assets")

    args = parser.parse_args()

    extractor = DiscExtractor(args.disc)
    if not extractor.detect_and_open():
        sys.exit(1)

    info = extractor.identify_game()
    print("=" * 60)
    print("  PersonaRM - Disc Inspection")
    print("=" * 60)
    print(f"  Game Title:   {info['title']}")
    print(f"  Platform:     {info['platform']}")
    print(f"  Serial:       {info.get('serial', 'N/A')}")
    print(f"  Files Found:  {len(extractor.files)}")
    print(f"  Sector Size:  {extractor.sector_size} bytes")
    print("=" * 60)

    if args.info_only:
        print("\nFiles on disc:")
        for name, (lba, size) in sorted(extractor.files.items()):
            print(f"  - {name:<30} {size:>10} bytes (LBA {lba})")
        return

    extractor.extract_all(args.output)

if __name__ == "__main__":
    main()

