# Writing a plugin

A plugin is a folder with a `forma-plugin.json` manifest. It can add generators, project templates, languages, build systems and themes. Plugins run as separate programs, so a crash in one can't take the IDE down with it.

The quickest way to start is the "FormaIDE plugin" project template, which gives you a working generator written in Python.

## The manifest

```json
{
  "id": "example.hello",
  "name": "Hello generator",
  "version": "1.0.0",
  "publisher": "Example",
  "apiVersion": 1,
  "permissions": [],
  "contributes": {
    "generators": [
      {
        "id": "hello",
        "label": "Hello",
        "language": "Text",
        "command": ["python", "generator.py"]
      }
    ]
  }
}
```

FormaIDE reads the application model and sends it as JSON on the generator's standard input. The generator prints JSON on standard output with the files it wants written, and FormaIDE treats them exactly like built-in generator output, including merging and conflict handling.

## Permissions

Before installing, FormaIDE shows what a plugin asks for and you decide. It records a hash of the plugin's files at install time, and if those files change later, the plugin is disabled until you reinstall it.
