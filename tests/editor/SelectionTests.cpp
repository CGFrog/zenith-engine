#include "editor/interaction/Selection.hpp"

#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}
} // namespace

int main() {
    Scene scene;
    const Entity parent = scene.createEntity("Parent");
    const Entity child = scene.createEntity("Child");
    const Entity other = scene.createEntity("Other");
    scene.setParent(child, parent);
    const SceneEntityId parentId = scene.id(parent);
    const SceneEntityId childId = scene.id(child);
    const SceneEntityId otherId = scene.id(other);

    Selection selection;
    expect(selection.empty() && !selection.active(), "a new selection should be empty with no active entity");

    selection.click(parentId, SelectionClickMode::Replace);
    selection.click(otherId, SelectionClickMode::Extend);
    expect(selection.size() == 2 && selection.active() == otherId,
           "extending a selection should make the clicked entity active");
    selection.click(parentId, SelectionClickMode::Extend);
    expect(selection.size() == 2 && selection.active() == parentId,
           "extend-clicking a selected entity should make it active without deselecting it");
    selection.click(parentId, SelectionClickMode::Extend);
    expect(selection.size() == 1 && selection.active() == otherId,
           "extend-clicking the active entity should deselect it");
    selection.click(childId, SelectionClickMode::Replace);
    expect(selection.size() == 1 && selection.active() == childId, "a plain click should replace the selection");

    selection.selectAll(scene);
    expect(selection.size() == 3 && selection.active() == childId,
           "select all should include every entity and keep the active entity active");
    selection.selectOnly(otherId);
    selection.invert(scene);
    expect(selection.size() == 2 && !selection.contains(otherId) && selection.contains(parentId) &&
               selection.contains(childId),
           "invert should select exactly the unselected entities");

    const std::vector<SceneEntityId> roots = selection.transformRoots(scene);
    expect(roots.size() == 1 && roots.front() == parentId,
           "a child selected with its parent should not be a separate transform root");

    scene.destroyEntity(scene.findEntity(parentId));
    selection.removeMissing(scene);
    expect(selection.empty(), "removing missing entities should drop a destroyed parent and its children");

    if (failures != 0) {
        std::cerr << failures << " selection test(s) failed\n";
        return 1;
    }
    return 0;
}
