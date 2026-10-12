# Pixelica Editor design

Mode: Operate. Surface: local desktop game editor.

The task is to select an object, place it, edit properties, save and test play in one workspace.
Use a familiar scene list / canvas / inspector arrangement with a persistent file-and-play toolbar.
The canvas gets the flexible center space; inspector contents scroll without shrinking labels.

For long sessions comparing colorful sprite art, use neutral blue-gray chrome with readable light text
and a restrained teal accent for selection and primary actions. System Segoe UI/Malgun Gothic text
serves Korean labels. Text buttons make actions understandable without decorative icons.

The viewport displays pixels from the native Pixelica renderer. Selection outlines and placement tools
are editor overlays and disappear during play. Play state is visible, and stop restores the edit scene.
Errors remain readable in the status area; unsaved replacement uses Save/Discard/Cancel.

Collision setup extends the inspector with task presets and labeled numeric fields. Solid regions use
orange outlines and triggers use dashed outlines. The optional collision view follows the active scene
during edit and play; selection handles remain exclusive to editing. Grounding and trigger names are
visible during play, with keyboard instructions for horizontal movement and jumping.

Keyboard focus, field labels, disabled play-time editing, resizable panels and a scrollable inspector
are part of the working surface. No decorative startup motion is needed.
