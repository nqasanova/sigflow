"""Tokenizer for the .msg schema language.

The grammar is small enough that a hand written scanner is clearer than
pulling in a parser generator. Every token remembers where it came from so
that error messages can point at the exact line and column.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from enum import Enum, auto

from .model import Location


class SchemaError(Exception):
    """Raised for any problem in a schema file. Always carries a location."""

    def __init__(self, loc: Location, message: str) -> None:
        super().__init__(f"{loc}: error: {message}")
        self.loc = loc
        self.message = message


class Kind(Enum):
    IDENT = auto()
    NUMBER = auto()
    SYMBOL = auto()
    EOF = auto()


@dataclass(frozen=True)
class Token:
    kind: Kind
    text: str
    loc: Location

    @property
    def number(self) -> int:
        return int(self.text, 0)


# Order matters: comments and whitespace are tried first and simply skipped.
# Hex has to come before decimal, otherwise "0x10" would lex as "0" + "x10".
_TOKEN_RE = re.compile(
    r"""
    (?P<ws>[ \t\r]+)
  | (?P<newline>\n)
  | (?P<comment>//[^\n]*)
  | (?P<number>-?0[xX][0-9a-fA-F]+|-?[0-9]+)
  | (?P<ident>[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*)
  | (?P<symbol>[{}:;=])
    """,
    re.VERBOSE,
)


def tokenize(source: str, path: str = "<input>") -> list[Token]:
    tokens: list[Token] = []
    line, line_start, pos = 1, 0, 0

    while pos < len(source):
        match = _TOKEN_RE.match(source, pos)
        loc = Location(path, line, pos - line_start + 1)
        if match is None:
            raise SchemaError(loc, f"unexpected character {source[pos]!r}")

        group = match.lastgroup
        text = match.group()
        if group == "newline":
            line += 1
            line_start = match.end()
        elif group == "number":
            tokens.append(Token(Kind.NUMBER, text, loc))
        elif group == "ident":
            tokens.append(Token(Kind.IDENT, text, loc))
        elif group == "symbol":
            tokens.append(Token(Kind.SYMBOL, text, loc))
        pos = match.end()

    tokens.append(Token(Kind.EOF, "", Location(path, line, pos - line_start + 1)))
    return tokens
