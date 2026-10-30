"""Compiles translated shaders (tools/xenos_to_hlsl.cpp output) to SPIR-V for Vulkan 1.0 with DXC,
rewrites the instructions old Adreno 5xx drivers crash on (tools/spirv_adreno5xx.py), then validates
each with spirv-val. One SPIR-V set serves every device.

Usage: python tools/shaders_to_spirv.py <hlsl folder> <spirv folder> --dxc <dxc.exe> [--spirv-opt <spirv-opt.exe>]
       [--val <spirv-val.exe>]   (spirv-opt and spirv-val ship with the Android NDK, in shader-tools)

Vulkan bindings (descriptor set 0), from the translator's D3D registers:
  b0, b1          -> bindings 0, 1     (FloatConstants, DrawState)
  t0..t15         -> bindings 2..17    (vertex streams, read-only storage buffers)
  t16..t79        -> bindings 18..81   (textures, including the 2D fallback views)
  s0..s31         -> bindings 100..131 (samplers)

-HV 2018 keeps HLSL's older per-component ternary (vector ? a : b), which the translator writes.
"""
import argparse
import pathlib
import struct
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import spirv_adreno5xx  # noqa: E402

T_SHIFT = 2
S_SHIFT = 100
SPIRV_OPT_PASSES = ["--scalar-replacement=0", "--ssa-rewrite", "--ccp", "--simplify-instructions",
                    "--eliminate-dead-code-aggressive", "--merge-blocks", "--eliminate-dead-code-aggressive"]


def split_interpolators(hlsl):
    """The translator passes 16 interpolators as one array (float4 interpolators[16] : TEXCOORD0). The LG
    V30's Adreno driver reads such an input array as garbage, so each becomes its own variable
    (TEXCOORD0..15, the same locations 0..15)."""
    members = "".join(f"    float4 interpolator{i} : TEXCOORD{i};\n" for i in range(16))
    hlsl = hlsl.replace("    float4 interpolators[16] : TEXCOORD0;\n", members)
    hlsl = hlsl.replace("    output.interpolators = o;\n", "".join(f"    output.interpolator{i} = o[{i}];\n" for i in range(16)))
    return hlsl.replace("    [unroll] for (int k = 0; k < 16; k++) r[k] = input.interpolators[k];\n",
                        "".join(f"    r[{i}] = input.interpolator{i};\n" for i in range(16)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("hlsl")
    parser.add_argument("spirv")
    parser.add_argument("--dxc", required=True)
    parser.add_argument("--val")
    # DXC's optimizer (-O1 and up) produces SPIR-V that crashes the LG V30's Adreno 540 driver
    # (2018) in its SPIR-V reader; -O0 output compiles there, and the drivers optimize anyway.
    parser.add_argument("--opt", default="O0", choices=["O0", "O1", "O2", "O3"])
    parser.add_argument("--no-adreno5xx", action="store_true", help="skip the Adreno 5xx rewrites")
    # With -O0 the register file stays an array in memory, and the V30's driver gives up on the largest
    # shaders (VK_INCOMPLETE). These spirv-opt passes promote it to SSA values without the rest of
    # DXC's optimizer (whose output crashes that driver).
    parser.add_argument("--spirv-opt", help="spirv-opt.exe: promote locals to SSA after DXC")
    args = parser.parse_args()

    out = pathlib.Path(args.spirv)
    out.mkdir(parents=True, exist_ok=True)
    failed = []
    total = 0
    rewrites = {}
    for source in sorted(pathlib.Path(args.hlsl).glob("*.hlsl")):
        total += 1
        name = source.stem  # <hash>.vs / <hash>.ps
        profile = "ps_6_0" if name.endswith(".ps") else "vs_6_0"
        target = out / (name + ".spv")
        compiled = out / (name + ".split.hlsl")
        compiled.write_text(split_interpolators(source.read_text()))
        command = [args.dxc, "-spirv", "-fspv-target-env=vulkan1.0", "-T", profile, "-E", "main", "-" + args.opt, "-HV", "2018",
                   "-fvk-b-shift", "0", "0", "-fvk-t-shift", str(T_SHIFT), "0", "-fvk-s-shift", str(S_SHIFT), "0",
                   "-Fo", str(target), str(compiled)]
        result = subprocess.run(command, capture_output=True, text=True)
        compiled.unlink()
        if result.returncode != 0:
            failed.append((name, "dxc", result.stderr.strip().splitlines()[:6]))
            continue
        if args.spirv_opt:
            result = subprocess.run([args.spirv_opt, "--target-env=vulkan1.0", *SPIRV_OPT_PASSES, str(target), "-o", str(target)],
                                    capture_output=True, text=True)
            if result.returncode != 0:
                failed.append((name, "spirv-opt", (result.stdout + result.stderr).strip().splitlines()[:6]))
                continue
        if not args.no_adreno5xx:
            data = target.read_bytes()
            words, counts = spirv_adreno5xx.patch(list(struct.unpack(f"<{len(data) // 4}I", data)))
            target.write_bytes(struct.pack(f"<{len(words)}I", *words))
            for key, value in counts.items():
                rewrites[key] = rewrites.get(key, 0) + value
        if args.val:
            result = subprocess.run([args.val, "--target-env", "vulkan1.0", str(target)], capture_output=True, text=True)
            if result.returncode != 0:
                failed.append((name, "spirv-val", (result.stdout + result.stderr).strip().splitlines()[:6]))
    for name, stage, lines in failed:
        print(f"FAIL {name} ({stage})")
        for line in lines:
            print("    " + line)
    if rewrites:
        print("Adreno 5xx rewrites: " + ", ".join(f"{k}: {v}" for k, v in rewrites.items()))
    print(f"{total - len(failed)}/{total} shaders compiled and validated")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
