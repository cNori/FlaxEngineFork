using System.Globalization;
using System.Numerics;
using System.Text.RegularExpressions;
using static VETools.Tokenizer;

namespace VETools.Text;

// This is a bounded replacement pass, not a C++ constant-expression evaluator.
internal static class DefaultMacroExpansion
{
    public static void Apply(FileDescriptor file, Dictionary<string, PreprocessorMacro> initial,
        List<(int Line, string Name, PreprocessorMacro? Value)> changes)
    {
        var ordered = changes.OrderBy(c => c.Line).ToArray();
        int next = 0;
        var members = EnumMembers(file).Cast<DeclarationDescriptor>();
        foreach (var declaration in Fields(file).Cast<DeclarationDescriptor>().Concat(members).OrderBy(p => p.Name.line).ThenBy(p => p.Name.col))
        {
            int line = declaration.Name.line;
            while (next < ordered.Length && ordered[next].Line < line)
            {
                var change = ordered[next++];
                if (change.Value == null) initial.Remove(change.Name); else initial[change.Name] = change.Value;
            }
            if (declaration is EnumMemberDescriptor member)
            {
                member.ExpandedValueTokens = member.ValueTokens.Count == 0 ? null : Expand(member.ValueTokens, initial);
                continue;
            }
            var parameter = (FieldDescriptor)declaration;
            var tokens = parameter.Api.DefaultValueTokens ?? parameter.InitializerTokens;
            if (parameter.IsParameter && tokens.Any(t => !t.IsTrivia)) parameter.ExpandedInitializerTokens = Expand(tokens, initial);
            parameter.ExpandedArrayExtents.Clear();
            var declarator = parameter.DeclaratorTokens;
            for (int i = 0; i < declarator.Count; i++)
            {
                if (declarator[i] != "[") continue;
                int start = ++i, depth = 1;
                while (i < declarator.Count && depth != 0)
                {
                    if (declarator[i] == "[") depth++;
                    if (declarator[i] == "]") depth--;
                    if (depth != 0) i++;
                }
                if (depth != 0) break;
                parameter.ExpandedArrayExtents.Add(Expand(declarator.GetRange(start, i - start), initial));
            }
        }
    }

    static IEnumerable<EnumMemberDescriptor> EnumMembers(ScopeDescriptor scope)
    {
        foreach (var enumeration in scope.Declarations.OfType<EnumDescriptor>())
            foreach (var member in enumeration.Members) yield return member;
        foreach (var child in scope.Declarations.OfType<ScopeDescriptor>())
            foreach (var member in EnumMembers(child)) yield return member;
    }

    static IEnumerable<FieldDescriptor> Fields(ScopeDescriptor scope)
    {
        foreach (var field in scope.Fields) yield return field;
        foreach (var function in scope.Functions)
            foreach (var parameter in function.Fields) yield return parameter;
        foreach (var child in scope.Declarations.OfType<ScopeDescriptor>())
            foreach (var field in Fields(child)) yield return field;
    }

    public static List<Token>? Expand(IReadOnlyList<Token> input, IReadOnlyDictionary<string, PreprocessorMacro> macros)
    {
        int budget = 8192;
        try { return Visit(Clean(input), new HashSet<string>(StringComparer.Ordinal), 0); }
        catch (InvalidOperationException) { return null; }

        List<Token> Visit(List<Token> tokens, HashSet<string> active, int depth)
        {
            if (depth > 32) throw new InvalidOperationException();
            List<Token> result = [];
            for (int i = 0; i < tokens.Count; i++)
            {
                if (--budget < 0) throw new InvalidOperationException();
                var token = tokens[i];
                if (token.kind != Token.Kind.Identifier || !macros.TryGetValue(token.text, out var macro))
                { result.Add(token); continue; }
                if (!active.Add(token.text)) throw new InvalidOperationException();
                var replacement = Clean(Tokenizer.Tokenize(macro.Body));
                if (replacement.Any(t => t == "#" || t == "##")) throw new InvalidOperationException();
                if (macro.FunctionLike)
                {
                    // An uninvoked function-like macro is an ordinary identifier.
                    if (i + 1 >= tokens.Count || tokens[i + 1] != "(")
                    { active.Remove(token.text); result.Add(token); continue; }
                    int close = Closing(replacement, 0);
                    if (close < 0) throw new InvalidOperationException();
                    var names = replacement.Take(close).Skip(1).Where(t => t != ",").ToArray();
                    if (names.Any(t => t.kind != Token.Kind.Identifier) || names.Select(t => t.text).Distinct().Count() != names.Length)
                        throw new InvalidOperationException();
                    int end = Closing(tokens, i + 1);
                    if (end < 0) throw new InvalidOperationException();
                    List<List<Token>> args = [[]]; int level = 0;
                    for (int j = i + 2; j < end; j++)
                    {
                        var t = tokens[j];
                        if (t == "," && level == 0) args.Add([]);
                        else { args[^1].Add(t); if (t == "(") level++; else if (t == ")") level--; }
                    }
                    if (names.Length == 0 && args.Count == 1 && args[0].Count == 0) args.Clear();
                    if (args.Count != names.Length) throw new InvalidOperationException();
                    // Arguments are expanded before substitution, with the caller's hide set.
                    var caller = new HashSet<string>(active, StringComparer.Ordinal); caller.Remove(token.text);
                    var bindings = names.Select((n, index) => (n.text, Value: Visit(args[index], caller, depth + 1)))
                        .ToDictionary(p => p.text, p => p.Value, StringComparer.Ordinal);
                    replacement = replacement.Skip(close + 1).SelectMany(t =>
                        t.kind == Token.Kind.Identifier && bindings.TryGetValue(t.text, out var value) ? value : new List<Token> { t }).ToList();
                    i = end;
                }
                result.AddRange(Visit(replacement, active, depth + 1));
                active.Remove(token.text);
                if (result.Count > 8192) throw new InvalidOperationException();
            }
            return result;
        }
    }

    static List<Token> Clean(IEnumerable<Token> tokens) => tokens.Where(t => !t.IsTrivia && t.kind != Token.Kind.EndOfFile).ToList();
    static int Closing(IReadOnlyList<Token> tokens, int start)
    {
        if (start >= tokens.Count || tokens[start] != "(") return -1;
        int depth = 0;
        for (int i = start; i < tokens.Count; i++)
        { if (tokens[i] == "(") depth++; else if (tokens[i] == ")" && --depth == 0) return i; }
        return -1;
    }

    // Only fixed-width engine integer names have intrinsic cast semantics here.
    // Unknown casts and arithmetic remain subject to the existing converter warning.
    public static string Normalize(IReadOnlyList<Token> tokens)
    {
        var clean = Clean(tokens);
        return NormalizeText(string.Concat(clean.Select(t => t.text)), 0);
    }

    static string NormalizeText(string text, int depth)
    {
        if (depth > 32) return text;
        while (text.StartsWith('(') && MatchingEnd(text) == text.Length - 1) text = text[1..^1];
        var cast = Regex.Match(text, @"^\((?<type>u?int(?:8|16|32|64))\)(?<value>.+)$");
        if (cast.Success)
        {
            string value = NormalizeText(cast.Groups["value"].Value, depth + 1);
            if (!TryInteger(value, out var number)) return text;
            string type = cast.Groups["type"].Value;
            int bits = int.Parse(Regex.Match(type, @"\d+").Value, CultureInfo.InvariantCulture);
            if (type.StartsWith('u'))
            {
                var modulus = BigInteger.One << bits;
                number = ((number % modulus) + modulus) % modulus;
                return number.ToString(CultureInfo.InvariantCulture) + "U";
            }
            if (number < -(BigInteger.One << (bits - 1)) || number >= (BigInteger.One << (bits - 1))) return text;
            return number.ToString(CultureInfo.InvariantCulture);
        }
        var hex = Regex.Match(text, @"^0[xX](?<digits>[0-9a-fA-F]+)(?<suffix>[uU](?:[lL]|ll|LL)?|(?:[lL]|ll|LL)[uU]?)?$");
        if (hex.Success && BigInteger.TryParse("0" + hex.Groups["digits"].Value, NumberStyles.AllowHexSpecifier,
            CultureInfo.InvariantCulture, out var integer) && integer <= ulong.MaxValue)
        {
            bool unsigned = hex.Groups["suffix"].Value.Contains('u', StringComparison.OrdinalIgnoreCase) || integer > long.MaxValue ||
                (integer > int.MaxValue && integer <= uint.MaxValue);
            return integer.ToString(CultureInfo.InvariantCulture) + (unsigned ? "U" : "");
        }
        if (text.StartsWith("-(", StringComparison.Ordinal) || text.StartsWith("+(", StringComparison.Ordinal))
            return text[0] + NormalizeText(text[1..], depth + 1);
        return text;
    }

    static bool TryInteger(string text, out BigInteger value)
    {
        var match = Regex.Match(text, @"^(?<number>[+-]?(?:0|[1-9][0-9]*))(?<suffix>[uU](?:[lL]|ll|LL)?|(?:[lL]|ll|LL)[uU]?)?$");
        value = default;
        if (!match.Success || !BigInteger.TryParse(match.Groups["number"].Value, CultureInfo.InvariantCulture, out value)) return false;
        bool unsigned = match.Groups["suffix"].Value.Contains('u', StringComparison.OrdinalIgnoreCase);
        return value >= long.MinValue && value <= (unsigned ? new BigInteger(ulong.MaxValue) : new BigInteger(long.MaxValue)) && (!unsigned || value >= 0);
    }

    static int MatchingEnd(string text)
    {
        int depth = 0;
        for (int i = 0; i < text.Length; i++)
        { if (text[i] == '(') depth++; else if (text[i] == ')' && --depth == 0) return i; }
        return -1;
    }
}
