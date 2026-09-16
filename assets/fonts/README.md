# Fonts

Both are redistributed here under the SIL Open Font License 1.1, whose full
text is in this directory. They are committed rather than downloaded at build
time so that a fresh clone builds and runs with no network access.

| File | Used for | Source |
|---|---|---|
| `Inter-Regular.ttf` | UI text: menus, labels, headings | [rsms/inter](https://github.com/rsms/inter) v4.0, `extras/ttf` |
| `JetBrainsMono-Regular.ttf` | Numeric columns in tables | [JetBrains/JetBrainsMono](https://github.com/JetBrains/JetBrainsMono) |

Two faces rather than one, for a specific reason: every number in this
application sits in a column next to another number, and a proportional font
gives `1` and `8` different widths. In a blotter that means the decimal points
do not line up and two prices of different magnitude look the same length.
Inter reads better as prose; JetBrains Mono makes a column of figures
scannable. Each is used where it wins.

The static instances are committed, not the variable ones. Dear ImGui rasterises
through stb_truetype, which renders a variable font at whatever its default
instance happens to be -- for Inter that is Thin, which is unreadable at UI
sizes.
