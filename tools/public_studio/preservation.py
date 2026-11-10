"""Apply shipped preservation changes after an explicit Build resolves sources."""
import hashlib
import json
import os
from pathlib import Path


def content_identity(root):
    members = {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
               for p in sorted(root.rglob("*")) if p.is_file()}
    digest = hashlib.sha256(json.dumps(members, sort_keys=True).encode()).hexdigest()
    return {"content_sha256": digest, "files": members}


def apply(kind, destination, source_record, runner, profile_root=None):
    if source_record["origin"] == "custom-folder":
        source_record["adaptation"] = "custom-folder-unmodified"
        return
    profile_root = profile_root or Path(__file__).parent / "compatibility"
    manifest = json.loads((profile_root / "profile.json").read_text(encoding="utf-8"))
    patch_record = manifest["patches"][kind]
    patch = profile_root / patch_record["file"]
    if patch.parent.resolve() != profile_root.resolve():
        raise ValueError("Preservation patch must be inside the shipped profile")
    digest = hashlib.sha256(patch.read_bytes()).hexdigest()
    if digest != patch_record["sha256"]:
        raise ValueError("Preservation patch checksum differs from its shipped manifest")
    env = os.environ.copy()
    for name in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE"):
        env.pop(name, None)
    # A snapshot can sit below a user's unrelated repository. Prevent Git from
    # discovering that repository and applying/skipping paths relative to it.
    env["GIT_CEILING_DIRECTORIES"] = str(destination.parent.resolve())
    runner.run(["git", "apply", "--check", patch.resolve()], destination, env=env)
    runner.run(["git", "apply", patch.resolve()], destination, env=env)
    source_record["upstream_content_sha256"] = source_record["content_sha256"]
    source_record["adaptation"] = manifest["profile"]
    source_record["patches"] = [dict(patch_record, sha256=digest)]
    source_record.update(content_identity(destination))
