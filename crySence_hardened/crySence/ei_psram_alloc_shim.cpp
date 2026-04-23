#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <cstring>

// Strong overrides for Edge Impulse allocators.
// This forces EI/TFLM scratch and overflow buffers to prefer PSRAM on ESP32-S3.
extern "C" void *ei_malloc(size_t size) {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  void *p = heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
  if (!p) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    p = heap_caps_aligned_alloc(16, size, MALLOC_CAP_DEFAULT);
#else
    p = heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
#endif
    static bool warned_malloc = false;
    if (!warned_malloc) {
      warned_malloc = true;
      Serial.printf("[EI_ALLOC] WARN: fallback DRAM em ei_malloc (%u bytes)\n", (unsigned)size);
    }
  }
  return p;
#else
  return malloc(size);
#endif
}

extern "C" void *ei_calloc(size_t nitems, size_t size) {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  const size_t total = nitems * size;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  void *p = heap_caps_aligned_alloc(16, total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  void *p = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
  if (!p) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    p = heap_caps_aligned_alloc(16, total, MALLOC_CAP_DEFAULT);
#else
    p = heap_caps_malloc(total, MALLOC_CAP_DEFAULT);
#endif
    static bool warned_calloc = false;
    if (!warned_calloc) {
      warned_calloc = true;
      Serial.printf("[EI_ALLOC] WARN: fallback DRAM em ei_calloc (%u bytes)\n", (unsigned)total);
    }
  }
  if (p) {
    memset(p, 0, total);
  }
  return p;
#else
  return calloc(nitems, size);
#endif
}

extern "C" void ei_free(void *ptr) {
  free(ptr);
}
