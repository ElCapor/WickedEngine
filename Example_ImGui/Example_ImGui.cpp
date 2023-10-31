#include "stdafx.h"
#include "Example_ImGui.h"

#include "ImGui/imgui.h"
#include "ImGui/imgui_internal.h"

#ifdef _WIN32
#include "ImGui/imgui_impl_win32.h"
#elif defined(SDL2)
#include "ImGui/imgui_impl_sdl.h"
#endif

#include <fstream>
#include <thread>
#include <iostream>
#include <utility>
using namespace wi::ecs;
using namespace wi::scene;
using namespace wi::graphics;

Shader imguiVS;
Shader imguiPS;
Texture fontTexture;
Sampler sampler;
InputLayout	imguiInputLayout;
PipelineState imguiPSO;

struct ImGui_Impl_Data
{
};

static ImGui_Impl_Data* ImGui_Impl_GetBackendData()
{
	return ImGui::GetCurrentContext() ? (ImGui_Impl_Data*)ImGui::GetIO().BackendRendererUserData : nullptr;
}

bool ImGui_Impl_CreateDeviceObjects()
{
	auto* backendData = ImGui_Impl_GetBackendData();

	// Build texture atlas
	ImGuiIO& io = ImGui::GetIO();

	unsigned char* pixels;
	int width, height;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

	// Upload texture to graphics system
	TextureDesc textureDesc;
	textureDesc.width = width;
	textureDesc.height = height;
	textureDesc.mip_levels = 1;
	textureDesc.array_size = 1;
	textureDesc.format = Format::R8G8B8A8_UNORM;
	textureDesc.bind_flags = BindFlag::SHADER_RESOURCE;

	SubresourceData textureData;
	textureData.data_ptr = pixels;
	textureData.row_pitch = width * GetFormatStride(textureDesc.format);
	textureData.slice_pitch = textureData.row_pitch * height;

	wi::graphics::GetDevice()->CreateTexture(&textureDesc, &textureData, &fontTexture);

	SamplerDesc samplerDesc;
	samplerDesc.address_u = TextureAddressMode::WRAP;
	samplerDesc.address_v = TextureAddressMode::WRAP;
	samplerDesc.address_w = TextureAddressMode::WRAP;
	samplerDesc.filter = Filter::MIN_MAG_MIP_LINEAR;
	wi::graphics::GetDevice()->CreateSampler(&samplerDesc, &sampler);

	// Store our identifier
	io.Fonts->SetTexID((ImTextureID)&fontTexture);

	imguiInputLayout.elements =
	{
		{ "POSITION", 0, Format::R32G32_FLOAT, 0, (uint32_t)IM_OFFSETOF(ImDrawVert, pos), InputClassification::PER_VERTEX_DATA },
		{ "TEXCOORD", 0, Format::R32G32_FLOAT, 0, (uint32_t)IM_OFFSETOF(ImDrawVert, uv), InputClassification::PER_VERTEX_DATA },
		{ "COLOR", 0, Format::R8G8B8A8_UNORM, 0, (uint32_t)IM_OFFSETOF(ImDrawVert, col), InputClassification::PER_VERTEX_DATA },
	};

	// Create pipeline
	PipelineStateDesc desc;
	desc.vs = &imguiVS;
	desc.ps = &imguiPS;
	desc.il = &imguiInputLayout;
	desc.dss = wi::renderer::GetDepthStencilState(wi::enums::DSSTYPE_DEPTHREAD);
	desc.rs = wi::renderer::GetRasterizerState(wi::enums::RSTYPE_DOUBLESIDED);
	desc.bs = wi::renderer::GetBlendState(wi::enums::BSTYPE_TRANSPARENT);
	desc.pt = PrimitiveTopology::TRIANGLELIST;
	wi::graphics::GetDevice()->CreatePipelineState(&desc, &imguiPSO);

	return true;
}

Example_ImGui::~Example_ImGui()
{
	// Cleanup
	//ImGui_ImplDX11_Shutdown();
#ifdef _WIN32
	ImGui_ImplWin32_Shutdown();
#elif defined(SDL2)
	ImGui_ImplSDL2_Shutdown();
#endif
	ImGui::DestroyContext();
}

void Example_ImGui::Initialize()
{
	// Compile shaders
	{
		auto shaderPath = wi::renderer::GetShaderSourcePath();
		wi::renderer::SetShaderSourcePath(wi::helper::GetCurrentPath() + "/");

		wi::renderer::LoadShader(ShaderStage::VS, imguiVS, "ImGuiVS.cso");
		wi::renderer::LoadShader(ShaderStage::PS, imguiPS, "ImGuiPS.cso");

		wi::renderer::SetShaderSourcePath(shaderPath);
	}

	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	//io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	//io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

	// Setup Dear ImGui style
	ImGui::StyleColorsDark();
	//ImGui::StyleColorsClassic();

#ifdef _WIN32
	ImGui_ImplWin32_Init(window);
#elif defined(SDL2)
	ImGui_ImplSDL2_InitForVulkan(window);
#endif

	IM_ASSERT(io.BackendRendererUserData == NULL && "Already initialized a renderer backend!");

	// Setup backend capabilities flags
	ImGui_Impl_Data* bd = IM_NEW(ImGui_Impl_Data)();
	io.BackendRendererUserData = (void*)bd;
	io.BackendRendererName = "Wicked";
	io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;  // We can honor the ImDrawCmd::VtxOffset field, allowing for large meshes.

	Application::Initialize();

	infoDisplay.active = true;
	infoDisplay.watermark = true;
	infoDisplay.fpsinfo = true;
	infoDisplay.resolution = true;
	infoDisplay.heap_allocation_counter = true;

	renderer.init(canvas);
	renderer.Load();

	ActivatePath(&renderer);
}

void Example_ImGui::Compose(wi::graphics::CommandList cmd)
{
	Application::Compose(cmd);

	// Rendering
	ImGui::Render();

	auto drawData = ImGui::GetDrawData();

	if (!drawData || drawData->TotalVtxCount == 0)
	{
		return;
	}

	// Avoid rendering when minimized, scale coordinates for retina displays (screen coordinates != framebuffer coordinates)
	int fb_width = (int)(drawData->DisplaySize.x * drawData->FramebufferScale.x);
	int fb_height = (int)(drawData->DisplaySize.y * drawData->FramebufferScale.y);
	if (fb_width <= 0 || fb_height <= 0)
		return;

	auto* bd = ImGui_Impl_GetBackendData();

	GraphicsDevice* device = wi::graphics::GetDevice();

	// Get memory for vertex and index buffers
	const uint64_t vbSize = sizeof(ImDrawVert) * drawData->TotalVtxCount;
	const uint64_t ibSize = sizeof(ImDrawIdx) * drawData->TotalIdxCount;
	auto vertexBufferAllocation = device->AllocateGPU(vbSize, cmd);
	auto indexBufferAllocation = device->AllocateGPU(ibSize, cmd);

	// Copy and convert all vertices into a single contiguous buffer
	ImDrawVert* vertexCPUMem = reinterpret_cast<ImDrawVert*>(vertexBufferAllocation.data);
	ImDrawIdx* indexCPUMem = reinterpret_cast<ImDrawIdx*>(indexBufferAllocation.data);
	for (int cmdListIdx = 0; cmdListIdx < drawData->CmdListsCount; cmdListIdx++)
	{
		const ImDrawList* drawList = drawData->CmdLists[cmdListIdx];
		memcpy(vertexCPUMem, &drawList->VtxBuffer[0], drawList->VtxBuffer.Size * sizeof(ImDrawVert));
		memcpy(indexCPUMem, &drawList->IdxBuffer[0], drawList->IdxBuffer.Size * sizeof(ImDrawIdx));
		vertexCPUMem += drawList->VtxBuffer.Size;
		indexCPUMem += drawList->IdxBuffer.Size;
	}

	// Setup orthographic projection matrix into our constant buffer
	struct ImGuiConstants
	{
		float   mvp[4][4];
	};

	{
		const float L = drawData->DisplayPos.x;
		const float R = drawData->DisplayPos.x + drawData->DisplaySize.x;
		const float T = drawData->DisplayPos.y;
		const float B = drawData->DisplayPos.y + drawData->DisplaySize.y;

		//Matrix4x4::CreateOrthographicOffCenter(0.0f, drawData->DisplaySize.x, drawData->DisplaySize.y, 0.0f, 0.0f, 1.0f, &constants.projectionMatrix);

		ImGuiConstants constants;

		float mvp[4][4] =
		{
			{ 2.0f / (R - L),   0.0f,           0.0f,       0.0f },
			{ 0.0f,         2.0f / (T - B),     0.0f,       0.0f },
			{ 0.0f,         0.0f,           0.5f,       0.0f },
			{ (R + L) / (L - R),  (T + B) / (B - T),    0.5f,       1.0f },
		};
		memcpy(&constants.mvp, mvp, sizeof(mvp));

		device->BindDynamicConstantBuffer(constants, 0, cmd);
	}

	const GPUBuffer* vbs[] = {
		&vertexBufferAllocation.buffer,
	};
	const uint32_t strides[] = {
		sizeof(ImDrawVert),
	};
	const uint64_t offsets[] = {
		vertexBufferAllocation.offset,
	};

	device->BindVertexBuffers(vbs, 0, 1, strides, offsets, cmd);
	device->BindIndexBuffer(&indexBufferAllocation.buffer, IndexBufferFormat::UINT16, indexBufferAllocation.offset, cmd);

	Viewport viewport;
	viewport.width = (float)fb_width;
	viewport.height = (float)fb_height;
	device->BindViewports(1, &viewport, cmd);

	device->BindPipelineState(&imguiPSO, cmd);

	device->BindSampler(&sampler, 0, cmd);

	// Will project scissor/clipping rectangles into framebuffer space
	ImVec2 clip_off = drawData->DisplayPos;         // (0,0) unless using multi-viewports
	ImVec2 clip_scale = drawData->FramebufferScale; // (1,1) unless using retina display which are often (2,2)

	//passEncoder->SetSampler(0, Sampler::LinearWrap());

	// Render command lists
	int32_t vertexOffset = 0;
	uint32_t indexOffset = 0;
	for (uint32_t cmdListIdx = 0; cmdListIdx < (uint32_t)drawData->CmdListsCount; ++cmdListIdx)
	{
		const ImDrawList* drawList = drawData->CmdLists[cmdListIdx];
		for (uint32_t cmdIndex = 0; cmdIndex < (uint32_t)drawList->CmdBuffer.size(); ++cmdIndex)
		{
			const ImDrawCmd* drawCmd = &drawList->CmdBuffer[cmdIndex];
			if (drawCmd->UserCallback)
			{
				// User callback, registered via ImDrawList::AddCallback()
				// (ImDrawCallback_ResetRenderState is a special callback value used by the user to request the renderer to reset render state.)
				if (drawCmd->UserCallback == ImDrawCallback_ResetRenderState)
				{
				}
				else
				{
					drawCmd->UserCallback(drawList, drawCmd);
				}
			}
			else
			{
				// Project scissor/clipping rectangles into framebuffer space
				ImVec2 clip_min(drawCmd->ClipRect.x - clip_off.x, drawCmd->ClipRect.y - clip_off.y);
				ImVec2 clip_max(drawCmd->ClipRect.z - clip_off.x, drawCmd->ClipRect.w - clip_off.y);
				if (clip_max.x < clip_min.x || clip_max.y < clip_min.y)
					continue;

				// Apply scissor/clipping rectangle
				Rect scissor;
				scissor.left = (int32_t)(clip_min.x);
				scissor.top = (int32_t)(clip_min.y);
				scissor.right = (int32_t)(clip_max.x);
				scissor.bottom = (int32_t)(clip_max.y);
				device->BindScissorRects(1, &scissor, cmd);

				const Texture* texture = (const Texture*)drawCmd->TextureId;
				device->BindResource(texture, 0, cmd);
				device->DrawIndexed(drawCmd->ElemCount, indexOffset, vertexOffset, cmd);
			}
			indexOffset += drawCmd->ElemCount;
		}
		vertexOffset += drawList->VtxBuffer.size();
	}

	//// Update and Render additional Platform Windows
	//if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
	//{
	//	ImGui::UpdatePlatformWindows();
	//	//ImGui::RenderPlatformWindowsDefault(NULL, (void*)g_pd3dCommandList);
	//}
}

void Example_ImGuiRenderer::ResizeLayout()
{
	RenderPath3D::ResizeLayout();

	float screenW = GetLogicalWidth();
	float screenH = GetLogicalHeight();
	label.SetPos(XMFLOAT2(screenW / 2.f - label.scale.x / 2.f, screenH * 0.95f));
}

void Example_ImGuiRenderer::Render() const
{
	RenderPath3D::Render();
}

void Example_ImGuiRenderer::Load()
{
	setSSREnabled(false);
	setReflectionsEnabled(true);
	setFXAAEnabled(false);

	label.Create("Label1");
	label.SetText("Wicked Engine ImGui integration");
	label.font.params.h_align = wi::font::WIFALIGN_CENTER;
	label.SetSize(XMFLOAT2(240, 20));
	GetGUI().AddWidget(&label);

	// Reset all state that tests might have modified:
	wi::eventhandler::SetVSync(true);
	wi::renderer::SetToDrawGridHelper(false);
	wi::renderer::SetTemporalAAEnabled(true);
	wi::renderer::ClearWorld(wi::scene::GetScene());
	wi::scene::GetScene().weather = WeatherComponent();
	this->ClearSprites();
	this->ClearFonts();
	if (wi::lua::GetLuaState() != nullptr) {
		wi::lua::KillProcesses();
	}

	// Reset camera position:
	TransformComponent transform;
	transform.Translate(XMFLOAT3(0, 2.f, -4.5f));
	transform.UpdateTransform();
	wi::scene::GetCamera().TransformCamera(transform);

	// Load model.
	wi::scene::LoadModel("../Content/models/teapot.wiscene");

	

	RenderPath3D::Load();
	AllocConsole();
	freopen_s((FILE**)stdout, "CONOUT$", "w", stdout);
}

bool show_demo_window = true;
bool show_another_window = false;
ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

typedef struct
{
	int line; // code line the error is at
	std::string errorMsg;
} LuaErrorStruct;

bool RunScriptImpl(const char* script, LuaErrorStruct& errorStruct)
{
	if (luaL_loadstring(wi::lua::GetLuaState(), script) == LUA_OK)
	{
		if (lua_pcall(wi::lua::GetLuaState(), 0, LUA_MULTRET, 0) != LUA_OK)
		{
			goto PostError;
		}
		return true;
	}
	else {
		goto PostError;
	}

	PostError:
	const char* errorStr = lua_tostring(wi::lua::GetLuaState(), -1);
	if (errorStr != nullptr)
	{
		std::cout << errorStr << std::endl;
		std::string errorfr = std::string(errorStr);
		int line = errorfr.find("]:") + 2;
		int codeline = std::atoi((errorfr.substr(line, errorfr.find(":", line) - line)).c_str());
		errorStruct.line = codeline;
		int line2 = errorfr.find(":", line) + 1;
		errorStruct.errorMsg = errorfr.substr(line2);

		lua_pop(wi::lua::GetLuaState(), 1); // remove error message
		return false;
	}
	else {
		return false;
	}
 }

bool started = false;
bool showCodeEditor = false;
bool showScriptingApiDoc = false;
#include "imgui_markdown.h"
static ImFont* H1 = NULL;
static ImFont* H2 = NULL;
static ImFont* H3 = NULL;

static ImGui::MarkdownConfig mdConfig;

inline ImGui::MarkdownImageData ImageCallback(ImGui::MarkdownLinkCallbackData data_)
{
	// In your application you would load an image based on data_ input. Here we just use the imgui font texture.
	ImTextureID image = ImGui::GetIO().Fonts->TexID;
	// > C++14 can use ImGui::MarkdownImageData imageData{ true, false, image, ImVec2( 40.0f, 20.0f ) };
	ImGui::MarkdownImageData imageData;
	imageData.isValid = true;
	imageData.useLinkCallback = false;
	imageData.user_texture_id = image;
	imageData.size = ImVec2(40.0f, 20.0f);

	// For image resize when available size.x > image width, add
	ImVec2 const contentSize = ImGui::GetContentRegionAvail();
	if (imageData.size.x > contentSize.x)
	{
		float const ratio = imageData.size.y / imageData.size.x;
		imageData.size.x = contentSize.x;
		imageData.size.y = contentSize.x * ratio;
	}

	return imageData;
}

void LoadFonts(float fontSize_ = 12.0f)
{
	ImGuiIO& io = ImGui::GetIO();
	io.Fonts->Clear();
	// Base font
	io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\Arial.ttf", fontSize_);
	// Bold headings H2 and H3
	H2 = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\Verdana.ttf", fontSize_);
	H3 = mdConfig.headingFormats[1].font;
	// bold heading H1
	float fontSizeH1 = fontSize_ * 1.1f;
	H1 = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\Verdana.ttf", fontSizeH1);
}

void ExampleMarkdownFormatCallback(const ImGui::MarkdownFormatInfo& markdownFormatInfo_, bool start_)
{
	// Call the default first so any settings can be overwritten by our implementation.
	// Alternatively could be called or not called in a switch statement on a case by case basis.
	// See defaultMarkdownFormatCallback definition for furhter examples of how to use it.
	ImGui::defaultMarkdownFormatCallback(markdownFormatInfo_, start_);

	switch (markdownFormatInfo_.type)
	{
		// example: change the colour of heading level 2
	case ImGui::MarkdownFormatType::HEADING:
	{
		if (markdownFormatInfo_.level == 2)
		{
			if (start_)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
			}
			else
			{
				ImGui::PopStyleColor();
			}
		}
		break;
	}
	default:
	{
		break;
	}
	}
}

void LinkCallback(const ImGui::MarkdownLinkCallbackData& data_) {
	std::string url(data_.link, data_.linkLength);
	std::cout << url << std::endl;
	// Check if the link is a local reference (e.g., anchor within the document)
	if (url[0] == '#') {
		// Handle local navigation here, e.g., scroll to the specified section or anchor
		// You would need to implement your own logic for navigating to the local content
		// based on the anchor provided in the URL.
		std::cout << "Local Reference" << std::endl;

		// Example: Scroll to a section with the specified anchor
		if (strcmp(url.c_str(), "#section1") == 0) {
			// Code to scroll to the "section1" in your ImGui window
			// Implement your logic to scroll to the desired location.
		}
	}
	// Handle external links (e.g., open in a web browser) if needed
	else {
		// ShellExecuteA( NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL );
	}
}


void Markdown(const std::string& markdown_)
{
	// You can make your own Markdown function with your prefered string container and markdown config.
	// > C++14 can use ImGui::MarkdownConfig mdConfig{ LinkCallback, NULL, ImageCallback, ICON_FA_LINK, { { H1, true }, { H2, true }, { H3, false } }, NULL };
	mdConfig.linkCallback = LinkCallback;
	mdConfig.tooltipCallback = NULL;
	mdConfig.imageCallback = ImageCallback;
	mdConfig.linkIcon = NULL;
	mdConfig.headingFormats[0] = { H1, true };
	mdConfig.headingFormats[1] = { H2, true };
	mdConfig.headingFormats[2] = { H3, false };
	mdConfig.userData = NULL;
	mdConfig.formatCallback = ExampleMarkdownFormatCallback;
	ImGui::Markdown(markdown_.c_str(), markdown_.length(), mdConfig);
}

#include "ImGui/imgui_md.h"


//Fonts and images (ImTextureID) must be loaded in other place
//see https://github.com/ocornut/imgui/blob/master/docs/FONTS.md
ImFont* g_font_regular = nullptr;
ImFont* g_font_bold = nullptr;
ImFont* g_font_bold_large = nullptr;
ImTextureID g_texture1 = nullptr;

struct my_markdown : public imgui_md
{
public:

	void initFont()
	{
		ImGuiIO& io = ImGui::GetIO();
		g_font_regular = ImGui::GetDefaultFont();
		g_font_bold = ImGui::GetDefaultFont();
		g_font_bold_large = ImGui::GetDefaultFont();
	}
	ImFont* get_font() const override
	{
		if (m_is_table_header) {
			return g_font_bold;
		}

		switch (m_hlevel)
		{
		case 0:
			return m_is_strong ? g_font_bold : g_font_regular;
		case 1:
			return g_font_bold_large;
		default:
			return g_font_bold;
		}
	};

	void open_url() const override
	{
		//platform dependent code
		std::cout << "Open url lmao " << m_href << std::endl;
	}

	bool get_image(image_info& nfo) const override
	{
		//use m_href to identify images
		nfo.texture_id = g_texture1;
		nfo.size = { 40,20 };
		nfo.uv0 = { 0,0 };
		nfo.uv1 = { 1,1 };
		nfo.col_tint = { 1,1,1,1 };
		nfo.col_border = { 0,0,0,0 };
		return true;
	}

	void html_div(const std::string& dclass, bool e) override
	{
		if (dclass == "red") {
			if (e) {
				m_table_border = false;
				ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));
			}
			else {
				ImGui::PopStyleColor();
				m_table_border = true;
			}
		}
	}
};

//call this function to render your markdown
void markdown(const std::string& aString)
{
	static my_markdown s_printer;
	s_printer.initFont();
	s_printer.print(aString);
}


int FindAllEntitiesLua(lua_State* L)
{
	wi::unordered_set<wi::ecs::Entity> listOfLoadedEntities;
	GetScene().FindAllEntities(listOfLoadedEntities);

	luaL_newmetatable(L, "test");
	int metatable = lua_gettop(L);

	for (auto entt : listOfLoadedEntities)
	{
		NameComponent* name = GetScene().names.GetComponent(entt);

		lua_pushstring(L, name ? name->name.c_str() : "Unknown");
		wi::lua::SSetLongLong(L, entt);
		lua_settable(L, metatable);
	}
	return 1;
	

}
void Example_ImGuiRenderer::Update(float dt)
{
	// Start the Dear ImGui frame
	auto* backendData = ImGui_Impl_GetBackendData();
	IM_ASSERT(backendData != NULL);

	if (!fontTexture.IsValid())
	{
		ImGui_Impl_CreateDeviceObjects();
	}

#ifdef _WIN32
	ImGui_ImplWin32_NewFrame();
#elif defined(SDL2)
	ImGui_ImplSDL2_NewFrame();
#endif
	ImGui::NewFrame();
	if (!started)
	{
		editor = new TextEditor();
		auto lang = TextEditor::LanguageDefinition::Lua();
		editor->SetLanguageDefinition(TextEditor::LanguageDefinition::WickedEngineLua());
		editor->SetPalette(TextEditor::GetMarianaPalette());
		started = true;
	}

	
	if (ImGui::Begin("Funny Window"))
	{
		ImGui::Checkbox("Code editor", &showCodeEditor);
		
		wi::unordered_set<wi::ecs::Entity> listOfLoadedEntities;
		GetScene().FindAllEntities(listOfLoadedEntities);
		if (ImGui::TreeNode("Scene"))
		{
			for (wi::ecs::Entity entt : listOfLoadedEntities)
			{
				NameComponent* name = GetScene().names.GetComponent(entt);
				
				if (ImGui::TreeNode(name ? name->name.c_str() : "Unnamed"))
				{
					ImGui::SameLine();
					if (ImGui::SmallButton("Delete"))
					{
						GetScene().Entity_Remove(entt);
					}
					ImGui::TreePop();
				}
				

			}
			ImGui::TreePop();
		}
		std::vector<uint8_t> bytes;
		wi::helper::FileRead("../Content/Documentation/ScriptingAPI-Documentation.md", bytes);
		std::string doc = std::string(bytes.begin(), bytes.end());
		Markdown(doc); //
		//markdown(doc);
		ImGui::End();
	}
	if (showCodeEditor)
	{
		std::pair<int, int> cpos;
		editor->GetCursorPosition(cpos.first, cpos.second);
		ImGui::Begin("Code Editor", nullptr, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_MenuBar);
		ImGui::SetWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
		if (ImGui::BeginMenuBar())
		{
			if (ImGui::BeginMenu("File"))
			{
				if (ImGui::MenuItem("Save"))
				{
					auto textToSave = editor->GetText();
					/// save text....
				}

				if (ImGui::MenuItem("Execute"))
				{
					LuaErrorStruct errorStruct;
					RunScriptImpl(editor->GetText().c_str(), errorStruct);

					std::cout << errorStruct.line << " " << errorStruct.errorMsg;
					std::map<int, std::string> errors;
					errors[errorStruct.line] = errorStruct.errorMsg;
					editor->SetErrorMarkers(errors);

				}

				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Edit"))
			{
				bool ro = editor->IsReadOnlyEnabled();
				if (ImGui::MenuItem("Read-only mode", nullptr, &ro))
					editor->SetReadOnlyEnabled(ro);
				ImGui::Separator();

				if (ImGui::MenuItem("Undo", "ALT-Backspace", nullptr, !ro && editor->CanUndo()))
					editor->Undo();
				if (ImGui::MenuItem("Redo", "Ctrl-Y", nullptr, !ro && editor->CanRedo()))
					editor->Redo();

				ImGui::Separator();

				if (ImGui::MenuItem("Copy", "Ctrl-C", nullptr, editor->AnyCursorHasSelection()))
					editor->Copy();
				if (ImGui::MenuItem("Cut", "Ctrl-X", nullptr, !ro && editor->AnyCursorHasSelection()))
					editor->Cut();
				if (ImGui::MenuItem("Undo", "Del", nullptr, !ro && editor->AnyCursorHasSelection()))
					editor->Undo();
				if (ImGui::MenuItem("Paste", "Ctrl-V", nullptr, !ro && ImGui::GetClipboardText() != nullptr))
					editor->Paste();

				ImGui::Separator();

				if (ImGui::MenuItem("Select all", nullptr, nullptr))
					editor->SelectAll();

				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("View"))
			{
				if (ImGui::MenuItem("Dark palette"))
					editor->SetPalette(TextEditor::GetDarkPalette());
				if (ImGui::MenuItem("Light palette"))
					editor->SetPalette(TextEditor::GetLightPalette());
				if (ImGui::MenuItem("Retro blue palette"))
					editor->SetPalette(TextEditor::GetRetroBluePalette());
				if (ImGui::MenuItem("Mariana palette"))
					editor->SetPalette(TextEditor::GetMarianaPalette());
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Misc"))
			{
				if (ImGui::MenuItem("BackLog View"))
				{
					std::cout << wi::backlog::getText() << std::endl;
				}
				ImGui::EndMenu();
			}
			ImGui::EndMenuBar();
		}

		ImGui::Text("%6d/%-6d %6d lines  | %s | %s | %s | %s", cpos.first + 1, cpos.second + 1, editor->GetLineCount(),
			editor->IsOverwriteEnabled() ? "Ovr" : "Ins",
			editor->CanUndo() ? "*" : " ",
			editor->GetLanguageDefinition().mName.c_str(), "Temp.lua");
		editor->Render("Lua Editor");
		ImGui::Dummy(ImVec2(20, 20));
		ImGui::End();
	}
	

	
	
	Scene& scene = wi::scene::GetScene();
	// teapot_material Base Base_mesh Top Top_mesh editorLight
	wi::ecs::Entity e_teapot_base = scene.Entity_FindByName("Base");
	wi::ecs::Entity e_teapot_top = scene.Entity_FindByName("Top");
	assert(e_teapot_base != wi::ecs::INVALID_ENTITY);
	assert(e_teapot_top != wi::ecs::INVALID_ENTITY);
	TransformComponent* transform_base = scene.transforms.GetComponent(e_teapot_base);
	TransformComponent* transform_top = scene.transforms.GetComponent(e_teapot_top);
	assert(transform_base != nullptr);
	assert(transform_top != nullptr);
	float rotation = dt;
	if (wi::input::Down(wi::input::KEYBOARD_BUTTON_LEFT))
	{
		transform_base->Rotate(XMVectorSet(0, rotation, 0, 1));
		transform_top->Rotate(XMVectorSet(0, rotation, 0, 1));
	}
	else if (wi::input::Down(wi::input::KEYBOARD_BUTTON_RIGHT))
	{
		transform_base->Rotate(XMVectorSet(0, -rotation, 0, 1));
		transform_top->Rotate(XMVectorSet(0, -rotation, 0, 1));
	}

	RenderPath3D::Update(dt);
}
