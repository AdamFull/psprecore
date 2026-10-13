// SDL3 window + Vulkan presentation. The PSP frame is uploaded into a small image each frame and blitted
// (scaled, aspect-preserving) into the swapchain. One frame in flight keeps this simple; the GE renderer
// (Phase 5) will replace the CPU upload with GPU-rendered images.

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "ff7cc/platform/display.hpp"

namespace ff7cc::platform {

namespace {

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string("Vulkan: ") + what + " failed (" + std::to_string(r) + ")");
}

}  // namespace

struct Display::Impl {
    SDL_Window* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchain_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    bool swapchain_dirty = false;

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    std::vector<VkSemaphore> rendered;  // one per swapchain image
    VkFence fence = VK_NULL_HANDLE;

    VkImage frame = VK_NULL_HANDLE;
    VmaAllocation frame_mem = VK_NULL_HANDLE;
    int frame_w = 0, frame_h = 0;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation staging_mem = VK_NULL_HANDLE;
    void* staging_ptr = nullptr;

    PadState pad;
    std::uint32_t keyboard_buttons = 0, gamepad_buttons = 0;
    float key_x = 0, key_y = 0;
    SDL_Gamepad* gamepad = nullptr;

    void create_device();
    void create_swapchain();
    void destroy_swapchain();
    void ensure_frame_image(int w, int h);
    void update_pad();
};

void Display::Impl::create_device() {
    check(volkInitialize(), "volkInitialize");
    Uint32 ext_count = 0;
    const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    std::vector<const char*> exts(sdl_exts, sdl_exts + ext_count);
    std::vector<const char*> layers;
    if (std::getenv("FF7CC_VULKAN_VALIDATION")) layers.push_back("VK_LAYER_KHRONOS_validation");

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "FF7CCRECOMP";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<std::uint32_t>(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    ici.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    check(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");
    volkLoadInstance(instance);
    if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface))
        throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError());

    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(instance, &count, gpus.data());
    int best_score = -1;
    for (VkPhysicalDevice g : gpus) {
        std::uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qcount, qprops.data());
        for (std::uint32_t i = 0; i < qcount; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(g, i, surface, &present);
            if (!(qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(g, &props);
            const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > best_score) {
                best_score = score;
                gpu = g;
                queue_family = i;
            }
            break;
        }
    }
    if (!gpu) throw std::runtime_error("no Vulkan device can present to the window");
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    std::fprintf(stderr, "Vulkan device: %s\n", props.deviceName);

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    const char* device_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = device_exts;
    check(vkCreateDevice(gpu, &dci, nullptr, &device), "vkCreateDevice");
    volkLoadDevice(device);
    vkGetDeviceQueue(device, queue_family, 0, &queue);

    VmaVulkanFunctions fns{};
    fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo aci{};
    aci.physicalDevice = gpu;
    aci.device = device;
    aci.instance = instance;
    aci.vulkanApiVersion = VK_API_VERSION_1_1;
    aci.pVulkanFunctions = &fns;
    check(vmaCreateAllocator(&aci, &allocator), "vmaCreateAllocator");

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queue_family;
    check(vkCreateCommandPool(device, &pci, nullptr, &pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(device, &cai, &cmd), "vkAllocateCommandBuffers");
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    check(vkCreateSemaphore(device, &sci, nullptr, &acquired), "vkCreateSemaphore");
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    check(vkCreateFence(device, &fci, nullptr, &fence), "vkCreateFence");
}

void Display::Impl::create_swapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps);
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    extent = caps.currentExtent.width != 0xFFFFFFFF
                 ? caps.currentExtent
                 : VkExtent2D{std::clamp<std::uint32_t>(static_cast<std::uint32_t>(w), caps.minImageExtent.width, caps.maxImageExtent.width),
                              std::clamp<std::uint32_t>(static_cast<std::uint32_t>(h), caps.minImageExtent.height, caps.maxImageExtent.height)};
    if (extent.width == 0 || extent.height == 0) return;  // minimised

    std::uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, formats.data());
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
            chosen = f;
            break;
        }
    swapchain_format = chosen.format;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface;
    sci.minImageCount = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    check(vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain), "vkCreateSwapchainKHR");
    vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
    images.resize(count);
    vkGetSwapchainImagesKHR(device, swapchain, &count, images.data());
    VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    rendered.resize(count);
    for (auto& s : rendered) check(vkCreateSemaphore(device, &semci, nullptr, &s), "vkCreateSemaphore");
    swapchain_dirty = false;
}

void Display::Impl::destroy_swapchain() {
    if (!device) return;
    vkDeviceWaitIdle(device);
    for (auto s : rendered) vkDestroySemaphore(device, s, nullptr);
    rendered.clear();
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
    images.clear();
}

void Display::Impl::ensure_frame_image(int w, int h) {
    if (frame && frame_w == w && frame_h == h) return;
    vkDeviceWaitIdle(device);
    if (frame) vmaDestroyImage(allocator, frame, frame_mem);
    if (staging) vmaDestroyBuffer(allocator, staging, staging_mem);
    frame_w = w;
    frame_h = h;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    check(vmaCreateImage(allocator, &ici, &aci, &frame, &frame_mem, nullptr), "frame image");

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = static_cast<VkDeviceSize>(w) * h * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo sai{};
    sai.usage = VMA_MEMORY_USAGE_AUTO;
    sai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info;
    check(vmaCreateBuffer(allocator, &bci, &sai, &staging, &staging_mem, &info), "staging buffer");
    staging_ptr = info.pMappedData;
}

void Display::Impl::update_pad() {
    // Last-used device wins: keyboard and gamepad are combined, analog comes from whichever moved.
    std::uint32_t buttons = keyboard_buttons | gamepad_buttons;
    float x = key_x, y = key_y;
    if (gamepad) {
        const float gx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
        const float gy = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
        if (std::abs(gx) > 0.2f || std::abs(gy) > 0.2f) {
            x = gx;
            y = gy;
        }
    }
    pad.buttons = buttons;
    pad.lx = static_cast<std::uint8_t>(std::clamp(128.0f + x * 127.0f, 0.0f, 255.0f));
    pad.ly = static_cast<std::uint8_t>(std::clamp(128.0f + y * 127.0f, 0.0f, 255.0f));
}

// ---------------------------------------------------------------------------------------------

Display::Display(const char* title, int width, int height) : impl_(std::make_unique<Impl>()) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());
    impl_->window = SDL_CreateWindow(title, width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!impl_->window) throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
    impl_->create_device();
    impl_->create_swapchain();
}

Display::~Display() {
    auto& d = *impl_;
    if (d.device) {
        vkDeviceWaitIdle(d.device);
        d.destroy_swapchain();
        if (d.frame) vmaDestroyImage(d.allocator, d.frame, d.frame_mem);
        if (d.staging) vmaDestroyBuffer(d.allocator, d.staging, d.staging_mem);
        vmaDestroyAllocator(d.allocator);
        vkDestroyFence(d.device, d.fence, nullptr);
        vkDestroySemaphore(d.device, d.acquired, nullptr);
        vkDestroyCommandPool(d.device, d.pool, nullptr);
        vkDestroyDevice(d.device, nullptr);
    }
    if (d.surface) vkDestroySurfaceKHR(d.instance, d.surface, nullptr);
    if (d.instance) vkDestroyInstance(d.instance, nullptr);
    if (d.gamepad) SDL_CloseGamepad(d.gamepad);
    if (d.window) SDL_DestroyWindow(d.window);
    SDL_Quit();
}

bool Display::poll_events() {
    auto& d = *impl_;
    SDL_Event e;
    bool running = true;
    const auto key_bit = [](SDL_Keycode k) -> std::uint32_t {
        switch (k) {
            case SDLK_UP: return kUp;
            case SDLK_DOWN: return kDown;
            case SDLK_LEFT: return kLeft;
            case SDLK_RIGHT: return kRight;
            case SDLK_K: return kCross;
            case SDLK_L: return kCircle;
            case SDLK_J: return kSquare;
            case SDLK_I: return kTriangle;
            case SDLK_Q: return kLTrigger;
            case SDLK_E: return kRTrigger;
            case SDLK_RETURN: return kStart;
            case SDLK_BACKSPACE: return kSelect;
            default: return 0;
        }
    };
    const auto pad_bit = [](Uint8 b) -> std::uint32_t {
        switch (b) {
            case SDL_GAMEPAD_BUTTON_SOUTH: return kCross;
            case SDL_GAMEPAD_BUTTON_EAST: return kCircle;
            case SDL_GAMEPAD_BUTTON_WEST: return kSquare;
            case SDL_GAMEPAD_BUTTON_NORTH: return kTriangle;
            case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return kLTrigger;
            case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return kRTrigger;
            case SDL_GAMEPAD_BUTTON_START: return kStart;
            case SDL_GAMEPAD_BUTTON_BACK: return kSelect;
            case SDL_GAMEPAD_BUTTON_DPAD_UP: return kUp;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return kDown;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return kLeft;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return kRight;
            default: return 0;
        }
    };
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: d.swapchain_dirty = true; break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                const bool down = e.type == SDL_EVENT_KEY_DOWN;
                if (const auto bit = key_bit(e.key.key)) d.keyboard_buttons = down ? d.keyboard_buttons | bit : d.keyboard_buttons & ~bit;
                const float v = down ? 1.0f : 0.0f;
                if (e.key.key == SDLK_A) d.key_x = -v;
                if (e.key.key == SDLK_D) d.key_x = v;
                if (e.key.key == SDLK_W) d.key_y = -v;
                if (e.key.key == SDLK_S) d.key_y = v;
                break;
            }
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!d.gamepad) d.gamepad = SDL_OpenGamepad(e.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (d.gamepad && SDL_GetGamepadID(d.gamepad) == e.gdevice.which) {
                    SDL_CloseGamepad(d.gamepad);
                    d.gamepad = nullptr;
                    d.gamepad_buttons = 0;
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP: {
                const auto bit = pad_bit(e.gbutton.button);
                d.gamepad_buttons = e.gbutton.down ? d.gamepad_buttons | bit : d.gamepad_buttons & ~bit;
                break;
            }
            default: break;
        }
    }
    d.update_pad();
    return running;
}

PadState Display::pad() const { return impl_->pad; }

void Display::present(const std::uint32_t* rgba, int width, int height) {
    auto& d = *impl_;
    if (d.swapchain_dirty || !d.swapchain) {
        d.destroy_swapchain();
        d.create_swapchain();
        if (!d.swapchain) return;
    }
    vkWaitForFences(d.device, 1, &d.fence, VK_TRUE, UINT64_MAX);
    d.ensure_frame_image(width, height);
    std::memcpy(d.staging_ptr, rgba, static_cast<std::size_t>(width) * height * 4);

    std::uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(d.device, d.swapchain, UINT64_MAX, d.acquired, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        d.swapchain_dirty = true;
        return;
    }
    vkResetFences(d.device, 1, &d.fence);
    vkResetCommandBuffer(d.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(d.cmd, &bi);

    const auto barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from;
        b.newLayout = to;
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(d.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
    };
    const VkImage target = d.images[index];

    barrier(d.frame, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1};
    vkCmdCopyBufferToImage(d.cmd, d.staging, d.frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(d.frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);

    barrier(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkClearColorValue black{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(d.cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    // Fit the frame into the window, keeping its aspect ratio.
    const double scale = std::min(static_cast<double>(d.extent.width) / width, static_cast<double>(d.extent.height) / height);
    const int dw = static_cast<int>(width * scale), dh = static_cast<int>(height * scale);
    const int dx = (static_cast<int>(d.extent.width) - dw) / 2, dy = (static_cast<int>(d.extent.height) - dh) / 2;
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {width, height, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {dx, dy, 0};
    blit.dstOffsets[1] = {dx + dw, dy + dh, 1};
    vkCmdBlitImage(d.cmd, d.frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_LINEAR);
    barrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    vkEndCommandBuffer(d.cmd);

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &d.acquired;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &d.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &d.rendered[index];
    check(vkQueueSubmit(d.queue, 1, &si, d.fence), "vkQueueSubmit");

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &d.rendered[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &d.swapchain;
    pi.pImageIndices = &index;
    r = vkQueuePresentKHR(d.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) d.swapchain_dirty = true;
}

}  // namespace ff7cc::platform
