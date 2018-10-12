#include "vkd3d_presenter.h"

namespace dxvk {

  VkD3DPresenter::VkD3DPresenter(
            IDXGIFactory4*            pFactory,
            ID3D12CommandQueue*       pQueue,
            HWND                      hWnd,
            DXGI_SWAP_CHAIN_DESC1*    pDesc)
  : m_factory (pFactory),
    m_parent  (pQueue),
    m_window  (hWnd),
    m_desc    (*pDesc) {
    if (FAILED(pQueue->GetDevice(__uuidof(ID3D12Device),
        reinterpret_cast<void**>(&m_device))))
      throw DxvkError("VkD3DPresenter: Failed to query D3D12 device");
    
    // Retrieve Vulkan device and instance from the D3D12 device
    auto vulkanInstance = m_vkd3d.vkd3d_instance_get_vk_instance(
      m_vkd3d.vkd3d_instance_from_device(m_device.ptr()));
    auto vulkanDevice   = m_vkd3d.vkd3d_get_vk_device(m_device.ptr());
    auto vulkanAdapter  = m_vkd3d.vkd3d_get_vk_physical_device(m_device.ptr());
    auto vulkanQueue    = m_vkd3d.vkd3d_get_vk_queue_family_index(pQueue);

    if (!vulkanInstance || !vulkanDevice)
      throw DxvkError("VkD3DPresenter: Failed to retrieve Vulkan objects");
    
    // Load Vulkan function pointers so we can use the API directly
    m_vki = new vk::InstanceFn(false, vulkanInstance);
    m_vkd = new vk::DeviceFn  (false, vulkanInstance, vulkanDevice);

    // Create the presenter
    m_deviceInfo.queueFamily   = vulkanQueue;
    m_deviceInfo.adapter       = vulkanAdapter;
    m_vkd->vkGetDeviceQueue(m_vkd->device(), vulkanQueue, 0, &m_deviceInfo.queue);

    vk::PresenterDesc presentDesc;
    presentDesc.imageExtent     = { pDesc->Width, pDesc->Height };
    presentDesc.imageCount      = pDesc->BufferCount;
    presentDesc.numFormats      = PickFormats(pDesc->Format, presentDesc.formats);
    presentDesc.numPresentModes = PickPresentModes(m_vsync, presentDesc.presentModes);

    m_presenter = new vk::Presenter(hWnd,
      m_vki, m_vkd, m_deviceInfo, presentDesc);

    // Create D3D12 back buffers
    if (FAILED(CreateBackBuffers()))
      throw DxvkError("VkD3DPresenter: Failed to create back buffers");
  }


  VkD3DPresenter::~VkD3DPresenter() {
    DestroyBackBuffers();
  }


  HRESULT STDMETHODCALLTYPE VkD3DPresenter::QueryInterface(
          REFIID                    riid,
          void**                    ppvObject) {
    InitReturnPtr(ppvObject);

    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(IDXGIVkSwapChain)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    Logger::warn("VkD3DPresenter::QueryInterface: Unknown interface query");
    return E_NOINTERFACE;
  }


  HRESULT STDMETHODCALLTYPE VkD3DPresenter::GetDesc(
          DXGI_SWAP_CHAIN_DESC1*    pDesc) {
    *pDesc = m_desc;
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE VkD3DPresenter::GetAdapter(
          REFIID                    riid,
          void**                    ppvObject) {
    // Querying the actual device causes random crashes for no reason
    Logger::warn("VkD3DPresenter::GetAdapter: Stub");
    return m_factory->EnumAdapterByLuid(LUID(), riid, ppvObject);
  }


  HRESULT STDMETHODCALLTYPE VkD3DPresenter::GetDevice(
          REFIID                    riid,
          void**                    ppDevice) {
    return m_parent->QueryInterface(riid, ppDevice);
  }

  
  HRESULT STDMETHODCALLTYPE VkD3DPresenter::GetImage(
          UINT                      BufferId,
          REFIID                    riid,
          void**                    ppBuffer) {
    InitReturnPtr(ppBuffer);

    if (BufferId >= m_backBuffers.size())
      return DXGI_ERROR_INVALID_CALL;
    
    return m_backBuffers[BufferId].resource->QueryInterface(riid, ppBuffer);
  }


  UINT STDMETHODCALLTYPE VkD3DPresenter::GetImageIndex() {
    return m_backBufferId;
  }


  HRESULT STDMETHODCALLTYPE VkD3DPresenter::ChangeProperties(
    const DXGI_SWAP_CHAIN_DESC1*    pDesc) {
    m_desc = *pDesc;

    if (RecreateSwapChain(m_vsync) != VK_SUCCESS)
      return E_FAIL;

    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE VkD3DPresenter::SetPresentRegion(
    const RECT*                     pRegion) {
    Logger::err("VkD3DPresenter::SetPresentRegion: Not implemented");
    return E_NOTIMPL;
  }


  HRESULT STDMETHODCALLTYPE VkD3DPresenter::SetGammaControl(
          UINT                      NumControlPoints,
    const DXGI_RGB*                 pControlPoints) {
    Logger::err("VkD3DPresenter::SetGammaControl: Not implemented");
    return E_NOTIMPL;
  }


  HRESULT STDMETHODCALLTYPE VkD3DPresenter::Present(
          UINT                      SyncInterval,
          UINT                      PresentFlags,
    const DXGI_PRESENT_PARAMETERS*  pPresentParameters) {
    bool vsync = SyncInterval != 0;

    if (vsync != m_vsync) {
      if (RecreateSwapChain(vsync) != VK_SUCCESS)
        return E_FAIL;
    }

    vk::PresenterInfo  info = m_presenter->info();
    vk::PresenterSync  sync = m_presenter->getSyncSemaphores();

    uint32_t swapImageId = 0;
    VkResult status = m_presenter->acquireNextImage(sync.acquire, swapImageId);

    while (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR) {
      if (RecreateSwapChain(vsync) != VK_SUCCESS)
        return E_FAIL;
      
      info = m_presenter->info();
      sync = m_presenter->getSyncSemaphores();

      status = m_presenter->acquireNextImage(sync.acquire, swapImageId);
    }

    vk::PresenterImage image = m_presenter->getImage(swapImageId);
    
    const BackBuffer& bb = m_backBuffers[m_backBufferId];

    status = m_vkd->vkWaitForFences(
      m_vkd->device(), 1, &bb.fence, VK_FALSE,
      std::numeric_limits<uint64_t>::max());
    
    if (status != VK_SUCCESS)
      return E_FAIL;
    
    status = m_vkd->vkResetFences(
      m_vkd->device(), 1, &bb.fence);

    if (status != VK_SUCCESS)
      return E_FAIL;
    
    status = m_vkd->vkResetCommandPool(
      m_vkd->device(), bb.cmdPool, 0);
    
    if (status != VK_SUCCESS)
      return E_FAIL;
    
    VkCommandBufferBeginInfo cmdBegin;
    cmdBegin.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cmdBegin.pNext            = nullptr;
    cmdBegin.flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    cmdBegin.pInheritanceInfo = nullptr;

    status = m_vkd->vkBeginCommandBuffer(bb.cmdBuffer, &cmdBegin);

    if (status != VK_SUCCESS)
      return E_FAIL;
    
    VkImageMemoryBarrier imgBarrier;
    imgBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imgBarrier.pNext               = nullptr;
    imgBarrier.srcAccessMask       = 0;
    imgBarrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    imgBarrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    imgBarrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    imgBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imgBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imgBarrier.image               = image.image;
    imgBarrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    m_vkd->vkCmdPipelineBarrier(bb.cmdBuffer,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      0, 0, nullptr, 0, nullptr,
      1, &imgBarrier);
    
    VkImageBlit blitRegion;
    blitRegion.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
    blitRegion.srcOffsets[0]  = { 0, 0, 0 };
    blitRegion.srcOffsets[1]  = { int32_t(m_desc.Width), int32_t(m_desc.Height), 1 };
    blitRegion.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
    blitRegion.dstOffsets[0]  = { 0, 0, 0 };
    blitRegion.dstOffsets[1]  = { int32_t(info.imageExtent.width), int32_t(info.imageExtent.height), 1 };
    
    VkFilter filter = VK_FILTER_NEAREST;

    m_vkd->vkCmdBlitImage(bb.cmdBuffer,
      bb.image,    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      1, &blitRegion, filter);

    imgBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    imgBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    
    m_vkd->vkCmdPipelineBarrier(bb.cmdBuffer,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      0, 0, nullptr, 0, nullptr,
      1, &imgBarrier);
    
    status = m_vkd->vkEndCommandBuffer(bb.cmdBuffer);

    if (status != VK_SUCCESS)
      return E_FAIL;
    
    VkPipelineStageFlags syncStages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    VkSubmitInfo submitInfo;
    submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.pNext                = nullptr;
    submitInfo.waitSemaphoreCount   = 1;
    submitInfo.pWaitSemaphores      = &sync.acquire;
    submitInfo.pWaitDstStageMask    = &syncStages;
    submitInfo.commandBufferCount   = 1;
    submitInfo.pCommandBuffers      = &bb.cmdBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores    = &sync.present;
    
    status = m_vkd->vkQueueSubmit(
      m_deviceInfo.queue, 1, &submitInfo, bb.fence);
    
    if (status != VK_SUCCESS)
      return E_FAIL;
    
    status = m_presenter->presentImage(sync.present);

    if (status != VK_SUCCESS)
      status = RecreateSwapChain(vsync);

    if (status != VK_SUCCESS)
      return E_FAIL;
    
    m_backBufferId += 1;
    m_backBufferId %= m_backBuffers.size();
    return S_OK;
  }


  HRESULT VkD3DPresenter::Create(
          IDXGIFactory4*            pFactory,
          ID3D12CommandQueue*       pQueue,
          HWND                      hWnd,
          DXGI_SWAP_CHAIN_DESC1*    pDesc,
          IDXGIVkSwapChain**        ppSwapChain) {
    try {
      *ppSwapChain = ref(new VkD3DPresenter(pFactory, pQueue, hWnd, pDesc));
      return S_OK;
    } catch (const DxvkError& e) {
      Logger::err(e.message());
      return E_FAIL;
    }
  }


  uint32_t VkD3DPresenter::PickFormats(
          DXGI_FORMAT               Format,
          VkSurfaceFormatKHR*       pDstFormats) {
    uint32_t n = 0;

    switch (Format) {
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_B8G8R8A8_UNORM: {
        pDstFormats[n++] = { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;
      
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: {
        pDstFormats[n++] = { VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;
      
      case DXGI_FORMAT_R10G10B10A2_UNORM: {
        pDstFormats[n++] = { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;
      
      case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        pDstFormats[n++] = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;
      
      default:
        Logger::warn(str::format("VkD3DPresenter: Unknown format: ", m_desc.Format));
    }

    return n;
  }


  uint32_t VkD3DPresenter::PickPresentModes(
          BOOL                      Vsync,
          VkPresentModeKHR*         pDstModes) {
    uint32_t n = 0;

    if (Vsync) {
      pDstModes[n++] = VK_PRESENT_MODE_FIFO_KHR;
    } else {
      pDstModes[n++] = VK_PRESENT_MODE_IMMEDIATE_KHR;
      pDstModes[n++] = VK_PRESENT_MODE_MAILBOX_KHR;
      pDstModes[n++] = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    }

    return n;
  }


  VkResult VkD3DPresenter::RecreateSwapChain(BOOL Vsync) {
    vk::PresenterDesc presentDesc;
    presentDesc.imageExtent     = { m_desc.Width, m_desc.Height };
    presentDesc.imageCount      = m_desc.BufferCount;
    presentDesc.numFormats      = PickFormats(m_desc.Format, presentDesc.formats);
    presentDesc.numPresentModes = PickPresentModes(Vsync, presentDesc.presentModes);

    m_vsync = Vsync;
    return m_presenter->recreateSwapChain(presentDesc);
  }


  HRESULT VkD3DPresenter::CreateBackBuffers() {
    m_backBuffers.resize(m_desc.BufferCount);

    VkPhysicalDeviceMemoryProperties memInfo;
    m_vki->vkGetPhysicalDeviceMemoryProperties(m_deviceInfo.adapter, &memInfo);
    
    VkImageCreateInfo imgInfo;
    imgInfo.sType           = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.pNext           = nullptr;
    imgInfo.flags           = 0;
    imgInfo.imageType       = VK_IMAGE_TYPE_2D;
    imgInfo.format          = m_vkd3d.vkd3d_get_vk_format(m_desc.Format);
    imgInfo.extent          = { m_desc.Width, m_desc.Height, 1 };
    imgInfo.mipLevels       = 1;
    imgInfo.arrayLayers     = 1;
    imgInfo.samples         = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling          = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage           = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                            | VK_IMAGE_USAGE_SAMPLED_BIT
                            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                            | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgInfo.sharingMode     = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.queueFamilyIndexCount = 0;
    imgInfo.pQueueFamilyIndices   = nullptr;
    imgInfo.initialLayout   = VK_IMAGE_LAYOUT_UNDEFINED;

    vkd3d_image_resource_create_info resInfo;
    resInfo.type = VKD3D_STRUCTURE_TYPE_IMAGE_RESOURCE_CREATE_INFO;
    resInfo.next = nullptr;
    resInfo.vk_image        = VK_NULL_HANDLE;
    resInfo.desc.Dimension  = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resInfo.desc.Alignment  = 0;
    resInfo.desc.Width      = m_desc.Width;
    resInfo.desc.Height     = m_desc.Height;
    resInfo.desc.DepthOrArraySize = 1;
    resInfo.desc.MipLevels  = 1;
    resInfo.desc.Format     = m_desc.Format;
    resInfo.desc.SampleDesc = { 1, 0 };
    resInfo.desc.Layout     = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resInfo.desc.Flags      = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    resInfo.flags           = VKD3D_RESOURCE_INITIAL_STATE_TRANSITION
                            | VKD3D_RESOURCE_PRESENT_STATE_TRANSITION;
    resInfo.present_state   = D3D12_RESOURCE_STATE_COPY_SOURCE;
    
    if (m_desc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS) {
      imgInfo.usage      |= VK_IMAGE_USAGE_STORAGE_BIT;
      resInfo.desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }

    for (uint32_t i = 0; i < m_desc.BufferCount; i++) {
      if (m_vkd->vkCreateImage(m_vkd->device(), &imgInfo, nullptr, &m_backBuffers[i].image) != VK_SUCCESS) {
        Logger::err("VkD3DPresenter: Failed to create D3D12 back buffers");
        DestroyBackBuffers();
        return E_FAIL;
      }

      VkMemoryRequirements memRequirements;
      m_vkd->vkGetImageMemoryRequirements(
        m_vkd->device(), m_backBuffers[i].image, &memRequirements);
      
      // Just use a dedicated allocation, may benefit Nvidia
      VkMemoryDedicatedAllocateInfoKHR allocInfoDedicated;
      allocInfoDedicated.sType  = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR;
      allocInfoDedicated.pNext  = VK_NULL_HANDLE;
      allocInfoDedicated.buffer = VK_NULL_HANDLE;
      allocInfoDedicated.image  = m_backBuffers[i].image;

      VkMemoryAllocateInfo allocInfo;
      allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      allocInfo.pNext           = &allocInfoDedicated;
      allocInfo.allocationSize  = memRequirements.size;

      for (uint32_t t = 0; t < memInfo.memoryTypeCount && !m_backBuffers[i].memory; t++) {
        allocInfo.memoryTypeIndex = t;

        if ((memRequirements.memoryTypeBits & (1 << t))
         && (memInfo.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
          m_vkd->vkAllocateMemory(m_vkd->device(),
            &allocInfo, nullptr, &m_backBuffers[i].memory);
        }
      }

      for (uint32_t t = 0; t < memInfo.memoryTypeCount && !m_backBuffers[i].memory; t++) {
        allocInfo.memoryTypeIndex = t;

        if (memRequirements.memoryTypeBits & (1 << t)) {
          m_vkd->vkAllocateMemory(m_vkd->device(),
            &allocInfo, nullptr, &m_backBuffers[i].memory);
        }
      }

      if (!m_backBuffers[i].memory) {
        Logger::err("VkD3DPresenter: Failed to allocate memory for D3D12 back buffers");
        DestroyBackBuffers();
        return E_FAIL;
      }

      if (m_vkd->vkBindImageMemory(m_vkd->device(), m_backBuffers[i].image, m_backBuffers[i].memory, 0) != VK_SUCCESS) {
        Logger::err("VkD3DPresenter: Failed to bind memory to D3D12 back buffers");
        DestroyBackBuffers();
        return E_FAIL;
      }

      // Create a command buffer and pool that will be used
      // for presentation. We're only going to perform blit
      // or resolve operations.
      VkCommandPoolCreateInfo cmdPoolInfo;
      cmdPoolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      cmdPoolInfo.pNext            = nullptr;
      cmdPoolInfo.flags            = 0;
      cmdPoolInfo.queueFamilyIndex = m_deviceInfo.queueFamily;

      if (m_vkd->vkCreateCommandPool(m_vkd->device(), &cmdPoolInfo, nullptr, &m_backBuffers[i].cmdPool) != VK_SUCCESS) {
        Logger::err("VkD3DPresenter: Failed to create command pool");
        DestroyBackBuffers();
        return E_FAIL;
      }

      VkCommandBufferAllocateInfo cmdBufInfo;
      cmdBufInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      cmdBufInfo.pNext              = nullptr;
      cmdBufInfo.commandPool        = m_backBuffers[i].cmdPool;
      cmdBufInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      cmdBufInfo.commandBufferCount = 1;

      if (m_vkd->vkAllocateCommandBuffers(m_vkd->device(), &cmdBufInfo, &m_backBuffers[i].cmdBuffer) != VK_SUCCESS) {
        Logger::err("VkD3DPresenter: Failed to create command buffer");
        DestroyBackBuffers();
        return E_FAIL;
      }

      // Create a signaled fence that we're going to
      // wait on before recording command buffers
      VkFenceCreateInfo fenceInfo;
      fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      fenceInfo.pNext = nullptr;
      fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

      if (m_vkd->vkCreateFence(m_vkd->device(), &fenceInfo, nullptr, &m_backBuffers[i].fence) != VK_SUCCESS) {
        Logger::err("VkD3DPresenter: Failed to create fence");
        DestroyBackBuffers();
        return E_FAIL;
      }

      // Create a D3D12 resource for this image. As in D3D11,
      // we cannot keep a strong reference to the image.
      resInfo.vk_image = m_backBuffers[i].image;

      HRESULT hr = m_vkd3d.vkd3d_create_image_resource(
        m_device.ptr(), &resInfo, &m_backBuffers[i].resource);
      
      if (FAILED(hr)) {
        Logger::err("VkD3DPresenter: Failed to create D3D12 resources");
        DestroyBackBuffers();
        return hr;
      }

      m_vkd3d.vkd3d_resource_incref(m_backBuffers[i].resource);
      m_backBuffers[i].resource->Release();
    }

    return S_OK;
  }


  void VkD3DPresenter::DestroyBackBuffers() {
    m_vkd->vkDeviceWaitIdle(m_vkd->device());

    for (const auto& buf : m_backBuffers) {
      if (buf.resource)
        m_vkd3d.vkd3d_resource_decref(buf.resource);

      m_vkd->vkDestroyCommandPool(m_vkd->device(), buf.cmdPool, nullptr);
      m_vkd->vkDestroyFence(m_vkd->device(), buf.fence, nullptr);
      m_vkd->vkDestroyImage(m_vkd->device(), buf.image, nullptr);
      m_vkd->vkFreeMemory(m_vkd->device(), buf.memory, nullptr);
    }
    
    m_backBuffers.clear();
  }

}