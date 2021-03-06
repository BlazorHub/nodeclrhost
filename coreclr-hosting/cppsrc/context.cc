#include "context.h"

#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "nativeapi.h"

namespace {

Napi::Value Noop(const Napi::CallbackInfo& info) {
  return info.Env().Undefined();
}

}  // namespace

namespace coreclrhosting {

class Context::SynchronizedFinalizerCallback {
  Context* context_;
  std::shared_ptr<std::mutex> mutex_;
  std::function<void()> callback_;

 public:
  SynchronizedFinalizerCallback(Context* context,
                                std::function<void()> callback);
  void Call();
  void Cancel();
  static void Wrapper(napi_env env, void* finalize_data, void* finalize_hint);
};

Context::SynchronizedFinalizerCallback::SynchronizedFinalizerCallback(
    Context* context, std::function<void()> callback) {
  context_ = context;
  mutex_ = context->finalizer_mutex_;
  callback_ = callback;
  context->function_finalizers_.insert(this);
}

void Context::SynchronizedFinalizerCallback::Wrapper(napi_env env,
                                                     void* finalize_data,
                                                     void* finalize_hint) {
  auto data = (SynchronizedFinalizerCallback*)finalize_data;
  data->Call();
  delete data;
}

void Context::SynchronizedFinalizerCallback::Call() {
  std::lock_guard<std::mutex> lock(*mutex_);

  if (context_) {
    context_->function_finalizers_.erase(this);
    context_ = nullptr;
    callback_();
  }
}

void Context::SynchronizedFinalizerCallback::Cancel() { context_ = nullptr; }

thread_local Context* Context::ThreadInstance::thread_instance_;

Context::Context(std::unique_ptr<DotNetHost> dotnet_host, Napi::Env env)
    : env_(env),
      finalizer_mutex_(std::make_shared<std::mutex>()),
      host_(std::move(dotnet_host)),
      dotnet_thread_safe_callback_(Napi::ThreadSafeFunction::New(
          env, Napi::Function::New(env, Noop), "dotnet callback", 0, 1)),
      process_event_loop_(nullptr),
      function_factory_(
          std::bind(&Context::CreateFunction, this, std::placeholders::_1)),
      array_buffer_factory_(std::bind(&Context::CreateArrayBuffer, this,
                                      std::placeholders::_1)) {}
Context::~Context() {
  ThreadInstance _(this);
  dotnet_thread_safe_callback_.Release();
  std::lock_guard<std::mutex> lock(*finalizer_mutex_);
  for (auto finalizer_data : function_finalizers_) {
    finalizer_data->Cancel();
  }
  host_.reset(nullptr);  // Explicit reset while having thread instance set
}

void Context::RegisterSchedulerCallbacks(void (*process_event_loop)(void*),
                                         void (*process_micro_task)(void*)) {
  Napi::HandleScope handle_scope(env_);

  process_event_loop_ = [this, process_event_loop](Napi::Env env,
                                                   Napi::Function jsCallback,
                                                   void* data) {
    // jsCallback == Noop, so we do not call it
    ThreadInstance _(this);

    // Napi::HandleScope handle_scope(env_);
    // Napi::AsyncContext async_context(env_, "dotnet scheduler");

    // auto isolate = v8::Isolate::GetCurrent();

    // We use a new scope per callback, so microtasks can be run after
    // each callback (empty dotnet stack)
    {
      // Napi::CallbackScope cb_scope(env_, async_context);
      (*process_event_loop)(data);
    }

    // DM 25.04.2020: Running them manually was necessary when using libuv
    // directly.
    //                     Consider triggering a thread safe function to
    //                     process dotnet macro tasks
    // v8::MicrotasksScope::PerformCheckpoint(isolate); Does not run
    // microtasks
    // isolate->RunMicrotasks();
  };

  /*process_micro_task_ = [this,
                         process_micro_task](const Napi::CallbackInfo& f_info) {
    ThreadInstance _(this);
    (*process_micro_task)(f_info.Data());
    return f_info.Env().Undefined();
  };*/

  process_micro_task_ = Napi::Persistent(Napi::Function::New(
      env_,
      [this, process_micro_task](const Napi::CallbackInfo& f_info) {
        ThreadInstance _(this);
        (*process_micro_task)(nullptr);
        return f_info.Env().Undefined();
      },
      "invokeDotnetMicrotask"));

  auto global = env_.Global();
  auto queue_microtask = global.Get("queueMicrotask").As<Napi::Function>();
  signal_micro_task_ = Napi::Persistent(queue_microtask);
}

void Context::SignalEventLoopEntry(void* data) {
  auto acquire_status = dotnet_thread_safe_callback_.Acquire();
  if (acquire_status != napi_ok) return;

  // This will never block, as we used an unlimited queue
  dotnet_thread_safe_callback_.BlockingCall(data, process_event_loop_);

  dotnet_thread_safe_callback_.Release();
}

void Context::SignalMicroTask(void* data) {
  Napi::HandleScope handle_scope(env_);
  /*auto bound_func = Napi::Function::New(env_, process_micro_task_,
                                        "invokeDotnetMicrotask", data);
  signal_micro_task_.Value().Call(env_.Global(), {bound_func});*/
  signal_micro_task_.Value().Call(env_.Global(), {process_micro_task_.Value()});
}

extern "C" typedef DotNetHandle (*EntryPointFunction)(Context* context,
                                                      NativeApi nativeApi,
                                                      int argc,
                                                      const char* argv[]);

Napi::Value Context::RunCoreApp(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::Error::New(env, "Expected path to assembly as first argument")
        .ThrowAsJavaScriptException();
    return Napi::Value();
  }

  std::vector<std::string> arguments(info.Length());
  std::vector<const char*> arguments_c(info.Length());
  for (auto i = 0u; i < info.Length(); i++) {
    if (!info[i].IsString()) {
      Napi::Error::New(env, "Expected only string arguments")
          .ThrowAsJavaScriptException();
      return Napi::Value();
    }
    arguments_c[i] = (arguments[i] = info[i].ToString()).c_str();
  }

  std::unique_ptr<DotNetHost> host;

  auto result = DotNetHost::Create(info[0].ToString(), host);
  switch (result) {
    case DotNetHostCreationResult::kOK:
      break;
    case DotNetHostCreationResult::kAssemblyNotFound: {
      std::ostringstream stringStream;
      stringStream << "Could not find the assembly at: "
                   << (std::string)info[0].ToString();
      Napi::Error::New(env, stringStream.str()).ThrowAsJavaScriptException();
      return Napi::Value();
    }
    case DotNetHostCreationResult::kCoreClrNotFound:
      Napi::Error::New(env, "The coreclr could not be found")
          .ThrowAsJavaScriptException();
      return Napi::Value();
    case DotNetHostCreationResult::kInvalidCoreClr:
      Napi::Error::New(env,
                       "The coreclr found at base path is invalid. Probably "
                       "incompatible version")
          .ThrowAsJavaScriptException();
      return Napi::Value();
    case DotNetHostCreationResult::kInitializeFailed:
      Napi::Error::New(env, "Failed to initialize the coreclr runtime.")
          .ThrowAsJavaScriptException();
      return Napi::Value();
    default:
      Napi::Error::New(env, "Unexpected error while creating coreclr host.")
          .ThrowAsJavaScriptException();
      return Napi::Value();
  }

  // Set up context on current thread
  auto context = new Context(std::move(host), env);
  ThreadInstance _(context);

  // TODO DM 20.03.2020: Move into method on context
  auto entry_point_ptr = context->host_->GetManagedFunction(
      "NodeHostEnvironment.NativeHost.NativeEntryPoint, NodeHostEnvironment",
      "RunHostedApplication",
      "NodeHostEnvironment.NativeHost.EntryPointSignature, "
      "NodeHostEnvironment");
  auto entry_point = reinterpret_cast<EntryPointFunction>(entry_point_ptr);

  auto return_value = entry_point(context, NativeApi::instance_,
                                  arguments_c.size(), arguments_c.data())
                          .ToValue(env, context->function_factory_,
                                   context->array_buffer_factory_);

  if (return_value.IsPromise()) {
    return return_value.As<Napi::Promise>()
        .Get("then")
        .As<Napi::Function>()
        .MakeCallback(
            return_value,
            {Napi::Function::New(env,
                                 [context](const Napi::CallbackInfo& f_info) {
                                   delete context;
                                   return f_info[0];
                                 }),
             Napi::Function::New(env,
                                 [context](const Napi::CallbackInfo& r_info) {
                                   delete context;
                                   return r_info[0];
                                 })});
  }

  delete context;
  return return_value;
}

JsHandle Context::GetMember(JsHandle& owner_handle, const char* name) {
  if (!owner_handle.SupportsMembers())
    return JsHandle::Error("JsHandle does not support member-access");
  if (!IsActiveContext())
    return JsHandle::Error("Must be called on node thread");

  Napi::HandleScope handleScope(env_);

  auto owner = owner_handle.AsObject(env_);
  auto owner_object = owner.ToObject();
  auto result = owner_object.Get(name);
  if (env_.IsExceptionPending()) {
    return JsHandle::Error(env_.GetAndClearPendingException().Message());
  }

  return JsHandle::FromValue(result);
}

JsHandle Context::GetMemberByIndex(JsHandle& owner_handle, int index) {
  if (!owner_handle.SupportsMembers())
    return JsHandle::Error("JsHandle does not support member-access");
  if (!IsActiveContext())
    return JsHandle::Error("Must be called on node thread");

  Napi::HandleScope handleScope(env_);

  auto owner = owner_handle.AsObject(env_);
  auto owner_object = owner.ToObject();
  auto result = owner_object[index];
  if (env_.IsExceptionPending()) {
    return JsHandle::Error(env_.GetAndClearPendingException().Message());
  }

  return JsHandle::FromValue(result);
}

JsHandle Context::SetMember(JsHandle& owner_handle, const char* name,
                            DotNetHandle& dotnet_handle) {
  if (!owner_handle.SupportsMembers())
    return JsHandle::Error("JsHandle does not support member-access");
  if (!IsActiveContext())
    return JsHandle::Error("Must be called on node thread");

  Napi::HandleScope handleScope(env_);

  auto owner = owner_handle.AsObject(env_);
  auto owner_object = owner.ToObject();
  auto value =
      dotnet_handle.ToValue(env_, function_factory_, array_buffer_factory_);
  dotnet_handle.Release();
  owner_object.Set(name, value);
  return JsHandle::Undefined();
}
JsHandle Context::CreateObject(JsHandle& prototype_function, int argc,
                               DotNetHandle* argv) {
  if (!IsActiveContext())
    return JsHandle::Error("Must be called on node thread");

  Napi::HandleScope handleScope(env_);

  if (prototype_function.IsNotNullFunction()) {
    std::vector<napi_value> arguments(argc);
    for (int c = 0; c < argc; c++) {
      arguments[c] =
          argv[c].ToValue(env_, function_factory_, array_buffer_factory_);
      argv[c].Release();
    }

    napi_value result;
    auto status = napi_new_instance(env_, prototype_function.AsObject(env_),
                                    argc, arguments.data(), &result);
    if (status != napi_ok) {
      return JsHandle::Error("Could not create instance");
    }
    return JsHandle::FromValue(Napi::Value(env_, result));
  }

  auto newObj = Napi::Object::New(env_);

  return JsHandle::FromObject(newObj);
}

JsHandle Context::Invoke(JsHandle& handle, JsHandle& receiver_handle, int argc,
                         DotNetHandle* argv) {
  if (!IsActiveContext())
    return JsHandle::Error("Must be called on node thread");

  Napi::HandleScope handleScope(env_);

  auto value = handle.AsObject(env_);

  return InvokeIntern(value, receiver_handle.AsObject(env_), argc, argv);
}

JsHandle Context::Invoke(const char* name, JsHandle& receiver_handle, int argc,
                         DotNetHandle* argv) {
  if (!receiver_handle.SupportsMembers())
    return JsHandle::Error("JsHandle does not support member-access");
  if (!IsActiveContext())
    return JsHandle::Error("Must be called on node thread");

  Napi::HandleScope handleScope(env_);

  auto receiver_object = receiver_handle.AsObject(env_).ToObject();
  auto handle = receiver_object.Get(name);

  return InvokeIntern(handle, receiver_object, argc, argv);
}

JsHandle Context::InvokeIntern(Napi::Value handle, Napi::Value receiver,
                               int argc, DotNetHandle* argv) {
  if (env_.IsExceptionPending()) {
    return JsHandle::Error(env_.GetAndClearPendingException().Message());
  }
  if (!handle.IsFunction())
    return JsHandle::Error("JS object is not an invocable function");

  auto function = handle.As<Napi::Function>();

  std::vector<napi_value> arguments(argc);
  for (int c = 0; c < argc; c++) {
    arguments[c] =
        argv[c].ToValue(env_, function_factory_, array_buffer_factory_);
    argv[c].Release();
  }

  auto result = function.Call(receiver, arguments);
  if (env_.IsExceptionPending()) {
    return JsHandle::Error(env_.GetAndClearPendingException().Message());
  }
  return JsHandle::FromValue(result);
}

void Context::CompletePromise(napi_deferred deferred, DotNetHandle& handle) {
  Napi::HandleScope handleScope(env_);

  // TODO DM 29.11.2019: How to handle errors from napi calls here?
  if (handle.type_ == DotNetType::Exception) {
    auto error = Napi::Error::New(env_, handle.StringValue(env_));
    napi_reject_deferred(env_, deferred, error.Value());
  } else {
    napi_resolve_deferred(
        env_, deferred,
        handle.ToValue(env_, function_factory_, array_buffer_factory_));
  }
  handle.Release();
}

Napi::Function Context::CreateFunction(DotNetHandle* handle) {
  auto release_func = handle->release_func_;
  auto function_value = handle->function_value_;
  handle->release_func_ = nullptr;  // We delay the release

  // TODO: Check if using data instead of capture is better performancewise
  auto function = Napi::Function::New(
      env_, [this, function_value](const Napi::CallbackInfo& info) {
        ThreadInstance _(this);
        auto argc = info.Length();
        std::vector<JsHandle> arguments;
        for (size_t c = 0; c < argc; c++) {
          arguments.push_back(JsHandle::FromValue(info[c]));
        }

        DotNetHandle resultIntern;
        (*function_value)(argc, arguments.data(), resultIntern);

        auto napiResultValue = resultIntern.ToValue(
            info.Env(), this->function_factory_, this->array_buffer_factory_);
        resultIntern.Release();
        return napiResultValue;
      });

  auto finalizer_data = new SynchronizedFinalizerCallback(this, [=]() {
    release_func(DotNetType::Function, reinterpret_cast<void*>(function_value));
  });
  napi_add_finalizer(env_, function, static_cast<void*>(finalizer_data),
                     SynchronizedFinalizerCallback::Wrapper, nullptr, nullptr);

  return function;
}

Napi::Value Context::CreateArrayBuffer(DotNetHandle* handle) {
  auto value_copy = handle->value_;
  auto release_array_func = handle->release_func_;
  handle->release_func_ = nullptr;  // We delay the release

  auto finalizerData = new SynchronizedFinalizerCallback(
      this, [=]() { release_array_func(DotNetType::ByteArray, value_copy); });
  // We have a pointer to a struct { int32_t, void* } and interpret it like a
  // int32_t*
  auto array_value_ptr = reinterpret_cast<int32_t*>(value_copy);
  auto length = *array_value_ptr;
  return Napi::Buffer<uint8_t>::New(
      env_, *reinterpret_cast<uint8_t**>(array_value_ptr + 1), length,
      [](napi_env env, void* data, SynchronizedFinalizerCallback* hint) {
        hint->Call();
      },
      finalizerData);
}

}  // namespace coreclrhosting