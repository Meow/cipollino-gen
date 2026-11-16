// Vulkan backend: any GPU with a Vulkan driver and 64-bit integers in shaders (AMD, Intel,
// NVIDIA, and Apple through MoltenVK). One instance per physical device.
//
// The kernel is the shared source compiled to SPIR-V at build time (kernel/entry_vulkan.comp),
// one module per field arithmetic and scratch mode.
//
// On Windows the Vulkan loader (vulkan-1.dll) is installed by the GPU driver, so a machine
// may have none, and a program that imports it would then not start at all. There the
// loader is opened at run time instead (CIP_VULKAN_DYNAMIC; the other systems can be built
// that way too, which is how this path gets tested).
#if !defined(CIP_VULKAN_DYNAMIC)
#if defined(_WIN32)
#define CIP_VULKAN_DYNAMIC 1
#else
#define CIP_VULKAN_DYNAMIC 0
#endif
#endif

#if CIP_VULKAN_DYNAMIC
#define VK_NO_PROTOTYPES
#endif
#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vulkan/vulkan.h>

#if CIP_VULKAN_DYNAMIC
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

#include "backend.h"

#define CIP_TARGET_CPU
#include "kernel/defs.h"
#include "kernel_vk.inc" // kernel_vk_{w,n}{0,1,2,3}_spv[] and their _len: arithmetic, CIP_SCRATCH_*

namespace {

#if CIP_VULKAN_DYNAMIC

// Every Vulkan function this file calls.
// clang-format off: a list, which the formatter would indent as one expression
#define CIP_VK_FUNCTIONS(X) \
  X(vkAllocateCommandBuffers) X(vkAllocateDescriptorSets) X(vkAllocateMemory) \
  X(vkBeginCommandBuffer) X(vkBindBufferMemory) X(vkCmdBindDescriptorSets) X(vkCmdBindPipeline) \
  X(vkCmdCopyBuffer) X(vkCmdDispatch) X(vkCmdPushConstants) X(vkCreateBuffer) \
  X(vkCreateCommandPool) X(vkCreateComputePipelines) X(vkCreateDescriptorPool) \
  X(vkCreateDescriptorSetLayout) X(vkCreateDevice) X(vkCreateFence) X(vkCreateInstance) \
  X(vkCreatePipelineLayout) X(vkCreateShaderModule) X(vkDestroyBuffer) X(vkDestroyCommandPool) \
  X(vkDestroyDescriptorPool) X(vkDestroyDescriptorSetLayout) X(vkDestroyDevice) X(vkDestroyFence) \
  X(vkDestroyInstance) X(vkDestroyPipeline) X(vkDestroyPipelineLayout) X(vkDestroyShaderModule) \
  X(vkDeviceWaitIdle) X(vkEndCommandBuffer) X(vkEnumerateDeviceExtensionProperties) \
  X(vkEnumerateInstanceExtensionProperties) X(vkEnumeratePhysicalDevices) X(vkFreeMemory) \
  X(vkGetBufferMemoryRequirements) X(vkGetDeviceQueue) X(vkGetPhysicalDeviceFeatures) \
  X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceProperties) \
  X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkMapMemory) \
  X(vkQueueSubmit) X(vkResetCommandBuffer) X(vkResetFences) X(vkUpdateDescriptorSets) \
  X(vkWaitForFences)
  // clang-format on

#define X(name) PFN_##name name = nullptr;
  CIP_VK_FUNCTIONS(X)
#undef X

#if !defined(CIP_VULKAN_LIBRARY)
#if defined(_WIN32)
#define CIP_VULKAN_LIBRARY "vulkan-1.dll"
#elif defined(__APPLE__)
#define CIP_VULKAN_LIBRARY "libvulkan.1.dylib"
#else
#define CIP_VULKAN_LIBRARY "libvulkan.so.1"
#endif
#endif

  // Opens the loader and resolves the functions, once. Returns false with a message if
  // this machine has no (or too old a) Vulkan loader.
  bool load_vulkan(std::string &err) {
    static bool tried = false;
    static std::string failure;
    if (!tried) {
      tried = true;
#if defined(_WIN32)
      HMODULE lib = LoadLibraryA(CIP_VULKAN_LIBRARY);
#define CIP_VK_SYMBOL(name) (void *)GetProcAddress(lib, name)
#else
      void *lib = dlopen(CIP_VULKAN_LIBRARY, RTLD_NOW | RTLD_LOCAL);
#define CIP_VK_SYMBOL(name) dlsym(lib, name)
#endif
      if (!lib) {
        failure = "no Vulkan loader (" CIP_VULKAN_LIBRARY ") on this machine";
      } else {
#define X(name)                                              \
  name = reinterpret_cast<PFN_##name>(CIP_VK_SYMBOL(#name)); \
  if (!name && failure.empty())                              \
    failure = "the Vulkan loader (" CIP_VULKAN_LIBRARY ") has no " #name;
        CIP_VK_FUNCTIONS(X)
#undef X
      }
#undef CIP_VK_SYMBOL
    }
    err = failure;
    return failure.empty();
  }

#endif // CIP_VULKAN_DYNAMIC

  const uint32_t kLocalSize = 64; // local_size_x in entry_vulkan.comp

  void check(VkResult r, const char *what) {
    if (r != VK_SUCCESS)
      throw std::runtime_error(std::string("Vulkan: ") + what + " failed (VkResult " + std::to_string((int)r) + ")");
  }

  struct Instance {
    VkInstance handle = VK_NULL_HANDLE;

    ~Instance() {
      if (handle)
        vkDestroyInstance(handle, nullptr);
    }
  };

  bool has_extension(const std::vector<VkExtensionProperties> &list, const char *name) {
    for (const VkExtensionProperties &e : list)
      if (strcmp(e.extensionName, name) == 0)
        return true;
    return false;
  }

  class VulkanBackend : public Backend {
  public:
    VulkanBackend(std::shared_ptr<Instance> instance, VkPhysicalDevice phys, int index) :
        instance_(std::move(instance)), phys_(phys), index_(index) {
      vkGetPhysicalDeviceProperties(phys_, &props_);
      vkGetPhysicalDeviceMemoryProperties(phys_, &mem_);
      try {
        create_device();
        create_pipeline_layout();
        hitctr_ = make_buffer(4, true);
        hits_   = make_buffer((size_t)kHitCap * CIP_HIT_WORDS * 4, true);
        // Placeholders so every binding is valid before configure() / set_filter().
        state_   = make_buffer(4, false);
        table_   = make_buffer(4, false);
        scratch_ = make_buffer(4, false);
        filt_    = make_buffer(16, false);
        bitmap_  = make_buffer((size_t)CIP_BITMAP_WORDS * 4, false);
        bind_buffers();
      } catch (...) {
        destroy();
        throw;
      }
    }

    ~VulkanBackend() override { destroy(); }

    std::string describe() const override { return "Vulkan #" + std::to_string(index_) + ": " + props_.deviceName; }

    // There is no portable way to ask a Vulkan device how wide it is, and a batch must stay
    // well under the seconds after which drivers reset a GPU that does not answer, so be
    // generous only where the class of device is known to be fast.
    uint32_t suggested_threads() const override {
      if (props_.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        return 131072;
      return apple() ? 65536 : 16384;
    }

    // Integrated GPUs come two compute units wide and sixteen, and the small ones lose a
    // third of their speed when given the threads (and with them the scratch memory) of
    // the large ones: measured on a 2-unit Radeon 610M, which is fastest at 1024 to 2048.
    // Discrete AMD GPUs vary as much: a 40-unit RX 6700 XT is fastest at 65536, 5% ahead
    // of 131072 and 15% ahead of 16384.
    std::vector<uint32_t> thread_candidates() const override {
      if (apple())
        return { suggested_threads() };
      if (props_.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
        return { 1024, 2048, 4096, 8192, 16384 };
      if (props_.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && amd())
        return { 16384, 32768, 65536, 131072 };
      return { suggested_threads() };
    }

    uint32_t suggested_pairs() const override {
      if (nvidia())
        return 512; // short batches, see suggested_launch_seconds()
      if (props_.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        return 2048;
      return apple() ? 1024 : 512;
    }

    uint64_t scratch_budget() const override {
      uint64_t local = 0;
      for (uint32_t i = 0; i < mem_.memoryHeapCount; i++)
        if (mem_.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
          local = std::max<uint64_t>(local, mem_.memoryHeaps[i].size);
      // One buffer, addressed with 32-bit word indices by the shader.
      return std::min<uint64_t>({ local / 4, max_buffer_bytes(), (uint64_t)4 << 30 });
    }

    std::vector<uint32_t> scratch_candidates() const override {
      return { CIP_SCRATCH_LEAN, CIP_SCRATCH_HALF, CIP_SCRATCH_FULL };
    }

    // NVIDIA's driver cannot cheaply preempt a running Vulkan compute dispatch. When
    // something else wants the GPU (a compositor drawing a frame is enough), the dispatch
    // in flight crawls at about 1/17 of its speed until it ends, and one that then needs
    // more than several seconds gets the device reset (Xid 109, "CTX SWITCH TIMEOUT"). On
    // an RTX 4090 that also drives a desktop, 250 ms launches stall for 4.4 s at a time
    // and 500 ms ones lose the device. Short launches bound the damage. CUDA has no such
    // problem and is what `auto` picks on NVIDIA.
    double suggested_launch_seconds() const override {
      if (nvidia())
        return 0.05;
      return props_.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 0.25 : 0.1;
    }

    // AMD: a 32x32 multiply costs RDNA 2 as much as 4.5 additions and a 24-bit one as
    // much as one, and Mesa's compiler spends two of the former on every 64-bit product,
    // so narrow is 1.6 times as fast there. AMD's own drivers have another compiler:
    // measure both. Apple: an M5 Pro runs narrow at 0.42 of wide's speed. NVIDIA: not
    // run; by the instruction energies of an RTX 4090 narrow's 400 multiplies cost twice
    // the 100 wide ones. Intel: the narrow kernel ends the process inside the Windows
    // driver of a UHD 730, which no self-test can catch. So narrow is tried on AMD alone;
    // --arith narrow asks for it anywhere.
    std::vector<uint32_t> arith_candidates() const override {
      if (amd())
        return { CIP_ARITH_NARROW, CIP_ARITH_WIDE };
      return { CIP_ARITH_WIDE };
    }

    void set_arith(uint32_t arith) override {
      if (arith > CIP_ARITH_NARROW)
        throw std::runtime_error("bad field arithmetic");
      arith_ = arith;
    }

    void configure(uint32_t nthreads, uint32_t npairs, uint32_t smode, const std::vector<uint32_t> &table) override {
      if (smode > CIP_SCRATCH_X25519)
        throw std::runtime_error("bad scratch mode");
      uint64_t scratch = scratch_bytes(nthreads, npairs, smode);
      if (scratch > max_buffer_bytes())
        throw std::runtime_error(
          "scratch buffer exceeds what this Vulkan device allows in one buffer; lower --threads or --pairs"
        );
      active_       = pipeline(arith_, smode); // compile before allocating anything
      prm_          = cip_params();
      prm_.nthreads = nthreads;
      prm_.npairs   = npairs;
      prm_.hitcap   = kHitCap;
      free_buffer(state_);
      free_buffer(table_);
      free_buffer(scratch_);
      state_   = make_buffer((size_t)nthreads * CIP_STATE_WORDS * 4, false);
      table_   = make_buffer(table.size() * 4, false);
      scratch_ = make_buffer(scratch, false);
      upload(table_, 0, table.data(), table.size() * 4);
      bind_buffers();
    }

    void set_filter(const Filter &f) override {
      prm_.mode  = f.mode;
      prm_.nfilt = f.count();
      memcpy(prm_.dm, f.dm, sizeof prm_.dm);
      memcpy(prm_.dv, f.dv, sizeof prm_.dv);
      size_t bytes = f.records.size() * 4;
      if (bytes > filt_.size) {
        free_buffer(filt_);
        filt_ = make_buffer(bytes, false);
        bind_buffers();
      }
      if (bytes)
        upload(filt_, 0, f.records.data(), bytes);
      if (!f.bitmap.empty())
        upload(bitmap_, 0, f.bitmap.data(), f.bitmap.size() * 4);
    }

    // Everything the kernel touches per launch lives in device memory: on a discrete GPU a
    // mapped buffer sits in system RAM, and reading every thread's state from there at the
    // start of each launch costs tens of milliseconds. Only the hit buffers stay mapped.
    void write_state(uint32_t tid, uint32_t count, const uint32_t *words) override {
      upload(state_, (size_t)tid * CIP_STATE_WORDS * 4, words, (size_t)count * CIP_STATE_WORDS * 4);
    }

    void read_state(uint32_t tid, uint32_t count, uint32_t *words) override {
      download(state_, (size_t)tid * CIP_STATE_WORDS * 4, words, (size_t)count * CIP_STATE_WORDS * 4);
    }

    uint32_t launch(uint32_t nbatches, std::vector<Hit> &hits, uint32_t first, uint32_t count) override {
      prm_.nbatches            = nbatches;
      prm_.first               = first;
      prm_.count               = count;
      *(uint32_t *)hitctr_.map = 0;

      begin();
      vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, active_);
      vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &dset_, 0, nullptr);
      vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof prm_, &prm_);
      vkCmdDispatch(cmd_, (prm_.nthreads + kLocalSize - 1) / kLocalSize, 1, 1);
      submit("kernel launch");

      uint32_t n        = *(uint32_t *)hitctr_.map;
      const uint32_t *h = (const uint32_t *)hits_.map;
      hits.clear();
      for (uint32_t i = 0; i < std::min(n, kHitCap); i++)
        hits.push_back(Hit { h[3 * i], h[3 * i + 1], h[3 * i + 2] });
      return n;
    }

  private:
    struct Buffer {
      VkBuffer buffer       = VK_NULL_HANDLE;
      VkDeviceMemory memory = VK_NULL_HANDLE;
      void *map             = nullptr;
      size_t size           = 0;
    };

    bool apple() const { return props_.vendorID == 0x106b; }

    bool amd() const { return props_.vendorID == 0x1002; }

    bool nvidia() const { return props_.vendorID == 0x10de; }

    uint64_t max_buffer_bytes() const {
      VkPhysicalDeviceMaintenance3Properties m3 = {};
      m3.sType                                  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES;
      VkPhysicalDeviceProperties2 p2            = {};
      p2.sType                                  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
      p2.pNext                                  = &m3;
      vkGetPhysicalDeviceProperties2(phys_, &p2);
      return std::min<uint64_t>(props_.limits.maxStorageBufferRange, m3.maxMemoryAllocationSize);
    }

    void create_device() {
      VkPhysicalDeviceFeatures have;
      vkGetPhysicalDeviceFeatures(phys_, &have);
      if (!have.shaderInt64)
        throw std::runtime_error("device has no 64-bit integers in shaders");

      uint32_t n = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(phys_, &n, nullptr);
      std::vector<VkQueueFamilyProperties> families(n);
      vkGetPhysicalDeviceQueueFamilyProperties(phys_, &n, families.data());
      family_ = UINT32_MAX;
      for (uint32_t i = 0; i < n; i++)
        if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && family_ == UINT32_MAX)
          family_ = i;
      if (family_ == UINT32_MAX)
        throw std::runtime_error("device has no compute queue");

      check(vkEnumerateDeviceExtensionProperties(phys_, nullptr, &n, nullptr), "vkEnumerateDeviceExtensionProperties");
      std::vector<VkExtensionProperties> exts(n);
      check(
        vkEnumerateDeviceExtensionProperties(phys_, nullptr, &n, exts.data()), "vkEnumerateDeviceExtensionProperties"
      );
      std::vector<const char *> enable;
      // Mandatory to enable where it exists (MoltenVK).
      if (has_extension(exts, "VK_KHR_portability_subset"))
        enable.push_back("VK_KHR_portability_subset");

      float priority                = 1.0f;
      VkDeviceQueueCreateInfo qi    = {};
      qi.sType                      = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
      qi.queueFamilyIndex           = family_;
      qi.queueCount                 = 1;
      qi.pQueuePriorities           = &priority;
      VkPhysicalDeviceFeatures want = {};
      want.shaderInt64              = VK_TRUE;
      VkDeviceCreateInfo di         = {};
      di.sType                      = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
      di.queueCreateInfoCount       = 1;
      di.pQueueCreateInfos          = &qi;
      di.enabledExtensionCount      = (uint32_t)enable.size();
      di.ppEnabledExtensionNames    = enable.data();
      di.pEnabledFeatures           = &want;
      check(vkCreateDevice(phys_, &di, nullptr, &dev_), "vkCreateDevice");
      vkGetDeviceQueue(dev_, family_, 0, &queue_);

      VkCommandPoolCreateInfo pi = {};
      pi.sType                   = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      pi.flags                   = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      pi.queueFamilyIndex        = family_;
      check(vkCreateCommandPool(dev_, &pi, nullptr, &pool_), "vkCreateCommandPool");
      VkCommandBufferAllocateInfo ai = {};
      ai.sType                       = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      ai.commandPool                 = pool_;
      ai.level                       = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      ai.commandBufferCount          = 1;
      check(vkAllocateCommandBuffers(dev_, &ai, &cmd_), "vkAllocateCommandBuffers");
      VkFenceCreateInfo fi = {};
      fi.sType             = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      check(vkCreateFence(dev_, &fi, nullptr, &fence_), "vkCreateFence");
    }

    void create_pipeline_layout() {
      VkDescriptorSetLayoutBinding bindings[kBindings] = {};
      for (uint32_t i = 0; i < kBindings; i++) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
      }
      VkDescriptorSetLayoutCreateInfo li = {};
      li.sType                           = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
      li.bindingCount                    = kBindings;
      li.pBindings                       = bindings;
      check(vkCreateDescriptorSetLayout(dev_, &li, nullptr, &dsl_), "vkCreateDescriptorSetLayout");

      VkPushConstantRange range     = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cip_params) };
      VkPipelineLayoutCreateInfo pl = {};
      pl.sType                      = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
      pl.setLayoutCount             = 1;
      pl.pSetLayouts                = &dsl_;
      pl.pushConstantRangeCount     = 1;
      pl.pPushConstantRanges        = &range;
      check(vkCreatePipelineLayout(dev_, &pl, nullptr, &layout_), "vkCreatePipelineLayout");

      VkDescriptorPoolSize size     = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kBindings };
      VkDescriptorPoolCreateInfo dp = {};
      dp.sType                      = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
      dp.maxSets                    = 1;
      dp.poolSizeCount              = 1;
      dp.pPoolSizes                 = &size;
      check(vkCreateDescriptorPool(dev_, &dp, nullptr, &dpool_), "vkCreateDescriptorPool");
      VkDescriptorSetAllocateInfo da = {};
      da.sType                       = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      da.descriptorPool              = dpool_;
      da.descriptorSetCount          = 1;
      da.pSetLayouts                 = &dsl_;
      check(vkAllocateDescriptorSets(dev_, &da, &dset_), "vkAllocateDescriptorSets");
    }

    // The compute pipeline for a field arithmetic and scratch mode, built (and compiled by
    // the driver) on first use.
    VkPipeline pipeline(uint32_t arith, uint32_t smode) {
      const uint32_t at = 4 * arith + smode;
      if (pipelines_[at])
        return pipelines_[at];
      static const unsigned char *const code[kModules] = { kernel_vk_w0_spv, kernel_vk_w1_spv, kernel_vk_w2_spv,
                                                           kernel_vk_w3_spv, kernel_vk_n0_spv, kernel_vk_n1_spv,
                                                           kernel_vk_n2_spv, kernel_vk_n3_spv };
      const unsigned len[kModules] = { kernel_vk_w0_spv_len, kernel_vk_w1_spv_len, kernel_vk_w2_spv_len,
                                       kernel_vk_w3_spv_len, kernel_vk_n0_spv_len, kernel_vk_n1_spv_len,
                                       kernel_vk_n2_spv_len, kernel_vk_n3_spv_len };
      std::vector<uint32_t> words(len[at] / 4); // SPIR-V must be handed over 4-byte aligned
      memcpy(words.data(), code[at], words.size() * 4);

      VkShaderModuleCreateInfo mi = {};
      mi.sType                    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      mi.codeSize                 = words.size() * 4;
      mi.pCode                    = words.data();
      check(vkCreateShaderModule(dev_, &mi, nullptr, &modules_[at]), "vkCreateShaderModule");

      VkComputePipelineCreateInfo ci = {};
      ci.sType                       = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
      ci.stage.sType                 = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      ci.stage.stage                 = VK_SHADER_STAGE_COMPUTE_BIT;
      ci.stage.module                = modules_[at];
      ci.stage.pName                 = "main";
      ci.layout                      = layout_;
      check(
        vkCreateComputePipelines(dev_, VK_NULL_HANDLE, 1, &ci, nullptr, &pipelines_[at]),
        "compiling the kernel (vkCreateComputePipelines)"
      );
      return pipelines_[at];
    }

    uint32_t memory_type(uint32_t allowed, bool host) const {
      const VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      const VkMemoryPropertyFlags local   = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      // Preferences, best first. Host buffers must be mappable; kernel-only buffers want
      // to live on the device but will take anything.
      const VkMemoryPropertyFlags host_want[] = { visible | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, visible };
      const VkMemoryPropertyFlags dev_want[]  = { local, 0 };
      for (VkMemoryPropertyFlags want : host ? host_want : dev_want) {
        // First pass for kernel-only buffers: device-local memory the host cannot see.
        for (int pass = 0; pass < 2; pass++)
          for (uint32_t i = 0; i < mem_.memoryTypeCount; i++) {
            VkMemoryPropertyFlags f = mem_.memoryTypes[i].propertyFlags;
            if (!(allowed & (1u << i)) || (f & want) != want)
              continue;
            if (!host && pass == 0 && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
              continue;
            return i;
          }
      }
      throw std::runtime_error("Vulkan: no suitable memory type");
    }

    Buffer make_buffer(size_t bytes, bool host) {
      Buffer b;
      b.size                = std::max<size_t>(bytes, 4);
      VkBufferCreateInfo bi = {};
      bi.sType              = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      bi.size               = b.size;
      bi.usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      check(vkCreateBuffer(dev_, &bi, nullptr, &b.buffer), "vkCreateBuffer");
      VkMemoryRequirements req;
      vkGetBufferMemoryRequirements(dev_, b.buffer, &req);
      VkMemoryAllocateInfo ai = {};
      ai.sType                = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      ai.allocationSize       = req.size;
      ai.memoryTypeIndex      = memory_type(req.memoryTypeBits, host);
      VkResult r              = vkAllocateMemory(dev_, &ai, nullptr, &b.memory);
      if (r != VK_SUCCESS) {
        vkDestroyBuffer(dev_, b.buffer, nullptr);
        throw std::runtime_error(
          "Vulkan: allocating " + std::to_string(b.size >> 20) + " MiB on " + props_.deviceName +
          " failed (lower --threads or --pairs, or use --scratch half)"
        );
      }
      check(vkBindBufferMemory(dev_, b.buffer, b.memory, 0), "vkBindBufferMemory");
      if (host) {
        check(vkMapMemory(dev_, b.memory, 0, VK_WHOLE_SIZE, 0, &b.map), "vkMapMemory");
        memset(b.map, 0, b.size);
      }
      return b;
    }

    void free_buffer(Buffer &b) {
      if (b.buffer)
        vkDestroyBuffer(dev_, b.buffer, nullptr);
      if (b.memory)
        vkFreeMemory(dev_, b.memory, nullptr); // unmaps
      b = Buffer();
    }

    // Copy between host memory and a kernel-only buffer through a temporary mapped one.
    void upload(Buffer &dst, size_t offset, const void *data, size_t bytes) {
      transfer(dst, offset, (void *)data, bytes, true);
    }

    void download(Buffer &src, size_t offset, void *data, size_t bytes) { transfer(src, offset, data, bytes, false); }

    void transfer(Buffer &device, size_t offset, void *data, size_t bytes, bool to_device) {
      Buffer staging = make_buffer(bytes, true);
      try {
        if (to_device)
          memcpy(staging.map, data, bytes);
        begin();
        VkBufferCopy region = { to_device ? 0 : offset, to_device ? offset : 0, bytes };
        if (to_device)
          vkCmdCopyBuffer(cmd_, staging.buffer, device.buffer, 1, &region);
        else
          vkCmdCopyBuffer(cmd_, device.buffer, staging.buffer, 1, &region);
        submit("buffer transfer");
        if (!to_device)
          memcpy(data, staging.map, bytes);
      } catch (...) {
        free_buffer(staging);
        throw;
      }
      free_buffer(staging);
    }

    void bind_buffers() {
      const Buffer *all[kBindings] = { &state_, &table_, &scratch_, &hitctr_, &hits_, &filt_, &bitmap_ };
      VkDescriptorBufferInfo info[kBindings];
      VkWriteDescriptorSet writes[kBindings] = {};
      for (uint32_t i = 0; i < kBindings; i++) {
        info[i]                   = { all[i]->buffer, 0, VK_WHOLE_SIZE };
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = dset_;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo     = &info[i];
      }
      vkUpdateDescriptorSets(dev_, kBindings, writes, 0, nullptr);
    }

    void begin() {
      check(vkResetCommandBuffer(cmd_, 0), "vkResetCommandBuffer");
      VkCommandBufferBeginInfo bi = {};
      bi.sType                    = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags                    = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      check(vkBeginCommandBuffer(cmd_, &bi), "vkBeginCommandBuffer");
    }

    void submit(const char *what) {
      check(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer");
      VkSubmitInfo si       = {};
      si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      si.commandBufferCount = 1;
      si.pCommandBuffers    = &cmd_;
      check(vkQueueSubmit(queue_, 1, &si, fence_), what);
      check(vkWaitForFences(dev_, 1, &fence_, VK_TRUE, UINT64_MAX), what);
      check(vkResetFences(dev_, 1, &fence_), "vkResetFences");
    }

    void destroy() {
      if (!dev_)
        return;
      vkDeviceWaitIdle(dev_);
      for (Buffer *b : { &state_, &table_, &scratch_, &hitctr_, &hits_, &filt_, &bitmap_ })
        free_buffer(*b);
      for (VkPipeline p : pipelines_)
        if (p)
          vkDestroyPipeline(dev_, p, nullptr);
      for (VkShaderModule m : modules_)
        if (m)
          vkDestroyShaderModule(dev_, m, nullptr);
      if (dpool_)
        vkDestroyDescriptorPool(dev_, dpool_, nullptr);
      if (layout_)
        vkDestroyPipelineLayout(dev_, layout_, nullptr);
      if (dsl_)
        vkDestroyDescriptorSetLayout(dev_, dsl_, nullptr);
      if (fence_)
        vkDestroyFence(dev_, fence_, nullptr);
      if (pool_)
        vkDestroyCommandPool(dev_, pool_, nullptr);
      vkDestroyDevice(dev_, nullptr);
      dev_ = VK_NULL_HANDLE;
    }

    static constexpr uint32_t kBindings = 7; // state, table, scratch, hitctr, hits, filt, bitmap
    static constexpr uint32_t kModules  = 8; // field arithmetics times loop variants

    // Members in order of alignment: the handles and what holds 64-bit values first.
    std::shared_ptr<Instance> instance_; // must outlive the device
    VkPhysicalDevice phys_;
    VkPhysicalDeviceProperties props_;
    VkDevice dev_                     = VK_NULL_HANDLE;
    VkQueue queue_                    = VK_NULL_HANDLE;
    VkCommandPool pool_               = VK_NULL_HANDLE;
    VkCommandBuffer cmd_              = VK_NULL_HANDLE;
    VkFence fence_                    = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_        = VK_NULL_HANDLE;
    VkPipelineLayout layout_          = VK_NULL_HANDLE;
    VkDescriptorPool dpool_           = VK_NULL_HANDLE;
    VkDescriptorSet dset_             = VK_NULL_HANDLE;
    VkShaderModule modules_[kModules] = {};
    VkPipeline pipelines_[kModules]   = {};
    VkPipeline active_                = VK_NULL_HANDLE; // the one configure() chose
    Buffer state_, table_, scratch_, hitctr_, hits_, filt_, bitmap_;
    VkPhysicalDeviceMemoryProperties mem_;
    cip_params prm_  = cip_params();
    uint32_t family_ = 0;
    uint32_t arith_  = CIP_ARITH_WIDE;
    int index_;
  };

} // namespace

std::vector<std::unique_ptr<Backend>> create_vulkan_backends(const std::vector<int> &devices, std::string &err) {
  std::vector<std::unique_ptr<Backend>> v;
#if CIP_VULKAN_DYNAMIC
  if (!load_vulkan(err))
    return v;
#endif
  try {
    uint32_t n = 0;
    check(vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr), "vkEnumerateInstanceExtensionProperties");
    std::vector<VkExtensionProperties> exts(n);
    check(vkEnumerateInstanceExtensionProperties(nullptr, &n, exts.data()), "vkEnumerateInstanceExtensionProperties");

    VkApplicationInfo app   = {};
    app.sType               = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName    = "cipollino";
    app.apiVersion          = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ii = {};
    ii.sType                = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ii.pApplicationInfo     = &app;
    std::vector<const char *> enable;
    // Without this the loader hides "non-conformant" drivers, which is how MoltenVK is listed.
    if (has_extension(exts, "VK_KHR_portability_enumeration")) {
      enable.push_back("VK_KHR_portability_enumeration");
      ii.flags |= 0x00000001; // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
    }
    ii.enabledExtensionCount   = (uint32_t)enable.size();
    ii.ppEnabledExtensionNames = enable.data();
    auto instance              = std::make_shared<Instance>();
    check(vkCreateInstance(&ii, nullptr, &instance->handle), "vkCreateInstance");

    check(vkEnumeratePhysicalDevices(instance->handle, &n, nullptr), "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> phys(n);
    if (n)
      check(vkEnumeratePhysicalDevices(instance->handle, &n, phys.data()), "vkEnumeratePhysicalDevices");

    std::string skipped;
    for (uint32_t i = 0; i < n; i++) {
      VkPhysicalDeviceProperties props;
      vkGetPhysicalDeviceProperties(phys[i], &props);
      bool chosen = std::find(devices.begin(), devices.end(), (int)i) != devices.end();
      // Software rasterisers (llvmpipe) are slower than the CPU backend: only on request.
      if (devices.empty() ? props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU : !chosen)
        continue;
      try {
        v.emplace_back(new VulkanBackend(instance, phys[i], (int)i));
      } catch (const std::exception &e) {
        if (chosen)
          throw;
        skipped += std::string(skipped.empty() ? "" : "; ") + props.deviceName + ": " + e.what();
      }
    }
    for (int d : devices)
      if (d < 0 || d >= (int)n)
        throw std::runtime_error("no Vulkan device " + std::to_string(d));
    if (v.empty())
      err = skipped.empty() ? "no Vulkan GPU found" : "no usable Vulkan GPU (" + skipped + ")";
  } catch (const std::exception &e) {
    err = e.what();
    v.clear();
  }
  return v;
}
