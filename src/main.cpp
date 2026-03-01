#include "main.hpp"

#include <cstddef>
#include <dlfcn.h>

#include "beatsaber-hook/shared/utils/il2cpp-functions.hpp"
#include "bsml/shared/BSML.hpp"
#include "bsml/shared/BSML/SharedCoroutineStarter.hpp"
#include "bsml/shared/Helpers/getters.hpp"
#include "custom-types/shared/coroutine.hpp"
#include "logger.hpp"
#include "modInfo.hpp"
#include "System/GC.hpp"

// TMPro
#include "TMPro/TextAlignmentOptions.hpp"
#include "TMPro/TextMeshPro.hpp"
using namespace TMPro;

// UnityEngine
#include "UnityEngine/AsyncOperation.hpp"
#include "UnityEngine/Camera.hpp"
#include "UnityEngine/Color.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Object.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/Transform.hpp"
using namespace UnityEngine;

namespace {
using MallocTrimFn = int (*)(size_t);
constexpr int kIncrementalGcPassLimit = 64;
constexpr int kUnloadUnusedAssetsFrameLimit = 1800;
bool cleanupRunning = false;

void TrimNativeHeap() {
  auto libcHandle = dlopen("libc.so", RTLD_NOW);
  if (!libcHandle) {
    Logger.info("libc.so is unavailable; skipping native heap trim.");
    return;
  }

  auto trimFn = reinterpret_cast<MallocTrimFn>(dlsym(libcHandle, "malloc_trim"));
  if (trimFn) {
    trimFn(0);
  } else {
    Logger.info("malloc_trim is unavailable; skipping native heap trim.");
  }

  dlclose(libcHandle);
}

void RunManagedGcSweep() {
  auto maxGeneration = System::GC::get_MaxGeneration();
  il2cpp_functions::gc_collect(maxGeneration);
  for (int i = 0; i < kIncrementalGcPassLimit; i++) {
    if (il2cpp_functions::gc_collect_a_little() == 0) {
      break;
    }
  }
  il2cpp_functions::gc_collect(maxGeneration);
}

void ShowDoneToast() {
  auto camera = Camera::get_main();
  if (!camera) {
    Logger.info("Main camera is unavailable; skipping DONE toast.");
    return;
  }

  auto cameraTransform = camera->get_transform();
  auto cameraPosition = cameraTransform->get_position();
  auto cameraForward = cameraTransform->get_forward();

  auto toastPosition = Vector3{
    cameraPosition.x + (cameraForward.x * 2.0f),
    cameraPosition.y + (cameraForward.y * 2.0f),
    cameraPosition.z + (cameraForward.z * 2.0f)
  };

  auto toastObject = GameObject::New_ctor("FreeMemoryDoneToast");
  auto toastText = toastObject->AddComponent<TextMeshPro*>();

  auto font = BSML::Helpers::GetMainTextFont();
  auto configureDoneText = [font](TextMeshPro* textComponent, Color color) {
    if (font) {
      textComponent->set_font(font);
    }
    textComponent->set_text("DONE!");
    textComponent->set_alignment(TextAlignmentOptions::Center);
    textComponent->set_fontSize(12.0f);
    textComponent->set_color(color);
  };

  configureDoneText(toastText, Color::get_green());

  auto toastTransform = toastObject->get_transform();
  toastTransform->SetPositionAndRotation(toastPosition, cameraTransform->get_rotation());
  toastTransform->set_localScale(Vector3{0.1f, 0.1f, 0.1f});

  auto shadowObject = GameObject::New_ctor("FreeMemoryDoneToastShadow");
  auto shadowTransform = shadowObject->get_transform();
  shadowTransform->SetParent(toastTransform, false);
  shadowTransform->set_localPosition(Vector3{0.06f, -0.06f, 0.0f});

  auto shadowText = shadowObject->AddComponent<TextMeshPro*>();
  configureDoneText(shadowText, Color(0.0f, 0.0f, 0.0f, 0.9f));

  Object::Destroy(toastObject, 1.0f);
}

void RunFinalCleanupPass() {
  RunManagedGcSweep();
  TrimNativeHeap();
}

void CompleteCleanup() {
  cleanupRunning = false;
  ShowDoneToast();
  Logger.info("Best-effort memory cleanup complete.");
}

custom_types::Helpers::Coroutine RunMemoryCleanupCoroutine() {
  Logger.info("Running best-effort memory cleanup.");
  RunManagedGcSweep();

  auto unloadOperation = Resources::UnloadUnusedAssets();
  if (unloadOperation) {
    int waitedFrames = 0;
    while (!unloadOperation->get_isDone() && waitedFrames < kUnloadUnusedAssetsFrameLimit) {
      waitedFrames++;
      co_yield nullptr;
    }

    if (!unloadOperation->get_isDone()) {
      Logger.info("UnloadUnusedAssets exceeded frame wait limit; continuing cleanup.");
    }
  } else {
    Logger.info("UnloadUnusedAssets is unavailable; skipping.");
  }

  RunFinalCleanupPass();
  CompleteCleanup();
  co_return;
}

void OnFreeMemoryButtonPressed() {
  if (cleanupRunning) {
    Logger.info("Memory cleanup already running; ignoring duplicate request.");
    return;
  }

  Logger.info("Free Memory triggered from main menu.");
  cleanupRunning = true;

  auto starter = BSML::SharedCoroutineStarter::get_instance();
  if (!starter) {
    Logger.info("SharedCoroutineStarter is unavailable; running synchronous cleanup fallback.");
    RunFinalCleanupPass();
    CompleteCleanup();
    return;
  }

  starter->StartCoroutine(custom_types::Helpers::CoroutineHelper::New(RunMemoryCleanupCoroutine()));
}
}

/// @brief Called at the early stages of game loading
/// @param info The mod info. Update this with your mod's info.
MOD_EXPORT_FUNC void setup(CModInfo& info) {
  info = modInfo.to_c();
  Logger.info("Completed setup!");
}

/// @brief Called early on in the game loading
MOD_EXPORT_FUNC void load() {
  il2cpp_functions::Init();
}

/// @brief Called later on in the game loading
MOD_EXPORT_FUNC void late_load() {
  BSML::Init();
  BSML::Register::RegisterMenuButton("Free Memory!", "Free leaked memory", OnFreeMemoryButtonPressed);
}
