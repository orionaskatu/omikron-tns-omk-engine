#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
r"""Split a giant C++ method by MOVING whole statements into a new method.

`todo/code-readability-audit.md` §6 asks for the giant functions to be broken
"along their own banners, nothing reordered", so that every check reading the
viewer's stdout stays byte-identical. Done by hand, each cut means finding
every local the block reads, its exact type and whether the block writes it.
This makes the compiler do that:

    python3 tools/extract_method.py extract <file.cpp> A B <name> [--comment TEXT] [--loop-body] [--returns] [--max-params N]
    python3 tools/extract_method.py batch <file.cpp> <jobs.txt>
    python3 tools/extract_method.py candidates <file.cpp> <method> [--min N] [--depth D]
    python3 tools/extract_method.py sizes [--over N]
    python3 tools/extract_method.py tidy <file.cpp>
    python3 tools/extract_method.py inline <file.cpp> <name>

`extract` moves lines A..B of a method of class C - whole statements at one
depth - verbatim into a new method `C::<name>`, placed after the enclosing one
and declared beside it in C's header. The locals of the enclosing method that
the range uses become REFERENCE parameters, so a write still reaches the
caller: which locals, the compiler says ("use of undeclared identifier",
asked again until none is left, since clang's error recovery hides some
behind others); their types, the compiler says too (a `decltype(x)::probe`
line at A, and `std::is_const` for the constness clang's printing drops on a
class type). libc++'s internal spellings (`std::__map_iterator<...>`) are
mapped to the standard ones, since the consoles compile with libstdc++, and a
const scalar arrives by value. The move is REFUSED, and every file restored,
when a needed local is a lambda (make it a method first), when the range has
a `break`/`continue`/`return` at its own level, or when the moved form does
not compile - a `break` that left an enclosing loop, a `return` of a value.

What makes the move safe rather than merely compiling: the code is the same
text in the same order; a parameter names the very object the local was; and
`-Wshadow` is on at zero warnings (step 4), so no name the block used as a
LOCAL can silently bind to a MEMBER of the same name once it is a method.

A bare `return;` nested in the range is refused too: it left the ENCLOSING
method, and moved it would leave only the new one, which compiles silently.
`--loop-body` is for a range that is exactly a loop's body: a `continue` of
that loop becomes `return;` (the same thing there), and a `break` that would
leave it, or a brace-less inner loop, refuses.

`--returns` moves a range that RETURNS from the enclosing method (a dispatch
arm): the new method returns `bool`, each `return X;` that is not a lambda's
becomes `{ retVal = X; return true; }` with `retVal` an out-parameter, falling
off the end is `false`, and the call is `if (T retVal{}; name(..., retVal))
return retVal;` (for a void method, `if (name(...)) return;`).

A block that needs more than `--max-params` (12) parameters is refused: it
shares too much state with its surroundings to read well alone, and wants a
struct for that state rather than a cut.

`batch` runs `extract` over `start|name|comment` lines, bottom-up so the line
numbers above each cut stay valid, finding each statement's end (an `else`
chain included). `candidates` lists a method's compound statements of at least
N lines at depth D (2 for a body inside the viewer's `do { } while (false)`),
with the banner above each. `sizes` lists the functions over N lines.

PlayState's methods rebuild `session`, `world` and `fs` from the members
rather than taking them as parameters.
"""
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENG = os.path.join(ROOT, "engine")
PLAYSTATE_H = os.path.join(ENG, "backends", "sdl", "playstate.h")
ALIASES = {   # PlayState's usual locals, rebuilt from the members
    "session": "    auto& session = *session_;",
    "world": "    omk::Renderer& world = *world_;",
    "fs": "    const auto& fs = *fs_;",
}


# ---- reading C++ without a parser -------------------------------------------

def code_mask(s):
    """True where s[i] is code: not in a comment, a string or a char literal."""
    m = [True] * len(s)
    i, n = 0, len(s)
    while i < n:
        if s.startswith("//", i):
            j = s.find("\n", i)
            j = n if j < 0 else j
            m[i:j] = [False] * (j - i)
            i = j
        elif s.startswith("/*", i):
            j = s.find("*/", i + 2) + 2
            m[i:j] = [False] * (j - i)
            i = j
        elif s[i] in "\"'":
            q, j = s[i], i + 1
            while s[j] != q:
                j += 2 if s[j] == "\\" else 1
            m[i:j + 1] = [False] * (j + 1 - i)
            i = j + 1
        else:
            i += 1
    return m


def match_close(s, mask, i, o="{", c="}"):
    d = 0
    for j in range(i, len(s)):
        if not mask[j]:
            continue
        if s[j] == o:
            d += 1
        elif s[j] == c:
            d -= 1
            if d == 0:
                return j
    raise ValueError("unbalanced")


def offset_of_line(lines, ln):
    return sum(len(x) + 1 for x in lines[:ln - 1])


def statement_end(s, mask, lines, ln):
    """The last line of the compound statement whose first `{` is on/after
    line `ln`, an `else` chain included."""
    b = offset_of_line(lines, ln)
    while True:                       # the first `{` that opens a block
        b = s.index("{", b)
        j = b - 1
        while s[j].isspace() or not mask[j]:
            j -= 1
        if mask[b] and (s[j] in ");{}" or s[j - 3:j + 1] == "else" or s[j - 1:j + 1] == "do"):
            break
        b = match_close(s, mask, b) + 1 if mask[b] else b + 1
    end = s[:match_close(s, mask, b)].count("\n") + 1
    while lines[end - 1].strip().startswith("}") and "else" in lines[end - 1]:
        b = s.index("{", offset_of_line(lines, end))
        end = s[:match_close(s, mask, b)].count("\n") + 1
    return end


def brace_kinds(text, m):
    """Each code `{` of `text` -> 'loop', 'switch', 'lambda' or 'block', read
    from what precedes it."""
    kinds = {}
    for i, c in enumerate(text):
        if c != "{" or not m[i]:
            continue
        j = i - 1
        while j >= 0 and (text[j].isspace() or not m[j]):
            j -= 1
        kind = "block"
        if j >= 0 and text[j] == ")":
            d, k = 0, j
            while k >= 0:
                if m[k] and text[k] == ")":
                    d += 1
                elif m[k] and text[k] == "(":
                    d -= 1
                    if d == 0:
                        break
                k -= 1
            before = text[max(0, k - 12):k].rstrip()
            if re.search(r"\b(for|while)$", before):
                kind = "loop"
            elif re.search(r"\bswitch$", before):
                kind = "switch"
            elif before.endswith("]") or re.search(r"\bmutable$", before):
                kind = "lambda"
            elif re.search(r"->\s*[\w:<>&* ]+$", text[max(0, k - 60):k]):
                kind = "lambda"
        elif j >= 0 and text[j] == "]":
            kind = "lambda"
        elif text[max(0, j - 1):j + 1] == "do":
            kind = "loop"
        kinds[i] = kind
    return kinds


def flow_owners(text, m):
    """For each `break` / `continue` / `return;` (bare) in `text`: (word, pos,
    the innermost enclosing brace kind that owns it, or None at the range's
    own level - 'loop' owns both, 'switch' a break, 'lambda' a return)."""
    kinds = brace_kinds(text, m)
    out, stack = [], []
    for mm in re.finditer(r"[{}]|\b(break|continue)\s*;|\breturn\s*;", text):
        if not m[mm.start()]:
            continue
        t = mm.group(0)
        if t == "{":
            stack.append(kinds.get(mm.start(), "block"))
        elif t == "}":
            if stack:
                stack.pop()
        else:
            word = t.split()[0].rstrip(";")
            want = {"break": ("loop", "switch", "lambda"), "continue": ("loop", "lambda"),
                    "return": ("lambda",)}[word]
            owner = next((k for k in reversed(stack) if k in want), None)
            out.append((word, mm.start(), owner))
    return out


def _statement_end(text, m, k):
    """The end of the one statement starting at code index k."""
    n = len(text)
    while k < n and (text[k].isspace() or not m[k]):
        k += 1
    if text[k] == "{":
        return match_close(text, m, k)
    head = re.match(r"(if|for|while|switch)\s*\(", text[k:])
    if head:
        d, j = 0, k + head.end() - 1
        while j < n:
            if m[j] and text[j] == "(":
                d += 1
            elif m[j] and text[j] == ")":
                d -= 1
                if d == 0:
                    break
            j += 1
        end = _statement_end(text, m, j + 1)
        if head.group(1) == "if":
            r = end + 1
            while r < n and (text[r].isspace() or not m[r]):
                r += 1
            if text[r:r + 4] == "else" and not (text[r + 4:r + 5].isalnum() or text[r + 4:r + 5] == "_"):
                end = _statement_end(text, m, r + 4)
        return end
    d = 0
    while k < n:
        if m[k] and text[k] in "({[":
            d += 1
        elif m[k] and text[k] in ")}]":
            d -= 1
        elif m[k] and text[k] == ";" and d == 0:
            return k
        k += 1
    return n - 1


def braceless_loops(text, m):
    """The spans of the range's `for`/`while` loops whose body has no braces -
    a `break`/`continue` in one belongs to it, which the braces cannot show."""
    spans = []
    for mm in re.finditer(r"\b(for|while)\s*\(", text):
        if not m[mm.start()]:
            continue
        d, k = 0, mm.end() - 1
        while k < len(text):
            if m[k] and text[k] == "(":
                d += 1
            elif m[k] and text[k] == ")":
                d -= 1
                if d == 0:
                    break
            k += 1
        j = k + 1
        while j < len(text) and (text[j].isspace() or not m[j]):
            j += 1
        if j < len(text) and text[j] not in "{;":
            spans.append((j, _statement_end(text, m, j)))
    return spans


def declared_type(before, bm, x):
    """The type `x` was DECLARED with in `before` (the enclosing method up to
    the range), as written - `std::uint32_t`, not the `unsigned int` clang
    prints for it, which is another type on the consoles' libraries. None
    for an `auto` local, or when no plain declaration is found."""
    best = None
    for mm in re.finditer(r"\b%s\b" % re.escape(x), before):
        if not bm[mm.start()]:
            continue
        after = before[mm.end():mm.end() + 40]
        arr = re.match(r"\s*(\[[^\]]*\])", after)
        rest = after[arr.end():] if arr else after
        if not re.match(r"\s*(=|;|\{|,|\()", rest) or re.match(r"\s*==", rest):
            continue
        st = mm.start()
        while st > 0 and not (bm[st - 1] and before[st - 1] in ";{}"):
            st -= 1
        prefix = "".join(c if bm[k] else " " for k, c in enumerate(before[st:mm.start()], st))
        prefix = re.sub(r"^\s*(?:for\s*\()?", "", prefix)
        one = re.match(r"^(static\s+|constexpr\s+)*(const\s+)?([\w:]+(?:\s*<[^;{}()]*>)?)"
                       r"\s*([*&]*)\s*(const\s*)?$", prefix)
        multi = re.match(r"^(static\s+|constexpr\s+)*(const\s+)?([\w:]+(?:\s*<[^;{}()]*>)?)"
                         r"\s+[^;]*,\s*([*&]*)\s*$", prefix)
        mm2 = one or multi
        if not mm2:
            continue
        base = mm2.group(3).strip()
        if base in ("auto", "return", "else", "case", "goto", "delete", "new", "throw") or \
                base.startswith("decltype"):
            best = None if base == "auto" else best
            continue
        const = bool(mm2.group(2)) or bool(re.search(r"constexpr", mm2.group(1) or ""))
        star = mm2.group(4) or ""
        t = ("const " if const and "*" not in star else "") + base + star
        if "*" in star and const:
            t = "const " + base + star            # a pointer to const
        if arr:
            t += arr.group(1)
        best = t
    return best


# ---- portable parameter types ------------------------------------------------

SCALAR = re.compile(r"^(bool|char|signed char|unsigned char|short|unsigned short|int|"
                    r"unsigned int|long|unsigned long|long long|unsigned long long|"
                    r"float|double|unsigned|std::size_t|std::u?int(8|16|32|64)_t)$")


def _angle(s, at):
    d = 0
    for i in range(at, len(s)):
        if s[i] == "<":
            d += 1
        elif s[i] == ">":
            d -= 1
            if d == 0:
                return s[at + 1:i], i
    raise ValueError(s)


def split_top(s):
    out, d, cur = [], 0, ""
    for c in s:
        d += c in "<("
        d -= c in ">)"
        if c == "," and d == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += c
    out.append(cur.strip())
    return out


def portable(t):
    """libc++'s internal spellings to the standard ones; refused if any remain."""
    for kind, member in (("std::__map_iterator<", "iterator"),
                         ("std::__map_const_iterator<", "const_iterator")):
        while kind in t:
            i = t.index(kind)
            inner, j = _angle(t, i + len(kind) - 1)
            vt = re.search(r"std::__value_type<", inner)
            k, v = split_top(_angle(inner, vt.end() - 1)[0])
            t = t[:i] + "std::map<%s, %s>::%s" % (k, v, member) + t[j + 1:]
    m = re.search(r"std::__wrap_iter<(const )?([^<>]+)\s*\*>", t)
    if m:
        t = t[:m.start()] + "std::vector<%s>::%s" % (
            m.group(2).strip(), "const_iterator" if m.group(1) else "iterator") + t[m.end():]
    if "std::__" in t:
        raise ValueError("a libc++ internal type: " + t)
    return t


def param(t, x, const):
    """The parameter for local `x` of type `t`; `const` when the variable
    itself is (a `const char*` is not: its POINTEE is)."""
    t = t.replace(" &", "&").replace(" *", "*").replace("omk::play::", "")
    t = re.sub(r"(?<![\w:])(vector|map|set|unordered_map|unordered_set|string|basic_string|array|optional|"
               r"pair|unique_ptr|shared_ptr|span|function)<", r"std::\1<", t)
    t = re.sub(r"(?<![\w:])string\b", "std::string", t)
    t = re.sub(r"(?:std::)?basic_string<char(?:, std::char_traits<char>, std::allocator<char>)?>",
               "std::string", t)
    t = portable(t)
    arr = re.match(r"(.+)\[([^\]]+)\]$", t)
    if arr:
        return "%s (&%s)[%s]" % (arr.group(1).strip(), x, arr.group(2))
    if t.endswith("&"):
        base = t[:-1].strip()
        if base.startswith("const ") and SCALAR.match(base[6:]):
            return "%s %s" % (base[6:], x)
        return "%s %s" % (t, x)
    if const:
        if t.endswith("*") or t.endswith("*const"):
            return "%s %s" % (t, x)                  # the pointer itself is const
        base = t[6:] if t.startswith("const ") else t
        if SCALAR.match(base):
            return "%s %s" % (base, x)
        return "const %s& %s" % (base, x)
    return "%s& %s" % (t, x)


def wrap(head, items, indent, tail=")"):
    one = head + "(" + ", ".join(items) + tail
    if len(one) + indent <= 100:
        return one
    pad = " " * len(head + "(")
    lines, cur = [], head + "("
    for i, p in enumerate(items):
        piece = p + (", " if i < len(items) - 1 else tail)
        if len(cur) + len(piece) + indent > 100 and cur.strip() != (head + "(").strip():
            lines.append(cur.rstrip())
            cur = pad
        cur += piece
    lines.append(cur)
    return ("\n" + " " * indent).join(lines)


# ---- the build ---------------------------------------------------------------

def object_of(cpp):
    rel = os.path.relpath(os.path.abspath(cpp), ENG)
    if rel.startswith("src/"):
        return "build/obj/" + rel[:-4] + ".o"
    variant = "play-vk" if os.path.isdir(os.path.join(ENG, "build", "obj", "play-vk")) else "play-sw"
    return "build/obj/%s/%s.o" % (variant, os.path.basename(rel)[:-4])


def compile_errors(cpp, warnings=False):
    """The errors of compiling `cpp` alone, with the Makefile's own command."""
    rel = os.path.relpath(os.path.abspath(cpp), ENG)
    obj = object_of(cpp)
    cmd = subprocess.run(["make", "-n", "-B", obj], cwd=ENG, capture_output=True,
                         text=True).stdout
    line = [l for l in cmd.split("\n") if rel in l and " -c " in l]
    if not line:
        sys.exit("no compile command for %s (target %s)" % (rel, obj))
    line = line[0].replace(" -c ", " -fsyntax-only -ferror-limit=0 ").replace("-o " + obj, "")
    r = subprocess.run(line, shell=True, cwd=ENG, capture_output=True, text=True)
    return [l for l in r.stderr.split("\n") if ": error: " in l or (warnings and ": warning: " in l)]


def drop_unused_aliases(path):
    """Remove an alias line (`auto& session = *session_;`...) a cut has left
    unused - the compiler's own `-Wunused-variable` says which."""
    rel = os.path.relpath(os.path.abspath(path), ENG)
    lines = open(path).read().split("\n")
    gone = []
    for w in compile_errors(path, warnings=True):
        u = re.match(r"%s:(\d+):\d+: warning: unused variable '(\w+)'" % re.escape(rel), w)
        if u and u.group(2) in ALIASES and lines[int(u.group(1)) - 1].strip() == ALIASES[u.group(2)].strip():
            gone.append(int(u.group(1)))
    for ln in sorted(set(gone), reverse=True):
        del lines[ln - 1]
    if gone:
        open(path, "w").write("\n".join(lines))
    return len(gone)


def header_of(cls):
    if cls == "PlayState":
        return PLAYSTATE_H
    for h in glob.glob(os.path.join(ENG, "src", "**", "*.h"), recursive=True) + \
            glob.glob(os.path.join(ENG, "backends", "**", "*.h"), recursive=True):
        if "third_party" in h:
            continue
        if re.search(r"^\s*(class|struct) %s\b[^;]*$" % cls, open(h).read(), re.M):
            return h
    sys.exit("no header defines class " + cls)


# ---- extract -----------------------------------------------------------------

def extract(path, A, B, name, comment="", tidy=True, loop_body=False, returns=False, max_params=12):
    src = open(path).read()
    L = src.split("\n")
    body = L[A - 1:B]
    text = "\n".join(body)
    m = code_mask(text)
    depth = 0
    for i, c in enumerate(text):
        if m[i] and c in "{}":
            depth += 1 if c == "{" else -1
            if depth < 0:
                return "refused: the range closes a block it did not open"
    if depth:
        return "refused: the range leaves a block open"
    d = 0
    owned = braceless_loops(text, m)
    for mm in re.finditer(r"[{}]|\b(break|continue|return)\b", text):
        if not m[mm.start()]:
            continue
        t = mm.group(0)
        if t in "{}":
            d += 1 if t == "{" else -1
        elif t != "return" and any(a <= mm.start() <= z for a, z in owned):
            continue                         # a brace-less inner loop's own
        elif d == 0 and not (loop_body and t == "continue") and not (returns and t == "return"):
            return "refused: a top-level `%s` in the range" % t
    # A BARE `return;` nested in the range left the ENCLOSING method; moved, it
    # would leave only the new one and the caller would carry on - silently,
    # since a void method compiles it either way. (`return value;` cannot
    # slip: the new method is void.) `continue`/`break` that leave the range
    # do not compile in it, except under --loop-body, handled here.
    to_return = []
    bl = braceless_loops(text, m)
    for word, pos, owner in flow_owners(text, m):
        if owner or (word != "return" and any(a <= pos <= z for a, z in bl)):
            continue
        if word == "return":
            if returns:
                continue                     # rewritten below
            return "refused: a bare `return;` would leave only the new method (--returns)"
        if word == "continue" and loop_body:
            to_return.append(pos)
        elif loop_body:
            return "refused: a `%s` leaves the loop whose body this is" % word
    spans = braceless_loops(text, m)
    to_return = [p for p in to_return if not any(a <= p <= z for a, z in spans)]
    if to_return:
        # `continue;` -> `return;`: the range IS the loop's whole body, so
        # leaving the body early is leaving this call
        for pos in sorted(to_return, reverse=True):
            end = text.index(";", pos) + 1
            text = text[:pos] + "return;" + text[end:]
        body = text.split("\n")
        m = code_mask(text)
    ind = len(body[0]) - len(body[0].lstrip(" "))
    # THE ENCLOSING FUNCTION: the column-0 definition whose body holds the
    # range - a method `Cls::name(` or, in free mode, a function `name(`
    fstart = None
    whole = "\n".join(L)
    wm = code_mask(whole)
    for i in range(A - 2, -1, -1):
        if not re.match(r"^(?!namespace\b|struct\b|class\b|enum\b|union\b|template\b|"
                        r"static_assert|using\b|#|//|\})[A-Za-z_][^;]*\b~?\w+\(", L[i]):
            continue
        b0 = whole.find("{", offset_of_line(L, i + 1))
        try:
            e0 = match_close(whole, wm, b0)
        except ValueError:
            continue
        if whole[:b0].count("\n") + 1 < A and whole[:e0].count("\n") + 1 > B:
            fstart = i
            break
    if fstart is None:
        return "refused: no enclosing function"
    head = re.match(r"^\S[^;]*?\b(\w+)::(~?\w+)\(", L[fstart])
    if head:
        cls, encl = head.groups()
    else:
        cls, encl = None, re.match(r"^\S[^;(]*?\b(\w+)\(", L[fstart]).group(1)
    cut = L[fstart].index(cls + "::") if cls else L[fstart].index(encl + "(")
    rtype = re.sub(r"^(static|inline|virtual|constexpr)\s+", "", L[fstart][:cut].strip())
    if returns:
        # every `return` of the range that is not a lambda's: it left the
        # enclosing method, so it now says so - `{ retVal = X; return true; }`
        # - and the caller returns retVal; falling off the end is `false`
        if "&" in rtype:
            return "refused: --returns with a reference return type (%s)" % rtype
        if re.search(r"\bretVal\b", text):
            return "refused: the range already names `retVal`"
        kinds = brace_kinds(text, m)
        lam = []
        for b0, k in kinds.items():
            if k == "lambda":
                lam.append((b0, match_close(text, m, b0)))
        edits = []
        for rm in re.finditer(r"\breturn\b", text):
            if not m[rm.start()] or any(a < rm.start() < z for a, z in lam):
                continue
            d2, k = 0, rm.end()
            while k < len(text):
                if m[k] and text[k] in "([{":
                    d2 += 1
                elif m[k] and text[k] in ")]}":
                    d2 -= 1
                elif m[k] and text[k] == ";" and d2 == 0:
                    break
                k += 1
            expr = text[rm.end():k].strip()
            if rtype == "void":
                rep = "return true;" if not expr else "{ %s; return true; }" % expr
            else:
                rep = "{ retVal = %s; return true; }" % expr
            edits.append((rm.start(), k + 1, rep))
        if not edits:
            return "refused: --returns, but the range has no return"
        for a0, z0, rep in sorted(edits, reverse=True):
            text = text[:a0] + rep + text[z0:]
        body = text.split("\n") + [" " * ind + "return false;"]
        text = "\n".join(body)
        m = code_mask(text)
    sig = "\n".join(L[fstart:A])
    is_const = bool(re.search(r"\)\s*const\s*\{", sig[:sig.index("{") + 1]))
    hdr_path = header_of(cls) if cls else path
    hdr = open(hdr_path).read()
    newbody = [l[ind - 4:] if l.startswith(" " * ind) else l for l in body]

    def restore():
        open(path, "w").write(src)
        open(hdr_path, "w").write(hdr)

    retp = ["%s& retVal" % rtype] if returns and rtype != "void" else []

    def write(params, args, aliases):
        if not returns:
            call = [" " * ind + wrap(name, list(args), ind) + ";"]
        elif rtype == "void":
            call = [" " * ind + "if (" + wrap(name, list(args), ind + 4) + ") return;"]
        else:
            call = [" " * ind + "if (%s retVal{}; " % rtype + wrap(name, list(args) + ["retVal"], ind)
                    + ") return retVal;"]
        out = L[:A - 1] + call + L[B:]
        if cls is None:
            fn = (["// " + c for c in comment.split("\n")] if comment else []) + \
                [wrap("static %s %s" % ("bool" if returns else "void", name), params + retp, 0, ") {")] + \
                newbody + ["}", ""]
            top = fstart                          # above its comment block
            while top > 0 and L[top - 1].lstrip().startswith("//"):
                top -= 1
            out = out[:top] + fn + out[top:]
            open(path, "w").write("\n".join(out))
            return
        fn = (["// " + c for c in comment.split("\n")] if comment else []) + \
            [wrap("%s %s::%s" % ("bool" if returns else "void", cls, name), params + retp, 0,
                  ") const {" if is_const else ") {")] + \
            aliases + newbody + ["}", ""]
        s2 = "\n".join(out)
        mk = code_mask(s2)
        b = s2.index("{", offset_of_line(out, fstart + 1))
        endline = s2[:match_close(s2, mk, b)].count("\n") + 1
        out = out[:endline] + [""] + fn + out[endline:]
        open(path, "w").write("\n".join(out))
        mm = re.search(r"^( *)[^\n;]*\b%s\([^;]*\)( const)?;[^\n]*\n" % re.escape(encl), hdr, re.M)
        if not mm:
            raise RuntimeError("no declaration of %s in %s" % (encl, hdr_path))
        decl = mm.group(1) + wrap(("bool " if returns else "void ") + name, params + retp, len(mm.group(1)),
                                  ") const;" if is_const else ");") + \
            "   // from `%s`\n" % encl
        open(hdr_path, "w").write(hdr[:mm.end()] + decl + hdr[mm.end():])

    def typed(need):
        if not need:
            return []
        probe = L[:A - 1] + [" " * ind + "decltype(%s)::omkprobe_; static_assert(!std::is_const_v<"
                             "std::remove_reference_t<decltype(%s)>>, \"omkconst:%s\");" % (x, x, x)
                             for x in need] + L[A - 1:]
        open(path, "w").write("\n".join(probe))
        perrs = compile_errors(path)
        restore()
        types, const = {}, set()
        for e in perrs:
            c = re.search(r"omkconst:(\w+)", e)
            if c:
                const.add(c.group(1))
                continue
            a = re.search(r"'decltype\((\w+)\)' \(aka '(.+)'\) is not a class", e)
            if a:
                types[a.group(1)] = a.group(2)
                continue
            n = re.search(r"no member named 'omkprobe_' in '(.+)'", e)
            if n:
                ln = int(re.search(r":(\d+):\d+: error", e).group(1))
                x = re.search(r"decltype\((\w+)\)::omkprobe_", probe[ln - 1]).group(1)
                types[x] = n.group(1)
        out = []
        before = "\n".join(L[fstart:A - 1])
        bm = code_mask(before)
        for x in list(need):
            t = types.get(x)
            if t is not None and "lambda at" not in t:
                d = declared_type(before, bm, x)
                if d:
                    t = d                          # the source's own spelling
                elif re.search(r"\b(unsigned|long|short|int|char)\b", t) and \
                        x not in const and not t.endswith("*"):
                    # an `auto` local of a platform-sized integer, by reference:
                    # clang's spelling is this host's, not the consoles'
                    raise ValueError("%s is an `auto` local of %s - write its type out first" % (x, t))
            if t is None and x in forced:
                # the guard's regex saw a declaration in another, closed scope:
                # no local of that name is visible here, so the range's use
                # means the member, as it did in place
                need.remove(x)
                missing.remove(x)
                continue
            if t is None:
                raise ValueError("no type for " + x)
            if "lambda at" in t:
                raise ValueError("%s is a lambda - make it a method first" % x)
            for a, b in renames.items():
                t = re.sub(r"(?<![\w:])%s\b" % re.escape(a), b, t)
            out.append(param(t, x, x in const))
        return out

    # A local that shares a member FUNCTION's name is not caught by -Wshadow:
    # moved, `name(...)` would call the method. Declared before the range and
    # used in it, it is passed like any other local.
    members = set() if cls is None else set(re.findall(r"^\s+(?:[\w:<>,]+[\s&*]+)+(\w+)\([^;{]*\)\s*(?:const\s*)?(?:override\s*)?;",
                             hdr, re.M))
    used = set(re.findall(r"\b([A-Za-z_]\w*)\b", "".join(c for c, k in zip(text, m) if k)))
    before = "\n".join(L[fstart:A - 1])
    missing = [n for n in sorted(members & used)
               if re.search(r"(?:\bauto|[\w>\]])\s*[&*]?\s+(?<![:.>])%s\s*(?:=|\{|;)" % re.escape(n),
                            before)]
    forced = set(missing)
    renames = {}   # an unqualified type clang printed -> the name it suggests
    for _ in range(10):
        aliases = [ALIASES[x] for x in ALIASES if x in missing and cls == "PlayState"]
        need = [x for x in missing if not (cls == "PlayState" and x in ALIASES)]
        try:
            params = typed(need)
        except ValueError as e:
            restore()
            return "refused: %s" % e
        if len(params) > max_params:
            restore()
            return "refused: %d parameters (over %d) - the block shares too much state to stand alone" % (
                len(params), max_params)
        write(params, need, aliases)
        errs = compile_errors(path)
        if not errs:
            tidied = drop_unused_aliases(path) if cls == "PlayState" and tidy else 0
            return "%s: %d lines, %d parameters %s%s" % (
                name, B - A + 1, len(params), need,
                ", %d unused alias(es) dropped" % tidied if tidied else "")
        new = []
        for e in errs:
            q = re.search(r"unknown type name '(\w+)'; did you mean '([\w:]+)'", e)
            if q and q.group(1) not in renames:
                renames[q.group(1)] = q.group(2)
                new.append(None)
            u = re.search(r"use of undeclared identifier '(\w+)'", e)
            if u and u.group(1) not in missing and u.group(1) not in new:
                new.append(u.group(1))
            f = re.search(r":(\d+):(\d+): error: reference to non-static member function", e)
            if f and e.startswith(os.path.relpath(os.path.abspath(path), ENG)):
                ln, col = int(f.group(1)), int(f.group(2))
                tok = re.match(r"\w+", open(path).read().split("\n")[ln - 1][col - 1:])
                if tok and tok.group(0) not in missing and tok.group(0) not in new:
                    new.append(tok.group(0))
        restore()
        if not new:
            return "refused: the moved form does not compile:\n  " + "\n  ".join(errs[:6])
        missing += [n for n in new if n]
    restore()
    return "refused: did not converge"


def inline(path, name):
    """Undo an extraction: method `name`'s body back at its one call site,
    re-indented, its alias lines dropped; the definition and the declaration
    removed. For a cut that read worse than what it replaced."""
    s = open(path).read()
    m = code_mask(s)
    dm = re.search(r"^(?:static )?void (?:(\w+)::)?%s\(" % re.escape(name), s, re.M)
    if not dm:
        return "no void function %s in %s" % (name, path)
    b = s.index("{", dm.end())
    e = match_close(s, m, b)
    c0 = dm.start()
    while True:                              # its comment block too
        p = s.rfind("\n", 0, c0 - 1) + 1
        if p < c0 and s[p:c0].startswith("//"):
            c0 = p
        else:
            break
    if s[c0 - 2:c0] == "\n\n":              # the blank line `extract` put before it
        c0 -= 1
    aliases = {a.strip() for a in ALIASES.values()}
    body = [l for l in s[b + 1:e].strip("\n").split("\n") if l.strip() not in aliases]
    dend = e + 1
    while s[dend:dend + 1] == "\n" and dend - e < 3:
        dend += 1
    s2 = s[:c0] + s[dend:]
    cm = re.search(r"^( *)%s\([^;]*\);\n" % re.escape(name), s2, re.M)
    if not cm:
        return "no plain call of %s (a --returns cut is inlined by hand)" % name
    ind = len(cm.group(1))
    inl = "\n".join((" " * (ind - 4) + l) if l.strip() else l for l in body) + "\n"
    open(path, "w").write(s2[:cm.start()] + inl + s2[cm.end():])
    if not dm.group(1):
        return "inlined %s: %d lines" % (name, len(body))
    hdr_path = header_of(dm.group(1))
    t = open(hdr_path).read()
    d2 = re.search(r"^ *void %s\([^;]*\)( const)?;[^\n]*\n" % re.escape(name), t, re.M)
    open(hdr_path, "w").write(t[:d2.start()] + t[d2.end():])
    return "inlined %s: %d lines" % (name, len(body))


def batch(path, jobs_file):
    jobs = []
    for line in open(jobs_file):
        if line.strip() and not line.startswith("#"):
            ln, name, comment = line.rstrip("\n").split("|", 2)
            jobs.append((int(ln), name, comment.replace("\\n", "\n")))
    # each job's start line by its TEXT and which occurrence of it: a free
    # function's piece goes ABOVE it, so line numbers do not survive a cut
    L0 = open(path).read().split("\n")
    keyed = []
    for ln, name, comment in jobs:
        text = L0[ln - 1]
        keyed.append((ln, text, sum(1 for l in L0[:ln - 1] if l == text), name, comment))
    for _, text, nth, name, comment in sorted(keyed, reverse=True):
        s = open(path).read()
        L = s.split("\n")
        ln = [i + 1 for i, l in enumerate(L) if l == text][nth]
        end = statement_end(s, code_mask(s), L, ln)
        # a tidy deletes lines ABOVE the cuts still to come: once, at the end
        print("%d-%d %s" % (ln, end, extract(path, ln, end, name, comment, tidy=False)))
    n = drop_unused_aliases(path)
    if n:
        print("%d unused alias(es) dropped" % n)


def candidates(path, method, minl, depth_want):
    s = open(path).read()
    m = code_mask(s)
    L = s.split("\n")
    mm = re.search(r"^\S[^\n;]*\b%s\(" % re.escape(method), s, re.M)
    b = s.index("{", mm.end())
    e = match_close(s, m, b)
    fstart, fend = s[:b].count("\n") + 1, s[:e].count("\n") + 1
    print("%s: %d-%d (%d lines)" % (method, fstart, fend, fend - fstart + 1))
    def opens_block(k):
        j = k - 1
        while j > b and (s[j].isspace() or not m[j]):
            j -= 1
        return s[j] in ");{}" or s[j - 3:j + 1] == "else" or s[j - 1:j + 1] == "do"
    depth, k = 1, b + 1
    while k < e:
        if not m[k] or s[k] not in "{}":
            k += 1
            continue
        if s[k] == "}":
            depth -= 1
            k += 1
            continue
        if not opens_block(k):               # an initialiser's braces, not a block
            k = match_close(s, m, k) + 1
            continue
        if depth != depth_want:
            depth += 1
            k += 1
            continue
        ln = st = s[:k].count("\n") + 1
        while st > fstart + 1:
            prev = L[st - 2].split("//")[0].rstrip()
            if prev.endswith((";", "{", "}")) or not prev:
                break
            st -= 1
        end = statement_end(s, m, L, ln)
        if end - st + 1 >= minl:
            c, com = st - 1, ""
            while c > fstart and L[c - 1].strip().startswith("//"):
                t = L[c - 1].strip()
                if "----" in t or re.match(r"//\s*[A-Z`'\"(]", t):
                    com = t
                c -= 1
            print("  %d-%d (%d) %r  | %s" % (st, end, end - st + 1, L[st - 1].strip()[:60], com[:90]))
        k = offset_of_line(L, end + 1)


def sizes(over):
    res = []
    files = [f for pat in ("src/**/*.cpp", "src/**/*.h", "backends/**/*.cpp", "backends/**/*.h")
             for f in glob.glob(os.path.join(ENG, pat), recursive=True) if "third_party" not in f]
    for f in sorted(files):
        s = open(f, errors="replace").read()
        m = code_mask(s)
        for mm in re.finditer(r"^(?:[\w:<>,&*~ ]+?)\b([\w:~]+)\(([^;{}]*)\)\s*(?:const\s*)?"
                              r"(?:noexcept\s*)?(?:override\s*)?\{", s, re.M | re.S):
            if not m[mm.start()] or mm.group(1) in ("if", "for", "while", "switch", "catch"):
                continue
            b = s.index("{", mm.end() - 1)
            try:
                n = s[b:match_close(s, m, b)].count("\n") + 1
            except ValueError:
                continue
            res.append((n, os.path.relpath(f, ENG), s[:mm.start()].count("\n") + 1, mm.group(1)))
    res.sort(reverse=True)
    big = [r for r in res if r[0] > over]
    print("%d functions over %d lines" % (len(big), over))
    for n, f, ln, name in big:
        print("%5d %s  %s:%d" % (n, name, f, ln))


def main(argv):
    def opt(flag, default):
        return argv[argv.index(flag) + 1] if flag in argv else default
    if len(argv) >= 5 and argv[0] == "extract":
        r = extract(argv[1], int(argv[2]), int(argv[3]), argv[4], opt("--comment", ""),
                    loop_body="--loop-body" in argv, returns="--returns" in argv,
                    max_params=int(opt("--max-params", "12")))
        print(r)
        return 1 if r.startswith("refused") else 0
    if len(argv) == 3 and argv[0] == "batch":
        batch(argv[1], argv[2])
        return 0
    if len(argv) >= 3 and argv[0] == "candidates":
        candidates(argv[1], argv[2], int(opt("--min", "40")), int(opt("--depth", "1")))
        return 0
    if len(argv) == 3 and argv[0] == "inline":
        print(inline(argv[1], argv[2]))
        return 0
    if len(argv) == 2 and argv[0] == "tidy":
        print("%d unused alias(es) dropped" % drop_unused_aliases(argv[1]))
        return 0
    if argv and argv[0] == "sizes":
        sizes(int(opt("--over", "300")))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
