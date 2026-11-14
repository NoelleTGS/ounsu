// resource-extract: reads manifest resources straight from an assembly file (no loading).
//   list    <dll>...                         sizes per resource, largest first, and a total
//   extract <out-dir> <substitutions.xml> <dll>...
//       writes every non-".resources" resource to <out-dir>/<manifest name> and an ILLink
//       substitutions file removing exactly those from the NativeAOT image. Compiled
//       .resources tables (localisation strings) stay embedded.
using System.Reflection.Metadata;
using System.Reflection.PortableExecutable;
using System.Xml.Linq;

static IEnumerable<(string Assembly, string Name, byte[] Data)> Resources(string dll)
{
    using var pe = new PEReader(File.OpenRead(dll));
    var md = pe.GetMetadataReader();
    string assembly = md.GetString(md.GetAssemblyDefinition().Name);
    var dir = pe.PEHeaders.CorHeader!.ResourcesDirectory;
    int start = pe.PEHeaders.TryGetDirectoryOffset(dir, out int o) ? o : -1;
    var image = pe.GetEntireImage().GetContent();
    var list = new List<(string, string, byte[])>();
    foreach (var h in md.ManifestResources)
    {
        var r = md.GetManifestResource(h);
        if (!r.Implementation.IsNil) continue;   // lives in another file/assembly
        int at = start + (int)r.Offset;
        int len = BitConverter.ToInt32(image.AsSpan(at, 4));
        list.Add((assembly, md.GetString(r.Name), image.Slice(at + 4, len).ToArray()));
    }
    return list;
}

if (args.Length >= 2 && args[0] == "list")
{
    var all = args.Skip(1).SelectMany(Resources).ToList();
    foreach (var g in all.GroupBy(r => r.Assembly).OrderByDescending(g => g.Sum(r => (long)r.Data.Length)))
        Console.WriteLine($"{g.Sum(r => (long)r.Data.Length) / 1e6,8:F1} MB  {g.Count(),5} resources  {g.Key}");
    foreach (var r in all.OrderByDescending(r => r.Data.Length).Take(15))
        Console.WriteLine($"    {r.Data.Length / 1e6,7:F2} MB  {r.Name}");
    Console.WriteLine($"{all.Sum(r => (long)r.Data.Length) / 1e6:F1} MB total");
    return 0;
}

if (args.Length >= 4 && args[0] == "extract")
{
    string outDir = args[1];
    Directory.CreateDirectory(outDir);
    var linker = new XElement("linker");
    long bytes = 0;
    int files = 0;
    foreach (var group in args.Skip(3).SelectMany(Resources).GroupBy(r => r.Assembly))
    {
        var asm = new XElement("assembly", new XAttribute("fullname", group.Key));
        foreach (var r in group.Where(r => !r.Name.EndsWith(".resources", StringComparison.Ordinal)))
        {
            File.WriteAllBytes(Path.Combine(outDir, r.Name), r.Data);
            asm.Add(new XElement("resource", new XAttribute("name", r.Name), new XAttribute("action", "remove")));
            bytes += r.Data.Length;
            files++;
        }
        linker.Add(asm);
    }
    new XDocument(linker).Save(args[2]);
    Console.WriteLine($"extracted {files} resources ({bytes / 1e6:F1} MB) to {outDir}; substitutions: {args[2]}");
    return 0;
}

Console.Error.WriteLine("usage: resource-extract list <dll>... | extract <out-dir> <substitutions.xml> <dll>...");
return 2;
