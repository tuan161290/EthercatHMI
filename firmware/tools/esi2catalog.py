#!/usr/bin/env python3
"""Generate components/ecat/ecat_catalog_gen.c from EtherCAT ESI files.

The firmware recognises a slave only if its vendor/product pair is in this catalog,
and configures a modular coupler (Leadshine R1EC) only with module data taken from
its ESI. Re-run after adding or updating an ESI file:

    python tools/esi2catalog.py ../vendor/ESI/Leadshine -o components/ecat/ecat_catalog_gen.c

Only the Python standard library is used.
"""

import argparse
import os
import sys
import xml.etree.ElementTree as ET

# ESI <GroupType> -> firmware device kind. Unlisted groups are skipped with a warning.
KIND_BY_GROUP = {
    'Servo Drive': 'ECAT_KIND_SERVO',
    'Close Loop Stepper Drive': 'ECAT_KIND_STEPPER',
    'TerminalCoupler': 'ECAT_KIND_COUPLER',
    'Ezi-SERVO Controller': 'ECAT_KIND_SERVO',
    'Ezi-IO Controller': 'ECAT_KIND_IO',
}

MAX_MODULE_PDO = 6  # keep in sync with ECAT_MODULE_MAX_PDO in ecat_catalog.h


def num(text):
    s = (text or '').strip()
    if s.lower().startswith('#x'):
        return int(s[2:], 16)
    if s.lower().startswith('0x'):
        return int(s[2:], 16)
    return int(s)


def first_text(elems):
    """Prefer the English (LcId 1033) element of a repeated <Name>."""
    elems = list(elems)
    for e in elems:
        if e.get('LcId') == '1033':
            return (e.text or '').strip()
    return (elems[0].text or '').strip() if elems else ''


def c_str(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def pdo_bits(pdo):
    return sum(num(e.findtext('BitLen')) for e in pdo.findall('Entry'))


def bool_count(pdo):
    return sum(1 for e in pdo.findall('Entry') if (e.findtext('DataType') or '').strip() == 'BOOL')


def assigned(pdos):
    """PDOs the ESI assigns to a sync manager by default (have an Sm attribute)."""
    return [p for p in pdos if p.get('Sm') is not None]


def dictionary(parent):
    """(data types by name, objects) of the <Profile><Dictionary> of a device or module."""
    d = parent.find('./Profile/Dictionary')
    if d is None:
        return {}, []
    types = {(t.findtext('Name') or '').strip(): t for t in d.findall('./DataTypes/DataType')}
    return types, d.findall('./Objects/Object')


def find_object(parent, index):
    types, objects = dictionary(parent)
    for obj in objects:
        if num(obj.findtext('Index')) == index:
            return types, types.get((obj.findtext('Type') or '').strip())
    return types, None


def object_bits(parent, index, sub):
    """BitSize of object index:sub in the dictionary of `parent`, None if not described."""
    _, t = find_object(parent, index)
    if t is None:
        return None
    subitems = t.findall('SubItem')
    if not subitems:
        return num(t.findtext('BitSize')) if sub == 0 else None
    for si in subitems:
        if si.findtext('SubIdx') is not None and num(si.findtext('SubIdx')) == sub:
            return num(si.findtext('BitSize'))
    return None


def array_elements(parent, index):
    """Element count of an array object such as 0xF030, 0 if not described."""
    types, t = find_object(parent, index)
    if t is None:
        return 0
    for si in t.findall('SubItem'):
        arr = types.get((si.findtext('Type') or '').strip())
        if arr is not None and arr.find('ArrayInfo') is not None:
            return num(arr.findtext('./ArrayInfo/Elements'))
    return 0


def init_cmds(parent, where):
    """PRE-OP -> SAFE-OP InitCmds. The download size is the object's size from the dictionary
    (<Data AdaptAutomatically> is shorter than the object, e.g. 1 byte for a UINT filter):
    a strict CoE stack rejects a download whose length differs from the object."""
    cmds = []
    for ic in parent.findall('./Mailbox/CoE/InitCmd'):
        transitions = [t.text.strip() for t in ic.findall('Transition')]
        if 'PS' not in transitions:
            continue
        data = (ic.findtext('Data') or '').strip()
        raw = bytes.fromhex(data)
        value = int.from_bytes(raw, 'little')
        index, sub = num(ic.findtext('Index')), num(ic.findtext('SubIndex'))
        bits = object_bits(parent, index, sub)
        size = len(raw)
        if bits is not None:
            if bits % 8 or not 1 <= bits // 8 <= 4:
                sys.exit(f'{where}: InitCmd 0x{index:04X}:{sub} object is {bits} bits, not supported')
            size = bits // 8
            if value >= 1 << (8 * size):
                sys.exit(f'{where}: InitCmd 0x{index:04X}:{sub} data {data!r} does not fit {bits} bits')
            if size != len(raw) and ic.find('Data').get('AdaptAutomatically') not in ('true', '1'):
                print(f'warning: {where}: InitCmd 0x{index:04X}:{sub} data is {len(raw)} byte(s), '
                      f'object is {size}: using the object size', file=sys.stderr)
        elif not 1 <= size <= 4:
            sys.exit(f'{where}: InitCmd data {data!r} is not 1..4 bytes')
        cmds.append((index, sub, size, value, (ic.findtext('Comment') or '').strip()))
    return cmds


def parse_file(path):
    root = ET.parse(path).getroot()
    vendor = num(root.findtext('./Vendor/Id'))
    rel = os.path.basename(path)
    devices, modules = [], []
    coupler_product = None

    for dev in root.findall('./Descriptions/Devices/Device'):
        t = dev.find('Type')
        product = num(t.get('ProductCode'))
        name = (t.text or '').strip()
        group = (dev.findtext('GroupType') or '').strip()
        kind = KIND_BY_GROUP.get(group)
        if kind is None:
            print(f'warning: {rel}: {name}: group {group!r} not mapped, skipped', file=sys.stderr)
            continue
        slots = dev.find('Slots')
        if kind == 'ECAT_KIND_COUPLER' and slots is None:
            kind = 'ECAT_KIND_IO'

        alias_index = 0
        for obj in dev.findall('./Profile/Dictionary/Objects/Object'):
            oname = first_text(obj.findall('Name'))
            if oname.startswith('Slave Alias') and 'Source' not in oname:
                alias_index = num(obj.findtext('Index'))
                break

        timeouts = dev.find('./Info/StateMachine/Timeout')
        d = {
            'vendor': vendor, 'product': product, 'name': name, 'kind': kind,
            'alias_index': alias_index, 'source': rel,
            'preop_ms': num(timeouts.findtext('PreopTimeout')) if timeouts is not None else 0,
            'safeop_op_ms': num(timeouts.findtext('SafeopOpTimeout')) if timeouts is not None else 0,
            'slot_index_inc': 0, 'slot_pdo_inc': 0, 'status_pdo': 0, 'status_bits': 0,
            'sm_out': (0, 0), 'sm_in': (0, 0),
        }
        if kind == 'ECAT_KIND_COUPLER':
            d['slot_index_inc'] = num(slots.get('SlotIndexIncrement'))
            d['slot_pdo_inc'] = num(slots.get('SlotPdoIncrement'))
            for sm in dev.findall('Sm'):
                role = (sm.text or '').strip()
                if role in ('Outputs', 'Inputs'):
                    key = 'sm_out' if role == 'Outputs' else 'sm_in'
                    d[key] = (num(sm.get('StartAddress')), num(sm.get('ControlByte')))
            fixed_tx = assigned(dev.findall('TxPdo'))
            if len(fixed_tx) > 1:
                sys.exit(f'{rel}: {name}: more than one coupler TxPDO, not supported')
            if fixed_tx:
                d['status_pdo'] = num(fixed_tx[0].findtext('Index'))
                d['status_bits'] = pdo_bits(fixed_tx[0])
            # Configured (0xF030) and detected (0xF050) module lists bound the rack size.
            sizes = [n for n in (array_elements(dev, 0xF030), array_elements(dev, 0xF050)) if n]
            if not sizes:
                sys.exit(f'{rel}: {name}: no 0xF030/0xF050 array in the dictionary')
            d['max_modules'] = min(sizes)
            coupler_product = product
        if any(x['product'] == product for x in devices):
            continue  # another revision of the same product
        devices.append(d)

    for mod in root.findall('./Descriptions/Modules/Module'):
        t = mod.find('Type')
        if t is None or t.get('ModuleIdent') is None:
            continue
        if coupler_product is None:
            sys.exit(f'{rel}: modules without a modular coupler device')
        name = (t.text or '').strip()
        rx = assigned(mod.findall('RxPdo'))
        tx = assigned(mod.findall('TxPdo'))
        if len(rx) > MAX_MODULE_PDO or len(tx) > MAX_MODULE_PDO:
            sys.exit(f'{rel}: {name}: more than {MAX_MODULE_PDO} PDOs')
        modules.append({
            'coupler': coupler_product,
            'ident': num(t.get('ModuleIdent')),
            'name': name,
            'di': sum(bool_count(p) for p in tx),
            'do': sum(bool_count(p) for p in rx),
            'in_bits': sum(pdo_bits(p) for p in tx),
            'out_bits': sum(pdo_bits(p) for p in rx),
            'rx': [num(p.findtext('Index')) for p in rx],
            'tx': [num(p.findtext('Index')) for p in tx],
            'init': init_cmds(mod, f'{rel}: {name}'),
            'source': rel,
        })
    return devices, modules


def collect(paths):
    files = []
    for p in paths:
        if os.path.isdir(p):
            for dirpath, _, names in os.walk(p):
                files += [os.path.join(dirpath, n) for n in names if n.lower().endswith('.xml')]
        else:
            files.append(p)
    return sorted(files)


def emit(devices, modules, sources):
    out = []
    w = out.append
    w('/*')
    w(' * GENERATED by tools/esi2catalog.py - do not edit by hand.')
    w(' * Sources (vendor/ESI/, read-only):')
    for s in sources:
        w(f' *   {s}')
    w(' */')
    w('')
    w('#include "ecat_catalog.h"')
    w('')
    w('const ecat_device_info_t ecat_catalog_devices[] = {')
    for d in devices:
        w('    {')
        w(f'        .vendor = 0x{d["vendor"]:08X}, .product = 0x{d["product"]:08X}, .name = {c_str(d["name"])},')
        w(f'        .kind = {d["kind"]}, .alias_index = 0x{d["alias_index"]:04X},')
        if d['kind'] == 'ECAT_KIND_COUPLER':
            w(f'        .slot_index_inc = 0x{d["slot_index_inc"]:X}, .slot_pdo_inc = 0x{d["slot_pdo_inc"]:X},'
              f' .max_modules = {d["max_modules"]},')
            w(f'        .status_pdo = 0x{d["status_pdo"]:04X}, .status_bits = {d["status_bits"]},')
            w(f'        .sm_out = {{ 0x{d["sm_out"][0]:04X}, 0x{d["sm_out"][1]:02X} }},'
              f' .sm_in = {{ 0x{d["sm_in"][0]:04X}, 0x{d["sm_in"][1]:02X} }},')
        w(f'        .source = {c_str(d["source"])},')
        w('    },')
    w('};')
    w('const size_t ecat_catalog_device_count = sizeof(ecat_catalog_devices) / sizeof(ecat_catalog_devices[0]);')
    w('')
    for key, var in (('preop_ms', 'preop'), ('safeop_op_ms', 'safeop_op')):
        worst = max(devices, key=lambda d: d[key])
        w(f'const uint32_t ecat_catalog_{var}_timeout_ms = {worst[key]}; /* {worst["name"]} */')
    w('')
    for i, m in enumerate(modules):
        if not m['init']:
            continue
        w(f'static const ecat_init_cmd_t s_init_{i}[] = {{ /* {m["name"]} */')
        for idx, sub, size, value, comment in m['init']:
            w(f'    {{ 0x{idx:04X}, {sub}, {size}, 0x{value:X} }}, /* {comment} */')
        w('};')
    w('')
    w('const ecat_module_info_t ecat_catalog_modules[] = {')
    for i, m in enumerate(modules):
        rx = ', '.join(f'0x{x:04X}' for x in m['rx']) or '0'
        tx = ', '.join(f'0x{x:04X}' for x in m['tx']) or '0'
        w('    {')
        w(f'        .coupler_product = 0x{m["coupler"]:08X}, .ident = 0x{m["ident"]:08X}, .name = {c_str(m["name"])},')
        w(f'        .di = {m["di"]}, .dout = {m["do"]}, .in_bits = {m["in_bits"]}, .out_bits = {m["out_bits"]},')
        w(f'        .n_rx = {len(m["rx"])}, .rx_pdo = {{ {rx} }}, .n_tx = {len(m["tx"])}, .tx_pdo = {{ {tx} }},')
        if m['init']:
            w(f'        .n_init = {len(m["init"])}, .init = s_init_{i},')
        w('    },')
    w('};')
    w('const size_t ecat_catalog_module_count = sizeof(ecat_catalog_modules) / sizeof(ecat_catalog_modules[0]);')
    return '\n'.join(out) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('inputs', nargs='+', help='ESI files or folders')
    ap.add_argument('-o', '--output', required=True)
    args = ap.parse_args()

    devices, modules, sources = [], [], []
    for f in collect(args.inputs):
        d, m = parse_file(f)
        if d or m:
            sources.append(os.path.relpath(f, os.path.dirname(os.path.abspath(args.output))).replace('\\', '/'))
        for x in d:
            if any(y['vendor'] == x['vendor'] and y['product'] == x['product'] for y in devices):
                continue
            devices.append(x)
        modules += m
    if not devices:
        sys.exit('no devices found')
    with open(args.output, 'w', newline='\n', encoding='utf-8') as fh:
        fh.write(emit(devices, modules, sources))
    print(f'{len(devices)} device(s), {len(modules)} module(s) -> {args.output}')


if __name__ == '__main__':
    main()
