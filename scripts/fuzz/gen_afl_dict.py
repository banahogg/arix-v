#!/usr/bin/env python3
"""Generate an AFL++ dictionary from the SPIR-V core grammar.

Emits one token per instruction: the little-endian first word of a minimal
encoding, (word_count << 16) | opcode, where word_count assumes one word per
non-optional operand. Also emits the module magic (both endiannesses) and the
version words. Token values are what a mutator cannot guess byte-by-byte;
coverage feedback does the rest.

Usage: gen_afl_dict.py [grammar.json] > spirv.dict
"""

import json
import sys
from pathlib import Path

DEFAULT_GRAMMAR = (
    Path(__file__).resolve().parents[2]
    / "grammar-deps/spirv-headers/include/spirv/unified1/spirv.core.grammar.json"
)


def afl_escape(data: bytes) -> str:
    return "".join(f"\\x{b:02x}" for b in data)


def word_le(value: int) -> bytes:
    return value.to_bytes(4, "little")


def main() -> int:
    grammar_path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_GRAMMAR
    grammar = json.loads(grammar_path.read_text())

    tokens: dict[str, bytes] = {
        "magic_le": word_le(0x07230203),
        "magic_be": bytes(reversed(word_le(0x07230203))),
    }
    for minor in range(7):
        tokens[f"version_1_{minor}"] = word_le(0x00010000 | (minor << 8))

    seen_opcodes = set()
    for inst in grammar["instructions"]:
        opcode = inst["opcode"]
        if opcode in seen_opcodes:  # grammar lists aliases under one opcode
            continue
        seen_opcodes.add(opcode)
        required = sum(
            1 for op in inst.get("operands", []) if "quantifier" not in op
        )
        word_count = 1 + required
        tokens[f"op_{inst['opname']}"] = word_le((word_count << 16) | opcode)

    print(f"# AFL++ dictionary generated from {grammar_path.name}")
    for name, value in tokens.items():
        print(f'{name}="{afl_escape(value)}"')
    return 0


if __name__ == "__main__":
    sys.exit(main())
