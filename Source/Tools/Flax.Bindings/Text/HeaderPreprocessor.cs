using System.Text.RegularExpressions;

namespace VETools.Text;

internal static class HeaderPreprocessor
{
    sealed class Branch(bool parent, bool active, Tokenizer.Token where)
    {
        public bool Parent = parent, Active = active, Taken = active, Else;
        public Tokenizer.Token Where = where;
    }

    public static string Filter(string source, string path, Dictionary<string, PreprocessorMacro> macros,
        Action<string, Tokenizer.Token> include, List<LogEntry> diagnostics,
        Action<string, PreprocessorMacro?>? macroChanged = null,
        Action<string, PreprocessorMacro?, Tokenizer.Token>? macroAtChange = null)
    {
        char[] filtered = source.ToCharArray();
        var directiveLines = Tokenizer.Tokenize(source).Where(t => t.text == "#").Select(t => t.line).ToHashSet();
        Stack<Branch> branches = new();
        bool Active() => branches.Count == 0 || branches.Peek().Active;
        void Mask(int start, int end) { for (int i = start; i < end; i++) if (filtered[i] is not ('\r' or '\n')) filtered[i] = ' '; }
        void Error(string message, Tokenizer.Token where) => diagnostics.Add(new(message, where, LogEntryType.Error, "VETP006", path));
        bool Condition(string text, Tokenizer.Token where)
        {
            try { return ConditionalExpression.Evaluate(text, macros); }
            catch (Exception error) when (error is ArgumentException or FormatException or OverflowException or DivideByZeroException)
            { Error("Cannot evaluate preprocessor condition '" + text + "': " + error.Message, where); return false; }
        }
        int start = 0, line = 0;
        while (start < source.Length)
        {
            int end = source.IndexOf('\n', start); end = end < 0 ? source.Length : end + 1;
            int lines = 1;
            string physical = source[start..end];
            bool directive = directiveLines.Contains(line) && Regex.IsMatch(physical, @"^[ \t]*#");
            if (directive)
            {
                while (source[start..end].TrimEnd('\r', '\n').EndsWith('\\') && end < source.Length)
                { int next = source.IndexOf('\n', end); end = next < 0 ? source.Length : next + 1; lines++; }
                string logical = source[start..end].Replace("\\\r\n", "").Replace("\\\n", "");
                var match = Regex.Match(logical, @"^[ \t]*#[ \t]*(?<name>[A-Za-z_]\w*)\b(?<body>.*)", RegexOptions.Singleline);
                string name = match.Groups["name"].Value, body = match.Groups["body"].Value.Trim();
                var where = new Tokenizer.Token { line = line, col = 0, text = "#" + name };
                bool wasActive = Active();
                switch (name)
                {
                    case "if": case "ifdef": case "ifndef":
                        bool condition = false;
                        if (wasActive)
                        {
                            string symbol = Regex.Match(body, @"^[A-Za-z_]\w*").Value;
                            if (name != "if" && symbol.Length == 0) Error("#" + name + " requires a macro name.", where);
                            else condition = name == "if" ? Condition(body, where) : name == "ifdef" ? macros.ContainsKey(symbol) : !macros.ContainsKey(symbol);
                        }
                        branches.Push(new(wasActive, wasActive && condition, where)); Mask(start, end); break;
                    case "elif":
                        if (branches.Count == 0) Error("#elif without #if.", where);
                        else
                        {
                            var branch = branches.Peek();
                            if (branch.Else) Error("#elif after #else.", where);
                            branch.Active = branch.Parent && !branch.Taken && !branch.Else && Condition(body, where);
                            branch.Taken |= branch.Active;
                        }
                        Mask(start, end); break;
                    case "else":
                        if (branches.Count == 0) Error("#else without #if.", where);
                        else { var branch = branches.Peek(); if (branch.Else) Error("Duplicate #else.", where); branch.Active = branch.Parent && !branch.Taken && !branch.Else; branch.Else = true; branch.Taken = true; }
                        Mask(start, end); break;
                    case "endif":
                        if (branches.Count == 0) Error("#endif without #if.", where); else branches.Pop();
                        Mask(start, end); break;
                    default:
                        if (!wasActive) { Mask(start, end); break; }
                        if (name == "define")
                        {
                            var definition = Regex.Match(body, @"^(?<name>[A-Za-z_]\w*)(?<rest>.*)$", RegexOptions.Singleline);
                            if (definition.Success)
                            {
                                string rest = definition.Groups["rest"].Value;
                                bool function = rest.StartsWith('(');
                                string symbol = definition.Groups["name"].Value;
                                macros[symbol] = new(rest.Trim(), function);
                                macroChanged?.Invoke(symbol, macros[symbol]);
                                macroAtChange?.Invoke(symbol, macros[symbol], where);
                            }
                        }
                        else if (name == "undef")
                        {
                            string symbol = Regex.Match(body, @"^[A-Za-z_]\w*").Value;
                            macros.Remove(symbol); macroChanged?.Invoke(symbol, null);
                            macroAtChange?.Invoke(symbol, null, where);
                        }
                        else if (name == "include") include(body, where);
                        else if (name == "error") Error("Active #error: " + body, where);
                        else if (name == "warning") diagnostics.Add(new(body, where, LogEntryType.Warning, "VETP006", path));
                        break;
                }
            }
            else if (!Active()) Mask(start, end);
            start = end; line += lines;
        }
        foreach (var branch in branches) Error("Unclosed preprocessor conditional.", branch.Where);
        return new string(filtered);
    }
}
