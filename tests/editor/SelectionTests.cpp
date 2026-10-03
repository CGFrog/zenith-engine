#include "editor/interaction/Selection.hpp"

#include <iostream>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

// Shift click ranges over rows in displayed order, which may repeat an entity.
void testRangeSelection() {
    const SceneEntityId a{1};
    const SceneEntityId b{2};
    const SceneEntityId c{3};
    const SceneEntityId d{4};
    const SceneEntityId hidden{9};
    // B is listed twice, as an entity in two collections is.
    const std::vector<SceneEntityId> displayed{c, a, b, d, b};

    expect(displayedRange(displayed, c, b) == std::vector<SceneEntityId>{c, a, b},
           "a range should run from the anchor to the first row of the clicked entity");
    expect(displayedRange(displayed, d, a) == std::vector<SceneEntityId>{a, b, d},
           "a range clicked above its anchor should still list rows in displayed order");
    expect(displayedRange(displayed, a, a) == std::vector<SceneEntityId>{a}, "a range onto itself is one row");
    expect(displayedRange(displayed, hidden, d) == std::vector<SceneEntityId>{d},
           "without a displayed anchor the range is the clicked row");
    expect(displayedRange(displayed, a, hidden).empty(), "an entity that is not displayed has no range");

    Selection selection;
    selection.assign({d});
    selection.clickRange(displayed, a, d, SelectionClickMode::Replace);
    expect(selection.ids() == std::vector<SceneEntityId>{a, b, d},
           "a range click should replace the selection and keep the anchor active");
    selection.clickRange(displayed, a, c, SelectionClickMode::Replace);
    expect(selection.ids() == std::vector<SceneEntityId>{a, c},
           "a second range click from the same anchor should shrink the range");
    selection.clickRange(displayed, a, d, SelectionClickMode::Extend);
    expect(selection.ids() == std::vector<SceneEntityId>{a, c, b, d},
           "a Ctrl Shift range click should add the range to the selection");
    selection.clickRange(displayed, hidden, b, SelectionClickMode::Replace);
    expect(selection.ids() == std::vector<SceneEntityId>{b},
           "without a displayed anchor the clicked row becomes the selection");
    selection.clickRange(displayed, a, hidden, SelectionClickMode::Replace);
    expect(selection.ids() == std::vector<SceneEntityId>{b}, "a range onto a row not displayed changes nothing");
}
} // namespace

int main() {
    testRangeSelection();
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
