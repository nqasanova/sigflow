"""Pieces shared by all code generator backends."""

from __future__ import annotations

from contextlib import contextmanager
from enum import Enum, auto
from typing import Iterator

from ..model import INT_TYPES, EnumDef, Schema, StructDef


class TypeKind(Enum):
    INT = auto()
    BOOL = auto()
    STRING = auto()
    BYTES = auto()
    ENUM = auto()
    STRUCT = auto()


def classify(schema: Schema, type_name: str) -> TypeKind:
    """Maps a schema type name to the category a backend cares about.

    Backends switch on the category instead of on raw type names, so a new
    integer width only has to be added to the model and nowhere else.
    """
    if type_name in INT_TYPES:
        return TypeKind.INT
    simple = {"bool": TypeKind.BOOL, "string": TypeKind.STRING, "bytes": TypeKind.BYTES}
    if type_name in simple:
        return simple[type_name]
    definition = schema.lookup(type_name)
    if isinstance(definition, EnumDef):
        return TypeKind.ENUM
    if isinstance(definition, StructDef):
        return TypeKind.STRUCT
    # The validator runs before any backend, so this means a bug in msgc.
    raise AssertionError(f"unvalidated type '{type_name}' reached the backend")


def pascal_from_upper_snake(name: str) -> str:
    """REJECTED_CONGESTION -> RejectedCongestion"""
    return "".join(part.capitalize() for part in name.split("_") if part)


class CodeWriter:
    """Collects lines of code and takes care of indentation.

    Building output line by line looks a bit more verbose than a template
    engine, but it needs no third party package and the indentation of
    nested blocks can never get out of sync.
    """

    def __init__(self, indent: str = "    ") -> None:
        self._lines: list[str] = []
        self._indent = indent
        self._level = 0

    def line(self, text: str = "") -> None:
        self._lines.append(f"{self._indent * self._level}{text}" if text else "")

    def lines(self, *texts: str) -> None:
        for text in texts:
            self.line(text)

    @contextmanager
    def indented(self, levels: int = 1) -> Iterator[None]:
        self._level += levels
        try:
            yield
        finally:
            self._level -= levels

    @contextmanager
    def block(self, opener: str, closer: str | None = "}") -> Iterator[None]:
        """Writes opener, indents everything inside, then writes closer.

        Pass closer=None for languages like Python where indentation alone
        ends the block.
        """
        self.line(opener)
        with self.indented():
            yield
        if closer is not None:
            self.line(closer)

    def render(self) -> str:
        # Exactly one trailing newline, no matter how the last block ended.
        return "\n".join(self._lines).rstrip("\n") + "\n"
