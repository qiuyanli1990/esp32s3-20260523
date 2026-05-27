import sys
import os
import json
import zipfile
import argparse
import re
import shlex
import subprocess
from pathlib import Path
from typing import Optional

# Switch to project root directory
os.chdir(Path(__file__).resolve().parent.parent)

################################################################################
# Common utility functions
################################################################################

def get_board_type_from_compile_commands() -> Optional[str]:
    """Parse the current compiled BOARD_TYPE from build/compile_commands.json"""
    compile_file = Path("build/compile_commands.json")
    if not compile_file.exists():
        return None
    with compile_file.open(encoding='utf-8') as f:
        data = json.load(f)
    for item in data:
        if not item["file"].endswith("main.cc"):
            continue
        cmd = item["command"]
        if "-DBOARD_TYPE=\\\"" in cmd:
            return cmd.split("-DBOARD_TYPE=\\\"")[1].split("\\\"")[0].strip()
    return None


def get_project_version() -> Optional[str]:
    """Read set(PROJECT_VER "x.y.z") from root CMakeLists.txt"""
    with Path("CMakeLists.txt").open(encoding='utf-8') as f:
        for line in f:
            if line.startswith("set(PROJECT_VER"):
                return line.split("\"")[1]
    return None


def _run_command(cmd: list[str]) -> None:
    print(f"+ {shlex.join(cmd)}")
    try:
        subprocess.run(cmd, check=True)
    except subprocess.CalledProcessError as exc:
        print(
            f"Command failed with exit code {exc.returncode}: {shlex.join(cmd)}",
            file=sys.stderr,
        )
        sys.exit(exc.returncode)


def merge_bin(build_dir: Path = Path("build")) -> None:
    _run_command(["idf.py", f"-B{build_dir}", "merge-bin"])


def zip_bin(name: str, version: str, build_dir: Path = Path("build")) -> None:
    """Zip build_dir/merged-binary.bin to releases/v{version}_{name}.zip"""
    out_dir = Path("releases")
    out_dir.mkdir(exist_ok=True)
    output_path = out_dir / f"v{version}_{name}.zip"

    if output_path.exists():
        output_path.unlink()

    with zipfile.ZipFile(output_path, "w", compression=zipfile.ZIP_DEFLATED) as zipf:
        zipf.write(build_dir / "merged-binary.bin", arcname="merged-binary.bin")
    print(f"zip bin to {output_path} done")


def print_flash_instructions(build_dir: Path) -> None:
    print("Flash this variant with:")
    print(f"  idf.py -B {build_dir} flash")
    print(f"  idf.py -B {build_dir} monitor")
    print(f"  idf.py -B {build_dir} flash monitor")
    print("Do not use plain `idf.py flash monitor` here, because that uses the root `build/` directory.")
    print("Note: `flash` does not erase device NVS. If you need a clean device state, use `idf.py -B "
          f"{build_dir} erase-flash flash monitor` on the target board.")

def _get_manufacturer(cfg: dict) -> Optional[str]:
    """Read manufacturer from config.json"""
    m = cfg.get("manufacturer")
    if isinstance(m, str) and m.strip():
        return m.strip()
    return None

################################################################################
# board / variant related functions
################################################################################

_BOARDS_DIR = Path("main/boards")
_BUILD_DIR = Path(".build")
_SDKCONFIG_DIR = Path(".sdkconfig")
_SDKCONFIG_DEFAULTS_DIR = Path(".sdkconfig.defaults")

def _collect_variants(config_filename: str = "config.json") -> list[dict[str, str]]:
    """Traverse all boards under main/boards, collect variant information.

    Return example:
        [{"board": "bread-compact-ml307", "name": "bread-compact-ml307", "full_name": "bread-compact-ml307"}, ...]
        [{"board": "waveshare/esp32-p4-nano", "name": "esp32-p4-nano-10.1-a", "full_name": "waveshare-esp32-p4-nano-10.1-a"}, ...]
    """
    variants: list[dict[str, str]] = []
    errors: list[str] = []

    for cfg_path in _BOARDS_DIR.rglob(config_filename):
        board_dir = cfg_path.parent
        if board_dir.name == "common":
            continue
        board = board_dir.relative_to(_BOARDS_DIR).as_posix()

        try:
            with cfg_path.open(encoding='utf-8') as f:
                cfg = json.load(f)

            manufacturer = _get_manufacturer(cfg)

            # Check manufacturer consistency with directory structure
            if "/" in board:
                # Board is in a subdirectory (e.g., waveshare/esp32-p4-nano)
                expected_manufacturer = board.split("/")[0]
                if not manufacturer:
                    errors.append(
                        f"{cfg_path}: Board is in '{expected_manufacturer}/' subdirectory, "
                        f"but config.json is missing \"manufacturer\": \"{expected_manufacturer}\""
                    )
                elif manufacturer != expected_manufacturer:
                    errors.append(
                        f"{cfg_path}: manufacturer mismatch, "
                        f"directory is '{expected_manufacturer}/' but config.json has \"{manufacturer}\""
                    )
            else:
                # Board is directly under boards/ directory
                if manufacturer:
                    errors.append(
                        f"{cfg_path}: Board is not in a manufacturer subdirectory, "
                        f"but config.json defines manufacturer \"{manufacturer}\", "
                        f"please move board to main/boards/{manufacturer}/{board}/"
                    )

            for build in cfg.get("builds", []):
                name = build["name"]
                full_name = f"{manufacturer}-{name}" if manufacturer else name
                variants.append({
                    "board": board, 
                    "name": name,
                    "full_name": full_name
                })

        except Exception as e:
            print(f"[ERROR] Failed to parse {cfg_path}: {e}", file=sys.stderr)

    # Report all errors at once
    if errors:
        print("\n[ERROR] Found manufacturer configuration issues:", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        print(file=sys.stderr)
        sys.exit(1)

    return variants



def _find_board_config_candidates(board_type: str) -> list[str]:
    """Find all CONFIG_BOARD_TYPE_xxx candidates for the given board_type."""
    board_leaf = board_type.split("/")[-1]
    pattern = f'set(BOARD_TYPE "{board_leaf}")'

    cmake_file = Path("main/CMakeLists.txt")
    lines = cmake_file.read_text(encoding="utf-8").splitlines()
    candidates: list[str] = []

    for idx, line in enumerate(lines):
        if pattern in line:
            # Found the BOARD_TYPE line, search backwards for the nearest config guard
            for back_idx in range(idx - 1, -1, -1):
                back_line = lines[back_idx]
                if "if(CONFIG_BOARD_TYPE_" in back_line:
                    candidates.append(back_line.strip().split("if(")[1].split(")")[0])
                    break
    return candidates


def _extract_board_config_from_sdkconfig_append(sdkconfig_append: list[str]) -> Optional[str]:
    """Extract explicit CONFIG_BOARD_TYPE_xxx=y from sdkconfig_append, if present."""
    pattern = re.compile(r"^(CONFIG_BOARD_TYPE_[A-Z0-9_]+)=y$")
    matches = []
    for item in sdkconfig_append:
        m = pattern.match(item.strip())
        if m:
            matches.append(m.group(1))
    if not matches:
        return None
    uniq = list(dict.fromkeys(matches))
    if len(uniq) > 1:
        raise ValueError(f"Multiple board type configs found in sdkconfig_append: {uniq}")
    return uniq[0]


def _symbol_supports_target(symbol: str, target: str) -> bool:
    """Check whether Kconfig symbol depends on given target (e.g. esp32c5)."""
    kconfig_file = Path("main/Kconfig.projbuild")
    if not kconfig_file.exists():
        return False

    target_flag = f"IDF_TARGET_{target.upper()}"
    lines = kconfig_file.read_text(encoding="utf-8").splitlines()

    in_symbol = False
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("config "):
            curr_symbol = stripped.split("config ", 1)[1].strip()
            in_symbol = curr_symbol == symbol
            continue
        if in_symbol and stripped.startswith(("config ", "choice ", "endchoice", "menu ", "endmenu")):
            break
        if in_symbol and "depends on" in stripped and target_flag in stripped:
            return True
    return False


def _resolve_board_config(board_type: str, target: str, sdkconfig_append: list[str]) -> str:
    """Resolve CONFIG_BOARD_TYPE_xxx for current board build."""
    explicit = _extract_board_config_from_sdkconfig_append(sdkconfig_append)
    if explicit:
        return explicit

    candidates = _find_board_config_candidates(board_type)
    if not candidates:
        raise ValueError(f"Cannot find board config symbol for {board_type}")
    if len(candidates) == 1:
        return candidates[0]

    by_target = [c for c in candidates if _symbol_supports_target(c, target)]
    if len(by_target) == 1:
        return by_target[0]
    if len(by_target) > 1:
        selected = by_target[0]
        print(
            f"[WARN] Ambiguous board config for {board_type} (target={target}), "
            f"target-matched candidates={by_target}, selecting first: {selected}",
            file=sys.stderr,
        )
        return selected

    target_u = target.upper()
    target_short = target_u.replace("ESP32", "")
    by_name = [
        c for c in candidates
        if target_u in c or f"_{target_short}" in c
    ]
    if len(by_name) == 1:
        return by_name[0]
    if len(by_name) > 1:
        selected = by_name[0]
        print(
            f"[WARN] Ambiguous board config for {board_type} (target={target}), "
            f"name-matched candidates={by_name}, selecting first: {selected}",
            file=sys.stderr,
        )
        return selected

    selected = candidates[0]
    print(
        f"[WARN] Ambiguous board config for {board_type} (target={target}), "
        f"candidates={candidates}, selecting first: {selected}",
        file=sys.stderr,
    )
    return selected


def _sanitize_variant_name(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "-", name)


def _sdkconfig_key(entry: str) -> str:
    unset_match = re.fullmatch(r"#\s*(CONFIG_[A-Z0-9_]+)\s+is not set", entry)
    if unset_match:
        return unset_match.group(1)
    return entry.split("=", 1)[0].strip()


def _read_sdkconfig_fragment(path: Path) -> list[str]:
    if not path.exists():
        return []

    entries: list[str] = []
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line:
            continue
        if line.startswith("#") and not re.fullmatch(r"#\s*CONFIG_[A-Z0-9_]+\s+is not set", line):
            continue
        entries.append(line)
    return entries


def _merge_sdkconfig_entries(entry_groups: list[list[str]]) -> list[str]:
    merged: list[str] = []
    index_by_key: dict[str, int] = {}

    for entries in entry_groups:
        for entry in entries:
            key = _sdkconfig_key(entry)
            if key in index_by_key:
                merged[index_by_key[key]] = entry
            else:
                index_by_key[key] = len(merged)
                merged.append(entry)
    return _normalize_choice_entries(merged)


_CHOICE_GROUP_PATTERNS = [
    re.compile(r"^CONFIG_ESPTOOLPY_FLASHSIZE_[A-Z0-9]+$"),
    re.compile(r"^CONFIG_PARTITION_TABLE_(SINGLE_APP|SINGLE_APP_LARGE|TWO_OTA|TWO_OTA_LARGE|CUSTOM)$"),
    re.compile(r"^CONFIG_LANGUAGE_[A-Z0-9_]+$"),
    re.compile(r"^CONFIG_FLASH_(NONE|DEFAULT|CUSTOM|EXPRESSION)_ASSETS$"),
    re.compile(r"^CONFIG_USE_(CHAT|EMOTE)_MESSAGE_STYLE$"),
]


def _normalize_choice_entries(entries: list[str]) -> list[str]:
    normalized = list(entries)

    for pattern in _CHOICE_GROUP_PATTERNS:
        matching_indexes = [
            idx for idx, entry in enumerate(normalized)
            if pattern.fullmatch(_sdkconfig_key(entry))
        ]
        if len(matching_indexes) <= 1:
            continue
        keep_index = matching_indexes[-1]
        normalized = [
            entry for idx, entry in enumerate(normalized)
            if idx == keep_index or idx not in matching_indexes
        ]

    return normalized


def _seed_fragment_paths(board_type: str, target: str) -> list[Path]:
    board_dir = _BOARDS_DIR / Path(board_type)
    return [
        Path("sdkconfig.defaults"),
        Path(f"sdkconfig.defaults.{target}"),
        board_dir / "sdkconfig.defaults",
        board_dir / f"sdkconfig.defaults.{target}",
    ]


def _write_seed_sdkconfig(
    path: Path,
    board_type: str,
    fragment_paths: list[Path],
    compat_entries: list[str],
    *,
    generator_name: str = "release.py",
) -> list[Path]:
    path.parent.mkdir(parents=True, exist_ok=True)

    existing_fragment_paths = [fragment.resolve() for fragment in fragment_paths if fragment.exists()]
    merged_entries = _merge_sdkconfig_entries(
        [_read_sdkconfig_fragment(fragment) for fragment in existing_fragment_paths] + [compat_entries]
    )

    with path.open("w", encoding="utf-8") as f:
        f.write(f"# Generated by {generator_name} for {board_type}\n")
        for fragment in existing_fragment_paths:
            f.write(f"# seed: {fragment}\n")
        f.write("# seed: generated compatibility entries\n")
        for entry in merged_entries:
            f.write(f"{entry}\n")

    return existing_fragment_paths


def _ensure_empty_defaults_file() -> Path:
    path = (_SDKCONFIG_DEFAULTS_DIR / "empty.defaults").resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        path.write_text("# Intentionally empty. Overrides project-level sdkconfig.defaults.\n", encoding="utf-8")
    return path


# Kconfig "select" entries are not automatically applied when we convert
# config.json compatibility entries into generated seed sdkconfig files, so add
# the required dependencies here to mimic menuconfig behaviour.
_AUTO_SELECT_RULES: dict[str, list[str]] = {
    "CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING": [
        "CONFIG_BT_ENABLED=y",
        "CONFIG_BT_BLUEDROID_ENABLED=y",
        "CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y",
        "CONFIG_BT_BLE_50_FEATURES_SUPPORTED=n",
        "CONFIG_BT_BLE_BLUFI_ENABLE=y",
        "CONFIG_MBEDTLS_DHM_C=y",
    ],
}


def _apply_auto_selects(sdkconfig_append: list[str]) -> list[str]:
    """Apply hardcoded auto-select rules to sdkconfig_append."""
    items: list[str] = []
    existing_keys: set[str] = set()

    def _append_if_missing(entry: str) -> None:
        key = entry.split("=", 1)[0]
        if key not in existing_keys:
            items.append(entry)
            existing_keys.add(key)

    # Preserve original order while tracking keys
    for entry in sdkconfig_append:
        _append_if_missing(entry)

    # Apply auto-select rules
    for key, deps in _AUTO_SELECT_RULES.items():
        for entry in sdkconfig_append:
            name, _, value = entry.partition("=")
            if name == key and value.lower().startswith("y"):
                for dep in deps:
                    _append_if_missing(dep)
                break

    return items

################################################################################
# Check board_type in CMakeLists
################################################################################

def _board_type_exists(board_type: str) -> bool:
    cmake_file = Path("main/CMakeLists.txt").read_text(encoding="utf-8")
    board_leaf = board_type.split("/")[-1]
    pattern = f'set(BOARD_TYPE "{board_leaf}")'
    return pattern in cmake_file

################################################################################
# Compile implementation
################################################################################

def release(board_type: str, config_filename: str = "config.json", *, filter_name: Optional[str] = None) -> None:
    """Compile and package all/specified variants of the specified board_type

    Args:
        board_type: directory name under main/boards
        config_filename: config.json name (default: config.json)
        filter_name: if specified, only compile the build["name"] that matches
    """
    cfg_path = _BOARDS_DIR / Path(board_type) / config_filename
    if not cfg_path.exists():
        print(f"[WARN] {cfg_path} does not exist, skipping {board_type}")
        return

    project_version = get_project_version()
    print(f"Project Version: {project_version} ({cfg_path})")

    with cfg_path.open(encoding='utf-8') as f:
        cfg = json.load(f)
    target = cfg["target"]
    manufacturer = _get_manufacturer(cfg)

    builds = cfg.get("builds", [])
    if filter_name:
        builds = [b for b in builds if b["name"] == filter_name]
        if not builds:
            print(f"[ERROR] Variant {filter_name} not found in {board_type}'s {config_filename}", file=sys.stderr)
            sys.exit(1)

    for build in builds:
        name = build["name"]
        board_leaf = board_type.split("/")[-1]

        if board_leaf not in name:
            raise ValueError(f"build.name {name} must contain {board_leaf}")
        
        final_name = f"{manufacturer}-{name}" if manufacturer else name
        output_path = Path("releases") / f"v{project_version}_{final_name}.zip"
        if output_path.exists():
            print(f"Skipping {final_name} because {output_path} already exists")
            continue

        # Process sdkconfig_append
        build_sdkconfig_append = build.get("sdkconfig_append", [])
        explicit_board_cfg = _extract_board_config_from_sdkconfig_append(build_sdkconfig_append)
        if explicit_board_cfg:
            print(
                f"[INFO] Board config explicitly set in config.json: {explicit_board_cfg}, "
                "skip auto-select.",
            )
            sdkconfig_append = list(build_sdkconfig_append)
        else:
            board_type_config = _resolve_board_config(board_type, target, build_sdkconfig_append)
            sdkconfig_append = [f"{board_type_config}=y"]
            sdkconfig_append.extend(build_sdkconfig_append)
        sdkconfig_append = _apply_auto_selects(sdkconfig_append)
        build_dir = (_BUILD_DIR / _sanitize_variant_name(final_name)).resolve()
        sdkconfig_path = (_SDKCONFIG_DIR / f"{_sanitize_variant_name(final_name)}.sdkconfig").resolve()
        seed_fragments = _seed_fragment_paths(board_type, target)
        seed_fragment_files = _write_seed_sdkconfig(
            sdkconfig_path,
            board_type,
            seed_fragments,
            sdkconfig_append,
        )
        empty_defaults = _ensure_empty_defaults_file()
        build_dir.parent.mkdir(parents=True, exist_ok=True)

        print("-" * 80)
        print(f"name: {final_name}")
        print(f"target: {target}")
        if manufacturer:
            print(f"manufacturer: {manufacturer}")
        print(f"build_dir: {build_dir}")
        print(f"sdkconfig: {sdkconfig_path}")
        for fragment in seed_fragment_files:
            print(f"sdkconfig_seed: {fragment}")
        print(f"sdkconfig_defaults_override: {empty_defaults}")
        for item in sdkconfig_append:
            print(f"sdkconfig_append: {item}")

        os.environ.pop("IDF_TARGET", None)
        sys.stdout.flush()

        # Build with board-specific sdkconfig output in an isolated build dir.
        # We pass IDF_TARGET directly instead of using `set-target`, because
        # `set-target` renames the seeded sdkconfig file away before CMake
        # loads it.
        _run_command([
            "idf.py",
            f"-B{build_dir}",
            f"-DIDF_TARGET={target}",
            f"-DSDKCONFIG={sdkconfig_path}",
            f"-DSDKCONFIG_DEFAULTS={empty_defaults}",
            f"-DBOARD_NAME={name}",
            f"-DBOARD_TYPE={board_type}",
            "reconfigure",
            "build",
        ])

        # merge-bin
        merge_bin(build_dir)

        # Zip
        zip_bin(final_name, project_version, build_dir)
        print_flash_instructions(build_dir)

################################################################################
# CLI entry
################################################################################

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("board", nargs="?", default=None, help="Board type or 'all'")
    parser.add_argument("-c", "--config", default="config.json", help="Config filename (default: config.json)")
    parser.add_argument("--list-boards", action="store_true", help="List all supported boards and variants")
    parser.add_argument("--json", action="store_true", help="Output in JSON format (use with --list-boards)")
    parser.add_argument("--name", help="Variant name to compile (original name without manufacturer prefix)")

    args = parser.parse_args()

    # List mode
    if args.list_boards:
        variants = _collect_variants(config_filename=args.config)
        if args.json:
            print(json.dumps(variants))
        else:
            for v in variants:
                print(f"{v['board']}: {v['name']}")
        sys.exit(0)

    # Current directory firmware packaging mode
    if args.board is None:
        merge_bin()
        curr_board_type = get_board_type_from_compile_commands()
        if curr_board_type is None:
            print("Failed to parse board_type from compile_commands.json", file=sys.stderr)
            sys.exit(1)
        project_ver = get_project_version()
        zip_bin(curr_board_type, project_ver)
        sys.exit(0)

    # Compile mode
    board_type_input: str = args.board
    name_filter: Optional[str] = args.name

    # Check board_type in CMakeLists
    if board_type_input != "all" and not _board_type_exists(board_type_input):
        print(f"[ERROR] board_type {board_type_input} not found in main/CMakeLists.txt", file=sys.stderr)
        sys.exit(1)

    variants_all = _collect_variants(config_filename=args.config)

    # Filter board_type list
    target_board_types: set[str]
    if board_type_input == "all":
        target_board_types = {v["board"] for v in variants_all}
    else:
        target_board_types = {board_type_input}

    for bt in sorted(target_board_types):
        if not _board_type_exists(bt):
            print(f"[ERROR] board_type {bt} not found in main/CMakeLists.txt", file=sys.stderr)
            sys.exit(1)
        cfg_path = _BOARDS_DIR / bt / args.config
        if bt == board_type_input and not cfg_path.exists():
            print(f"Board {bt} has no {args.config} config file, skipping")
            sys.exit(0)
        release(bt, config_filename=args.config, filter_name=name_filter if bt == board_type_input else None)
