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
- defines its own copy of a file helper that tests/support provides
  (readText, writeText, readBytes, writeBytes, or a readFile, writeFile, or
  freshDirectory variant);
- declares doctest cases without TEST_SUITE_BEGIN("<Suite>"), names a case
  that does not start with "<Suite>: ", uses a comma or wildcard (* ? [ ])
  in a case name (CTest selects cases by name), or repeats a case name of its
  area;
- is not named by any CMakeLists.txt under tests/.
"""
import re
import sys
from collections import defaultdict
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]
TESTS = REPOSITORY / 'tests'

# Standalone executables with their own main(): they need their own process,
# command-line arguments, or a frame loop (docs/TESTING.md, "Standalone tests").
STANDALONE_DIRECTORIES = ('capture', 'validation')
STANDALONE_NAME_PARTS = ('Smoke', 'Benchmark')
STANDALONE = {
    # Runs as two processes, one per file-watcher backend (native, polling).
    'tests/engine/FileWatcherTests.cpp',
    # Needs a window; skips (77) without one.
    'tests/engine/InputManagerTests.cpp',
    # Argument-driven: builds (and times) a generated project's game module.
    'tests/editor/GeneratedModuleRuns.cpp',
    # A tool that writes the FPS template's committed scene and prefab.
    'tests/editor/FpsTemplateWriter.cpp',
}

HAND_ROLLED = re.compile(r'^\s*(?:static\s+)?(?:void\s+expect\w*\s*\(|int\s+failures\s*=)', re.M)
MAIN = re.compile(r'^\s*int\s+main\s*\(', re.M)
TEMP_PATH = re.compile(r'\btemp_directory_path\s*\(')
# A function definition (a return type, the name, then a parameter list) of a
# helper tests/support/Files.hpp or TempDirectory already provides.
HELPER_COPY = re.compile(
    r'^[ \t]*(?:(?:static|inline)[ \t]+)*[\w:<>]+(?:[ \t]*[&*])?[ \t]+'
    r'(readText|writeText|readBytes|writeBytes|readFile|writeFile|freshDirectory)[ \t]*\([^;{]*\)[ \t]*\{', re.M)
SUITE = re.compile(r'TEST_SUITE_BEGIN\s*\(\s*"([^"]+)"\s*\)')
CASE = re.compile(r'TEST_CASE(?:_FIXTURE)?\s*\((?:\s*\w+\s*,)?\s*"((?:[^"\\]|\\.)*)"')


def relative(path):
    return path.relative_to(REPOSITORY).as_posix()


def is_standalone(path):
    rel = path.relative_to(TESTS)
    if rel.parts[0] in STANDALONE_DIRECTORIES or 'modules' in rel.parts[:-1]:
        return True
    return any(part in path.name for part in STANDALONE_NAME_PARTS) or relative(path) in STANDALONE


def cmake_names():
    names = set()
    for cmake in TESTS.rglob('CMakeLists.txt'):
        names.update(re.findall(r'[\w./-]+\.(?:cpp|hpp)', cmake.read_text(encoding='utf-8')))
    return {Path(name).name for name in names}


def main():
    problems = []
    listed = cmake_names()
    sources = sorted(p for p in TESTS.rglob('*') if p.suffix in ('.cpp', '.hpp') and 'support' not in
                     p.relative_to(TESTS).parts[:1])
    cases_by_area = defaultdict(dict)

    for path in sources:
        rel = relative(path)
        text = path.read_text(encoding='utf-8')
        breaches = []
        if HAND_ROLLED.search(text):
            breaches.append('hand-rolled expect()/failures; use doctest CHECK/REQUIRE '
                            '(or testsupport::StandaloneChecks in a standalone executable)')
        if MAIN.search(text) and not is_standalone(path):
            breaches.append('has its own main(); unit tests are doctest cases in their area suite')
        if TEMP_PATH.search(text):
            breaches.append('builds a path from temp_directory_path(); own a testsupport::TempDirectory')
        for helper in sorted(set(HELPER_COPY.findall(text))):
            breaches.append(f'defines its own {helper}(); use support/Files.hpp (readText, writeText, readBytes, '
                            'writeBytes) or a testsupport::TempDirectory')
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
    print(f'{len(problems)} problem(s); {converted} doctest case(s)')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
