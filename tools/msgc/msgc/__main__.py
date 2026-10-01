"""Command line entry point: python -m msgc --lang cpp --out-dir build/gen schemas/*.msg"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import __version__
from .backends import BACKENDS
from .lexer import SchemaError
from .parser import parse_file
from .validate import ValidationError, validate


def _write_if_changed(path: Path, content: str) -> bool:
    # Leaving untouched files alone keeps their timestamps, so the build
    # system does not recompile everything that includes them just because
    # the generator ran again.
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return False
    path.write_text(content, encoding="utf-8")
    return True


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="msgc", description="Compile .msg schemas into source code.")
    parser.add_argument("schemas", nargs="+", type=Path, help="schema files to compile")
    parser.add_argument("--lang", choices=sorted(BACKENDS), action="append",
                        help="target language, can be given more than once (default: all)")
    parser.add_argument("--out-dir", type=Path, help="where generated files go")
    parser.add_argument("--check", action="store_true", help="only parse and validate, write nothing")
    parser.add_argument("--list-outputs", action="store_true",
                        help="print the files that would be generated and exit")
    parser.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    args = parser.parse_args(argv)

    try:
        schemas = [parse_file(path) for path in args.schemas]
        validate(schemas)
    except (SchemaError, ValidationError) as error:
        print(error, file=sys.stderr)
        return 1
    except OSError as error:
        print(f"msgc: {error}", file=sys.stderr)
        return 1

    if args.check:
        return 0

    languages = args.lang or sorted(BACKENDS)
    if args.list_outputs:
        for lang in languages:
            for schema in schemas:
                print("\n".join(BACKENDS[lang].output_names(schema)))
        return 0

    if args.out_dir is None:
        parser.error("--out-dir is required unless --check or --list-outputs is used")

    args.out_dir.mkdir(parents=True, exist_ok=True)
    for lang in languages:
        for schema in schemas:
            for name, content in BACKENDS[lang].generate(schema).items():
                if _write_if_changed(args.out_dir / name, content):
                    print(f"msgc: wrote {args.out_dir / name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
