#pragma once

struct EditorContext;

// The viewport sidebar's Navigation tab: Bake and Clear for the editor's
// preview mesh (navigation.bake, navigation.clear) with its stats, the
// project settings it bakes with and a button to edit them (project.settings
// {tab: navigation}), the viewport's Navigation Mesh overlay
// (showNavigationMesh), and in Play the running game's mesh, which it never
// bakes.
void drawNavigationSidebarTab(EditorContext& context, bool& showNavigationMesh);
