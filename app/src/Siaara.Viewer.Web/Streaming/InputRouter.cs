// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Globalization;
using System.Text.Json;
using Siaara.Viewer.Core.Viewing;

namespace Siaara.Viewer.Web.Streaming;

/// <summary>Routes decoded browser messages to a <see cref="ViewerSession"/>.</summary>
public static class InputRouter
{
    private static readonly Dictionary<string, ViewerTool> s_tools = new(StringComparer.OrdinalIgnoreCase)
    {
        ["wl"] = ViewerTool.WindowLevel,
        ["zoom"] = ViewerTool.Zoom,
        ["pan"] = ViewerTool.Pan,
        ["scroll"] = ViewerTool.Scroll,
        ["length"] = ViewerTool.Length,
        ["polyline"] = ViewerTool.Polyline,
        ["angle"] = ViewerTool.Angle,
        ["cobb"] = ViewerTool.Cobb,
        ["ellipse"] = ViewerTool.Ellipse,
        ["circle"] = ViewerTool.Circle,
        ["rect"] = ViewerTool.Rectangle,
        ["polygon"] = ViewerTool.Polygon,
        ["freehand"] = ViewerTool.Freehand,
        ["probe"] = ViewerTool.Probe,
        ["calibrate"] = ViewerTool.Calibrate,
    };

    /// <summary>Tool by name.</summary>
    /// <param name="name">Name.</param>
    /// <param name="tool">Tool.</param>
    /// <returns>True when known.</returns>
    public static bool TryParseTool(string? name, out ViewerTool tool) => s_tools.TryGetValue(name ?? "", out tool);

    /// <summary>Name of a tool.</summary>
    /// <param name="tool">Tool.</param>
    /// <returns>Name.</returns>
    public static string ToolName(ViewerTool tool) => s_tools.First(kv => kv.Value == tool).Key;

    /// <summary>Dispatches one message.</summary>
    /// <param name="session">Session.</param>
    /// <param name="msg">Message.</param>
    public static void Dispatch(ViewerSession session, BrowserMessage msg)
    {
        ArgumentNullException.ThrowIfNull(session);
        switch (msg.Type)
        {
            case MessageType.Ack:
                session.Ack(msg.Viewport, msg.Seq);
                break;
            case MessageType.Input:
                session.OnInput(msg.Input);
                break;
            case MessageType.Command:
                Command(session, msg.Viewport, msg.Json);
                break;
        }
    }

    private static void Command(ViewerSession session, int viewport, ReadOnlyMemory<byte> json)
    {
        JsonDocument doc;
        try
        {
            doc = JsonDocument.Parse(json);
        }
        catch (JsonException)
        {
            return;
        }
        using (doc)
        {
            JsonElement r = doc.RootElement;
            if (r.ValueKind != JsonValueKind.Object)
            {
                return;
            }
            if (r.TryGetProperty("activate", out JsonElement act) && act.TryGetInt32(out int a))
            {
                session.Activate(a);
            }
            if (r.TryGetProperty("tool", out JsonElement t) && TryParseTool(t.GetString(), out ViewerTool tool))
            {
                session.SetTool(tool);
            }
            if (r.TryGetProperty("preset", out JsonElement p) && p.TryGetInt32(out int n))
            {
                session.ApplyPreset(n);
            }
            if (r.TryGetProperty("layout", out JsonElement l) && LayoutModel.TryParse(l.GetString(), out LayoutModel layout))
            {
                session.SetLayout(layout);
            }
            if (r.TryGetProperty("action", out JsonElement ac) && ac.GetString() is string action)
            {
                session.RunAction(action);
            }
            if (r.TryGetProperty("fps", out JsonElement f) && f.TryGetDouble(out double fps))
            {
                session.SetCineFps(fps);
            }
            if (r.TryGetProperty("dpr", out JsonElement d) && d.TryGetDouble(out double dpr))
            {
                session.SetDevicePixelRatio(dpr);
            }
            if (r.TryGetProperty("series", out JsonElement s) && s.TryGetInt32(out int si))
            {
                session.AssignSeries(viewport, si);
            }
            if (r.TryGetProperty("calibrate", out JsonElement c) && c.ValueKind == JsonValueKind.Object
                && c.TryGetProperty("id", out JsonElement cid) && cid.GetString() is string id)
            {
                double mm = c.TryGetProperty("mm", out JsonElement m) && m.ValueKind == JsonValueKind.Number ? m.GetDouble()
                    : c.TryGetProperty("mm", out m) && m.ValueKind == JsonValueKind.String
                      && double.TryParse(m.GetString(), NumberStyles.Float, CultureInfo.InvariantCulture, out double parsed) ? parsed : 0;
                _ = session.CalibrateAsync(id, mm);
            }
        }
    }
}
