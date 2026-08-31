#!/usr/bin/python3
"""Root-only NetworkManager helper for SLOT-GUARD network mode changes."""

import configparser
import os
import subprocess
import sys
from pathlib import Path


CONFIG_PATH = Path("/etc/slotguard/network.conf")
NMCLI_PATH = Path("/usr/bin/nmcli")
SUPPORTED_ACTIONS = {"status", "operation", "development"}
SUPPORTED_MODES = {"operation", "development"}


class NetworkHelperError(RuntimeError):
    pass


def load_config(path=CONFIG_PATH):
    parser = configparser.ConfigParser(interpolation=None)
    try:
        with Path(path).open(encoding="utf-8") as config_file:
            parser.read_file(config_file)
        section = parser["network"]
        config = {
            "interface": section["interface"].strip(),
            "operation_connection": section[
                "operation_connection"
            ].strip(),
            "development_connection": section[
                "development_connection"
            ].strip(),
        }
    except (OSError, KeyError, configparser.Error) as error:
        raise NetworkHelperError(
            f"invalid network helper configuration: {error}"
        ) from error

    if not all(config.values()):
        raise NetworkHelperError("network helper configuration is incomplete")
    if any(
        character not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.:-"
        for character in config["interface"]
    ):
        raise NetworkHelperError("network interface name is invalid")
    return config


def run_nmcli(arguments, timeout_seconds=25):
    try:
        return subprocess.run(
            [str(NMCLI_PATH), *arguments],
            capture_output=True,
            text=True,
            timeout=timeout_seconds,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise NetworkHelperError(f"nmcli execution failed: {error}") from error


def active_connection(config):
    result = run_nmcli(
        [
            "-g",
            "GENERAL.CONNECTION",
            "device",
            "show",
            config["interface"],
        ],
        timeout_seconds=5,
    )
    if result.returncode != 0:
        raise NetworkHelperError(
            (result.stderr or result.stdout).strip()
            or "failed to read the active Wi-Fi connection"
        )
    return result.stdout.strip()


def network_mode(config):
    connection = active_connection(config)
    if connection == config["operation_connection"]:
        return "operation"
    if connection == config["development_connection"]:
        return "development"
    return "unavailable"


def activate_connection(config, connection_name):
    result = run_nmcli(
        [
            "--wait",
            "20",
            "connection",
            "up",
            connection_name,
            "ifname",
            config["interface"],
        ]
    )
    if result.returncode != 0:
        raise NetworkHelperError(
            (result.stderr or result.stdout).strip()
            or f"failed to activate connection: {connection_name}"
        )


def switch_mode(config, target_mode):
    if target_mode not in SUPPORTED_MODES:
        raise ValueError(f"unsupported network mode: {target_mode}")

    target_connection = config[f"{target_mode}_connection"]
    try:
        activate_connection(config, target_connection)
        active_mode = network_mode(config)
        if active_mode != target_mode:
            raise NetworkHelperError(
                f"target connection did not become active: {target_connection}"
            )
        return active_mode
    except NetworkHelperError as switch_error:
        if target_mode == "development":
            try:
                activate_connection(config, config["operation_connection"])
            except NetworkHelperError as fallback_error:
                raise NetworkHelperError(
                    f"development switch failed ({switch_error}); "
                    f"operation fallback also failed ({fallback_error})"
                ) from switch_error
            raise NetworkHelperError(
                f"development switch failed; operation mode restored: "
                f"{switch_error}"
            ) from switch_error
        raise


def main(argv=None):
    arguments = sys.argv[1:] if argv is None else list(argv)
    if os.geteuid() != 0:
        print("slotguard-network must run as root", file=sys.stderr)
        return 1
    if len(arguments) != 1 or arguments[0] not in SUPPORTED_ACTIONS:
        print(
            "usage: slotguard-network <status|operation|development>",
            file=sys.stderr,
        )
        return 2
    if not NMCLI_PATH.is_file():
        print(f"nmcli not found: {NMCLI_PATH}", file=sys.stderr)
        return 1

    try:
        config = load_config()
        action = arguments[0]
        mode = network_mode(config) if action == "status" else switch_mode(
            config,
            action,
        )
    except (NetworkHelperError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 1

    print(mode)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
