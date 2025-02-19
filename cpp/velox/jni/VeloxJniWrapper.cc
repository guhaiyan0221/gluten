/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <jni.h>

#include <glog/logging.h>
#include <jni/JniCommon.h>

#include <exception>
#include "JniUdf.h"
#include "compute/Runtime.h"
#include "compute/VeloxBackend.h"
#include "compute/VeloxRuntime.h"
#include "config/GlutenConfig.h"
#include "jni/JniError.h"
#include "jni/JniFileSystem.h"
#include "memory/VeloxMemoryManager.h"
#include "substrait/SubstraitToVeloxPlanValidator.h"
#include "velox/common/base/BloomFilter.h"

#include <iostream>

using namespace facebook;

#ifdef __cplusplus
extern "C" {
#endif

jint JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), jniVersion) != JNI_OK) {
    return JNI_ERR;
  }

  gluten::getJniCommonState()->ensureInitialized(env);
  gluten::getJniErrorState()->ensureInitialized(env);
  gluten::initVeloxJniFileSystem(env);
  gluten::initVeloxJniUDF(env);

  DLOG(INFO) << "Loaded Velox backend.";

  return jniVersion;
}

void JNI_OnUnload(JavaVM* vm, void*) {
  JNIEnv* env;
  vm->GetEnv(reinterpret_cast<void**>(&env), jniVersion);
  gluten::finalizeVeloxJniUDF(env);
  gluten::finalizeVeloxJniFileSystem(env);
  gluten::getJniErrorState()->close();
  gluten::getJniCommonState()->close();
  google::ShutdownGoogleLogging();
}

JNIEXPORT void JNICALL Java_org_apache_gluten_init_NativeBackendInitializer_initialize( // NOLINT
    JNIEnv* env,
    jclass,
    jbyteArray conf) {
  JNI_METHOD_START
  auto safeArray = gluten::getByteArrayElementsSafe(env, conf);
  auto sparkConf = gluten::parseConfMap(env, safeArray.elems(), safeArray.length());
  gluten::VeloxBackend::create(sparkConf);
  JNI_METHOD_END()
}

JNIEXPORT void JNICALL Java_org_apache_gluten_udf_UdfJniWrapper_getFunctionSignatures( // NOLINT
    JNIEnv* env,
    jclass) {
  JNI_METHOD_START
  gluten::jniGetFunctionSignatures(env);
  JNI_METHOD_END()
}

JNIEXPORT jobject JNICALL
Java_org_apache_gluten_vectorized_PlanEvaluatorJniWrapper_nativeValidateWithFailureReason( // NOLINT
    JNIEnv* env,
    jobject wrapper,
    jbyteArray planArray) {
  JNI_METHOD_START
  auto ctx = gluten::getRuntime(env, wrapper);
  auto safeArray = gluten::getByteArrayElementsSafe(env, planArray);
  auto planData = safeArray.elems();
  auto planSize = env->GetArrayLength(planArray);
  auto runtime = dynamic_cast<gluten::VeloxRuntime*>(ctx);
  if (runtime->debugModeEnabled()) {
    try {
      auto jsonPlan = gluten::substraitFromPbToJson("Plan", planData, planSize, std::nullopt);
      LOG(INFO) << std::string(50, '#') << " received substrait::Plan: for validation";
      LOG(INFO) << jsonPlan;
    } catch (const std::exception& e) {
      LOG(WARNING) << "Error converting Substrait plan for validation to JSON: " << e.what();
    }
  }

  ::substrait::Plan subPlan;
  gluten::parseProtobuf(planData, planSize, &subPlan);

  // A query context with dummy configs. Used for function validation.
  std::unordered_map<std::string, std::string> configs{
      {velox::core::QueryConfig::kSparkPartitionId, "0"}, {velox::core::QueryConfig::kSessionTimezone, "GMT"}};
  velox::core::QueryCtx queryCtx(nullptr, velox::core::QueryConfig(configs));
  auto pool = gluten::defaultLeafVeloxMemoryPool().get();
  // An execution context used for function validation.
  velox::core::ExecCtx execCtx(pool, &queryCtx);

  gluten::SubstraitToVeloxPlanValidator planValidator(pool, &execCtx);
  jclass infoCls = env->FindClass("Lorg/apache/gluten/validate/NativePlanValidationInfo;");
  if (infoCls == nullptr) {
    std::string errorMessage = "Unable to CreateGlobalClassReferenceOrError for NativePlanValidationInfo";
    throw gluten::GlutenException(errorMessage);
  }
  jmethodID method = env->GetMethodID(infoCls, "<init>", "(ILjava/lang/String;)V");
  try {
    auto isSupported = planValidator.validate(subPlan);
    auto logs = planValidator.getValidateLog();
    std::string concatLog;
    for (int i = 0; i < logs.size(); i++) {
      concatLog += logs[i] + "@";
    }
    return env->NewObject(infoCls, method, isSupported, env->NewStringUTF(concatLog.c_str()));
  } catch (std::invalid_argument& e) {
    LOG(INFO) << "Failed to validate substrait plan because " << e.what();
    // return false;
    auto isSupported = false;
    return env->NewObject(infoCls, method, isSupported, env->NewStringUTF(""));
  }
  JNI_METHOD_END(nullptr)
}

JNIEXPORT jlong JNICALL Java_org_apache_gluten_utils_VeloxBloomFilterJniWrapper_empty( // NOLINT
    JNIEnv* env,
    jobject wrapper,
    jint capacity) {
  JNI_METHOD_START
  auto ctx = gluten::getRuntime(env, wrapper);
  auto filter = std::make_shared<velox::BloomFilter<std::allocator<uint64_t>>>();
  filter->reset(capacity);
  GLUTEN_CHECK(filter->isSet(), "Bloom-filter is not initialized");
  return ctx->objectStore()->save(filter);
  JNI_METHOD_END(gluten::kInvalidResourceHandle)
}

JNIEXPORT jlong JNICALL Java_org_apache_gluten_utils_VeloxBloomFilterJniWrapper_init( // NOLINT
    JNIEnv* env,
    jobject wrapper,
    jbyteArray data) {
  JNI_METHOD_START
  auto safeArray = gluten::getByteArrayElementsSafe(env, data);
  auto ctx = gluten::getRuntime(env, wrapper);
  auto filter = std::make_shared<velox::BloomFilter<std::allocator<uint64_t>>>();
  uint8_t* serialized = safeArray.elems();
  filter->merge(reinterpret_cast<char*>(serialized));
  return ctx->objectStore()->save(filter);
  JNI_METHOD_END(gluten::kInvalidResourceHandle)
}

JNIEXPORT void JNICALL Java_org_apache_gluten_utils_VeloxBloomFilterJniWrapper_insertLong( // NOLINT
    JNIEnv* env,
    jobject wrapper,
    jlong handle,
    jlong item) {
  JNI_METHOD_START
  auto ctx = gluten::getRuntime(env, wrapper);
  auto filter = ctx->objectStore()->retrieve<velox::BloomFilter<std::allocator<uint64_t>>>(handle);
  GLUTEN_CHECK(filter->isSet(), "Bloom-filter is not initialized");
  filter->insert(folly::hasher<int64_t>()(item));
  JNI_METHOD_END()
}

JNIEXPORT jboolean JNICALL Java_org_apache_gluten_utils_VeloxBloomFilterJniWrapper_mightContainLong( // NOLINT
    JNIEnv* env,
    jobject wrapper,
    jlong handle,
    jlong item) {
  JNI_METHOD_START
  auto ctx = gluten::getRuntime(env, wrapper);
  auto filter = ctx->objectStore()->retrieve<velox::BloomFilter<std::allocator<uint64_t>>>(handle);
  GLUTEN_CHECK(filter->isSet(), "Bloom-filter is not initialized");
  bool out = filter->mayContain(folly::hasher<int64_t>()(item));
  return out;
  JNI_METHOD_END(false)
}

namespace {
static std::vector<char> serialize(BloomFilter<std::allocator<uint64_t>>* bf) {
  uint32_t size = bf->serializedSize();
  std::vector<char> buffer;
  buffer.reserve(size);
  char* data = buffer.data();
  bf->serialize(data);
  return buffer;
}
} // namespace

JNIEXPORT void JNICALL Java_org_apache_gluten_utils_VeloxBloomFilterJniWrapper_mergeFrom( // NOLINT
    JNIEnv* env,
    jobject wrapper,
    jlong handle,
    jlong other) {
  JNI_METHOD_START
  auto ctx = gluten::getRuntime(env, wrapper);
  auto to = ctx->objectStore()->retrieve<velox::BloomFilter<std::allocator<uint64_t>>>(handle);
  auto from = ctx->objectStore()->retrieve<velox::BloomFilter<std::allocator<uint64_t>>>(other);
  GLUTEN_CHECK(to->isSet(), "Bloom-filter is not initialized");
  GLUTEN_CHECK(from->isSet(), "Bloom-filter is not initialized");
  std::vector<char> serialized = serialize(from.get());
  to->merge(serialized.data());
  JNI_METHOD_END()
}

JNIEXPORT jbyteArray JNICALL Java_org_apache_gluten_utils_VeloxBloomFilterJniWrapper_serialize( // NOLINT
    JNIEnv* env,
    jobject wrapper,
    jlong handle) {
  JNI_METHOD_START
  auto ctx = gluten::getRuntime(env, wrapper);
  auto filter = ctx->objectStore()->retrieve<velox::BloomFilter<std::allocator<uint64_t>>>(handle);
  GLUTEN_CHECK(filter->isSet(), "Bloom-filter is not initialized");
  std::vector<char> buffer = serialize(filter.get());
  auto size = buffer.capacity();
  jbyteArray out = env->NewByteArray(size);
  env->SetByteArrayRegion(out, 0, size, reinterpret_cast<jbyte*>(buffer.data()));
  return out;
  JNI_METHOD_END(nullptr)
}

#ifdef __cplusplus
}
#endif
