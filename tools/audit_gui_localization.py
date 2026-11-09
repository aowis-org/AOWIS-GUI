#!/usr/bin/env python3
"""Inventory missing Qt translation keys and suspicious hardcoded GUI strings.

This is an audit, not an automatic translator.  It intentionally doesn't flag
user values (UUIDs, URLs, model identifiers) as translation errors and only
asserts completeness for explicitly selected migration contexts.
"""

import argparse
import ast
from collections import Counter
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / "src"
TRANSLATIONS = ROOT / "translations"
LITERAL = r'"(?:[^"\\]|\\.)*"'
LITERALS = rf'(?:{LITERAL}\s*)+'
EXPLICIT_PATTERNS = [
    re.compile(rf'QT_TRANSLATE_NOOP(?:3)?\(\s*({LITERAL})\s*,\s*({LITERALS})'),
    re.compile(rf'QCoreApplication::translate\(\s*({LITERAL})\s*,\s*({LITERALS})'),
]
SIMPLE_TR = re.compile(rf'(?<![A-Za-z_])tr\(\s*({LITERALS})')
UI_LITERAL = re.compile(rf'\b(?:setText|setToolTip|setWindowTitle|setPlaceholderText|setStatusTip)\(\s*(?:QStringLiteral\(\s*)?({LITERALS})')
KNOWN_DIRECT = {
    'src/app/footer_statusbar.cpp': 'FooterStatusBar',
    'src/map/core/map_navigation_widget.cpp': 'MapNavigationWidget',
    'src/settings/map_settings_widget.cpp': 'MapSettingsWidget',
}


def decode_cpp_literal(expression):
    return ''.join(ast.literal_eval(match.group())
                   for match in re.finditer(LITERAL, expression))


def read_catalogues():
    available = {}
    for language in ('fr', 'ar', 'de', 'es'):
        path = TRANSLATIONS / f'aowis_gui_{language}.ts'
        root = ET.parse(path).getroot()
        available[language] = {
            (context.findtext('name'), message.findtext('source'))
            for context in root.findall('context')
            for message in context.findall('message')
            if message.findtext('source') is not None
        }
    return available


def audit_sources():
    keys = {}
    hardcoded = []
    for path in sorted(SOURCES.rglob('*')):
        if path.suffix not in ('.cpp', '.h') or not path.is_file():
            continue
        rel = path.relative_to(ROOT).as_posix()
        source = path.read_text(encoding='utf-8', errors='replace')
        for pattern in EXPLICIT_PATTERNS:
            for match in pattern.finditer(source):
                key = (decode_cpp_literal(match.group(1)), decode_cpp_literal(match.group(2)))
                keys.setdefault(key, set()).add(rel)
        if rel in KNOWN_DIRECT:
            for match in SIMPLE_TR.finditer(source):
                key = (KNOWN_DIRECT[rel], decode_cpp_literal(match.group(1)))
                keys.setdefault(key, set()).add(rel)
        for match in UI_LITERAL.finditer(source):
            literal = decode_cpp_literal(match.group(1))
            if len(literal) >= 4 and any(character.isalpha() for character in literal):
                hardcoded.append((rel, source.count('\n', 0, match.start()) + 1, literal[:85]))
    return keys, hardcoded


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check-context', action='append', default=[],
                        help='Require every extracted key in this context to exist in all catalogues')
    parser.add_argument('--details', action='store_true', help='Show missing keys and source locations')
    args = parser.parse_args()
    catalogues = read_catalogues()
    keys, hardcoded = audit_sources()
    print(f'Found {len(keys)} distinct explicit translation keys across source files.')
    print(f'Found {len(hardcoded)} suspicious literal widget captions/tooltips (review manually).')
    failing = False
    for lang, available in catalogues.items():
        missing = {key: paths for key, paths in keys.items() if key not in available}
        print(f'{lang}: {len(available)} catalogue keys, {len(missing)} missing explicit keys')
        missing_by_file = Counter(next(iter(paths)) for paths in missing.values())
        for filename, count in missing_by_file.most_common(5):
            print(f'  {count:3}  {filename}')
        if args.details:
            for (context, text), paths in sorted(missing.items()):
                print(f'  MISSING [{context}] {text!r} @ {", ".join(sorted(paths))}')
        scoped = [(context, text) for context, text in missing if context in args.check_context]
        if scoped:
            failing = True
            for context, text in sorted(scoped):
                print(f'  ERROR [{context}] {text!r}')
    if args.details:
        print('Suspicious hardcoded UI literals:')
        for filename, line, text in hardcoded:
            print(f'  {filename}:{line}  {text!r}')
    return 1 if failing else 0


if __name__ == '__main__':
    sys.exit(main())
