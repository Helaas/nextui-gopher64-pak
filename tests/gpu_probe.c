/*
 * GPU capabilities probe for tg5050 (Mali-G57)
 * Tests all viable rendering + display paths to determine
 * what works for getting accelerated graphics on screen.
 *
 * Cross-compile with:
 *   clang --target=aarch64-unknown-linux-gnu --sysroot=$SYSROOT \
 *     -fuse-ld=lld -o gpu_probe gpu_probe.c -ldl
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

/* ===== Minimal Vulkan types (avoid header dependency) ===== */
typedef int32_t  VkResult;
typedef uint32_t VkBool32;
typedef uint32_t VkFlags;
typedef uint64_t VkDeviceSize;
typedef struct VkInstance_T*       VkInstance;
typedef struct VkPhysicalDevice_T* VkPhysicalDevice;
typedef struct VkDevice_T*        VkDevice;
typedef struct VkQueue_T*         VkQueue;
typedef struct VkSurfaceKHR_T*    VkSurfaceKHR;
typedef struct VkSwapchainKHR_T*  VkSwapchainKHR;
typedef struct VkDisplayKHR_T*    VkDisplayKHR;
typedef struct VkImage_T*         VkImage;
typedef struct VkDeviceMemory_T*  VkDeviceMemory;
typedef struct VkBuffer_T*        VkBuffer;
typedef struct VkCommandPool_T*   VkCommandPool;
typedef struct VkCommandBuffer_T* VkCommandBuffer;
typedef void* PFN_vkVoidFunction;

#define VK_SUCCESS 0
#define VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO 1
#define VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO 3
#define VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO 2
#define VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO 12
#define VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO 5
#define VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO 39
#define VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO 40
#define VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO 42
#define VK_STRUCTURE_TYPE_SUBMIT_INFO 4
#define VK_QUEUE_COMPUTE_BIT 0x00000002
#define VK_QUEUE_GRAPHICS_BIT 0x00000001
#define VK_BUFFER_USAGE_STORAGE_BUFFER_BIT 0x00000020
#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT 0x00000002
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT 0x00000004
#define VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT 0x00000002
#define VK_COMMAND_BUFFER_LEVEL_PRIMARY 0
#define VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT 0x00000001

typedef struct { uint32_t width; uint32_t height; } VkExtent2D;
typedef struct { uint32_t width; uint32_t height; uint32_t depth; } VkExtent3D;

typedef struct {
    uint32_t queueFlags;
    uint32_t queueCount;
    uint32_t timestampValidBits;
    VkExtent3D minImageTransferGranularity;
} VkQueueFamilyProperties;

typedef struct {
    uint32_t apiVersion; uint32_t driverVersion;
    uint32_t vendorID; uint32_t deviceID; uint32_t deviceType;
    char deviceName[256]; uint8_t pipelineCacheUUID[16];
    /* VkPhysicalDeviceLimits + VkPhysicalDeviceSparseProperties omitted */
    uint8_t _pad[1024];
} VkPhysicalDeviceProperties;

typedef struct {
    uint32_t memoryTypeCount;
    struct { uint32_t propertyFlags; uint32_t heapIndex; } memoryTypes[32];
    uint32_t memoryHeapCount;
    struct { VkDeviceSize size; uint32_t flags; } memoryHeaps[16];
} VkPhysicalDeviceMemoryProperties;

typedef struct { char extensionName[256]; uint32_t specVersion; } VkExtensionProperties;

typedef struct {
    VkDisplayKHR display; const char *displayName;
    VkExtent2D physicalDimensions; VkExtent2D physicalResolution;
    uint32_t supportedTransforms; VkBool32 planeReorderPossible; VkBool32 persistentContent;
} VkDisplayPropertiesKHR;

typedef struct {
    VkDisplayKHR currentDisplay; uint32_t currentStackIndex;
} VkDisplayPlanePropertiesKHR;

typedef struct {
    uint32_t sType; const void *pNext; uint32_t flags;
} VkHeadlessSurfaceCreateInfoEXT;
#define VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT 1000256000

typedef struct {
    uint32_t sType; const void *pNext; uint32_t flags;
    const void *pApplicationInfo;
    uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames;
} VkInstanceCreateInfo;

typedef struct {
    uint32_t sType; const void *pNext; uint32_t flags;
    uint32_t queueFamilyIndex; uint32_t queueCount; const float *pQueuePriorities;
} VkDeviceQueueCreateInfo;

typedef struct {
    uint32_t sType; const void *pNext; uint32_t flags;
    uint32_t queueCreateInfoCount; const VkDeviceQueueCreateInfo *pQueueCreateInfos;
    uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames;
    const void *pEnabledFeatures;
} VkDeviceCreateInfo;

typedef struct {
    uint32_t minImageCount; uint32_t maxImageCount;
    VkExtent2D currentExtent; VkExtent2D minImageExtent; VkExtent2D maxImageExtent;
    uint32_t maxImageArrayLayers; uint32_t supportedTransforms;
    uint32_t currentTransform; uint32_t supportedCompositeAlpha;
    uint32_t supportedUsageFlags;
} VkSurfaceCapabilitiesKHR;

/* ===== Minimal DRM types ===== */
#define DRM_IOCTL_BASE 'd'
#define DRM_IOCTL_VERSION          0xC0406400  /* approximate */
#define DRM_IOCTL_MODE_GETRESOURCES 0xC04064A0

struct drm_mode_card_res {
    uint64_t fb_id_ptr;
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t min_width, max_width;
    uint32_t min_height, max_height;
};

struct drm_mode_get_connector {
    uint64_t encoders_ptr;
    uint64_t modes_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_modes;
    uint32_t count_props;
    uint32_t count_encoders;
    uint32_t encoder_id;
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t connection;
    uint32_t mm_width, mm_height;
    uint32_t subpixel;
    uint32_t pad;
};

struct drm_mode_create_dumb {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
};

struct drm_mode_map_dumb {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
};

struct drm_mode_destroy_dumb {
    uint32_t handle;
};

#define DRM_IOCTL_MODE_CREATE_DUMB  _IOWR('d', 0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB     _IOWR('d', 0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB _IOWR('d', 0xB4, struct drm_mode_destroy_dumb)

/* ===== Minimal EGL types ===== */
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean;
typedef void *EGLNativeDisplayType;
typedef void *EGLClientBuffer;
typedef void *EGLImageKHR;

#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_DEFAULT_DISPLAY ((EGLNativeDisplayType)0)
#define EGL_NONE 0x3038
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3025
#define EGL_BLUE_SIZE 0x3026
#define EGL_ALPHA_SIZE 0x3027
#define EGL_DEPTH_SIZE 0x3028
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_OPENGL_ES3_BIT_KHR 0x0040
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3058
#define EGL_TRUE 1
#define EGL_FALSE 0

#define EGL_PLATFORM_GBM_KHR 0x31D7

/* ===== GBM types ===== */
typedef struct gbm_device *GBMDevice;
typedef struct gbm_surface *GBMSurface;
typedef struct gbm_bo *GBMBo;

#define GBM_FORMAT_XRGB8888 0x34325258
#define GBM_FORMAT_ARGB8888 0x34325241
#define GBM_BO_USE_SCANOUT  (1 << 0)
#define GBM_BO_USE_RENDERING (1 << 2)

/* ===== Helpers ===== */
static int g_pass = 0, g_fail = 0, g_skip = 0;

#define TEST_SECTION(name) \
    fprintf(stderr, "\n========== %s ==========\n", name); fflush(stderr)

#define RESULT_PASS(fmt, ...) do { \
    fprintf(stderr, "  [PASS] " fmt "\n", ##__VA_ARGS__); fflush(stderr); g_pass++; \
} while(0)

#define RESULT_FAIL(fmt, ...) do { \
    fprintf(stderr, "  [FAIL] " fmt "\n", ##__VA_ARGS__); fflush(stderr); g_fail++; \
} while(0)

#define RESULT_SKIP(fmt, ...) do { \
    fprintf(stderr, "  [SKIP] " fmt "\n", ##__VA_ARGS__); fflush(stderr); g_skip++; \
} while(0)

#define RESULT_INFO(fmt, ...) do { \
    fprintf(stderr, "  [INFO] " fmt "\n", ##__VA_ARGS__); fflush(stderr); \
} while(0)

/* ===== Function pointer typedefs ===== */
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(VkInstance, const char*);
typedef VkResult (*PFN_vkCreateInstance)(const VkInstanceCreateInfo*, const void*, VkInstance*);
typedef void (*PFN_vkDestroyInstance)(VkInstance, const void*);
typedef VkResult (*PFN_vkEnumeratePhysicalDevices)(VkInstance, uint32_t*, VkPhysicalDevice*);
typedef void (*PFN_vkGetPhysicalDeviceProperties)(VkPhysicalDevice, VkPhysicalDeviceProperties*);
typedef void (*PFN_vkGetPhysicalDeviceMemoryProperties)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*);
typedef void (*PFN_vkGetPhysicalDeviceQueueFamilyProperties)(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties*);
typedef VkResult (*PFN_vkEnumerateDeviceExtensionProperties)(VkPhysicalDevice, const char*, uint32_t*, VkExtensionProperties*);
typedef VkResult (*PFN_vkEnumerateInstanceExtensionProperties)(const char*, uint32_t*, VkExtensionProperties*);
typedef VkResult (*PFN_vkCreateDevice)(VkPhysicalDevice, const VkDeviceCreateInfo*, const void*, VkDevice*);
typedef void (*PFN_vkDestroyDevice)(VkDevice, const void*);
typedef void (*PFN_vkGetDeviceQueue)(VkDevice, uint32_t, uint32_t, VkQueue*);
typedef VkResult (*PFN_vkCreateBuffer)(VkDevice, const void*, const void*, VkBuffer*);
typedef void (*PFN_vkDestroyBuffer)(VkDevice, VkBuffer, const void*);
typedef void (*PFN_vkGetBufferMemoryRequirements)(VkDevice, VkBuffer, void*);
typedef VkResult (*PFN_vkAllocateMemory)(VkDevice, const void*, const void*, VkDeviceMemory*);
typedef void (*PFN_vkFreeMemory)(VkDevice, VkDeviceMemory, const void*);
typedef VkResult (*PFN_vkBindBufferMemory)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
typedef VkResult (*PFN_vkMapMemory)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, uint32_t, void**);
typedef void (*PFN_vkUnmapMemory)(VkDevice, VkDeviceMemory);
typedef VkResult (*PFN_vkCreateCommandPool)(VkDevice, const void*, const void*, VkCommandPool*);
typedef void (*PFN_vkDestroyCommandPool)(VkDevice, VkCommandPool, const void*);
typedef VkResult (*PFN_vkQueueWaitIdle)(VkQueue);

/* VK_KHR_display */
typedef VkResult (*PFN_vkGetPhysicalDeviceDisplayPropertiesKHR)(VkPhysicalDevice, uint32_t*, VkDisplayPropertiesKHR*);
typedef VkResult (*PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR)(VkPhysicalDevice, uint32_t*, VkDisplayPlanePropertiesKHR*);

/* VK_EXT_headless_surface */
typedef VkResult (*PFN_vkCreateHeadlessSurfaceEXT)(VkInstance, const VkHeadlessSurfaceCreateInfoEXT*, const void*, VkSurfaceKHR*);

/* VK_KHR_surface */
typedef void (*PFN_vkDestroySurfaceKHR)(VkInstance, VkSurfaceKHR, const void*);
typedef VkResult (*PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR*);
typedef VkResult (*PFN_vkGetPhysicalDeviceSurfaceSupportKHR)(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32*);

/* EGL */
typedef EGLDisplay (*PFN_eglGetDisplay)(EGLNativeDisplayType);
typedef EGLDisplay (*PFN_eglGetPlatformDisplay)(unsigned int, void*, const EGLint*);
typedef EGLBoolean (*PFN_eglInitialize)(EGLDisplay, EGLint*, EGLint*);
typedef EGLBoolean (*PFN_eglTerminate)(EGLDisplay);
typedef EGLBoolean (*PFN_eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
typedef EGLContext (*PFN_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
typedef void (*PFN_eglDestroyContext)(EGLDisplay, EGLContext);
typedef EGLSurface (*PFN_eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint*);
typedef EGLSurface (*PFN_eglCreateWindowSurface)(EGLDisplay, EGLConfig, void*, const EGLint*);
typedef void (*PFN_eglDestroySurface)(EGLDisplay, EGLSurface);
typedef EGLBoolean (*PFN_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLBoolean (*PFN_eglSwapBuffers)(EGLDisplay, EGLSurface);
typedef const char* (*PFN_eglQueryString)(EGLDisplay, EGLint);
typedef EGLBoolean (*PFN_eglBindAPI)(unsigned int);
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define EGL_EXTENSIONS 0x3055
#define EGL_CLIENT_APIS 0x308D

/* GBM */
typedef GBMDevice (*PFN_gbm_create_device)(int);
typedef void (*PFN_gbm_device_destroy)(GBMDevice);
typedef GBMSurface (*PFN_gbm_surface_create)(GBMDevice, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void (*PFN_gbm_surface_destroy)(GBMSurface);
typedef GBMBo (*PFN_gbm_surface_lock_front_buffer)(GBMSurface);
typedef void (*PFN_gbm_surface_release_buffer)(GBMSurface, GBMBo);
typedef uint32_t (*PFN_gbm_bo_get_handle_u32)(GBMBo);
typedef uint32_t (*PFN_gbm_bo_get_stride)(GBMBo);
typedef GBMBo (*PFN_gbm_bo_create)(GBMDevice, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void (*PFN_gbm_bo_destroy)(GBMBo);

/* GL */
typedef const unsigned char* (*PFN_glGetString)(unsigned int);
typedef void (*PFN_glClearColor)(float, float, float, float);
typedef void (*PFN_glClear)(unsigned int);
typedef unsigned int (*PFN_glGetError)(void);
#define GL_VENDOR    0x1F00
#define GL_RENDERER  0x1F01
#define GL_VERSION   0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_COLOR_BUFFER_BIT 0x00004000


/* ===== Test 1: Vulkan basics (no WSI) ===== */
static void test_vulkan_compute(void) {
    TEST_SECTION("VULKAN COMPUTE (no WSI extensions)");

    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW);
    if (!lib) lib = dlopen("libmali.so", RTLD_NOW);
    if (!lib) { RESULT_FAIL("Cannot load Vulkan library"); return; }
    RESULT_PASS("Vulkan library loaded");

    PFN_vkGetInstanceProcAddr getProc = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!getProc) { RESULT_FAIL("vkGetInstanceProcAddr not found"); dlclose(lib); return; }

    /* Create instance with NO extensions */
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)getProc(NULL, "vkCreateInstance");
    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    VkInstance inst = NULL;
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    if (r != VK_SUCCESS) { RESULT_FAIL("vkCreateInstance (no ext): %d", r); dlclose(lib); return; }
    RESULT_PASS("vkCreateInstance (no extensions): OK");

    PFN_vkEnumeratePhysicalDevices vkEnumDevs = (PFN_vkEnumeratePhysicalDevices)getProc(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties vkGetProps = (PFN_vkGetPhysicalDeviceProperties)getProc(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetQFP = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)getProc(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)getProc(inst, "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkEnumerateDeviceExtensionProperties vkEnumDevExt = (PFN_vkEnumerateDeviceExtensionProperties)getProc(inst, "vkEnumerateDeviceExtensionProperties");
    PFN_vkCreateDevice vkCreateDevice = (PFN_vkCreateDevice)getProc(inst, "vkCreateDevice");
    PFN_vkDestroyDevice vkDestroyDevice = (PFN_vkDestroyDevice)getProc(inst, "vkDestroyDevice");
    PFN_vkGetDeviceQueue vkGetDeviceQueue = (PFN_vkGetDeviceQueue)getProc(inst, "vkGetDeviceQueue");
    PFN_vkCreateBuffer vkCreateBuffer = (PFN_vkCreateBuffer)getProc(inst, "vkCreateBuffer");
    PFN_vkDestroyBuffer vkDestroyBuffer = (PFN_vkDestroyBuffer)getProc(inst, "vkDestroyBuffer");
    PFN_vkGetBufferMemoryRequirements vkGetBufMemReq = (PFN_vkGetBufferMemoryRequirements)getProc(inst, "vkGetBufferMemoryRequirements");
    PFN_vkAllocateMemory vkAllocMemory = (PFN_vkAllocateMemory)getProc(inst, "vkAllocateMemory");
    PFN_vkFreeMemory vkFreeMemory = (PFN_vkFreeMemory)getProc(inst, "vkFreeMemory");
    PFN_vkBindBufferMemory vkBindBufMem = (PFN_vkBindBufferMemory)getProc(inst, "vkBindBufferMemory");
    PFN_vkMapMemory vkMapMemory = (PFN_vkMapMemory)getProc(inst, "vkMapMemory");
    PFN_vkUnmapMemory vkUnmapMemory = (PFN_vkUnmapMemory)getProc(inst, "vkUnmapMemory");
    PFN_vkCreateCommandPool vkCreateCmdPool = (PFN_vkCreateCommandPool)getProc(inst, "vkCreateCommandPool");
    PFN_vkDestroyCommandPool vkDestroyCmdPool = (PFN_vkDestroyCommandPool)getProc(inst, "vkDestroyCommandPool");
    PFN_vkQueueWaitIdle vkQueueWaitIdle = (PFN_vkQueueWaitIdle)getProc(inst, "vkQueueWaitIdle");

    uint32_t devCount = 0;
    vkEnumDevs(inst, &devCount, NULL);
    if (devCount == 0) { RESULT_FAIL("No physical devices"); goto cleanup_inst; }

    VkPhysicalDevice phys[4];
    vkEnumDevs(inst, &devCount, phys);

    VkPhysicalDeviceProperties props;
    vkGetProps(phys[0], &props);
    RESULT_INFO("GPU: %s (Vulkan %u.%u.%u)", props.deviceName,
        (props.apiVersion >> 22) & 0x3ff, (props.apiVersion >> 12) & 0x3ff, props.apiVersion & 0xfff);

    /* Queue families */
    uint32_t qfCount = 0;
    vkGetQFP(phys[0], &qfCount, NULL);
    VkQueueFamilyProperties qfProps[16];
    vkGetQFP(phys[0], &qfCount, qfProps);
    int computeQF = -1, graphicsQF = -1;
    for (uint32_t i = 0; i < qfCount; i++) {
        RESULT_INFO("Queue family %u: flags=0x%x count=%u", i, qfProps[i].queueFlags, qfProps[i].queueCount);
        if ((qfProps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && computeQF < 0) computeQF = i;
        if ((qfProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && graphicsQF < 0) graphicsQF = i;
    }
    if (computeQF >= 0) RESULT_PASS("Compute queue: family %d", computeQF);
    else RESULT_FAIL("No compute queue found");
    if (graphicsQF >= 0) RESULT_PASS("Graphics queue: family %d", graphicsQF);
    else RESULT_FAIL("No graphics queue found");

    /* Memory properties */
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetMemProps(phys[0], &memProps);
    int hostVisibleType = -1;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memProps.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            hostVisibleType = i;
            break;
        }
    }
    if (hostVisibleType >= 0) RESULT_PASS("Host-visible coherent memory: type %d", hostVisibleType);
    else RESULT_FAIL("No host-visible coherent memory");

    /* Device extensions */
    uint32_t extCount = 0;
    vkEnumDevExt(phys[0], NULL, &extCount, NULL);
    VkExtensionProperties *exts = calloc(extCount, sizeof(VkExtensionProperties));
    vkEnumDevExt(phys[0], NULL, &extCount, exts);

    const char *interesting[] = {
        "VK_KHR_swapchain",
        "VK_KHR_external_memory",
        "VK_KHR_external_memory_fd",
        "VK_EXT_external_memory_dma_buf",
        "VK_EXT_image_drm_format_modifier",
        "VK_KHR_external_fence",
        "VK_KHR_external_fence_fd",
        "VK_KHR_external_semaphore",
        "VK_KHR_external_semaphore_fd",
        "VK_KHR_maintenance1",
        "VK_KHR_push_descriptor",
        "VK_KHR_descriptor_update_template",
        "VK_EXT_external_memory_host",
        NULL
    };
    for (int j = 0; interesting[j]; j++) {
        int found = 0;
        for (uint32_t i = 0; i < extCount; i++) {
            if (strcmp(exts[i].extensionName, interesting[j]) == 0) { found = 1; break; }
        }
        if (found) RESULT_PASS("Device ext: %s", interesting[j]);
        else RESULT_INFO("Device ext: %s NOT available", interesting[j]);
    }
    RESULT_INFO("Total device extensions: %u", extCount);
    free(exts);

    /* Create logical device (compute only, no WSI) */
    float priority = 1.0f;
    int useQF = (computeQF >= 0) ? computeQF : graphicsQF;
    if (useQF < 0) { RESULT_FAIL("No usable queue family"); goto cleanup_inst; }

    VkDeviceQueueCreateInfo dqci = {0};
    dqci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    dqci.queueFamilyIndex = useQF;
    dqci.queueCount = 1;
    dqci.pQueuePriorities = &priority;

    VkDeviceCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dqci;

    VkDevice dev = NULL;
    r = vkCreateDevice(phys[0], &dci, NULL, &dev);
    if (r != VK_SUCCESS) { RESULT_FAIL("vkCreateDevice (no WSI): %d", r); goto cleanup_inst; }
    RESULT_PASS("vkCreateDevice (compute-only, no WSI): OK");

    VkQueue queue;
    vkGetDeviceQueue(dev, useQF, 0, &queue);
    RESULT_PASS("vkGetDeviceQueue: OK");

    /* Test buffer creation + host-visible memory mapping */
    if (hostVisibleType >= 0) {
        struct { uint32_t sType; void *pNext; uint32_t flags; VkDeviceSize size; uint32_t usage; uint32_t sharingMode; uint32_t qfic; uint32_t *pqfi; } bufCI = {0};
        bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufCI.size = 4096;
        bufCI.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VkBuffer buf = NULL;
        r = vkCreateBuffer(dev, &bufCI, NULL, &buf);
        if (r == VK_SUCCESS) {
            struct { VkDeviceSize size; VkDeviceSize alignment; uint32_t memoryTypeBits; } memReq;
            vkGetBufMemReq(dev, buf, &memReq);

            struct { uint32_t sType; void *pNext; VkDeviceSize allocationSize; uint32_t memoryTypeIndex; } allocInfo = {0};
            allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize = memReq.size;
            allocInfo.memoryTypeIndex = hostVisibleType;
            VkDeviceMemory mem = NULL;
            r = vkAllocMemory(dev, &allocInfo, NULL, &mem);
            if (r == VK_SUCCESS) {
                vkBindBufMem(dev, buf, mem, 0);
                void *mapped = NULL;
                r = vkMapMemory(dev, mem, 0, 4096, 0, &mapped);
                if (r == VK_SUCCESS && mapped) {
                    memset(mapped, 0xAB, 4096);
                    vkUnmapMemory(dev, mem);
                    RESULT_PASS("Buffer create + map + write: OK (GPU readback viable)");
                } else {
                    RESULT_FAIL("vkMapMemory: %d", r);
                }
                vkFreeMemory(dev, mem, NULL);
            } else {
                RESULT_FAIL("vkAllocateMemory: %d", r);
            }
            vkDestroyBuffer(dev, buf, NULL);
        } else {
            RESULT_FAIL("vkCreateBuffer: %d", r);
        }
    }

    /* Command pool test */
    struct { uint32_t sType; void *pNext; uint32_t flags; uint32_t queueFamilyIndex; } cpCI = {0};
    cpCI.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpCI.queueFamilyIndex = useQF;
    VkCommandPool cmdPool = NULL;
    r = vkCreateCmdPool(dev, &cpCI, NULL, &cmdPool);
    if (r == VK_SUCCESS) {
        RESULT_PASS("vkCreateCommandPool: OK");
        vkDestroyCmdPool(dev, cmdPool, NULL);
    } else {
        RESULT_FAIL("vkCreateCommandPool: %d", r);
    }

    vkDestroyDevice(dev, NULL);

cleanup_inst:;
    PFN_vkDestroyInstance vkDestroyInstance = (PFN_vkDestroyInstance)getProc(inst, "vkDestroyInstance");
    vkDestroyInstance(inst, NULL);
    dlclose(lib);
}


/* ===== Test 2: VK_KHR_display (expected to crash on Mali) ===== */
static void test_vk_khr_display(void) {
    TEST_SECTION("VK_KHR_display (display enumeration)");

    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW);
    if (!lib) lib = dlopen("libmali.so", RTLD_NOW);
    if (!lib) { RESULT_FAIL("Cannot load Vulkan"); return; }

    PFN_vkGetInstanceProcAddr getProc = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)getProc(NULL, "vkCreateInstance");

    const char *exts[] = { "VK_KHR_surface", "VK_KHR_display" };
    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = exts;

    VkInstance inst = NULL;
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    if (r != VK_SUCCESS) { RESULT_FAIL("vkCreateInstance with VK_KHR_display: %d", r); dlclose(lib); return; }
    RESULT_PASS("vkCreateInstance with VK_KHR_display: OK");

    PFN_vkEnumeratePhysicalDevices vkEnumDevs = (PFN_vkEnumeratePhysicalDevices)getProc(inst, "vkEnumeratePhysicalDevices");
    uint32_t devCount = 0;
    vkEnumDevs(inst, &devCount, NULL);
    VkPhysicalDevice phys[4];
    vkEnumDevs(inst, &devCount, phys);

    PFN_vkGetPhysicalDeviceDisplayPropertiesKHR getDisplayProps =
        (PFN_vkGetPhysicalDeviceDisplayPropertiesKHR)getProc(inst, "vkGetPhysicalDeviceDisplayPropertiesKHR");
    PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR getPlaneProps =
        (PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR)getProc(inst, "vkGetPhysicalDeviceDisplayPlanePropertiesKHR");

    if (getDisplayProps) {
        RESULT_INFO("vkGetPhysicalDeviceDisplayPropertiesKHR: %p (calling...)", (void*)getDisplayProps);
        fflush(stderr);
        uint32_t displayCount = 0;
        r = getDisplayProps(phys[0], &displayCount, NULL);
        RESULT_PASS("vkGetPhysicalDeviceDisplayPropertiesKHR: result=%d count=%u", r, displayCount);

        if (displayCount > 0 && displayCount < 100) {
            VkDisplayPropertiesKHR displays[8];
            uint32_t n = displayCount < 8 ? displayCount : 8;
            getDisplayProps(phys[0], &n, displays);
            for (uint32_t d = 0; d < n; d++) {
                RESULT_INFO("  Display %u: '%s' res=%ux%u", d,
                    displays[d].displayName ? displays[d].displayName : "(null)",
                    displays[d].physicalResolution.width, displays[d].physicalResolution.height);
            }
        }
    } else {
        RESULT_FAIL("vkGetPhysicalDeviceDisplayPropertiesKHR: NULL function pointer");
    }

    if (getPlaneProps) {
        RESULT_INFO("vkGetPhysicalDeviceDisplayPlanePropertiesKHR: calling...");
        fflush(stderr);
        uint32_t planeCount = 0;
        r = getPlaneProps(phys[0], &planeCount, NULL);
        RESULT_PASS("vkGetPhysicalDeviceDisplayPlanePropertiesKHR: result=%d count=%u", r, planeCount);
    }

    PFN_vkDestroyInstance vkDestroyInstance = (PFN_vkDestroyInstance)getProc(inst, "vkDestroyInstance");
    vkDestroyInstance(inst, NULL);
    dlclose(lib);
}


/* ===== Test 3: VK_EXT_headless_surface ===== */
static int g_skip_headless_caps = 0;
static void test_vk_headless_surface(void) {
    TEST_SECTION("VK_EXT_headless_surface");

    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW);
    if (!lib) lib = dlopen("libmali.so", RTLD_NOW);
    if (!lib) { RESULT_FAIL("Cannot load Vulkan"); return; }

    PFN_vkGetInstanceProcAddr getProc = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)getProc(NULL, "vkCreateInstance");

    /* Check instance extension availability */
    PFN_vkEnumerateInstanceExtensionProperties vkEnumInstExt =
        (PFN_vkEnumerateInstanceExtensionProperties)getProc(NULL, "vkEnumerateInstanceExtensionProperties");
    uint32_t iextCount = 0;
    vkEnumInstExt(NULL, &iextCount, NULL);
    VkExtensionProperties *iexts = calloc(iextCount, sizeof(VkExtensionProperties));
    vkEnumInstExt(NULL, &iextCount, iexts);

    int hasHeadless = 0, hasSurface = 0;
    RESULT_INFO("Instance extensions (%u):", iextCount);
    for (uint32_t i = 0; i < iextCount; i++) {
        RESULT_INFO("  %s (v%u)", iexts[i].extensionName, iexts[i].specVersion);
        if (strcmp(iexts[i].extensionName, "VK_EXT_headless_surface") == 0) hasHeadless = 1;
        if (strcmp(iexts[i].extensionName, "VK_KHR_surface") == 0) hasSurface = 1;
    }
    free(iexts);

    if (!hasHeadless) { RESULT_FAIL("VK_EXT_headless_surface not available"); dlclose(lib); return; }
    RESULT_PASS("VK_EXT_headless_surface available");

    const char *exts[] = { "VK_KHR_surface", "VK_EXT_headless_surface" };
    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = exts;

    VkInstance inst = NULL;
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    if (r != VK_SUCCESS) { RESULT_FAIL("vkCreateInstance with headless: %d", r); dlclose(lib); return; }
    RESULT_PASS("vkCreateInstance with VK_EXT_headless_surface: OK");

    PFN_vkCreateHeadlessSurfaceEXT vkCreateHeadless =
        (PFN_vkCreateHeadlessSurfaceEXT)getProc(inst, "vkCreateHeadlessSurfaceEXT");
    if (!vkCreateHeadless) { RESULT_FAIL("vkCreateHeadlessSurfaceEXT: NULL"); goto cleanup; }

    VkHeadlessSurfaceCreateInfoEXT hsci = {0};
    hsci.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;

    VkSurfaceKHR surface = NULL;
    r = vkCreateHeadless(inst, &hsci, NULL, &surface);
    if (r != VK_SUCCESS || !surface) { RESULT_FAIL("vkCreateHeadlessSurfaceEXT: %d", r); goto cleanup; }
    RESULT_PASS("Headless surface created: %p", (void*)surface);

    /* Check if swapchain can be created on headless surface */
    PFN_vkEnumeratePhysicalDevices vkEnumDevs = (PFN_vkEnumeratePhysicalDevices)getProc(inst, "vkEnumeratePhysicalDevices");
    uint32_t devCount = 0;
    vkEnumDevs(inst, &devCount, NULL);
    VkPhysicalDevice phys[4];
    vkEnumDevs(inst, &devCount, phys);

    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetSurfCaps =
        (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)getProc(inst, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR vkGetSurfSupport =
        (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)getProc(inst, "vkGetPhysicalDeviceSurfaceSupportKHR");

    if (vkGetSurfSupport && devCount > 0) {
        /* Check all queue families for presentation support */
        PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetQFP =
            (PFN_vkGetPhysicalDeviceQueueFamilyProperties)getProc(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
        uint32_t qfCount = 0;
        vkGetQFP(phys[0], &qfCount, NULL);
        for (uint32_t i = 0; i < qfCount; i++) {
            VkBool32 supported = 0;
            r = vkGetSurfSupport(phys[0], i, surface, &supported);
            RESULT_INFO("Queue family %u presentation support: %s (result=%d)", i,
                supported ? "YES" : "NO", r);
        }
    }

    if (g_skip_headless_caps) {
        RESULT_SKIP("vkGetPhysicalDeviceSurfaceCapabilitiesKHR (--skip-headless-caps)");
    } else if (vkGetSurfCaps && devCount > 0) {
        RESULT_INFO("Calling vkGetPhysicalDeviceSurfaceCapabilitiesKHR (may crash on Mali)...");
        fflush(stderr);
        VkSurfaceCapabilitiesKHR caps = {0};
        r = vkGetSurfCaps(phys[0], surface, &caps);
        if (r == VK_SUCCESS) {
            RESULT_PASS("Headless surface caps: images=%u-%u, extent=%ux%u, usage=0x%x",
                caps.minImageCount, caps.maxImageCount,
                caps.currentExtent.width, caps.currentExtent.height,
                caps.supportedUsageFlags);
        } else {
            RESULT_FAIL("vkGetPhysicalDeviceSurfaceCapabilitiesKHR: %d", r);
        }
    }

    /* Try to create a device with VK_KHR_swapchain for the headless surface */
    if (g_skip_headless_caps) {
        RESULT_SKIP("Swapchain on headless test (depends on surface caps)");
    } else if (devCount > 0) {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo dqci = {0};
        dqci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        dqci.queueFamilyIndex = 0;
        dqci.queueCount = 1;
        dqci.pQueuePriorities = &priority;

        const char *devExts[] = { "VK_KHR_swapchain" };
        VkDeviceCreateInfo dci = {0};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &dqci;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = devExts;

        PFN_vkCreateDevice vkCreateDevice = (PFN_vkCreateDevice)getProc(inst, "vkCreateDevice");
        PFN_vkDestroyDevice vkDestroyDevice = (PFN_vkDestroyDevice)getProc(inst, "vkDestroyDevice");
        VkDevice dev = NULL;
        r = vkCreateDevice(phys[0], &dci, NULL, &dev);
        if (r == VK_SUCCESS) {
            RESULT_PASS("vkCreateDevice with VK_KHR_swapchain on headless: OK");
            vkDestroyDevice(dev, NULL);
        } else {
            RESULT_FAIL("vkCreateDevice with VK_KHR_swapchain on headless: %d", r);
        }
    }

    PFN_vkDestroySurfaceKHR vkDestroySurface = (PFN_vkDestroySurfaceKHR)getProc(inst, "vkDestroySurfaceKHR");
    if (vkDestroySurface) vkDestroySurface(inst, surface, NULL);

cleanup:;
    PFN_vkDestroyInstance vkDestroyInstance = (PFN_vkDestroyInstance)getProc(inst, "vkDestroyInstance");
    vkDestroyInstance(inst, NULL);
    dlclose(lib);
}


/* ===== Test 4: DRM/KMS framebuffer ===== */
static void test_drm_kms(void) {
    TEST_SECTION("DRM/KMS FRAMEBUFFER");

    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) { RESULT_FAIL("Cannot open /dev/dri/card0"); return; }
    RESULT_PASS("Opened /dev/dri/card0");

    /* Try to get DRM resources */
    struct drm_mode_card_res res = {0};
    if (ioctl(fd, 0xC04064A0, &res) == 0) {
        RESULT_PASS("DRM resources: %u connectors, %u CRTCs, %u FBs, %u encoders",
            res.count_connectors, res.count_crtcs, res.count_fbs, res.count_encoders);
    } else {
        RESULT_FAIL("DRM_IOCTL_MODE_GETRESOURCES failed");
    }

    /* Test dumb buffer creation (for software/readback scanout) */
    struct drm_mode_create_dumb create = {0};
    create.width = 640;
    create.height = 480;
    create.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) == 0) {
        RESULT_PASS("DRM dumb buffer 640x480x32: handle=%u pitch=%u size=%llu",
            create.handle, create.pitch, (unsigned long long)create.size);

        /* Try to mmap it */
        struct drm_mode_map_dumb map = {0};
        map.handle = create.handle;
        if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) == 0) {
            void *ptr = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
            if (ptr != MAP_FAILED) {
                memset(ptr, 0xFF, 4096); /* write test */
                RESULT_PASS("DRM dumb buffer mmap: OK (CPU-writable scanout buffer works)");
                munmap(ptr, create.size);
            } else {
                RESULT_FAIL("DRM dumb buffer mmap failed");
            }
        } else {
            RESULT_FAIL("DRM_IOCTL_MODE_MAP_DUMB failed");
        }

        struct drm_mode_destroy_dumb destroy = {0};
        destroy.handle = create.handle;
        ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    } else {
        RESULT_FAIL("DRM dumb buffer creation failed");
    }

    close(fd);
}


/* ===== Test 5: EGL + GBM (accelerated display without Vulkan WSI) ===== */
static void test_egl_gbm(void) {
    TEST_SECTION("EGL + GBM (accelerated display path)");

    /* Load GBM */
    void *gbm_lib = dlopen("libgbm.so.1", RTLD_NOW);
    if (!gbm_lib) gbm_lib = dlopen("libgbm.so", RTLD_NOW);
    if (!gbm_lib) gbm_lib = dlopen("libmali.so", RTLD_NOW);
    if (!gbm_lib) { RESULT_FAIL("Cannot load libgbm"); return; }
    RESULT_PASS("libgbm loaded");

    PFN_gbm_create_device gbm_create = (PFN_gbm_create_device)dlsym(gbm_lib, "gbm_create_device");
    PFN_gbm_device_destroy gbm_destroy = (PFN_gbm_device_destroy)dlsym(gbm_lib, "gbm_device_destroy");
    PFN_gbm_surface_create gbm_surf_create = (PFN_gbm_surface_create)dlsym(gbm_lib, "gbm_surface_create");
    PFN_gbm_surface_destroy gbm_surf_destroy = (PFN_gbm_surface_destroy)dlsym(gbm_lib, "gbm_surface_destroy");
    PFN_gbm_surface_lock_front_buffer gbm_lock = (PFN_gbm_surface_lock_front_buffer)dlsym(gbm_lib, "gbm_surface_lock_front_buffer");
    PFN_gbm_surface_release_buffer gbm_release = (PFN_gbm_surface_release_buffer)dlsym(gbm_lib, "gbm_surface_release_buffer");
    PFN_gbm_bo_get_handle_u32 gbm_bo_handle = NULL; /* will try alternate API */
    PFN_gbm_bo_get_stride gbm_bo_stride = (PFN_gbm_bo_get_stride)dlsym(gbm_lib, "gbm_bo_get_stride");
    PFN_gbm_bo_create gbm_bo_create_fn = (PFN_gbm_bo_create)dlsym(gbm_lib, "gbm_bo_create");
    PFN_gbm_bo_destroy gbm_bo_destroy_fn = (PFN_gbm_bo_destroy)dlsym(gbm_lib, "gbm_bo_destroy");

    if (!gbm_create) { RESULT_FAIL("gbm_create_device not found"); dlclose(gbm_lib); return; }

    int drm_fd = open("/dev/dri/card0", O_RDWR);
    if (drm_fd < 0) { RESULT_FAIL("Cannot open /dev/dri/card0 for GBM"); dlclose(gbm_lib); return; }

    GBMDevice gbm_dev = gbm_create(drm_fd);
    if (!gbm_dev) { RESULT_FAIL("gbm_create_device failed"); close(drm_fd); dlclose(gbm_lib); return; }
    RESULT_PASS("GBM device created");

    /* Test GBM buffer creation */
    if (gbm_bo_create_fn && gbm_bo_destroy_fn) {
        GBMBo bo = gbm_bo_create_fn(gbm_dev, 640, 480, GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        if (bo) {
            RESULT_PASS("GBM BO created (640x480 XRGB8888, scanout+rendering)");
            gbm_bo_destroy_fn(bo);
        } else {
            RESULT_FAIL("GBM BO creation failed");
        }
    }

    /* Test GBM surface */
    if (gbm_surf_create) {
        GBMSurface gbm_surf = gbm_surf_create(gbm_dev, 640, 480, GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        if (gbm_surf) {
            RESULT_PASS("GBM surface created (640x480)");
            gbm_surf_destroy(gbm_surf);
        } else {
            RESULT_FAIL("GBM surface creation failed");
        }
    }

    /* Load EGL */
    void *egl_lib = dlopen("libEGL.so.1", RTLD_NOW);
    if (!egl_lib) egl_lib = dlopen("libEGL.so", RTLD_NOW);
    if (!egl_lib) egl_lib = dlopen("libmali.so", RTLD_NOW);
    if (!egl_lib) { RESULT_FAIL("Cannot load libEGL"); gbm_destroy(gbm_dev); close(drm_fd); dlclose(gbm_lib); return; }
    RESULT_PASS("libEGL loaded");

    PFN_eglGetDisplay eglGetDisplay = (PFN_eglGetDisplay)dlsym(egl_lib, "eglGetDisplay");
    PFN_eglGetPlatformDisplay eglGetPlatformDisplay = (PFN_eglGetPlatformDisplay)dlsym(egl_lib, "eglGetPlatformDisplayEXT");
    if (!eglGetPlatformDisplay) eglGetPlatformDisplay = (PFN_eglGetPlatformDisplay)dlsym(egl_lib, "eglGetPlatformDisplay");
    PFN_eglInitialize eglInitialize = (PFN_eglInitialize)dlsym(egl_lib, "eglInitialize");
    PFN_eglTerminate eglTerminate = (PFN_eglTerminate)dlsym(egl_lib, "eglTerminate");
    PFN_eglChooseConfig eglChooseConfig = (PFN_eglChooseConfig)dlsym(egl_lib, "eglChooseConfig");
    PFN_eglCreateContext eglCreateContext = (PFN_eglCreateContext)dlsym(egl_lib, "eglCreateContext");
    PFN_eglDestroyContext eglDestroyContext = (PFN_eglDestroyContext)dlsym(egl_lib, "eglDestroyContext");
    PFN_eglCreateWindowSurface eglCreateWindowSurface = (PFN_eglCreateWindowSurface)dlsym(egl_lib, "eglCreateWindowSurface");
    PFN_eglCreatePbufferSurface eglCreatePbufferSurface = (PFN_eglCreatePbufferSurface)dlsym(egl_lib, "eglCreatePbufferSurface");
    PFN_eglDestroySurface eglDestroySurface = (PFN_eglDestroySurface)dlsym(egl_lib, "eglDestroySurface");
    PFN_eglMakeCurrent eglMakeCurrent = (PFN_eglMakeCurrent)dlsym(egl_lib, "eglMakeCurrent");
    PFN_eglSwapBuffers eglSwapBuffers = (PFN_eglSwapBuffers)dlsym(egl_lib, "eglSwapBuffers");
    PFN_eglQueryString eglQueryString = (PFN_eglQueryString)dlsym(egl_lib, "eglQueryString");
    PFN_eglBindAPI eglBindAPI = (PFN_eglBindAPI)dlsym(egl_lib, "eglBindAPI");

    /* Try EGL with GBM platform */
    EGLDisplay egl_dpy = EGL_NO_DISPLAY;
    if (eglGetPlatformDisplay) {
        egl_dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm_dev, NULL);
        if (egl_dpy != EGL_NO_DISPLAY)
            RESULT_PASS("eglGetPlatformDisplay(GBM): OK");
        else
            RESULT_INFO("eglGetPlatformDisplay(GBM): failed, trying eglGetDisplay");
    }
    if (egl_dpy == EGL_NO_DISPLAY && eglGetDisplay) {
        egl_dpy = eglGetDisplay((EGLNativeDisplayType)gbm_dev);
        if (egl_dpy != EGL_NO_DISPLAY)
            RESULT_PASS("eglGetDisplay(gbm_dev): OK");
    }
    if (egl_dpy == EGL_NO_DISPLAY) {
        RESULT_FAIL("Cannot get EGL display"); goto cleanup_gbm;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(egl_dpy, &major, &minor)) {
        RESULT_FAIL("eglInitialize failed"); goto cleanup_gbm;
    }
    RESULT_PASS("EGL initialized: %d.%d", major, minor);

    if (eglQueryString) {
        const char *vendor = eglQueryString(egl_dpy, EGL_VENDOR);
        const char *version = eglQueryString(egl_dpy, EGL_VERSION);
        const char *apis = eglQueryString(egl_dpy, EGL_CLIENT_APIS);
        const char *extensions = eglQueryString(egl_dpy, EGL_EXTENSIONS);
        RESULT_INFO("EGL Vendor: %s", vendor ? vendor : "(null)");
        RESULT_INFO("EGL Version: %s", version ? version : "(null)");
        RESULT_INFO("EGL Client APIs: %s", apis ? apis : "(null)");
        /* Print interesting EGL extensions */
        if (extensions) {
            if (strstr(extensions, "EGL_KHR_image_base")) RESULT_PASS("EGL ext: EGL_KHR_image_base");
            if (strstr(extensions, "EGL_EXT_image_dma_buf_import")) RESULT_PASS("EGL ext: EGL_EXT_image_dma_buf_import");
            if (strstr(extensions, "EGL_KHR_gl_renderbuffer_image")) RESULT_PASS("EGL ext: EGL_KHR_gl_renderbuffer_image");
            if (strstr(extensions, "EGL_KHR_fence_sync")) RESULT_PASS("EGL ext: EGL_KHR_fence_sync");
            if (strstr(extensions, "EGL_ANDROID_native_fence_sync")) RESULT_PASS("EGL ext: EGL_ANDROID_native_fence_sync");
            if (strstr(extensions, "EGL_KHR_surfaceless_context")) RESULT_PASS("EGL ext: EGL_KHR_surfaceless_context");
            if (strstr(extensions, "EGL_KHR_platform_gbm")) RESULT_PASS("EGL ext: EGL_KHR_platform_gbm");
        }
    }

    /* Bind OpenGL ES API */
    if (eglBindAPI) eglBindAPI(EGL_OPENGL_ES_API);

    /* Choose config for GLES 3.x */
    EGLint config_attribs[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,
        EGL_DEPTH_SIZE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_NONE
    };
    EGLConfig config;
    EGLint num_configs = 0;
    int gles_version = 3;
    if (!eglChooseConfig(egl_dpy, config_attribs, &config, 1, &num_configs) || num_configs == 0) {
        /* Fallback to ES2 */
        config_attribs[11] = EGL_OPENGL_ES2_BIT;
        gles_version = 2;
        if (!eglChooseConfig(egl_dpy, config_attribs, &config, 1, &num_configs) || num_configs == 0) {
            /* Fallback: no renderable type filter */
            EGLint simple_attribs[] = {
                EGL_RED_SIZE, 8,
                EGL_GREEN_SIZE, 8,
                EGL_BLUE_SIZE, 8,
                EGL_NONE
            };
            gles_version = 2;
            if (!eglChooseConfig(egl_dpy, simple_attribs, &config, 1, &num_configs) || num_configs == 0) {
                /* Last resort: get any config */
                EGLint any_attribs[] = { EGL_NONE };
                if (!eglChooseConfig(egl_dpy, any_attribs, &config, 1, &num_configs) || num_configs == 0) {
                    RESULT_FAIL("eglChooseConfig: no config found at all (tried all fallbacks)");
                    /* Enumerate all configs for debugging */
                    EGLint total_configs = 0;
                    eglChooseConfig(egl_dpy, any_attribs, NULL, 0, &total_configs);
                    RESULT_INFO("Total EGL configs available: %d", total_configs);
                    goto cleanup_egl;
                }
            }
            RESULT_PASS("EGL config found (minimal attribs, %d configs)", num_configs);
        } else {
            RESULT_PASS("EGL config found (GLES 2.0)");
        }
    } else {
        RESULT_PASS("EGL config found (GLES 3.x)");
    }

    /* Create GLES context */
    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, gles_version, EGL_NONE };
    EGLContext ctx = eglCreateContext(egl_dpy, config, EGL_NO_CONTEXT, ctx_attribs);
    if (ctx == EGL_NO_CONTEXT && gles_version == 3) {
        ctx_attribs[1] = 2;
        gles_version = 2;
        ctx = eglCreateContext(egl_dpy, config, EGL_NO_CONTEXT, ctx_attribs);
    }
    if (ctx == EGL_NO_CONTEXT) {
        RESULT_FAIL("eglCreateContext failed"); goto cleanup_egl;
    }
    RESULT_PASS("EGL context created (GLES %d)", gles_version);

    /* Create GBM surface and EGL window surface */
    GBMSurface gbm_surf2 = NULL;
    EGLSurface egl_surf = EGL_NO_SURFACE;
    if (gbm_surf_create) {
        gbm_surf2 = gbm_surf_create(gbm_dev, 640, 480, GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        if (gbm_surf2) {
            egl_surf = eglCreateWindowSurface(egl_dpy, config, (void*)gbm_surf2, NULL);
            if (egl_surf != EGL_NO_SURFACE) {
                RESULT_PASS("EGL window surface on GBM: OK");
            } else {
                RESULT_FAIL("eglCreateWindowSurface on GBM surface failed");
            }
        }
    }

    /* Try to make current and do a GL call */
    if (egl_surf != EGL_NO_SURFACE) {
        if (eglMakeCurrent(egl_dpy, egl_surf, egl_surf, ctx)) {
            RESULT_PASS("eglMakeCurrent: OK");

            /* Load GL functions */
            void *gl_lib = dlopen("libGLESv2.so.2", RTLD_NOW);
            if (!gl_lib) gl_lib = dlopen("libGLESv2.so", RTLD_NOW);
            if (!gl_lib) gl_lib = dlopen("libmali.so", RTLD_NOW);
            if (gl_lib) {
                PFN_glGetString glGetString = (PFN_glGetString)dlsym(gl_lib, "glGetString");
                PFN_glClearColor glClearColor = (PFN_glClearColor)dlsym(gl_lib, "glClearColor");
                PFN_glClear glClear = (PFN_glClear)dlsym(gl_lib, "glClear");
                PFN_glGetError glGetError = (PFN_glGetError)dlsym(gl_lib, "glGetError");

                if (glGetString) {
                    RESULT_INFO("GL Vendor: %s", glGetString(GL_VENDOR));
                    RESULT_INFO("GL Renderer: %s", glGetString(GL_RENDERER));
                    RESULT_INFO("GL Version: %s", glGetString(GL_VERSION));
                }

                if (glClearColor && glClear && glGetError) {
                    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
                    glClear(GL_COLOR_BUFFER_BIT);
                    unsigned int err = glGetError();
                    if (err == 0) {
                        RESULT_PASS("glClear: OK (GPU rendering works!)");
                    } else {
                        RESULT_FAIL("glClear error: 0x%x", err);
                    }
                }

                /* Test eglSwapBuffers */
                if (eglSwapBuffers(egl_dpy, egl_surf)) {
                    RESULT_PASS("eglSwapBuffers: OK (display pipeline works!)");

                    /* Try to lock the front buffer */
                    if (gbm_lock) {
                        GBMBo bo = gbm_lock(gbm_surf2);
                        if (bo) {
                            RESULT_PASS("gbm_surface_lock_front_buffer: OK (scanout ready)");
                            if (gbm_bo_stride) {
                                uint32_t stride = gbm_bo_stride(bo);
                                RESULT_INFO("  Front buffer stride: %u", stride);
                            }
                            if (gbm_release) gbm_release(gbm_surf2, bo);
                        } else {
                            RESULT_FAIL("gbm_surface_lock_front_buffer failed");
                        }
                    }
                } else {
                    RESULT_FAIL("eglSwapBuffers failed");
                }

                if (gl_lib != egl_lib) dlclose(gl_lib);
            }

            eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        } else {
            RESULT_FAIL("eglMakeCurrent failed");
        }
    } else {
        /* Try pbuffer as fallback */
        EGLint pbuf_attribs[] = { EGL_WIDTH, 640, EGL_HEIGHT, 480, EGL_NONE };
        egl_surf = eglCreatePbufferSurface(egl_dpy, config, pbuf_attribs);
        if (egl_surf != EGL_NO_SURFACE) {
            RESULT_PASS("EGL pbuffer surface: OK (offscreen rendering available)");
            if (eglMakeCurrent(egl_dpy, egl_surf, egl_surf, ctx)) {
                RESULT_PASS("eglMakeCurrent (pbuffer): OK");
                eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            }
        } else {
            RESULT_FAIL("EGL pbuffer surface creation also failed");
        }
    }

    if (egl_surf != EGL_NO_SURFACE) eglDestroySurface(egl_dpy, egl_surf);
    if (gbm_surf2) gbm_surf_destroy(gbm_surf2);
    eglDestroyContext(egl_dpy, ctx);

cleanup_egl:
    eglTerminate(egl_dpy);
cleanup_gbm:
    gbm_destroy(gbm_dev);
    close(drm_fd);
    if (egl_lib != gbm_lib) dlclose(egl_lib);
    dlclose(gbm_lib);
}


/* ===== Test 6: Vulkan external memory (DMA-BUF export for zero-copy display) ===== */
static void test_vk_external_memory(void) {
    TEST_SECTION("VULKAN EXTERNAL MEMORY (DMA-BUF / zero-copy to DRM)");

    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW);
    if (!lib) lib = dlopen("libmali.so", RTLD_NOW);
    if (!lib) { RESULT_SKIP("Cannot load Vulkan"); return; }

    PFN_vkGetInstanceProcAddr getProc = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)getProc(NULL, "vkCreateInstance");

    /* Check for external memory instance extensions */
    PFN_vkEnumerateInstanceExtensionProperties vkEnumInstExt =
        (PFN_vkEnumerateInstanceExtensionProperties)getProc(NULL, "vkEnumerateInstanceExtensionProperties");
    uint32_t iextCount = 0;
    vkEnumInstExt(NULL, &iextCount, NULL);
    VkExtensionProperties *iexts = calloc(iextCount, sizeof(VkExtensionProperties));
    vkEnumInstExt(NULL, &iextCount, iexts);

    int hasExtMemCaps = 0;
    for (uint32_t i = 0; i < iextCount; i++) {
        if (strcmp(iexts[i].extensionName, "VK_KHR_external_memory_capabilities") == 0) hasExtMemCaps = 1;
        if (strcmp(iexts[i].extensionName, "VK_KHR_get_physical_device_properties2") == 0)
            RESULT_PASS("Instance ext: VK_KHR_get_physical_device_properties2");
    }
    free(iexts);

    if (hasExtMemCaps) RESULT_PASS("Instance ext: VK_KHR_external_memory_capabilities");
    else RESULT_INFO("VK_KHR_external_memory_capabilities not available");

    /* Create instance and check device extensions */
    const char *inst_exts[] = { "VK_KHR_external_memory_capabilities", "VK_KHR_get_physical_device_properties2" };
    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    if (hasExtMemCaps) {
        ici.enabledExtensionCount = 2;
        ici.ppEnabledExtensionNames = inst_exts;
    }

    VkInstance inst = NULL;
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    if (r != VK_SUCCESS) { RESULT_FAIL("vkCreateInstance: %d", r); dlclose(lib); return; }

    PFN_vkEnumeratePhysicalDevices vkEnumDevs = (PFN_vkEnumeratePhysicalDevices)getProc(inst, "vkEnumeratePhysicalDevices");
    uint32_t devCount = 0;
    vkEnumDevs(inst, &devCount, NULL);
    VkPhysicalDevice phys[4];
    vkEnumDevs(inst, &devCount, phys);

    if (devCount > 0) {
        PFN_vkEnumerateDeviceExtensionProperties vkEnumDevExt =
            (PFN_vkEnumerateDeviceExtensionProperties)getProc(inst, "vkEnumerateDeviceExtensionProperties");
        uint32_t extCount = 0;
        vkEnumDevExt(phys[0], NULL, &extCount, NULL);
        VkExtensionProperties *exts = calloc(extCount, sizeof(VkExtensionProperties));
        vkEnumDevExt(phys[0], NULL, &extCount, exts);

        const char *wanted[] = {
            "VK_KHR_external_memory",
            "VK_KHR_external_memory_fd",
            "VK_EXT_external_memory_dma_buf",
            "VK_EXT_image_drm_format_modifier",
            "VK_KHR_external_fence",
            "VK_KHR_external_fence_fd",
            "VK_KHR_external_semaphore",
            "VK_KHR_external_semaphore_fd",
            "VK_EXT_external_memory_host",
            "VK_EXT_queue_family_foreign",
            NULL
        };

        int has_ext_mem = 0, has_ext_fd = 0, has_dmabuf = 0, has_drm_fmt = 0;
        for (int j = 0; wanted[j]; j++) {
            int found = 0;
            for (uint32_t i = 0; i < extCount; i++) {
                if (strcmp(exts[i].extensionName, wanted[j]) == 0) { found = 1; break; }
            }
            if (found) {
                RESULT_PASS("Device ext: %s", wanted[j]);
                if (strcmp(wanted[j], "VK_KHR_external_memory") == 0) has_ext_mem = 1;
                if (strcmp(wanted[j], "VK_KHR_external_memory_fd") == 0) has_ext_fd = 1;
                if (strcmp(wanted[j], "VK_EXT_external_memory_dma_buf") == 0) has_dmabuf = 1;
                if (strcmp(wanted[j], "VK_EXT_image_drm_format_modifier") == 0) has_drm_fmt = 1;
            } else {
                RESULT_INFO("Device ext: %s NOT available", wanted[j]);
            }
        }

        if (has_ext_mem && has_ext_fd && has_dmabuf) {
            RESULT_PASS("ZERO-COPY PATH VIABLE: Vulkan -> DMA-BUF -> DRM/KMS scanout");
        } else if (has_ext_mem && has_ext_fd) {
            RESULT_PASS("FD EXPORT PATH VIABLE: Vulkan -> fd -> import elsewhere");
        } else {
            RESULT_INFO("External memory path limited; CPU readback may be needed");
        }

        free(exts);
    }

    PFN_vkDestroyInstance vkDestroyInstance = (PFN_vkDestroyInstance)getProc(inst, "vkDestroyInstance");
    vkDestroyInstance(inst, NULL);
    dlclose(lib);
}


/* ===== Main ===== */
int main(int argc, char **argv) {
    int skip_display_test = 0;
    int skip_headless_caps = 0;

    /* Allow skipping crashy tests */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--skip-display") == 0) skip_display_test = 1;
        if (strcmp(argv[i], "--skip-headless-caps") == 0) skip_headless_caps = 1;
        if (strcmp(argv[i], "--safe") == 0) { skip_display_test = 1; skip_headless_caps = 1; }
        if (strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "Usage: %s [--skip-display] [--skip-headless-caps] [--safe]\n", argv[0]);
            fprintf(stderr, "  --skip-display        Skip VK_KHR_display test (crashes on Mali)\n");
            fprintf(stderr, "  --skip-headless-caps  Skip headless surface caps query (crashes on Mali)\n");
            fprintf(stderr, "  --safe                Skip all tests known to crash on Mali\n");
            return 0;
        }
    }

    fprintf(stderr, "=== GPU Capabilities Probe for tg5050 ===\n");
    fprintf(stderr, "=== Testing all viable rendering + display paths ===\n");
    fflush(stderr);

    /* Safe tests first */
    test_vulkan_compute();
    g_skip_headless_caps = skip_headless_caps;
    test_vk_headless_surface();
    test_vk_external_memory();
    test_drm_kms();
    test_egl_gbm();

    /* Potentially crashy test last */
    if (!skip_display_test) {
        fprintf(stderr, "\n>>> WARNING: VK_KHR_display test may crash on Mali. <<<\n");
        fprintf(stderr, ">>> Use --skip-display to skip. Running in 2 seconds... <<<\n");
        fflush(stderr);
        sleep(2);
        test_vk_khr_display();
    } else {
        fprintf(stderr, "\n========== VK_KHR_display ==========\n");
        fprintf(stderr, "  [SKIP] VK_KHR_display test (--skip-display)\n");
        g_skip++;
    }

    fprintf(stderr, "\n========== SUMMARY ==========\n");
    fprintf(stderr, "  PASS: %d  FAIL: %d  SKIP: %d\n", g_pass, g_fail, g_skip);
    fprintf(stderr, "=============================\n");
    fflush(stderr);

    return (g_fail > 0) ? 1 : 0;
}
