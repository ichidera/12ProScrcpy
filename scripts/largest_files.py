import subprocess
from pathlib import Path
from fnmatch import fnmatch

TOP_N = 10


def find_repo_root(start: Path) -> Path:
    """Find the git repository root starting from the given path."""
    p = start.resolve()
    for parent in [p] + list(p.parents):
        if (parent / '.git').exists() or (parent / '.gitignore').exists():
            return parent
    return Path.cwd()


def parse_gitignore(repo_root: Path) -> list[str]:
    """Parse .gitignore file and return list of patterns."""
    gi = repo_root / '.gitignore'
    if not gi.exists():
        return []
    patterns: list[str] = []
    for line in gi.read_text(encoding='utf-8', errors='ignore').splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        patterns.append(line)
    return patterns


def matches_simple(patterns: list[str], rel: Path) -> bool:
    """Check if a path matches any gitignore patterns."""
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
            if '/' in clean_pat:
                if s.startswith(clean_pat + '/'):
                    return True
                # Check if path starts with pattern as a directory component
                for i, part in enumerate(path_parts):
                    partial = '/'.join(path_parts[:i+1])
                    if partial == clean_pat:
                        return True
    
    return False


# Find repo root
repo_root = find_repo_root(Path(__file__).resolve().parent)

# Parse gitignore patterns
patterns = parse_gitignore(repo_root)

# Get tracked + untracked, non-ignored files from Git.
result = subprocess.run(
    ["git", "ls-files", "-co", "--exclude-standard", "-z"],
    capture_output=True,
    cwd=repo_root,
    check=True,
)

files = result.stdout.decode("utf-8", errors="surrogateescape").split("\0")

items = []

for file in files:
    if not file:
        continue

    path = repo_root / file
    rel_path = Path(file)

    # Skip files matching gitignore patterns
    if matches_simple(patterns, rel_path):
        continue

    if not path.is_file():
        continue

    try:
        size = path.stat().st_size
        items.append((size, file))
    except OSError:
        pass

items.sort(reverse=True)

print(f"\nTop {TOP_N} largest non-ignored files:\n")

for size, file in items[:TOP_N]:
    print(f"{size / (1024 ** 2):10.2f} MB  {file}")

