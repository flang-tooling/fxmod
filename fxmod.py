#!/usr/bin/env python3
"""
fxmod -- read gfortran .mod files and present them to Flang.

A gfortran module is a gzip-compressed S-expression dump; a Flang module is
Fortran source text behind a small checksummed header.  This translates the
former into the latter, so Flang (and flang-tidy) can resolve USE statements
against modules built by gfortran.

Scope: semantic interoperability only.  This does not make the two compilers
link-compatible, and is not meant to.

Subcommands
    dump      parse a gfortran .mod and print the S-expression tree
    info      summarise a gfortran .mod (version, module, public symbols)
    convert   translate a gfortran .mod into a Flang .mod
    wrap      run a command with gfortran modules transparently converted

Wrapper use:

    fxmod.py wrap -- flang -fsyntax-only -I/path/with/gfortran/mods foo.f90

which scans the module search paths in the command line, converts any gfortran
modules found into a cache directory, prepends that directory as -I, and execs
the real command.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import os
import re
import shutil
import sys
import textwrap
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterator, Sequence

# ---------------------------------------------------------------------------
# Flang module header
# ---------------------------------------------------------------------------

BOM = b"\xef\xbb\xbf"
MAGIC = b"!mod$ v1 sum:"
SUM_LEN = 16

# FNV-1a 64-bit, matching ComputeCheckSum() in flang/lib/Semantics/mod-file.cpp
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = 0xFFFFFFFFFFFFFFFF


def flang_checksum(body: bytes) -> str:
    """Checksum of a Flang module body, formatted as Flang writes it."""
    h = FNV_OFFSET
    for byte in body:
        h ^= byte & 0xFF
        h = (h * FNV_PRIME) & MASK64
    return format(h, "016x")


def flang_module_bytes(body_text: str) -> bytes:
    """Wrap Fortran module source in the header Flang expects."""
    body = body_text.encode("utf-8")
    return BOM + MAGIC + flang_checksum(body).encode("ascii") + b"\n" + body


GFORTRAN_MAGIC = b"\x1f\x8b"


def is_gfortran_module(path: Path) -> bool:
    try:
        with open(path, "rb") as f:
            return f.read(2) == GFORTRAN_MAGIC
    except OSError:
        return False


def is_flang_module(path: Path) -> bool:
    try:
        with open(path, "rb") as f:
            return f.read(len(BOM) + len(MAGIC)) == BOM + MAGIC
    except OSError:
        return False


# ---------------------------------------------------------------------------
# S-expression reader
# ---------------------------------------------------------------------------


class ModuleFormatError(Exception):
    """The module could not be understood.  Always fail rather than guess."""


@dataclass
class Str:
    """A single-quoted string atom, kept distinct from a bare name."""

    value: str

    def __repr__(self) -> str:
        return f"'{self.value}'"


@dataclass
class Name:
    """A bare identifier atom such as DERIVED or UNKNOWN-INTENT."""

    value: str

    def __repr__(self) -> str:
        return self.value


# A parsed tree is nested lists of Str | Name | int.
Node = "list | Str | Name | int"

HEADER_RE = re.compile(
    r"GFORTRAN module version '(\d+)' created from (.*)"
)

_TOKEN_RE = re.compile(
    r"""
      (?P<lparen>\()
    | (?P<rparen>\))
    | (?P<string>'(?:[^']|'')*')
    | (?P<int>-?\d+)
    | (?P<name>[^\s()']+)
    """,
    re.VERBOSE,
)


def tokenize(text: str) -> Iterator[object]:
    pos = 0
    end = len(text)
    while pos < end:
        if text[pos].isspace():
            pos += 1
            continue
        m = _TOKEN_RE.match(text, pos)
        if not m:
            raise ModuleFormatError(
                f"unrecognised character {text[pos]!r} at offset {pos}"
            )
        pos = m.end()
        kind = m.lastgroup
        if kind == "lparen":
            yield "("
        elif kind == "rparen":
            yield ")"
        elif kind == "string":
            yield Str(m.group()[1:-1].replace("''", "'"))
        elif kind == "int":
            yield int(m.group())
        else:
            yield Name(m.group())


def parse_forest(tokens: Iterator[object]) -> list:
    """Parse a whole token stream into a list of top-level items."""
    stack: list[list] = [[]]
    for tok in tokens:
        if tok == "(":
            new: list = []
            stack[-1].append(new)
            stack.append(new)
        elif tok == ")":
            if len(stack) == 1:
                raise ModuleFormatError("unbalanced ')' in module body")
            stack.pop()
        else:
            stack[-1].append(tok)
    if len(stack) != 1:
        raise ModuleFormatError("unterminated '(' in module body")
    return stack[0]


@dataclass
class GfortranModule:
    path: Path
    version: int
    created_from: str
    forest: list

    @property
    def name(self) -> str:
        """Module name, taken from the file name as gfortran does."""
        return self.path.stem


def read_gfortran_module(path: Path) -> GfortranModule:
    raw = path.read_bytes()
    if raw[:2] != GFORTRAN_MAGIC:
        raise ModuleFormatError(f"{path}: not a gzip-compressed gfortran module")
    try:
        text = gzip.decompress(raw).decode("utf-8", errors="replace")
    except OSError as exc:
        raise ModuleFormatError(f"{path}: gzip decompression failed: {exc}") from exc

    header, _, body = text.partition("\n")
    m = HEADER_RE.match(header.strip())
    if not m:
        raise ModuleFormatError(
            f"{path}: unexpected header line {header.strip()!r}"
        )
    return GfortranModule(
        path=path,
        version=int(m.group(1)),
        created_from=m.group(2).strip(),
        forest=parse_forest(tokenize(body)),
    )


# ---------------------------------------------------------------------------
# Schema interpretation
#
# Layout follows mio_symbol() in gcc/fortran/module.cc:
#
#   <number> '<name>' '<module>' '<binding-label>' <flag>
#     ( <attributes> <components> [<component-access>] <typespec> ... )
#
# The component-access atom is present only when the component list is
# non-empty, which is why the typespec shifts position between flavors.
#
# The module version is checked before any of this is trusted.
# ---------------------------------------------------------------------------

SUPPORTED_VERSIONS = {16}

# Symbols gfortran emits for its own use, which have no source spelling.
ARTIFICIAL_PREFIXES = ("__def_init_", "__copy_", "__vtab_", "__vtype_")
INTRINSIC_MODULE = "(intrinsic)"


@dataclass
class TypeSpec:
    base: str  # INTEGER, REAL, LOGICAL, CHARACTER, DERIVED, ...
    kind: int | None = None
    derived_ref: int | None = None

    def fortran(self, symbols: dict[int, "Symbol"]) -> str:
        if self.base == "DERIVED":
            if self.derived_ref is None:
                raise ModuleFormatError("derived typespec without a symbol ref")
            target = symbols.get(self.derived_ref)
            if target is None:
                raise ModuleFormatError(
                    f"derived typespec references unknown symbol {self.derived_ref}"
                )
            return f"type({target.name})"
        if self.base in ("INTEGER", "REAL", "LOGICAL", "COMPLEX"):
            return f"{self.base.lower()}({self.kind})"
        if self.base == "CHARACTER":
            # Length lives elsewhere in the symbol; only kind is handled here.
            return f"character(kind={self.kind})"
        raise ModuleFormatError(f"unsupported base type {self.base}")


@dataclass
class Component:
    name: str
    # None when the type could not be interpreted; only fatal if the containing
    # type actually has to be emitted.
    typespec: TypeSpec | None

    @property
    def is_internal(self) -> bool:
        """gfortran vtable machinery: _copy, _vptr, _hash, _size, ..."""
        return self.name.startswith("_")


def constant_literal(node: object) -> str | None:
    """Render a  (CONSTANT <typespec> <flag> <value> ())  node as Fortran.

    Returns None for anything that is not a constant of a type whose spelling
    has been verified, so the caller can refuse rather than guess.
    """
    if not (isinstance(node, list) and len(node) >= 4):
        return None
    head = node[0]
    if not (isinstance(head, Name) and head.value == "CONSTANT"):
        return None
    ts = _as_typespec(node[1])
    raw = node[3]
    if ts is None:
        return None
    if ts.base == "INTEGER":
        # gfortran writes the mpz value as a decimal string.
        if isinstance(raw, Str) and re.fullmatch(r"-?\d+", raw.value):
            return raw.value
        if isinstance(raw, int):
            return str(raw)
    elif ts.base == "LOGICAL":
        if isinstance(raw, int):
            return ".true." if raw else ".false."
        if isinstance(raw, Str) and raw.value in ("0", "1"):
            return ".true." if raw.value == "1" else ".false."
    elif ts.base == "CHARACTER":
        if isinstance(raw, Str):
            escaped = raw.value.replace("'", "''")
            return f"'{escaped}'"
    return None


@dataclass
class Symbol:
    number: int
    name: str
    module: str
    attributes: list[str]
    typespec: TypeSpec | None
    components: list[Component] = field(default_factory=list)
    value: str | None = None
    # Symbol numbers of the dummy arguments, in order.
    formal_args: list[int] = field(default_factory=list)
    # True when mio_array_spec wrote a non-empty spec.
    is_array: bool = False

    @property
    def intent(self) -> str | None:
        """Fortran intent clause for a dummy argument, if it has one."""
        if len(self.attributes) < 2:
            return None
        return {"IN": "in", "OUT": "out", "INOUT": "inout"}.get(
            self.attributes[1]
        )

    @property
    def is_dummy(self) -> bool:
        return "DUMMY" in self.attributes

    @property
    def is_generic(self) -> bool:
        return "GENERIC" in self.attributes

    @property
    def is_function(self) -> bool:
        return "FUNCTION" in self.attributes

    @property
    def flavor(self) -> str:
        return self.attributes[0] if self.attributes else "UNKNOWN"

    @property
    def is_artificial(self) -> bool:
        return (
            "ARTIFICIAL" in self.attributes
            or self.module == INTRINSIC_MODULE
            or self.name.startswith(ARTIFICIAL_PREFIXES)
            or self.name.startswith("_")
        )


def _as_typespec(node: object) -> TypeSpec | None:
    """Interpret a (TYPE kind ...) typespec node."""
    if not isinstance(node, list) or not node:
        return None
    head = node[0]
    if not isinstance(head, Name):
        return None
    base = head.value
    if base == "UNKNOWN":
        return None
    if base in ("DERIVED", "CLASS"):
        ref = node[1] if len(node) > 1 and isinstance(node[1], int) else None
        return TypeSpec(base="DERIVED", derived_ref=ref)
    kind = node[1] if len(node) > 1 and isinstance(node[1], int) else None
    return TypeSpec(base=base, kind=kind)


def _parse_components(node: object) -> list[Component]:
    components: list[Component] = []
    if not isinstance(node, list):
        return components
    for entry in node:
        # (<number> '<name>' <typespec> ...)
        if (
            isinstance(entry, list)
            and len(entry) >= 3
            and isinstance(entry[0], int)
            and isinstance(entry[1], Str)
        ):
            # An uninterpretable type is recorded as None rather than raising:
            # inspection must work even on modules that cannot be translated.
            components.append(
                Component(name=entry[1].value, typespec=_as_typespec(entry[2]))
            )
    return components


def _sections(module: GfortranModule) -> tuple[list, list]:
    """Return (symbol pool, symtree).

    The body is a fixed sequence of top-level lists:

        [0]  intrinsic operator interfaces (27 entries)
        [1]  user operator interfaces
        [2]  generic interfaces
        [3]  common blocks
        [4]  equivalences
        [5]  derived-type extensions / OpenMP UDRs
        [6]  symbol pool
        [7]  symtree

    The two of interest are the last two, which is more robust than indexing
    from the front should a future version add a leading section.
    """
    forest = module.forest
    if len(forest) < 2 or not all(isinstance(x, list) for x in forest[-2:]):
        raise ModuleFormatError(
            f"{module.path}: expected a symbol pool and symtree at the end of "
            f"the module body, found {len(forest)} top-level items"
        )
    return forest[-2], forest[-1]


def parse_symbols(module: GfortranModule) -> dict[int, Symbol]:
    """Pull the symbol pool apart.

    Entries are a flat run of  <number> '<name>' '<module>' '<label>' <flag>
    (<body>)  inside a single list.
    """
    symbols: dict[int, Symbol] = {}
    flat, _ = _sections(module)
    i = 0
    while i <= len(flat) - 6:
        if (
            isinstance(flat[i], int)
            and isinstance(flat[i + 1], Str)
            and isinstance(flat[i + 2], Str)
            and isinstance(flat[i + 3], Str)
            and isinstance(flat[i + 4], int)
            and isinstance(flat[i + 5], list)
        ):
            number = flat[i]
            name = flat[i + 1].value
            mod = flat[i + 2].value
            body = flat[i + 5]

            attrs = [a.value for a in body[0] if isinstance(a, Name)] if body else []
            raw_components = body[1] if len(body) > 1 else []
            components = _parse_components(raw_components)
            # The component-access atom is written only when the component list
            # is non-empty, which shifts everything after it by one.
            has_components = isinstance(raw_components, list) and bool(raw_components)
            ts_index = 3 if has_components else 2
            typespec = (
                _as_typespec(body[ts_index]) if len(body) > ts_index else None
            )
            # mio_symbol writes, after the typespec:
            #   formal_ns, common_next, formal_arglist,
            #   [value -- only when the flavor is PARAMETER],
            #   array_spec, result, ...
            # so everything after the arglist shifts for named constants.
            def at(offset: int) -> object:
                idx = ts_index + offset
                return body[idx] if len(body) > idx else None

            formal_node = at(3)
            formal_args = (
                [x for x in formal_node if isinstance(x, int)]
                if isinstance(formal_node, list)
                else []
            )

            is_parameter = "PARAMETER" in attrs
            value = constant_literal(at(4)) if is_parameter else None
            array_node = at(5) if is_parameter else at(4)
            is_array = isinstance(array_node, list) and bool(array_node)

            symbols[number] = Symbol(
                number=number,
                name=name,
                module=mod,
                attributes=attrs,
                typespec=typespec,
                components=components,
                value=value,
                formal_args=formal_args,
                is_array=is_array,
            )
            i += 6
            continue
        i += 1
    return symbols


def parse_symtree(module: GfortranModule) -> dict[str, int]:
    """Public name -> symbol number.

    The symtree is a flat run of  '<name>' <ambiguous-flag> <symbol-number>.
    """
    tree: dict[str, int] = {}
    _, flat = _sections(module)
    if len(flat) % 3 != 0:
        raise ModuleFormatError(
            f"{module.path}: symtree has {len(flat)} atoms, not a multiple of 3"
        )
    for i in range(0, len(flat), 3):
        name, _ambiguous, number = flat[i], flat[i + 1], flat[i + 2]
        if not (isinstance(name, Str) and isinstance(number, int)):
            raise ModuleFormatError(
                f"{module.path}: malformed symtree entry at index {i}"
            )
        tree[name.value] = number
    return tree


# ---------------------------------------------------------------------------
# Emission
# ---------------------------------------------------------------------------


class Unsupported(ModuleFormatError):
    """A construct outside the translated subset.

    Raised rather than emitting an approximation: a wrong declaration would
    produce confident, wrong diagnostics downstream.
    """


def emit_interface(
    public_name: str, sym: Symbol, symbols: dict[int, Symbol]
) -> list[str]:
    """Render an explicit interface for a procedure symbol.

    The formal argument list is a list of symbol numbers; each referenced
    symbol carries the dummy's name, type and intent.
    """
    if sym.is_generic:
        # A generic name stands for several specifics, which live in the
        # generic-interface section rather than here.  Emitting one specific
        # under the generic name would silently change overload resolution.
        raise Unsupported("generic interfaces are not translated yet")

    dummies: list[Symbol] = []
    for ref in sym.formal_args:
        dummy = symbols.get(ref)
        if dummy is None:
            raise Unsupported(f"dummy argument symbol {ref} is not in the pool")
        if dummy.typespec is None:
            raise Unsupported(
                f"dummy argument {dummy.name!r} has an unrecoverable type"
            )
        if dummy.is_array:
            raise Unsupported(
                f"dummy argument {dummy.name!r} is an array, which needs the "
                f"array spec decoded"
            )
        dummies.append(dummy)

    kind = "function" if sym.is_function else "subroutine"
    if kind == "function" and sym.typespec is None:
        raise Unsupported("function result type is unrecoverable")

    names = ", ".join(d.name for d in dummies)
    lines = ["interface", f"  {kind} {public_name}({names})"]

    if kind == "function":
        lines.append(f"    {sym.typespec.fortran(symbols)} :: {public_name}")
    for d in dummies:
        decl = d.typespec.fortran(symbols)
        intent = d.intent
        if intent:
            decl += f", intent({intent})"
        if "OPTIONAL" in d.attributes:
            decl += ", optional"
        lines.append(f"    {decl} :: {d.name}")

    lines.append(f"  end {kind} {public_name}")
    lines.append("end interface")
    return lines


def analyse(module: GfortranModule) -> tuple[list[str], list[str]]:
    """Translate what can be translated.

    Returns (body lines, problems).  Every public symbol outside the subset
    contributes one problem, so callers can see the whole picture rather than
    only the first obstacle.
    """
    symbols = parse_symbols(module)
    symtree = parse_symtree(module)
    problems: list[str] = []
    body: list[str] = []
    emitted: set[int] = set()

    # gfortran auto-generates a structure-constructor generic for every derived
    # type, and it is *that* symbol the symtree points at -- the type itself is
    # only reachable through the pool.  Index the real types by name so a public
    # name resolves to the type whichever of the two it names.
    derived_by_name = {
        sym.name.lower(): sym
        for sym in symbols.values()
        if sym.flavor == "DERIVED" and not sym.is_artificial
    }

    def emit_derived(public_name: str, sym: Symbol) -> None:
        if sym.number in emitted:
            return
        lines = [f"type :: {public_name}"]
        for comp in sym.components:
            if comp.is_internal:
                continue  # vtable machinery, no source spelling
            if comp.typespec is None:
                problems.append(
                    f"type {public_name!r}: component {comp.name!r} has a type "
                    f"that cannot be expressed"
                )
                return
            try:
                lines.append(f"  {comp.typespec.fortran(symbols)} :: {comp.name}")
            except ModuleFormatError as exc:
                problems.append(f"type {public_name!r}: {exc}")
                return
        lines.append(f"end type {public_name}")
        body.extend(lines)
        emitted.add(sym.number)

    # Derived types first; later declarations may refer to them.
    for public_name in sorted(symtree):
        dt = derived_by_name.get(public_name.lower())
        if dt is not None:
            emit_derived(public_name, dt)

    for public_name, number in sorted(symtree.items()):
        if public_name.lower() in derived_by_name:
            continue  # already handled above
        sym = symbols.get(number)
        if sym is None or sym.is_artificial or number in emitted:
            continue
        if sym.flavor == "MODULE":
            continue  # a use-associated module, not a declaration of our own

        if sym.flavor == "PARAMETER":
            if sym.typespec is None or sym.value is None:
                problems.append(
                    f"named constant {public_name!r}: value not recoverable"
                )
                continue
            try:
                decl = sym.typespec.fortran(symbols)
            except ModuleFormatError as exc:
                problems.append(f"named constant {public_name!r}: {exc}")
                continue
            body.append(f"{decl}, parameter :: {public_name} = {sym.value}")
            emitted.add(number)
        elif sym.flavor == "VARIABLE" and sym.typespec is not None:
            try:
                decl = sym.typespec.fortran(symbols)
            except ModuleFormatError as exc:
                problems.append(f"variable {public_name!r}: {exc}")
                continue
            body.append(f"{decl} :: {public_name}")
            emitted.add(number)
        elif sym.flavor == "PROCEDURE":
            try:
                body.extend(emit_interface(public_name, sym, symbols))
                emitted.add(number)
            except Unsupported as exc:
                problems.append(f"procedure {public_name!r}: {exc}")
        else:
            problems.append(
                f"symbol {public_name!r}: flavor {sym.flavor} is outside the "
                f"translated subset"
            )

    return body, problems


def summarise_problems(problems: list[str], limit: int = 6) -> str:
    shown = problems[:limit]
    rest = len(problems) - len(shown)
    text = "; ".join(shown)
    if rest > 0:
        text += f"; and {rest} more"
    return text


def emit_flang_module(module: GfortranModule, strict: bool = True) -> str:
    if module.version not in SUPPORTED_VERSIONS:
        raise Unsupported(
            f"{module.path}: gfortran module version {module.version} is not "
            f"supported (known: {sorted(SUPPORTED_VERSIONS)}). The format "
            f"changes between GCC releases; the schema must be checked against "
            f"gcc/fortran/module.cc for this version."
        )

    symtree = parse_symtree(module)
    if not symtree:
        raise Unsupported(f"{module.path}: no public symbols found")

    body, problems = analyse(module)
    if problems and strict:
        raise Unsupported(
            f"{module.path}: {len(problems)} untranslatable symbol(s): "
            f"{summarise_problems(problems)}"
        )

    lines = [f"module {module.name}", "implicit none"]
    lines.extend(body)
    lines.append(f"end module {module.name}")
    return "\n".join(lines) + "\n"


def convert_file(src: Path, dst: Path) -> None:
    module = read_gfortran_module(src)
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(flang_module_bytes(emit_flang_module(module)))


# ---------------------------------------------------------------------------
# Wrapper
# ---------------------------------------------------------------------------

# Flang module search options.  -J/-module-dir is single valued and must not be
# duplicated, so the cache is always injected as -I.
PATH_FLAGS_JOINED = ("-I", "-J")
PATH_FLAGS_SEPARATE = ("-I", "-J", "-module-dir", "-fintrinsic-modules-path")


def module_search_dirs(argv: Sequence[str]) -> list[str]:
    dirs: list[str] = []
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg in PATH_FLAGS_SEPARATE and i + 1 < len(argv):
            dirs.append(argv[i + 1])
            i += 2
            continue
        for flag in PATH_FLAGS_JOINED:
            if arg.startswith(flag) and len(arg) > len(flag):
                dirs.append(arg[len(flag) :])
                break
        else:
            if arg.startswith("-fintrinsic-modules-path="):
                dirs.append(arg.split("=", 1)[1])
        i += 1
    dirs.append(".")
    return dirs


def cache_dir_for(explicit: str | None) -> Path:
    if explicit:
        return Path(explicit)
    base = os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache")
    return Path(base) / "fxmod"


def populate_cache(dirs: Sequence[str], cache: Path, verbose: bool) -> tuple[int, int]:
    """Convert every gfortran module found in dirs into cache.

    Originals are never modified.  Returns (converted, skipped).
    """
    converted = skipped = 0
    cache.mkdir(parents=True, exist_ok=True)
    for d in dirs:
        directory = Path(d)
        if not directory.is_dir():
            continue
        for mod in sorted(directory.glob("*.mod")):
            if not is_gfortran_module(mod):
                continue
            digest = hashlib.sha256(mod.read_bytes()).hexdigest()[:16]
            stamp = cache / f".{mod.stem}.{digest}.stamp"
            target = cache / mod.name
            if stamp.exists() and target.exists():
                converted += 1
                continue
            try:
                convert_file(mod, target)
            except ModuleFormatError as exc:
                # Leave the original alone: Flang will report it as not being
                # one of its module files, which is clearer than a translation
                # that is quietly wrong.
                if verbose:
                    # exc already names the file.
                    print(f"fxmod: skipping {exc}", file=sys.stderr)
                if target.exists():
                    target.unlink()
                skipped += 1
                continue
            for old in cache.glob(f".{mod.stem}.*.stamp"):
                old.unlink()
            stamp.touch()
            converted += 1
            if verbose:
                print(f"fxmod: converted {mod} -> {target}", file=sys.stderr)
    return converted, skipped


def run_wrap(args: argparse.Namespace) -> int:
    command = args.command
    if not command:
        print("fxmod: wrap needs a command after --", file=sys.stderr)
        return 2

    dirs = module_search_dirs(command[1:])
    cache = cache_dir_for(args.cache)
    converted, skipped = populate_cache(dirs, cache, args.verbose)

    if args.verbose:
        print(
            f"fxmod: {converted} converted, {skipped} skipped, cache {cache}",
            file=sys.stderr,
        )

    # Inject the cache first so converted modules win the search order.
    new_command = [command[0], f"-I{cache}", *command[1:]]
    if args.dry_run:
        print(" ".join(new_command))
        return 0

    exe = shutil.which(new_command[0])
    if exe is None:
        print(f"fxmod: command not found: {new_command[0]}", file=sys.stderr)
        return 127
    os.execv(exe, new_command)


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------


def render(node: object, indent: int = 0) -> str:
    pad = "  " * indent
    if isinstance(node, list):
        if not node:
            return pad + "()"
        if all(not isinstance(c, list) for c in node) and len(node) <= 12:
            return pad + "(" + " ".join(repr(c) for c in node) + ")"
        inner = "\n".join(render(c, indent + 1) for c in node)
        return f"{pad}(\n{inner}\n{pad})"
    return pad + repr(node)


def run_dump(args: argparse.Namespace) -> int:
    module = read_gfortran_module(Path(args.file))
    print(f"; gfortran module version {module.version}")
    print(f"; created from {module.created_from}")
    for item in module.forest:
        print(render(item))
    return 0


def run_info(args: argparse.Namespace) -> int:
    module = read_gfortran_module(Path(args.file))
    symbols = parse_symbols(module)
    symtree = parse_symtree(module)
    print(f"file      : {module.path}")
    print(f"version   : {module.version}")
    print(f"created   : {module.created_from}")
    print(f"module    : {module.name}")
    print(f"symbols   : {len(symbols)}")
    print(f"public    : {len(symtree)}")
    flavors: dict[str, int] = {}
    for number in symtree.values():
        sym = symbols.get(number)
        key = "<missing>" if sym is None else sym.flavor
        if sym is not None and sym.is_artificial:
            key += " (artificial)"
        flavors[key] = flavors.get(key, 0) + 1
    print("public symbols by flavor:")
    for key in sorted(flavors):
        print(f"  {key:<28} {flavors[key]}")
    if args.list:
        print("public symbols:")
        for name in sorted(symtree):
            sym = symbols.get(symtree[name])
            flavor = "<missing>" if sym is None else sym.flavor
            print(f"  {name:<40} {flavor}")

    body, problems = analyse(module)
    print(f"translatable   : {len(body)} declaration line(s)")
    print(f"untranslatable : {len(problems)}")
    if problems:
        # Group identical reasons so a module blocked on 113 procedures reads
        # as one line rather than 113.
        buckets: dict[str, list[str]] = {}
        for p in problems:
            what, _, why = p.partition(": ")
            buckets.setdefault(why or p, []).append(what)
        print("blockers:")
        for why in sorted(buckets):
            names = buckets[why]
            sample = ", ".join(names[:3])
            more = f", and {len(names) - 3} more" if len(names) > 3 else ""
            print(f"  [{len(names):>3}] {why}")
            print(f"        e.g. {sample}{more}")
    return 0


def run_convert(args: argparse.Namespace) -> int:
    src = Path(args.file)
    module = read_gfortran_module(src)
    text = emit_flang_module(module)
    if args.output:
        Path(args.output).write_bytes(flang_module_bytes(text))
        print(f"wrote {args.output}", file=sys.stderr)
    else:
        sys.stdout.write(text)
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="fxmod",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("dump", help="print the parsed S-expression tree")
    p.add_argument("file")
    p.set_defaults(func=run_dump)

    p = sub.add_parser("info", help="summarise a gfortran module")
    p.add_argument("file")
    p.add_argument("--list", action="store_true", help="list public symbols")
    p.set_defaults(func=run_info)

    p = sub.add_parser("convert", help="translate to a Flang module")
    p.add_argument("file")
    p.add_argument("-o", "--output")
    p.set_defaults(func=run_convert)

    p = sub.add_parser("wrap", help="run a command with modules converted")
    p.add_argument("--cache", help="cache directory (default ~/.cache/fxmod)")
    p.add_argument("-v", "--verbose", action="store_true")
    p.add_argument("-n", "--dry-run", action="store_true",
                   help="print the rewritten command instead of running it")
    p.add_argument("command", nargs=argparse.REMAINDER)
    p.set_defaults(func=run_wrap)

    args = parser.parse_args(argv)
    if args.cmd == "wrap" and args.command and args.command[0] == "--":
        args.command = args.command[1:]

    try:
        return args.func(args)
    except ModuleFormatError as exc:
        print(f"fxmod: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
