<p align="center"><img src="resources/data/branding/logo-light.svg" alt="FormaIDE" width="320"></p>

FormaIDE is a native visual development environment. You design screens on a canvas, it generates ordinary source code for the framework you picked, and you keep working on that code in a real editor with a terminal, Git, a debugger and build tools alongside. Everything runs on your own computer, and the projects it makes build without FormaIDE installed.

It is written in C++20 with Qt 6 and Scintilla. The core libraries have no Qt in them, so a browser version stays possible later. This is an early release, so please read [what works today](documentation/status.md) before relying on it.

## What you can do with it

Start a project from a template, lay out every screen side by side on one canvas, link them together, and press Run. FormaIDE writes the code, builds it with the framework's own toolchain and starts your app. Your own code lives in separate files that regeneration never overwrites. If you edit a generated file, FormaIDE merges your change, and when it can't merge safely it asks you and changes nothing until you answer. Simple edits in generated code, such as a button's text, find their way back to the design.

The visual designer generates these today:

| Framework | Language | Runs on |
| --- | --- | --- |
| Qt Widgets | C++ | Windows, macOS, Linux |
| WPF | C# | Windows |
| Jetpack Compose | Kotlin | Android |
| SwiftUI | Swift | macOS |
| React with Vite | TypeScript | Web |
| HTML and CSS | HTML, CSS, JavaScript | Web |
| PySide6 and Tkinter | Python | Windows, macOS, Linux |

The code editor covers far more than that. It highlights 216 languages, and it uses whichever language server and debugger you have installed for completion, go to definition, rename and stepping through code. Build support includes CMake, Gradle, .NET, Cargo, Go, Maven, Meson, Make, Zig, npm, Flutter, Swift and others, and you can open any existing folder as a project.

The assistant is optional and off until you set it up. It works with local models through Ollama, LM Studio or llama.cpp, or with your own API key for Anthropic, OpenAI or any compatible service. Changes it proposes wait for your approval as a diff, you can undo them, and commands it wants to run ask first. Keys stay in your system's credential store.

You don't have to set up compilers and SDKs by hand. Tools and toolchains shows what each kind of project needs, the exact commands it will run, and installs what's missing when you press Install.

## Building it

You need CMake 3.25 or newer, Ninja, a C++20 compiler and Qt 6.5 or newer with Qt5Compat. On Windows, use MSVC.

```
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The app ends up in `build/dev/bin`. The `headless` preset builds the libraries and the `forma` command line tool without Qt. Run `forma --help` to see what it does.

## More

The [documentation](documentation) folder explains the [architecture](documentation/architecture.md), [how to write a plugin](documentation/plugins.md) and the [assistant's privacy and safety rules](documentation/assistant.md). Third-party licenses are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

FormaIDE is released under the [MIT license](LICENSE).
