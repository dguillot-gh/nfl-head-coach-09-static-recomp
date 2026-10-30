"""Rewrites SPIR-V for old Adreno 5xx drivers (LG V30, 2018 driver), whose compiler crashes on some
instructions. Found with tools/vkprobe/vkpipeprobe on the V30:

  - vector OpSelect (crash in generateComponentSelect): split into one scalar OpSelect per component,
    then OpCompositeConstruct the vector. A condition built by splatting one bool (OpCompositeConstruct
    of identical components) is used directly as the scalar condition.
  - NMin/NMax -> FMin/FMax (differ only when an operand is NaN).

Usage: python tools/spirv_adreno5xx.py <in.spv> <out.spv>   (prints the counts of each rewrite)
Also importable: patch(words) -> (words, counts).
"""
import struct
import sys

OP_EXT_INST_IMPORT = 11
OP_EXT_INST = 12
OP_TYPE_BOOL = 20
OP_TYPE_VECTOR = 23
OP_FUNCTION = 54
OP_COMPOSITE_CONSTRUCT = 80
OP_COMPOSITE_EXTRACT = 81
OP_SELECT = 169
GLSL_FMIN, GLSL_FMAX, GLSL_NMIN, GLSL_NMAX = 37, 40, 79, 80
REPLACE_EXT = {GLSL_NMIN: GLSL_FMIN, GLSL_NMAX: GLSL_FMAX}


def instructions(words):
    i = 5
    while i < len(words):
        length = words[i] >> 16
        if length == 0:
            raise ValueError("bad SPIR-V instruction length")
        yield words[i:i + length]
        i += length


def encode(opcode, operands):
    return [((len(operands) + 1) << 16) | opcode] + operands


def patch(words):
    header = words[:5]
    bound = header[3]
    insts = list(instructions(words))

    vectors = {}      # type id -> (component type id, count)
    bool_vectors = {} # count -> bool vector type id
    bool_type = None
    glsl = set()
    splats = {}       # bool vector id -> the scalar it splats
    for inst in insts:
        opcode = inst[0] & 0xFFFF
        if opcode == OP_TYPE_BOOL:
            bool_type = inst[1]
        elif opcode == OP_TYPE_VECTOR:
            vectors[inst[1]] = (inst[2], inst[3])
        elif opcode == OP_EXT_INST_IMPORT:
            name = struct.pack(f"<{len(inst) - 2}I", *inst[2:]).split(b"\0")[0]
            if name == b"GLSL.std.450":
                glsl.add(inst[1])
        elif opcode == OP_COMPOSITE_CONSTRUCT and len(inst) > 4 and len(set(inst[3:])) == 1:
            splats[inst[2]] = inst[3]
    for type_id, (component, count) in vectors.items():
        if component == bool_type:
            bool_vectors[count] = type_id

    counts = {"vector selects": 0, "nmin/nmax": 0}
    out = []
    for inst in insts:
        opcode = inst[0] & 0xFFFF
        if opcode == OP_EXT_INST and inst[3] in glsl and inst[4] in REPLACE_EXT:
            inst = list(inst)
            inst[4] = REPLACE_EXT[inst[4]]
            counts["nmin/nmax"] += 1
        elif opcode == OP_SELECT and inst[1] in vectors:
            result_type, result, condition, a, b = inst[1:6]
            component, count = vectors[result_type]
            scalar_condition = splats.get(condition)
            parts = []
            for c in range(count):
                if scalar_condition is not None:
                    cond = scalar_condition
                else:
                    cond = bound
                    bound += 1
                    out.append(encode(OP_COMPOSITE_EXTRACT, [bool_type, cond, condition, c]))
                ea, eb, sel = bound, bound + 1, bound + 2
                bound += 3
                out.append(encode(OP_COMPOSITE_EXTRACT, [component, ea, a, c]))
                out.append(encode(OP_COMPOSITE_EXTRACT, [component, eb, b, c]))
                out.append(encode(OP_SELECT, [component, sel, cond, ea, eb]))
                parts.append(sel)
            out.append(encode(OP_COMPOSITE_CONSTRUCT, [result_type, result] + parts))
            counts["vector selects"] += 1
            continue
        out.append(list(inst))

    header = list(header)
    header[3] = bound
    flat = header + [w for inst in out for w in inst]
    return flat, counts


def main():
    data = open(sys.argv[1], "rb").read()
    words = list(struct.unpack(f"<{len(data) // 4}I", data))
    if words[0] != 0x07230203:
        sys.exit("not SPIR-V")
    words, counts = patch(words)
    open(sys.argv[2], "wb").write(struct.pack(f"<{len(words)}I", *words))
    print(", ".join(f"{k}: {v}" for k, v in counts.items()))


if __name__ == "__main__":
    main()
