# Workspaces Overview

The overview displays every workspace and lets you navigate, focus, close, and move windows.

## Settings and behavior

```toml
[overview]
style = "spaces"
zoom = 0.5
scroll_factor_horizontal = 1.0
scroll_factor_vertical = 1.0
background_blur = true
workspace_wallpaper = true
shortcuts = true
shortcut_keys = "1234567890"
```

| Key                        | Default        | Description                                                  |
| -------------------------- | -------------- | ------------------------------------------------------------ |
| `style`                    | `"spaces"`     | Overview style; see [Styles](#styles).                       |
| `zoom`                     | `0.5`          | Strip row scale from 0.1 to 0.75. Other styles ignore it.    |
| `scroll_factor_horizontal` | `1.0`          | Horizontal gesture and wheel sensitivity.                    |
| `scroll_factor_vertical`   | `1.0`          | Vertical gesture and wheel sensitivity.                      |
| `background_blur`          | `true`         | Blur the desktop behind the overview.                        |
| `workspace_wallpaper`      | `true`         | Show each output's background inside its workspace previews. |
| `shortcuts`                | `true`         | Show and accept keyboard shortcut badges.                    |
| `shortcut_keys`            | `"1234567890"` | Preferred keys for shortcut badges.                          |

Background blur uses `[appearance.blur]`. Preview backgrounds use `colors.overview.workspace_background` when wallpaper mirroring is disabled or no background surface is available.

### Styles

- `"spaces"`: the current workspace fills the middle of the screen, with every workspace as a thumbnail along the top.
- `"taskview"`: the current workspace's windows spread out side by side, with workspaces as thumbnails along the bottom.
- `"strip"`: each workspace is a scaled-down row stacked along the output's [workspace axis](workspaces.md#workspace-axis). `zoom` sets the scale.
- `"grid"`: every workspace as an equal tile in a grid.

The style changes only how the overview looks. Each workspace keeps its own layout.

### Open and navigate

Press `Mod+O` by default, or use an [overview action](actions.md#overview). Opening the overview temporarily hides scratchpads and pinned windows.

- Click a window to focus it and close the overview.
- Middle-click a window to close it.
- Drag a window to move it to another workspace.
- Use the wheel to navigate vertically and Shift+wheel horizontally.
- Use a four-finger swipe to open or close the overview.

Two-finger scrolling and three-finger swipes navigate continuously. Movement along the output's [workspace axis](workspaces.md#workspace-axis) moves between workspaces. Movement across it pans a scrolling workspace preview. Dwindle and Master layouts have no strip to pan.

Touchpad `natural_scroll` controls gesture direction. The horizontal and vertical overview factors adjust gesture distance and wheel sensitivity without changing application scrolling.

#### Which window actions act on

One card carries the full focused-border color while the overview is open. It is the target for focus, close, and other window actions. Each other workspace keeps a fainter marker showing which card will receive focus when selected.

The current output follows the cursor. Output-changing actions move the cursor to their destination as usual.

#### Keyboard shortcuts

Normal `[keybinds]` remain active in the overview. Directional focus actions select neighboring cards, and workspace or output actions keep their normal fallback behavior.

Shortcut badges provide direct, unmodified key sequences for visible window cards. When there are more cards than keys, Andromeda creates multi-key labels. Backspace removes the last key from a pending sequence. Escape clears the sequence first and closes the overview when no sequence is pending.

Set `shortcuts = false` to disable badges. A normal keybind takes precedence over a badge. `shortcut_keys` must contain at least two unique printable ASCII characters.

### Move windows

Drag a window onto another workspace preview to move it. In a dynamic workspace list, dropping into a gap creates a workspace at that position. Static workspace inventories accept drops only onto existing previews.

The destination layout shows an insertion preview before the drop. Empty dynamic workspaces may disappear immediately after their last window is moved or closed.

### Appearance

Overview cards reuse each window's borders, corner radius, opacity, blur, and color presentation. Shortcut badges use `colors.overview.badge`.

Configure overview colors under [`[colors.overview]`](appearance.md#overview-colors).
