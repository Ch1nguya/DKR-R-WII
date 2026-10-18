#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
manifest="$project_root/patches/manifest.json"

python3 - "$project_root" "$manifest" <<'PY'
import hashlib
import json
import pathlib
import subprocess
import sys

root = pathlib.Path(sys.argv[1])
manifest_path = pathlib.Path(sys.argv[2])
data = json.loads(manifest_path.read_text(encoding="utf-8"))
if data.get("schemaVersion") != 1:
    raise SystemExit("Unsupported patch manifest schema")

for dependency in data["dependencies"]:
    repo = root / dependency["repositoryPath"]
    if not (repo / ".git").is_dir():
        print(f"[SKIP] {dependency['name']}: checkout not present at {repo}")
        continue
    commit = subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()
    if commit != dependency["expectedCommit"]:
        raise SystemExit(f"{dependency['name']} commit mismatch: expected {dependency['expectedCommit']}, got {commit}")

    patches = []
    for entry in dependency["patches"]:
        patch = root / entry["path"]
        digest = hashlib.sha256(patch.read_bytes()).hexdigest()
        if digest != entry["sha256"]:
            raise SystemExit(f"Patch checksum mismatch: {entry['path']}")
        patches.append((entry, patch))

    # Check if every patch can be cleanly reverse-applied (all already applied).
    all_applied = all(
        subprocess.run(["git", "-C", str(repo), "apply", "--reverse", "--check", str(p)], capture_output=True).returncode == 0
        for _, p in patches
    )
    if all_applied:
        for entry, _ in patches:
            print(f"[OK] {dependency['name']}: {entry['path']} (already-applied)")
        continue

    # Check if every patch can be cleanly forward-applied (none applied yet).
    all_unapplied = all(
        subprocess.run(["git", "-C", str(repo), "apply", "--check", str(p)], capture_output=True).returncode == 0
        for _, p in patches
    )
    if all_unapplied:
        for entry, p in patches:
            subprocess.run(["git", "-C", str(repo), "apply", str(p)], check=True)
            print(f"[OK] {dependency['name']}: {entry['path']} (applied)")
        continue

    # Partially applied or context-shifted: reset to the pinned commit and
    # re-apply everything. Safe because the commit is pinned and verified.
    print(f"[INFO] {dependency['name']}: resetting to pinned commit and re-applying all patches")
    subprocess.run(["git", "-C", str(repo), "checkout", "--", "."], check=True)
    subprocess.run(["git", "-C", str(repo), "clean", "-fd"], check=True)
    for entry, p in patches:
        subprocess.run(["git", "-C", str(repo), "apply", str(p)], check=True)
        print(f"[OK] {dependency['name']}: {entry['path']} (applied)")
PY
