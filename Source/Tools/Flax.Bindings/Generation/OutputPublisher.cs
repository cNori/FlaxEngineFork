using System.Text;

namespace VETools.Generation;

// Exception-safe publication of one generation. Replacement is atomic per file;
// a process crash/power loss between files is not an atomic multi-file commit.
internal static class OutputPublisher
{
    internal const string CleanupWarningsKey="VET.OutputCleanupWarnings";
    internal sealed record Output(string Path, string Content);
    sealed class Pending(string path, string content)
    {
        public readonly string Path=path, Content=content;
        public readonly string Stage=path+".vet-"+Guid.NewGuid().ToString("N")+".tmp";
        public readonly string Backup=path+".vet-"+Guid.NewGuid().ToString("N")+".bak";
        public bool Existed, Published;
    }

    public static IReadOnlyList<string> Write(IReadOnlyList<Output> outputs, Action<int>? beforeCommit=null)
    {
        if(outputs.Count==0)throw new ArgumentException("No generated outputs supplied.");
        var pending=outputs.Select(o=>new Pending(System.IO.Path.GetFullPath(o.Path),o.Content)).ToArray();
        if(pending.Select(p=>p.Path).Distinct(StringComparer.OrdinalIgnoreCase).Count()!=pending.Length)
            throw new ArgumentException("Generated output paths must be distinct.");
        var targets=pending.Select(p=>p.Path).ToHashSet(StringComparer.OrdinalIgnoreCase);
        if(pending.Any(p=>targets.Contains(p.Path+".vet-output.lock")))
            throw new ArgumentException("Generated output collides with another output's reservation path.");
        List<FileStream> reservations=[];
        List<string> warnings=[];
        bool keepRecovery=false;
        try
        {
            // Sidecar reservations coordinate concurrent instances of this writer.
            foreach(var item in pending.OrderBy(p=>p.Path,StringComparer.OrdinalIgnoreCase))
            {
                Directory.CreateDirectory(System.IO.Path.GetDirectoryName(item.Path)!);
                // Persistent lock files avoid unlink/recreate races and never delete
                // an existing file merely because it has the reservation spelling.
                reservations.Add(new FileStream(item.Path+".vet-output.lock",FileMode.OpenOrCreate,FileAccess.ReadWrite,FileShare.None,1,FileOptions.None));
                if(Directory.Exists(item.Path))throw new IOException("Generated output is a directory: "+item.Path);
                item.Existed=File.Exists(item.Path);
                if(item.Existed)
                {
                    // Reject an already locked/read-only destination before changing either file.
                    using var probe=new FileStream(item.Path,FileMode.Open,FileAccess.ReadWrite,FileShare.None);
                }
            }
            foreach(var item in pending)
            {
                using var stream=new FileStream(item.Stage,FileMode.CreateNew,FileAccess.Write,FileShare.None);
                using(var writer=new StreamWriter(stream,new UTF8Encoding(false,true),4096,leaveOpen:true))writer.Write(item.Content);
                stream.Flush(flushToDisk:true);
            }
            for(int index=0;index<pending.Length;index++)
            {
                var item=pending[index];
                beforeCommit?.Invoke(index);
                if(item.Existed)File.Replace(item.Stage,item.Path,item.Backup);
                else File.Move(item.Stage,item.Path);
                item.Published=true;
            }
        }
        catch(Exception failure)
        {
            keepRecovery=true;
            List<Exception> rollbackFailures=[];
            foreach(var item in pending.Reverse())
            {
                try
                {
                    // Check backups even if replacement itself threw after creating one.
                    if(File.Exists(item.Backup))
                    {
                        if(File.Exists(item.Path))File.Replace(item.Backup,item.Path,null);
                        else File.Move(item.Backup,item.Path);
                    }
                    else if(item.Published && !item.Existed)File.Delete(item.Path);
                }
                catch(Exception rollback)
                { rollbackFailures.Add(new IOException("Cannot restore generated output: "+item.Path,rollback)); }
            }
            if(rollbackFailures.Count!=0)
            {
                keepRecovery=true;
                string paths=string.Join(", ",pending.Where(p=>File.Exists(p.Backup)).Select(p=>p.Backup));
                var error=new IOException("Output publication failed and rollback was incomplete. Recovery backups retained: "+paths,
                    new AggregateException(new[]{failure}.Concat(rollbackFailures)));
                error.Data[CleanupWarningsKey]=warnings;
                throw error;
            }
            keepRecovery=false;
            failure.Data[CleanupWarningsKey]=warnings;
            throw;
        }
        finally
        {
            foreach(var item in pending)
            {
                if(!keepRecovery) { Cleanup(item.Stage); Cleanup(item.Backup); }
            }
            foreach(var reservation in reservations)
            {
                try { reservation.Dispose(); }
                catch(Exception error) when(error is IOException or UnauthorizedAccessException) { warnings.Add("Cannot close output reservation: "+error.Message); }
            }
        }
        return warnings;

        void Cleanup(string path)
        {
            try { File.Delete(path); }
            catch(Exception error) when(error is IOException or UnauthorizedAccessException)
            { warnings.Add("Cannot remove generated-output temporary '"+path+"': "+error.Message); }
        }
    }
}
