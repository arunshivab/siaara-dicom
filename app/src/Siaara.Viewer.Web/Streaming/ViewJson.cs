// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Text.Json.Serialization;
using Siaara.Viewer.Core.Viewing;

namespace Siaara.Viewer.Web.Streaming;

/// <summary>Source-generated JSON for the state message.</summary>
[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase, DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull)]
[JsonSerializable(typeof(ViewportStateMessage))]
internal sealed partial class ViewJsonContext : JsonSerializerContext;
