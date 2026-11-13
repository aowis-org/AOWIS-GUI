#!/usr/bin/env python3
"""Inventory missing Qt translation keys and suspicious hardcoded GUI strings.

This is an audit, not an automatic translator.  It intentionally doesn't flag
user values (UUIDs, URLs, model identifiers) as translation errors and only
asserts completeness for explicitly mapped Qt translation contexts. Classes
that inherit QObject::tr without Q_OBJECT or Q_DECLARE_TR_FUNCTIONS may actually
translate in the QObject context: do not map them by their C++ class names.
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
SIMPLE_TR = re.compile(rf'(?<![A-Za-z_:])tr\(\s*({LITERALS})')
UI_LITERAL = re.compile(rf'\b(?:setText|setToolTip|setWindowTitle|setPlaceholderText|setStatusTip|addAction)\(\s*(?:QStringLiteral\(\s*)?({LITERALS})')
KNOWN_DIRECT = {
    'src/app/footer_statusbar.cpp': 'FooterStatusBar',
    'src/app/top_control_bar.cpp': 'TopControlBar',
    'src/app/main_window.cpp': 'MainWindow',
    'src/simulation/simulation_diagnostics_widget.cpp': 'SimulationDiagnosticsWidget',
    'src/tabs/tab_map_monitor_container.cpp': 'MapMonitorContainer',
    'src/map/core/map_navigation_widget.cpp': 'MapNavigationWidget',
    'src/map/editor/map_canvas_pipes.cpp': 'MapCanvasPipes',
    'src/config/shortcut_registry.cpp': 'GuiShortcutRegistry',
    'src/settings/map_settings_widget.cpp': 'MapSettingsWidget',
    # These classes have verified Qt translation contexts (Q_OBJECT or
    # Q_DECLARE_TR_FUNCTIONS), including the inline Projects inspector classes
    # that intentionally use the ProjectsWidget translation context.
    'src/app/history_dock.cpp': 'HistoryDock',
    'src/projects/project_source_reader.cpp': 'ProjectSourceReader',
    'src/projects/project_source_writer.cpp': 'ProjectSourceWriter',
    'src/tabs/tab_projects_widget.cpp': 'ProjectsWidget',  # ProjectsWidget declares Q_OBJECT.
    'src/simulation/simulation_manager.cpp': 'SimulationManager',
    'src/settings/personal_repository_settings_widget.cpp': 'PersonalRepositorySettingsWidget',
    'src/entity_inspector/entity_inspector_tank.cpp': 'EntityInspectorTank',
    'src/simulation/simulation_statistics_dialog.cpp': 'SimulationStatisticsDialog',
    'src/tabs/personal_working_copy_dialog.cpp': 'PersonalWorkingCopyDialog',
    'src/tabs/project_revision_graph.cpp': 'ProjectRevisionGraph',
    'src/tabs/projects_inspector_creation.h': 'ProjectsWidget',
    'src/tabs/projects_inspector_details.h': 'ProjectsWidget',
    'src/tabs/projects_inspector_panels.h': 'ProjectsWidget',
    'src/tabs/tab_map_workspace.cpp': 'MapWorkspaceWidget',
    'src/tabs/tab_settings_widget.cpp': 'SettingsWidget',
    'src/settings/wireguard_vpn_settings_widget.cpp': 'WireGuardVpnSettingsWidget',
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
    implicit_not_checked = {}
    for path in sorted(SOURCES.rglob('*')):
        if path.suffix not in ('.cpp', '.h') or not path.is_file():
            continue
        rel = path.relative_to(ROOT).as_posix()
        source = path.read_text(encoding='utf-8', errors='replace')
        for pattern in EXPLICIT_PATTERNS:
            for match in pattern.finditer(source):
                key = (decode_cpp_literal(match.group(1)), decode_cpp_literal(match.group(2)))
                keys.setdefault(key, set()).add(rel)
        matches = list(SIMPLE_TR.finditer(source))
        if rel in KNOWN_DIRECT:
            for match in matches:
                key = (KNOWN_DIRECT[rel], decode_cpp_literal(match.group(1)))
                keys.setdefault(key, set()).add(rel)
        elif matches:
            # Unqualified QObject::tr() uses the declaring class's translation
            # context, not the filename. Never guess a context just to make a
            # completeness check pass. Keep these sources visible as backlog.
            implicit_not_checked[rel] = len(matches)
        for match in UI_LITERAL.finditer(source):
            literal = decode_cpp_literal(match.group(1))
            if literal.startswith(('http://', 'https://')):
                # Endpoint examples are technical input values, not prose.
                continue
            if len(literal) >= 4 and any(character.isalpha() for character in literal):
                hardcoded.append((rel, source.count('\n', 0, match.start()) + 1, literal[:85]))
    return keys, hardcoded, implicit_not_checked


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check-context', action='append', default=[],
                        help='Require every extracted key in this context to exist in all catalogues')
    parser.add_argument('--details', action='store_true', help='Show missing keys and source locations')
    parser.add_argument('--check-all', action='store_true',
                        help='Fail when any mapped translation key is missing from any catalogue')
    parser.add_argument('--require-all-contexts', action='store_true',
                        help='Fail if any ordinary tr() source calls still have an unmapped Qt context')
    args = parser.parse_args()
    catalogues = read_catalogues()
    keys, hardcoded, implicit_not_checked = audit_sources()
    print(f'Found {len(keys)} distinct explicit translation keys across source files.')
    print(f'Found {len(hardcoded)} suspicious literal widget captions/tooltips (review manually).')
    print(f'Unmapped ordinary tr() calls: {sum(implicit_not_checked.values())} across '
          f'{len(implicit_not_checked)} source files (not checked by --check-all).')
    for filename, count in sorted(implicit_not_checked.items()):
        print(f'  {count:3}  {filename}')
    failing = args.require_all_contexts and bool(implicit_not_checked)
    for lang, available in catalogues.items():
        missing = {key: paths for key, paths in keys.items() if key not in available}
        print(f'{lang}: {len(available)} catalogue keys, {len(missing)} missing explicit keys')
        missing_by_file = Counter(next(iter(paths)) for paths in missing.values())
        for filename, count in missing_by_file.most_common(5):
            print(f'  {count:3}  {filename}')
        if args.details:
            for (context, text), paths in sorted(missing.items()):
                print(f'  MISSING [{context}] {text!r} @ {", ".join(sorted(paths))}')
        scoped = [(context, text) for context, text in missing
                  if args.check_all or context in args.check_context]
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
