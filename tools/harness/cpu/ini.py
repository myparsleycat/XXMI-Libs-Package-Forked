# Ini files as the DLL reads them (ParseIniStream and ParseIniSectionLine in
# DirectX11/IniHandler.cpp), for setup.py and scene.py.
import collections
import os

# Sections whose names are prefixes (SectionPrefix). In an included file these
# are namespaced, so the DLL drops a repeat of one in the same file as it does
# any repeated section of d3dx.ini. The other sections are global: repeats of
# them in included files add to them, and their values override earlier ones.
PREFIXES = ('textureoverride', 'commandlist', 'shaderoverride', 'customshader', 'shaderregex',
            'builtincommandlist', 'builtincustomshader', 'pool', 'resource', 'key', 'include', 'preset')
# Sections that keep their lines without '=' (DoesSectionAllowLinesWithoutEquals)
LINE_PREFIXES = PREFIXES[:7]
LINE_SECTIONS = ('constants', 'present', 'clearrendertargetview', 'cleardepthstencilview',
                 'clearunorderedaccessviewuint', 'clearunorderedaccessviewfloat', 'profile')

ASCII_LOWER = str.maketrans('ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz')


def fold(text):
    # The order of _wcsicmp, which the DLL sorts section and file names by,
    # under the en_US.UTF-8 locale LoadConfigFile sets: ASCII and U+0100 up
    # lowercased (Python's case mapping stands in for the one of LCMapString),
    # U+0080 to U+00FF left as they are, UTF-16 code units compared
    if text.isascii():
        return text.translate(ASCII_LOWER).encode('utf-16-be')
    out = []
    for c in text:
        if c < '\x80':
            c = c.translate(ASCII_LOWER)
        elif c >= '\u0100' and c.isupper() and len(c.lower()) == 1:
            c = c.lower()
        out.append(c)
    return ''.join(out).encode('utf-16-be', 'surrogatepass')


def msvc_utf8(data):
    # codecvt_utf8<wchar_t>: overlong forms and surrogates are taken, a code
    # point past U+FFFF keeps its low 16 bits, and the text ends before the
    # first sequence it rejects or that the file cuts off
    out = []
    at = 0
    while at < len(data):
        try:
            text = data[at:].decode('utf-8', 'surrogatepass')
        except UnicodeDecodeError as e:
            out.append(data[at:at + e.start].decode('utf-8', 'surrogatepass'))
            at += e.start
        else:
            out.append(text)
            break
        lead = data[at]
        if lead < 0xc0:
            break
        extra = 1 if lead < 0xe0 else 2 if lead < 0xf0 else 3 if lead < 0xf8 else 4 if lead < 0xfc else 5
        code = lead & (0x1f, 0x0f, 0x07, 0x03, 0x03)[extra - 1]
        tail = data[at + 1:at + 1 + extra]
        if len(tail) < extra or any(b < 0x80 or b >= 0xc0 for b in tail):
            break
        for b in tail:
            code = code << 6 | (b & 0x3f)
        if code > 0x10ffff:
            break
        out.append(chr(code))
        at += 1 + extra
    text = ''.join(out)
    if not text.isascii() and max(text) > '\uffff':
        text = ''.join(chr(ord(c) & 0xffff) for c in text)
    return text


def read_text(path):
    # The text of an ini as the wifstream of the DLL reads it, or None when it
    # does not open: text mode first (\r\n read as \n, Ctrl-Z ends the file),
    # then codecvt_utf8 past a leading BOM
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except OSError:
        return None
    data = data.split(b'\x1a', 1)[0].replace(b'\r\n', b'\n')
    text = msvc_utf8(data)
    return text[1:] if text.startswith('\ufeff') else text


def prefixed(name, prefixes):
    return name.translate(ASCII_LOWER).startswith(prefixes)


Line = collections.namedtuple('Line', 'kind section key value start end text')


def lines(text, included=False):
    # The lines the DLL keeps, in order, as Line: kind 'preamble' (section
    # None) or 'value' with key and value as written, start and end spanning
    # the value; 'section' with the name as written, or None for a repeat
    # the DLL drops along with its lines, start and end spanning the line;
    # 'line' for a line without '=' that its section keeps, spanning the line.
    # text is the line without the blanks around it.
    # included: an included file or the user config, else d3dx.ini.
    # text may still have the \r\n and BOM of the file (setup.py edits it).
    seen = set()
    section = None
    preamble = True
    keeps_lines = False
    at = 1 if text.startswith('\ufeff') else 0
    while at < len(text):
        end = text.find('\n', at)
        if end < 0:
            end = len(text)
        start, at = at, end + 1
        if text[end - 1:end] == '\r' and end < len(text):
            end -= 1
        line_end = end
        while start < end and text[start] in ' \t':
            start += 1
        while end > start and text[end - 1] in ' \t':
            end -= 1
        if start == end or text[start] == ';':
            continue
        if text[start] == '[':
            preamble = False
            close = text.find(']', start, end)
            name = text[start + 1:close if close >= 0 else end].strip(' \t')
            if not included or prefixed(name, PREFIXES):
                key = fold(name)
                section = None if key in seen else name
                seen.add(key)
            else:
                section = name
            keeps_lines = section is not None and (prefixed(name, LINE_PREFIXES) or
                                                   name.translate(ASCII_LOWER) in LINE_SECTIONS)
            yield Line('section', section, None, None, start, line_end, text[start:end])
            continue
        if not preamble and section is None:
            continue
        delim = text.find('=', start, end)
        if delim < 0:
            if keeps_lines:
                yield Line('line', section, None, None, start, end, text[start:end])
            continue
        first = delim + 1
        while first < end and text[first] in ' \t':
            first += 1
        key = text[start:delim].rstrip(' \t')
        if preamble:
            yield Line('preamble', None, key, text[first:end], first, end, text[start:end])
        elif first < end:
            yield Line('value', section, key, text[first:end], first, end, text[start:end])


def user_config_path(top, value):
    # [Include] user_config, relative to the importer folder top unless it
    # starts with a drive or a backslash; UNC (\\server\share) and \\?\
    # paths are complete as they are
    if value[1:2] == ':' or value.startswith('\\\\'):
        return value
    if value.startswith('\\'):
        return os.path.splitdrive(top)[0] + value
    return os.path.normpath(top + '\\' + value)
