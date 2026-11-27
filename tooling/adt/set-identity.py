#!/usr/bin/env python3
"""Give a built piano ADT another machine identity (docs/identity.md): the model values of configs/k27-identity/NAME.toml on the
root node and /product, everything else byte for byte the input's.

  set-identity.py IN.adt OUT.adt --identity NAME     NAME: vma2 (the input unchanged: configs/k27-adt.toml's VirtualMac2,1) or a
                                                     configs/k27-identity/NAME.toml
  set-identity.py ADT --show                         the identity an ADT carries (model, target-type, compatible, ...)

The overlay's ["root"."string"] and ["product"."string"] values replace (or add) those properties; "compatible" under root is put first
in the root's compatible list (hw.target is its first entry), the DTS's entries stay after it. The output's root also gets
unixnu-identity = NAME, the mark tooling/persist/build-esp.py refuses unless PIANO_IDENTITY_INSTALL=1 (an identity reaches the
installed system only after RAM boots have passed docs/identity.md section 6). An input that already has the mark is refused (give
the ADT made without one). The input is parsed and written back first and must come out byte for byte (else nothing is written).
"""
import argparse
import importlib.util
import sys
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:  # Python 3.9 and 3.10 use the tomli backport.
    try:
        import tomli as tomllib
    except ModuleNotFoundError:
        from pip._vendor import tomli as tomllib

DEVICE = Path(__file__).resolve().parents[2]
IDENTITIES = DEVICE / 'configs/k27-identity'
MARK = 'unixnu-identity'
SHOW = (('', ('model', 'target-type', 'target-sub-type', 'compatible', 'manufacturer', MARK)),
        ('product', ('product-name', 'product-description', 'fdr-product-type', 'sub-product-type', 'unique-model',
                     'compatible-device-fallback', 'compatible-app-variant')))

spec = importlib.util.spec_from_file_location('boot_args', Path(__file__).with_name('boot_args.py'))
boot_args = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot_args)


def prop(node, name):
    return next((p for p in node[0] if p[0] == name), None)


def set_string(node, name, text):
    value = text.encode() + b'\0'
    p = prop(node, name)
    if p:
        if p[1] & 0x80000000:
            raise SystemExit(f'{name}: a placeholder property (DarwinBoot fills it), not an identity value')
        p[1], p[2] = len(value), value
    else:
        node[0].append([name, len(value), value, None])


def child(root, name):
    for c in root[1]:
        p = prop(c, 'name')
        if p and p[2].rstrip(b'\0').decode() == name:
            return c
    raise SystemExit(f'the ADT has no /{name} node')


def strings(value):
    return [s.decode() for s in value.rstrip(b'\0').split(b'\0')] if value else []


def show(root):
    for node_name, names in SHOW:
        node = child(root, node_name) if node_name else root
        for n in names:
            p = prop(node, n)
            if p:
                print(f'/{node_name}  {n} = {" ".join(repr(s) for s in strings(p[2]))}')


def load_identity(name):
    path = IDENTITIES / f'{name}.toml'
    if not path.is_file():
        known = ', '.join(['vma2'] + sorted(p.stem for p in IDENTITIES.glob('*.toml')))
        raise SystemExit(f'identity {name}: no {path.relative_to(DEVICE)} (known: {known})')
    table = tomllib.loads(path.read_text())
    for section in ('root', 'product'):
        for kind, values in table.get(section, {}).items():
            if kind != 'string' or not all(isinstance(v, str) for v in values.values()):
                raise SystemExit(f'{path}: [{section}.{kind}]: only string values are identity values here')
    return table


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('input', type=Path)
    p.add_argument('output', type=Path, nargs='?')
    p.add_argument('--identity')
    p.add_argument('--show', action='store_true')
    a = p.parse_args()
    data = a.input.read_bytes()
    root = boot_args.load(data)
    if a.show:
        show(root)
        return
    if not a.output or not a.identity:
        p.error('IN OUT --identity NAME (or ADT --show)')
    if prop(root, MARK):
        raise SystemExit(f'{a.input}: already carries {MARK} = {strings(prop(root, MARK)[2])}; give the ADT made without one')
    if a.identity == 'vma2':
        a.output.write_bytes(data)
        print(f'{a.output}: identity vma2 (configs/k27-adt.toml\'s), the input unchanged')
        return
    table = load_identity(a.identity)
    product = child(root, 'product')
    for name, text in table.get('root', {}).get('string', {}).items():
        old = prop(root, name)
        if name == 'compatible' and old:
            if old[1] & 0x80000000:
                raise SystemExit('compatible: a placeholder property (DarwinBoot fills it), not an identity value')
            rest = [s for s in strings(old[2]) if s != text]
            value = b''.join(s.encode() + b'\0' for s in [text, *rest])
            old[1], old[2] = len(value), value
        else:
            set_string(root, name, text)
    for name, text in table.get('product', {}).get('string', {}).items():
        set_string(product, name, text)
    set_string(root, MARK, a.identity)
    out = boot_args.serialize(root)
    check = boot_args.load(out)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_bytes(out)
    print(f'{a.output}: identity {a.identity} ({table.get("identity", {}).get("summary", "")}), {len(out)} bytes')
    show(check)


if __name__ == '__main__':
    sys.exit(main())
