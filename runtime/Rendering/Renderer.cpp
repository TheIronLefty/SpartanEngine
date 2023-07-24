/*
Copyright(c) 2016-2023 Panos Karabelas

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
copies of the Software, and to permit persons to whom the Software is furnished
to do so, subject to the following conditions :

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

//= INCLUDES ===================================
#include "pch.h"                                
#include "Renderer.h"                           
#include "../World/Entity.h"                    
#include "../World/Components/Camera.h"         
#include "../World/Components/Light.h"          
#include "../World/Components/ReflectionProbe.h"
#include "../RHI/RHI_ConstantBuffer.h"          
#include "../RHI/RHI_StructuredBuffer.h"        
#include "../RHI/RHI_Implementation.h"          
#include "../RHI/RHI_CommandPool.h"
#include "../RHI/RHI_AMD_FidelityFX.h"
#include "../RHI/RHI_RenderDoc.h"
#include "../Core/Window.h"                     
#include "../Input/Input.h"                     
#include "../World/Components/Environment.h"
#include "../World/Components/AudioSource.h"
#include "../RHI/RHI_SwapChain.h"
#include "../Display/Display.h"
//==============================================

//= NAMESPACES ===============
using namespace std;
using namespace Spartan::Math;
//============================

namespace Spartan
{
    namespace
    {
        // states
        atomic<bool> is_rendering_allowed  = true;
        atomic<bool> flush_requested       = false;
        bool dirty_orthographic_projection = true;

        // resolution & viewport
        Math::Vector2 m_resolution_render = Math::Vector2::Zero;
        Math::Vector2 m_resolution_output = Math::Vector2::Zero;
        RHI_Viewport m_viewport           = RHI_Viewport(0, 0, 0, 0);

        // environment texture
        shared_ptr<RHI_Texture> environment_texture;
        mutex mutex_environment_texture;

        // swapchain
        const uint8_t swap_chain_buffer_count = 2;
        shared_ptr<RHI_SwapChain> swap_chain;

        // mip generation
        mutex mutex_mip_generation;
        vector<RHI_Texture*> textures_mip_generation;

        // rhi resources
        RHI_CommandPool* cmd_pool    = nullptr;
        RHI_CommandList* cmd_current = nullptr;

        // misc
        array<float, 34> m_options;
        thread::id render_thread_id;
        mutex mutex_entity_addition;
        vector<shared_ptr<Entity>> m_entities_to_add;
        shared_ptr<Camera> m_camera;
        uint64_t frame_num                   = 0;
        Math::Vector2 jitter_offset          = Math::Vector2::Zero;
        const uint32_t resolution_shadow_min = 128;
        float near_plane                     = 0.0f;
        float far_plane                      = 1.0f;

        void sort_renderables(vector<shared_ptr<Entity>>* renderables, const bool are_transparent)
        {
            if (!m_camera || renderables->size() <= 2)
                return;

            auto comparison_op = [](shared_ptr<Entity> entity)
            {
                auto renderable = entity->GetComponent<Renderable>();
                if (!renderable)
                    return 0.0f;

                return (renderable->GetAabb().GetCenter() - m_camera->GetTransform()->GetPosition()).LengthSquared();
            };

            // sort by depth
            sort(renderables->begin(), renderables->end(), [&comparison_op, &are_transparent](shared_ptr<Entity> a, shared_ptr<Entity> b)
            {
                bool front_to_back = comparison_op(a) <= comparison_op(b);
                bool back_to_front = !front_to_back;
                return are_transparent ? back_to_front : front_to_back;
            });
        }
    }

    unordered_map<Renderer_Entity, vector<shared_ptr<Entity>>> Renderer::m_renderables;
    Cb_Frame Renderer::m_cb_frame_cpu;
    Cb_Pass Renderer::m_cb_pass_cpu;
    Cb_Light Renderer::m_cb_light_cpu;
    Cb_Material Renderer::m_cb_material_cpu;
    shared_ptr<RHI_VertexBuffer> Renderer::m_vertex_buffer_lines;
    unique_ptr<Font> Renderer::m_font;
    unique_ptr<Grid> Renderer::m_world_grid;
    vector<RHI_Vertex_PosCol> Renderer::m_line_vertices;
    vector<float> Renderer::m_lines_duration;
    uint32_t Renderer::m_lines_index_depth_off;
    uint32_t Renderer::m_lines_index_depth_on;
    bool Renderer::m_brdf_specular_lut_rendered;
    uint32_t Renderer::m_resource_index = 0;

    void Renderer::Initialize()
    {
        render_thread_id             = this_thread::get_id();
        m_brdf_specular_lut_rendered = false;

        Display::DetectDisplayModes();

        // RHI Initialization
        {
            RHI_Context::Initialize();

            if (RHI_Context::renderdoc)
            {
                RHI_RenderDoc::OnPreDeviceCreation();
            }

            RHI_Device::Initialize();
        }

        // Resolution
        {
            uint32_t width  = Window::GetWidth();
            uint32_t height = Window::GetHeight();

            // The resolution of the actual rendering
            SetResolutionRender(width, height, false);

            // The resolution of the output frame *we can upscale to that linearly or with FSR 2)
            SetResolutionOutput(width, height, false);

            // The resolution/size of the editor's viewport. This is overridden by the editor based on the actual viewport size
            SetViewport(static_cast<float>(width), static_cast<float>(height));

            // Note: If the editor is active, it will set the render and viewport resolution to what the actual viewport is
        }

        // Create swap chain
        swap_chain = make_shared<RHI_SwapChain>
        (
            Window::GetHandleSDL(),
            static_cast<uint32_t>(m_resolution_output.x),
            static_cast<uint32_t>(m_resolution_output.y),
            // Present mode: For v-sync, we could Mailbox for lower latency, but Fifo is always supported, so we'll assume that
            GetOption<bool>(Renderer_Option::Vsync) ? RHI_Present_Mode::Fifo : RHI_Present_Mode::Immediate,
            swap_chain_buffer_count,
            "renderer"
        );

        // Create command pool
        cmd_pool = RHI_Device::AllocateCommandPool("renderer", swap_chain->GetObjectId(), RHI_Queue_Type::Graphics);

        RHI_AMD_FidelityFX::Initialize();

        // Adjust render option to reflect whether the swapchain is HDR or not
        SetOption(Renderer_Option::Hdr, swap_chain->IsHdr());

        // Default options
        m_options.fill(0.0f);
        SetOption(Renderer_Option::Bloom,                  0.05f); // Non-zero values activate it and define the blend factor.
        SetOption(Renderer_Option::MotionBlur,             1.0f);
        SetOption(Renderer_Option::Ssgi,                   1.0f);
        SetOption(Renderer_Option::ScreenSpaceShadows,     1.0f);
        SetOption(Renderer_Option::ScreenSpaceReflections, 1.0f);
        SetOption(Renderer_Option::Anisotropy,             16.0f);
        SetOption(Renderer_Option::ShadowResolution,       2048.0f);
        SetOption(Renderer_Option::Tonemapping,            static_cast<float>(Renderer_Tonemapping::Disabled));
        SetOption(Renderer_Option::Gamma,                  1.7f); // despite what literature says, 2.2 is too bright (optimal solution is gamma calibration screen)
        SetOption(Renderer_Option::Exposure,               1.0f);
        SetOption(Renderer_Option::PaperWhite,             150.0f); // nits
        SetOption(Renderer_Option::Sharpness,              0.5f);
        SetOption(Renderer_Option::Fog,                    0.0f);
        SetOption(Renderer_Option::Antialiasing,           static_cast<float>(Renderer_Antialiasing::TaaFxaa)); // This is using FSR 2 for TAA
        SetOption(Renderer_Option::Upsampling,             static_cast<float>(Renderer_Upsampling::FSR2));
        // Debug
        SetOption(Renderer_Option::Debug_TransformHandle,    1.0f);
        SetOption(Renderer_Option::Debug_SelectionOutline,   1.0f);
        SetOption(Renderer_Option::Debug_Grid,               1.0f);
        SetOption(Renderer_Option::Debug_ReflectionProbes,   1.0f);
        SetOption(Renderer_Option::Debug_Lights,             1.0f);
        SetOption(Renderer_Option::Debug_Physics,            0.0f);
        SetOption(Renderer_Option::Debug_PerformanceMetrics, 1.0f);
        SetOption(Renderer_Option::Vsync,                    0.0f);
        //SetOption(RendererOption::DepthOfField,        1.0f); // This is depth of field from ALDI, so until I improve it, it should be disabled by default.
        //SetOption(RendererOption::Render_DepthPrepass, 1.0f); // Depth-pre-pass is not always faster, so by default, it's disabled.
        //SetOption(RendererOption::Debanding,           1.0f); // Disable debanding as we shouldn't be seeing banding to begin with.
        //SetOption(RendererOption::VolumetricFog,       1.0f); // Disable by default because it's not that great, I need to do it with a voxelised approach.

        // Create all the resources
        CreateConstantBuffers();
        CreateShaders();
        CreateDepthStencilStates();
        CreateRasterizerStates();
        CreateBlendStates();
        CreateRenderTextures(true, true, true, true);
        CreateFonts();
        CreateSamplers(false);
        CreateStructuredBuffers();
        CreateStandardTextures();
        CreateStandardMeshes();

        // Subscribe to events
        SP_SUBSCRIBE_TO_EVENT(EventType::WorldResolved,                   SP_EVENT_HANDLER_VARIANT_STATIC(OnWorldResolved));
        SP_SUBSCRIBE_TO_EVENT(EventType::WorldClear,                      SP_EVENT_HANDLER_STATIC(OnClear));
        SP_SUBSCRIBE_TO_EVENT(EventType::WindowFullscreenWindowedToggled, SP_EVENT_HANDLER_STATIC(OnFullScreenToggled));

        // Fire event
        SP_FIRE_EVENT(EventType::RendererOnInitialized);
    }

    void Renderer::Shutdown()
    {
        // console doesn't render anymore, log to file
        Log::SetLogToFile(true);

        // Fire event
        SP_FIRE_EVENT(EventType::RendererOnShutdown);

        // Manually invoke the deconstructors so that ParseDeletionQueue(), releases their RHI resources.
        {
            DestroyResources();

            m_entities_to_add.clear();
            m_renderables.clear();
            m_world_grid.reset();
            m_font.reset();
            swap_chain            = nullptr;
            m_vertex_buffer_lines = nullptr;
            environment_texture   = nullptr;
        }

        RHI_RenderDoc::Shutdown();
        RHI_Device::QueueWaitAll();
        RHI_AMD_FidelityFX::Destroy();
        RHI_Device::DeletionQueue_Parse();
        RHI_Device::Destroy();
    }

    void Renderer::Tick()
    {
        // Don't bother producing frames if the window is minimized
        if (Window::IsMinimised())
            return;

        // After the first frame has completed, we know the renderer is working.
        // We stop logging to a file and we start logging to the on-screen console.
        if (frame_num == 1)
        {
            Log::SetLogToFile(false);
            SP_FIRE_EVENT(EventType::RendererOnFirstFrameCompleted);
        }

        // Happens when core resources are created/destroyed
        if (flush_requested)
        {
            Flush();
        }

        if (!is_rendering_allowed)
            return;

        RHI_Device::Tick(frame_num);

        // Tick command pool
        bool reset = cmd_pool->Tick();

        // Begin
        cmd_current = cmd_pool->GetCurrentCommandList();
        cmd_current->Begin();

        if (reset)
        {
            // switch to the next array of constant buffers and structured buffers
            m_resource_index = (m_resource_index + 1) % 2;

            // Reset dynamic buffer indices
            for (shared_ptr<RHI_ConstantBuffer> constant_buffer : GetConstantBuffers())
            {
                constant_buffer->ResetOffset();
            }

            GetStructuredBuffer()->ResetOffset();

            // Delete any RHI resources that have accumulated an need to be deleted
            if (RHI_Device::DeletionQueue_NeedsToParse())
            {
                RHI_Device::QueueWaitAll();
                RHI_Device::DeletionQueue_Parse();
                SP_LOG_INFO("Parsed deletion queue");
            }
        }

        OnFrameStart(cmd_current);

        // Update frame buffer
        {
            // Matrices
            {
                if (m_camera)
                {
                    if (near_plane != m_camera->GetNearPlane() || far_plane != m_camera->GetFarPlane())
                    {
                        near_plane                    = m_camera->GetNearPlane();
                        far_plane                     = m_camera->GetFarPlane();
                        dirty_orthographic_projection = true;
                    }

                    m_cb_frame_cpu.view                = m_camera->GetViewMatrix();
                    m_cb_frame_cpu.projection          = m_camera->GetProjectionMatrix();
                    m_cb_frame_cpu.projection_inverted = Matrix::Invert(m_cb_frame_cpu.projection);
                }

                if (dirty_orthographic_projection)
                { 
                    // near clip does not affect depth accuracy in orthographic projection, so set it to 0 to avoid problems which can result an infinitely small [3,2] (NaN) after the multiplication below.
                    m_cb_frame_cpu.projection_ortho      = Matrix::CreateOrthographicLH(m_viewport.width, m_viewport.height, 0.0f, far_plane);
                    m_cb_frame_cpu.view_projection_ortho = Matrix::CreateLookAtLH(Vector3(0, 0, -near_plane), Vector3::Forward, Vector3::Up) * m_cb_frame_cpu.projection_ortho;
                    dirty_orthographic_projection        = false;
                }
            }

            // generate jitter sample in case FSR (which also does TAA) is enabled
            Renderer_Upsampling upsampling_mode = GetOption<Renderer_Upsampling>(Renderer_Option::Upsampling);
            if (upsampling_mode == Renderer_Upsampling::FSR2 || GetOption<Renderer_Antialiasing>(Renderer_Option::Antialiasing) == Renderer_Antialiasing::Taa)
            {
                RHI_AMD_FidelityFX::FSR2_GenerateJitterSample(&jitter_offset.x, &jitter_offset.y);
                jitter_offset.x            = (jitter_offset.x / m_resolution_render.x);
                jitter_offset.y            = (jitter_offset.y / m_resolution_render.y);
                m_cb_frame_cpu.projection *= Matrix::CreateTranslation(Vector3(jitter_offset.x, jitter_offset.y, 0.0f));
            }
            else
            {
                jitter_offset = Vector2::Zero;
            }
            
            // update the remaining of the frame buffer
            m_cb_frame_cpu.view_projection_previous = m_cb_frame_cpu.view_projection;
            m_cb_frame_cpu.view_projection          = m_cb_frame_cpu.view * m_cb_frame_cpu.projection;
            m_cb_frame_cpu.view_projection_inv      = Matrix::Invert(m_cb_frame_cpu.view_projection);
            if (m_camera)
            {
                m_cb_frame_cpu.view_projection_unjittered = m_cb_frame_cpu.view * m_camera->GetProjectionMatrix();
                m_cb_frame_cpu.camera_aperture            = m_camera->GetAperture();
                m_cb_frame_cpu.camera_shutter_speed       = m_camera->GetShutterSpeed();
                m_cb_frame_cpu.camera_iso                 = m_camera->GetIso();
                m_cb_frame_cpu.camera_near                = m_camera->GetNearPlane();
                m_cb_frame_cpu.camera_far                 = m_camera->GetFarPlane();
                m_cb_frame_cpu.camera_position            = m_camera->GetTransform()->GetPosition();
                m_cb_frame_cpu.camera_direction           = m_camera->GetTransform()->GetForward();
            }
            m_cb_frame_cpu.resolution_output      = m_resolution_output;
            m_cb_frame_cpu.resolution_render      = m_resolution_render;
            m_cb_frame_cpu.taa_jitter_previous    = m_cb_frame_cpu.taa_jitter_current;
            m_cb_frame_cpu.taa_jitter_current     = jitter_offset;
            m_cb_frame_cpu.delta_time             = static_cast<float>(Timer::GetDeltaTimeSmoothedSec());
            m_cb_frame_cpu.time                   = static_cast<float>(Timer::GetTimeSec());
            m_cb_frame_cpu.bloom_intensity        = GetOption<float>(Renderer_Option::Bloom);
            m_cb_frame_cpu.sharpness              = GetOption<float>(Renderer_Option::Sharpness);
            m_cb_frame_cpu.fog                    = GetOption<float>(Renderer_Option::Fog);
            m_cb_frame_cpu.tonemapping            = GetOption<float>(Renderer_Option::Tonemapping);
            m_cb_frame_cpu.gamma                  = GetOption<float>(Renderer_Option::Gamma);
            m_cb_frame_cpu.exposure               = GetOption<float>(Renderer_Option::Exposure);
            m_cb_frame_cpu.luminance_max          = Display::GetLuminanceMax();
            m_cb_frame_cpu.shadow_resolution      = GetOption<float>(Renderer_Option::ShadowResolution);
            m_cb_frame_cpu.frame                  = static_cast<uint32_t>(frame_num);
            m_cb_frame_cpu.frame_mip_count        = GetRenderTarget(Renderer_RenderTexture::frame_render)->GetMipCount();
            m_cb_frame_cpu.ssr_mip_count          = GetRenderTarget(Renderer_RenderTexture::ssr)->GetMipCount();
            m_cb_frame_cpu.resolution_environment = Vector2(GetEnvironmentTexture()->GetWidth(), GetEnvironmentTexture()->GetHeight());

            // These must match what Common_Buffer.hlsl is reading
            m_cb_frame_cpu.set_bit(GetOption<bool>(Renderer_Option::ScreenSpaceReflections), 1 << 0);
            m_cb_frame_cpu.set_bit(GetOption<bool>(Renderer_Option::Ssgi),                   1 << 1);
            m_cb_frame_cpu.set_bit(GetOption<bool>(Renderer_Option::VolumetricFog),          1 << 2);
            m_cb_frame_cpu.set_bit(GetOption<bool>(Renderer_Option::ScreenSpaceShadows),     1 << 3);
        }

        Pass_Frame(cmd_current);

        if (Window::IsFullScreen())
        {
            cmd_current->BeginMarker("copy_to_back_buffer");
            cmd_current->Blit(GetRenderTarget(Renderer_RenderTexture::frame_output).get(), swap_chain.get());
            cmd_current->EndMarker();
        }

        OnFrameEnd(cmd_current);

        // submit
        cmd_current->End();
        cmd_current->Submit();

        // track frame
        frame_num++;
    }

    const RHI_Viewport& Renderer::GetViewport()
    {
        return m_viewport;
    }

    void Renderer::SetViewport(float width, float height)
    {
        SP_ASSERT_MSG(width  != 0, "Width can't be zero");
        SP_ASSERT_MSG(height != 0, "Height can't be zero");

        if (m_viewport.width != width || m_viewport.height != height)
        {
            m_viewport.width              = width;
            m_viewport.height             = height;
            dirty_orthographic_projection = true;
        }
    }

    const Vector2& Renderer::GetResolutionRender()
    {
        return m_resolution_render;
    }

    void Renderer::SetResolutionRender(uint32_t width, uint32_t height, bool recreate_resources /*= true*/)
    {
        // Early exit if the resolution is invalid
        if (!RHI_Device::IsValidResolution(width, height))
        {
            SP_LOG_WARNING("%dx%d is an invalid resolution", width, height);
            return;
        }

        // Early exit if the resoution is already set
        if (m_resolution_render.x == width && m_resolution_render.y == height)
            return;

        // Set resolution
        m_resolution_render.x = static_cast<float>(width);
        m_resolution_render.y = static_cast<float>(height);

        if (recreate_resources)
        {
            // Re-create render textures
            CreateRenderTextures(true, false, false, true);

            // Re-create samplers
            CreateSamplers(true);
        }

        // Register this resolution as a display mode so it shows up in the editor's render options (it won't happen if already registered)
        Display::RegisterDisplayMode(static_cast<uint32_t>(width), static_cast<uint32_t>(height), Display::GetRefreshRate(), Display::GetIndex());

        // Log
        SP_LOG_INFO("Render resolution has been set to %dx%d", width, height);
    }

    const Vector2& Renderer::GetResolutionOutput()
    {
        return m_resolution_output;
    }

    void Renderer::SetResolutionOutput(uint32_t width, uint32_t height, bool recreate_resources /*= true*/)
    {
        // Return if resolution is invalid
        if (!RHI_Device::IsValidResolution(width, height))
        {
            SP_LOG_WARNING("%dx%d is an invalid resolution", width, height);
            return;
        }

        // Silently return if resolution is already set
        if (m_resolution_output.x == width && m_resolution_output.y == height)
            return;

        // Set resolution
        m_resolution_output.x = static_cast<float>(width);
        m_resolution_output.y = static_cast<float>(height);

        if (recreate_resources)
        {
            // Re-create render textures
            CreateRenderTextures(false, true, false, true);

            // Re-create samplers
            CreateSamplers(true);
        }

        // Log
        SP_LOG_INFO("Output resolution output has been set to %dx%d", width, height);
    }

    void Renderer::UpdateConstantBufferFrame(RHI_CommandList* cmd_list)
    {
        GetConstantBuffer(Renderer_ConstantBuffer::Frame)->Update(&m_cb_frame_cpu);

        // Bind because the offset just changed
        cmd_list->SetConstantBuffer(Renderer_BindingsCb::frame, GetConstantBuffer(Renderer_ConstantBuffer::Frame));
    }

    void Renderer::UpdateConstantBufferPass(RHI_CommandList* cmd_list)
    {
        GetConstantBuffer(Renderer_ConstantBuffer::Pass)->Update(&m_cb_pass_cpu);

        // Bind because the offset just changed
        cmd_list->SetConstantBuffer(Renderer_BindingsCb::uber, GetConstantBuffer(Renderer_ConstantBuffer::Pass));
    }

    void Renderer::UpdateConstantBufferLight(RHI_CommandList* cmd_list, shared_ptr<Light> light, const RHI_Shader_Type scope)
    {
        for (uint32_t i = 0; i < light->GetShadowArraySize(); i++)
        {
            m_cb_light_cpu.view_projection[i] = light->GetViewMatrix(i) * light->GetProjectionMatrix(i);
        }

        m_cb_light_cpu.intensity_range_angle_bias = Vector4
        (
            light->GetIntensityForShader(m_camera.get()),
            light->GetRange(), light->GetAngle(),
            light->GetBias()
        );

        m_cb_light_cpu.color       = light->GetColor();
        m_cb_light_cpu.normal_bias = light->GetNormalBias();
        m_cb_light_cpu.position    = light->GetTransform()->GetPosition();
        m_cb_light_cpu.direction   = light->GetTransform()->GetForward();
        m_cb_light_cpu.options     = 0;
        m_cb_light_cpu.options     |= light->GetLightType() == LightType::Directional ? (1 << 0) : 0;
        m_cb_light_cpu.options     |= light->GetLightType() == LightType::Point       ? (1 << 1) : 0;
        m_cb_light_cpu.options     |= light->GetLightType() == LightType::Spot        ? (1 << 2) : 0;
        m_cb_light_cpu.options     |= light->GetShadowsEnabled()                      ? (1 << 3) : 0;
        m_cb_light_cpu.options     |= light->GetShadowsTransparentEnabled()           ? (1 << 4) : 0;
        m_cb_light_cpu.options     |= light->GetShadowsScreenSpaceEnabled()           ? (1 << 5) : 0;
        m_cb_light_cpu.options     |= light->GetVolumetricEnabled()                   ? (1 << 6) : 0;

        GetConstantBuffer(Renderer_ConstantBuffer::Light)->Update(&m_cb_light_cpu);

        // Bind because the offset just changed
        cmd_list->SetConstantBuffer(Renderer_BindingsCb::light, GetConstantBuffer(Renderer_ConstantBuffer::Light));
    }

    void Renderer::UpdateConstantBufferMaterial(RHI_CommandList* cmd_list, Material* material)
    {
        // Set
        m_cb_material_cpu.color.x              = material->GetProperty(MaterialProperty::ColorR);
        m_cb_material_cpu.color.y              = material->GetProperty(MaterialProperty::ColorG);
        m_cb_material_cpu.color.z              = material->GetProperty(MaterialProperty::ColorB);
        m_cb_material_cpu.color.w              = material->GetProperty(MaterialProperty::ColorA);
        m_cb_material_cpu.tiling_uv.x          = material->GetProperty(MaterialProperty::UvTilingX);
        m_cb_material_cpu.tiling_uv.y          = material->GetProperty(MaterialProperty::UvTilingY);
        m_cb_material_cpu.offset_uv.x          = material->GetProperty(MaterialProperty::UvOffsetX);
        m_cb_material_cpu.offset_uv.y          = material->GetProperty(MaterialProperty::UvOffsetY);
        m_cb_material_cpu.roughness_mul        = material->GetProperty(MaterialProperty::RoughnessMultiplier);
        m_cb_material_cpu.metallic_mul         = material->GetProperty(MaterialProperty::MetalnessMultiplier);
        m_cb_material_cpu.normal_mul           = material->GetProperty(MaterialProperty::NormalMultiplier);
        m_cb_material_cpu.height_mul           = material->GetProperty(MaterialProperty::HeightMultiplier);
        m_cb_material_cpu.anisotropic          = material->GetProperty(MaterialProperty::Anisotropic);
        m_cb_material_cpu.anisitropic_rotation = material->GetProperty(MaterialProperty::AnisotropicRotation);
        m_cb_material_cpu.clearcoat            = material->GetProperty(MaterialProperty::Clearcoat);
        m_cb_material_cpu.clearcoat_roughness  = material->GetProperty(MaterialProperty::Clearcoat_Roughness);
        m_cb_material_cpu.sheen                = material->GetProperty(MaterialProperty::Sheen);
        m_cb_material_cpu.sheen_tint           = material->GetProperty(MaterialProperty::SheenTint);
        m_cb_material_cpu.properties           = 0;
        m_cb_material_cpu.properties          |= material->GetProperty(MaterialProperty::SingleTextureRoughnessMetalness) ? (1U << 0) : 0;
        m_cb_material_cpu.properties          |= material->HasTexture(MaterialTexture::Height)                            ? (1U << 1) : 0;
        m_cb_material_cpu.properties          |= material->HasTexture(MaterialTexture::Normal)                            ? (1U << 2) : 0;
        m_cb_material_cpu.properties          |= material->HasTexture(MaterialTexture::Color)                             ? (1U << 3) : 0;
        m_cb_material_cpu.properties          |= material->HasTexture(MaterialTexture::Roughness)                         ? (1U << 4) : 0;
        m_cb_material_cpu.properties          |= material->HasTexture(MaterialTexture::Metalness)                        ? (1U << 5) : 0;
        m_cb_material_cpu.properties          |= material->HasTexture(MaterialTexture::AlphaMask)                         ? (1U << 6) : 0;
        m_cb_material_cpu.properties          |= material->HasTexture(MaterialTexture::Emission)                          ? (1U << 7) : 0;
        m_cb_material_cpu.properties          |= material->HasTexture(MaterialTexture::Occlusion)                         ? (1U << 8) : 0;

        // Update
        GetConstantBuffer(Renderer_ConstantBuffer::Material)->Update(&m_cb_material_cpu);

        // Bind because the offset just changed
        cmd_list->SetConstantBuffer(Renderer_BindingsCb::material, GetConstantBuffer(Renderer_ConstantBuffer::Material));
    }

    void Renderer::OnWorldResolved(sp_variant data)
    {
        // note: m_renderables is a vector of shared pointers.
        // this ensures that if any entities are deallocated by the world.
        // we'll still have some valid pointers until the are overridden by m_renderables_world.

        vector<shared_ptr<Entity>> entities = get<vector<shared_ptr<Entity>>>(data);

        lock_guard lock(mutex_entity_addition);
        m_entities_to_add.clear();

        for (shared_ptr<Entity> entity : entities)
        {
            SP_ASSERT_MSG(entity != nullptr, "Entity is null");

            if (entity->IsActiveRecursively())
            {
                m_entities_to_add.emplace_back(entity);
            }
        }
    }

    void Renderer::OnClear()
    {
        // Flush to remove references to entity resources that will be deallocated
        Flush();
        m_renderables.clear();
    }

    void Renderer::OnFullScreenToggled()
    {
        static float    width_previous_viewport  = 0;
        static float    height_previous_viewport = 0;
        static uint32_t width_previous_output    = 0;
        static uint32_t height_previous_output   = 0;

        if (Window::IsFullScreen())
        {
            uint32_t width  = Window::GetWidth();
            uint32_t height = Window::GetHeight();

            width_previous_viewport  = m_viewport.width;
            height_previous_viewport = m_viewport.height;
            SetViewport(static_cast<float>(width), static_cast<float>(height));

            width_previous_output  = static_cast<uint32_t>(m_viewport.width);
            height_previous_output = static_cast<uint32_t>(m_viewport.height);
            SetResolutionOutput(width, height);
        }
        else
        {
            SetViewport(width_previous_viewport, height_previous_viewport);
            SetResolutionOutput(width_previous_output, height_previous_output);
        }

        Input::SetMouseCursorVisible(!Window::IsFullScreen());
    }

    void Renderer::OnFrameStart(RHI_CommandList* cmd_list)
    {
        // acquire renderables
        if (!m_entities_to_add.empty())
        {
            // clear previous state
            m_renderables.clear();
            m_camera = nullptr;

            for (shared_ptr<Entity> entity : m_entities_to_add)
            {
                if (shared_ptr<Renderable> renderable = entity->GetComponent<Renderable>())
                {
                    bool is_transparent = false;
                    bool is_visible     = true;

                    if (const Material* material = renderable->GetMaterial())
                    {
                        is_transparent = material->GetProperty(MaterialProperty::ColorA) < 1.0f;
                        is_visible     = material->GetProperty(MaterialProperty::ColorA) != 0.0f;
                    }

                    if (is_visible)
                    {
                        m_renderables[is_transparent ? Renderer_Entity::GeometryTransparent : Renderer_Entity::Geometry].emplace_back(entity);
                    }
                }

                if (shared_ptr<Light> light = entity->GetComponent<Light>())
                {
                    m_renderables[Renderer_Entity::Light].emplace_back(entity);
                }

                if (shared_ptr<Camera> camera = entity->GetComponent<Camera>())
                {
                    m_renderables[Renderer_Entity::Camera].emplace_back(entity);
                    m_camera = camera;
                }

                if (shared_ptr<ReflectionProbe> reflection_probe = entity->GetComponent<ReflectionProbe>())
                {
                    m_renderables[Renderer_Entity::ReflectionProbe].emplace_back(entity);
                }

                if (shared_ptr<AudioSource> audio_source = entity->GetComponent<AudioSource>())
                {
                    m_renderables[Renderer_Entity::AudioSource].emplace_back(entity);
                }
            }

            // sort them by distance
            sort_renderables(&m_renderables[Renderer_Entity::Geometry], false);
            sort_renderables(&m_renderables[Renderer_Entity::GeometryTransparent], true);

            m_entities_to_add.clear();
        }

        // generate mips
        {
            lock_guard lock(mutex_mip_generation);
            for (RHI_Texture* texture : textures_mip_generation)
            {
                Pass_GenerateMips(cmd_list, texture);
            }
            textures_mip_generation.clear();
        }

        Lines_OneFrameStart();
    }

    void Renderer::OnFrameEnd(RHI_CommandList* cmd_list)
    {
        Lines_OnFrameEnd();
    }

    bool Renderer::IsCallingFromOtherThread()
    {
        return render_thread_id != this_thread::get_id();
    }

    const shared_ptr<RHI_Texture> Renderer::GetEnvironmentTexture()
    {
        return environment_texture ? environment_texture : GetStandardTexture(Renderer_StandardTexture::Black);
    }

    void Renderer::SetEnvironment(Environment* environment)
    {
        lock_guard lock(mutex_environment_texture);
        environment_texture = environment->GetTexture();
    }

    void Renderer::SetOption(Renderer_Option option, float value)
    {
        // Clamp value
        {
            // Anisotropy
            if (option == Renderer_Option::Anisotropy)
            {
                value = Helper::Clamp(value, 0.0f, 16.0f);
            }
            // Shadow resolution
            else if (option == Renderer_Option::ShadowResolution)
            {
                value = Helper::Clamp(value, static_cast<float>(resolution_shadow_min), static_cast<float>(RHI_Device::GetMaxTexture2dDimension()));
            }
        }

        // Early exit if the value is already set
        if (m_options[static_cast<uint32_t>(option)] == value)
            return;

        // Reject changes (if needed)
        {
            if (option == Renderer_Option::Hdr)
            {
                if (value == 1.0f && !Display::GetHdr())
                {
                    SP_LOG_INFO("This display doesn't support HDR");
                    return;
                }
            }
        }

        // Set new value
        m_options[static_cast<uint32_t>(option)] = value;

        // Handle cascading changes
        {
            // Antialiasing
            if (option == Renderer_Option::Antialiasing)
            {
                bool taa_enabled = value == static_cast<float>(Renderer_Antialiasing::Taa) || value == static_cast<float>(Renderer_Antialiasing::TaaFxaa);
                bool fsr_enabled = GetOption<Renderer_Upsampling>(Renderer_Option::Upsampling) == Renderer_Upsampling::FSR2;

                if (taa_enabled)
                {
                    // Implicitly enable FSR since it's doing TAA.
                    if (!fsr_enabled)
                    {
                        m_options[static_cast<uint32_t>(Renderer_Option::Upsampling)] = static_cast<float>(Renderer_Upsampling::FSR2);
                        RHI_AMD_FidelityFX::FSR2_ResetHistory();
                        SP_LOG_INFO("Enabled FSR 2.0 since it's used for TAA.");
                    }
                }
                else
                {
                    // Implicitly disable FSR since it's doing TAA
                    if (fsr_enabled)
                    {
                        m_options[static_cast<uint32_t>(Renderer_Option::Upsampling)] = static_cast<float>(Renderer_Upsampling::Linear);
                        SP_LOG_INFO("Disabed FSR 2.0 since it's used for TAA.");
                    }
                }
            }
            // Upsampling
            else if (option == Renderer_Option::Upsampling)
            {
                bool taa_enabled = GetOption<Renderer_Antialiasing>(Renderer_Option::Antialiasing) == Renderer_Antialiasing::Taa;

                if (value == static_cast<float>(Renderer_Upsampling::Linear))
                {
                    // Implicitly disable TAA since FSR 2.0 is doing it
                    if (taa_enabled)
                    {
                        m_options[static_cast<uint32_t>(Renderer_Option::Antialiasing)] = static_cast<float>(Renderer_Antialiasing::Disabled);
                        SP_LOG_INFO("Disabled TAA since it's done by FSR 2.0.");
                    }
                }
                else if (value == static_cast<float>(Renderer_Upsampling::FSR2))
                {
                    // Implicitly enable TAA since FSR 2.0 is doing it
                    if (!taa_enabled)
                    {
                        m_options[static_cast<uint32_t>(Renderer_Option::Antialiasing)] = static_cast<float>(Renderer_Antialiasing::Taa);
                        RHI_AMD_FidelityFX::FSR2_ResetHistory();
                        SP_LOG_INFO("Enabled TAA since FSR 2.0 does it.");
                    }
                }
            }
            // Shadow resolution
            else if (option == Renderer_Option::ShadowResolution)
            {
                const auto& light_entities = m_renderables[Renderer_Entity::Light];
                for (const auto& light_entity : light_entities)
                {
                    auto light = light_entity->GetComponent<Light>();
                    if (light->GetShadowsEnabled())
                    {
                        light->CreateShadowMap();
                    }
                }
            }
            else if (option == Renderer_Option::Hdr)
            {
                swap_chain->SetHdr(value == 1.0f);
            }
            else if (option == Renderer_Option::Vsync)
            {
                swap_chain->SetVsync(value == 1.0f);
            }
        }
    }

    array<float, 34>& Renderer::GetOptions()
    {
        return m_options;
    }

    void Renderer::SetOptions(array<float, 34> options)
    {
        m_options = options;
    }

    RHI_SwapChain* Renderer::GetSwapChain()
    {
        return swap_chain.get();
    }
    
    void Renderer::Present()
    {
        if (!is_rendering_allowed)
            return;

        SP_ASSERT_MSG(!Window::IsMinimised(), "Don't call present if the window is minimized");
        SP_ASSERT(swap_chain->GetLayout() == RHI_Image_Layout::Present_Src);

        swap_chain->Present();

        SP_FIRE_EVENT(EventType::RendererPostPresent);
    }

    void Renderer::Flush()
    {
        // The external thread requests a flush from the renderer thread (to avoid a myriad of thread issues and Vulkan errors)
        if (IsCallingFromOtherThread())
        {
            is_rendering_allowed = false;
            flush_requested      = true;

            while (flush_requested)
            {
                SP_LOG_INFO("External thread is waiting for the renderer thread to flush...");
                this_thread::sleep_for(chrono::milliseconds(16));
            }

            return;
        }

        // Flushing
        if (!is_rendering_allowed)
        {
            SP_LOG_INFO("Renderer thread is flushing...");
            RHI_Device::QueueWaitAll();
        }

        flush_requested = false;
    }

    void Renderer::AddTextureForMipGeneration(RHI_Texture* texture)
    {
        lock_guard<mutex> guard(mutex_mip_generation);
        textures_mip_generation.push_back(texture);
    }

    RHI_CommandList* Renderer::GetCmdList()
    {
        return cmd_current;
    }

    RHI_Api_Type Renderer::GetRhiApiType()
    {
        return RHI_Context::api_type;
    }

    RHI_Texture* Renderer::GetFrameTexture()
    {
        return GetRenderTarget(Renderer_RenderTexture::frame_output).get();
    }

    uint64_t Renderer::GetFrameNum()
    {
        return frame_num;
    }

    shared_ptr<Camera> Renderer::GetCamera()
    {
        return m_camera;
    }

    unordered_map<Renderer_Entity, vector<shared_ptr<Entity>>>& Renderer::GetEntities()
    {
        return m_renderables;
    }
}
