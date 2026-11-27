#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check repository-local Markdown links, heading anchors and the driver index."""

import html
import os
import posixpath
import re
import string
import subprocess
import sys
import unicodedata
from pathlib import Path, PurePosixPath
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[2]
DRIVER_INDEX = 'docs/drivers/README.md'
# Vendored upstream README whose relative links point into its original repository.
VENDORED_SKIP = {'package/PianoA830/adreno830/README.md'}
FENCE = re.compile(r' {0,3}(`{3,}|~{3,})(.*)')
QUOTE = re.compile(r' {0,3}>[ \t]?')
ITEM = re.compile(r' {0,3}(?:[-+*]|[0-9]{1,9}[.)]) {1,4}')
ATX = re.compile(r' {0,3}#{1,6}(?:[ \t]+(.*)|$)')
SETEXT = re.compile(r' {0,3}(?:=+|-+)[ \t]*$')
THEMATIC = re.compile(r' {0,3}(?:(?:\*[ \t]*){3,}|(?:-[ \t]*){3,}|(?:_[ \t]*){3,})$')
ESCAPE = r'\\([' + re.escape(string.punctuation) + r'])'
CODE = re.compile(ESCAPE + r'|<!--.*?-->|(?<!`)(`+)(?!`)(.*?)(?<!`)\2(?!`)', re.S)
HTML_TAG = re.compile(r'</?[A-Za-z][A-Za-z0-9-]*(?:\s+[^<>]*?)?\s*/?>')
LABEL = r'\[((?:\\.|[^\[\]\\])*)\]'
REFERENCE = re.compile(LABEL)
DEFINITION = re.compile(r' {0,3}' + LABEL + r':[ \t]*(?:\n[ \t]*)?')
BRACKET = re.compile(ESCAPE + r'|!?\[|\]')
TITLE = r'(?:"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|\((?:\\.|[^()\\])*\))'
INLINE_END = re.compile(r'(?:\s+' + TITLE + r')?\s*\)')
DEFINITION_END = re.compile(r'(?:(?:[ \t]*\n[ \t]*|[ \t]+)' + TITLE + r')?[ \t]*(?:\n|$)')
EXTERNAL = re.compile(r'[A-Za-z][A-Za-z0-9+.-]*:|//')
UNDERSCORE = re.compile(r'(?<!\w)(_{1,3})(?=\S)(.+?)(?<=\S)\1(?!\w)', re.S)


def repository_files(root):
    # The full index supplies non-Markdown targets and their parent directories.
    try:
        result = subprocess.run(
            ['git', 'ls-files', '-z'], cwd=root, capture_output=True, check=True,
        )
        return set(os.fsdecode(result.stdout).split('\0')) - {''}
    except (OSError, subprocess.CalledProcessError):
        pass

    # Source archives have no Git index. Skip checkout metadata and local outputs.
    excluded = {'.git', '_gitdir', '.agents', '.aws', '.claude', '.codex',
                '__pycache__', 'build', 'out', 'private'}
    files = set()
    for directory, dirs, names in os.walk(root):
        dirs[:] = [name for name in dirs if name not in excluded]
        parent = Path(directory).relative_to(root)
        files.update((parent / name).as_posix() for name in names if name not in excluded)
    return files


def markdown_blocks(source):
    """Yield line-numbered paragraphs and ATX/Setext headings outside code."""
    paragraph = []
    first_line = 0
    quote_depth = 0
    indents = []
    fence = None
    comment = False
    for number, line in enumerate(source.split('\n'), 1):
        depth = 0
        while fence is None or depth < quote_depth:
            quote = QUOTE.match(line)
            if not quote:
                break
            depth += 1
            line = line[quote.end():]
        if depth != quote_depth:
            if paragraph:
                yield first_line, False, '\n'.join(paragraph)
                paragraph.clear()
            indents.clear()
            fence = None
            comment = False
            quote_depth = depth

        tail = line.lstrip(' \t')
        indent = len(line[:len(line) - len(tail)].expandtabs(4))
        if tail:
            if indents and indent < indents[-1]:
                fence = None
            while indents and indent < indents[-1]:
                indents.pop()
        removed = indents[-1] if indents else 0
        line = ' ' * (indent - removed) + tail
        marker = FENCE.fullmatch(line)
        if fence is not None:
            if marker and marker[1].startswith(fence) and not marker[2].strip():
                fence = None
            continue

        item = None if THEMATIC.fullmatch(line) else ITEM.match(line)
        if item:
            while item:
                removed += item.end()
                indents.append(removed)
                line = line[item.end():]
                item = ITEM.match(line)
            if paragraph:
                yield first_line, False, '\n'.join(paragraph)
                paragraph.clear()
            marker = FENCE.fullmatch(line)

        heading = ATX.fullmatch(line)
        opens_fence = marker and (marker[1][0] == '~' or '`' not in marker[2])
        opens_comment = line.startswith('<!--') and '-->' not in line
        if SETEXT.fullmatch(line) and paragraph:
            yield first_line, True, '\n'.join(paragraph)
            paragraph.clear()
        elif (not line.strip() or opens_fence or heading or THEMATIC.fullmatch(line)
              or opens_comment or comment):
            if paragraph:
                yield first_line, False, '\n'.join(paragraph)
                paragraph.clear()
            if opens_fence and not comment:
                fence = marker[1]
            elif opens_comment or comment:
                comment = '-->' not in line
            elif heading:
                title = re.sub(r'(?:^|[ \t])#+[ \t]*$', '', heading[1] or '')
                yield number, True, title
        elif paragraph or not line.startswith('    '):
            if not paragraph:
                first_line = number
            paragraph.append(line)
    if paragraph:
        yield first_line, False, '\n'.join(paragraph)


def destination(text, start):
    """Read an angle-bracketed or balanced bare destination."""
    if text.startswith('<', start):
        match = re.match(r'<((?:\\.|[^<>\\\n])*)>', text[start:])
        return (match[1], start + match.end()) if match else None

    depth = 0
    end = start
    while end < len(text):
        char = text[end]
        if char == '\\' and end + 1 < len(text) and text[end + 1] in string.punctuation:
            end += 2
            continue
        if char.isspace() or (char == ')' and depth == 0):
            break
        if char == '(':
            depth += 1
        elif char == ')':
            depth -= 1
        end += 1
    return (text[start:end], end) if depth == 0 else None


def reference_key(label):
    return ' '.join(label.split()).casefold()


def inline_links(text, references):
    """Yield the span, label and destination of inline and reference links."""
    masked = CODE.sub(lambda m: m[0] if m[1] else re.sub(r'[^\n]', ' ', m[0]), text)
    stack = []
    consumed = 0
    for token in BRACKET.finditer(masked):
        start, end = token.span()
        if start < consumed or token[1]:
            continue
        if token[0] != ']':
            stack.append((start, end))
            continue
        if not stack:
            continue
        opening, label_start = stack.pop()
        label = text[label_start:start]
        if text.startswith('(', end):
            dest_start = end + 1
            while dest_start < len(text) and text[dest_start].isspace():
                dest_start += 1
            parsed = destination(text, dest_start)
            closing = INLINE_END.match(text, parsed[1]) if parsed else None
            if closing:
                consumed = closing.end()
                yield opening, consumed, label_start, start, parsed[0]
                continue
        reference = REFERENCE.match(text, end)
        key = reference_key((reference[1] if reference else '') or label)
        if key in references:
            consumed = reference.end() if reference else end
            yield opening, consumed, label_start, start, references[key]
        elif reference:
            consumed = reference.end()


def heading_slug(text, links):
    # Strip the syntax around link labels, including images nested in links.
    syntax = sorted(
        span for start, end, label_start, label_end, _ in links
        for span in ((start, label_start), (label_end, end))
    )
    pieces = []
    cursor = 0
    for start, end in syntax:
        pieces.append(text[cursor:start])
        cursor = max(cursor, end)
    pieces.append(text[cursor:])
    text = ''.join(pieces)

    literals = []

    def protect(match):
        if match[1]:
            literal = match[1]
        elif match[2]:
            code = match[3].replace('\n', ' ')
            literal = code[1:-1] if code.startswith(' ') and code.endswith(' ') and code.strip(' ') else code
        else:
            return ''
        literals.append(literal)
        return f'\0{len(literals) - 1}\0'

    # GitHub slugs rendered text. Keep Unicode letters and marks, including CJK.
    text = CODE.sub(protect, text)
    text = UNDERSCORE.sub(r'\2', HTML_TAG.sub('', text))
    text = re.sub(r'\0(\d+)\0', lambda m: literals[int(m[1])], html.unescape(text)).strip().lower()
    return ''.join(
        '-' if char == ' ' else char
        for char in text
        if char in ' -' or unicodedata.category(char) in
        {'Lu', 'Ll', 'Lt', 'Lm', 'Lo', 'Mn', 'Mc', 'Me', 'Nd', 'Nl', 'Pc'}
    )


def scan_markdown(source):
    blocks = []
    references = {}
    for number, heading, text in markdown_blocks(source):
        start = 0
        while not heading:
            definition = DEFINITION.match(text, start)
            if not definition or not definition[1].strip() or definition[1].startswith('^'):
                break
            parsed = destination(text, definition.end())
            closing = DEFINITION_END.match(text, parsed[1]) if parsed and parsed[1] > definition.end() else None
            if not closing:
                break
            references.setdefault(reference_key(definition[1]), parsed[0])
            start = closing.end()
        blocks.append((number + text.count('\n', 0, start), heading, text[start:]))

    anchors = {}
    destinations = []
    for number, heading, text in blocks:
        links = list(inline_links(text, references))
        if heading:
            base = slug = heading_slug(text, links)
            while slug in anchors:
                anchors[base] += 1
                slug = f'{base}-{anchors[base]}'
            anchors[slug] = 0
        for start, _, _, _, target in links:
            destinations.append((number + text.count('\n', 0, start), target, text[start] == '!'))
    return anchors, destinations


def resolve_link(source, destination_text):
    url = html.unescape(re.sub(ESCAPE, r'\1', destination_text))
    if EXTERNAL.match(url):
        return None
    path, _, fragment = url.partition('#')
    path = unquote(path.partition('?')[0])
    if path.startswith('/'):
        target = posixpath.normpath(path.lstrip('/'))
    elif path:
        target = posixpath.normpath(posixpath.join(posixpath.dirname(source), path))
    else:
        target = source
    return target, unquote(fragment), path.endswith(('/', '/.', '/..'))


def main():
    files = repository_files(ROOT)
    directories = {str(parent) for file in files for parent in PurePosixPath(file).parents}
    documents = {}
    errors = []
    for path in sorted(file for file in files if file.endswith('.md')):
        source = ROOT / path
        try:
            if source.is_symlink():
                errors.append(f'{path}: Markdown source is a symbolic link')
                continue
            documents[path] = scan_markdown(source.read_text(encoding='utf-8-sig'))
        except (OSError, UnicodeError):
            errors.append(f'{path}: cannot read Markdown source as UTF-8')

    indexed = set()
    link_count = anchor_count = 0
    for path, (_, links) in documents.items():
        if path in VENDORED_SKIP:
            continue
        for number, destination_text, image in links:
            resolved = resolve_link(path, destination_text)
            if resolved is None:
                continue
            target, fragment, directory_only = resolved
            link_count += 1
            check_anchor = bool(fragment) and target.endswith('.md') and not directory_only
            anchor_count += check_anchor
            location = f'{path}:{number}'
            if target not in directories and (target not in files or directory_only):
                errors.append(f'{location}: relative link target is missing from the repository')
            elif check_anchor and (target not in documents or fragment not in documents[target][0]):
                errors.append(f'{location}: heading anchor is missing in {target}')
            if path == DRIVER_INDEX and not image:
                indexed.add(target)

    driver_pages = {
        path for path in files if path.endswith('.md') and posixpath.dirname(path) == 'docs/drivers'
    } - {DRIVER_INDEX}
    if DRIVER_INDEX not in documents:
        errors.append(f'{DRIVER_INDEX}: cannot check driver index')
    for path in sorted(driver_pages - indexed):
        errors.append(f'{DRIVER_INDEX}: missing entry for {posixpath.basename(path)}')

    for error in errors:
        print(error, file=sys.stderr)
    print(f'Checked {len(documents)} Markdown files, {link_count} relative links, '
          f'{anchor_count} heading anchor references, and {len(indexed & driver_pages)}/{len(driver_pages)} '
          f'driver index entries. {len(errors)} error(s).')
    return int(bool(errors))


if __name__ == '__main__':
    sys.exit(main())
