#!/usr/bin/env python3
"""Install and start the user service for this checkout, or preview with --print."""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def quote(value, *, argument=False):
    """Quote an ExecStart token; environment expansion applies only to arguments."""
    value = str(value).replace('\\', '\\\\').replace('"', '\\"')
    value = value.replace('\n', '\\n').replace('\r', '\\r').replace('%', '%%')
    if argument:
        value = value.replace('$', '$$')
    return '"'+value+'"'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--print', dest='print_only', action='store_true',
                        help='Print the rendered unit without installing or starting it')
    args = parser.parse_args()
    if '\n' in str(ROOT) or '\r' in str(ROOT):
        parser.error('The checkout path must not contain a newline')
    # Keep the virtualenv path: resolving its Python symlink loses its packages.
    python = ROOT/'.venv/bin/python'
    if not python.is_file():
        parser.error('Create .venv and install requirements.txt first')
    unit = (ROOT/'systemd/elekstube-monitor.service').read_text()
    for marker, value in {
        '@WORKING_DIRECTORY@': str(ROOT).replace('%', '%%'),
        '@PYTHON@': quote(python),
        '@MONITOR@': quote(ROOT/'monitor.py', argument=True),
    }.items():
        unit = unit.replace(marker, value)
    if args.print_only:
        print(unit, end='')
        return
    config_dir = Path(os.environ.get('XDG_CONFIG_HOME', Path.home()/'.config'))
    destination = config_dir/'systemd/user/elekstube-monitor.service'
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(unit)
    subprocess.run(['systemctl', '--user', 'daemon-reload'], check=True)
    subprocess.run(['systemctl', '--user', 'enable', '--now', destination.name], check=True)
    print(f'Installed {destination}')


if __name__ == '__main__':
    main()
