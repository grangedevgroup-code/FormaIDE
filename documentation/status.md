# What works today

This page says what has actually been tested in version 0.1.0, and what hasn't. If something isn't listed here as tested, assume it may be rough.

## The application

The desktop app builds and passes its automated interface tests on Windows, where the whole loop was run for real: create a project, add a component on the canvas, generate, edit the code, regenerate, build with MSVC and Qt, run the app, and stop it. Linux and macOS builds of the app pass the same test suite in CI, but without a display, and nobody has used them by hand yet, so they are less proven than Windows. The Windows installer is the only package offered so far. On Linux and macOS, build from source.

## Generators

Every generator here is built with its real toolchain in CI on each commit, on every platform it targets. Being built isn't the same as being looked at, so this is what has actually been seen running.

Qt Widgets, WPF, Tkinter, HTML and React were each run and compared by eye against the canvas on Windows. They match in layout, colours and text, with small differences in font rendering. The WPF screenshot was taken with software rendering. Tkinter draws square buttons, because Tk has no rounded corners.

Jetpack Compose builds a real APK, but that APK has not been run on a phone or emulator yet. PySide6 opens its window under CI's offscreen platform and hasn't been compared by eye. SwiftUI builds on the macOS runner, and it targets macOS only, not iOS, because iOS needs an Xcode project that FormaIDE doesn't write yet.

All of them handle containers, the main layouts, text, buttons, inputs, checkboxes, radio buttons, switches, dropdowns, sliders, dividers, spacers, lists and images. Qt Widgets also has tabs, tables, charts, media, dialogs, bindings, state and reusable components. In the other generators, state changes and data bindings are only partly there, and anything a generator can't do shows up as a warning in Problems rather than being dropped silently.

Reading changes back from code into the design works for text in Qt Widgets, and the others emit the source maps it relies on, but only Qt Widgets has been tested end to end.

The code-only templates for C++ libraries, Rust, Go, Python and Node.js are also built in CI on all three systems.

## Templates FormaIDE doesn't offer yet

QML, Android Views in Java, WinUI, GTK, Flutter and React Native have template definitions but no generator. The New Project dialog hides them until they exist and pass the same tests.

## The canvas

The canvas draws with QPainter on the CPU. It has not been measured on very large screens, and GPU drawing is planned once a real project shows it is needed.

## Language servers and debuggers

Highlighting comes from Lexilla, and a test checks that every listed language loads its lexer. Language server and debugger support were tested against scripted protocol tests, not against every server. clangd, pyright, gopls and rust-analyzer are the expected ones, and only the ones you install will work. FormaIDE tells you how to install a missing one.

## The assistant

The provider code, staged changes, approvals and rollback are covered by tests using a fake provider. It has not been run against a live paid API as part of this release, so treat the cloud providers as untested until you've tried yours. On api.anthropic.com, FormaIDE turns on server-side model fallback for the newest models, which means a request your chosen model can't serve may be answered by another Claude model. The settings page says so too.

## Web version

There is no web version of the app. The core libraries compile to WebAssembly in CI, which keeps that door open and nothing more.
