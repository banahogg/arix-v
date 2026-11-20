#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate SPIR-V grammar lookup tables."""

from __future__ import annotations

import json
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable


ROOT = Path(__file__).resolve().parents[1]
GRAMMAR_DIR = ROOT / "grammar-deps" / "spirv-headers" / "include" / "spirv" / "unified1"
OUT = ROOT / "src" / "grammar_tables.cpp"


# Keep this as the one place to add another supported extended instruction set.
EXT_INST_GRAMMARS = [
    ("extinst.glsl.std.450.grammar.json", "GLSL.std.450"),
    ("extinst.opencl.std.100.grammar.json", "OpenCL.std"),
    ("extinst.debuginfo.grammar.json", "DebugInfo"),
    ("extinst.opencl.debuginfo.100.grammar.json", "OpenCL.DebugInfo.100"),
    (
        "extinst.nonsemantic.shader.debuginfo.100.grammar.json",
        "NonSemantic.Shader.DebugInfo.100",
    ),
    ("extinst.nonsemantic.debugprintf.grammar.json", "NonSemantic.DebugPrintf"),
    ("extinst.nonsemantic.debugbreak.grammar.json", "NonSemantic.DebugBreak"),
    (
        "extinst.nonsemantic.clspvreflection.grammar.json",
        "NonSemantic.ClspvReflection",
    ),
    ("extinst.spv-amd-shader-ballot.grammar.json", "SPV_AMD_shader_ballot"),
    (
        "extinst.spv-amd-shader-explicit-vertex-parameter.grammar.json",
        "SPV_AMD_shader_explicit_vertex_parameter",
    ),
    (
        "extinst.spv-amd-shader-trinary-minmax.grammar.json",
        "SPV_AMD_shader_trinary_minmax",
    ),
    ("extinst.spv-amd-gcn-shader.grammar.json", "SPV_AMD_gcn_shader"),
]


ID_CATEGORIES = {
    "IdResult": "kIdResult",
    "IdResultType": "kIdResultType",
    "IdRef": "kIdRef",
    "IdScope": "kIdScope",
    "IdMemorySemantics": "kIdMemorySemantics",
}

LITERAL_CATEGORIES = {
    "LiteralInteger": "kLiteralInteger",
    "LiteralString": "kLiteralString",
    "LiteralContextDependentNumber": "kLiteralContextDependentNumber",
    "LiteralExtInstInteger": "kLiteralExtInstInteger",
    "LiteralSpecConstantOpInteger": "kLiteralSpecConstantOpInteger",
}

ONE_WORD_CATEGORIES = {
    "kIdResult",
    "kIdResultType",
    "kIdRef",
    "kIdScope",
    "kIdMemorySemantics",
    "kLiteralInteger",
    "kLiteralExtInstInteger",
}

WELL_KNOWN_KIND_NAMES = [
    "IdResult",
    "IdResultType",
    "IdRef",
    "LiteralInteger",
    "LiteralString",
    "LiteralContextDependentNumber",
    "LiteralExtInstInteger",
    "LiteralSpecConstantOpInteger",
]

TYPE_CLASS_VALUES = {
    "OpTypeVoid": "InstructionTypeClass::kVoid",
    "OpTypeBool": "InstructionTypeClass::kBool",
    "OpTypeInt": "InstructionTypeClass::kInt",
    "OpTypeFloat": "InstructionTypeClass::kFloat",
    "OpTypeVector": "InstructionTypeClass::kVector",
    "OpTypeMatrix": "InstructionTypeClass::kMatrix",
    "OpTypePointer": "InstructionTypeClass::kPointer",
    "OpTypeStruct": "InstructionTypeClass::kStruct",
    "OpTypeArray": "InstructionTypeClass::kArray",
    "OpTypeRuntimeArray": "InstructionTypeClass::kRuntimeArray",
    "OpTypeImage": "InstructionTypeClass::kImage",
    "OpTypeSampler": "InstructionTypeClass::kSampler",
    "OpTypeSampledImage": "InstructionTypeClass::kSampledImage",
    "OpTypeFunction": "InstructionTypeClass::kFunction",
}

TERMINATORS = {
    "OpBranch",
    "OpBranchConditional",
    "OpSwitch",
    "OpReturn",
    "OpReturnValue",
    "OpKill",
    "OpUnreachable",
    "OpTerminateInvocation",
    "OpTerminateRayKHR",
    "OpTerminateRayNV",
    "OpIgnoreIntersectionKHR",
    "OpIgnoreIntersectionNV",
}

DEBUG_SECTION_OPS = {
    "OpString",
    "OpSource",
    "OpSourceExtension",
    "OpSourceContinued",
    "OpName",
    "OpMemberName",
    "OpModuleProcessed",
}

TYPE_COMPONENT_OPS = {
    "OpTypeVector",
    "OpTypeMatrix",
    "OpTypeArray",
    "OpTypeRuntimeArray",
    "OpTypeImage",
    "OpTypeSampledImage",
}

SECTION_VALUES = {
    "Capabilities": 0,
    "Extensions": 1,
    "ExtInstImports": 2,
    "MemoryModel": 3,
    "EntryPoints": 4,
    "ExecutionModes": 5,
    "Debug": 6,
    "Annotations": 7,
    "TypesConstantsVars": 8,
    "FunctionDecls": 9,
    "FunctionBodies": 10,
}


@dataclass
class Enumerant:
    value: int
    param_kinds: list[int]


@dataclass
class OperandKind:
    name: str
    category: str
    enumerants: list[Enumerant] = field(default_factory=list)
    components: list[int] = field(default_factory=list)


@dataclass
class OperandSlot:
    kind: int
    quantifier: str


@dataclass
class Instruction:
    name: str
    opcode: int
    operands: list[OperandSlot]
    has_result: bool
    has_result_type: bool
    instruction_class: int
    result_bucket: int
    type_class: str
    section: int
    flags: list[str]
    min_word_count: int
    fixed_word_count: bool
    aux_word: int


@dataclass
class ExtInstSet:
    ident: str
    name: str
    instructions: list[Instruction]
    operand_slots: list[OperandSlot]


def load_json(path: Path) -> object:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def parse_int(value: object) -> int:
    if isinstance(value, str):
        return int(value, 0)
    return int(value)


def cpp_string(value: str) -> str:
    pieces = ['"']
    for ch in value:
        code = ord(ch)
        if ch == "\\":
            pieces.append("\\\\")
        elif ch == '"':
            pieces.append('\\"')
        elif ch == "\n":
            pieces.append("\\n")
        elif ch == "\r":
            pieces.append("\\r")
        elif ch == "\t":
            pieces.append("\\t")
        elif 32 <= code <= 126:
            pieces.append(ch)
        else:
            pieces.append(f"\\x{code:02x}")
    pieces.append('"')
    return "".join(pieces)


def cpp_ident(value: str) -> str:
    out = []
    for ch in value:
        if ch.isalnum():
            out.append(ch)
        else:
            out.append("_")
    ident = "".join(out).strip("_")
    if not ident or ident[0].isdigit():
        ident = "_" + ident
    return ident


def category_for(kind: dict[str, object]) -> str:
    category = kind.get("category")
    name = str(kind["kind"])
    if category == "Id":
        return ID_CATEGORIES.get(name, "kUnknown")
    if category == "ValueEnum":
        return "kValueEnum"
    if category == "BitEnum":
        return "kBitEnum"
    if category == "Literal":
        return LITERAL_CATEGORIES.get(name, "kUnknown")
    if category == "Composite":
        return "kComposite"
    return "kUnknown"


def quantifier_for(operand: dict[str, object]) -> str:
    quantifier = operand.get("quantifier")
    if quantifier == "?":
        return "kOptional"
    if quantifier == "*":
        return "kVariadic"
    return "kOne"


def resolve_kind(name: str, local_scope: dict[str, int], core_scope: dict[str, int]) -> int:
    if name in local_scope:
        return local_scope[name]
    if name in core_scope:
        return core_scope[name]
    raise KeyError(f"unknown operand kind {name!r}")


def dedupe_enumerants(
    raw_enumerants: Iterable[dict[str, object]],
    local_scope: dict[str, int],
    core_scope: dict[str, int],
) -> list[Enumerant]:
    deduped: dict[int, Enumerant] = {}
    for raw in raw_enumerants:
        value = parse_int(raw["value"])
        if value in deduped:
            continue
        params = [
            resolve_kind(str(param["kind"]), local_scope, core_scope)
            for param in raw.get("parameters", [])
        ]
        deduped[value] = Enumerant(value=value, param_kinds=params)
    return sorted(deduped.values(), key=lambda enumerant: enumerant.value)


def add_operand_kinds(
    raw_kinds: list[dict[str, object]],
    kinds: list[OperandKind],
    core_scope: dict[str, int],
    include_in_core_scope: bool,
) -> dict[str, int]:
    local_scope: dict[str, int] = {}
    for raw in raw_kinds:
        name = str(raw["kind"])
        index = len(kinds)
        kinds.append(OperandKind(name=name, category=category_for(raw)))
        local_scope[name] = index
        if include_in_core_scope:
            core_scope[name] = index

    for raw in raw_kinds:
        name = str(raw["kind"])
        kind = kinds[local_scope[name]]
        kind.enumerants = dedupe_enumerants(
            raw.get("enumerants", []), local_scope, core_scope
        )
        kind.components = [
            resolve_kind(str(component), local_scope, core_scope)
            for component in raw.get("bases", [])
        ]
    return local_scope


def is_fixed_one_word(kind: OperandKind) -> bool:
    if kind.category in ONE_WORD_CATEGORIES:
        return True
    if kind.category in {"kValueEnum", "kBitEnum"}:
        return all(len(enumerant.param_kinds) == 0 for enumerant in kind.enumerants)
    return False


def is_type_class_name(class_name: str) -> bool:
    return class_name in {"Type-Declaration", "Type-Declaring"}


def result_bucket_for(name: str, class_name: str) -> int:
    if is_type_class_name(class_name):
        return 0
    if class_name == "Constant-Creation":
        return 1
    if class_name in {"Function", "Control-Flow"} or name in {"OpLabel", "OpFunction"}:
        return 2
    return 3


def section_for(name: str, class_name: str) -> int:
    if name == "OpCapability":
        return SECTION_VALUES["Capabilities"]
    if name == "OpExtension":
        return SECTION_VALUES["Extensions"]
    if name == "OpExtInstImport":
        return SECTION_VALUES["ExtInstImports"]
    if name == "OpMemoryModel":
        return SECTION_VALUES["MemoryModel"]
    if name == "OpEntryPoint":
        return SECTION_VALUES["EntryPoints"]
    if name in {"OpExecutionMode", "OpExecutionModeId"}:
        return SECTION_VALUES["ExecutionModes"]
    if name in DEBUG_SECTION_OPS:
        return SECTION_VALUES["Debug"]
    if class_name == "Annotation":
        return SECTION_VALUES["Annotations"]
    if (
        is_type_class_name(class_name)
        or class_name == "Constant-Creation"
        or name in {"OpVariable", "OpUndef"}
    ):
        return SECTION_VALUES["TypesConstantsVars"]
    if name == "OpFunction":
        return SECTION_VALUES["FunctionDecls"]
    if name == "OpLabel":
        return SECTION_VALUES["FunctionBodies"]
    return SECTION_VALUES["Capabilities"]


def flags_for(name: str, class_name: str) -> list[str]:
    flags: list[str] = []
    if name in TERMINATORS or name.startswith("OpTerminate"):
        flags.append("kInstTerminator")
    if is_type_class_name(class_name) or name.startswith("OpType"):
        flags.append("kInstDeclaresType")
    if name == "OpVariable":
        flags.append("kInstVariable")
    if name == "OpFunction":
        flags.append("kInstFunction")
    if name == "OpFunctionEnd":
        flags.append("kInstFunctionEnd")
    if name == "OpLabel":
        flags.append("kInstLabel")
    if name == "OpExtInstImport":
        flags.append("kInstExtInstImport")
    if name == "OpString":
        flags.append("kInstString")
    if name == "OpExtInst":
        flags.append("kInstExtInst")
    if name in {"OpTypeInt", "OpTypeFloat"}:
        flags.append("kInstTypeWidth")
    if name in TYPE_COMPONENT_OPS:
        flags.append("kInstTypeComponent")
    if name == "OpTypePointer":
        flags.append("kInstTypePointer")
    if name == "OpTypeFunction":
        flags.append("kInstTypeFunction")
    if name in {"OpConstant", "OpSpecConstant"}:
        flags.append("kInstConstantNumber")
    return flags


def aux_word_for(
    name: str,
    flags: list[str],
    operands: list[OperandSlot],
    kinds: list[OperandKind],
) -> int:
    """Fixed word index of the operand the tracker reads at instruction end
    (type width, component/pointee/return type, or ext-inst-set import name);
    0 when the instruction has none. Only valid — and enforced here — when the
    operand and everything before it sit at fixed word positions, so the
    tracker can read words[aux_word] directly instead of re-walking the
    instruction with an OperandCursor."""
    if "kInstTypeWidth" in flags:
        target = "kLiteralInteger"
    elif (
        "kInstTypeComponent" in flags
        or "kInstTypePointer" in flags
        or "kInstTypeFunction" in flags
    ):
        target = "kIdRef"
    elif "kInstExtInstImport" in flags:
        target = "kLiteralString"
    else:
        return 0
    for index, slot in enumerate(operands):
        if kinds[slot.kind].category != target:
            continue
        if slot.quantifier != "kOne":
            raise ValueError(f"{name}: aux operand is not required")
        for prev in operands[:index]:
            if prev.quantifier != "kOne" or not is_fixed_one_word(kinds[prev.kind]):
                raise ValueError(f"{name}: aux operand not at a fixed word index")
        return 1 + index
    raise ValueError(f"{name}: no {target} operand for aux extraction")


def check_result_slots_lead(
    name: str,
    operands: list[OperandSlot],
    kinds: list[OperandKind],
    has_result: bool,
    has_result_type: bool,
) -> None:
    """The tracker reads the result type from words[1] and the result ID from
    words[1 + has_result_type]; enforce the SPIR-V layout rule that makes
    those positions fixed."""
    if has_result_type and kinds[operands[0].kind].category != "kIdResultType":
        raise ValueError(f"{name}: result type is not the first operand")
    result_slot = 1 if has_result_type else 0
    if has_result and (
        len(operands) <= result_slot
        or kinds[operands[result_slot].kind].category != "kIdResult"
    ):
        raise ValueError(f"{name}: result ID not directly after the result type")


def emit_flags(flags: list[str]) -> str:
    if not flags:
        return "0"
    return " | ".join(flags)


def build_instruction(
    raw: dict[str, object],
    local_scope: dict[str, int],
    core_scope: dict[str, int],
    kinds: list[OperandKind],
    instruction_class: int,
) -> Instruction:
    operands = [
        OperandSlot(
            kind=resolve_kind(str(operand["kind"]), local_scope, core_scope),
            quantifier=quantifier_for(operand),
        )
        for operand in raw.get("operands", [])
    ]
    min_word_count = 1 + sum(1 for slot in operands if slot.quantifier == "kOne")
    fixed_word_count = all(
        slot.quantifier == "kOne" and is_fixed_one_word(kinds[slot.kind])
        for slot in operands
    )
    name = str(raw["opname"])
    class_name = str(raw.get("class", ""))
    has_result = any(kinds[slot.kind].category == "kIdResult" for slot in operands)
    has_result_type = any(
        kinds[slot.kind].category == "kIdResultType" for slot in operands
    )
    check_result_slots_lead(name, operands, kinds, has_result, has_result_type)
    flags = flags_for(name, class_name)
    return Instruction(
        name=name,
        opcode=int(raw["opcode"]),
        operands=operands,
        has_result=has_result,
        has_result_type=has_result_type,
        instruction_class=instruction_class,
        result_bucket=result_bucket_for(name, class_name),
        type_class=TYPE_CLASS_VALUES.get(name, "InstructionTypeClass::kOther"),
        section=section_for(name, class_name),
        flags=flags,
        min_word_count=min_word_count,
        fixed_word_count=fixed_word_count,
        aux_word=aux_word_for(name, flags, operands, kinds),
    )


def dedupe_instructions(instructions: list[Instruction]) -> list[Instruction]:
    first_by_opcode: dict[int, Instruction] = {}
    for instruction in instructions:
        if instruction.opcode not in first_by_opcode:
            first_by_opcode[instruction.opcode] = instruction
    return sorted(first_by_opcode.values(), key=lambda instruction: instruction.opcode)


def flatten_operands(instructions: list[Instruction]) -> tuple[list[Instruction], list[OperandSlot]]:
    flattened: list[OperandSlot] = []
    copied: list[Instruction] = []
    for instruction in instructions:
        first = len(flattened)
        flattened.extend(instruction.operands)
        copied.append(
            Instruction(
                name=instruction.name,
                opcode=instruction.opcode,
                operands=[OperandSlot(first, "INDEX")],
                has_result=instruction.has_result,
                has_result_type=instruction.has_result_type,
                instruction_class=instruction.instruction_class,
                result_bucket=instruction.result_bucket,
                type_class=instruction.type_class,
                section=instruction.section,
                flags=instruction.flags,
                min_word_count=instruction.min_word_count,
                fixed_word_count=instruction.fixed_word_count,
                aux_word=instruction.aux_word,
            )
        )
        copied[-1].num_operands = len(instruction.operands)  # type: ignore[attr-defined]
        copied[-1].first_operand = first  # type: ignore[attr-defined]
    return copied, flattened


def checked_uint(name: str, value: int, limit: int) -> None:
    if not 0 <= value <= limit:
        raise ValueError(f"{name} value {value} exceeds {limit}")


def emit_operand_slot(slot: OperandSlot) -> str:
    return f"{{{slot.kind}, Quantifier::{slot.quantifier}}}"


def emit_instruction(instruction: Instruction) -> str:
    first_operand = getattr(instruction, "first_operand")
    num_operands = getattr(instruction, "num_operands")
    return (
        "{"
        f"{cpp_string(instruction.name)}, {instruction.opcode}, {first_operand}, "
        f"{instruction.min_word_count}, "
        f"static_cast<uint16_t>({emit_flags(instruction.flags)}), "
        f"{num_operands}, {str(instruction.has_result).lower()}, "
        f"{str(instruction.has_result_type).lower()}, {instruction.instruction_class}, "
        f"{instruction.result_bucket}, {instruction.type_class}, {instruction.section}, "
        f"{str(instruction.fixed_word_count).lower()}, {instruction.aux_word}"
        "}"
    )


def enum_constant_name(class_name: str) -> str:
    return "kInstructionClass" + "".join(
        part[:1].upper() + part[1:] for part in cpp_ident(class_name).split("_") if part
    )


# --- Cold-start priors (ratio backlog item 7) -------------------------------
# A prior is a SymbolCdf (symbol_coder.h) proportional to integer weights.
# prior_cdf mirrors SymbolCdfSetPrior exactly; tests/arixv_test.cpp rebuilds
# every emitted table with the C++ function and compares.
SYMBOL_SCALE = 1 << 15
OPCODE_PRIOR_LEGAL_WEIGHT = 64  # must equal kOpcodePriorLegalWeight

# Instructions the logical-layout rules allow in any section.
ANYWHERE_OPS = {"OpNop", "OpLine", "OpNoLine", "OpExtInst",
                "OpExtInstWithForwardRefsKHR"}


def prior_cdf(n: int, w: list[int]) -> tuple[int, ...]:
    def uniform(i: int) -> int:
        return i * (SYMBOL_SCALE // n) if i <= n else SYMBOL_SCALE + (i - n)
    total = sum(w)
    if total == 0:
        return (0,) * 16
    spread = SYMBOL_SCALE - n
    freq = [1 + (x * spread) // total for x in w]
    largest = 0
    for i in range(n):
        if freq[i] > freq[largest]:
            largest = i
    freq[largest] += SYMBOL_SCALE - sum(freq)
    d = [0] * 16
    c = 0
    for i in range(1, 16):
        if i <= n:
            c += freq[i - 1]
        cdf = c if i < n else uniform(i)
        d[i] = (cdf - uniform(i)) & 0xFFFF
    return tuple(d)


def opcode_legal_in_section(inst: "Instruction", section: int) -> bool:
    """Whether the module's logical layout allows inst while the tracker is in
    `section` (sections only advance). Soft: illegal opcodes stay codable."""
    if inst.name in ANYWHERE_OPS:
        return True
    advancing = inst.section != 0 or inst.name == "OpCapability"
    decls = SECTION_VALUES["FunctionDecls"]
    if section < decls:
        return advancing and inst.section >= section
    return (not advancing or inst.section >= decls
            or inst.name in {"OpVariable", "OpUndef"})


def main() -> None:
    global GRAMMAR_DIR
    args = sys.argv[1:]
    if "--grammar-dir" in args:
        index = args.index("--grammar-dir")
        if index + 1 >= len(args):
            raise SystemExit("--grammar-dir requires a path")
        GRAMMAR_DIR = Path(args[index + 1])
    if not GRAMMAR_DIR.is_dir():
        raise SystemExit(
            f"grammar directory is missing: {GRAMMAR_DIR}\n"
            "run: python3 scripts/fetch_pinned_repos.py "
            "--lock deps/grammar-sources.lock.json --dest grammar-deps"
        )
    core = load_json(GRAMMAR_DIR / "spirv.core.grammar.json")
    assert isinstance(core, dict)

    kinds: list[OperandKind] = []
    core_scope: dict[str, int] = {}
    core_local_scope = add_operand_kinds(
        core["operand_kinds"], kinds, core_scope, include_in_core_scope=True
    )

    ext_raw: list[tuple[str, str, dict[str, object], dict[str, int]]] = []
    for filename, canonical_name in EXT_INST_GRAMMARS:
        grammar = load_json(GRAMMAR_DIR / filename)
        assert isinstance(grammar, dict)
        local_scope = add_operand_kinds(
            grammar.get("operand_kinds", []),
            kinds,
            core_scope,
            include_in_core_scope=False,
        )
        ext_raw.append((filename, canonical_name, grammar, local_scope))

    instruction_classes = sorted(
        {str(inst.get("class", "")) for inst in core["instructions"]}
    )
    class_indices = {name: i for i, name in enumerate(instruction_classes)}

    core_instructions = dedupe_instructions(
        [
            build_instruction(
                inst,
                core_local_scope,
                core_scope,
                kinds,
                class_indices[str(inst.get("class", ""))],
            )
            for inst in core["instructions"]
        ]
    )
    core_instructions, core_operand_slots = flatten_operands(core_instructions)

    ext_sets: list[ExtInstSet] = []
    for _filename, canonical_name, grammar, local_scope in ext_raw:
        instructions = dedupe_instructions(
            [
                build_instruction(inst, local_scope, core_scope, kinds, 0)
                for inst in grammar.get("instructions", [])
            ]
        )
        flattened_instructions, operand_slots = flatten_operands(instructions)
        ext_sets.append(
            ExtInstSet(
                ident=cpp_ident(canonical_name),
                name=canonical_name,
                instructions=flattened_instructions,
                operand_slots=operand_slots,
            )
        )
    ext_sets.sort(key=lambda ext_set: ext_set.name)

    enum_param_kinds: list[int] = []
    enumerants = []
    composite_components: list[int] = []
    operand_kind_records = []
    for kind in kinds:
        first_enumerant = len(enumerants)
        for enumerant in kind.enumerants:
            first_param = len(enum_param_kinds)
            enum_param_kinds.extend(enumerant.param_kinds)
            enumerants.append((enumerant.value, first_param, len(enumerant.param_kinds)))
        first_component = len(composite_components)
        composite_components.extend(kind.components)
        operand_kind_records.append(
            (
                kind.name,
                kind.category,
                first_enumerant,
                len(kind.enumerants),
                first_component,
                len(kind.components),
            )
        )

    for i, record in enumerate(operand_kind_records):
        checked_uint(f"operand kind {i} first_enumerant", record[2], 65535)
        checked_uint(f"operand kind {i} num_enumerants", record[3], 65535)
        checked_uint(f"operand kind {i} first_component", record[4], 65535)
        checked_uint(f"operand kind {i} num_components", record[5], 255)
    for i, (_value, first_param, num_params) in enumerate(enumerants):
        checked_uint(f"enumerant {i} first_param", first_param, 65535)
        checked_uint(f"enumerant {i} num_params", num_params, 255)
    for instruction in core_instructions:
        checked_uint(f"instruction {instruction.name} first_operand", getattr(instruction, "first_operand"), 65535)
        checked_uint(f"instruction {instruction.name} num_operands", getattr(instruction, "num_operands"), 255)
        checked_uint(f"instruction {instruction.name} opcode", instruction.opcode, 65535)
        checked_uint(f"instruction {instruction.name} class", instruction.instruction_class, 255)
        checked_uint(f"instruction {instruction.name} result_bucket", instruction.result_bucket, 255)
        checked_uint(f"instruction {instruction.name} section", instruction.section, 255)
        checked_uint(f"instruction {instruction.name} min_word_count", instruction.min_word_count, 65535)
        checked_uint(f"instruction {instruction.name} aux_word", instruction.aux_word, 255)
    for ext_set in ext_sets:
        for instruction in ext_set.instructions:
            checked_uint(f"instruction {instruction.name} first_operand", getattr(instruction, "first_operand"), 65535)
            checked_uint(f"instruction {instruction.name} num_operands", getattr(instruction, "num_operands"), 255)
            checked_uint(f"instruction {instruction.name} opcode", instruction.opcode, 65535)
            checked_uint(f"instruction {instruction.name} result_bucket", instruction.result_bucket, 255)
            checked_uint(f"instruction {instruction.name} section", instruction.section, 255)
            checked_uint(f"instruction {instruction.name} min_word_count", instruction.min_word_count, 65535)
            checked_uint(f"instruction {instruction.name} aux_word", instruction.aux_word, 255)

    well_known = [core_scope[name] for name in WELL_KNOWN_KIND_NAMES]

    lines: list[str] = []
    lines.append("// Copyright (c) 2026, Eric Werness")
    lines.append("// SPDX-License-Identifier: BSD-2-Clause")
    lines.append("//")
    lines.append("// Derived from the Khronos SPIRV-Headers unified grammar JSON files:")
    lines.append("// Copyright (c) 2015-2024 The Khronos Group Inc.")
    lines.append("// The Khronos permission notice is reproduced in THIRD_PARTY.md and must")
    lines.append("// accompany copies of this file.")
    lines.append("")
    lines.append("// GENERATED by scripts/gen_grammar_tables.py — do not edit by hand.")
    lines.append("// Change the generator and rerun it instead; `make check-generated`")
    lines.append("// (part of `make test`) fails if this file and the generator disagree.")
    lines.append("")
    lines.append('#include "grammar_tables.h"')
    lines.append("")
    lines.append("#include <cstddef>")
    lines.append("#include <string_view>")
    lines.append("")
    lines.append("namespace arixv {")
    lines.append("namespace {")
    lines.append("")
    lines.append("enum InstructionClass : uint8_t {")
    for i, name in enumerate(instruction_classes):
        lines.append(f"  {enum_constant_name(name)} = {i},")
    lines.append("};")
    lines.append("")
    lines.append("constexpr const char* kInstructionClassNames[] = {")
    for name in instruction_classes:
        lines.append(f"  {cpp_string(name)},")
    lines.append("};")
    lines.append("")
    lines.append("constexpr uint16_t kEnumParamKinds[] = {")
    for value in enum_param_kinds:
        lines.append(f"  {value},")
    lines.append("};")
    lines.append("")
    lines.append("constexpr EnumerantDesc kEnumerants[] = {")
    for value, first_param, num_params in enumerants:
        lines.append(f"  {{{value}u, {first_param}, {num_params}}},")
    lines.append("};")
    lines.append("")
    lines.append("constexpr uint16_t kCompositeComponents[] = {")
    for value in composite_components:
        lines.append(f"  {value},")
    lines.append("};")
    lines.append("")
    lines.append("constexpr OperandKindDesc kOperandKinds[] = {")
    for name, category, first_enumerant, num_enumerants, first_component, num_components in operand_kind_records:
        lines.append(
            "  {"
            f"{cpp_string(name)}, OperandCategory::{category}, {first_enumerant}, "
            f"{num_enumerants}, {first_component}, {num_components}"
            "},"
        )
    lines.append("};")
    lines.append("")
    lines.append("constexpr OperandSlotDesc kCoreOperandSlots[] = {")
    for slot in core_operand_slots:
        lines.append(f"  {emit_operand_slot(slot)},")
    lines.append("};")
    lines.append("")
    lines.append("constexpr InstructionDesc kCoreInstructions[] = {")
    for instruction in core_instructions:
        lines.append(f"  {emit_instruction(instruction)},")
    lines.append("};")
    lines.append("")
    for ext_set in ext_sets:
        lines.append(f"constexpr OperandSlotDesc k{ext_set.ident}OperandSlots[] = {{")
        for slot in ext_set.operand_slots:
            lines.append(f"  {emit_operand_slot(slot)},")
        lines.append("};")
        lines.append("")
        lines.append(f"constexpr InstructionDesc k{ext_set.ident}Instructions[] = {{")
        for instruction in ext_set.instructions:
            lines.append(f"  {emit_instruction(instruction)},")
        lines.append("};")
        lines.append("")
    lines.append("constexpr ExtInstSetDesc kExtInstSets[] = {")
    for ext_set in ext_sets:
        lines.append(
            "  {"
            f"{cpp_string(ext_set.name)}, k{ext_set.ident}Instructions, "
            f"{len(ext_set.instructions)}, k{ext_set.ident}OperandSlots"
            "},"
        )
    lines.append("};")
    lines.append("")
    lines.append("constexpr WellKnownKinds kWellKnownKinds = {")
    for value in well_known:
        lines.append(f"  {value},")
    lines.append("};")
    lines.append("")
    lines.append("template <typename T, size_t N>")
    lines.append("constexpr size_t ArraySize(const T (&)[N]) {")
    lines.append("  return N;")
    lines.append("}")
    lines.append("")
    lines.append("}  // namespace")
    lines.append("")
    lines.append("const char* InstructionClassName(uint8_t instruction_class) {")
    lines.append("  if (instruction_class >= ArraySize(kInstructionClassNames)) {")
    lines.append("    return nullptr;")
    lines.append("  }")
    lines.append("  return kInstructionClassNames[instruction_class];")
    lines.append("}")
    lines.append("")
    lines.append("uint8_t NumInstructionClasses() {")
    lines.append("  return static_cast<uint8_t>(ArraySize(kInstructionClassNames));")
    lines.append("}")
    lines.append("")
    lines.append("const InstructionDesc* FindInstruction(uint32_t opcode) {")
    lines.append("  size_t lo = 0;")
    lines.append("  size_t hi = ArraySize(kCoreInstructions);")
    lines.append("  while (lo < hi) {")
    lines.append("    const size_t mid = lo + (hi - lo) / 2;")
    lines.append("    const InstructionDesc& inst = kCoreInstructions[mid];")
    lines.append("    if (inst.opcode == opcode) {")
    lines.append("      return &inst;")
    lines.append("    }")
    lines.append("    if (inst.opcode < opcode) {")
    lines.append("      lo = mid + 1;")
    lines.append("    } else {")
    lines.append("      hi = mid;")
    lines.append("    }")
    lines.append("  }")
    lines.append("  return nullptr;")
    lines.append("}")
    lines.append("")
    lines.append("uint16_t NumCoreInstructions() {")
    lines.append("  return static_cast<uint16_t>(ArraySize(kCoreInstructions));")
    lines.append("}")
    lines.append("")
    lines.append("const InstructionDesc& GetCoreInstruction(uint16_t index) {")
    lines.append("  return kCoreInstructions[index];")
    lines.append("}")
    lines.append("")
    lines.append("int FindInstructionIndex(uint32_t opcode) {")
    lines.append("  const InstructionDesc* inst = FindInstruction(opcode);")
    lines.append("  if (inst == nullptr) {")
    lines.append("    return -1;")
    lines.append("  }")
    lines.append("  return static_cast<int>(inst - kCoreInstructions);")
    lines.append("}")
    lines.append("")
    lines.append("const OperandKindDesc& GetOperandKind(uint16_t kind_index) {")
    lines.append("  return kOperandKinds[kind_index];")
    lines.append("}")
    lines.append("")
    lines.append("uint16_t NumOperandKinds() {")
    lines.append("  return static_cast<uint16_t>(ArraySize(kOperandKinds));")
    lines.append("}")
    lines.append("")
    lines.append("const OperandSlotDesc* GetOperandSlots(const InstructionDesc& inst) {")
    lines.append("  return kCoreOperandSlots + inst.first_operand;")
    lines.append("}")
    lines.append("")
    lines.append("const uint16_t* GetEnumParamKinds(const EnumerantDesc& e) {")
    lines.append("  return kEnumParamKinds + e.first_param;")
    lines.append("}")
    lines.append("")
    lines.append("const uint16_t* GetCompositeComponents(const OperandKindDesc& kind) {")
    lines.append("  return kCompositeComponents + kind.first_component;")
    lines.append("}")
    lines.append("")
    lines.append("const EnumerantDesc* FindEnumerant(const OperandKindDesc& kind,")
    lines.append("                                    uint32_t value) {")
    lines.append("  size_t lo = kind.first_enumerant;")
    lines.append("  size_t hi = lo + kind.num_enumerants;")
    lines.append("  while (lo < hi) {")
    lines.append("    const size_t mid = lo + (hi - lo) / 2;")
    lines.append("    const EnumerantDesc& enumerant = kEnumerants[mid];")
    lines.append("    if (enumerant.value == value) {")
    lines.append("      return &enumerant;")
    lines.append("    }")
    lines.append("    if (enumerant.value < value) {")
    lines.append("      lo = mid + 1;")
    lines.append("    } else {")
    lines.append("      hi = mid;")
    lines.append("    }")
    lines.append("  }")
    lines.append("  return nullptr;")
    lines.append("}")
    lines.append("")
    lines.append("const EnumerantDesc& GetEnumerant(const OperandKindDesc& kind,")
    lines.append("                                  uint16_t index) {")
    lines.append("  return kEnumerants[kind.first_enumerant + index];")
    lines.append("}")
    lines.append("")
    lines.append("const ExtInstSetDesc* FindExtInstSet(std::string_view import_name) {")
    lines.append("  const ExtInstSetDesc* best_prefix = nullptr;")
    lines.append("  size_t best_prefix_len = 0;")
    lines.append("  for (const ExtInstSetDesc& set : kExtInstSets) {")
    lines.append("    const std::string_view name(set.name);")
    lines.append("    if (import_name == name) {")
    lines.append("      return &set;")
    lines.append("    }")
    lines.append("    if (import_name.size() > name.size() &&")
    lines.append("        import_name.substr(0, name.size()) == name &&")
    lines.append("        name.size() > best_prefix_len) {")
    lines.append("      best_prefix = &set;")
    lines.append("      best_prefix_len = name.size();")
    lines.append("    }")
    lines.append("  }")
    lines.append("  return best_prefix;")
    lines.append("}")
    lines.append("")
    lines.append("uint16_t NumExtInstSets() {")
    lines.append("  return static_cast<uint16_t>(ArraySize(kExtInstSets));")
    lines.append("}")
    lines.append("")
    lines.append("int FindExtInstSetIndex(const ExtInstSetDesc* set) {")
    lines.append("  for (size_t i = 0; i < ArraySize(kExtInstSets); ++i) {")
    lines.append("    if (&kExtInstSets[i] == set) {")
    lines.append("      return static_cast<int>(i);")
    lines.append("    }")
    lines.append("  }")
    lines.append("  return -1;")
    lines.append("}")
    lines.append("")
    lines.append("const InstructionDesc* FindExtInstruction(const ExtInstSetDesc& set,")
    lines.append("                                          uint32_t ext_opcode) {")
    lines.append("  size_t lo = 0;")
    lines.append("  size_t hi = set.num_instructions;")
    lines.append("  while (lo < hi) {")
    lines.append("    const size_t mid = lo + (hi - lo) / 2;")
    lines.append("    const InstructionDesc& inst = set.instructions[mid];")
    lines.append("    if (inst.opcode == ext_opcode) {")
    lines.append("      return &inst;")
    lines.append("    }")
    lines.append("    if (inst.opcode < ext_opcode) {")
    lines.append("      lo = mid + 1;")
    lines.append("    } else {")
    lines.append("      hi = mid;")
    lines.append("    }")
    lines.append("  }")
    lines.append("  return nullptr;")
    lines.append("}")
    lines.append("")
    # String-model knowledge: names the grammar itself spells out.
    def collect_extensions(node: object, out: set[str]) -> None:
        if isinstance(node, dict):
            for key, value in node.items():
                if key == "extensions" and isinstance(value, list):
                    out.update(str(v) for v in value)
                else:
                    collect_extensions(value, out)
        elif isinstance(node, list):
            for value in node:
                collect_extensions(value, out)

    extension_names: set[str] = set()
    collect_extensions(core, extension_names)
    grammar_strings = sorted(extension_names | {s.name for s in ext_sets})
    # Every enumerant name of every core operand kind, in grammar order, first
    # occurrence only: a rule with no selection among kinds.
    enumerant_names: list[str] = []
    seen_enumerants: set[str] = set()
    for kind in core["operand_kinds"]:
        for e in kind.get("enumerants", []):
            name = str(e["enumerant"])
            if name not in seen_enumerants:
                seen_enumerants.add(name)
                enumerant_names.append(name)
    for name in grammar_strings + enumerant_names:
        if not re.fullmatch(r"[A-Za-z0-9_.]+", name):
            raise SystemExit(f"unexpected characters in grammar string {name!r}")
    checked_uint("grammar string count", len(grammar_strings), 65535)
    lines.append("// Every extension name the core grammar lists plus every generated")
    lines.append("// extended-instruction-set name, sorted: whole-string dictionary for")
    lines.append("// OpExtension / OpExtInstImport-style literals.")
    lines.append("constexpr const char* kGrammarStrings[] = {")
    for name in grammar_strings:
        lines.append(f'    "{name}",')
    lines.append("};")
    lines.append("")
    lines.append("// String match-model primer: every core enumerant name (grammar order,")
    lines.append("// first occurrence), then the dictionary above, each NUL-terminated.")
    lines.append("constexpr char kStringPrimer[] =")
    for name in enumerant_names + grammar_strings:
        lines.append(f'    "{name}\\0"')
    lines.append("    ;")
    lines.append("")
    # The primed order-2 match table, so decoders need no runtime (or
    # process-wide) initialization. Must match StringMatchHash in model.cpp
    # (tests/arixv_test.cpp rebuilds it from the primer and compares).
    match_bits = 10
    primer = b"".join(n.encode() + b"\0" for n in enumerant_names + grammar_strings)
    table = [0] * (1 << match_bits)
    for v in range(2, len(primer)):
        key = (primer[v - 2] << 8) | primer[v - 1]
        table[((key * 0x9E3779B1) & 0xFFFFFFFF) >> (32 - match_bits)] = v
    lines.append(f"constexpr int kStringMatchTableBits = {match_bits};")
    lines.append("// Per order-2 context hash: primer position of the byte that last")
    lines.append("// followed that context, 0 = none.")
    lines.append("constexpr uint32_t kPrimedStringMatch[] = {")
    for i in range(0, len(table), 12):
        lines.append("    " + ", ".join(str(x) for x in table[i:i + 12]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("const uint32_t* GetPrimedStringMatchTable(int* bits) {")
    lines.append("  *bits = kStringMatchTableBits;")
    lines.append("  return kPrimedStringMatch;")
    lines.append("}")
    lines.append("")
    lines.append("uint16_t NumGrammarStrings() {")
    lines.append("  return static_cast<uint16_t>(ArraySize(kGrammarStrings));")
    lines.append("}")
    lines.append("")
    lines.append("const char* GetGrammarString(uint16_t index) {")
    lines.append("  return kGrammarStrings[index];")
    lines.append("}")
    lines.append("")
    lines.append("std::string_view GetStringPrimer() {")
    lines.append("  // The literal's own terminating NUL is not part of the primer.")
    lines.append("  return std::string_view(kStringPrimer, sizeof(kStringPrimer) - 1);")
    lines.append("}")
    lines.append("")
    # Cold-start priors: a deduplicated pool of SymbolCdf tables, the
    # per-section opcode-index tree priors, and uniform-over-[0, n) priors for
    # 8-bit index trees (enum and extended-instruction indices).
    pool: list[tuple[int, ...]] = [(0,) * 16]
    pool_index = {pool[0]: 0}

    def intern(cdf: tuple[int, ...]) -> int:
        if cdf not in pool_index:
            pool_index[cdf] = len(pool)
            pool.append(cdf)
        return pool_index[cdf]

    num_sections = len(SECTION_VALUES)
    if len(core_instructions) > 1024:
        raise SystemExit("opcode prior assumes at most 1024 core instructions")
    legal_masks: list[list[int]] = []
    section_priors: list[list[int]] = []
    for section in range(num_sections):
        weight = [0] * 1024
        mask = [0] * 32
        for i, inst in enumerate(core_instructions):
            if opcode_legal_in_section(inst, section):
                weight[i] = OPCODE_PRIOR_LEGAL_WEIGHT
                mask[i >> 5] |= 1 << (i & 31)
            else:
                weight[i] = 1
        root = intern(prior_cdf(4, [sum(weight[t << 8:(t + 1) << 8]) for t in range(4)]))
        mids = []
        leaves = []
        for top in range(4):
            mids.append(intern(prior_cdf(16, [
                sum(weight[(top << 8) | (m << 4):((top << 8) | (m << 4)) + 16])
                for m in range(16)])))
        for leaf in range(64):
            leaves.append(intern(prior_cdf(16, weight[leaf << 4:(leaf + 1) << 4])))
        section_priors.append([root] + mids + leaves)
        legal_masks.append(mask)
    index_root = [intern(prior_cdf(16, [max(0, min(16, n - 16 * h)) for h in range(16)]))
                  for n in range(257)]
    index_leaf = [intern(prior_cdf(16, [1 if l < r else 0 for l in range(16)])) if r else 0
                  for r in range(16)]
    # Header priors. Versions: major is the grammar's; minor is uniform over
    # 0..the grammar's minor. Generator IDs: the registered tool IDs in
    # spir-v.xml (one level above the grammar directory), which are
    # contiguous from 0.
    major = int(core["major_version"])
    minor = int(core["minor_version"])
    if major > 15 or minor > 15:
        raise SystemExit("version prior assumes 4-bit major/minor")
    version_major_prior = intern(prior_cdf(16, [1 if v == major else 0 for v in range(16)]))
    version_minor_prior = intern(prior_cdf(16, [1 if v <= minor else 0 for v in range(16)]))
    registry = ET.parse(GRAMMAR_DIR.parent / "spir-v.xml").getroot()
    vendor_ids = sorted(
        parse_int(e.get("value"))
        for block in registry.iter("ids") if block.get("type") == "vendor"
        for e in block.iter("id")
    )
    if vendor_ids != list(range(len(vendor_ids))) or len(vendor_ids) > 256:
        raise SystemExit("expected registered generator IDs 0..n-1 with n <= 256")
    checked_uint("prior pool size", len(pool), 65535)
    lines.append("// Cold-start prior pool: SymbolCdf contents (offsets from uniform,")
    lines.append("// warm-up count 0). Entry 0 is the uniform table.")
    lines.append("constexpr uint16_t kPriorCdfs[][16] = {")
    for cdf in pool:
        lines.append("    {" + ", ".join(str(x) for x in cdf) + "},")
    lines.append("};")
    lines.append("")
    lines.append(f"constexpr uint32_t kOpcodePriorLegalWeightGenerated = {OPCODE_PRIOR_LEGAL_WEIGHT};")
    lines.append("static_assert(kOpcodePriorLegalWeightGenerated == kOpcodePriorLegalWeight,")
    lines.append('              "rerun scripts/gen_grammar_tables.py");')
    lines.append("// Per section: which core instruction indices the logical layout allows")
    lines.append("// (bit i of word i / 32), and the opcode-index tree prior built from it")
    lines.append("// (pool indices: root, mid[0..3], leaf[0..63]).")
    lines.append(f"constexpr uint32_t kOpcodeSectionLegal[{num_sections}][32] = {{")
    for mask in legal_masks:
        lines.append("    {" + ", ".join(f"0x{x:08x}u" for x in mask) + "},")
    lines.append("};")
    lines.append(f"constexpr uint16_t kOpcodeSectionPrior[{num_sections}][69] = {{")
    for row in section_priors:
        lines.append("    {" + ", ".join(str(x) for x in row) + "},")
    lines.append("};")
    lines.append("// 8-bit index trees over [0, n): root prior by n (n >= 256: uniform),")
    lines.append("// prior of the partial leaf holding n % 16 valid low nibbles.")
    lines.append("constexpr uint16_t kIndexRootPrior[257] = {")
    for i in range(0, 257, 16):
        lines.append("    " + ", ".join(str(x) for x in index_root[i:i + 16]) + ",")
    lines.append("};")
    lines.append("constexpr uint16_t kIndexLeafPrior[16] = {")
    lines.append("    " + ", ".join(str(x) for x in index_leaf) + ",")
    lines.append("};")
    lines.append("")
    lines.append(f"constexpr uint16_t kVersionMajorPrior = {version_major_prior};")
    lines.append(f"constexpr uint16_t kVersionMinorPrior = {version_minor_prior};")
    lines.append("// Tool IDs registered in spir-v.xml (contiguous from 0).")
    lines.append(f"constexpr uint32_t kNumRegisteredGenerators = {len(vendor_ids)};")
    lines.append("")
    lines.append("uint16_t GetVersionMajorPrior() { return kVersionMajorPrior; }")
    lines.append("")
    lines.append("uint16_t GetVersionMinorPrior() { return kVersionMinorPrior; }")
    lines.append("")
    lines.append("uint32_t NumRegisteredGenerators() { return kNumRegisteredGenerators; }")
    lines.append("")
    lines.append("const uint16_t* GetPriorCdf(uint16_t index) { return kPriorCdfs[index]; }")
    lines.append("")
    lines.append("uint16_t NumPriorCdfs() {")
    lines.append("  return static_cast<uint16_t>(ArraySize(kPriorCdfs));")
    lines.append("}")
    lines.append("")
    lines.append("const uint16_t* GetOpcodeSectionPrior(uint8_t section) {")
    lines.append("  return kOpcodeSectionPrior[section];")
    lines.append("}")
    lines.append("")
    lines.append("const uint32_t* GetOpcodeSectionLegal(uint8_t section) {")
    lines.append("  return kOpcodeSectionLegal[section];")
    lines.append("}")
    lines.append("")
    lines.append("uint16_t GetIndexRootPrior(uint32_t n) {")
    lines.append("  return kIndexRootPrior[n < 256 ? n : 256];")
    lines.append("}")
    lines.append("")
    lines.append("uint16_t GetIndexLeafPrior(uint32_t partial) {")
    lines.append("  return kIndexLeafPrior[partial & 15u];")
    lines.append("}")
    lines.append("")
    lines.append("const WellKnownKinds& GetWellKnownKinds() {")
    lines.append("  return kWellKnownKinds;")
    lines.append("}")
    lines.append("")
    lines.append("}  // namespace arixv")
    lines.append("")

    generated = "\n".join(lines)
    if "--check" in sys.argv[1:]:
        if OUT.read_text(encoding="utf-8") != generated:
            print(
                f"{OUT.relative_to(ROOT)} is out of sync with the generator; "
                "rerun scripts/gen_grammar_tables.py",
                file=sys.stderr,
            )
            sys.exit(1)
        print(f"{OUT.relative_to(ROOT)} matches the generator")
        return
    OUT.write_text(generated, encoding="utf-8")

    print(f"core_instructions={len(core_instructions)}")
    print(f"operand_kinds={len(kinds)}")
    print(f"ext_inst_sets={len(ext_sets)}")


if __name__ == "__main__":
    main()
