# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Read the repository TOML manifests on hosts whose Python has no tomllib."""

import json
import re
from pathlib import Path

# The three manifests use single-line TOML statements with quoted or bare keys,
# strings, decimal integers, booleans, arrays, inline tables and table headers.
TOML_TOKEN = re.compile(
    r'''[ \t]*("(?:[^"\\\x00-\x08\x0a-\x1f\x7f]|\\(?:["\\btnfr]|u[0-9a-fA-F]{4}))*"'''
    r"""|'[^'\x00-\x08\x0a-\x1f\x7f]*'|[A-Za-z0-9_+-]+|[\[\]{},.=])"""
)
BARE_KEY = re.compile(r'[A-Za-z0-9_-]+')
INTEGER = re.compile(r'[+-]?(?:0|[1-9](?:_?[0-9])*)')


class TomlLine:
    def __init__(self, line):
        self.tokens = []
        self.index = 0
        while True:
            tail = line.lstrip(' \t')
            if not tail or tail.startswith('#'):
                break
            match = TOML_TOKEN.match(line)
            if not match:
                raise ValueError('invalid TOML token')
            self.tokens.append(match[0])
            line = line[match.end():]
        self.tokens.append('')

    def peek(self):
        return self.tokens[self.index].lstrip(' \t')

    def take(self, token, *, adjacent=False):
        current = self.tokens[self.index] if adjacent else self.peek()
        if current != token:
            return False
        self.index += 1
        return True

    def expect(self, token, *, adjacent=False):
        if not self.take(token, adjacent=adjacent):
            raise ValueError(f'expected {token!r}')

    def key(self):
        token = self.peek()
        if not (token.startswith(('"', "'")) or BARE_KEY.fullmatch(token)):
            raise ValueError('expected a key')
        self.index += 1
        return toml_string(token) if token.startswith(('"', "'")) else token

    def value(self):
        if self.take('['):
            items = []
            while not self.take(']'):
                items.append(self.value())
                if not self.take(','):
                    self.expect(']')
                    break
            return items
        if self.take('{'):
            entries = {}
            if not self.take('}'):
                self.assignment(entries)
                while self.take(','):
                    self.assignment(entries)
                self.expect('}')
            return entries
        token = self.peek()
        if token.startswith(('"', "'")):
            value = toml_string(token)
        elif token in ('true', 'false'):
            value = token == 'true'
        elif INTEGER.fullmatch(token):
            value = int(token.replace('_', ''))
            if not -(2 ** 63) <= value < 2 ** 63:
                raise ValueError('TOML integer out of range')
        else:
            raise ValueError('expected a TOML value')
        self.index += 1
        return value

    def assignment(self, table):
        key = self.key()
        self.expect('=')
        if key in table:
            raise ValueError('duplicate key')
        table[key] = self.value()


def toml_string(token):
    if token.startswith("'"):
        return token[1:-1]
    value = json.loads(token.replace('\t', r'\t'))
    value.encode('utf-8')  # Reject lone Unicode surrogate escapes.
    return value


def parse_manifest(contents, path):
    document = current = {}
    tables = {(): document}
    defined = set()
    arrays = {}
    for lineno, line in enumerate(contents.split('\n'), 1):
        try:
            tokens = TomlLine(line)
            if not tokens.peek():
                continue
            if tokens.take('['):
                is_array = tokens.take('[', adjacent=True)
                parts = [tokens.key()]
                while tokens.take('.'):
                    parts.append(tokens.key())
                tokens.expect(']')
                if is_array:
                    tokens.expect(']', adjacent=True)
                parent = document
                for depth, key in enumerate(parts, 1):
                    prefix = tuple(parts[:depth])
                    if is_array and depth == len(parts):
                        if prefix not in arrays:
                            if key in parent:
                                raise ValueError('table conflicts with an existing key')
                            arrays[prefix] = parent[key] = []
                        current = {}
                        arrays[prefix].append(current)
                        break
                    if prefix not in tables:
                        if key in parent:
                            raise ValueError('table conflicts with an existing key')
                        tables[prefix] = parent[key] = {}
                    parent = tables[prefix]
                else:
                    if prefix in defined:
                        raise ValueError('duplicate table')
                    defined.add(prefix)
                    current = parent
            else:
                tokens.assignment(current)
            if tokens.peek():
                raise ValueError('unexpected content after statement')
        except ValueError as exc:
            raise ValueError(f'{path}:{lineno}: {exc}') from None
    return document


def load_manifest(path):
    """Use full TOML when available, with support for repository syntax on Python 3.9."""
    contents = Path(path).read_text(encoding="utf-8")
    try:
        import tomllib
    except ModuleNotFoundError:
        return parse_manifest(contents, path)
    return tomllib.loads(contents)
