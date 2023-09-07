#
# Parts of this build script are copied from
# the Nodic (nrf-sdk) nRF5340 Audio sample.
#

from dataclasses import dataclass
from pathlib import Path
import subprocess
import argparse
import shutil
import sys
import os
import re

PROJECT_FOLDER = Path(__file__).resolve().parent
BIN_FOLDER = (PROJECT_FOLDER / "bin").resolve()
BUILD_FOLDER = Path(__file__).resolve().parent / "build"
NRF52840_DK_NAME = "nrf52840dk_nrf52840"
NRF52840_DONGLE_NAME = "nrf52840dongle_nrf52840"

@dataclass
class BuildConf:
    pristine: bool

def __find_snr():
    stdout = subprocess.check_output(
        "nrfjprog --ids", shell=True).decode("utf-8")
    snrs = re.findall(r"([\d]+)", stdout)

    if not snrs:
        print("No programmer/debugger connected to PC")

    return list(map(int, snrs))

def __main():
    parser = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=("This script builds and programs the nRF52840"),
        allow_abbrev=False
    )
    parser.add_argument(
        "-p",
        "--program",
        default=False,
        action="store_true",
        help="Will program and reboot nRF52840 DK",
    )
    parser.add_argument(
        "--pristine", default=False, action="store_true", help="Will build cleanly"
    )
    parser.add_argument(
        "--dongle", default=False, action="store_true", help="Use nRF52840 Dongle instead of DK"
    )
    parser.add_argument(
        "-b",
        "--binaries",
        default=False,
        action="store_true",
        help="Will copy the compiled binaries in the bin folder",
    )
    options = parser.parse_args(args=sys.argv[1:])

    buildconf = BuildConf(
        pristine=options.pristine,
    )

    boards_snr_connected = __find_snr()
    if not boards_snr_connected:
        print("No snrs connected")

    if buildconf.pristine and BUILD_FOLDER.exists():
        shutil.rmtree(BUILD_FOLDER)

    if options.dongle:
        build_cmd = f"west build -b {NRF52840_DONGLE_NAME}"
    else:
        build_cmd = f"west build -b {NRF52840_DK_NAME}"
    
    flash_cmd = f"west flash"

    ret_val = os.system(build_cmd)
    if ret_val:
        raise Exception("cmake error: " + str(ret_val))
    if options.program:
        ret_val = os.system(flash_cmd)
        if ret_val:
            raise Exception("cmake error: " + str(ret_val))
    if options.binaries:
        shutil.copyfile((BUILD_FOLDER / "zephyr/zephyr.hex").resolve(), (BIN_FOLDER / "mallory.hex").resolve())
    
if __name__ == "__main__":
    __main()