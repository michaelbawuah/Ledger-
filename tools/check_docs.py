#!/usr/bin/env python3
"""Check local Markdown links and images without fetching external URLs."""
from pathlib import Path
import re
from urllib.parse import unquote, urlsplit


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


def check_document(path, root):
    """Return local-link problems for one Markdown document."""
    path, root = Path(path).resolve(), Path(root).resolve()
    issues = []
    text = path.read_text(encoding="utf-8")
    for line, target in inline_links(text):
        parts = urlsplit(target)
        if parts.scheme or parts.netloc:
            continue
        if not parts.path:
            continue
        resolved = (path.parent / unquote(parts.path)).resolve()
        if not resolved.exists():
            issues.append(f"{path.relative_to(root)}:{line}: missing local target {target}")
    return issues
