"""Registry of code generator backends.

A backend is a module with two functions:

    output_names(schema) -> list[str]      file names it will write
    generate(schema)     -> dict[str, str] file name to file content

Adding a new target language means writing one module like that and
listing it here, nothing else in msgc has to change.
"""

from types import ModuleType

from . import cpp, python

BACKENDS: dict[str, ModuleType] = {
    "cpp": cpp,
    "python": python,
}
