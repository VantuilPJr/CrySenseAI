#line 1 "C:\\Arduino\\crySense_ai\\crySence_hardened\\crySence\\ei_linker_shims.cpp"
// Forces compilation/link of generated Edge Impulse runtime porting layer.
// TFLite Micro runtime is provided by ESP32 platform (libespressif__esp-tflite-micro.a)
#include "libs/CrySense_AI_inferencing/src/edge-impulse-sdk/porting/espressif/ei_classifier_porting.cpp"

// TFLite MicroContext - needed by compiled models
#include "libs/CrySense_AI_inferencing/src/edge-impulse-sdk/tensorflow/lite/micro/micro_context.cpp"

// TFLite logging
#include "libs/CrySense_AI_inferencing/src/edge-impulse-sdk/tensorflow/lite/micro/micro_log.cpp"

// RESHAPE operator kernel
#include "libs/CrySense_AI_inferencing/src/edge-impulse-sdk/tensorflow/lite/micro/kernels/reshape.cpp"

// Minimal RegisterOp shim for EI kernels when ESP32 TFLM archive lacks this symbol.
namespace tflite {
namespace micro {
TfLiteRegistration RegisterOp(
		void* (*init)(TfLiteContext* context, const char* buffer, size_t length),
		TfLiteStatus (*prepare)(TfLiteContext* context, TfLiteNode* node),
		TfLiteStatus (*invoke)(TfLiteContext* context, TfLiteNode* node),
		void (*free)(TfLiteContext* context, void* buffer)) {
	TfLiteRegistration registration = {};
	registration.init = init;
	registration.free = free;
	registration.prepare = prepare;
	registration.invoke = invoke;
	registration.profiling_string = nullptr;
	registration.builtin_code = 0;
	registration.custom_name = nullptr;
	registration.version = 0;
	registration.registration_external = nullptr;
	return registration;
}
}  // namespace micro
}  // namespace tflite
