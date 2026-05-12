import argparse
import json
import re
import shutil
import sys
from pathlib import Path
from typing import Optional

import release as release_tools


_ROOT_SDKCONFIG = Path("sdkconfig")
_ROOT_SDKCONFIG_OLD = Path("sdkconfig.old")
_ROOT_BUILD_CACHE = Path("build/CMakeCache.txt")


def _read_current_target() -> Optional[str]:
    if _ROOT_SDKCONFIG.exists():
        pattern = re.compile(r'^CONFIG_IDF_TARGET="([^"]+)"$')
        for line in _ROOT_SDKCONFIG.read_text(encoding="utf-8").splitlines():
            match = pattern.match(line.strip())
            if match:
                return match.group(1)

    if _ROOT_BUILD_CACHE.exists():
        pattern = re.compile(r"^IDF_TARGET:STRING=(.+)$")
        for line in _ROOT_BUILD_CACHE.read_text(encoding="utf-8").splitlines():
            match = pattern.match(line.strip())
            if match:
                return match.group(1)

    return None


def _select_build(board_type: str, cfg: dict, filter_name: Optional[str]) -> dict:
    builds = cfg.get("builds", [])
    if filter_name:
        matched = [build for build in builds if build["name"] == filter_name]
        if not matched:
            print(
                f"[ERROR] Variant {filter_name} not found in {board_type}'s config.json",
                file=sys.stderr,
            )
            sys.exit(1)
        return matched[0]

    if not builds:
        print(f"[ERROR] No builds defined in {board_type}'s config.json", file=sys.stderr)
        sys.exit(1)

    if len(builds) > 1:
        available = ", ".join(build["name"] for build in builds)
        print(
            f"[ERROR] Board {board_type} has multiple variants. "
            f"Use --name to choose one: {available}",
            file=sys.stderr,
        )
        sys.exit(1)

    return builds[0]


def _build_sdkconfig_append(board_type: str, target: str, build: dict) -> list[str]:
    build_sdkconfig_append = build.get("sdkconfig_append", [])
    explicit_board_cfg = release_tools._extract_board_config_from_sdkconfig_append(
        build_sdkconfig_append
    )
    if explicit_board_cfg:
        sdkconfig_append = list(build_sdkconfig_append)
    else:
        board_type_config = release_tools._resolve_board_config(
            board_type, target, build_sdkconfig_append
        )
        sdkconfig_append = [f"{board_type_config}=y"]
        sdkconfig_append.extend(build_sdkconfig_append)
    return release_tools._apply_auto_selects(sdkconfig_append)


def _backup_root_sdkconfig() -> None:
    if not _ROOT_SDKCONFIG.exists():
        return

    shutil.copyfile(_ROOT_SDKCONFIG, _ROOT_SDKCONFIG_OLD)
    print(f"Backed up current sdkconfig to {_ROOT_SDKCONFIG_OLD}")


def select_board(
    board_type: str,
    config_filename: str = "config.json",
    *,
    filter_name: Optional[str] = None,
) -> None:
    if not release_tools._board_type_exists(board_type):
        print(
            f"[ERROR] board_type {board_type} not found in main/CMakeLists.txt",
            file=sys.stderr,
        )
        sys.exit(1)

    cfg_path = release_tools._BOARDS_DIR / Path(board_type) / config_filename
    if not cfg_path.exists():
        print(f"[ERROR] {cfg_path} does not exist", file=sys.stderr)
        sys.exit(1)

    with cfg_path.open(encoding="utf-8") as f:
        cfg = json.load(f)

    build = _select_build(board_type, cfg, filter_name)
    target = cfg["target"]
    board_name = build["name"]
    sdkconfig_append = _build_sdkconfig_append(board_type, target, build)

    current_target = _read_current_target()
    if current_target != target:
        print(
            f"Switching root development target from "
            f"{current_target or 'unset'} to {target}"
        )
        release_tools._run_command(["idf.py", "set-target", target])
    else:
        _backup_root_sdkconfig()

    seed_fragment_files = release_tools._write_seed_sdkconfig(
        _ROOT_SDKCONFIG.resolve(),
        board_type,
        release_tools._seed_fragment_paths(board_type, target),
        sdkconfig_append,
        generator_name="select_board.py",
    )

    print("-" * 80)
    print(f"Selected board: {board_type}")
    print(f"Variant: {board_name}")
    print(f"Target: {target}")
    print(f"sdkconfig: {_ROOT_SDKCONFIG.resolve()}")
    for fragment in seed_fragment_files:
        print(f"sdkconfig_seed: {fragment}")
    for item in sdkconfig_append:
        print(f"sdkconfig_append: {item}")

    sys.stdout.flush()
    release_tools._run_command(["idf.py", f"-DBOARD_NAME={board_name}", "reconfigure"])

    print("-" * 80)
    print("Development environment is ready. Use:")
    print("  idf.py menuconfig")
    print("  idf.py build")
    print("  idf.py flash monitor")
    print("If you need a clean runtime state on the device, use:")
    print("  idf.py erase-flash flash monitor")
    print("If you switch to another board, run this script again first.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("board", nargs="?", default=None, help="Board type")
    parser.add_argument(
        "-c",
        "--config",
        default="config.json",
        help="Config filename (default: config.json)",
    )
    parser.add_argument(
        "--list-boards",
        action="store_true",
        help="List all supported boards and variants",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Output in JSON format (use with --list-boards)",
    )
    parser.add_argument(
        "--name",
        help="Variant name to select (original name without manufacturer prefix)",
    )

    args = parser.parse_args()

    if args.list_boards:
        variants = release_tools._collect_variants(config_filename=args.config)
        if args.json:
            print(json.dumps(variants))
        else:
            for variant in variants:
                print(f"{variant['board']}: {variant['name']}")
        sys.exit(0)

    if not args.board:
        parser.error("the following arguments are required: board")

    select_board(args.board, config_filename=args.config, filter_name=args.name)
