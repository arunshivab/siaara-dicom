// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Buffers.Binary;
using System.Text;
using Siaara.Viewer.Core.Viewing;

namespace Siaara.Viewer.Web.Streaming;

/// <summary>Message types of the /ws/view protocol.</summary>
public enum MessageType
{
    /// <summary>Unknown.</summary>
    None = 0,
    /// <summary>Frame, server to browser.</summary>
    Frame = 1,
    /// <summary>State, server to browser.</summary>
    State = 2,
    /// <summary>Ack, browser to server.</summary>
    Ack = 3,
    /// <summary>Input, browser to server.</summary>
    Input = 4,
    /// <summary>Command, browser to server.</summary>
    Command = 5,
}

/// <summary>A decoded browser message.</summary>
/// <param name="Type">Type.</param>
/// <param name="Viewport">Viewport.</param>
/// <param name="Seq">Sequence number (ack, input).</param>
/// <param name="Input">Input event (input messages).</param>
/// <param name="Json">UTF-8 JSON (commands).</param>
public readonly record struct BrowserMessage(MessageType Type, int Viewport, uint Seq, InputEvent Input, ReadOnlyMemory<byte> Json);

/// <summary>
/// The binary /ws/view protocol, little endian:
/// frame  u8 1, u8 format (1 RGBA, 2 gray8, 3 JPEG, 4 PNG), u16 viewport, u32 seq, u16 width, u16 height, pixels;
/// state  u8 2, u16 viewport, u32 seq, UTF-8 JSON;
/// ack    u8 3, u16 viewport, u32 seq;
/// input  u8 4, u16 viewport, u32 seq, u8 kind, i16 x, i16 y, u8 buttons, i16 wheel, u16 key, u8 modifiers, u16 w, u16 h;
/// command u8 5, u16 viewport, UTF-8 JSON.
/// </summary>
public static class FrameProtocol
{
    /// <summary>Largest message accepted or sent.</summary>
    public const int MaxMessageBytes = 64 * 1024 * 1024;

    /// <summary>Frame header size.</summary>
    public const int FrameHeaderBytes = 12;

    /// <summary>State header size.</summary>
    public const int StateHeaderBytes = 7;

    /// <summary>Ack size.</summary>
    public const int AckBytes = 7;

    /// <summary>Input size.</summary>
    public const int InputBytes = 22;

    /// <summary>Writes a frame header.</summary>
    /// <param name="dst">At least 12 bytes.</param>
    /// <param name="format">Format.</param>
    /// <param name="viewport">Viewport.</param>
    /// <param name="seq">Sequence number.</param>
    /// <param name="width">Width.</param>
    /// <param name="height">Height.</param>
    public static void WriteFrameHeader(Span<byte> dst, FramePixelFormat format, int viewport, uint seq, int width, int height)
    {
        dst[0] = (byte)MessageType.Frame;
        dst[1] = (byte)format;
        BinaryPrimitives.WriteUInt16LittleEndian(dst[2..], (ushort)viewport);
        BinaryPrimitives.WriteUInt32LittleEndian(dst[4..], seq);
        BinaryPrimitives.WriteUInt16LittleEndian(dst[8..], (ushort)width);
        BinaryPrimitives.WriteUInt16LittleEndian(dst[10..], (ushort)height);
    }

    /// <summary>Reads a frame header.</summary>
    /// <param name="src">Message.</param>
    /// <returns>Format, viewport, seq, width, height.</returns>
    public static (FramePixelFormat Format, int Viewport, uint Seq, int Width, int Height) ReadFrameHeader(ReadOnlySpan<byte> src)
    {
        if (src.Length < FrameHeaderBytes || src[0] != (byte)MessageType.Frame)
        {
            throw new FormatException("not a frame message");
        }
        return ((FramePixelFormat)src[1], BinaryPrimitives.ReadUInt16LittleEndian(src[2..]), BinaryPrimitives.ReadUInt32LittleEndian(src[4..]),
            BinaryPrimitives.ReadUInt16LittleEndian(src[8..]), BinaryPrimitives.ReadUInt16LittleEndian(src[10..]));
    }

    /// <summary>Builds a state message.</summary>
    /// <param name="viewport">Viewport.</param>
    /// <param name="seq">Sequence number.</param>
    /// <param name="utf8Json">JSON.</param>
    /// <returns>The message.</returns>
    public static byte[] State(int viewport, uint seq, ReadOnlySpan<byte> utf8Json)
    {
        var b = new byte[StateHeaderBytes + utf8Json.Length];
        b[0] = (byte)MessageType.State;
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(1), (ushort)viewport);
        BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(3), seq);
        utf8Json.CopyTo(b.AsSpan(StateHeaderBytes));
        return b;
    }

    /// <summary>Reads a state message.</summary>
    /// <param name="src">Message.</param>
    /// <returns>Viewport, seq, JSON text.</returns>
    public static (int Viewport, uint Seq, string Json) ReadState(ReadOnlySpan<byte> src)
    {
        if (src.Length < StateHeaderBytes || src[0] != (byte)MessageType.State)
        {
            throw new FormatException("not a state message");
        }
        return (BinaryPrimitives.ReadUInt16LittleEndian(src[1..]), BinaryPrimitives.ReadUInt32LittleEndian(src[3..]),
            Encoding.UTF8.GetString(src[StateHeaderBytes..]));
    }

    /// <summary>Builds an ack (browser side; used by tests and tools).</summary>
    /// <param name="viewport">Viewport.</param>
    /// <param name="seq">Sequence number.</param>
    /// <returns>The message.</returns>
    public static byte[] Ack(int viewport, uint seq)
    {
        var b = new byte[AckBytes];
        b[0] = (byte)MessageType.Ack;
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(1), (ushort)viewport);
        BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(3), seq);
        return b;
    }

    /// <summary>Builds an input message (browser side; used by tests and tools).</summary>
    /// <param name="e">Event.</param>
    /// <returns>The message.</returns>
    public static byte[] Input(InputEvent e)
    {
        var b = new byte[InputBytes];
        Span<byte> s = b;
        s[0] = (byte)MessageType.Input;
        BinaryPrimitives.WriteUInt16LittleEndian(s[1..], (ushort)e.Viewport);
        BinaryPrimitives.WriteUInt32LittleEndian(s[3..], e.Seq);
        s[7] = (byte)e.Kind;
        BinaryPrimitives.WriteInt16LittleEndian(s[8..], (short)Math.Clamp(e.X, short.MinValue, short.MaxValue));
        BinaryPrimitives.WriteInt16LittleEndian(s[10..], (short)Math.Clamp(e.Y, short.MinValue, short.MaxValue));
        s[12] = (byte)e.Buttons;
        BinaryPrimitives.WriteInt16LittleEndian(s[13..], (short)Math.Clamp(e.Wheel, short.MinValue, short.MaxValue));
        BinaryPrimitives.WriteUInt16LittleEndian(s[15..], (ushort)e.Key);
        s[17] = (byte)e.Modifiers;
        BinaryPrimitives.WriteUInt16LittleEndian(s[18..], (ushort)Math.Clamp(e.Width, 0, ushort.MaxValue));
        BinaryPrimitives.WriteUInt16LittleEndian(s[20..], (ushort)Math.Clamp(e.Height, 0, ushort.MaxValue));
        return b;
    }

    /// <summary>Builds a command message.</summary>
    /// <param name="viewport">Viewport.</param>
    /// <param name="json">JSON text.</param>
    /// <returns>The message.</returns>
    public static byte[] Command(int viewport, string json)
    {
        ArgumentNullException.ThrowIfNull(json);
        byte[] j = Encoding.UTF8.GetBytes(json);
        var b = new byte[3 + j.Length];
        b[0] = (byte)MessageType.Command;
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(1), (ushort)viewport);
        j.CopyTo(b.AsSpan(3));
        return b;
    }

    /// <summary>Decodes a browser message; null when malformed.</summary>
    /// <param name="msg">Bytes.</param>
    /// <returns>The message.</returns>
    public static BrowserMessage? Decode(ReadOnlyMemory<byte> msg)
    {
        ReadOnlySpan<byte> s = msg.Span;
        if (s.Length < 3)
        {
            return null;
        }
        int vp = BinaryPrimitives.ReadUInt16LittleEndian(s[1..]);
        switch ((MessageType)s[0])
        {
            case MessageType.Ack when s.Length >= AckBytes:
                return new BrowserMessage(MessageType.Ack, vp, BinaryPrimitives.ReadUInt32LittleEndian(s[3..]), default, default);
            case MessageType.Input when s.Length >= InputBytes:
                {
                    uint seq = BinaryPrimitives.ReadUInt32LittleEndian(s[3..]);
                    if (s[7] > (byte)InputKind.Resize)
                    {
                        return null;
                    }
                    var e = new InputEvent(vp, seq, (InputKind)s[7], BinaryPrimitives.ReadInt16LittleEndian(s[8..]), BinaryPrimitives.ReadInt16LittleEndian(s[10..]),
                        s[12], BinaryPrimitives.ReadInt16LittleEndian(s[13..]), BinaryPrimitives.ReadUInt16LittleEndian(s[15..]), (InputModifiers)s[17],
                        BinaryPrimitives.ReadUInt16LittleEndian(s[18..]), BinaryPrimitives.ReadUInt16LittleEndian(s[20..]));
                    return new BrowserMessage(MessageType.Input, vp, seq, e, default);
                }
            case MessageType.Command:
                return new BrowserMessage(MessageType.Command, vp, 0, default, msg[3..]);
            default:
                return null;
        }
    }
}
