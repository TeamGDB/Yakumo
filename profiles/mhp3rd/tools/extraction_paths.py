"""Validate archive names before writing below a caller-selected directory."""

from pathlib import Path
import os


def archive_component(name):
    """Accept one portable filename, never a path supplied by an archive."""
    reserved = {"CON", "PRN", "AUX", "NUL"} | {f"{prefix}{number}" for prefix in ("COM", "LPT") for number in range(1, 10)}
    if (name.split(".", 1)[0].upper() in reserved or not name or name in (".", "..") or any(c in name for c in '/\\:')
            or any(ord(c) < 32 or ord(c) == 127 for c in name)
            or name.endswith((".", " "))):
        raise ValueError(f"unsafe archive filename: {name!r}")
    return name


def extraction_path(directory, components):
    """Reject traversal and existing symlinks that lead outside the output root."""
    root = os.path.normcase(os.path.realpath(directory))
    destination = os.path.normcase(os.path.realpath(os.path.join(
        root, *(archive_component(name) for name in components))))
    # Include the separator: a neighboring directory with the same prefix is
    # outside the root. realpath also resolves existing symlink components.
    if not destination.startswith(root.rstrip(os.sep) + os.sep) or destination == root:
        raise ValueError("archive destination escapes the output directory")
    return Path(destination)
