// SIAARA Viewer. Copyright (c) Sthraas Intelligence Pvt Ltd.

using Microsoft.Extensions.Logging;
using Siaara.Viewer.Core.Logging;
using MsLogLevel = Microsoft.Extensions.Logging.LogLevel;

namespace Siaara.Viewer.Host;

/// <summary>Routes ASP.NET Core's own logging into <see cref="AppLog"/>, so every line passes the PHI redactor.</summary>
internal sealed class RedactingLoggerProvider : ILoggerProvider
{
    public ILogger CreateLogger(string categoryName) => new Logger(categoryName);

    public void Dispose()
    {
    }

    private sealed class Logger(string category) : ILogger
    {
        public IDisposable? BeginScope<TState>(TState state) where TState : notnull => null;

        public bool IsEnabled(MsLogLevel logLevel) => logLevel >= MsLogLevel.Warning;

        public void Log<TState>(MsLogLevel logLevel, EventId eventId, TState state, Exception? exception, Func<TState, Exception?, string> formatter)
        {
            if (!IsEnabled(logLevel))
            {
                return;
            }
            Siaara.Viewer.Core.Logging.LogLevel level = logLevel >= MsLogLevel.Error ? Siaara.Viewer.Core.Logging.LogLevel.Error : Siaara.Viewer.Core.Logging.LogLevel.Warning;
            // Exception messages may carry request data; the type and the redacted message only.
            AppLog.Write(level, category, formatter(state, null) + (exception is null ? "" : " (" + exception.GetType().Name + ")"));
        }
    }
}
