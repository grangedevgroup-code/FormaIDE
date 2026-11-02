# The assistant

The assistant is off until you set it up in Settings. Nothing is sent anywhere before that, and FormaIDE works fully without it.

You choose a provider. Ollama, LM Studio and llama.cpp run on your own computer, so nothing leaves it. With Anthropic, OpenAI or another compatible service, what you approve is sent to that service under your own account and its terms.

## What gets sent

Above the message box, FormaIDE lists what will go with your next message, such as the current file, the selection, the current screen and any build errors, and you can uncheck any of it. While it works, the assistant can read other project files through tools, unless they look like secrets. Files such as `.env`, private keys and credential stores are blocked and can't be read or written.

## What it can change

Nothing is written straight away. File edits and design changes are collected as proposed changes that you review as a diff, and you apply the ones you want. FormaIDE saves a snapshot first, so one click undoes the lot.

The setting beside the provider controls how much it may do: only suggest, propose changes, apply what you've approved, or also run builds and tests. Commands always show you exactly what will run and ask first, and FormaIDE flags dangerous ones. Everything it does is listed in the Activity panel and in the project's history.

## API keys

Keys are stored in Windows Credential Manager, the macOS Keychain or the Linux secret service. They are never written to project files or settings.

## Model fallback

With the newest Claude models on api.anthropic.com, FormaIDE enables server-side fallback by default. If the model you chose can't serve a request, another Claude model may answer it. Other providers and other hosts don't get this setting.
