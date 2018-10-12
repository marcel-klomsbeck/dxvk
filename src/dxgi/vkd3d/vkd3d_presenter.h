#pragma once

#include "vkd3d_loader.h"

#include "../../vulkan/vulkan_presenter.h"

namespace dxvk {

  /**
   * \brief vkd3d presenter
   */
  class VkD3DPresenter : public ComObject<IDXGIVkSwapChain> {

  public:

    VkD3DPresenter(
            IDXGIFactory4*            pFactory,
            ID3D12CommandQueue*       pQueue,
            HWND                      hWnd,
            DXGI_SWAP_CHAIN_DESC1*    pDesc);
    
    ~VkD3DPresenter();

    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID                    riid,
            void**                    ppvObject);

    HRESULT STDMETHODCALLTYPE GetDesc(
            DXGI_SWAP_CHAIN_DESC1*    pDesc);

    HRESULT STDMETHODCALLTYPE GetAdapter(
            REFIID                    riid,
            void**                    ppvObject);
    
    HRESULT STDMETHODCALLTYPE GetDevice(
            REFIID                    riid,
            void**                    ppDevice);

    HRESULT STDMETHODCALLTYPE GetImage(
            UINT                      BufferId,
            REFIID                    riid,
            void**                    ppBuffer);

    UINT STDMETHODCALLTYPE GetImageIndex();

    HRESULT STDMETHODCALLTYPE ChangeProperties(
      const DXGI_SWAP_CHAIN_DESC1*    pDesc);

    HRESULT STDMETHODCALLTYPE SetPresentRegion(
      const RECT*                     pRegion);

    HRESULT STDMETHODCALLTYPE SetGammaControl(
            UINT                      NumControlPoints,
      const DXGI_RGB*                 pControlPoints);

    HRESULT STDMETHODCALLTYPE Present(
            UINT                      SyncInterval,
            UINT                      PresentFlags,
      const DXGI_PRESENT_PARAMETERS*  pPresentParameters);
    
    static HRESULT Create(
            IDXGIFactory4*            pFactory,
            ID3D12CommandQueue*       pQueue,
            HWND                      hWnd,
            DXGI_SWAP_CHAIN_DESC1*    pDesc,
            IDXGIVkSwapChain**        ppSwapChain);

  private:

    struct BackBuffer {
      VkImage               image       = VK_NULL_HANDLE;
      VkDeviceMemory        memory      = VK_NULL_HANDLE;
      VkCommandPool         cmdPool     = VK_NULL_HANDLE;
      VkCommandBuffer       cmdBuffer   = VK_NULL_HANDLE;
      VkFence               fence       = VK_NULL_HANDLE;
      ID3D12Resource*       resource    = nullptr;
    };

    struct CmdBuffer {
    };

    VkD3DLoader             m_vkd3d;

    Com<IDXGIFactory4>      m_factory;
    Com<ID3D12CommandQueue> m_parent;
    Com<ID3D12Device>       m_device;

    HWND                    m_window;
    DXGI_SWAP_CHAIN_DESC1   m_desc;

    vk::PresenterDevice     m_deviceInfo;

    Rc<vk::InstanceFn>      m_vki;
    Rc<vk::DeviceFn>        m_vkd;
    Rc<vk::Presenter>       m_presenter;

    uint32_t                m_backBufferId = 0;
    std::vector<BackBuffer> m_backBuffers;

    bool                    m_vsync = true;
    
    uint32_t PickFormats(
            DXGI_FORMAT               Format,
            VkSurfaceFormatKHR*       pDstFormats);
    
    uint32_t PickPresentModes(
            BOOL                      Vsync,
            VkPresentModeKHR*         pDstModes);

    VkResult RecreateSwapChain(
            BOOL                      Vsync);

    HRESULT CreateBackBuffers();

    void DestroyBackBuffers();

  };

}