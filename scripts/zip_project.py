#!/usr/bin/env python3
"""
Create a zip archive of the project root, excluding paths listed in .gitignore.

Usage:
  python scripts/zip_project.py [--output project.zip]

By default writes `project.zip` at the repository root and overwrites any
existing file with the same name.
"""
from __future__ import annotations

import argparse
import subprocess
from pathlib import Path
import zipfile
from typing import Iterable, List


def find_repo_root(start: Path) -> Path:
    p = start.resolve()
    for parent in [p] + list(p.parents):
        if (parent / '.git').exists() or (parent / '.gitignore').exists():
            return parent
    return Path.cwd()


def git_list_files(repo_root: Path) -> List[Path]:
    try:
        # -c tracked files, -o others (untracked), --exclude-standard filters .gitignore
        cmd = ['git', 'ls-files', '-z', '-c', '-o', '--exclude-standard']
        out = subprocess.check_output(cmd, cwd=repo_root)
        if not out:
            return []
        parts = out.split(b"\x00")
        paths = [repo_root / p.decode('utf-8') for p in parts if p]
        return paths
    except Exception:
        return []


def parse_gitignore(repo_root: Path) -> List[str]:
    gi = repo_root / '.gitignore'
    if not gi.exists():
        return []
    patterns: List[str] = []
    for line in gi.read_text(encoding='utf-8', errors='ignore').splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        patterns.append(line)
    return patterns


def matches_simple(patterns: List[str], rel: Path) -> bool:
    # Best-effort simple matching for common patterns. Not a full pathspec impl.
    from fnmatch import fnmatch

    s = str(rel.as_posix())
    for pat in patterns:
        if pat.endswith('/'):
            if s.startswith(pat.rstrip('/')):
                return True
        if fnmatch(s, pat) or fnmatch(rel.name, pat):
            return True
    return False


def walk_and_filter(repo_root: Path, patterns: List[str]) -> Iterable[Path]:
    for p in repo_root.rglob('*'):
        if p.is_dir():
            continue
        rel = p.relative_to(repo_root)
        if matches_simple(patterns, rel):
            continue
        yield p


def create_zip(output: Path, files: Iterable[Path], repo_root: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_DEFLATED) as z:
        for f in files:
            arcname = f.relative_to(repo_root).as_posix()
            if arcname == output.name:
                continue
            z.write(f, arcname)


def main() -> int:
    ap = argparse.ArgumentParser(description='Zip project excluding .gitignore')
    ap.add_argument('--output', '-o', default='12PROSCRCPY.zip', help='output zip name')
    args = ap.parse_args()

    start = Path(__file__).resolve().parent
    repo_root = find_repo_root(start)
    output = (repo_root / args.output).resolve()

    # Prefer git to get an accurate list that excludes .gitignore entries
    files = git_list_files(repo_root)
    if files:
        files = [p for p in files if p.is_file() and p.resolve() != output]
    else:
        patterns = parse_gitignore(repo_root)
        files = list(walk_and_filter(repo_root, patterns))
        files = [p for p in files if p.resolve() != output]

    create_zip(output, files, repo_root)
    print(f'Wrote {output} ({len(files)} files)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
