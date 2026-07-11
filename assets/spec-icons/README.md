Drop spec icons into this folder.

Naming convention:
- `<classname>-<specname>.jpg`
- Example: `demonhunter-devourer.jpg`
- Use lowercase and remove spaces/punctuation from class/spec names.

Rendering details:
- The recordings participants list loads icons from `<exe folder>/assets/spec-icons/`.
- Icons are displayed at `16x16` in the participant row.
- If an icon is missing for a spec, Bean falls back to the text abbreviation (for example `[DEV]`).
- Bean also accepts `.png` with the same naming pattern as a fallback.
