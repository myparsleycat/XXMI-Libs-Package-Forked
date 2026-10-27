# setup.py <importer> [--dll path] [--name folder] [--api dx11|dx12|vk]
#          [--set Section.key=value ...]
# Makes %XH_RUNS%\<importer>\<folder> (default "cur"): the deployed
# configuration of the importer as the game has it - Core, Mods and
# ShaderFixes are junctions to the real folders, d3dx.ini and d3dx_user.ini
# copies - with the DLL to test (default: x64\Release of this repository) and
# the harness next to it. Nothing in the deployed folders is written to: the
# DLL writes its settings and ShaderCache next to itself, which is the copy,
# and cache_shaders is off, also against the includes, so no .bin lands
# beside the junctioned shaders.
#
# NEVER delete such a folder with rm -rf: remove it with clean.py, which takes
# the junctions away first.
import os
import re
import shutil
import subprocess
import sys

import ini
from condition import ini_int
from paths import HERE, REPO, RUNS, importers_root


def windows(path):
    return path.replace('/', '\\')


def junction(link, target):
    target = os.path.realpath(target)
    if os.path.lexists(link):
        if os.path.realpath(link) == target:
            return
        os.rmdir(link)
    subprocess.check_call(['cmd', '/c', 'mklink', '/J', windows(link), windows(target)], stdout=subprocess.DEVNULL)


def ini_value(text, section, key):
    # The span of the value the DLL takes: the first one in the first [section]
    for line in ini.lines(text):
        if line.kind == 'value' and line.section.lower() == section and line.key.lower() == key:
            return line.start, line.end
    return None


def set_value(text, section, key, value):
    # Makes the value the DLL takes this one: replaced where it reads it, else
    # added right below the section's header, or in a new section at the end
    found = ini_value(text, section.lower(), key.lower())
    if found:
        return text[:found[0]] + value + text[found[1]:]
    newline = '\r\n' if '\r\n' in text else '\n'
    header = [line.end for line in ini.lines(text)
              if line.kind == 'section' and line.section and line.section.lower() == section.lower()]
    if header:
        return text[:header[0]] + newline + key + ' = ' + value + text[header[0]:]
    if text and not text.endswith('\n'):
        text += newline
    return text + '[' + section + ']' + newline + key + ' = ' + value + newline


PERSIST = re.compile(r'(?im)^[ \t]*(?:(?:global|locked)[ \t]+)*persist[ \t]+(?:(?:global|locked)[ \t]+)*\$')


def caching_stays_off(text):
    # The cache_shaders = 0 of the run's user config lasts until the DLL
    # rewrites that file, which it does at a config reload if a persist
    # variable changed. The harness never reloads, but the DLL does once on its
    # own (config_initialization_delay >= 0), and by then d3dx.ini has run, the
    # includes too with skip_early_includes_load = 0. Whether a mod changes a
    # persist variable is not looked into: that case is refused as a whole.
    found = ini_value(text, 'system', 'config_initialization_delay')
    if ini_int(text[found[0]:found[1]] if found else None, 0, True) < 0:
        return True
    found = ini_value(text, 'system', 'skip_early_includes_load')
    if found and text[found[0]:found[1]].strip(' \t').lower() not in ('1', 'true', 'yes', 'on'):
        return False
    return not PERSIST.search(text)


def main():
    importer = sys.argv[1]
    dll = os.path.join(REPO, 'x64', 'Release', 'd3d11.dll')
    name = 'cur'
    api = 'dx11'
    if '--dll' in sys.argv:
        dll = sys.argv[sys.argv.index('--dll') + 1]
    if '--name' in sys.argv:
        name = sys.argv[sys.argv.index('--name') + 1]
    if '--api' in sys.argv:
        api = sys.argv[sys.argv.index('--api') + 1]
    sets = [sys.argv[i + 1] for i, arg in enumerate(sys.argv[:-1]) if arg == '--set']
    source = os.path.join(importers_root(), importer)
    run = os.path.join(RUNS, importer, name)
    # A wait at startup (WWMI has half a second) is
    # nothing a frame's work depends on, and the clock of a check does not
    # advance while the DLL sleeps on it.
    with open(os.path.join(source, 'd3dx.ini'), encoding='utf-8', errors='surrogateescape', newline='') as f:
        text = f.read()
    text = re.sub(r'(?im)^(\s*dll_initialization_delay\s*=\s*)\d+', r'\g<1>0', text)
    # cache_shaders writes the .bin of a custom shader next to its HLSL, which
    # is in the junctioned Mods or Core, and replaced shaders into ShaderFixes.
    # Caches already there are still read.
    text = re.sub(r'(?im)^([ \t]*cache_shaders[ \t]*=)[^\r\n]*', r'\g<1> 0', text)
    text = set_value(text, 'Rendering', 'cache_shaders', '0')
    for setting in sets:
        m = re.match(r'^([^.=]+)\.([^=]+)=(.*)$', setting)
        if not m:
            raise SystemExit('not Section.key=value: %s' % setting)
        text = set_value(text, m.group(1).strip(), m.group(2).strip(), m.group(3).strip())
    if not caching_stays_off(text):
        raise SystemExit('%s: persist variables may change before the delayed config reload, which then '
                         'rewrites d3dx_user.ini without the cache_shaders = 0 the harness puts there, and an '
                         'include could write shader caches into the deployed folders. Needs '
                         'config_initialization_delay < 0, or skip_early_includes_load = 1 and no persist '
                         'variable in d3dx.ini.' % importer)
    os.makedirs(run, exist_ok=True)

    for folder in ('Core', 'Mods', 'ShaderFixes'):
        if os.path.exists(os.path.join(source, folder)):
            junction(os.path.join(run, folder), os.path.join(source, folder))
    shutil.copy2(os.path.join(source, 'd3dcompiler_47.dll'), os.path.join(run, 'd3dcompiler_47.dll'))
    # What the game left of the user's settings is where every run starts from.
    # [Include] user_config may name a file in the deployed folders (an
    # absolute path, or one through a junction), which the DLL would autosave
    # into: the run's own d3dx_user.ini is read and written instead.
    user = os.path.join(source, 'd3dx_user.ini')
    found = ini_value(text, 'include', 'user_config')
    if found:
        user = ini.user_config_path(source, text[found[0]:found[1]])
        text = text[:found[0]] + 'd3dx_user.ini' + text[found[1]:]
    # A check compares frames call for call. With shader_regex_background = 1
    # a ShaderRegex takes effect whenever its background job ends, which
    # differs from run to run, so xh.sh check uses 2: the draw waits for it.
    variants = {'d3dx.ini': text, 'd3dx.ini.game': text,
                'd3dx.ini.check': re.sub(r'(?im)^(\s*shader_regex_background\s*=\s*)1\b', r'\g<1>2', text)}
    for file, content in variants.items():
        with open(os.path.join(run, file), 'w', encoding='utf-8', errors='surrogateescape', newline='') as f:
            f.write(content)
    # An included ini may set cache_shaders in [Rendering] as well, and wins
    # over d3dx.ini. The user config is parsed after every include, so it turns
    # caching off again (caching_stays_off says for how long).
    settings = ''
    if os.path.isfile(user):
        with open(user, encoding='utf-8', errors='surrogateescape', newline='') as f:
            settings = f.read()
    with open(os.path.join(run, 'd3dx_user.ini.start'), 'w', encoding='utf-8', errors='surrogateescape', newline='') as f:
        f.write(set_value(settings, 'Rendering', 'cache_shaders', '0'))
    shutil.copy2(dll, os.path.join(run, 'd3d11.dll'))
    pdb = os.path.splitext(dll)[0] + '.pdb'
    if os.path.exists(pdb):
        shutil.copy2(pdb, os.path.join(run, 'd3d11.pdb'))
    for built in ('harness.exe', 'harness.pdb'):
        path = os.path.join(HERE, api, 'bin', built)
        if os.path.exists(path):
            shutil.copy2(path, os.path.join(run, api + '_' + built))
    for extra in ('dxcompiler.dll', 'dxil.dll', 'SPIRV-Tools-shared.dll'):
        path = os.path.join(REPO, 'Dependencies', extra)
        if os.path.exists(path):
            shutil.copy2(path, os.path.join(run, extra))
    print('%s -> %s' % (dll, run))


main()
