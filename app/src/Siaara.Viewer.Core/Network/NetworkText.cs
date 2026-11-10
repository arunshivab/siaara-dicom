// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using System.Globalization;
using Siaara.Viewer.Core.Toolkit;

namespace Siaara.Viewer.Core.Network;

/// <summary>A network action cannot be done as asked; the message is plain words for the user (no patient data).</summary>
public sealed class NetworkException : Exception
{
    /// <summary>Creates the exception.</summary>
    public NetworkException()
    {
    }

    /// <summary>Creates the exception.</summary>
    /// <param name="message">Plain words.</param>
    public NetworkException(string message) : base(message)
    {
    }

    /// <summary>Creates the exception.</summary>
    /// <param name="message">Plain words.</param>
    /// <param name="innerException">Cause.</param>
    public NetworkException(string message, Exception innerException) : base(message, innerException)
    {
    }
}

/// <summary>Plain words for network failures. Nothing here ever includes a value from a data set or a query.</summary>
public static class NetworkText
{
    /// <summary>Plain words for a failed network call.</summary>
    /// <param name="e">The failure.</param>
    /// <returns>A sentence fragment without a full stop.</returns>
    public static string Describe(Exception e) => e switch
    {
        NetworkException n => n.Message,
        TkException { Error: TkError.Network } => "could not connect, or the connection was lost (check the address, the port and the firewall)",
        TkException { Error: TkError.Timeout } => "the other system did not answer in time",
        TkException { Error: TkError.Rejected } => "the other system refused the connection or the request (check both AE titles, and that this viewer is registered there)",
        TkException { Error: TkError.NotFound } => "the other system does not have what was asked for (check the address)",
        TkException { Error: TkError.InvalidArgument } => "a setting of this node is not valid",
        TkException { Error: TkError.Unsupported } => "the other system does not support this",
        TkException { Error: TkError.Truncated } => "the file was not read completely (truncated or damaged)",
        TkException { Error: TkError.LimitExceeded } => "the answer was larger than the limit",
        TkException tk => tk.Message,
        OperationCanceledException => "cancelled",
        Audit.AuditUnavailableException => "the audit log could not be written, so nothing was done",
        IOException => "file system error",
        UnauthorizedAccessException => "access denied",
        ObjectDisposedException => "the app is shutting down",
        _ => "unexpected error (" + e.GetType().Name + ")",
    };

    /// <summary>Plain words for a final DIMSE status that is not success.</summary>
    /// <param name="status">The status.</param>
    /// <returns>A sentence fragment.</returns>
    public static string DescribeStatus(int status)
    {
        string hex = status.ToString("X4", CultureInfo.InvariantCulture);
        string text = status switch
        {
            0 => "success",
            0x0122 => "the other system does not offer this service",
            0x0124 => "the other system says this viewer is not authorised",
            0x0110 => "the other system could not process the request",
            0xA700 => "the other system is out of resources",
            0xA701 or 0xA702 => "the other system could not carry out the transfer (out of resources)",
            0xA801 => "the other system does not know the destination AE title (register this viewer's receiver there)",
            0xA900 => "the other system says the request does not match the service",
            0xFE00 => "the transfer was cancelled",
            0xB000 => "some images could not be transferred",
            _ when (status & 0xF000) == 0xC000 => "the other system could not process the request",
            _ when (status & 0xF000) == 0xA000 => "the other system refused the request",
            _ when (status & 0xF000) == 0xB000 => "finished with warnings",
            _ => "the other system answered with an error",
        };
        return text + " (status " + hex + ")";
    }

    /// <summary>Plain words for a Storage Commitment failure reason.</summary>
    /// <param name="reason">Failure Reason (0008,1197).</param>
    /// <returns>Text.</returns>
    public static string DescribeCommitReason(int reason) => reason switch
    {
        0 => "confirmed",
        0xFFFF => "not reported",
        0x0110 => "processing failure",
        0x0112 => "the PACS does not have the image",
        0x0213 => "resource limitation",
        0x0122 => "image type not supported",
        0x0119 => "class and instance do not match",
        0x0131 => "duplicate transaction",
        _ => "failure reason " + reason.ToString("X4", CultureInfo.InvariantCulture),
    };
}
