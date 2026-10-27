import os

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
# The DLL keeps paths in MAX_PATH buffers, and some mods are deep: the folder
# the runs are in must not be longer than where the importers really are.
RUNS = os.environ.get('XH_RUNS') or os.path.join(os.path.expanduser('~'), '.xh')


def importers_root():
    # The folder that holds one folder per importer (GIMI, SRMI, ...), each with
    # its d3dx.ini, Core, Mods and ShaderFixes as XXMI deploys them
    root = os.environ.get('XXMI_ROOT')
    if not root or not os.path.isdir(root):
        raise SystemExit('set XXMI_ROOT to the folder that holds the importer folders (GIMI, SRMI, ZZMI, WWMI, EFMI)')
    return root
