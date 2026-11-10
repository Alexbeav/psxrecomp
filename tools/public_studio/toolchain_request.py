"""Explicit-Build toolchain request, confined to this edition's cache."""
import argparse
import json
import os
from pathlib import Path
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("core", type=Path)
    parser.add_argument("cache", type=Path)
    parser.add_argument("result", type=Path)
    args = parser.parse_args()
    cache = args.cache.resolve()
    os.environ["PSX_STUDIO_PROJECT_ONLY"] = "1"
    os.environ["RETCOMM_TOOLCHAIN_CACHE"] = str(cache / "toolchains")
    os.environ["RETCOMM_DATA_HOME"] = str(cache / "retcomm")
    for name in ("RETCOMM_TOOLCHAIN_DIR", "PSXRECOMP_TOOLCHAIN_DIR"):
        os.environ.pop(name, None)
    sys.path.insert(0, str(args.core.resolve() / "tools"))
    import toolchain_pack
    # The adapter does not publish a user's PATH, even for a custom source
    # implementing the older supported toolchain API.
    toolchain_pack.shared_cache_roots = lambda: [cache / "toolchains"]
    toolchain_pack.retcomm_data_homes = lambda: [cache / "retcomm"]
    toolchain_pack.register_toolchain_user_env = lambda root, log=None: toolchain_pack.unwrap_pack_root(root)
    host = args.result.parent / "toolchain-host"
    host.mkdir(parents=True, exist_ok=True)
    binary = toolchain_pack.ensure_toolchain(project_root=host, download=True, log=print).resolve()
    metadata = binary.parent / "retcomm-toolchain.json"
    record = {"bin": str(binary), "metadata": json.loads(metadata.read_text(encoding="utf-8")),
              "global_environment_published": False}
    args.result.write_text(json.dumps(record, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
