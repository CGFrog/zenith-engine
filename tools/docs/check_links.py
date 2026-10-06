"""Checks the relative links in the repository's Markdown files.

Every link to a repository file must name an existing file, and every
`#anchor` into a Markdown file must name one of its headings (GitHub's slug
rules: lower case, punctuation dropped, spaces as hyphens). External links,
code blocks, and inline code are ignored.

Run from anywhere:  python tools/docs/check_links.py
Exits 1 and lists each broken link when any is found.
"""

import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
FENCE = re.compile(r"^\s*```")
LINK = re.compile(r"\]\(([^)\s]+)\)")
INLINE_CODE = re.compile(r"`[^`]*`")
HEADING = re.compile(r"^(#+)\s+(.*)$")


def markdown_files():
    output = subprocess.run(["git", "ls-files", "*.md"], cwd=ROOT, capture_output=True, text=True, check=True).stdout
    return [ROOT / name for name in output.split()]


def prose_lines(path):
    """The file's lines outside fenced code blocks."""
    in_code = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if FENCE.match(line):
            in_code = not in_code
            continue
        if not in_code:
            yield line


def slug(heading):
    text = heading.strip().lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def anchors(path, cache={}):
    if path not in cache:
        found = set()
        counts = {}
        for line in prose_lines(path):
            match = HEADING.match(line)
            if match:
                base = slug(match.group(2))
                # GitHub numbers repeated headings: name, name-1, name-2, ...
                count = counts.get(base, 0)
                found.add(base if count == 0 else f"{base}-{count}")
                counts[base] = count + 1
        cache[path] = found
    return cache[path]


def broken_links(path):
    for line in prose_lines(path):
        for match in LINK.finditer(INLINE_CODE.sub("", line)):
            link = match.group(1)
            if re.match(r"^[a-z][a-z0-9+.-]*:", link):
                continue
            target, _, anchor = link.partition("#")
            resolved = (path.parent / target).resolve() if target else path
            if not resolved.exists():
                yield f"missing file: {link}"
            elif anchor and resolved.suffix == ".md" and anchor not in anchors(resolved):
                yield f"missing heading: {link}"


def main():
    failures = 0
    for path in markdown_files():
        for problem in broken_links(path):
            print(f"{path.relative_to(ROOT).as_posix()}: {problem}")
            failures += 1
    if failures:
        print(f"{failures} broken link(s)")
        return 1
    print("all Markdown links resolve")
    return 0


if __name__ == "__main__":
    sys.exit(main())
