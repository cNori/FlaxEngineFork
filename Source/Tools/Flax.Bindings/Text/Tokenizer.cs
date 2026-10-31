
using System.Diagnostics;

namespace VETools
{
    public static class Tokenizer
    {
        public sealed class Stream
        {
            public readonly List<Tokenizer.Token> Tokens;
            public int Index { get; private set; }

            public List<LogEntry> Errors = new();

            public Stream(List<Tokenizer.Token> tokens)
            {
                Tokens = tokens;
            }

            public Tokenizer.Token Peek(int offset = 0)
            {
                long i = (long)Index + offset;
                if (i < 0) i = 0;
                if (i >= Tokens.Count)
                {
                    if (Tokens.Count == 0)
                        return new Token { kind = Token.Kind.EndOfFile, text = string.Empty };
                    var end = Tokens[^1];
                    if (end.kind != Token.Kind.EndOfFile)
                    {
                        end.col += end.text.Length;
                        end.kind = Token.Kind.EndOfFile;
                        end.text = string.Empty;
                    }
                    return end;
                }
                return Tokens[(int)i];
            }

            public Tokenizer.Token Next()
            {
                var t = Peek(0);
                Advance();
                return t;
            }

            public void Advance(int count = 1)
            {
                Index = (int)Math.Clamp((long)Index + count, 0, Tokens.Count);
            }

            public bool IsEOF => Peek().kind == Tokenizer.Token.Kind.EndOfFile;

            // Collect trivia without losing it
            public List<Tokenizer.Token> ReadTrivia()
            {
                var trivia = new List<Tokenizer.Token>();

                while (true)
                {
                    var t = Peek();
                    if (t.IsTrivia)
                    {
                        trivia.Add(t);
                        Advance();
                        continue;
                    }
                    break;
                }

                return trivia;
            }

            public void SkipNoise()
            {
                while (true)
                {
                    var t = Peek();
                    if (t.kind == Tokenizer.Token.Kind.Whitespace || t.kind == Tokenizer.Token.Kind.NewLine)
                    {
                        Advance();
                        continue;
                    }
                    break;
                }
            }

            public bool TryExpect(Tokenizer.Token.Kind kind, out Tokenizer.Token token, string? text = null)
            {
                token = Peek();

                if (token.kind != kind || (text != null && token.text != text))
                {
                    Errors.Add(new LogEntry(
                        $"Expected {kind}{(text != null ? $" '{text}'" : "")}, got {token.kind} '{token.text}'",
                        token
                    ));
                    return false;
                }

                Advance();
                return true;
            }
        }
        public struct Token
        {
            public enum Kind
            {
                Invalid,
                Whitespace,
                NewLine,
                Comment,

                Identifier,
                Number,
                String,
                Character,
                ControlChar,
                EndOfFile,
            }

            public Kind kind;
            public string text;

            public int line;
            public int col;
            public bool IsTrivia => kind == Tokenizer.Token.Kind.Whitespace ||
                        kind == Tokenizer.Token.Kind.NewLine ||
                        kind == Tokenizer.Token.Kind.Comment;
            public override string ToString()
                => $"{kind} '{text}' ({line},{col})";

            public static bool operator ==(Token token, string text)
                => token.text == text;

            public static bool operator !=(Token token, string text)
                => token.text != text;

            public static bool operator ==(string text, Token token)
                => token == text;

            public static bool operator !=(string text, Token token)
                => token != text;

            public override bool Equals(object? obj)
                => obj is Token other
                    && kind == other.kind
                    && text == other.text
                    && line == other.line
                    && col == other.col;

            public override int GetHashCode()
                => HashCode.Combine(kind, text, line, col);
        }
        public static List<Token> Tokenize(string src)
        {
            List<Token> tokens = [];

            int line = 0;
            int col = 0;
            int i = 0;

            char Peek(int off = 0)
                => (i + off < src.Length) ? src[i + off] : '\0';

            void Advance()
            {
                if (i >= src.Length) return;

                if (src[i] == '\r' || src[i] == '\n')
                {
                    bool carriageReturn = src[i] == '\r';
                    i++;
                    if (carriageReturn && i < src.Length && src[i] == '\n')
                        i++;
                    line++;
                    col = 0;
                }
                else
                {
                    i++;
                    col++;
                }
            }

            void Add(Token.Kind kind, string text, int l, int c)
            {
                tokens.Add(new Token
                {
                    kind = kind,
                    text = text,
                    line = l,
                    col = c
                });
            }

            bool IsNewLine(char c)
            {
                return c == '\r' || c == '\n';
            }

            while (i < src.Length)
            {
                char ch = Peek();

                // Preserve newline spelling and advance once for CRLF, LF or CR.
                if (IsNewLine(ch))
                {
                    int l = line;
                    int c = col;
                    int start = i;
                    Advance();
                    Add(Token.Kind.NewLine, src.Substring(start, i - start), l, c);
                    continue;
                }
                // whitespace: group spaces/tabs
                if (ch == ' ' || ch == '\t')
                {
                    int l = line;
                    int c = col;

                    int start = i;
                    while (Peek() == ' ' || Peek() == '\t')
                        Advance();

                    string ws = src.Substring(start, i - start);
                    Add(Token.Kind.Whitespace, ws, l, c);
                    continue;
                }

                // single char symbols
                {
                    int l = line;
                    int c = col;

                    switch (ch)
                    {
                        case '(':
                        case ')':
                        case '{':
                        case '}':
                        case '[':
                        case ']':
                        case ',':
                        case ':':
                        case ';':
                        case '.':
                        case '=':
                        case '#':
                        case '<':
                        case '>':
                            Add(Token.Kind.ControlChar, ch.ToString(), l, c); Advance();
                            continue;

                    }
                }


                // Block comments may contain braces and span lines.
                if (ch == '/' && Peek(1) == '*')
                {
                    int l = line;
                    int c = col;
                    int start = i;
                    Advance();
                    Advance();
                    while (i < src.Length && !(Peek() == '*' && Peek(1) == '/'))
                        Advance();
                    bool closed = i < src.Length;
                    if (closed)
                    {
                        Advance();
                        Advance();
                    }
                    Add(closed ? Token.Kind.Comment : Token.Kind.Invalid, src.Substring(start, i - start), l, c);
                    continue;
                }
                // comment //
                if (ch == '/' && Peek(1) == '/')
                {
                    int l = line;
                    int c = col;

                    //Advance(); // '/'
                    //Advance(); // '/'

                    int start = i;
                    while (i < src.Length && !IsNewLine(Peek()))
                        Advance();

                    string txt = src.Substring(start, i - start);
                    Add(Token.Kind.Comment, txt, l, c);
                    continue;
                }

                // Literal prefixes belong to the literal token, including raw strings.
                int prefixLength = ch == 'u' && Peek(1) == '8' ? 2 : ch is 'u' or 'U' or 'L' ? 1 : 0;
                bool rawString = Peek(prefixLength) == 'R' && Peek(prefixLength + 1) == '"';
                int quoteOffset = rawString ? prefixLength + 1 : prefixLength;
                char quote = Peek(quoteOffset);
                if (rawString || quote == '"' || quote == '\'')
                {
                    int l = line;
                    int c = col;
                    int start = i;
                    for (int n = 0; n <= quoteOffset; n++)
                        Advance();
                    bool closed = false;
                    if (rawString)
                    {
                        int delimiterStart = i;
                        while (i < src.Length && Peek() != '(' && !char.IsWhiteSpace(Peek())
                            && Peek() != ')' && Peek() != '\\' && i - delimiterStart <= 16)
                            Advance();
                        if (Peek() == '(' && i - delimiterStart <= 16)
                        {
                            string ending = ")" + src.Substring(delimiterStart, i - delimiterStart) + "\"";
                            Advance();
                            while (i < src.Length && !src.AsSpan(i).StartsWith(ending))
                                Advance();
                            closed = i < src.Length;
                            if (closed)
                                for (int n = 0; n < ending.Length; n++)
                                    Advance();
                        }
                    }
                    else
                    {
                        while (i < src.Length && !IsNewLine(Peek()))
                        {
                            if (Peek() == quote)
                            {
                                Advance();
                                closed = true;
                                break;
                            }
                            if (Peek() == '\\')
                            {
                                Advance();
                                if (i < src.Length)
                                    Advance();
                            }
                            else
                                Advance();
                        }
                    }
                    Add(closed ? (quote == '\'' ? Token.Kind.Character : Token.Kind.String) : Token.Kind.Invalid,
                        src.Substring(start, i - start), l, c);
                    continue;
                }


                // identifier
                if (char.IsLetter(ch) || ch == '_')
                {
                    int l = line;
                    int c = col;

                    int start = i;
                    while (char.IsLetterOrDigit(Peek()) || Peek() == '_')
                        Advance();

                    string id = src.Substring(start, i - start);
                    Add(Token.Kind.Identifier, id, l, c);
                    continue;
                }

                // number (int/float, supports leading '-')
                if (char.IsDigit(ch) || (ch == '-' && char.IsDigit(Peek(1))))
                {
                    int l = line;
                    int c = col;

                    int start = i;
                    if (ch == '-') Advance();

                    bool dot = false;
                    while (true)
                    {
                        char p = Peek();
                        if (char.IsDigit(p))
                        {
                            Advance();
                        }
                        else if (p == '.' && !dot && char.IsDigit(Peek(1)))
                        {
                            dot = true;
                            Advance();
                        }
                        else break;
                    }

                    string num = src.Substring(start, i - start);
                    Add(Token.Kind.Number, num, l, c);
                    continue;
                }

                // invalid char fallback
                Add(Token.Kind.Invalid, ch.ToString(), line, col);
                Advance();
            }
            Add(Token.Kind.EndOfFile, string.Empty, line, col);
            return tokens;
        }
    }
}
