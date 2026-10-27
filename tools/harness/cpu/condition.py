# The condition in the preamble of an ini, worked out the way the DLL does when
# it includes the file (CommandListExpression::parse and static_evaluate in
# DirectX11/CommandList.cpp): the same tokens, operators bound in the same
# passes, float arithmetic. static_condition returns None where the DLL fails
# to parse or to evaluate it then, which also makes it skip the file.
#
# Not followed: the dxgi_format_* and d3d11_bind_* constants, and random(),
# which the DLL evaluates to a different value every time. They give None, as
# do the variables and time that GetIniInt can evaluate (ini_int).
import math
import re
import struct
from fractions import Fraction

OPERATORS = ('===', '!==', '<<', '>>', '==', '!=', '//', '<=', '>=', '&&', '||', '**',
             '(', ')', '!', '~', '&', '|', '^', '*', '/', '%', '+', '-', '<', '>')
FUNCTIONS = ('countbits', 'sin', 'cos', 'tan', 'asin', 'acos', 'atan', 'abs', 'sign', 'ceil', 'floor',
             'trunc', 'round', 'frac', 'sqrt', 'rsqrt', 'exp', 'exp2', 'log', 'log2', 'saturate', 'random')
UNARY = ('!', '~', '-', '+') + FUNCTIONS
# After the unary pass, highest precedence first; True binds right to left
BINARY = ((('**',), True), (('*', '/', '//', '%'), False), (('+', '-'), False), (('<<', '>>'), False),
          (('<', '<=', '>', '>='), False), (('==', '!=', '===', '!=='), False), (('&',), False),
          (('^',), False), (('|',), False), (('&&',), False), (('||',), False))

DECIMAL = re.compile(r'(?:\d+(?:\.\d*)?|\.\d+)(?:e[+-]?\d+)?')
HEX = re.compile(r'0x([0-9a-f]+(?:\.[0-9a-f]*)?|\.[0-9a-f]+)(?:p([+-]?\d+))?')
BINARY_LITERAL = re.compile(r'0b([01]{1,64})(?![01])')
INTEGER = re.compile(r'\s*[+-]?\d+')
SPECIAL = re.compile(r'infinity|inf|nan(?:\([0-9a-z_]*\))?')
IDENTIFIER = re.compile(r'[a-z_][a-z0-9_]*')


class Fail(Exception):
    pass


class Token:
    # An operator not bound to its operands yet
    def __init__(self, text):
        self.text = text


class Operator:
    def __init__(self, text, lhs, rhs):
        self.text, self.lhs, self.rhs = text, lhs, rhs


class Value:
    def __init__(self, value):
        self.value = value


def f32(x):
    if math.isnan(x) or math.isinf(x):
        return x
    try:
        return struct.unpack('<f', struct.pack('<f', x))[0]
    except OverflowError:
        return math.copysign(math.inf, x)


def nearest_f32(q):
    # The float nearest to the exact value q, ties to even, as the C runtime
    # rounds a literal; values past the largest float become infinity
    if q == 0:
        return 0.0
    sign, q = (-1.0 if q < 0 else 1.0), abs(q)
    e = q.numerator.bit_length() - q.denominator.bit_length()
    if q < Fraction(2) ** e:
        e -= 1
    step = Fraction(2) ** max(e - 23, -149)
    n, rest = divmod(q, step)
    if rest * 2 > step or rest * 2 == step and n % 2:
        n += 1
    value = n * step
    return sign * (math.inf if value >= 2 ** 128 else float(value))


def bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def int32(x):
    # cvttss2si: out of range and NaN give 0x80000000
    if math.isnan(x) or x >= 2.0 ** 31 or x < -2.0 ** 31:
        return -2 ** 31
    return int(x)


def wrap32(n):
    n &= 0xffffffff
    return n - 2 ** 32 if n >= 2 ** 31 else n


def uint32(x):
    if math.isnan(x) or abs(x) >= 2.0 ** 63:
        return 0
    return int(x) & 0xffffffff


def divide(a, b):
    if b == 0:
        if a == 0 or math.isnan(a):
            return math.nan
        return math.copysign(math.inf, a) * math.copysign(1, b)
    return a / b


def power(a, b):
    try:
        return math.pow(a, b)
    except OverflowError:
        return math.inf if a > 0 or b != int(b) or int(b) % 2 == 0 else -math.inf
    except ValueError:
        if a == 0 and b < 0:
            odd = b == int(b) and int(b) % 2
            return math.copysign(math.inf, a) if odd else math.inf
        return math.nan


def call(fn, x):
    try:
        return fn(x)
    except (ValueError, OverflowError):
        return math.nan


def finite(fn):
    # A result of 0 keeps the sign of x, as ceil(-0.5) is -0 in C
    def apply(x):
        if math.isnan(x) or math.isinf(x):
            return x
        result = float(fn(x))
        return math.copysign(result, x) if result == 0 else result
    return apply


def c_round(x):
    whole = math.floor(abs(x))
    if abs(x) - whole >= 0.5:
        whole += 1
    return math.copysign(whole, x)


# The min and max macros of windows.h, which DirectX11 builds with
def c_min(a, b):
    return a if a < b else b


def c_max(a, b):
    return a if a > b else b


UNARY_FN = {
    '!': lambda x: 1.0 if x == 0 else 0.0,
    '~': lambda x: float(~int32(x)),
    '+': lambda x: x,
    '-': lambda x: -x,
    'countbits': lambda x: float(bin(uint32(x)).count('1')),
    'sin': lambda x: call(math.sin, x),
    'cos': lambda x: call(math.cos, x),
    'tan': lambda x: call(math.tan, x),
    'asin': lambda x: call(math.asin, x),
    'acos': lambda x: call(math.acos, x),
    'atan': lambda x: call(math.atan, x),
    'abs': abs,
    'sign': lambda x: float((x > 0) - (x < 0)),
    'ceil': finite(math.ceil),
    'floor': finite(math.floor),
    'trunc': finite(math.trunc),
    'round': finite(c_round),
    'frac': lambda x: x - finite(math.floor)(x),
    'sqrt': lambda x: math.nan if x < 0 else math.sqrt(x),
    # 1.0 / sqrt(rhs): the float sqrt, divided in double
    'rsqrt': lambda x: divide(1.0, math.nan if x < 0 else f32(math.sqrt(x))),
    'exp': lambda x: 0.0 if x == -math.inf else math.inf if x > 709 else math.exp(x),
    'exp2': lambda x: power(2.0, x),
    'log': lambda x: -math.inf if x == 0 else math.nan if x < 0 else call(math.log, x),
    'log2': lambda x: -math.inf if x == 0 else math.nan if x < 0 else call(math.log2, x),
    'saturate': lambda x: c_max(0.0, c_min(x, 1.0)),
}

BINARY_FN = {
    '**': power,
    '*': lambda a, b: a * b,
    '/': divide,
    # floor(lhs / rhs): the float quotient, then floor
    '//': lambda a, b: finite(math.floor)(f32(divide(a, b))),
    '%': lambda a, b: call(lambda _: math.fmod(a, b), 0),
    '+': lambda a, b: a + b,
    '-': lambda a, b: a - b,
    '<<': lambda a, b: float(wrap32(int32(a) << (int32(b) & 31))),
    '>>': lambda a, b: float(int32(a) >> (int32(b) & 31)),
    '<': lambda a, b: float(a < b),
    '<=': lambda a, b: float(a <= b),
    '>': lambda a, b: float(a > b),
    '>=': lambda a, b: float(a >= b),
    '==': lambda a, b: float(a == b),
    '!=': lambda a, b: float(a != b),
    '===': lambda a, b: float(bits(a) == bits(b)),
    '!==': lambda a, b: float(bits(a) != bits(b)),
    '&': lambda a, b: float(int32(a) & int32(b)),
    '^': lambda a, b: float(int32(a) ^ int32(b)),
    '|': lambda a, b: float(int32(a) | int32(b)),
    '&&': lambda a, b: float(a != 0 and b != 0),
    '||': lambda a, b: float(a != 0 or b != 0),
}


def number(text, at):
    # ParseFloatToken: binary literals, then what wcstof takes
    if text.startswith('0b', at) and len(text) - at >= 3:
        # Read into a uint64, then static_cast<float>
        m = BINARY_LITERAL.match(text, at)
        if not m:
            raise Fail
        return nearest_f32(Fraction(int(m.group(1), 2))), m.end()
    m = HEX.match(text, at)
    if m:
        whole, _, fraction = m.group(1).partition('.')
        digits = whole + fraction
        exponent = int(m.group(2) or 0) - 4 * len(fraction)
        mantissa = int(digits, 16) if digits else 0
        magnitude = mantissa.bit_length() + exponent
        base = 2
    else:
        m = DECIMAL.match(text, at)
        if not m:
            m = SPECIAL.match(text, at)
            if not m:
                raise Fail
            return float(m.group(0).split('(')[0]), m.end()
        number_part, _, e = m.group(0).partition('e')
        whole, _, fraction = number_part.partition('.')
        digits = (whole + fraction).lstrip('0')
        exponent = int(e or 0) - len(fraction)
        mantissa = int(digits) if digits else 0
        magnitude = (len(digits) + exponent) * 4
        base = 10
    # Far past the range of a float either way, without building the number
    if mantissa == 0:
        value = 0.0
    elif magnitude > 600:
        value = math.inf
    elif magnitude < -600:
        value = 0.0
    else:
        value = nearest_f32(mantissa * Fraction(base) ** exponent)
    # ParseFloatToken turns the ERANGE of wcstof into an infinity: for overflow
    # and for a value that rounds to 0, not for one that stays subnormal
    if mantissa and value == 0:
        value = math.inf
    return value, m.end()


def tokenise(text, known):
    tokens = []
    at = 0
    last_was_operand = False
    while True:
        while at < len(text) and text[at] in ' \t':
            at += 1
        if at == len(text):
            return tokens
        op = next((o for o in OPERATORS if text.startswith(o, at)), None)
        if op is None:
            op = next((f for f in FUNCTIONS if text.startswith(f, at) and text[at + len(f):at + len(f) + 1] == '('), None)
        if op is not None:
            tokens.append(Token(op))
            at += len(op)
            last_was_operand = False
            continue
        if text[at].isdigit() or text[at] == '.':
            value, at = number(text, at)
        else:
            # Variables, slots, IniParams and the like cannot be evaluated
            # when the file is included, and neither can what does not parse
            m = IDENTIFIER.match(text, at)
            if not m:
                raise Fail
            if m.group(0) in known:
                value, at = known[m.group(0)], m.end()
            elif SPECIAL.match(text, at):
                value, at = number(text, at)
            else:
                raise Fail
        if last_was_operand:
            raise Fail
        tokens.append(Value(value))
        last_was_operand = True


def group(tokens):
    stack = [[]]
    for token in tokens:
        if isinstance(token, Token) and token.text == '(':
            stack.append([])
        elif isinstance(token, Token) and token.text == ')':
            if len(stack) == 1:
                raise Fail
            inner = stack.pop()
            stack[-1].append(inner)
        else:
            stack[-1].append(token)
    if len(stack) != 1:
        raise Fail
    return stack[0]


def is_operand(token):
    return isinstance(token, (Value, Operator, list))


def bind(tokens, i, patterns, unary):
    # transform_operators_token: an operator already bound keeps its text and
    # is looked at again, as in the DLL
    token = tokens[i]
    if not isinstance(token, (Token, Operator)) or token.text not in patterns:
        return i
    lhs = tokens[i - 1] if i > 0 and is_operand(tokens[i - 1]) else None
    rhs = tokens[i + 1] if i + 1 < len(tokens) and is_operand(tokens[i + 1]) else None
    if unary:
        if rhs is not None and lhs is None:
            tokens[i:i + 2] = [Operator(token.text, None, rhs)]
    elif lhs is not None and rhs is not None:
        tokens[i - 1:i + 2] = [Operator(token.text, lhs, rhs)]
        return i - 1
    return i


def visit(tokens, patterns, right, unary):
    if not tokens:
        raise Fail
    if right:
        i = len(tokens) - 2
        while i >= (0 if unary else 1):
            i = bind(tokens, i, patterns, unary) - 1
    else:
        i = 1
        while i < len(tokens) - 1:
            i = bind(tokens, i, patterns, unary) + 1


def transform(node, patterns, right, unary):
    if isinstance(node, list):
        for token in node:
            transform(token, patterns, right, unary)
        visit(node, patterns, right, unary)
    elif isinstance(node, Operator):
        for side in (node.lhs, node.rhs):
            if side is not None:
                transform(side, patterns, right, unary)


def finalise(node):
    if isinstance(node, list):
        node = [finalise(token) for token in node]
        if len(node) != 1:
            raise Fail
        return node[0]
    if isinstance(node, Operator):
        return Operator(node.text, finalise(node.lhs) if node.lhs is not None else None, finalise(node.rhs))
    if isinstance(node, Value):
        return node
    raise Fail


def evaluate(node):
    if isinstance(node, Value):
        return node.value
    if node.text == 'random':
        raise Fail
    rhs = evaluate(node.rhs)
    if node.lhs is None:
        return f32(UNARY_FN[node.text](rhs))
    return f32(BINARY_FN[node.text](evaluate(node.lhs), rhs))


def static_value(text, hunting_static):
    # The value of an expression where the DLL works it out on its own, or
    # None. hunting_static: whether G->hunting is still HUNTING_MODE_DISABLED
    # then, which makes hunting 0. frame_analysis is not recognised there.
    known = {'sli': 0.0, 'stereo_active': 0.0, 'stereo_available': 0.0}
    if hunting_static:
        known['hunting'] = 0.0
    try:
        tree = group(tokenise(text.lower(), known))
        transform(tree, UNARY, True, True)
        for patterns, right in BINARY:
            transform(tree, patterns, right, False)
        return evaluate(finalise(tree))
    except (Fail, RecursionError):
        return None


def static_condition(text, hunting_static):
    value = static_value(text, hunting_static)
    return None if value is None else value != 0


def ini_int(text, default, hunting_static):
    # GetIniInt: a whole decimal integer as it is, anything else as an
    # expression, both saturated to an int
    if not text:
        return default
    if INTEGER.fullmatch(text):
        return max(-2 ** 31, min(2 ** 31 - 1, int(text)))
    value = static_value(text, hunting_static)
    if value is None:
        return default
    if math.isnan(value):
        return 0
    if value <= -2.0 ** 31:
        return -2 ** 31
    if value >= 2.0 ** 31:
        return 2 ** 31 - 1
    return int(value)
