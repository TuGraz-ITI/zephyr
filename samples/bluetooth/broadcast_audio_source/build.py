#
# Parts of this build script are copied from
# the Nodic (nrf-sdk) nRF5340 Audio sample.
#

from dataclasses import InitVar, dataclass, field
from enum import auto, Enum
from zipfile import ZipFile
from pathlib import Path
import subprocess
import argparse
import shutil
import sys
import os
import re

PROJECT_FOLDER = Path(__file__).resolve().parent
HCI_RPMSG_FOLDER = (PROJECT_FOLDER / "../hci_rpmsg").resolve()
BIN_FOLDER = (PROJECT_FOLDER / "bin").resolve()
BUILD_FOLDER_APP = Path(__file__).resolve().parent / "build_app"
BUILD_FOLDER_NET = Path(__file__).resolve().parent / "build_net"
NRF5340_AUDIO_DK_APP_NAME = "nrf5340_audio_dk_nrf5340_cpuapp"
NRF5340_AUDIO_DK_NET_NAME = "nrf5340_audio_dk_nrf5340_cpunet"

class Core(Enum):
    app = "app"
    net = "network"
    both = "both"

@dataclass
class BuildConf:
    core: Core
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
        description=("This script builds and programs the nRF5340"),
        allow_abbrev=False
    )
    parser.add_argument(
        "-p",
        "--program",
        default=False,
        action="store_true",
        help="Will program and reboot nRF5340 Audio DK",
    )
    parser.add_argument(
        "-c",
        "--core",
        type=str,
        choices=[i.name for i in Core],
        help="Select which cores to include in build",
    )
    parser.add_argument(
        "--pristine", default=False, action="store_true", help="Will build cleanly"
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
        core=options.core,
        pristine=options.pristine,
    )

    boards_snr_connected = __find_snr()
    if not boards_snr_connected:
        print("No snrs connected")

    if buildconf.pristine and buildconf.core != None:
        if (buildconf.core == "app" or buildconf.core == "both") and BUILD_FOLDER_APP.exists():
            shutil.rmtree(BUILD_FOLDER_APP)
        if (buildconf.core == "net" or buildconf.core == "both") and BUILD_FOLDER_NET.exists():
            shutil.rmtree(BUILD_FOLDER_NET)

    build_cmd_app = f"west build -b {NRF5340_AUDIO_DK_APP_NAME} --build-dir {BUILD_FOLDER_APP}"
    build_cmd_net = f"west build {HCI_RPMSG_FOLDER} -b {NRF5340_AUDIO_DK_NET_NAME} --build-dir {BUILD_FOLDER_NET} -- -DOVERLAY_CONFIG=nrf5340_cpunet_bis-bt_ll_sw_split.conf"
    flash_cmd_app = f"west flash --build-dir {BUILD_FOLDER_APP}"
    flash_cmd_net = f"west flash --build-dir {BUILD_FOLDER_NET}"

    if (buildconf.core == "app" or buildconf.core == "both"):
        ret_val = os.system(build_cmd_app)
        if ret_val:
            raise Exception("cmake error: " + str(ret_val))
        if options.program:
            ret_val = os.system(flash_cmd_app)
            if ret_val:
                raise Exception("cmake error: " + str(ret_val))
        if options.binaries:
            shutil.copyfile((BUILD_FOLDER_APP / "zephyr/zephyr.hex").resolve(), (BIN_FOLDER / "app.hex").resolve())
    if (buildconf.core == "net" or buildconf.core == "both"):
        ret_val = os.system(build_cmd_net)
        if ret_val:
            raise Exception("cmake error: " + str(ret_val))
        if options.program:
            ret_val = os.system(flash_cmd_net)
            if ret_val:
                raise Exception("cmake error: " + str(ret_val))
        if options.binaries:
            shutil.copyfile((BUILD_FOLDER_NET / "zephyr/zephyr.hex").resolve(), (BIN_FOLDER / "net.hex").resolve())

    if options.binaries:
        if (BIN_FOLDER / "app.hex").resolve().exists() and (BIN_FOLDER / "net.hex").resolve().exists():
            with ZipFile((BIN_FOLDER / "source.zip").resolve(), 'w') as zip:
                zip.write((BIN_FOLDER / "app.hex").resolve())
                zip.write((BIN_FOLDER / "net.hex").resolve())
    
if __name__ == "__main__":
    __main()