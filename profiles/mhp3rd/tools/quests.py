#!/usr/bin/env python3
"""The game's quest files, for making quests mods add (docs/QUESTS.md).

A quest is one DATA.BIN entry, encrypted. This tool reads the keys from the
game's executable (EBOOT.ELF, as prepare_game.sh leaves it), decrypts a quest
from the disc image into the payload a mod's [Quests] section takes, shows a
payload's fields, and changes the ones a new quest usually needs.

    quests.py list GAME.iso EBOOT.ELF
    quests.py extract GAME.iso EBOOT.ELF 204 quest.bin
    quests.py show quest.bin
    quests.py edit quest.bin new.bin --id 62001 --name "Tigrex Peaks" --monster 42=58

Nothing here writes the disc or the executable. The payloads it writes are the
game's data: keep them out of public repositories you do not have the rights
for.
"""

import argparse
import hashlib
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import databin  # noqa: E402
from extraction_paths import extraction_path  # noqa: E402

MULTIPLIERS = 0x089D5CE0
MODULI = 0x089D5CD0
SALT = 0x0896BB48
HEADER = 32
MAX_PAYLOAD = 24576
FIRST_QUEST_ENTRY = 2800  # the quest files lie between these entries
LAST_QUEST_ENTRY = 4060


def user_file(path):
    """A file the user named: one portable name inside the directory it names."""
    path = Path(path)
    return extraction_path(path.parent, [path.name])


def read_keys(eboot):
    """The multipliers, moduli and salt, from the executable's loadable segment."""
    data = user_file(eboot).read_bytes()
    if data[:4] != b'\x7fELF':
        raise ValueError('not an ELF executable')
    phoff, = struct.unpack_from('<I', data, 0x1C)
    _, offset, address, _, size = struct.unpack_from('<5I', data, phoff)

    def at(guest, length):
        start = guest - address + offset
        if guest < address or guest + length > address + size:
            raise ValueError('the executable does not hold the quest keys')
        return data[start:start + length]

    keys = (struct.unpack('<4I', at(MULTIPLIERS, 16)), struct.unpack('<4I', at(MODULI, 16)), at(SALT, 16))
    if 0 in keys[0] or 0 in keys[1]:
        raise ValueError('the executable does not hold the quest keys')
    return keys


def _stream(state, keys, data, offset):
    multipliers, moduli, _ = keys
    out = bytearray(data)
    for at in range(0, len(out) - 1, 2):
        i = ((at + offset) >> 1) & 3
        state[i] = (state[i] * multipliers[i]) % moduli[i] & 0xFFFF
        word, = struct.unpack_from('<H', out, at)
        struct.pack_into('<H', out, at, word ^ state[i])
    return bytes(out)


def _start(seeds, keys):
    return [seed if seed else keys[0][i] & 0xFFFF for i, seed in enumerate(seeds)]


def decrypt(blob, keys):
    """The payload of an encrypted quest file, or None when it is not one."""
    if len(blob) < HEADER:
        return None
    state = _start(struct.unpack_from('<4H', blob, 0), keys)
    head = _stream(state, keys, blob[8:HEADER], 8)
    size, = struct.unpack_from('<I', head, 0)
    if size == 0 or size > MAX_PAYLOAD or size > len(blob) - HEADER:
        return None
    payload = _stream(state, keys, blob[HEADER:HEADER + size], HEADER)
    return payload if hashlib.sha1(payload + keys[2]).digest() == head[4:24] else None


def encrypt(payload, keys, seeds=(0x1F2E, 0x3D4C, 0x5B6A, 0x7988)):
    """An encrypted quest file the game accepts (an odd payload gets a zero byte)."""
    if len(payload) % 2:
        payload += b'\0'
    state = list(seeds)
    head = _stream(state, keys, struct.pack('<I', len(payload)) + hashlib.sha1(payload + keys[2]).digest(), 8)
    return struct.pack('<4H', *seeds) + head + _stream(state, keys, payload, HEADER)


def record(payload):
    """The quest record's fields this tool knows."""
    offset, = struct.unpack_from('<I', payload, 0)
    if offset < 4 or offset + 0x48 > len(payload):
        raise ValueError('not a quest payload')
    fee, reward = struct.unpack_from('<2I', payload, offset + 4)
    frames, = struct.unpack_from('<I', payload, offset + 0x10)
    quest, stars = struct.unpack_from('<2H', payload, offset + 0x1C)
    target, count = struct.unpack_from('<2H', payload, offset + 0x2C)
    monsters = [m for m in struct.unpack_from('<5H', payload, offset + 0x3C) if m]
    name = payload[offset + 0x48:].split(b'\0', 1)[0].decode('utf-8', 'replace')
    return {'offset': offset, 'id': quest, 'stars': stars, 'map': payload[offset + 0x20], 'fee': fee,
            'reward': reward, 'minutes': frames // 1800, 'target': target, 'count': count,
            'monsters': monsters, 'name': name}


def large_monsters(payload):
    """Offsets of the large monsters' kinds: the list of kinds to load and each spawn record."""
    group, = struct.unpack_from('<I', payload, 0x20)
    if not 0 < group <= len(payload) - 16:
        return [], []
    loads, spawns = struct.unpack_from('<2I', payload, group + 8)
    kinds = []
    at = loads
    while 0 < at <= len(payload) - 2 and struct.unpack_from('<H', payload, at)[0] != 0xFFFF:
        kinds.append(at)
        at += 4
    records = []
    at = spawns
    while 0 < at <= len(payload) - 0x30 and struct.unpack_from('<I', payload, at)[0] not in (0xFFFF, 0):
        records.append(at)
        at += 0x30
    return kinds, records


def edit(payload, quest=None, name=None, monsters=()):
    out = bytearray(payload)
    info = record(payload)['offset']
    if quest is not None:
        struct.pack_into('<H', out, info + 0x1C, quest)
    if name is not None:
        start = info + 0x48
        room = out[start:].index(b'\0')
        text = name.encode('utf-8')
        if len(text) > room:
            raise ValueError('the name has room for %d bytes here' % room)
        out[start:start + room] = text.ljust(room, b'\0')
    kinds, records = large_monsters(payload)
    for old, new in monsters:
        places = [(at, '<H') for at in kinds] + [(at, '<I') for at in records]
        places += [(info + 0x2C, '<H')] + [(info + 0x3C + 2 * i, '<H') for i in range(5)]
        changed = 0
        for at, layout in places:
            if struct.unpack_from(layout, out, at)[0] == old:
                struct.pack_into(layout, out, at, new)
                changed += 1
        if not changed:
            raise ValueError('monster kind %d is not in this quest' % old)
    return bytes(out)


def quest_entries(image, keys):
    archive = databin.Archive(image)
    for entry in range(FIRST_QUEST_ENTRY, min(LAST_QUEST_ENTRY, len(archive))):
        size = archive.entry_size(entry)
        if HEADER < size <= MAX_PAYLOAD + HEADER + 2048:
            payload = decrypt(archive.read(entry), keys)
            if payload is not None:
                yield entry, payload


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    listing = sub.add_parser('list', help='every quest on the disc')
    listing.add_argument('image')
    listing.add_argument('eboot')
    extract = sub.add_parser('extract', help="a quest's payload, by its id")
    extract.add_argument('image')
    extract.add_argument('eboot')
    extract.add_argument('id', type=int)
    extract.add_argument('output')
    show = sub.add_parser('show', help="a payload's fields")
    show.add_argument('payload')
    change = sub.add_parser('edit', help="change a payload's id, name or large monsters")
    change.add_argument('payload')
    change.add_argument('output')
    change.add_argument('--id', type=int)
    change.add_argument('--name')
    change.add_argument('--monster', action='append', default=[], metavar='OLD=NEW',
                        help='replace a large monster kind wherever the quest names it')
    args = parser.parse_args(argv)

    if args.command in ('list', 'extract'):
        keys = read_keys(args.eboot)
        for entry, payload in quest_entries(args.image, keys):
            fields = record(payload)
            if args.command == 'list':
                print('%5d  entry %4d  %d stars  map %2d  %s' % (fields['id'], entry, fields['stars'], fields['map'],
                                                                  fields['name']))
            elif fields['id'] == args.id:
                user_file(args.output).write_bytes(payload)
                print('quest %d (%s) from entry %d: %d bytes' % (args.id, fields['name'], entry, len(payload)))
                return 0
        if args.command == 'extract':
            print('quest %d is not on the disc' % args.id, file=sys.stderr)
            return 1
        return 0
    payload = user_file(args.payload).read_bytes()
    if args.command == 'show':
        for key, value in record(payload).items():
            print('%-8s %s' % (key, value))
        kinds, records = large_monsters(payload)
        print('loads    %s' % [struct.unpack_from('<H', payload, at)[0] for at in kinds])
        for at in records:
            kind, = struct.unpack_from('<I', payload, at)
            area, = struct.unpack_from('<H', payload, at + 6)
            x, y, z = struct.unpack_from('<3f', payload, at + 0x10)
            print('spawn    kind %d area %d at %.0f, %.0f, %.0f' % (kind, area, x, y, z))
        return 0
    pairs = [tuple(int(part) for part in item.split('=', 1)) for item in args.monster]
    user_file(args.output).write_bytes(edit(payload, args.id, args.name, pairs))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
