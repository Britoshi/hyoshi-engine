# 0002: An editor framework in the engine, editors in the games

- **Status:** accepted, 2026-09-28. Milestone 0 is built.
- **Numbering:** 0001 is kept for the stack choices (DESIGN.md, M0 item 12), which aren't written yet.

## Context

The engine should grow from debug panels into a full editor. DESIGN.md plans a chart editor on Dear ImGui (section 19.4, M11) and says ImGui is for debug and tools only, never player-facing UI (section 17.3). Games need editors for their own content too, and the engine can't know a game's scenes or data.

What was there to build on: games plug into the engine through `app::Game`, and `hyoshi_add_app` builds them. Scenes are code (`app::Scene::RunFrame`), not data: the entity and component model of section 17.1 was never built. The RHI draws only to the swapchain, so there are no render targets for a game viewport inside a panel. ImGui was pinned to a release without docking.

How other engines do it:

- **Editor UI and game UI are separate toolkits almost everywhere.** Unity: IMGUI, then UI Toolkit, for the editor, and uGUI or UI Toolkit runtime for games. Unreal: Slate for the editor, and UMG (built on Slate) for games. Godot is the exception: its editor is a Godot app built from the same Control nodes as games. Custom engines mostly use Dear ImGui's docking branch; in-house AAA tools use Qt (CryEngine Sandbox, O3DE) or C# and WPF (Frostbite, Stride).
- **The scene model is shared, never separate.** The editor edits the data the game loads. Editors differ from the game in having an edit mode and a play mode: Unity saves the scene, plays, and restores it; Unreal's Play In Editor duplicates the world.
- **Games extend the engine's editor.** Unreal games add editor modules, Unity games add editor scripts, and Godot games add `EditorPlugin`s.

## Decision

1. **The engine provides an editor framework, `hyoshi::editor` (`engine/editor`).** It covers dockable panels, the menu bar, and later play mode, undo, and file dialogs. A game implements `editor::Editor` and adds its own panels, the way it implements `app::Game`.
2. **The editor's UI is Dear ImGui's docking branch.** `ui::Ui` stays the player-facing kit. Building the editor from the game's UI, Godot's way, would first need a full retained toolkit.
3. **The editor runs the game itself.** It runs the game's own `app::Game` and scenes in the same process, not a copy or a separate runtime.
4. **Each game has a separate editor executable.** `hyoshi_add_editor` builds it from the game's code as a library, next to the executable `hyoshi_add_app` builds. `hyoshi_add_app` never links `hyoshi::editor`, so shipped builds contain no editor code, without macros like Unreal's `WITH_EDITOR` through the code. The editor is for desktops only.
5. **The editor shares the game's user data folder**, so it uses the player's settings and offsets. It keeps its own ImGui layout (`editor-imgui.ini`) apart from the debug overlay's.
6. **Game code can ask whether it runs in the editor** (`AppServices::IsEditor`, like Unity's `Application.isEditor`) to skip what only suits players, such as turning the window to the saved orientation.

## Milestones

- **0, overlay (built):** the ImGui docking branch, `engine/editor`, and `hyoshi_add_editor`. The game runs full-window, with the menu bar and dockable panels over it; the dock space's empty center passes input through to the game. The first game builds as a library with two executables and adds one panel of its own.
- **1, chart editor:** the chart and timeline panel of section 19.4 as an engine panel (charts are the engine's format), and a play-mode hook, `Editor::CreatePlayScene`, so "play from here" starts the game's gameplay scene at a time in the chart. Undo and file dialogs arrive with it.
- **2, the game in a panel:** render targets in the RHI and a camera per viewport, sized from the panel rather than the window (`Application` sizes its one camera from the swapchain today).
- **Later:** a description of each type's fields (a `Describe(visitor)` function per type that drives the inspector, JSON, and undo diffs), inspectors, and the entity and component model of section 17.1. None of the milestones above need them.

## Consequences

- A game's code builds as a static library that both executables link; its `main` stays in a small file for each.
- `app::Application` takes an optional `app::EditorLayer`, which `engine/editor` implements. With one, ImGui is always on with docking, the ImGui windows capture keys and the pointer while they want them (not only while the F1 overlay shows), and the window title can differ from the user data folder's name (`AppConfig::WindowTitle`).
- Until milestone 2, panels cover the game, and the menu bar covers the top of it.
