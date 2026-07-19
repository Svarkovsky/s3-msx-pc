#!/usr/bin/env python3
import argparse
import subprocess
import shutil
import glob
import os
import sys
import re

PROJECT_NAME = "S3-MSX-PC"
DEFAULT_BAUD = os.getenv("RG_TOOL_BAUD", "1152000")
DEFAULT_PORT = os.getenv("RG_TOOL_PORT", "/dev/ttyUSB0")

PROJECT_APPS = {
    'launcher': [0, 0, 0xE0000],
    'fmsx':     [0, 1, 0xD0000],
}

try:
    PROJECT_VER = os.getenv("PROJECT_VER") or subprocess.check_output(
        "git describe --tags --abbrev=5 --dirty --always", shell=True
    ).decode().rstrip()
except:
    PROJECT_VER = "unknown"

TARGETS = []
for t in glob.glob("components/retro-go/targets/*/config.h"):
    TARGETS.append(os.path.basename(os.path.dirname(t)))
DEFAULT_TARGET = "s3-msx-pc"

IDF_TARGET = os.getenv("IDF_TARGET", "esp32s3")
IDF_PATH = os.getenv("IDF_PATH")
if not IDF_PATH:
    exit("IDF_PATH is not defined. Are you running inside esp-idf environment?")

IDF_PY = "idf.py"
ESPTOOL_PY = "esptool.py"
PARTTOOL_PY = "parttool.py"
GEN_ESP32PART_PY = "gen_esp32part.py"


def run(cmd, cwd=None):
    print(f"Running: {' '.join(cmd)}")
    subprocess.run(cmd, cwd=cwd, check=True)


def clean_app(app):
    print(f"Cleaning '{app}'...")
    try:
        shutil.rmtree(os.path.join(app, "build"))
    except:
        pass


def build_app(app, target, is_release=False):
    print(f"Building '{app}' for {target}...")
    args = [IDF_PY, "app",
            f"-DRG_PROJECT_APP={app}",
            f"-DRG_PROJECT_VER={PROJECT_VER}",
            f"-DRG_BUILD_TARGET=RG_TARGET_{re.sub(r'[^A-Z0-9]', '_', target.upper())}",
            f"-DRG_BUILD_RELEASE={1 if is_release else 0}",
            "-DRG_ENABLE_PROFILING=0",
            "-DRG_ENABLE_NETWORKING=1"]
    with open("partitions.csv", "w") as f:
        f.write("dummy, app, ota_0, 65536, 3145728\n")
    run(args, cwd=os.path.join(os.getcwd(), app))


def flash_app(app, port, baudrate):
    os.environ["ESPTOOL_CHIP"] = IDF_TARGET
    os.environ["ESPTOOL_BAUD"] = str(baudrate)
    os.environ["ESPTOOL_PORT"] = port
    if not os.path.exists("partitions.bin"):
        run([ESPTOOL_PY, "read_flash", "0x8000", "0x1000", "partitions.bin"])
        run([GEN_ESP32PART_PY, "partitions.bin"])
    app_bin = os.path.join(app, "build", app + ".bin")
    run([PARTTOOL_PY, "--partition-table-file", "partitions.bin",
         "write_partition", "--partition-name", app, "--input", app_bin])


def flash_image(image_file, port, baudrate):
    os.environ["ESPTOOL_CHIP"] = IDF_TARGET
    os.environ["ESPTOOL_BAUD"] = str(baudrate)
    os.environ["ESPTOOL_PORT"] = port
    run([ESPTOOL_PY, "write_flash", "--flash_size", "detect", "0x0", image_file])


def monitor_app(app, port):
    elf = os.path.join(app, "build", app + ".elf")
    if os.path.exists(elf):
        run([IDF_PY, "monitor", "--port", port, elf])
    else:
        run([IDF_PY, "monitor", "--port", port, "-d", sys.argv[0]])


parser = argparse.ArgumentParser(description="S3-MSX-PC build tool")
parser.add_argument("command", choices=["build", "clean", "flash", "monitor", "release"])
parser.add_argument("apps", nargs="*", default="all", choices=["all", "launcher", "fmsx"])
parser.add_argument("--target", default=DEFAULT_TARGET, choices=TARGETS)
parser.add_argument("--port", default=DEFAULT_PORT)
parser.add_argument("--baud", default=DEFAULT_BAUD)
args = parser.parse_args()

if os.path.exists(f"components/retro-go/targets/{args.target}/env.py"):
    with open(f"components/retro-go/targets/{args.target}/env.py", "rb") as f:
        prev = os.getenv("IDF_TARGET")
        exec(f.read())
        if os.getenv("IDF_TARGET") != prev:
            IDF_TARGET = os.getenv("IDF_TARGET")

if os.path.exists(f"components/retro-go/targets/{args.target}/sdkconfig"):
    os.environ["SDKCONFIG_DEFAULTS"] = os.path.abspath(f"components/retro-go/targets/{args.target}/sdkconfig")

os.environ["IDF_TARGET"] = IDF_TARGET

apps = ["launcher", "fmsx"] if "all" in args.apps else args.apps

try:
    if args.command in ["clean", "release"]:
        for app in apps:
            clean_app(app)

    if args.command in ["build", "release"]:
        for app in apps:
            build_app(app, args.target, args.command == "release")

    if args.command == "flash":
        for app in apps:
            flash_app(app, args.port, args.baud)

    if args.command == "monitor":
        monitor_app(apps[-1], args.port)

    print("Done.")
except KeyboardInterrupt:
    exit("\nAborted.")
except Exception as e:
    exit(f"\nError: {e}")
