# 2. Play and inspect the course

[Back to the outline](README.md)

## Play

Press **F5** (`play.start`). The active viewport becomes the game view.

- Click it to capture the mouse.
- Walk with WASD, look with the mouse, jump with Space, and sprint with Shift.
- Shoot the targets with the left mouse button. Each takes two hits.
- **Escape** releases the mouse and opens the pause menu. Settings has the
  look sensitivity and invert look.
- Walk into the exit at the far end. **Shift F5** stops.

Stopping restores the scene as it was before Play, so the destroyed targets
come back.

## The pieces

Select **Player** in the Outliner. It has the engine's **Character
Controller** (`CharacterControllerComponent`), which moves with the `Move`,
`Jump`, and `Sprint` actions. Its child **Camera** has the **First Person
Camera** (look and pitch limits) and the template's **Hitscan Weapon**
behaviour: range, damage, cooldown, the fire action, and the impact prefab.

Select a target. It has a model, a collider, a static rigid body, **Tags**
(`target`), and the **Target** behaviour with its health. The **Exit** is a
rigid body with **Sensor** set and the **Goal Zone** behaviour.

Behaviour sections have two extras in their header:

- **Edit Script** opens the source where the module registers the type.
- A badge shows when the module is **Building**, when the last build
  **failed** (the behaviour runs its previous code), or when the module
  **failed to load**.

## Edit while playing

Press F5 again and select a target. Its section shows the running values. Set
its health to 200, and it takes eight hits. Right click the Target header and
choose **Apply to Edit Scene** to keep the value after Stop, as one undoable
edit. Other changes made in Play are lost on Stop.

Next: [Write a behaviour](03-write-a-behaviour.md).
