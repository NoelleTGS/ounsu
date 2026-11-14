using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;

// NativeAOT probe for the lx host on kaiHEN.
// Each stage logs before it runs (via raw write(2), which avoids .NET's console
// setup), so the last "stage N" line in the nxlink log says where it died.

interface IShape { double Area(); }
sealed class Sq : IShape { public double S; public double Area() => S * S; }
sealed class Ci : IShape { public double R; public double Area() => Math.PI * R * R; }

static unsafe partial class Program
{
    [LibraryImport("libc", EntryPoint = "write")]
    private static partial nint write(int fd, byte* buf, nint count);

    static void Log(string s)
    {
        var bytes = System.Text.Encoding.UTF8.GetBytes("aot-hello: " + s + "\n");
        fixed (byte* p = bytes) write(2, p, bytes.Length);
    }

    [ThreadStatic] static int tls;
    static readonly object[] ring = new object[1024];

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    delegate int Comparison(void* a, void* b);

    [DllImport("libc")]
    static extern void qsort(void* base_, nuint count, nuint size, IntPtr compare);

    static int Main()
    {
        Log("stage 0: managed Main reached");

        Log("stage 1: virtual/interface dispatch");
        var shapes = new List<IShape>();
        for (int i = 0; i < 100000; i++) shapes.Add(i % 2 == 0 ? new Sq { S = i } : new Ci { R = i });
        double sum = 0;
        foreach (var s in shapes) sum += s.Area();
        Log($"stage 1: ok, sum={sum:E3}");

        Log("stage 2: GC.Collect");
        shapes = null!;
        GC.Collect();
        GC.WaitForPendingFinalizers();
        Log($"stage 2: ok, heap={GC.GetTotalMemory(false)} collections={GC.CollectionCount(0)}");

        Log("stage 3: allocation pressure (~200 MiB churn, forces GCs)");
        // Kept in a ring so they escape: .NET 10 would put non-escaping small arrays on the stack.
        long kept = 0;
        int gen0Before = GC.CollectionCount(0);
        var t3 = Environment.TickCount64;
        for (int i = 0; i < 200_000; i++) { var b = new byte[1024]; ring[i & (ring.Length - 1)] = b; kept += b.Length; }
        Log($"stage 3: ok, {kept >> 20} MiB in {Environment.TickCount64 - t3} ms, gen0 collections +{GC.CollectionCount(0) - gen0Before}, " +
            $"heap {GC.GetTotalMemory(false) >> 20} MiB");

        Log("stage 4: threads + [ThreadStatic] + GC while threads run");
        int counter = 0;
        var threads = new Thread[4];
        for (int t = 0; t < threads.Length; t++)
        {
            threads[t] = new Thread(() =>
            {
                tls = 1;
                var mine = new object[64];
                for (int k = 0; k < 20_000; k++) { Interlocked.Add(ref counter, tls); mine[k & 63] = new byte[256]; }
            });
            threads[t].Start();
        }
        foreach (var t in threads) t.Join();
        Log($"stage 4: ok, counter={counter} (expect 80000), gen0 collections so far {GC.CollectionCount(0)}");

        Log("stage 5: managed exception throw/catch");
        try { throw new InvalidOperationException("managed throw"); }
        catch (Exception e) { Log($"stage 5: ok ({e.GetType().Name})"); }

        Log("stage 6: ThreadPool + async");
        var task = System.Threading.Tasks.Task.Run(() => 6 * 7);
        Log($"stage 6: ok, result={task.Result}");

        Log("stage 6b: delegate marshalled to native code (qsort with a managed comparer: the thunk block)");
        int[] values = { 5, 3, 9, 1, 7 };
        Comparison cmp = (pa, pb) => (*(int*)pa).CompareTo(*(int*)pb);
        fixed (int* pv = values) qsort(pv, (nuint)values.Length, sizeof(int), Marshal.GetFunctionPointerForDelegate(cmp));
        GC.KeepAlive(cmp);
        Log($"stage 6b: {(string.Join(",", values) == "1,3,5,7,9" ? "ok" : "WRONG")}, sorted {string.Join(",", values)}");

        Log("stage 7: System.Console");
        Console.WriteLine("aot-hello: stage 7: ok (Console.WriteLine works)");

        Log("RESULT: all stages passed");
        return 0;
    }
}
