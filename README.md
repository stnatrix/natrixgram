# Natrixgram

Custom Telegram Desktop fork with quick local passcode unlock and workflow enhancements.

## Features

- **Instant Global Passcode Unlock**: Rapidly unlock Telegram Desktop using a global hotkey, even when minimized or running in the background.
- **Isolated Portable Environment**: Runs out of a standalone portable directory (`TelegramForcePortable`) with `-many` support, ensuring zero interference with standard Telegram installations.
- **Cleaned Codebase**: Stripped of upstream AI agent pipelines, redundant tracker files, and heavy CI workflows.
- **Lightweight Windows CI**: Dedicated GitHub Actions pipeline targeting Windows x64 Debug builds.

## Releases

Ready-to-use binaries are available in the [Releases](https://github.com/stnatrix/natrixgram/releases) section.

## Building

Windows 64-bit builds are compiled through the GitHub Actions workflow in [`.github/workflows/build_win.yml`](.github/workflows/build_win.yml).
For local building instructions, see [`docs/building-win.md`](docs/building-win.md).

## License

Natrixgram is distributed under the GNU General Public License v3 with OpenSSL exception, based on the official [Telegram Desktop](https://github.com/telegramdesktop/tdesktop).
