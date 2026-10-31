using System.Runtime.InteropServices;
using System.Text;

namespace VETools;

// Local diagnostics only. This does not make a terminating/fatal exception recoverable.
internal static class CrashReporting
{
    static int installed;
    static readonly object gate = new();
    public static void Install()
    {
        if (Interlocked.Exchange(ref installed, 1) != 0) return;
        AppDomain.CurrentDomain.UnhandledException += (_, e) => Report(e.ExceptionObject, "Unhandled managed exception; terminating=" + e.IsTerminating);
        TaskScheduler.UnobservedTaskException += (_, e) => Report(e.Exception, "Unobserved task exception");
    }

    public static string? Report(object exception, string context = "Binding tool failure", string? path = null)
    {
        string detail;
        try { detail = exception.ToString() ?? "No exception details available."; }
        catch { detail = "Exception formatting failed."; }
        string text = $"[{DateTime.UtcNow:O}] {context}\nRuntime: {Environment.Version}; {RuntimeInformation.OSDescription}; thread {Environment.CurrentManagedThreadId}\n{detail}\n\n";
        try { Console.Error.WriteLine(text); } catch { }
        string[] paths = path == null ? [Path.Combine(Environment.CurrentDirectory, "VETools.crash.log"), Path.Combine(Path.GetTempPath(), "VETools.crash.log")] : [path];
        foreach (string candidate in paths)
        {
            try
            {
                lock (gate) File.AppendAllText(candidate, text, new UTF8Encoding(false));
                try { Console.Error.WriteLine("Exception details saved to: " + Path.GetFullPath(candidate)); } catch { }
                return Path.GetFullPath(candidate);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException) { }
        }
        return null;
    }
}
