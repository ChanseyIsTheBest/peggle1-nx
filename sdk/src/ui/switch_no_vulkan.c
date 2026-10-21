// peggle2-nx (2026-10-20): a Switch title linked without the Vulkan driver (REXGLUE_SWITCH_LINK_NVK OFF; its renderer is
// its own, on deko3d). The SDK's and plume's Vulkan code reach the driver through these entry points only (everything
// else through the function pointers they return); here they report that there is no Vulkan, so a Vulkan instance or
// device is never made and the code that would use one gives up as it does without a driver. posix_memalign was
// Mesa's (newlib has none); FFmpeg's allocator calls it.
#include <errno.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*PFN_vkVoidFunction)(void);
typedef int32_t VkResult;
#define VK_ERROR_INCOMPATIBLE_DRIVER (-9)

PFN_vkVoidFunction vkGetInstanceProcAddr(void *instance, const char *name) {
  (void)instance;
  (void)name;
  return NULL;
}

void vkDestroyInstance(void *instance, const void *allocator) {
  (void)instance;
  (void)allocator;
}

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(void *instance, const char *name) {
  (void)instance;
  (void)name;
  return NULL;
}

VkResult vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *version) {
  (void)version;
  return VK_ERROR_INCOMPATIBLE_DRIVER;
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
  if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
    return EINVAL;
  void *p = memalign(alignment, size ? size : 1);
  if (!p)
    return ENOMEM;
  *memptr = p;
  return 0;
}
