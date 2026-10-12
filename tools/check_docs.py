#!/usr/bin/env python3
"""Check local Markdown links and images without fetching external URLs."""
import argparse
import os
from pathlib import Path
import re
import sys
from urllib.parse import unquote, urlsplit

DEFINITION = re.compile(r"(?m)^ {0,3}\[([^\]\n]+)\]:[ \t]*(.+)$")
REFERENCE = re.compile(r"!?\[([^\]\n]+)\](?:\[([^\]\n]*)\])?")


def mask_region(text):
    return "".join("\n" if character == "\n" else " " for character in text)


def prose_only(text):
    """Hide code examples/comments while preserving source line numbers."""
    lines = text.splitlines(keepends=True)
    fence = None
    result = []
    for line in lines:
        marker = re.match(r"^ {0,3}(`{3,}|~{3,})(.*)$", line.rstrip("\r\n"))
        if fence is not None:
            result.append(mask_region(line))
            if marker and marker.group(1)[0] == fence[0] and len(marker.group(1)) >= fence[1] \
                    and not marker.group(2).strip():
                fence = None
            continue
        if marker:
            fence = (marker.group(1)[0], len(marker.group(1)))
            result.append(mask_region(line))
        elif line.startswith("    ") or line.startswith("\t"):
            result.append(mask_region(line))
        else:
            result.append(line)
    body = "".join(result)
    body = re.sub(r"<!--[\s\S]*?-->", lambda match: mask_region(match.group()), body)
    body = re.sub(r"(`+)([\s\S]*?)\1", lambda match: mask_region(match.group()), body)
    body = re.sub(r"\\[\[\]]", "  ", body)
    return body


def destination(content):
    """Extract a destination, preserving spaces in angle-bracket destinations."""
    content = content.strip()
    if content.startswith("<"):
        end = content.find(">")
        return content[1:end] if end >= 0 else ""
    # An optional quoted title follows the path; bare spaces are accepted for
    # repository file names even though angle brackets are more portable.
    content = re.sub(r'\s+["\'].*["\']\s*$', "", content)
    return re.sub(r"\\([\\() ])", r"\1", content)


def inline_links(text):
    """Yield (line, destination) for inline links/images with balanced paths."""
    index = 0
    while index < len(text):
        start = text.find("[", index)
        if start < 0:
            return
        end = text.find("](", start + 1)
        if end < 0:
            return
        # Do not let a stray '[' swallow a later link's label.
        nested = text.find("[", start + 1, end)
        if nested >= 0:
            index = nested
            continue
        cursor, depth = end + 2, 1
        begin = cursor
        while cursor < len(text) and depth:
            if text[cursor] == "\\":
                cursor += 2
                continue
            if text[cursor] == "(":
                depth += 1
            elif text[cursor] == ")":
                depth -= 1
            cursor += 1
        if depth:
            return
        yield text.count("\n", 0, start) + 1, destination(text[begin:cursor - 1])
        index = cursor


def reference_label(text):
    return " ".join(text.split()).casefold()


def document_links(text):
    """Resolve full/collapsed/shortcut references alongside inline links."""
    text = prose_only(text)
    definitions = {}
    issues = []
    masked = list(text)
    for match in DEFINITION.finditer(text):
        label = reference_label(match.group(1))
        if label in definitions:
            issues.append((text.count("\n", 0, match.start()) + 1, f"duplicate reference definition {label}"))
        definitions[label] = destination(match.group(2))
        masked[match.start():match.end()] = " " * (match.end() - match.start())
    body = "".join(masked)
    links = list(inline_links(body))
    for match in REFERENCE.finditer(body):
        # Inline targets and their optional titles are handled above.
        if body[match.end():match.end() + 1] == "(":
            continue
        explicit = match.group(2) is not None
        label = reference_label(match.group(2) or match.group(1))
        line = body.count("\n", 0, match.start()) + 1
        if label in definitions:
            links.append((line, definitions[label]))
        elif explicit:
            issues.append((line, f"undefined reference {label}"))
    return sorted(links), issues


def check_document(path, root):
    """Return local-link problems for one Markdown document."""
    path, root = Path(path).resolve(), Path(root).resolve()
    if not path.is_relative_to(root):
        raise ValueError(f"document escapes repository root: {path}")
    display_path = path.relative_to(root).as_posix()
    issues = []
    text = path.read_text(encoding="utf-8")
    links, reference_issues = document_links(text)
    for line, message in reference_issues:
        issues.append(f"{display_path}:{line}: {message}")
    for line, target in links:
        parts = urlsplit(target)
        if parts.scheme or parts.netloc:
            continue
        if not parts.path:
            continue
        resolved = (path.parent / unquote(parts.path)).resolve()
        if not resolved.is_relative_to(root):
            issues.append(f"{display_path}:{line}: target escapes repository root {target}")
        elif not resolved.exists():
            issues.append(f"{display_path}:{line}: missing local target {target}")
    return sorted(issues)


def skipped_directory(name):
    return name in {".git", ".venv", "__pycache__", "node_modules", "build", "dist"} \
        or name.startswith(("build-", "cmake-build-"))


def markdown_files(root):
    """Discover source documentation deterministically, excluding build output."""
    root = Path(root).resolve()
    if not root.is_dir():
        raise ValueError(f"repository root is not a directory: {root}")
    paths = []
    for directory, children, files in os.walk(root, followlinks=False):
        children[:] = sorted(name for name in children if not skipped_directory(name))
        for name in sorted(files):
            if name.lower().endswith(".md"):
                paths.append(Path(directory) / name)
    return sorted(paths)


def check_repository(root, paths=None):
    root = Path(root).resolve()
    selected = markdown_files(root) if not paths else sorted({Path(path).resolve() for path in paths})
    issues = []
    for path in selected:
        issues.extend(check_document(path, root))
    return len(selected), sorted(issues)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", type=Path, help="Optional Markdown files to check")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1],
                        help="Repository boundary (defaults to this script's repository)")
    args = parser.parse_args(argv)
    try:
        count, issues = check_repository(args.root, args.paths)
    except (OSError, ValueError) as error:
        print(f"documentation check failed: {error}", file=sys.stderr)
        return 1
    for issue in issues:
        print(issue, file=sys.stderr)
    print(f"Checked {count} Markdown files; {len(issues)} local-link problems.")
    return 1 if issues else 0


if __name__ == "__main__":
    sys.exit(main())
