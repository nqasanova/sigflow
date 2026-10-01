"""msgc, the sigflow message compiler.

Turns .msg schema files into C++ and Python code. See the README in the
repository root for the schema language.
"""

from .lexer import SchemaError
from .parser import parse_file, parse_source
from .validate import ValidationError, validate

__all__ = ["SchemaError", "ValidationError", "parse_file", "parse_source", "validate"]
__version__ = "0.3.0"
