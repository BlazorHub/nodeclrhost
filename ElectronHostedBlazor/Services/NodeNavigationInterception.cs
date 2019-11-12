// Copyright (c) .NET Foundation. All rights reserved.
// Licensed under the Apache License, Version 2.0. See License.txt in the project root for license information.
// Modified by Daniel Martin for nodeclrhost

using System.Threading.Tasks;
using Microsoft.AspNetCore.Components.Routing;

namespace BlazorApp.Services
{
    internal sealed class NodeNavigationInterception : INavigationInterception
    {
        public static readonly NodeNavigationInterception Instance = new NodeNavigationInterception();

        public Task EnableNavigationInterceptionAsync()
        {
            // TODO DM 19.08.2019: Implement
            //NodeJSRuntime.Instance.Invoke<object>(Interop.EnableNavigationInterception);
            var window = NodeJSRuntime.Instance.Host.Global.window;
            var blazor = window.Blazor._internal;
            blazor.navigationManager.enableNavigationInterception();
            return Task.CompletedTask;
        }
    }
}