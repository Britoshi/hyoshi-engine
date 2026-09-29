# Contributing to Hyoshi Engine

Thanks for your interest in the engine. Bug reports, questions, and pull requests are all welcome.

## Reporting bugs and requesting features

Open an [issue](https://github.com/Britoshi/hyoshi-engine/issues). For bugs, include your platform, GPU, how you built the engine (preset, compiler), the steps to reproduce, and the log output. Check [Known issues](docs/DEVELOPMENT.md#known-issues) first.

For larger changes, such as a new module, a new game mode, or a change to a public interface, please open an issue to discuss it before writing the code.

## Setting up

Follow [Getting started](README.md#getting-started) in the README to build the engine and run the tests. [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) explains how the code is organized, and [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) covers debug options, stress tests, and platform-specific tips.

## Making a change

1. Fork the repository and create a branch from `main`.
2. Make your change, with unit tests for new behavior. Tests live in `tests/unit` and use [doctest](https://github.com/doctest/doctest).
3. Make sure the build has no warnings and these pass:

   ```sh
   ctest --preset <preset>
   cmake --build --preset <preset> --target format-check
   cmake --build --preset <preset> --target tidy
   ```

   For changes to timing, audio, or rendering, also run the [stress tests](docs/DEVELOPMENT.md#checks).
4. Update the documentation if your change affects how the engine is used or built.
5. Open a pull request describing what changed, why, and how you tested it.

## Code style

- Formatting follows [`.clang-format`](.clang-format) (Allman braces, 4-space indent, 120 columns). Run `cmake --build --preset <preset> --target format` to apply it.
- Naming is checked by clang-tidy: `PascalCase` for types, functions, and public members; `camelCase` for locals and private members; `UPPER_CASE` for constants.
- Include what you use. MSVC's standard library and libc++ include different headers transitively, so code that builds on one may not build on the other.
- Avoid names that are common C macros, such as `PAGE_SIZE`.
- Keep the [design rules](docs/ARCHITECTURE.md#design-rules): gameplay time is integer microseconds, song time never goes backwards, judgment is deterministic, and engine code never depends on a specific game.

## Commit messages

- Write the subject line in the imperative mood ("Add spinner judgment", not "Added spinner judgment"), in 72 characters or less.
- In the body, explain what changed and why, and say how you tested it and on which platforms.

## Documentation

If a change makes the code differ from [DESIGN.md](docs/DESIGN.md), add a note next to the original plan explaining what was done instead, rather than rewriting the plan. Significant design decisions get a record in [docs/decisions](docs/decisions/).

## License

By contributing, you agree that your contributions will be licensed under the [MIT License](LICENSE).
