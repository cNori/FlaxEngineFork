using System.Globalization;
using System.Text.RegularExpressions;

namespace VETools.Text;

internal sealed record PreprocessorMacro(string Body, bool FunctionLike = false);

// Integer preprocessor expressions only; never evaluates C++ declarations or expands type macros.
internal static class ConditionalExpression
{
    readonly record struct Number(ulong Bits, bool Unsigned = false)
    {
        public long Signed => unchecked((long)Bits);
        public bool Truth => Bits != 0;
        public static Number Bool(bool value) => new(value ? 1UL : 0UL);
    }
    public static bool Evaluate(string expression, IReadOnlyDictionary<string, PreprocessorMacro> macros) => new Reader(expression, macros, []).Read().Truth;

    sealed class Reader
    {
        readonly List<string> tokens = [];
        readonly IReadOnlyDictionary<string, PreprocessorMacro> macros;
        readonly HashSet<string> expanding;
        int at;
        public Reader(string source, IReadOnlyDictionary<string, PreprocessorMacro> macros, HashSet<string> expanding)
        {
            this.macros = macros; this.expanding = expanding;
            tokens.AddRange(Expand(Lex(source), macros, expanding));
        }
        static List<string> Lex(string source)
        {
            List<string> result = [];
            foreach (Match match in Regex.Matches(source, @"\s+|/\*.*?\*/|//[^\r\n]*|0[xX][0-9a-fA-F']+[uUlL]*|0[bB][01']+[uUlL]*|[0-9][0-9']*[uUlL]*|[A-Za-z_]\w*|&&|\|\||<<|>>|<=|>=|==|!=|.", RegexOptions.Singleline))
            {
                string token = match.Value;
                if (string.IsNullOrWhiteSpace(token) || token.StartsWith("/*") || token.StartsWith("//")) continue;
                result.Add(token);
            }
            return result;
        }
        static List<string> Expand(List<string> source, IReadOnlyDictionary<string, PreprocessorMacro> macros, HashSet<string> disabled)
        {
            List<string> result = [];
            for (int i = 0; i < source.Count; i++)
            {
                string token = source[i];
                if (token == "defined")
                {
                    result.Add(token);
                    if (++i < source.Count && source[i] == "(") { result.Add(source[i]); if (++i < source.Count) result.Add(source[i]); if (++i < source.Count) result.Add(source[i]); }
                    else if (i < source.Count) result.Add(source[i]);
                    continue;
                }
                if (macros.TryGetValue(token, out var macro) && !macro.FunctionLike && disabled.Add(token))
                {
                    result.AddRange(Expand(Lex(macro.Body), macros, disabled)); disabled.Remove(token);
                }
                else result.Add(token);
            }
            return result;
        }
        string Peek => at < tokens.Count ? tokens[at] : "";
        string Next() => at < tokens.Count ? tokens[at++] : throw new ArgumentException("Missing preprocessor operand.");
        bool Take(string token) { if (Peek != token) return false; at++; return true; }
        void Require(string token) { if (!Take(token)) throw new ArgumentException("Expected '" + token + "' in preprocessor condition."); }
        public Number Read() { var value = Expression(0, true); if (at != tokens.Count) throw new ArgumentException("Unsupported preprocessor expression near '" + Peek + "'."); return value; }
        static int Priority(string op) => op switch { "||" => 1, "&&" => 2, "|" => 3, "^" => 4, "&" => 5, "==" or "!=" => 6, "<" or ">" or "<=" or ">=" => 7, "<<" or ">>" => 8, "+" or "-" => 9, "*" or "/" or "%" => 10, _ => -1 };
        Number Expression(int minimum, bool evaluate)
        {
            Number left = Unary(evaluate);
            while (Priority(Peek) >= minimum)
            {
                string op = Next(); int priority = Priority(op);
                bool rightActive = evaluate && !(op == "&&" && !left.Truth) && !(op == "||" && left.Truth);
                Number right = Expression(priority + 1, rightActive);
                if (evaluate) left = Binary(op, left, right);
                else left = new(0, op is "||" or "&&" or "==" or "!=" or "<" or ">" or "<=" or ">=" ? false : op is "<<" or ">>" ? left.Unsigned : left.Unsigned || right.Unsigned);
            }
            if (minimum == 0 && Take("?"))
            {
                var yes = Expression(0, evaluate && left.Truth); Require(":");
                var no = Expression(0, evaluate && !left.Truth);
                left = new(left.Truth ? yes.Bits : no.Bits, yes.Unsigned || no.Unsigned);
            }
            return evaluate ? left : new(0, left.Unsigned);
        }
        Number Unary(bool evaluate)
        {
            if (Take("!")) return Number.Bool(!Unary(evaluate).Truth);
            if (Take("~")) { var n = Unary(evaluate); return new(~n.Bits, n.Unsigned); }
            if (Take("+")) return Unary(evaluate);
            if (Take("-")) { var n = Unary(evaluate); return new(unchecked(0UL - n.Bits), n.Unsigned); }
            if (Take("(")) { var n = Expression(0, evaluate); Require(")"); return n; }
            string token = Next();
            if (token == "defined")
            {
                bool parens = Take("("); string name = Next();
                if (!Regex.IsMatch(name, @"^[A-Za-z_]\w*$")) throw new ArgumentException("defined requires a macro name.");
                if (parens) Require(")"); return Number.Bool(macros.ContainsKey(name));
            }
            if (token == "true") return new(1);
            if (token == "false") return default;
            if (char.IsDigit(token[0]))
            {
                bool unsigned = token.Contains('u') || token.Contains('U');
                string digits = token.TrimEnd('u', 'U', 'l', 'L').Replace("'", "");
                ulong bits = digits.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? ulong.Parse(digits[2..], NumberStyles.HexNumber, CultureInfo.InvariantCulture)
                    : digits.StartsWith("0b", StringComparison.OrdinalIgnoreCase) ? Convert.ToUInt64(digits[2..], 2)
                    : digits.Length > 1 && digits[0] == '0' ? Convert.ToUInt64(digits, 8) : ulong.Parse(digits, CultureInfo.InvariantCulture);
                return new(bits, unsigned || bits > long.MaxValue);
            }
            if (!Regex.IsMatch(token, @"^[A-Za-z_]\w*$")) throw new ArgumentException("Unsupported preprocessor token '" + token + "'.");
            if (!macros.TryGetValue(token, out var macro)) return default; // Undefined identifiers are zero, as in C++.
            if (macro.FunctionLike) { if (Peek == "(") throw new ArgumentException("Function-like macro '" + token + "' needs an explicit build definition for this condition."); return default; }
            return default; // Suppressed recursive object macros become ordinary identifiers (zero).
        }
        static Number Binary(string op, Number a, Number b)
        {
            if (op is "<<" or ">>" && (b.Signed < 0 || b.Bits >= 64)) throw new ArgumentException("Invalid preprocessor shift count.");
            bool unsigned = a.Unsigned || b.Unsigned;
            int compare = unsigned ? a.Bits.CompareTo(b.Bits) : a.Signed.CompareTo(b.Signed);
            return op switch
            {
                "||" => Number.Bool(a.Truth || b.Truth), "&&" => Number.Bool(a.Truth && b.Truth),
                "==" => Number.Bool(compare == 0), "!=" => Number.Bool(compare != 0), "<" => Number.Bool(compare < 0), ">" => Number.Bool(compare > 0), "<=" => Number.Bool(compare <= 0), ">=" => Number.Bool(compare >= 0),
                "|" => new(a.Bits | b.Bits, unsigned), "&" => new(a.Bits & b.Bits, unsigned), "^" => new(a.Bits ^ b.Bits, unsigned),
                "+" => new(unchecked(a.Bits + b.Bits), unsigned), "-" => new(unchecked(a.Bits - b.Bits), unsigned), "*" => new(unchecked(a.Bits * b.Bits), unsigned),
                "<<" => new(a.Bits << (int)b.Bits, a.Unsigned), ">>" => new(a.Unsigned ? a.Bits >> (int)b.Bits : unchecked((ulong)(a.Signed >> (int)b.Bits)), a.Unsigned),
                "/" when b.Bits != 0 => new(unsigned ? a.Bits / b.Bits : unchecked((ulong)(a.Signed / b.Signed)), unsigned),
                "%" when b.Bits != 0 => new(unsigned ? a.Bits % b.Bits : unchecked((ulong)(a.Signed % b.Signed)), unsigned),
                _ => throw new ArgumentException("Invalid arithmetic in preprocessor condition: " + op)
            };
        }
    }
}
