namespace VETools;

[Flags]
public enum LogEntryType
{
    None = 0,
    Info = 1,
    Warning = 2,
    Error = 4,
    InvalidHandleError = 8,
}

public struct LogEntry
{
    public string Message;
    public Tokenizer.Token Where;
    public LogEntryType Type;
    public string Code;
    public string? File;

    public LogEntry(string message, Tokenizer.Token where, LogEntryType type = LogEntryType.Error, string code = "VETP001", string? file = null)
    {
        Message = message; Where = where; Type = type; Code = code; File = file;
    }

    public readonly bool IsInvalidHandleError => (Type & LogEntryType.InvalidHandleError) != 0;
    public readonly bool IsError => (Type & (LogEntryType.Error | LogEntryType.InvalidHandleError)) != 0;
    // InvalidHandleError is recoverable only by excluding the entire affected binding.
    // Combining it with Error explicitly makes the diagnostic fatal again.
    public readonly bool BlocksGeneration => (Type & LogEntryType.Error) != 0;
    public readonly bool IsWarning => (Type & LogEntryType.Warning) != 0;
    public readonly string Severity => IsError ? "error" : IsWarning ? "warning" : "info";
    public readonly string Format(string file) => $"{File ?? file}({Where.line + 1},{Where.col + 1}): {Severity} {Code}: {Message}";
    public override readonly string ToString() => $"{Severity} {Code}: {Message} at {Where.line + 1}:{Where.col + 1}";
}
