"""Compare real model output with a frozen old CPU engine in another process."""
import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--binary", required=True, type=Path)
parser.add_argument("--oracle", required=True, type=Path)
parser.add_argument("--model-dir", required=True, type=Path)
parser.add_argument("--out", required=True, type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
out = args.out.resolve() / ("run-" + str(time.time_ns()))
out.mkdir(parents=True)
commands = []
checks = []


def digest(data):
    return hashlib.sha256(data).hexdigest()


def run(name, argv, expected=0):
    command = list(map(str, argv))
    start = dt.datetime.now(dt.timezone.utc).isoformat()
    result = subprocess.run(command, cwd=root, capture_output=True, timeout=180)
    (out / (name + ".stdout.log")).write_bytes(result.stdout)
    (out / (name + ".stderr.log")).write_bytes(result.stderr)
    commands.append({"argv": command, "name": name, "cwd": str(root), "started_utc": start,
                     "ended_utc": dt.datetime.now(dt.timezone.utc).isoformat(), "exit_code": result.returncode})
    (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n", encoding="utf-8")
    assert result.returncode == expected, (name, result.returncode, expected, result.stderr.decode(errors="replace"))
    return result


catalog = json.loads((root / "models/catalog/rife46.manifest.json").read_text())
for field in ("graph", "weights"):
    identity = catalog[field]
    data = (args.model_dir / identity["file"]).read_bytes()
    assert len(data) == identity["size_bytes"] and digest(data) == identity["sha256"]

for w, h in ((32, 32), (65, 35)):
    folder = out / f"{w}x{h}"
    folder.mkdir()
    def pixels(shift):
        return bytes(value for y in range(h) for x in range(w)
                     for value in ((max(0, x-shift)*255//w), y*255//h, 220 if ((max(0,x-shift)//8+y//8)%2) else 30))
    a, b = pixels(0), pixels(2)
    header = f"P6\n{w} {h}\n255\n".encode()
    (folder / "left.ppm").write_bytes(header + a)
    (folder / "right.ppm").write_bytes(header + b)
    (folder / "left.rgb").write_bytes(a)
    (folder / "right.rgb").write_bytes(b)
    for alpha in ("0", "0.5", "1"):
        label = f"{w}x{h}-a{alpha}"
        spec = {"schema_version": 1, "model_directory": str(args.model_dir.resolve()),
                "left": str(folder / "left.ppm"), "right": str(folder / "right.ppm"), "alpha": alpha,
                "output": str(folder / f"candidate-{alpha}.ppm"), "precision_profile": "CPU_FP32_REFERENCE"}
        spec_path = folder / f"case-{alpha}.json"
        spec_path.write_text(json.dumps(spec), encoding="utf-8")
        report = folder / f"report-{alpha}.json"
        run(label + "-candidate", [args.binary, "interpolate", "--run-spec", spec_path, "--report", report])
        output = Path(spec["output"]).read_bytes()
        measured = json.loads(report.read_text())
        assert measured["algorithm_accepted"] and measured["output_sha256"] == digest(output)
        assert output.startswith(header)
        raw = output[len(header):]
        oracle_output = folder / f"oracle-{alpha}.rgb"
        run(label + "-oracle", [args.oracle, args.model_dir, folder / "left.rgb", folder / "right.rgb", w, h, alpha, oracle_output, "1"])
        expected = oracle_output.read_bytes()
        assert raw == expected, (label, "old CPU engine mismatch")
        if alpha in {"0", "1"}:
            assert raw == (a if alpha == "0" else b) and measured["frame_kind"] == 2
        else:
            assert measured["frame_kind"] == 1 and raw not in (a, b)
            # A/A is a distinct process and must reproduce the complete bytes.
            run(label + "-repeat", [args.binary, "interpolate", "--run-spec", spec_path, "--report", report])
            assert Path(spec["output"]).read_bytes() == output
        checks.append({"case": label, "result": "PASS", "output_sha256": digest(output),
                       "pixel_sha256": digest(raw), "oracle_pixel_sha256": digest(expected)})

invalid = dict(spec, alpha="nan", output=str(out / "invalid-output.ppm"))
for name, delta, expected_code in (("alpha_nan", {}, 4), ("alpha_range", {"alpha": "1.01"}, 4),
                    ("missing_model", {"alpha": "0.5", "model_directory": str(out / "missing-model")}, 9),
                    ("unsupported_precision", {"alpha": "0.5", "precision_profile": "LEGACY_FP16"}, 3)):
    request = dict(invalid, **delta)
    path = out / (name + ".json")
    path.write_text(json.dumps(request), encoding="utf-8")
    run(name, [args.binary, "interpolate", "--run-spec", path], expected=expected_code)
    assert not Path(request["output"]).exists()
    checks.append({"case": name, "result": "PASS", "scope": "failure produces no successful output"})

summary = {"result": "PASS", "scope": "real v4.6 CPU FP32 full-graph reference; not Vulkan/guards/present/performance qualification",
           "binary_sha256": digest(args.binary.read_bytes()), "oracle_sha256": digest(args.oracle.read_bytes()),
           "model_graph_sha256": catalog["graph"]["sha256"], "model_weights_sha256": catalog["weights"]["sha256"],
           "checks": checks, "commands": len(commands)}
(out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
manifest = [{"path": str(p.relative_to(out)), "size_bytes": p.stat().st_size, "sha256": digest(p.read_bytes())}
            for p in sorted(out.rglob("*")) if p.is_file()]
(out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print(json.dumps({"result": "PASS", "summary": str(out / "summary.json"), "checks": len(checks)}))
