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
        paths: List[Path] = [repo_root / p.decode('utf-8') for p in parts if p]

        # If repository has submodules, collect their tracked files too
        gitmodules = repo_root / '.gitmodules'
        if gitmodules.exists():
            # parse submodule paths from .gitmodules
            subpaths: List[str] = []
            for line in gitmodules.read_text(encoding='utf-8', errors='ignore').splitlines():
                line = line.strip()
                if line.startswith('path ='):
                    subpaths.append(line.split('=', 1)[1].strip())

            for sp in subpaths:
                subdir = repo_root / sp
                try:
                    cmd2 = ['git', 'ls-files', '-z', '-c', '-o', '--exclude-standard']
                    out2 = subprocess.check_output(cmd2, cwd=subdir)
                    parts2 = out2.split(b"\x00")
                    for p in parts2:
                        if not p:
                            continue
                        # store full path to file inside parent repo
                        paths.append(subdir / p.decode('utf-8'))
                except Exception:
                    # ignore submodule collection errors
                    continue

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


def is_hardcoded_excluded(rel: Path) -> bool:
    excluded_roots = (
        '12ProScrcpy/12ProScrcpyCore/src/third_party',
        '12ProScrcpyCore/src/third_party',
        'CONTRIBUTING.md',
        'CODE_OF_CONDUCT.md',
        '.github',
        'docs/DEVELOPMENT.md',
    )
    s = rel.as_posix()
    for root in excluded_roots:
        if s == root or s.startswith(root + '/'):
            return True
    return False


def matches_simple(patterns: List[str], rel: Path) -> bool:
    # Best-effort simple matching for common patterns. Not a full pathspec impl.
    from fnmatch import fnmatch

    s = str(rel.as_posix())
    path_parts = rel.parts
    
    for pat in patterns:
        # Skip empty patterns and comments
        if not pat or pat.startswith('#'):
            continue
            
        # Handle patterns ending with / (directory-only)
        if pat.endswith('/'):
            dir_pat = pat.rstrip('/').lstrip('/') 
            # Check if this directory or any parent is the pattern
            for i, part in enumerate(path_parts):
                if part == dir_pat:
                    return True
                # Also check full path up to this point
                partial_path = '/'.join(path_parts[:i+1])
                if partial_path == dir_pat:
                    return True
        else:
            # Remove leading / from pattern (means match from root in gitignore)
            clean_pat = pat.lstrip('/')
            
            # Direct match
            if s == clean_pat:
                return True
            # Match against filename only
            if fnmatch(rel.name, clean_pat):
                return True
            # Fnmatch against full path
            if fnmatch(s, clean_pat):
                return True
            # Check if this is a file under an ignored directory
            # e.g., pattern /QtScrcpy/screenshot should match QtScrcpy/screenshot/game.png
            if '/' in clean_pat:
                if s.startswith(clean_pat + '/'):
                    return True
                # Also check if path starts with pattern as a directory component
                for i, part in enumerate(path_parts):
                    partial = '/'.join(path_parts[:i+1])
                    if partial == clean_pat:
                        return True
    
    return False


def walk_and_filter(repo_root: Path, patterns: List[str]) -> Iterable[Path]:
    for p in repo_root.rglob('*'):
        if p.is_dir():
            continue
        rel = p.relative_to(repo_root)
        if is_hardcoded_excluded(rel):
            continue
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

    # Parse gitignore patterns
    patterns = parse_gitignore(repo_root)
    
    # Prefer git to get an accurate list that excludes .gitignore entries
    files = git_list_files(repo_root)
    
    if files:
        # Even with git, apply ignore patterns to handle tracked files that should be ignored
        files = [p for p in files if p.is_file() and p.resolve() != output]
        # Filter out files matching gitignore patterns
        filtered_files = []
        for f in files:
            rel = f.relative_to(repo_root)
            if is_hardcoded_excluded(rel):
                continue
            if not matches_simple(patterns, rel):
                filtered_files.append(f)
        files = filtered_files
    else:
        files = list(walk_and_filter(repo_root, patterns))
        files = [p for p in files if p.resolve() != output]

    create_zip(output, files, repo_root)
    print(f'Wrote {output} ({len(files)} files)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
