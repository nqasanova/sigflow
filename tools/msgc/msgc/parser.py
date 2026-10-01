"""Recursive descent parser for .msg schemas.

Grammar (informal):

    schema     := "namespace" NAME ";" definition*
    definition := enum | struct | message
    enum       := "enum" NAME ":" TYPE "{" (NAME "=" NUMBER ";")* "}"
    struct     := "struct" NAME "{" field* "}"
    message    := "message" NAME "=" NUMBER "{" field* "}"
    field      := NUMBER ":" ["optional" | "repeated"] TYPE NAME ";"

The parser only checks syntax. Semantic rules (unknown types, duplicate
tags and so on) live in validate.py so they can be tested on their own.
"""

from __future__ import annotations

from pathlib import Path

from .lexer import Kind, SchemaError, Token, tokenize
from .model import EnumDef, EnumValue, FieldDef, Modifier, Schema, StructDef

_MODIFIERS = {"optional": Modifier.OPTIONAL, "repeated": Modifier.REPEATED}


class _Parser:
    def __init__(self, tokens: list[Token], path: str) -> None:
        self._tokens = tokens
        self._pos = 0
        self._path = path

    # Small helpers ---------------------------------------------------------

    def _peek(self) -> Token:
        return self._tokens[self._pos]

    def _advance(self) -> Token:
        token = self._tokens[self._pos]
        if token.kind is not Kind.EOF:
            self._pos += 1
        return token

    def _expect_symbol(self, symbol: str) -> Token:
        token = self._advance()
        if token.kind is not Kind.SYMBOL or token.text != symbol:
            raise SchemaError(token.loc, f"expected '{symbol}' but found {_describe(token)}")
        return token

    def _expect_ident(self, what: str) -> Token:
        token = self._advance()
        if token.kind is not Kind.IDENT:
            raise SchemaError(token.loc, f"expected {what} but found {_describe(token)}")
        return token

    def _expect_number(self, what: str) -> Token:
        token = self._advance()
        if token.kind is not Kind.NUMBER:
            raise SchemaError(token.loc, f"expected {what} but found {_describe(token)}")
        return token

    def _at_symbol(self, symbol: str) -> bool:
        token = self._peek()
        return token.kind is Kind.SYMBOL and token.text == symbol

    # Grammar rules ---------------------------------------------------------

    def parse(self) -> Schema:
        keyword = self._expect_ident("'namespace'")
        if keyword.text != "namespace":
            raise SchemaError(keyword.loc, "a schema has to start with a namespace declaration")
        namespace = self._expect_ident("namespace name")
        self._expect_symbol(";")

        definitions = []
        while self._peek().kind is not Kind.EOF:
            token = self._expect_ident("'enum', 'struct' or 'message'")
            if token.text == "enum":
                definitions.append(self._parse_enum(token))
            elif token.text == "struct":
                definitions.append(self._parse_struct(token, is_message=False))
            elif token.text == "message":
                definitions.append(self._parse_struct(token, is_message=True))
            else:
                raise SchemaError(token.loc, f"unknown keyword '{token.text}'")

        return Schema(
            path=self._path,
            namespace=tuple(namespace.text.split(".")),
            definitions=tuple(definitions),
        )

    def _parse_enum(self, keyword: Token) -> EnumDef:
        name = self._expect_ident("enum name")
        self._expect_symbol(":")
        underlying = self._expect_ident("underlying type")
        self._expect_symbol("{")

        values = []
        while not self._at_symbol("}"):
            value_name = self._expect_ident("enum value name")
            self._expect_symbol("=")
            number = self._expect_number("enum value")
            self._expect_symbol(";")
            values.append(EnumValue(value_name.text, number.number, value_name.loc))
        self._expect_symbol("}")

        return EnumDef(name.text, underlying.text, tuple(values), keyword.loc)

    def _parse_struct(self, keyword: Token, *, is_message: bool) -> StructDef:
        name = self._expect_ident(f"{keyword.text} name")
        message_id = None
        if is_message:
            self._expect_symbol("=")
            message_id = self._expect_number("message id").number
        self._expect_symbol("{")

        fields = []
        while not self._at_symbol("}"):
            fields.append(self._parse_field())
        self._expect_symbol("}")

        return StructDef(name.text, tuple(fields), keyword.loc, message_id)

    def _parse_field(self) -> FieldDef:
        tag = self._expect_number("field tag")
        self._expect_symbol(":")

        type_token = self._expect_ident("field type")
        modifier = Modifier.REQUIRED
        if type_token.text in _MODIFIERS:
            modifier = _MODIFIERS[type_token.text]
            type_token = self._expect_ident("field type")

        name = self._expect_ident("field name")
        self._expect_symbol(";")
        return FieldDef(tag.number, name.text, type_token.text, modifier, tag.loc)


def _describe(token: Token) -> str:
    return "end of file" if token.kind is Kind.EOF else f"'{token.text}'"


def parse_source(source: str, path: str = "<input>") -> Schema:
    return _Parser(tokenize(source, path), path).parse()


def parse_file(path: str | Path) -> Schema:
    path = Path(path)
    return parse_source(path.read_text(encoding="utf-8"), str(path))
