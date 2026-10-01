"""Data model produced by the parser and consumed by the backends.

Everything here is a plain frozen dataclass, so a backend can never modify
the schema it was given. Keeping the model independent from the parser also
means a different front end (JSON, YAML, whatever) could feed the same
backends later.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum


@dataclass(frozen=True)
class Location:
    """Points back into the schema source so errors can be reported precisely."""

    path: str
    line: int
    column: int

    def __str__(self) -> str:
        return f"{self.path}:{self.line}:{self.column}"


@dataclass(frozen=True)
class IntType:
    name: str
    size: int  # bytes on the wire
    signed: bool

    @property
    def min_value(self) -> int:
        return -(1 << (self.size * 8 - 1)) if self.signed else 0

    @property
    def max_value(self) -> int:
        bits = self.size * 8 - (1 if self.signed else 0)
        return (1 << bits) - 1


# Integer types are always fixed width and big endian on the wire. A varint
# encoding would save a few bytes, but fixed widths keep the decoder trivial
# and make hex dumps easy to read by eye, which matters more here.
INT_TYPES: dict[str, IntType] = {
    t.name: t
    for t in (
        IntType("u8", 1, False),
        IntType("u16", 2, False),
        IntType("u32", 4, False),
        IntType("u64", 8, False),
        IntType("i8", 1, True),
        IntType("i16", 2, True),
        IntType("i32", 4, True),
        IntType("i64", 8, True),
    )
}

OTHER_BUILTINS = frozenset({"bool", "string", "bytes"})
BUILTIN_TYPES = frozenset(INT_TYPES) | OTHER_BUILTINS


class Modifier(Enum):
    REQUIRED = "required"
    OPTIONAL = "optional"
    REPEATED = "repeated"


@dataclass(frozen=True)
class EnumValue:
    name: str
    value: int
    loc: Location


@dataclass(frozen=True)
class EnumDef:
    name: str
    underlying: str  # one of the unsigned INT_TYPES
    values: tuple[EnumValue, ...]
    loc: Location

    @property
    def default(self) -> EnumValue:
        # The first declared value doubles as the default. Using 0 blindly
        # would produce an invalid enum whenever 0 is not a declared value.
        return self.values[0]


@dataclass(frozen=True)
class FieldDef:
    tag: int
    name: str
    type_name: str
    modifier: Modifier
    loc: Location


@dataclass(frozen=True)
class StructDef:
    """A group of fields. Top level messages carry an id, nested structs do not."""

    name: str
    fields: tuple[FieldDef, ...]
    loc: Location
    message_id: int | None = None

    @property
    def is_message(self) -> bool:
        return self.message_id is not None


@dataclass(frozen=True)
class Schema:
    path: str
    namespace: tuple[str, ...]
    # Declaration order is kept on purpose: types must be declared before
    # use, so emitting them in this order always compiles.
    definitions: tuple[EnumDef | StructDef, ...] = field(default_factory=tuple)

    @property
    def enums(self) -> list[EnumDef]:
        return [d for d in self.definitions if isinstance(d, EnumDef)]

    @property
    def structs(self) -> list[StructDef]:
        return [d for d in self.definitions if isinstance(d, StructDef)]

    @property
    def messages(self) -> list[StructDef]:
        return [s for s in self.structs if s.is_message]

    def lookup(self, name: str) -> EnumDef | StructDef | None:
        for definition in self.definitions:
            if definition.name == name:
                return definition
        return None
