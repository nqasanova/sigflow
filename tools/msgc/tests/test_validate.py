import unittest

from msgc import ValidationError, parse_source, validate


def check(source, path="test.msg"):
    validate([parse_source("namespace t;\n" + source, path)])


class ValidateTest(unittest.TestCase):
    def assert_invalid(self, source, expected_text):
        with self.assertRaises(ValidationError) as ctx:
            check(source)
        messages = [e.message for e in ctx.exception.errors]
        self.assertTrue(
            any(expected_text in m for m in messages),
            f"expected an error containing {expected_text!r}, got {messages}",
        )

    def test_valid_schema_passes(self):
        check("""
            enum E : u16 { A = 1; B = 2; }
            struct S { 1: u8 a; }
            message M = 1 { 1: E e; 2: S s; 3: repeated S many; 4: optional bytes raw; }
        """)

    def test_empty_message_is_allowed(self):
        check("message KeepAlive = 7 {}")

    def test_duplicate_tag(self):
        self.assert_invalid("message M = 1 { 1: u8 a; 1: u8 b; }", "tag 1 is used twice")

    def test_tag_out_of_range(self):
        self.assert_invalid("message M = 1 { 256: u8 a; }", "out of range")
        self.assert_invalid("message M = 1 { 0: u8 a; }", "out of range")

    def test_unknown_type(self):
        self.assert_invalid("message M = 1 { 1: u24 a; }", "unknown type 'u24'")

    def test_types_must_be_declared_before_use(self):
        self.assert_invalid("message M = 1 { 1: Later l; }\nstruct Later { 1: u8 x; }", "unknown type 'Later'")

    def test_messages_cannot_be_nested(self):
        self.assert_invalid("message A = 1 { 1: u8 x; }\nmessage B = 2 { 1: A a; }", "only structs can be nested")

    def test_enum_needs_unsigned_type(self):
        self.assert_invalid("enum E : i8 { A = 0; }", "unsigned underlying type")

    def test_enum_value_must_fit(self):
        self.assert_invalid("enum E : u8 { A = 256; }", "does not fit into u8")

    def test_duplicate_enum_value(self):
        self.assert_invalid("enum E : u8 { A = 1; B = 1; }", "duplicate enum value 1")

    def test_reserved_field_name(self):
        self.assert_invalid("message M = 1 { 1: u8 class; }", "not a valid field name")

    def test_naming_conventions(self):
        self.assert_invalid("message lower = 1 {}", "PascalCase")
        self.assert_invalid("enum E : u8 { lower = 0; }", "UPPER_SNAKE_CASE")

    def test_message_id_range(self):
        self.assert_invalid("message M = 0 {}", "message id must be between")
        self.assert_invalid("message M = 0x10000 {}", "message id must be between")

    def test_message_ids_unique_across_files(self):
        first = parse_source("namespace a;\nmessage One = 5 {}", "a.msg")
        second = parse_source("namespace b;\nmessage Two = 5 {}", "b.msg")
        with self.assertRaises(ValidationError) as ctx:
            validate([first, second])
        self.assertIn("already used by One", str(ctx.exception))

    def test_collects_all_errors(self):
        with self.assertRaises(ValidationError) as ctx:
            check("message M = 1 { 1: nope a; 1: u8 b; }")
        self.assertEqual(len(ctx.exception.errors), 2)


if __name__ == "__main__":
    unittest.main()
