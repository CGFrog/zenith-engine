#!/usr/bin/env python3
"""Checks that the tests follow docs/TESTING.md.

Run from anywhere: python tools/tests/check_tests.py. Exits nonzero, listing each
problem, when a test file under tests/:

- hand-rolls assertions (a local `expect(` helper or an `int failures` counter)
  instead of doctest's CHECK/REQUIRE;
- has its own `int main(` without being a standalone executable (a smoke,
  benchmark, capture, or a file listed in STANDALONE below);
- builds a temporary path from `temp_directory_path()` instead of owning a
  testsupport::TempDirectory;
- declares doctest cases without TEST_SUITE_BEGIN("<Suite>"), names a case
  that does not start with "<Suite>: ", uses a comma or wildcard (* ? [ ])
  in a case name (CTest selects cases by name), or repeats a case name of its
  area;
- is not named by any CMakeLists.txt under tests/.

Files in tools/tests/legacy_tests.txt predate the scheme and are exempt from
the first three rules until they are converted; converting one means removing
it from that list, which this check insists on.
"""
import re
import sys
from collections import defaultdict
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]
TESTS = REPOSITORY / 'tests'
LEGACY_LIST = Path(__file__).resolve().parent / 'legacy_tests.txt'

# Standalone executables with their own main(): they need their own process,
# command-line arguments, or a frame loop (docs/TESTING.md, "Standalone tests").
STANDALONE_DIRECTORIES = ('capture', 'validation')
STANDALONE_NAME_PARTS = ('Smoke', 'Benchmark')
STANDALONE = set()

HAND_ROLLED = re.compile(r'^\s*(?:static\s+)?(?:void\s+expect\w*\s*\(|int\s+failures\s*=)', re.M)
MAIN = re.compile(r'^\s*int\s+main\s*\(', re.M)
TEMP_PATH = re.compile(r'\btemp_directory_path\s*\(')
SUITE = re.compile(r'TEST_SUITE_BEGIN\s*\(\s*"([^"]+)"\s*\)')
CASE = re.compile(r'TEST_CASE(?:_FIXTURE)?\s*\((?:\s*\w+\s*,)?\s*"((?:[^"\\]|\\.)*)"')


def relative(path):
    return path.relative_to(REPOSITORY).as_posix()


def is_standalone(path):
    rel = path.relative_to(TESTS)
    if rel.parts[0] in STANDALONE_DIRECTORIES or 'modules' in rel.parts[:-1]:
        return True
    return any(part in path.name for part in STANDALONE_NAME_PARTS) or relative(path) in STANDALONE


def read_legacy():
    if not LEGACY_LIST.exists():
        return set()
    entries = set()
    for line in LEGACY_LIST.read_text(encoding='utf-8').splitlines():
        line = line.split('#', 1)[0].strip()
        if line:
            entries.add(line)
    return entries


def cmake_names():
    names = set()
    for cmake in TESTS.rglob('CMakeLists.txt'):
        names.update(re.findall(r'[\w./-]+\.(?:cpp|hpp)', cmake.read_text(encoding='utf-8')))
    return {Path(name).name for name in names}


def main():
    problems = []
    legacy = read_legacy()
    listed = cmake_names()
    sources = sorted(p for p in TESTS.rglob('*') if p.suffix in ('.cpp', '.hpp') and 'support' not in
                     p.relative_to(TESTS).parts[:1])
    cases_by_area = defaultdict(dict)

    for entry in sorted(legacy):
        if not (REPOSITORY / entry).is_file():
            problems.append(f'{relative(LEGACY_LIST)}: {entry} does not exist; remove it from the list')

    for path in sources:
        rel = relative(path)
        text = path.read_text(encoding='utf-8')
        is_legacy = rel in legacy
        breaches = []
        if HAND_ROLLED.search(text):
            breaches.append('hand-rolled expect()/failures; use doctest CHECK/REQUIRE '
                            '(or testsupport::SmokeCheck in a standalone executable)')
        if MAIN.search(text) and not is_standalone(path):
            breaches.append('has its own main(); unit tests are doctest cases in their area suite')
        if TEMP_PATH.search(text):
            breaches.append('builds a path from temp_directory_path(); own a testsupport::TempDirectory')
        if is_legacy and not breaches:
            problems.append(f'{rel}: follows docs/TESTING.md now; remove it from {relative(LEGACY_LIST)}')
        if not is_legacy:
            problems.extend(f'{rel}: {breach}' for breach in breaches)

        if path.suffix == '.cpp' and path.name not in listed and 'references' not in path.parts:
            problems.append(f'{rel}: not named by any tests/**/CMakeLists.txt')

        cases = CASE.findall(text)
        if not cases:
            continue
        suites = SUITE.findall(text)
        if len(suites) != 1:
            problems.append(f'{rel}: doctest cases need exactly one TEST_SUITE_BEGIN("<Suite>"), found {len(suites)}')
            continue
        suite = suites[0]
        area = path.relative_to(TESTS).parts[0]
        for name in cases:
            if not name.startswith(suite + ': '):
                problems.append(f'{rel}: case "{name}" must start with "{suite}: "')
            if re.search(r'[,*?\[\]]', name):
                problems.append(f'{rel}: case "{name}" has a comma or wildcard character')
            if name in cases_by_area[area]:
                problems.append(f'{rel}: case "{name}" repeats one in {cases_by_area[area][name]}')
            cases_by_area[area][name] = rel

    for problem in problems:
        print(problem)
    converted = sum(len(cases) for cases in cases_by_area.values())
    print(f'{len(problems)} problem(s); {converted} doctest case(s); {len(legacy)} legacy file(s) still to convert')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
