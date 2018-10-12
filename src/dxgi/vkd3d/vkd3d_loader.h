#pragma once

#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif // __GNUC__

#include "../dxgi_interfaces.h"

#include "../../util/log/log.h"

#include "../../util/util_error.h"
#include "../../util/util_string.h"

#define VKD3D_NO_PROTOTYPES
#define VKD3D_NO_VULKAN_H
#define VKD3D_NO_WIN32_TYPES
#define WINE_VK_HOST
#include <d3d12.h>
#include <vkd3d.h>

#define VKD3D_FN(func) \
  VkD3DFn<PFN_ ## func> func = sym(#func)

namespace dxvk {

  template<typename T>
  class VkD3DFn;

  template<typename Ret, typename... Args>
  class VkD3DFn<Ret (*)(Args...)> {
    using FnType = Ret (*)(Args...);
  public:

    VkD3DFn(void* fn)
    : m_fn(reinterpret_cast<FnType>(fn)) { }

    Ret operator () (Args... args) const {
      return (*m_fn)(args...);
    }

  private:

    FnType m_fn;

  };


  /**
   * \brief VkD3D library loader
   */
  class VkD3DLoader {

  public:

    VkD3DLoader();
    ~VkD3DLoader();

    void* handle = nullptr;

    VKD3D_FN(vkd3d_instance_decref);
    VKD3D_FN(vkd3d_instance_incref);
    VKD3D_FN(vkd3d_instance_get_vk_instance);

    VKD3D_FN(vkd3d_get_vk_device);
    VKD3D_FN(vkd3d_get_vk_physical_device);
    VKD3D_FN(vkd3d_instance_from_device);

    VKD3D_FN(vkd3d_get_vk_queue_family_index);
    VKD3D_FN(vkd3d_acquire_vk_queue);
    VKD3D_FN(vkd3d_release_vk_queue);

    VKD3D_FN(vkd3d_create_image_resource);
    VKD3D_FN(vkd3d_resource_decref);
    VKD3D_FN(vkd3d_resource_incref);

    VKD3D_FN(vkd3d_get_vk_format);

  private:

    void* sym(const char* name);

  };

}