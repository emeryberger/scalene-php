import subprocess
import os
import time
import json

# run target and return timing stats
def bench(bench_name, profiler, target, prog, script, arg, stdin, runs=5):
    results = {
        "times": [],
    }

    if profiler is None:
        command = "./php %s/%s/%s %s" % (target, prog, script, arg)
    else:
        command = "./php %s %s/%s/%s %s" % (profiler, target, prog, script, arg)
    command = command.split(" ")

    for i in range(runs):
        start = time.time()

        proc = subprocess.run(
            command,
            input=stdin,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            cwd="..",
            encoding="utf-8")

        if proc.returncode != 0:
            print("%s FAILED" % bench_name)
            print("stdout:")
            print(proc.stdout)
            print("stderr:")
            print(proc.stderr)
            exit(1)

        results["times"].append(time.time() - start)

    print("%s DONE" % bench_name)
    return results

# used later as the input to some scripts
proc = subprocess.run(
    ["./php", "benchmarks/benchmarksgame/fasta/fasta.php-3.php", "400000"],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    cwd="..",
    encoding="utf-8")

if proc.returncode == 0:
    fasta_output = proc.stdout
else:
    print("failed to capture fasta output")
    print("stdout:")
    print(proc.stdout)
    print("stderr:")
    print(proc.stderr)
    exit(1)

# specify script configurations (arg, STDIN content)
configs = {
    "binarytrees": ("14", None),
    "fannkuchredux": ("9", None),
    "fasta": ("400000", None),
    "knucleotide": ("", fasta_output),
    "mandelbrot": ("800", None),
    "nbody": ("100000", None),
    "pidigits": ("10000", None),
    "regexredux": ("", fasta_output),
    "revcomp": ("", fasta_output),
    "spectralnorm": ("500", None),
}

# do benchmark
results = {}
for (prog, (arg, stdin)) in configs.items():
    for script in sorted(os.listdir("benchmarksgame/%s" % prog)):
        script_stripped = script.replace(".php", "")
        results[script_stripped] = {}

        results[script_stripped]["base"] = bench(
            "base-%s" % script_stripped,
            None,
            "benchmarks/benchmarksgame",
            prog, script, arg, stdin)

        results[script_stripped]["cpu"] = bench(
            "scalene-cpu-%s" % script_stripped,
            "scalene.php --cpu-only",
            "benchmarks/benchmarksgame-scalene",
            prog, script, arg, stdin)

        results[script_stripped]["full"] = bench(
            "scalene-full-%s" % script_stripped,
            "scalene.php",
            "benchmarks/benchmarksgame-scalene",
            prog, script, arg, stdin)

# dump json
with open("%s.json" % int(time.time()), 'w') as f:
    json.dump(results, f, indent=2)
