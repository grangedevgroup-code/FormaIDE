"""Create a project from each template and build it with its real toolchain.

Usage: run_generators.py <forma executable> <work dir> [template ...]
"""
import os
import pathlib
import shutil
import subprocess
import sys

TEMPLATES = {
    "qt-desktop": ["cmake", "qt"],
    "web-static": [],
    "web-react": ["npm"],
    "windows-wpf": ["dotnet"],
    "python-tk": ["python"],
    "python-qt": ["python"],
    "android-compose": ["android"],
    "apple-swiftui": ["swift"],
}

EXPECT = {
    "qt-desktop": ["CMakeLists.txt", "src/main.cpp"],
    "web-static": ["index.html", "styles.css", "app.js", "handlers.js"],
    "web-react": ["dist/index.html", "src/App.tsx"],
    "windows-wpf": ["App.xaml", "Handlers.cs"],
    "python-tk": ["main.py", "handlers.py"],
    "python-qt": ["main.py", "handlers.py"],
    "android-compose": ["app/build/outputs/apk/debug/app-debug.apk"],
    "apple-swiftui": ["Package.swift"],
}

forma = os.path.abspath(sys.argv[1])
work = pathlib.Path(sys.argv[2]).resolve()
wanted = sys.argv[3:] or list(TEMPLATES)
failures = []
shutil.rmtree(work, ignore_errors=True)
work.mkdir(parents=True)

for template in wanted:
    name = "T" + template.replace("-", "")
    print(f"== {template}", flush=True)
    created = subprocess.run([forma, "new", name, "--template", template, "--dir", str(work)], capture_output=True, text=True)
    if created.returncode != 0:
        print(created.stdout, created.stderr)
        failures.append(template)
        continue
    project = work / name
    built = subprocess.run([forma, "build", str(project)], capture_output=True, text=True, errors="replace")
    missing = [path for path in EXPECT.get(template, []) if not (project / path).exists()]
    if built.returncode != 0 or missing:
        print(built.stdout[-6000:])
        print(built.stderr[-3000:])
        print("missing:", missing)
        failures.append(template)
    else:
        print("ok", flush=True)

print("failed:" if failures else "all passed", failures if failures else "")
sys.exit(1 if failures else 0)
