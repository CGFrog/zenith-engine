#pragma once

#include "editor/document/PrefabWriteCommand.hpp"
#include "engine/scene/Prefab.hpp"

#include <filesystem>
#include <optional>
#include <string>

// A prefab opened as a document of its own (the editor's prefab windows): its
// content is instantiated into an otherwise empty scene, edited there like any
// scene, and written back to the file. Named apart from the engine's
// PrefabDocument, which is the file's parsed content.
//
// The opened scene holds one instance of the prefab: its root carries the
// prefab's own link (PrefabInstanceComponent naming the reference), and every
// entity created from the file its prefab-local id, so saving keeps entity and
// nested record ids stable, exactly as an apply would, except that the root's
// transform is saved too (PrefabCaptureOptions::keepRootTransform). Opening
// and saving without edits writes the file byte for byte as the editor wrote
// it. A prefab holding a model whose node entities the file does not list
// gains those node entities on its first save (they are instantiated on
// open).

// What openPrefabDocument loaded.
struct OpenedPrefabDocument {
    // The prefab root in the scene.
    SceneEntityId root;
    // The file's content as read, for a later PrefabWriteCommand's before
    // side and for telling whether the file changed while it was open.
    std::string fileBytes;
    // The prefab's display name (PrefabDocument::displayName), or its file
    // name without the extension when it has none.
    std::string displayName;
};

// What savePrefabDocument wrote: the file before (nullopt when it did not
// exist, for example after it was deleted while open) and after, for a
// PrefabWriteCommand covering the save.
struct SavedPrefabDocument {
    std::optional<std::string> before;
    std::string after;
};

// A prefab open as a document of its own: its content instantiated into an
// otherwise empty scene (openPrefabDocument), as a prefab window keeps it
// (application/PrefabWindowSet.hpp).
struct PrefabEditTarget {
    // Project-relative, generic form, as instances name it.
    std::filesystem::path reference;
    // Absolute and lexically normal.
    std::filesystem::path file;
    // The prefab's display name, for window titles, prompts, and step names.
    std::string displayName;
    // The prefab root in the document's scene, which carries the prefab's own
    // link (PrefabInstanceComponent naming reference).
    SceneEntityId root;
};

// Reads the prefab at reference (project-relative) and instantiates it into
// scene, which must have no entities. Fails without changing the scene when
// the file is missing or does not instantiate.
PrefabResult openPrefabDocument(const std::filesystem::path& reference, Scene& scene, PrefabSystem& prefabs,
                                OpenedPrefabDocument& output);

// Writes the prefab rooted at root (the root openPrefabDocument returned)
// back to reference: captures it with keepRootTransform, saves the file
// atomically, drops prefabs' cached copy, and reports the write through hooks
// (the session's other caches and its PrefabWriteLedger). Then links the
// captured entities (linkPrefabCapture), so entities added since opening keep
// the ids this save gave them: that changes their components, so a caller
// recording history captures the scene around the call. Fails before writing
// anything when the capture fails (for example a nested cycle).
PrefabResult savePrefabDocument(Scene& scene, SceneEntityId root, const std::filesystem::path& reference,
                                PrefabSystem& prefabs, SavedPrefabDocument& output, const PrefabWriteHooks& hooks = {});
