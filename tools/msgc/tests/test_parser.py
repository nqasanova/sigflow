import unittest

from msgc import SchemaError, parse_source
from msgc.model import EnumDef, Modifier, StructDef

SAMPLE = """
// leading comment
namespace demo.inner;

enum Color : u8 {
    RED = 0;
    GREEN = 0x10;  // hex works too
}

struct Point {
    1: i32 x;
    2: i32 y;
}

message Shape = 0x0A01 {
    1: Color color;
    2: repeated Point points;
    3: optional string label;
}
"""


class ParserTest(unittest.TestCase):
    def test_parses_all_definition_kinds(self):
        schema = parse_source(SAMPLE, "sample.msg")

        self.assertEqual(schema.namespace, ("demo", "inner"))
        self.assertEqual([d.name for d in schema.definitions], ["Color", "Point", "Shape"])

        color = schema.definitions[0]
        self.assertIsInstance(color, EnumDef)
        self.assertEqual([(v.name, v.value) for v in color.values], [("RED", 0), ("GREEN", 16)])

        shape = schema.definitions[2]
        self.assertIsInstance(shape, StructDef)
        self.assertEqual(shape.message_id, 0x0A01)
        self.assertEqual(
            [(f.tag, f.name, f.type_name, f.modifier) for f in shape.fields],
            [
                (1, "color", "Color", Modifier.REQUIRED),
                (2, "points", "Point", Modifier.REPEATED),
                (3, "label", "string", Modifier.OPTIONAL),
            ],
        )

    def test_struct_has_no_message_id(self):
        schema = parse_source(SAMPLE)
        point = schema.lookup("Point")
        self.assertFalse(point.is_message)
        self.assertEqual(len(schema.messages), 1)

    def test_error_points_at_line_and_column(self):
        source = "namespace a;\nmessage M = 1 {\n    1: u8 x\n}\n"
        with self.assertRaises(SchemaError) as ctx:
            parse_source(source, "broken.msg")
        # The missing ';' is noticed at the closing brace on line 4.
        self.assertEqual((ctx.exception.loc.line, ctx.exception.loc.column), (4, 1))
        self.assertIn("broken.msg:4:1", str(ctx.exception))
        self.assertIn("expected ';'", str(ctx.exception))

    def test_requires_namespace_first(self):
        with self.assertRaises(SchemaError):
            parse_source("message M = 1 {}")

    def test_rejects_unknown_keyword(self):
        with self.assertRaises(SchemaError) as ctx:
            parse_source("namespace a;\nunion U {}")
        self.assertIn("unknown keyword 'union'", ctx.exception.message)

    def test_rejects_stray_characters(self):
        with self.assertRaises(SchemaError) as ctx:
            parse_source("namespace a;\n@")
        self.assertIn("unexpected character", ctx.exception.message)

    def test_reports_unexpected_end_of_file(self):
        with self.assertRaises(SchemaError) as ctx:
            parse_source("namespace a;\nstruct S {\n 1: u8 x;")
        self.assertIn("end of file", ctx.exception.message)


if __name__ == "__main__":
    unittest.main()
