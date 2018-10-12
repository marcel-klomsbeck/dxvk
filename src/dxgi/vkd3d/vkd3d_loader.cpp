#include <iostream>

#include <dlfcn.h>
#include <wine/library.h>

#include "vkd3d_loader.h"

// TODO get this from wine maybe?
#define SONAME_LIBVKD3D "libvkd3d.so.1"

namespace dxvk {

  VkD3DLoader::VkD3DLoader()
  : handle(wine_dlopen(SONAME_LIBVKD3D, RTLD_NOW, nullptr, 0)) {
    if (!handle)
      throw DxvkError(SONAME_LIBVKD3D ": Failed to load library");
  }


  VkD3DLoader::~VkD3DLoader() {
    wine_dlclose(handle, nullptr, 0);
  }


  void* VkD3DLoader::sym(const char* name) {
    void* ptr = wine_dlsym(handle, name, nullptr, 0);
    if (!ptr)
      Logger::err(str::format(SONAME_LIBVKD3D ": ", name, " = null"));
    return ptr;
  }

}