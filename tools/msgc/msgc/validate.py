"""Semantic checks on parsed schemas.

All problems are collected instead of stopping at the first one, so a
single run of the compiler shows everything that needs fixing.
"""

from __future__ import annotations

import keyword
import re

from .lexer import SchemaError
from .model import (
    BUILTIN_TYPES,
    INT_TYPES,
    EnumDef,
    Location,
    Schema,
    StructDef,
)

MAX_TAG = 255  # tags are a single byte on the wire
MAX_MESSAGE_ID = 0xFFFF

_IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
_TYPE_NAME_RE = re.compile(r"^[A-Z][A-Za-z0-9]*$")
_FIELD_NAME_RE = re.compile(r"^[a-z][a-z0-9_]*$")
_ENUM_VALUE_RE = re.compile(r"^[A-Z][A-Z0-9_]*$")

# Names that would break at least one of the generated languages. Not a
# complete list of C++ keywords, just the ones that look like plausible
# field names.
_RESERVED = frozenset(keyword.kwlist) | {
    "auto", "case", "char", "class", "const", "default", "delete", "double",
    "explicit", "float", "int", "long", "namespace", "new", "operator",
    "private", "protected", "public", "register", "short", "signed",
    "sizeof", "static", "struct", "switch", "template", "this", "throw",
    "typedef", "typename", "union", "unsigned", "using", "virtual", "void",
    "volatile", "encode", "decode",
}


class ValidationError(Exception):
    def __init__(self, errors: list[SchemaError]) -> None:
        super().__init__("\n".join(str(e) for e in errors))
        self.errors = errors


def validate(schemas: list[Schema]) -> None:
    """Checks every schema and the ids shared between them.

    Message ids have to be unique across all files that are compiled
    together, because they end up on the same wire.
    """
    errors: list[SchemaError] = []
    seen_ids: dict[int, str] = {}

    for schema in schemas:
        errors.extend(_check_schema(schema))
        for message in schema.messages:
            if message.message_id is None:
                continue
            owner = seen_ids.get(message.message_id)
            if owner is not None:
                errors.append(SchemaError(
                    message.loc,
                    f"message id 0x{message.message_id:04x} is already used by {owner}",
                ))
            else:
                seen_ids[message.message_id] = message.name

    if errors:
        raise ValidationError(errors)


def _check_schema(schema: Schema) -> list[SchemaError]:
    errors: list[SchemaError] = []

    for part in schema.namespace:
        if not _IDENT_RE.match(part) or part in _RESERVED:
            errors.append(SchemaError(
                schema.definitions[0].loc if schema.definitions else _fallback_loc(schema),
                f"invalid namespace component '{part}'",
            ))

    declared: dict[str, EnumDef | StructDef] = {}
    for definition in schema.definitions:
        if not _TYPE_NAME_RE.match(definition.name):
            errors.append(SchemaError(
                definition.loc, f"type name '{definition.name}' should be in PascalCase"))
        if definition.name in declared or definition.name in BUILTIN_TYPES:
            errors.append(SchemaError(definition.loc, f"'{definition.name}' is declared twice"))

        if isinstance(definition, EnumDef):
            errors.extend(_check_enum(definition))
        else:
            # Passing only what was declared so far enforces define before
            # use, which also rules out recursive structs for free.
            errors.extend(_check_struct(definition, declared))

        declared[definition.name] = definition

    return errors


def _check_enum(enum: EnumDef) -> list[SchemaError]:
    errors: list[SchemaError] = []
    int_type = INT_TYPES.get(enum.underlying)
    if int_type is None or int_type.signed:
        errors.append(SchemaError(
            enum.loc, f"enum '{enum.name}' needs an unsigned underlying type, got '{enum.underlying}'"))
    if not enum.values:
        errors.append(SchemaError(enum.loc, f"enum '{enum.name}' has no values"))

    names: set[str] = set()
    numbers: set[int] = set()
    for value in enum.values:
        if not _ENUM_VALUE_RE.match(value.name):
            errors.append(SchemaError(
                value.loc, f"enum value '{value.name}' should be in UPPER_SNAKE_CASE"))
        if value.name in names:
            errors.append(SchemaError(value.loc, f"duplicate enum value name '{value.name}'"))
        if value.value in numbers:
            errors.append(SchemaError(value.loc, f"duplicate enum value {value.value}"))
        if int_type is not None and not int_type.min_value <= value.value <= int_type.max_value:
            errors.append(SchemaError(
                value.loc, f"{value.value} does not fit into {enum.underlying}"))
        names.add(value.name)
        numbers.add(value.value)
    return errors


def _check_struct(struct: StructDef, declared: dict[str, EnumDef | StructDef]) -> list[SchemaError]:
    errors: list[SchemaError] = []

    if struct.message_id is not None and not 0 < struct.message_id <= MAX_MESSAGE_ID:
        errors.append(SchemaError(
            struct.loc, f"message id must be between 1 and 0x{MAX_MESSAGE_ID:x}"))

    tags: set[int] = set()
    names: set[str] = set()
    for f in struct.fields:
        if not 1 <= f.tag <= MAX_TAG:
            errors.append(SchemaError(f.loc, f"tag {f.tag} is out of range (1..{MAX_TAG})"))
        if f.tag in tags:
            errors.append(SchemaError(f.loc, f"tag {f.tag} is used twice in '{struct.name}'"))
        if not _FIELD_NAME_RE.match(f.name) or f.name in _RESERVED:
            errors.append(SchemaError(f.loc, f"'{f.name}' is not a valid field name"))
        if f.name in names:
            errors.append(SchemaError(f.loc, f"field '{f.name}' is declared twice"))
        tags.add(f.tag)
        names.add(f.name)

        target = declared.get(f.type_name)
        if f.type_name in BUILTIN_TYPES or isinstance(target, EnumDef):
            continue
        if isinstance(target, StructDef):
            if target.is_message:
                errors.append(SchemaError(
                    f.loc, f"'{f.type_name}' is a message; only structs can be nested"))
            continue
        errors.append(SchemaError(f.loc, f"unknown type '{f.type_name}'"))

    # Messages without fields are allowed on purpose, a keep alive or a
    # plain acknowledgement does not need any payload.
    return errors


def _fallback_loc(schema: Schema) -> Location:
    return Location(schema.path, 1, 1)
