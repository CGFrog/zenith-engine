# 3. Build the first-person player

The checked-in `game/player/FirstPersonController.*` is the reference
implementation. Copy it into `src/fps/player/`, rename the types, and then tune
the values for your game. Do not move controller rules into the engine: input
meaning and movement feel are game policy.

## Player component

Keep editable tuning and runtime state together for the first version:

```cpp
struct FpsControllerComponent {
    float radius{0.4f};
    float halfHeight{0.5f};
    float walkSpeed{4.0f};
    float sprintMultiplier{1.75f};
    float jumpSpeed{5.5f};
    float acceleration{30.0f};
    float airControl{0.25f};
    float lookSensitivity{0.0025f};
    float yawRadians{0.0f};
    float pitchRadians{0.0f};
    bool grounded{false};
    Entity camera{entt::null};
};
```

Create the player after loading the authored scene:

```cpp
Entity player = context.scene.createEntity("Player");
auto& playerTransform = context.scene.registry().get<TransformComponent>(player);
playerTransform.local = spawnTransform;

FpsControllerComponent controller;
Entity camera = context.scene.createEntity("Player Camera");
controller.camera = camera;
context.scene.registry().emplace<FpsControllerComponent>(player, controller);
context.scene.registry().emplace<ColliderComponent>(
    player, CapsuleCollider{controller.halfHeight, controller.radius});
context.scene.registry().emplace<RigidBodyComponent>(player, RigidBodyComponent{
    .motionType = PhysicsMotionType::Dynamic,
    .layer = PhysicsLayer::Player,
    .mass = 80.0f,
    .friction = 0.0f,
    .angularDamping = 1.0f,
    .continuousCollision = true,
    .lockRotation = true,
});

context.scene.registry().emplace<CameraComponent>(camera);
context.scene.registry().emplace<AudioListenerComponent>(camera);
context.scene.setParent(camera, player);
```

Place the camera at `halfHeight + radius` above the player's origin. The player
body owns yaw-independent translation; the camera child owns yaw and pitch.

## Split view and physics updates

Mouse look belongs in the variable-rate `update` callback so it feels smooth:

```cpp
controller.yawRadians -= input.mouseDelta.x * controller.lookSensitivity;
controller.pitchRadians = std::clamp(
    controller.pitchRadians - input.mouseDelta.y * controller.lookSensitivity,
    glm::radians(-89.0f), glm::radians(89.0f));
```

Build the camera transform from yaw followed by pitch. Clamp pitch to avoid
flipping. Movement belongs in `fixedUpdate`, which runs at the physics step:

```cpp
const float x = float(input.keyDown(Key::D)) - float(input.keyDown(Key::A));
const float z = float(input.keyDown(Key::W)) - float(input.keyDown(Key::S));
glm::vec3 direction = glm::vec3(yawMatrix * glm::vec4(x, 0.0f, -z, 0.0f));
if (glm::dot(direction, direction) > 1.0f) {
    direction = glm::normalize(direction);
}
```

Read the existing velocity with `PhysicsWorld::linearVelocity`, blend its X/Z
components toward the desired speed, preserve Y, and write it with
`setLinearVelocity`. Use `LeftShift` or `RightShift` for sprint.

## Grounding and jumping

At each fixed step, inspect `PhysicsWorld::contactEvents()`. A contact normal
pointing sufficiently upward relative to the player marks it grounded. The
normal reverses depending on whether the player is `first` or `second` in the
event; handle both cases as the demo controller does.

Only apply a jump on a pressed edge:

```cpp
if (input.keyPressed(Key::Space) && controller.grounded) {
    velocity.y = controller.jumpSpeed;
}
```

Zenith buffers pressed edges until a fixed update consumes them, so a short
Space press is not lost between physics ticks.

## Wire the callbacks

```cpp
void TrainingScene::update(ApplicationContext& context, const InputState& input, float) {
    m_controllers.updateView(context.scene, input);
}

void TrainingScene::fixedUpdate(ApplicationContext& context, const InputState& input, float dt) {
    m_controllers.fixedUpdate(context.scene, context.physics, input, dt);
}
```

Run the game and verify walking, diagonal normalization, sprinting, jumping,
wall collision, mouse capture, and Escape releasing the cursor before adding a
weapon.
