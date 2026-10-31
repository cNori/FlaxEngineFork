using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace VETools
{
    public class SourceFile
    {
        public class Line
        {
            public string Raw = "";
            public List<Tokenizer.Token> Tokens = [];
            public void Tokenizere()
            {
                Tokens = Tokenizer.Tokenize(Raw);
            }
        }
        public List<Line> Lines = [];
        public string? Path;
        public void Read(string path) 
        {
            var lines = System.IO.File.ReadLines(path);
            foreach (var line in lines)
            {
                var lineobj = new Line() { Raw = line };
                lineobj.Tokenizere();
                Lines.Add(lineobj);
            }
            Path = path;
        }
        public bool Write()
        {
            if(Path == null)
                return false;
            StringBuilder sb = new StringBuilder();
            foreach (var l in Lines)
            {
                sb.Append(l.Raw);
            }
            System.IO.File.WriteAllText(Path, sb.ToString());
            return true;
        }
    }
}
