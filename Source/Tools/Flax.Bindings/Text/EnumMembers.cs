using static VETools.Tokenizer;

namespace VETools.Text;

// One member model is shared by annotation processing, name validation and
// emission. An annotation invocation is never treated as the enumerator name.
internal static class EnumMembers
{
    public static void Parse(FileDescriptor file)
    {
        void Visit(ScopeDescriptor scope)
        {
            foreach(var declaration in scope.Declarations)
            {
                if(declaration is EnumDescriptor enumeration) Read(enumeration,file);
                if(declaration is ScopeDescriptor child)Visit(child);
            }
        }
        Visit(file);
    }

    static void Read(EnumDescriptor owner,FileDescriptor file)
    {
        if(owner.Body.Count<2)return;
        List<Token> part=[];int round=0,square=0,curly=0,angle=0;
        var body=owner.Body.Skip(1).SkipLast(1).Append(new Token {kind=Token.Kind.ControlChar,text=","}).ToArray();
        for(int position=0;position<body.Length;++position)
        {
            var token=body[position];
            if(token=="," && round==0 && square==0 && curly==0 && angle==0) { Add(part);part=[];continue; }
            part.Add(token);
            if(round==0 && square==0 && curly==0)
            {
                if(token=="<" && (angle>0 || TemplateStart(body,position)))++angle;
                else if(token==">" && angle>0)--angle;
            }
            if(token=="(")round++;else if(token==")")round--;
            if(token=="[")square++;else if(token=="]")square--;
            if(token=="{")curly++;else if(token=="}")curly--;
        }
        if(part.Any(t=>!t.IsTrivia))Add(part);
        void Add(List<Token> raw)
        {
            var tokens=raw.Where(t=>!t.IsTrivia).ToList();if(tokens.Count==0)return;
            int index=0;
            bool Group(string open,string close)
            {
                int depth=0;
                do { if(tokens[index]==open)++depth;else if(tokens[index]==close)--depth;++index; }while(index<tokens.Count && depth>0);
                if(depth==0)return true;
                file.Errors.Add(new LogEntry($"Unclosed '{open}' in enum member declaration.",tokens[0]));return false;
            }
            bool Prefix()
            {
                if(index>=tokens.Count)return false;
                if(tokens[index]=="[") {Group("[","]");return true;}
                if(tokens[index]=="API_ENUM" || file.Converters.DeclarationMacros.ContainsKey(tokens[index].text))
                {
                    ++index;
                    if(index<tokens.Count && tokens[index]=="(")Group("(",")");
                    return true;
                }
                return false;
            }
            while(Prefix()) { }
            if(index>=tokens.Count || tokens[index].kind!=Token.Kind.Identifier)
            { file.Errors.Add(new LogEntry("Expected an enumerator name after its annotations.",tokens[0]));return; }
            var name=tokens[index++];
            EnumMemberDescriptor member=new() {Name=name,HeaderTokens=new(raw),Parent=owner,Access="public",NamespaceTokens=new(owner.NamespaceTokens),EnclosingTypeTokens=new(owner.EnclosingTypeTokens)};
            if(!string.IsNullOrEmpty(owner.Name.text))
            {
                if(member.EnclosingTypeTokens.Count>0)member.EnclosingTypeTokens.AddRange(Tokenizer.Tokenize("::").Where(t=>t.kind!=Token.Kind.EndOfFile));
                member.EnclosingTypeTokens.Add(owner.Name);
            }
            while(index<tokens.Count && tokens[index]=="[")Group("[","]");
            if(index<tokens.Count)
            {
                if(tokens[index]!="=")file.Errors.Add(new LogEntry("Expected '=' or the end of an enum member declaration.",tokens[index]));
                else
                {
                    member.ValueTokens=tokens.Skip(index+1).ToList();
                    if(member.ValueTokens.Count==0)file.Errors.Add(new LogEntry("Expected an expression after enum member '='.",tokens[index]));
                }
            }
            owner.Members.Add(member);
        }
    }

    static bool TemplateStart(Token[] tokens,int start)
    {
        int previous=start-1;while(previous>=0 && tokens[previous].IsTrivia)--previous;
        if(previous<0 || tokens[previous].kind!=Token.Kind.Identifier)return false;
        int angle=1,round=0;
        for(int i=start+1;i<tokens.Length;++i)
        {
            if(tokens[i]=="(")++round;else if(tokens[i]==")")--round;
            if(round!=0)continue;
            if(tokens[i]=="<")++angle;
            if(tokens[i]==">" && --angle==0)
            {
                int next=i+1;while(next<tokens.Length && tokens[next].IsTrivia)++next;
                return next<tokens.Length && tokens[next].text is ":" or "(" or "{";
            }
            if(tokens[i]==";")return false;
        }
        return false;
    }
}
