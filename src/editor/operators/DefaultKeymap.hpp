#pragma once

#include "editor/operators/Keymap.hpp"

#include <vector>

// The editor's default bindings: Blender's object-mode keys for the features
// that exist, plus the usual application shortcuts (Ctrl+S, F5, ...). Users
// change them in the keymap editor; keymap.json stores only the differences.
std::vector<KeymapBinding> defaultEditorKeymap();
