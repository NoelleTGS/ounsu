// osu!'s "high performance session" for the console: no garbage collections while a map plays.
//
// osu.Desktop's HighPerformanceSessionManager switches to GCLatencyMode.LowLatency, which pins the gen0 and gen1
// budgets at 256 KB (gc.cpp, low_latency_alloc): a gen1 GC every 256 KB allocated. On a desktop each takes well under
// a millisecond; on the console ~4.6 ms, ~14 times a second during gameplay (run nobgc1), each longer than a short
// audio queue. Without any session, gen2s of 100-170 ms hit gameplay (run speed1).
//
// Here a session starts a no-GC region instead: gameplay allocates ~4 MB/s, so LX_NOGC_MB (default 1536) covers
// ~6 minutes of play. LowLatency is set first: when a long map exhausts the region, the runtime collects once and
// restores LowLatency, as on desktop. The garbage is collected after the map (results screen). LX_NOGC_MB=0 gives
// osu.Desktop's behaviour. Needs a heap limit well above the region (osu-game.csproj: HeapHardLimit).
// BackgroundDataStoreProcessor also waits while a session is active.

using System;
using System.Runtime;
using System.Threading;
using osu.Framework.Allocation;
using osu.Game.Performance;

public class GameplayGcSession : IHighPerformanceSessionManager
{
    private const long loh_budget = 256L << 20;

    private int activeSessions;
    private GCLatencyMode originalMode;
    private long allocatedAtStart;
    private int gcsAtStart;
    private readonly long budget;

    public GameplayGcSession()
    {
        long mb = 1536;
        string? env = Environment.GetEnvironmentVariable("LX_NOGC_MB");
        if (!string.IsNullOrEmpty(env) && long.TryParse(env, out long parsed) && parsed >= 0)
            mb = parsed;
        budget = mb << 20;
    }

    public bool IsSessionActive => activeSessions > 0;

    public IDisposable BeginSession()
    {
        if (Interlocked.Increment(ref activeSessions) == 1)
            enter();
        return new InvokeOnDisposal<GameplayGcSession>(this, static s =>
        {
            if (Interlocked.Decrement(ref s.activeSessions) == 0)
                s.exit();
        });
    }

    private void enter()
    {
        originalMode = GCSettings.LatencyMode;
        GCSettings.LatencyMode = GCLatencyMode.LowLatency;
        allocatedAtStart = GC.GetTotalAllocatedBytes();
        gcsAtStart = GC.CollectionCount(0);

        if (budget == 0)
        {
            GC.Collect(0); // as osu.Desktop: LowLatency only takes effect at the next GC
            Program.Log("gc session: started (LowLatency only, LX_NOGC_MB=0)");
            return;
        }

        try
        {
            // disallowFullBlockingGC: an ephemeral GC makes room instead of a full blocking one.
            bool ok = GC.TryStartNoGCRegion(budget, Math.Min(loh_budget, budget / 2), disallowFullBlockingGC: true);
            Program.Log(ok
                ? $"gc session: started, no-GC region of {budget >> 20} MiB"
                : "gc session: no-GC region refused (not enough memory); LowLatency");
        }
        catch (Exception e)
        {
            Program.Log($"gc session: no-GC region failed ({e.GetType().Name}: {e.Message}); LowLatency");
        }

        if (GCSettings.LatencyMode != GCLatencyMode.NoGCRegion)
            GC.Collect(0);
    }

    private void exit()
    {
        bool stillInRegion = GCSettings.LatencyMode == GCLatencyMode.NoGCRegion;
        long allocated = GC.GetTotalAllocatedBytes() - allocatedAtStart;
        int gcs = GC.CollectionCount(0) - gcsAtStart;

        if (stillInRegion)
        {
            try
            {
                GC.EndNoGCRegion();
            }
            catch (InvalidOperationException)
            {
                // The region ended under us (exhausted, or a GC was induced): nothing to end.
            }
        }

        if (GCSettings.LatencyMode == GCLatencyMode.LowLatency)
            GCSettings.LatencyMode = originalMode;

        Program.Log($"gc session: ended, {allocated >> 20} MiB allocated, {gcs} GC(s) during the session"
                    + (budget > 0 ? (stillInRegion ? ", region held" : ", region exhausted or lost") : ""));
    }
}
