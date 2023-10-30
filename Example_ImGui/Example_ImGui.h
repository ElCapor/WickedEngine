#pragma once
#include "WickedEngine.h"
#define IMGUI_EDITOR_NO_BOOST
#include "ImGui/TextEditor.h"

class Example_ImGuiRenderer : public wi::RenderPath3D
{
	TextEditor* editor = nullptr;
	wi::gui::Label label;
public:
	void Load() override;
	void Update(float dt) override;
	void ResizeLayout() override;
	void Render() const override;
};

class Example_ImGui : public wi::Application
{
	Example_ImGuiRenderer renderer;

public:
	~Example_ImGui() override;
	void Initialize() override;
	void Compose(wi::graphics::CommandList cmd) override;
};

