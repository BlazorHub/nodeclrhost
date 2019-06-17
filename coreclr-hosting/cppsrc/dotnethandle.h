#ifndef __CORECLR_HOSTING_DOTNETHANDLE_H__
#define __CORECLR_HOSTING_DOTNETHANDLE_H__

#include <functional>

#include "jshandle.h"

namespace DotNetType {
typedef enum
{
    Undefined,
    Null,
    Boolean,
    Int32,
    Int64,
    Double,
    String, // Do we need encodings or do we assume UTF8?
    JsHandle, // A handle that was received from node
    Function
} Enum;
}

extern "C" struct DotNetHandle
{
    DotNetType::Enum type_;
    union {
        void* value_;
        JsHandle* jshandle_value_;
        char* string_value_;
        bool bool_value_;
        int32_t int32_value_;
        int64_t int64_value_;
        double double_value_;
        void (*function_value_)(int,JsHandle*, DotNetHandle&);
    };
    void (*release_func_)(DotNetHandle*);    

    void Release()
    {
        if (nullptr != release_func_)
            release_func_(this);        
    }

    Napi::Value ToValue(const Napi::Env& env, std::function<Napi::Function(DotNetHandle*)> function_factory)
    {
        if (type_ == DotNetType::Null)
            return env.Null();
        if (type_ == DotNetType::JsHandle)
            return jshandle_value_->ToValue(env);
        if (type_ == DotNetType::String)
            return Napi::String::New(env, string_value_); // TODO: string is copied, we could use char16_t to prevent a copy
        if (type_ == DotNetType::Boolean)
            return Napi::Boolean::New(env, bool_value_);
        if (type_ == DotNetType::Int32)
            return Napi::Number::New(env, int32_value_);
        if (type_ == DotNetType::Int64)
            return Napi::Number::New(env, int64_value_);
        if (type_ == DotNetType::Double)
            return Napi::Number::New(env, double_value_);

        if (type_ == DotNetType::Function)
        {
            return function_factory(this);
        }
        
        // TODO: Support other types
        return Napi::Value::Value();
    }
};

#endif