# Changelog

## 0.2.0

The interface was rebuilt around the canvas. All screens now sit on one zoomable page with a dot grid, there is one top bar instead of a menu bar, toolbar and side rail, and Design, Code, Preview, Debug and Project are modes you switch between at the top. The canvas has a floating toolbar for placing frames, text, shapes, images, buttons and inputs by clicking or dragging, keyboard shortcuts for each of them, an Add screen slot, and spacing measurements while you hold Alt. Layers now lists every screen. Components are a grid of tiles. The inspector has Design, Prototype and Code tabs: Prototype links screens together and draws the links on the canvas, and Code shows the source generated for the selected layer. Ctrl+K finds any command.

FormaIDE can now install what it needs. Tools and toolchains lists what each kind of project requires, shows the exact commands, and runs them when you press Install. It covers compilers, SDKs, the Android SDK and emulator, Qt, Rust, Go, Node.js, Python, .NET and language servers. It is offered from the welcome page, the new project dialog, the build panel and whenever a build finds something missing.

Running was fixed for Android. Run now picks a connected phone or a virtual phone, starts the emulator if it needs to, installs the app, opens it and shows its log. Before, it failed silently when no device was connected. Web projects open in your browser, and Run on a folder with no build system runs the file you have open. Projects that are only code now run in the terminal, so they can read input.

Coding got easier. Files open with one click, and the terminal is the default bottom panel. Without a language server you still get completion from keywords and the words in the file, and FormaIDE offers to install the server. Brackets and quotes close themselves. New File and New Folder are buttons in the Files panel.

New projects now get their own folder. Before, a project was created directly in the folder you chose, so a second one failed. Android projects now start at a phone size that exists on Android.

## 0.1.0

The first release. It has a visual designer with undo and redo, generators for Qt Widgets, WPF, Jetpack Compose, SwiftUI, React, HTML and CSS, PySide6 and Tkinter, a code editor for 216 languages, language server and debugger support, a terminal, Git, builds with many toolchains, plugins, crash recovery and an optional assistant. See [what works today](documentation/status.md) for what has and hasn't been tested.
