# scene.py <importer> [--stats] [--out file]
# Reads the deployed configuration of an importer (%XXMI_ROOT%\<importer>)
# the way the DLL includes it, and writes what a harness has to draw for the
# TextureOverride sections of the active mods to match: one object per mod ini,
# with the buffers and textures whose hashes its sections look for.
import fnmatch
import os
import re
import sys

from condition import ini_int, static_condition, static_value
from paths import importers_root

ROOT = importers_root()

SECTION = re.compile(r'^\[(.+)\]\s*$')
ASSIGN = re.compile(r'^\s*(?:(?:pre|post|global|local|persist)\s+)*([^=;]+?)\s*=\s*(.*?)\s*$')


MAX_PATH = 260
ASCII_LOWER = str.maketrans('ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz')


def fold(text):
    # The order of _wcsicmp, which the DLL sorts section and file names by
    return text.translate(ASCII_LOWER)


def include_entries(path, hunting_static=False):
    # (namespace = of the preamble or None, [(name, [(key, value)])] of the
    # [Include...] sections of an ini with their lines in order); None when a
    # condition in its preamble makes the DLL skip the whole file
    sections = []
    namespace = None
    preamble = True
    entries = None
    try:
        with open(path, encoding='utf-8-sig', errors='replace') as f:
            lines = f.read().splitlines()
    except OSError:
        return namespace, sections
    for line in lines:
        line = line.strip()
        if not line or line.startswith(';'):
            continue
        m = SECTION.match(line)
        if m:
            preamble = False
            name = m.group(1).strip()
            entries = [] if name.lower().startswith('include') else None
            if entries is not None:
                sections.append((name, entries))
            continue
        m = ASSIGN.match(line)
        if not m:
            continue
        if preamble:
            if m.group(1).lower() == 'condition' and not static_condition(m.group(2), hunting_static):
                return None
            if m.group(1).lower() == 'namespace':
                namespace = m.group(2)
        elif entries is not None:
            entries.append((m.group(1), m.group(2)))
    return namespace, sections


def ini_value(path, section, key):
    for name, lines in read_ini(path):
        if name.lower() == section:
            for line in lines:
                m = ASSIGN.match(line)
                if m and m.group(1).lower() == key:
                    return m.group(2)
    return None


def includes_hunting_static(importer):
    # With config_initialization_delay < 0 the includes are parsed only as the
    # DLL loads, before [Hunting] is read: hunting is still disabled then. At
    # the delayed reload it is not, as [Hunting] turns 0 into soft disabled.
    # The setting is read as the DLL loads, when hunting is still disabled too.
    delay = ini_value(os.path.join(ROOT, importer, 'd3dx.ini'), 'system', 'config_initialization_delay')
    return ini_int(delay, 0, True) < 0


def mod_inis(importer):
    # In the order the DLL parses them (ParseIncludedIniFiles): round by round,
    # the [Include...] sections known so far sorted by name, where those of an
    # included file are named Include\<namespace>\<rest>, and each section's
    # lines in order. A path is relative to the folder of the ini that names
    # it; exclude_recursive is d3dx.ini's [Include] only.
    top = os.path.join(ROOT, importer)
    hunting_static = includes_hunting_static(importer)
    parsed = include_entries(os.path.join(top, 'd3dx.ini'), hunting_static)
    first = parsed[1] if parsed else []
    excluded = [value.lower() for name, entries in first if name.lower() == 'include'
                for key, value in entries if key.lower() == 'exclude_recursive']

    def skip(name):
        return any(fnmatch.fnmatch(name.lower(), p) for p in excluded)

    def recursive(rel):
        # ParseIniFilesRecursive: the .ini files of a folder (the extension
        # compared case sensitively), then its folders, each sorted by name
        if len(os.path.join(top, rel, '*')) >= MAX_PATH:
            return
        try:
            names = os.listdir(os.path.join(top, rel))
        except OSError:
            return
        names = sorted((name for name in names if not skip(name)), key=fold)
        for name in names:
            if name.endswith('.ini') and os.path.isfile(os.path.join(top, rel, name)):
                yield os.path.join(top, rel, name), rel + '\\' + name
        for name in names:
            if os.path.isdir(os.path.join(top, rel, name)):
                yield from recursive(rel + '\\' + name)

    # A file reached twice (by include and by include_recursive) is parsed
    # twice: its [Rendering] lines apply again in the later place, its own
    # sections are duplicates the DLL drops. found lists it once, order every
    # time it is parsed.
    # Paths that grow with every round (a file including ..\<itself>) end as
    # the DLL's do, when they no longer open within MAX_PATH.
    found, order, seen, seen_files = [], [], set(), set()
    current = {}
    for name, entries in first:
        current.setdefault(fold(name), ('', entries))
    while current:
        later = {}
        for section in sorted(current):
            folder, entries = current[section]
            for key, value in entries:
                rel = folder + value
                if rel in seen:
                    continue
                seen.add(rel)
                if key == 'include':
                    path = os.path.join(top, rel)
                    paths = [(path, rel)] if os.path.isfile(path) and len(path) < MAX_PATH else []
                elif key == 'include_recursive':
                    paths = recursive(rel)
                else:
                    continue
                for path, namespace in paths:
                    parsed = include_entries(path, hunting_static)
                    if parsed is None:
                        continue
                    order.append(path)
                    if os.path.normcase(os.path.normpath(path)) not in seen_files:
                        seen_files.add(os.path.normcase(os.path.normpath(path)))
                        found.append(path)
                    renamed, sections = parsed
                    # Relative paths stay those of the file's own folder when
                    # namespace = renames it
                    base = namespace[:namespace.rfind('\\') + 1]
                    for name, entries in sections:
                        full = 'Include\\%s\\%s' % (namespace if renamed is None else renamed, name[len('Include'):])
                        later.setdefault(fold(full), (base, entries))
        current = later
    return found, order


def read_ini(path):
    try:
        with open(path, encoding='utf-8-sig', errors='replace') as f:
            lines = f.read().splitlines()
    except OSError:
        return []
    sections = []
    current = None
    for line in lines:
        stripped = line.strip()
        if not stripped or stripped.startswith(';'):
            continue
        m = SECTION.match(stripped)
        if m:
            current = (m.group(1), [])
            sections.append(current)
        elif current is not None:
            current[1].append(stripped)
    return sections


MATCH_KEYS = ('match_type', 'match_usage', 'match_bind_flags', 'match_cpu_access_flags', 'match_misc_flags',
              'match_byte_width', 'match_stride', 'match_mips', 'match_format', 'match_width', 'match_height',
              'match_depth', 'match_array', 'match_msaa', 'match_msaa_quality')


def overrides(path):
    out = []
    for name, body in read_ini(path):
        if not name.lower().startswith('textureoverride'):
            continue
        o = {'name': name[len('TextureOverride'):], 'hash': None, 'first': None, 'count': None, 'fuzzy': {},
             'body': body, 'vertex_count': None}
        for line in body:
            m = ASSIGN.match(line)
            if not m:
                continue
            key, value = m.group(1).lower(), m.group(2)
            if key == 'hash':
                try:
                    o['hash'] = int(value, 16)
                except ValueError:
                    pass
            elif key == 'match_first_index':
                o['first'] = value
            elif key == 'match_index_count':
                o['count'] = value
            elif key == 'match_first_vertex' or key == 'match_vertex_count' or key == 'match_first_instance' or key == 'match_instance_count':
                o['fuzzy'][key] = value
            elif key in MATCH_KEYS:
                o['fuzzy'][key] = value
            elif key.startswith('override_'):
                o['vertex_count'] = value
        out.append(o)
    return out


VK = {'BACKSPACE': 8, 'TAB': 9, 'RETURN': 13, 'ENTER': 13, 'PAUSE': 19, 'CAPITAL': 20, 'ESCAPE': 27, 'SPACE': 32, 'PRIOR': 33,
      'NEXT': 34, 'END': 35, 'HOME': 36, 'LEFT': 37, 'UP': 38, 'RIGHT': 39, 'DOWN': 40, 'INSERT': 45, 'DELETE': 46,
      'MULTIPLY': 0x6a, 'ADD': 0x6b, 'SUBTRACT': 0x6d, 'DECIMAL': 0x6e, 'DIVIDE': 0x6f, 'OEM_1': 0xba, 'OEM_PLUS': 0xbb,
      'OEM_COMMA': 0xbc, 'OEM_MINUS': 0xbd, 'OEM_PERIOD': 0xbe, 'OEM_2': 0xbf, 'OEM_3': 0xc0, 'OEM_4': 0xdb, 'OEM_5': 0xdc,
      'OEM_6': 0xdd, 'OEM_7': 0xde, 'LBUTTON': 1, 'RBUTTON': 2, 'MBUTTON': 4, 'XBUTTON1': 5, 'XBUTTON2': 6,
      'CONTROL': 17, 'CTRL': 17, 'MENU': 18, 'ALT': 18, 'SHIFT': 16}
for _i in range(10):
    VK['NUMPAD%d' % _i] = 0x60 + _i
for _i in range(1, 25):
    VK['F%d' % _i] = 0x6f + _i


def key_combo(text):
    # The first binding of a key = line as the virtual keys to hold, or None
    # for what the harness cannot press (controllers, names not known here).
    combo = []
    main = None
    for token in text.split():
        t = token.upper()
        if t.startswith('NO_') or t.startswith('XB_'):
            if t.startswith('XB_'):
                return None
            continue
        if t.startswith('VK_'):
            t = t[3:]
        if t in ('CTRL', 'CONTROL', 'ALT', 'MENU', 'SHIFT'):
            combo.append(VK[t])
        elif t in VK:
            main = VK[t]
        elif len(t) == 1 and (t.isalpha() or t.isdigit()):
            main = ord(t)
        else:
            return None
    if main is None or main < 8:
        return None
    return combo + [main]


def keys_of(path):
    out = []
    for name, body in read_ini(path):
        if not name.lower().startswith('key'):
            continue
        for line in body:
            m = ASSIGN.match(line)
            if m and m.group(1).lower() == 'key':
                combo = key_combo(m.group(2))
                if combo:
                    out.append(combo)
                break
    return out


def reserved_keys(importer):
    # What d3dx.ini and the Core bind: reloading, frame analysis, switching
    # the mods off. The harness leaves those alone.
    keys = set()
    paths = [os.path.join(ROOT, importer, 'd3dx.ini')]
    for base, dirs, files in os.walk(os.path.join(ROOT, importer, 'Core')):
        paths += [os.path.join(base, n) for n in files if n.lower().endswith('.ini')]
    for path in paths:
        for name, body in read_ini(path):
            for line in body:
                m = ASSIGN.match(line)
                if not m:
                    continue
                if name.lower().startswith('key') and m.group(1).lower() != 'key':
                    continue
                if not name.lower().startswith('key') and name.lower() != 'hunting':
                    continue
                combo = key_combo(m.group(2))
                if combo:
                    keys.add(combo[-1])
    return keys


def user_config(importer):
    # [Include] user_config of d3dx.ini, relative to the importer folder
    # unless it starts with a drive or a backslash
    top = os.path.join(ROOT, importer)
    value = ini_value(os.path.join(top, 'd3dx.ini'), 'include', 'user_config') or 'd3dx_user.ini'
    if value[1:2] == ':' or value.startswith('\\\\'):
        return value
    if value.startswith('\\'):
        return os.path.splitdrive(top)[0] + value
    return os.path.normpath(top + '\\' + value)


def rendering(importer, order, key):
    # [Rendering] as the DLL ends up with it: the first value in d3dx.ini,
    # then every parse of an included ini (order of mod_inis) and last the
    # user config, each overriding it with its own last value. The user config
    # is parsed like an include, so a condition in its preamble can skip it too.
    top = os.path.join(ROOT, importer)
    value = ini_value(os.path.join(top, 'd3dx.ini'), 'rendering', key)
    user = user_config(importer)
    if include_entries(user, includes_hunting_static(importer)) is not None:
        order = order + [user]
    for path in order:
        for name, lines in read_ini(path):
            if name.lower() == 'rendering':
                for line in lines:
                    m = ASSIGN.match(line)
                    if m and m.group(1).lower() == key:
                        value = m.group(2)
    return value


def ini_bool(text, default):
    # GetIniBool: the words ParseBoolValue knows, anything else as an expression
    if not text:
        return default
    word = text.strip().lower()
    if word in ('1', 'true', 'yes', 'on'):
        return True
    if word in ('0', 'false', 'no', 'off'):
        return False
    value = static_value(word, False)
    return default if value is None else value != 0


# Where the mesh a section with match_first_index is for sits in a draw, and
# how the game draws: the Unity games with DrawIndexed, the others instanced.
PROFILES = {
    'GIMI': ('ib', 'indexed'), 'SRMI': ('ib', 'indexed'), 'ZZMI': ('ib', 'indexed'),
    'WWMI': ('vb0', 'instanced'), 'EFMI': ('ib', 'instanced'),
}


def number(text):
    try:
        return int(text.strip(), 0)
    except (ValueError, AttributeError):
        return None


def expanded(body, lists, depth=0):
    # The lines of a section with those of the command lists it runs in place
    # of the run = lines: merged mods keep what a section does in such lists.
    out = []
    for line in body:
        lower = line.lower()
        m = re.match(r'^(?:(?:pre|post)\s+)?run\s*=\s*(\S+)', lower)
        if m and depth < 4 and m.group(1) in lists:
            out += expanded(lists[m.group(1)], lists, depth + 1)
        else:
            out.append(lower)
    return out


def kind_of(o, lists):
    # Which slot of a draw the resource with this hash goes in. A guess from
    # how the mod tools of these games name and write their sections. A wrong
    # guess matters: a section for an index buffer met at a texture slot runs
    # its texture checks from inside a texture check, without end.
    name = o['name'].lower()
    body = expanded(o['body'], lists)
    sets = lambda pattern: any(re.match(r'^(?:(?:pre|post)\s+)?' + pattern + r'\s*=', line) for line in body)
    has = lambda word: any(word in line for line in body)
    if has('cs ==') or 'callback' in name:
        return 'other'
    if o['first'] is not None or o['count'] is not None:
        return 'mesh'
    # CommandListSkinTexture is the Core's list of texture checks, which the
    # sections for an index buffer run
    if re.search(r'(ib|indexbuffer)$', name) or sets('ib') or has('drawindexed') or has('skintexture'):
        return 'mesh'
    if name.endswith('cb') or 'bonedata' in name:
        return 'cb'
    if 'blend' in name and not sets(r'ps-t\d+'):
        return 'vb2' if sets('vb2') and not sets('vb1') else 'vb1'
    if 'texcoord' in name and not sets(r'ps-t\d+'):
        return 'vb2' if sets('vb2') and not sets('vb1') else 'vb1'
    if 'position' in name or 'vertexlimit' in name or name.endswith('draw') or name.endswith('vb'):
        return 'vb0'
    if has('checktextureoverride') or (has('draw_type') and has('handling')):
        return 'mesh'
    for slot in (0, 1, 2, 3):
        if sets('vb%d' % slot):
            return 'vb%d' % slot
    if any(k in o['fuzzy'] for k in ('match_byte_width', 'match_stride')):
        return 'cb'
    return 'tex'


def main():
    importer = sys.argv[1]
    stats = '--stats' in sys.argv
    out_path = None
    if '--out' in sys.argv:
        out_path = sys.argv[sys.argv.index('--out') + 1]
    inis, order = mod_inis(importer)
    objects = []
    kinds = {}
    # Hashes with a section that changes the description of what is made:
    # the game makes those without data, and so must the harness.
    resized = set()
    no_hash = 0
    fuzzy_keys = {}
    for path in inis:
        found = overrides(path)
        lists = {name.lower(): body for name, body in read_ini(path)}
        parts = []
        for o in found:
            if o['hash'] is None:
                no_hash += 1
                for k in o['fuzzy']:
                    fuzzy_keys[k] = fuzzy_keys.get(k, 0) + 1
                continue
            o['kind'] = kind_of(o, lists)
            if o['vertex_count'] is not None or 'VertexLimitRaise' in o['name']:
                resized.add(o['hash'])
            kinds[o['kind']] = kinds.get(o['kind'], 0) + 1
            # 'other': what no draw of a game brings to a slot the Core checks
            if o['kind'] != 'other':
                parts.append(o)
        if parts:
            objects.append((path, parts))
    # A hash is one resource whichever ini names it: if any section takes it
    # for a mesh (or a vertex buffer), the sections that say nothing about it
    # must not put it at a texture slot.
    rank = {'mesh': 0, 'vb0': 1, 'vb1': 1, 'vb2': 1, 'vb3': 1, 'cb': 2, 'tex': 3}
    best = {}
    for path, parts in objects:
        for o in parts:
            if o['hash'] not in best or rank[o['kind']] < rank[best[o['hash']]]:
                best[o['hash']] = o['kind']
    for path, parts in objects:
        for o in parts:
            o['kind'] = best[o['hash']]
    if stats:
        print('%s: %d inis, %d with hashed sections, kinds %s, sections without a hash %d %s' % (
            importer, len(inis), len(objects), kinds, no_hash, fuzzy_keys))
        return

    slot, draw = PROFILES.get(importer, ('ib', 'indexed'))
    reserved = reserved_keys(importer)
    lines = ['importer %s' % importer, 'mesh_slot %s' % slot, 'draw %s' % draw,
             'region %d' % ini_bool(rendering(importer, order, 'track_region_hashes'), False),
             'texture_hash %d' % ini_int(rendering(importer, order, 'texture_hash'), 0, False)]
    for path, parts in objects:
        relative = os.path.relpath(path, os.path.join(ROOT, importer)).replace('\\', '/')
        lines.append('object %s' % relative)
        seen = []
        for combo in keys_of(path):
            if combo[-1] not in reserved and combo not in seen:
                seen.append(combo)
                lines.append('  key %s' % ' '.join(str(k) for k in combo))
        # One resource per hash and slot; an index buffer lists the ranges
        # its sections match.
        resources = {}
        order = []
        for o in parts:
            key = (o['kind'], o['hash'])
            if key not in resources:
                resources[key] = {'ranges': [], 'vertex_count': None}
                order.append(key)
            r = resources[key]
            if o['kind'] == 'mesh':
                first, count = number(o['first']), number(o['count'])
                if (first, count) not in r['ranges']:
                    r['ranges'].append((first, count))
            if o['vertex_count'] is not None and number(o['vertex_count']):
                r['vertex_count'] = number(o['vertex_count'])
        for kind, value in order:
            r = resources[(kind, value)]
            if kind == 'mesh':
                ranges = r['ranges']
                known = sorted(first for first, count in ranges if first is not None)
                text = []
                for first, count in ranges:
                    if count is None and first is not None:
                        # What the game draws from this first index: up to the
                        # next range a section of the mod knows of.
                        later = [f for f in known if f > first]
                        count = (later[0] - first) if later else 3000
                    text.append('%d:%d' % (-1 if first is None else first, -1 if count is None else count))
                lines.append('  mesh %08x %s%s' % (value, ' '.join(text), ' empty' if value in resized else ''))
            else:
                lines.append('  %s %08x %d' % (kind, value, 1 if value in resized else 0))
    text = '\n'.join(lines) + '\n'
    if out_path:
        with open(out_path, 'w', encoding='utf-8') as f:
            f.write(text)
        print('%s: %d objects, %s -> %s' % (importer, len(objects), kinds, out_path))
    else:
        sys.stdout.write(text)


main()
