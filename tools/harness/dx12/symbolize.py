# symbolize.py d3d11.dll rva [rva ...]: function and line of offsets into a DLL whose PDB is next to it
import ctypes
import ctypes.wintypes as wt
import os
import sys

dbghelp = ctypes.WinDLL('dbghelp.dll')
kernel32 = ctypes.WinDLL('kernel32.dll')
kernel32.GetCurrentProcess.restype = wt.HANDLE
process = kernel32.GetCurrentProcess()


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.ULONG), ('TypeIndex', wt.ULONG), ('Reserved', ctypes.c_uint64 * 2),
                ('Index', wt.ULONG), ('Size', wt.ULONG), ('ModBase', ctypes.c_uint64), ('Flags', wt.ULONG),
                ('Value', ctypes.c_uint64), ('Address', ctypes.c_uint64), ('Register', wt.ULONG),
                ('Scope', wt.ULONG), ('Tag', wt.ULONG), ('NameLen', wt.ULONG), ('MaxNameLen', wt.ULONG),
                ('Name', ctypes.c_char * 1024)]


class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.DWORD), ('Key', ctypes.c_void_p), ('LineNumber', wt.DWORD),
                ('FileName', ctypes.c_char_p), ('Address', ctypes.c_uint64)]


dbghelp.SymSetOptions(0x10 | 0x2)
dbghelp.SymInitialize.argtypes = [wt.HANDLE, ctypes.c_char_p, wt.BOOL]
dbghelp.SymLoadModuleEx.argtypes = [wt.HANDLE, wt.HANDLE, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint64, wt.DWORD,
                                    ctypes.c_void_p, wt.DWORD]
dbghelp.SymLoadModuleEx.restype = ctypes.c_uint64
dbghelp.SymFromAddr.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYMBOL_INFO)]
dbghelp.SymGetLineFromAddr64.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(wt.DWORD), ctypes.POINTER(IMAGEHLP_LINE64)]

path = os.path.abspath(sys.argv[1])
dbghelp.SymInitialize(process, os.path.dirname(path).encode(), False)
base = dbghelp.SymLoadModuleEx(process, None, path.encode(), None, 0x180000000, 0, None, 0)
for arg in sys.argv[2:]:
    address = base + int(arg, 16)
    symbol = SYMBOL_INFO()
    symbol.SizeOfStruct = ctypes.sizeof(SYMBOL_INFO) - 1024
    symbol.MaxNameLen = 1000
    displacement = ctypes.c_uint64()
    line = IMAGEHLP_LINE64()
    line.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINE64)
    line_displacement = wt.DWORD()
    name = symbol.Name.decode() if dbghelp.SymFromAddr(process, address, ctypes.byref(displacement), ctypes.byref(symbol)) else '?'
    where = ''
    if dbghelp.SymGetLineFromAddr64(process, address, ctypes.byref(line_displacement), ctypes.byref(line)):
        where = ' %s:%d' % (os.path.basename(line.FileName.decode()), line.LineNumber)
    print('%s  %s%s' % (arg, name, where))
