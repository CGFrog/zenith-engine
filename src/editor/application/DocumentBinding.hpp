#pragma once

// The objects holding one open document: its scene, document state, undo history, and selection.

class CommandHistory;
class EditorDocument;
class Scene;
class Selection;

// The objects holding one open document: its scene, document state, undo
// history, and selection. The level's are the session's own members (and the
// engine's scene); a prefab window owns its own (PrefabWindowSet). Operators
// and panels edit whichever document the binding they were given names, so a
// binding must outlive the context built from it.
struct DocumentBinding {
    Scene& scene;
    EditorDocument& document;
    CommandHistory& history;
    Selection& selection;
};
