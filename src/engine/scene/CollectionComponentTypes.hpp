#pragma once

#include "SceneDocument.hpp"

class ComponentTypeRegistry;

// Registers the collection and visibility components ("collection_membership",
// "collection_instance", "visibility"); part of the built-in types.
SceneDocumentResult registerCollectionComponentTypes(ComponentTypeRegistry& registry);
