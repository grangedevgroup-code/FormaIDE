# Architecture

FormaIDE has three layers. At the bottom, `core` holds what every other part needs: results and errors, JSON, paths, the project filesystem, processes, file watching, the SQLite database, credentials, history and crash recovery. Above it, `modules` holds the features as plain C++ libraries. At the top, `applications` holds the two programs, the Qt desktop app and the `forma` command line tool.

Only `applications/desktop` knows about Qt. That is the rule that keeps a future browser version possible, and CI enforces it by compiling the libraries to WebAssembly.

## The application model

A design is stored as an application model, a set of typed structures saved as readable JSON in the project's `design` folder: one file per screen, plus files for components, themes and the app itself. Unknown keys survive a load and save, so newer projects don't lose data in older versions.

All edits go through `ModelEditor`, which validates them, records them for undo and redo, and tells the panels what changed. The canvas, the inspector, the assistant and code sync all use that same path.

## From design to code

The layout engine turns a screen into boxes, and the canvas turns those into a display list that a backend draws. Today's backend is QPainter.

Generators implement `ICodeGenerator` and return a list of files. Each file is either generated, which FormaIDE owns, or user, which it creates once and never touches again. Generated files carry a source map linking lines to components, which powers jumping between the canvas and the code and reading simple edits back into the design.

The pipeline keeps a copy of what it last generated. When it regenerates, it does a three-way merge between that copy, your file and the new output. If your edit doesn't conflict, it's kept. If it does, the file is left alone and marked as a conflict for you to resolve.

## Building and running

`ToolchainDetector` finds the tools on your computer without assuming anything is installed. A build provider turns a project into a plan of steps and a run command. Some providers are code, and simple ones are a few lines of data, which is also how a plugin can add one. Build output goes through a diagnostics parser that understands the common compilers, so errors become clickable problems.

## Editor services

The code editor is Scintilla with Lexilla for highlighting. Language servers and debug adapters run as separate programs and share one message framing layer. The terminal uses ConPTY on Windows and a pseudo terminal elsewhere, with its own screen model.

## Files and safety

Everything that writes project files goes through `IFilesystemService`, which refuses paths outside the project, writes atomically, checks for changes made since you opened a file, and keeps a backup and a history entry. The assistant and plugins have no other route to your files.

## Adding things

New generators, templates, languages and build systems can come from plugins, described in [plugins.md](plugins.md). A built-in generator goes in `generators/`, gets registered in `generators/common/registry.cpp`, and has to pass `tests/toolchain/run_generators.py` with its real toolchain before it's offered to anyone.
