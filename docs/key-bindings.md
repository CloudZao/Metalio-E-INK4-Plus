# Keys & gestures

[简体中文](key-bindings_zn.md)

Under Settings → Keys & gestures, review Function help and the notes, then choose a key under Custom functions. Select its short or long press on the full-screen editor and assign a function. Assigned options use white text on black. Reset key functions at the end of the page restores the original assignments without changing the gesture switch.

The first section is Gesture switch. Gesture recognition is enabled by default. Swipe right for the bottom previous key’s default action and left for the bottom next key’s default action, independently of custom key assignments. Turning recognition off disables only these horizontal swipes; bottom keys retain their assignments, and vertical swipes and quick controls remain available. Sliders, text inputs and controls with horizontal scroll ranges keep their own drag gestures.

Swipe vertically to turn help pages. The scrollbar on the right shows the current page position.

Global functions cover volume, navigation, frontlight and power. Page functions run only where supported: page navigation uses the current page; Open AI app applies to Home and reading; Hold to record applies to AI pages with voice input; Reading settings applies to book text; Stop speaking applies to the assistant conversation page.

Supported combinations are Back with Previous page, Enter with Exit standby, Open AI with Hold to record, and Reading settings with Stop speaking. The page or state determines which action runs. Holding a page key repeats when its long press is unassigned; an explicit long-press assignment takes precedence.

Open AI app alone only opens the app. Combined with Hold to record, a long press can open the app and continue recording while held; release to finish.

Keep a physical key assigned to Exit standby and a key assigned to Power off. Exit standby requires a short press on a physical key; it cannot be assigned to bottom touch keys or any long press. Hold to record requires a long press on a non-power key. Assign a replacement before removing a required function. Options unsupported by the hardware or gesture have dashed borders. Removing the last required binding prompts you to assign another key first. Duplicate assignments can be shared after confirmation. Hardware power-on and forced shutdown remain fixed.

Functions depend on the board. The 3.97-inch board has no frontlight, so frontlight toggle and brightness options are omitted from the menu and help. The 4.7-inch board provides these functions.

## SD configuration

Key assignments and the gesture switch are stored only in `metalio/e-ink/settings/key_bindings.json` on SD, never in NVS. A missing file is created with the initial assignments. A valid file is loaded after mounting. Without available SD storage, initial assignments are used in memory; user edits last only for that session. Inserting a card loads its configuration or creates initial assignments if the file is absent.

The file uses the same two-space JSON indentation as Wi-Fi configuration. Only `settings/key_bindings.json` and its backup are read. If neither file exists, the default configuration is created.

Loading, default-file creation and saving run on the background storage worker without blocking the UI. Each write first loads the current card configuration, preserving unmodified assignments and the gesture switch. Assignments apply after saving. Only one edit is accepted at a time; an accepted save finishes after leaving the page, but changing media while it is queued cancels that write. Writes use staging and backup replacement. Failed saves leave current assignments unchanged. FAT replacement is not guaranteed atomic across power loss. Invalid configurations fall back to the complete initial configuration, retaining the file for correction.

JSON has `version: 1`. The `gestures_enabled` field controls horizontal swipe recognition and defaults to enabled when absent. Key objects are `boot`, `power`, `volume_up`, `volume_down`, `previous`, `next`, and `home`. Each has `short` and `long` action-ID arrays; an empty array means No action. IDs are defined in [key_bindings.cc](../main/infra/storage/key_bindings.cc). A gesture can contain at most two actions in a supported combination. Unknown versions or actions, missing fields, invalid combinations and missing required bindings are rejected.

Storage and validation belong to `infra/storage/key_bindings`. Common input dispatch owns execution and release cleanup. Board code prepares physical wake inputs from valid assignments. JSON and the UI use the same hardware and gesture validation.
