#include "RNMultithreadingInstaller.h"
#include "ThreadPool.h"
#include <RNReanimated/Scheduler.h>
#include <RNReanimated/ShareableValue.h>
#include <RNReanimated/RuntimeManager.h>
#include <RNReanimated/RuntimeDecorator.h>
#include <RNReanimated/ErrorHandler.h>
#include "MakeJSIRuntime.h"

#define MAX_THREAD_COUNT 2

namespace mrousavy {
namespace multithreading {

static ThreadPool pool(1);
static std::unique_ptr<reanimated::RuntimeManager> manager;

//reanimated::RuntimeManager manager;

void install(jsi::Runtime& runtime,
             std::function<std::shared_ptr<reanimated::Scheduler>()> makeScheduler,
             std::function<std::shared_ptr<reanimated::ErrorHandler>(std::shared_ptr<reanimated::Scheduler>)> makeErrorHandler) {
  // Quickly setup the runtime - this is executed in parallel, and _might_ introduce race conditions if spawnThread is called before this finishes.
  pool.enqueue([makeScheduler, makeErrorHandler]() {
    auto runtime = makeJSIRuntime();
    reanimated::RuntimeDecorator::decorateRuntime(*runtime, "CUSTOM_THREAD");
    auto scheduler = makeScheduler();
    manager = std::make_unique<reanimated::RuntimeManager>(std::move(runtime),
                                                           makeErrorHandler(scheduler),
                                                           scheduler);
  });
  
  // spawnThread(run: () => T): Promise<T>
  auto spawnThread = jsi::Function::createFromHostFunction(runtime,
                                                           jsi::PropNameID::forAscii(runtime, "spawnThread"),
                                                           1,  // run
                                                           [](jsi::Runtime& runtime, const jsi::Value& thisValue, const jsi::Value* arguments, size_t count) -> jsi::Value {
    if (!arguments[0].isObject())
      throw jsi::JSError(runtime, "spawnThread: First argument has to be a function!");
    
    auto worklet = reanimated::ShareableValue::adapt(runtime, arguments[0], manager.get());
    
    auto spawnThreadCallback = jsi::Function::createFromHostFunction(runtime,
                                                                     jsi::PropNameID::forAscii(runtime, "spawnThreadCallback"),
                                                                     2,
                                                                     [worklet](jsi::Runtime& runtime, const jsi::Value& thisValue, const jsi::Value* arguments, size_t count) -> jsi::Value {
      auto resolverValue = std::make_shared<jsi::Value>((arguments[0].asObject(runtime)));
      auto rejecterValue = std::make_shared<jsi::Value>((arguments[1].asObject(runtime)));
      
      auto resolver = [&runtime, resolverValue](jsi::Value value) {
        manager->scheduler->scheduleOnJS([&runtime, resolverValue, &value] () {
          resolverValue->asObject(runtime).asFunction(runtime).call(runtime, value);
        });
      };
      auto rejecter = [&runtime, rejecterValue](std::string message) {
        manager->scheduler->scheduleOnJS([&runtime, rejecterValue, message] () {
          rejecterValue->asObject(runtime).asFunction(runtime).call(runtime, jsi::JSError(runtime, message).value());
        });
      };
      
      pool.enqueue([resolver, rejecter, worklet]() {
        try {
          auto& runtime = *manager->runtime;
          auto function = worklet->getValue(runtime).asObject(runtime).asFunction(runtime);
          auto result = function.getFunction(runtime).callWithThis(runtime, function);
          
          // TODO: Copy over result to other runtime to make it thread-safe
          resolver(jsi::Value(42));
        } catch (std::exception& exc) {
          rejecter(exc.what());
        }
      });
      return jsi::Value::undefined();
    });
    
    auto newPromise = runtime.global().getProperty(runtime, "Promise");
    auto promise = newPromise
                      .asObject(runtime)
                      .asFunction(runtime)
                      .callAsConstructor(runtime, spawnThreadCallback);
    
    return promise;
  });
  runtime.global().setProperty(runtime, "spawnThread", std::move(spawnThread));
}

} // namespace multithreading
} // namespace mrousavy
