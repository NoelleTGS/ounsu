using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using osu.Framework;
using osu.Framework.Logging;
using osu.Framework.Screens;
using osu.Framework.Platform;
using osu.Game;
using SixLabors.ImageSharp;
using SixLabors.ImageSharp.PixelFormats;

// osu! (lazer) on Switch 2 (kaiHEN homebrew, Linux syscalls emulated by the host .nro): cut-down console entry point.
// No Velopack/Discord/IPC/tournament (osu.Desktop's extras). Logs go through hbxl_log
// and osu!'s own logs land on the SD card under the data root's
// /home/.local/share/osu/logs, next to periodic screenshots (screenshots-console/, opt-in: HBXL_SCREENSHOT_EVERY=<s>).
static unsafe partial class Program
{
#if HBXL
    [LibraryImport("hbxl", EntryPoint = "hbxl_log", StringMarshalling = StringMarshalling.Utf8)]
    private static partial void hbxl_log(string text);

    [LibraryImport("hbxl", EntryPoint = "hbxl_thread_backtrace")]
    private static partial int hbxl_thread_backtrace(int tid, ulong* output, int max);

    /// <summary>
    /// Hang diagnostics: backtrace (pc, lr, frame-pointer chain) of every hbxl thread except the caller.
    /// Symbolise the addresses with nm on app/build/app.elf.
    /// </summary>
    /// <summary>Stall detector heartbeat (shim/hbxl_input.c): 0 window, 1 draw, 2 update.</summary>
    [LibraryImport("hbxl", EntryPoint = "hbxl_stall_beat")]
    internal static partial void StallBeat(int which);

    [LibraryImport("hbxl", EntryPoint = "hbxl_gettid")]
    private static partial long hbxl_gettid();

    [LibraryImport("hbxl", EntryPoint = "hbxl_thread_state")]
    private static partial int hbxl_thread_state(int tid);

    [LibraryImport("hbxl", EntryPoint = "hbxl_clone_progress")]
    private static partial void hbxl_clone_progress(int* output);

    [LibraryImport("hbxl", EntryPoint = "hbxl_dbg_tl_lock")]
    private static partial int hbxl_dbg_tl_lock();

    [LibraryImport("hbxl", EntryPoint = "hbxl_dbg_malloc_lock")]
    private static partial int hbxl_dbg_malloc_lock();

    internal static void DumpThreads(string reason)
    {
        int* clone = stackalloc int[3];
        hbxl_clone_progress(clone);
        Log($"threads: dump ({reason}); by tid {hbxl_gettid()}; musl thread-list lock {hbxl_dbg_tl_lock():x} (owner tid, 0 = free), " +
            $"malloc lock {hbxl_dbg_malloc_lock():x}, " +
            $"last clone: by tid {clone[0]} stage {clone[1]} (1 reap 2 alloc 3 create 4 start 5 log 6 done) new tid {clone[2]}");
        ulong* frames = stackalloc ulong[32];

        for (int tid = 1; tid < 160; tid++)
        {
            int state = hbxl_thread_state(tid);
            if (state == 0)
                continue;

            int n = state == 1 ? hbxl_thread_backtrace(tid, frames, 32) : 0;
            var text = new System.Text.StringBuilder($"threads: tid {tid}:");
            if (state != 1)
                text.Append($" (state {state}: {(state == 3 ? "starting" : state == 2 ? "finished" : "?")})");
            for (int k = 0; k < n; k++)
                text.Append($" {frames[k]:x}");
            Log(text.ToString());
        }

        Log("threads: end of dump");
    }
#else
    private static void hbxl_log(string text) => Console.Error.WriteLine(text);
#endif

    internal static void Log(string s)
    {
        foreach (string line in ("osu-console: " + s).Split('\n'))
            hbxl_log(line.Length > 400 ? line[..400] : line);
    }

    internal static string DataDir =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "osu");

    static void Crash(Exception e)
    {
        string text = e.ToString();
        Log("FAILED: " + (text.Length > 6000 ? text[..6000] : text));
        try
        {
            Directory.CreateDirectory(Path.Combine(DataDir, "logs"));
            File.WriteAllText(Path.Combine(DataDir, "logs", "osu-console-crash.txt"), DateTime.Now + "\n" + text + "\n");
        }
        catch
        {
        }
    }

#if HBXL
    // NativeAOT: mods name custom setting controls (SettingSourceAttribute.SettingControlType) that osu! creates with
    // Activator.CreateInstance; constructed generic types only get a reflection-invokable constructor when something
    // asks for it ("No parameterless constructor defined for SettingsSlider<int, MuteComboSlider>"). Non-generic
    // control types live in rooted assemblies and need nothing. Keep this list in sync with
    // `grep -rho "SettingControlType = typeof([^)]*)" src/osu` (generic ones only).
    [System.Diagnostics.CodeAnalysis.DynamicDependency(System.Diagnostics.CodeAnalysis.DynamicallyAccessedMemberTypes.PublicParameterlessConstructor,
        typeof(osu.Game.Overlays.Settings.SettingsSlider<int, osu.Game.Rulesets.Mods.MuteComboSlider>))]
    [System.Diagnostics.CodeAnalysis.DynamicDependency(System.Diagnostics.CodeAnalysis.DynamicallyAccessedMemberTypes.PublicParameterlessConstructor,
        typeof(osu.Game.Overlays.Settings.SettingsSlider<int, osu.Game.Rulesets.Mods.HiddenComboSlider>))]
    [System.Diagnostics.CodeAnalysis.DynamicDependency(System.Diagnostics.CodeAnalysis.DynamicallyAccessedMemberTypes.PublicParameterlessConstructor,
        typeof(osu.Game.Overlays.Settings.SettingsSlider<int, osu.Game.Rulesets.Osu.Mods.MaxSizeComboSlider>))]
    [System.Diagnostics.CodeAnalysis.DynamicDependency(System.Diagnostics.CodeAnalysis.DynamicallyAccessedMemberTypes.PublicParameterlessConstructor,
        typeof(osu.Game.Overlays.Settings.SettingsSlider<float, osu.Game.Graphics.UserInterface.RoundedSliderBar<float>>))]
    [System.Diagnostics.CodeAnalysis.DynamicDependency(System.Diagnostics.CodeAnalysis.DynamicallyAccessedMemberTypes.PublicParameterlessConstructor,
        typeof(osu.Game.Overlays.Settings.SettingsPercentageSlider<float>))]
    [System.Diagnostics.CodeAnalysis.DynamicDependency(System.Diagnostics.CodeAnalysis.DynamicallyAccessedMemberTypes.PublicParameterlessConstructor,
        typeof(osu.Game.Screens.Play.PlayerSettings.PlayerSliderBar<int>))]
    // Shader cache (osu.Framework ShaderCompilationStore → Newtonsoft): SpirvReflection's struct arrays are built through
    // List<T> made with MakeGenericType, so those instantiations must exist ("List`1[Veldrid.ResourceLayoutDescription]
    // is missing native code or metadata" → every shader recompiled through SPIRV-Cross on every launch).
    [System.Diagnostics.CodeAnalysis.DynamicDependency(System.Diagnostics.CodeAnalysis.DynamicallyAccessedMemberTypes.PublicConstructors,
        typeof(System.Collections.Generic.List<Veldrid.ResourceLayoutDescription>))]
    [System.Diagnostics.CodeAnalysis.DynamicDependency(System.Diagnostics.CodeAnalysis.DynamicallyAccessedMemberTypes.PublicConstructors,
        typeof(System.Collections.Generic.List<Veldrid.ResourceLayoutElementDescription>))]
    [System.Diagnostics.CodeAnalysis.DynamicDependency(System.Diagnostics.CodeAnalysis.DynamicallyAccessedMemberTypes.PublicConstructors,
        typeof(System.Collections.Generic.List<Veldrid.VertexElementDescription>))]
#endif
    /// <summary>
    /// OSU_HBXL_JSON_SELFTEST=1: deserialize canned API responses offline (no network) and exit. NativeAOT trimming broke
    /// news/beatmap search (Cursor's [JsonExtensionData] dictionary) and beatmap sets (List of a struct), 2026-11-09;
    /// rd.xml (tools/gen-rdxml) fixes those. 0 = all passed.
    /// </summary>
    static int JsonSelfTest()
    {
        int failures = 0;

        void check<T>(string name, string json, Func<T, string> describe)
        {
            try
            {
                var value = Newtonsoft.Json.JsonConvert.DeserializeObject<T>(json)!;
                Log($"json-selftest: ok {name}: {describe(value)}");
            }
            catch (Exception e)
            {
                failures++;
                Log($"json-selftest: FAIL {name}: {e.GetType().Name}: {e.Message}\n{e.StackTrace?.Split('\n').FirstOrDefault()}");
            }
        }

        const string cursor = @"""cursor"":{""published_at"":""2026-11-01T00:00:00+00:00"",""id"":123}";
        const string beatmapset = @"{""id"":2058976,""title"":""T"",""artist"":""A"",""title_unicode"":""T"",""artist_unicode"":""A"",
            ""status"":""ranked"",""covers"":{""cover"":""c"",""cover@2x"":""c"",""card"":""c"",""card@2x"":""c"",""list"":""l"",""list@2x"":""l""},
            ""ratings"":[0,1,2,3,4,5,6,7,8,9,10],""bpm"":180.5,""nsfw"":false,""video"":false,""storyboard"":false,
            ""submitted_date"":""2024-01-01T00:00:00+00:00"",""ranked_date"":""2024-02-01T00:00:00+00:00"",""last_updated"":""2024-02-01T00:00:00+00:00"",
            ""hype"":{""current"":5,""required"":5},
            ""nominations_summary"":{""current"":2,""required_meta"":{""main_ruleset"":2,""non_main_ruleset"":1}},
            ""availability"":{""download_disabled"":false,""more_information"":null},
            ""genre"":{""id"":2,""name"":""Video Game""},""language"":{""id"":3,""name"":""Japanese""},
            ""current_nominations"":[{""beatmapset_id"":2058976,""reset"":false,""rulesets"":[""osu""],""user_id"":1}],
            ""related_users"":[{""id"":1,""username"":""u""}],""tags"":""a b"",""user_id"":1,""creator"":""u"",
            ""beatmaps"":[{""id"":1,""beatmapset_id"":2058976,""mode"":""osu"",""version"":""Hard"",""difficulty_rating"":5.1,
                ""checksum"":""abc"",""status"":""ranked"",""total_length"":120,""hit_length"":110,""count_circles"":1,""count_sliders"":2,""count_spinners"":0,
                ""cs"":4,""ar"":9,""accuracy"":8,""drain"":5,""bpm"":180,""max_combo"":300,""playcount"":1,""passcount"":1,
                ""failtimes"":{""fail"":[0,1,2],""exit"":[3,4]},""owners"":[{""id"":1,""username"":""u""}]}],
            ""converts"":[]}";

        check<osu.Game.Online.API.Requests.GetNewsResponse>("news (Cursor extension data)",
            @"{""news_posts"":[],""news_sidebar"":null," + cursor + "}",
            r => $"cursor properties {r.Cursor?.Properties.Count}");
        check<osu.Game.Online.API.Requests.SearchBeatmapSetsResponse>("beatmap search",
            @"{""beatmapsets"":[" + beatmapset + @"],""total"":1," + cursor + "}",
            r => $"{r.BeatmapSets.Count()} set(s), total {r.Total}");
        check<osu.Game.Online.API.Requests.Responses.APIBeatmapSet>("beatmap set (nominations, genre, beatmaps)", beatmapset,
            r => $"{r.CurrentNominations?.Length} nomination(s), genre {r.Genre.Name}, {r.Beatmaps.Length} beatmap(s)");

        // Login: the server names the second factor as "totp" or "mail" ([EnumMember] values). If this parsed as null, the
        // client would skip its verification form (2026-11-09: no TOTP form appeared, the user verified by email).
        foreach (string method in new[] { "totp", "mail" })
        {
            check<osu.Game.Online.API.Requests.Responses.APIMe>($"login verification method \"{method}\"",
                @"{""id"":1,""username"":""u"",""session_verification_method"":""" + method + @"""}",
                r => r.SessionVerificationMethod == null
                    ? throw new InvalidOperationException("parsed as null")
                    : r.SessionVerificationMethod.ToString()!);
        }

        // Skin layouts name component types with the assembly version they were saved with; osu.Game must be at least that
        // new (src/osu/Directory.Build.targets stamps the build date) or every saved layout falls back to the defaults.
        var gameVersion = typeof(OsuGame).Assembly.GetName().Version!;
        string olderVersion = $"{gameVersion.Major}.{Math.Max(0, gameVersion.Minor - 1)}.0.0";
        check<Type>($"skin layout type reference from an older release (Version={olderVersion})",
            @"""osu.Game.Skinning.LegacyScoreCounter, osu.Game, Version=" + olderVersion + @", Culture=neutral, PublicKeyToken=null""",
            t => $"{t.FullName} (osu.Game is {gameVersion})");

        Log($"json-selftest: done, {failures} failure(s)");
        return failures == 0 ? 0 : 1;
    }

    static int Main()
    {
        // No `using`: an exception from host.Run (e.g. osu!'s "crashing out") must be logged BEFORE
        // the game and host are disposed, and each teardown step gets a breadcrumb, because a death
        // during teardown can end the process before the log is written.
        GameHost? host = null;
        Game? game = null;
        int result;

        // osu!'s own log files are written by a background thread, so a hard death loses the last lines.
        // Mirror every entry synchronously through hbxl_log instead.
        Logger.NewEntry += mirrorLogEntry;
        if (Environment.GetEnvironmentVariable("OSU_HBXL_JSON_SELFTEST") == "1")
            return JsonSelfTest();
        if (Environment.GetEnvironmentVariable("OSU_HBXL_HUB_SELFTEST") is string hubGolden && hubGolden.Length > 0)
            return HubSelfTest(hubGolden);
        if (Environment.GetEnvironmentVariable("OSU_HBXL_HUB_LIVETEST") is string hubUrl && hubUrl.Length > 0)
            return HubLiveTest(hubUrl);
#if HBXL
        // Construct the shader-cache list types once so ILC compiles them completely (see the attributes above).
        GC.KeepAlive(new System.Collections.Generic.List<Veldrid.ResourceLayoutDescription>(0));
        GC.KeepAlive(new System.Collections.Generic.List<Veldrid.ResourceLayoutElementDescription>(0));
        GC.KeepAlive(new System.Collections.Generic.List<Veldrid.VertexElementDescription>(0));

        // The DynamicDependency attributes above keep only constructor metadata. A constructed generic that nothing else
        // creates still has no compiled type, so Activator.CreateInstance threw "PlayerSliderBar`1[Int32] is missing native
        // code or metadata" (ReplayAnalysisSettings, every replay; 2026-11-09). Construct each one in a branch that never
        // runs, so ILC compiles the types completely.
        if (Environment.GetEnvironmentVariable("HBXL_AOT_NEVER_SET") == "construct-setting-controls")
        {
            GC.KeepAlive(new osu.Game.Overlays.Settings.SettingsSlider<int, osu.Game.Rulesets.Mods.MuteComboSlider>());
            GC.KeepAlive(new osu.Game.Overlays.Settings.SettingsSlider<int, osu.Game.Rulesets.Mods.HiddenComboSlider>());
            GC.KeepAlive(new osu.Game.Overlays.Settings.SettingsSlider<int, osu.Game.Rulesets.Osu.Mods.MaxSizeComboSlider>());
            GC.KeepAlive(new osu.Game.Overlays.Settings.SettingsSlider<float, osu.Game.Graphics.UserInterface.RoundedSliderBar<float>>());
            GC.KeepAlive(new osu.Game.Overlays.Settings.SettingsPercentageSlider<float>());
            GC.KeepAlive(new osu.Game.Screens.Play.PlayerSettings.PlayerSliderBar<int>());
        }
#endif

        try
        {
            Log("stage 0: managed Main reached");
            Log("stage 1: creating host");
            host = Host.GetSuitableDesktopHost(@"osu", new HostOptions
            {
                IPCPort = null,
                FriendlyGameName = OsuGameBase.GAME_NAME,
            });
            Log($"stage 1: ok ({host.GetType().Name})");

            // No auto-exit unless HBXL_EXIT_AFTER=<s> is set (a test aid; it used to default to 120 s).
            int exitAfter = int.TryParse(Environment.GetEnvironmentVariable("HBXL_EXIT_AFTER"), out int s) && s > 0 ? s : 0;
            Log(exitAfter > 0 ? $"stage 2: host.Run(OsuGame), exits by itself after {exitAfter} s" : "stage 2: host.Run(OsuGame)");
            game = new ConsoleOsuGame(exitAfter);
            var hostForStatus = host;
            var status = new System.Threading.Thread(() => statusLoop(hostForStatus)) { IsBackground = true, Name = "osu-console status" };
            status.Start();
#if HBXL
            new System.Threading.Thread(gcWatch) { IsBackground = true, Name = "osu-console gcwatch" }.Start();
#endif
            host.Run(game);
            Log("RESULT: host.Run returned");
            result = 0;
        }
        catch (Exception e)
        {
            Crash(e);
            result = 1;
        }

        teardown("game", game);
        teardown("host", host);
        Log($"teardown: done, returning {result}");
        return result;
    }

    // Screen loads that never finish (song select): set by mirrorLogEntry from the screen stack's
    // "📺 ... loading X" / "entered X" / "exit from X" lines, checked by statusLoop.
    private static readonly System.Text.RegularExpressions.Regex screen_event =
        new System.Text.RegularExpressions.Regex(@"📺 .*\) (loading|entered|exit from) (\S+)");

    private static volatile string? loadingScreen;
    private static DateTime loadingSince;

    static void trackScreenLoad(string? message)
    {
        if (message == null || !message.Contains("📺"))
            return;

        var m = screen_event.Match(message);
        if (!m.Success)
            return;

        if (m.Groups[1].Value == "loading")
        {
            loadingSince = DateTime.UtcNow;
            loadingScreen = m.Groups[2].Value;
        }
        else if (m.Groups[2].Value == loadingScreen)
            loadingScreen = null;
    }

    static void mirrorLogEntry(LogEntry entry)
    {
        try
        {
            if (entry.Message?.Contains("That operation is not supported") == true)
                return; // SDL spam

            trackScreenLoad(entry.Message);

            string text = $"log [{entry.Target?.ToString() ?? entry.LoggerName}/{entry.Level}] {entry.Message}";

            if (entry.Exception != null)
            {
                text += $"\n  {entry.Exception.GetType().FullName}: {entry.Exception.Message}";
                string[] frames = (entry.Exception.StackTrace ?? string.Empty).Split('\n');
                foreach (string frame in frames.Take(12))
                    text += "\n  " + frame.Trim();
                if (entry.Exception.InnerException is Exception inner)
                    text += $"\n  inner {inner.GetType().FullName}: {inner.Message}";
            }

            Log(text);
        }
        catch
        {
        }
    }

    static void teardown(string what, IDisposable? disposable)
    {
        if (disposable == null)
            return;

        Log($"teardown: disposing {what}");

        try
        {
            disposable.Dispose();
            Log($"teardown: {what} disposed");
        }
        catch (Exception e)
        {
            Log($"teardown: disposing {what} threw {e.GetType().Name}: {e.Message}");
        }
    }

#if HBXL
    [LibraryImport("hbxl", EntryPoint = "hbxl_log")]
    private static unsafe partial void hbxl_log_raw(byte* text);

    /// <summary>
    /// Allocation-free GC watch: during a GC storm every allocation waits for a full GC, so the status line (which formats
    /// strings) goes silent. This thread never allocates after start, so it keeps logging between collections:
    /// "hbxl-gcwatch: N MiB live-ish, gen0/1/2 +a/+b/+c in 2 s" whenever gen2 collections pile up (2026-11-09 freeze).
    /// </summary>
    static unsafe void gcWatch()
    {
        byte* buf = stackalloc byte[160];
        int g0 = GC.CollectionCount(0), g1 = GC.CollectionCount(1), g2 = GC.CollectionCount(2);
        while (true)
        {
            System.Threading.Thread.Sleep(2000);
            int n0 = GC.CollectionCount(0), n1 = GC.CollectionCount(1), n2 = GC.CollectionCount(2);
            if (n2 - g2 >= 3)
            {
                var span = new Span<byte>(buf, 159);
                int at = 0;
                void put(ReadOnlySpan<byte> text, Span<byte> dst, ref int pos) { text.CopyTo(dst[pos..]); pos += text.Length; }
                void num(long value, Span<byte> dst, ref int pos) { System.Buffers.Text.Utf8Formatter.TryFormat(value, dst[pos..], out int w); pos += w; }
                put("hbxl-gcwatch: heap "u8, span, ref at);
                num(GC.GetTotalMemory(false) >> 20, span, ref at);
                put(" MiB, allocated total "u8, span, ref at);
                num(GC.GetTotalAllocatedBytes() >> 20, span, ref at);
                put(" MiB, gen0/1/2 +"u8, span, ref at);
                num(n0 - g0, span, ref at);
                put("/+"u8, span, ref at);
                num(n1 - g1, span, ref at);
                put("/+"u8, span, ref at);
                num(n2 - g2, span, ref at);
                put(" in 2 s"u8, span, ref at);
                buf[at] = 0;
                hbxl_log_raw(buf);
            }
            g0 = n0; g1 = n1; g2 = n2;
        }
    }
#endif

    static void statusLoop(GameHost host)
    {
        double lastClock = -1;
        int stalled = 0;
        bool dumped = false;
        string? lastStuckScreen = null;
        int stuckDumps = 0;

        for (int i = 1; ; i++)
        {
            System.Threading.Thread.Sleep(10000);
            try
            {
#if HBXL
                // The update clock not moving for two intervals = the update thread is stuck: dump all threads once.
                double clock = host.UpdateThread?.Clock.CurrentTime ?? -1;
                stalled = clock == lastClock ? stalled + 1 : 0;
                lastClock = clock;
                if (stalled >= 2 && !dumped)
                {
                    dumped = true;
                    DumpThreads($"update clock stuck at {clock / 1000:F1}s");
                }

                // A screen that has been loading for over 15 s (song select never finishes loading): dump once per screen.
                string? stuck = loadingScreen;
                if (stuck != null && stuck != lastStuckScreen && stuckDumps < 3 && DateTime.UtcNow - loadingSince > TimeSpan.FromSeconds(15))
                {
                    lastStuckScreen = stuck;
                    stuckDumps++;
                    DumpThreads($"screen {stuck} still loading after {(DateTime.UtcNow - loadingSince).TotalSeconds:F0}s");
                }
#endif
                var w = host.Window;
                var gcInfo = GC.GetGCMemoryInfo();
                Log($"status {i * 10}s: state {host.ExecutionState}, window {(w == null ? "none" : $"{w.WindowState} {w.ClientSize.Width}x{w.ClientSize.Height}")}, " +
                    $"draw {host.DrawThread?.Clock.FramesPerSecond:F0} fps, update {host.UpdateThread?.Clock.FramesPerSecond:F0} fps, " +
                    $"managed heap {GC.GetTotalMemory(false) >> 20} MiB (gc: committed {gcInfo.TotalCommittedBytes >> 20}, heap {gcInfo.HeapSizeBytes >> 20}, " +
                    $"fragmented {gcInfo.FragmentedBytes >> 20}, limit {gcInfo.TotalAvailableMemoryBytes >> 20} MiB), " +
                    $"gc {GC.CollectionCount(0)}/{GC.CollectionCount(1)}/{GC.CollectionCount(2)} pause {GC.GetTotalPauseDuration().TotalMilliseconds:F0} ms, " +
                    $"pool threads {System.Threading.ThreadPool.ThreadCount} pending {System.Threading.ThreadPool.PendingWorkItemCount} " +
                    $"done {System.Threading.ThreadPool.CompletedWorkItemCount}, update clock {host.UpdateThread?.Clock.CurrentTime / 1000:F1}s");
            }
            catch (Exception e)
            {
                Log($"status {i * 10}s: {e.GetType().Name}: {e.Message}");
            }
        }
    }
}

/// <summary>
/// OsuGame that closes itself after a while (the console has no safe way to close it yet) and
/// saves a screenshot every N seconds (opt-in) so frames can be inspected later.
/// </summary>
public partial class ConsoleOsuGame : OsuGame
{
    private readonly int exitAfterSeconds;
    private int shots;
    private int screenshotEvery;

    public ConsoleOsuGame(int exitAfterSeconds)
        : base(Array.Empty<string>())
    {
        this.exitAfterSeconds = exitAfterSeconds;
#if HBXL_SETTINGS
        // The settings overlay asks for the section when it loads (after LoadComplete below has made the config).
        osu.Game.Overlays.SettingsOverlay.CreateExtraSections = () => switch2Config == null
            ? Array.Empty<osu.Game.Overlays.Settings.SettingsSection>()
            : new osu.Game.Overlays.Settings.SettingsSection[] { new Switch2Section(switch2Config, () => importFromFolder(everything: true, byUser: true)) };
#endif
    }

    /// <summary>The "Switch 2" settings (Switch2Settings.cs): switch2.ini in osu!'s data folder, sent to the C shims.</summary>
    private Switch2Config? switch2Config;

    private void loadSwitch2Config()
    {
        try
        {
            switch2Config = new Switch2Config(Storage);
            switch2Config.Apply();
            importAuto = switch2Config.GetBindable<bool>(Switch2Setting.ImportAutoCheck);
            importInterval = switch2Config.GetBindable<int>(Switch2Setting.ImportCheckInterval);
            Program.Log("switch2 settings: loaded");
        }
        catch (Exception e)
        {
            // The shims keep their hbxl-env values; the section is left out.
            switch2Config = null;
            Program.Log($"switch2 settings FAILED: {e}");
        }
    }

    protected override void Dispose(bool isDisposing)
    {
        switch2Config?.Dispose();
        base.Dispose(isDisposing);
    }

    // Periodic work runs from Update(), never as a repeating task on the game's scheduler: osu.Framework throws "Too many
    // timed tasks are in the queue!" when a repeating task comes due while that scheduler holds more than 1000 timed
    // tasks, and it is shared with every DelayedLoadWrapper in the game. Opening the dashboard with a long friends
    // list (one wrapper per panel) while importTick repeated there crashed the game, 2026-11-11.
    private double nextImportTick;
    private double nextScreenshot;

    protected override void Update()
    {
        base.Update();

        if (IsLoaded && Clock.CurrentTime >= nextImportTick && nextImportTick > 0)
        {
            nextImportTick = Clock.CurrentTime + 1000;
            importTick();
        }

        if (screenshotEvery > 0 && nextScreenshot > 0 && Clock.CurrentTime >= nextScreenshot)
        {
            nextScreenshot = Clock.CurrentTime + screenshotEvery * 1000;
            takeScreenshot();
        }

#if HBXL
        Program.StallBeat(2); // update-thread heartbeat for the stall detector
#endif
    }

    protected override void LoadComplete()
    {
        // Before base.LoadComplete: it starts loading the settings overlay, which asks for the "Switch 2" section.
        loadSwitch2Config();
        base.LoadComplete();
        Program.Log("stage 3: OsuGame LoadComplete");
        // Diagnostic screenshots are opt-in (HBXL_SCREENSHOT_EVERY=<seconds>): each one is a full-frame GPU readback plus a
        // PNG encode, and froze update + draw for ~1.4-1.9 s every 15 s (stall detector, 2026-11-09).
        if (int.TryParse(Environment.GetEnvironmentVariable("HBXL_SCREENSHOT_EVERY"), out screenshotEvery) && screenshotEvery > 0)
            nextScreenshot = Clock.CurrentTime + screenshotEvery * 1000;
        importFromFolder(everything: true, byUser: false);
        nextImportTick = Clock.CurrentTime + 1000;

        // Test hook: HBXL_TEST_SONGSELECT=<s> opens song select after <s> seconds (like pressing Play).
        if (int.TryParse(Environment.GetEnvironmentVariable("HBXL_TEST_SONGSELECT"), out int songSelectAfter) && songSelectAfter > 0)
        {
            Scheduler.AddDelayed(() =>
            {
                Program.Log("test: opening song select");
                PerformFromScreen(s => s.Push(new osu.Game.Screens.Select.SoloSongSelect()));
            }, songSelectAfter * 1000);
        }

#if HBXL_SETTINGS
        // Test hook: HBXL_TEST_SETTINGS=<s> opens the settings at the "Switch 2" section after <s> seconds.
        if (int.TryParse(Environment.GetEnvironmentVariable("HBXL_TEST_SETTINGS"), out int settingsAfter) && settingsAfter > 0)
        {
            Scheduler.AddDelayed(() =>
            {
                Program.Log("test: opening the Switch 2 settings");
                Settings.ShowAtControl<Switch2Section>();
            }, settingsAfter * 1000);
        }
#endif

        if (exitAfterSeconds > 0)
        {
            Scheduler.AddDelayed(() =>
            {
                Program.Log("stage 4: exit timer reached");
                Exit();
            }, exitAfterSeconds * 1000);
        }
    }

    /// <summary>
    /// Beatmaps (.osz/.olz), skins (.osk) and replays (.osr) placed in the import folder (HBXL_IMPORT_DIR, default
    /// /import in the data root on the SD card) are imported as if dropped on the window: everything at startup and from the
    /// "Switch 2" settings' button, and new files on each automatic check (settings: on/off and interval).
    /// A file is moved out of the folder the moment it is picked up, into importing/, so no later check or button
    /// press can pick it up again. After the import osu! deletes beatmap archives itself, like on desktop; whatever is
    /// left (skins, replays) goes to imported/, and a file whose import failed to failed/. Only one run at a time.
    /// </summary>
    private readonly System.Collections.Generic.Dictionary<string, (long size, DateTime time)> importSeen = new();
    private int importBusy;
    private double importCheckedAt;
    private osu.Framework.Bindables.IBindable<bool>? importAuto;
    private osu.Framework.Bindables.IBindable<int>? importInterval;

    private static readonly string[] import_extensions = { ".osz", ".olz", ".osk", ".osr" };

    /// <summary>Update thread, once a second: starts an automatic check when one is due.</summary>
    private void importTick()
    {
        if (importAuto?.Value != true || importInterval == null)
            return;
        if (Clock.CurrentTime - importCheckedAt < importInterval.Value * 1000.0)
            return;
        // Not while playing (osu! holds beatmap imports back then anyway).
        if (UserPlayingState.Value != osu.Game.Screens.Play.LocalUserPlayingState.NotPlaying)
            return;
        importFromFolder(everything: false, byUser: false);
    }

    /// <param name="everything">Also files this session already imported (startup, the settings' button).</param>
    /// <param name="byUser">Report an empty folder and failures as notifications.</param>
    private void importFromFolder(bool everything, bool byUser)
    {
        if (System.Threading.Interlocked.Exchange(ref importBusy, 1) != 0)
        {
            if (byUser)
                Notifications.Post(new osu.Game.Overlays.Notifications.SimpleNotification { Text = "An import from the import folder is already running." });
            return;
        }

        importCheckedAt = Clock.CurrentTime;
        string dir = Environment.GetEnvironmentVariable("HBXL_IMPORT_DIR") ?? "/import";

        void notify(string text, bool error = false)
        {
            if (byUser)
            {
                Schedule(() => Notifications.Post(error
                    ? new osu.Game.Overlays.Notifications.SimpleErrorNotification { Text = text }
                    : new osu.Game.Overlays.Notifications.SimpleNotification { Text = text }));
            }
        }

        System.Threading.Tasks.Task.Run(async () =>
        {
            try
            {
                if (!Directory.Exists(dir))
                {
                    notify($"The import folder ({dir}) doesn't exist.", true);
                    return;
                }

                string importing = Path.Combine(dir, "importing");
                Directory.CreateDirectory(importing);

                (string path, (long size, DateTime time) stamp)[] list(string folder) =>
                    Directory.GetFiles(folder)
                             .Where(f => import_extensions.Contains(Path.GetExtension(f).ToLowerInvariant()))
                             .Select(f =>
                             {
                                 var info = new FileInfo(f);
                                 return (f, (info.Length, info.LastWriteTimeUtc));
                             })
                             .ToArray();

                var found = list(dir);

                // An automatic check takes a file once it looked the same on two checks in a row: it may still be
                // being copied into the folder. Startup and the button take everything.
                var taken = found.Where(f => everything || (importSeen.TryGetValue(f.path, out var seen) && seen == f.stamp)).ToArray();

                importSeen.Clear();
                foreach (var f in found.Except(taken))
                    importSeen[f.path] = f.stamp;

                // Out of the folder right away, before any import starts.
                foreach (var f in taken)
                {
                    try
                    {
                        File.Move(f.path, Path.Combine(importing, Path.GetFileName(f.path)), true);
                    }
                    catch (Exception e)
                    {
                        Program.Log($"import: can't move {Path.GetFileName(f.path)} to importing/: {e.Message}");
                    }
                }

                // Everything in importing/: what was just moved, plus leftovers of a run that died.
                // Beatmaps first (replays need their beatmap), then skins, then replays.
                string[] files = list(importing).Select(f => f.path)
                                                .OrderBy(f => Array.IndexOf(import_extensions, Path.GetExtension(f).ToLowerInvariant()))
                                                .ThenBy(f => f, StringComparer.Ordinal)
                                                .ToArray();

                if (files.Length == 0)
                {
                    notify($"Nothing to import: no .osz, .olz, .osk or .osr files in {dir}.");
                    return;
                }

                // One file at a time: the console's managed heap is capped and a parallel import of several large
                // archives risks running out.
                Program.Log($"import: {files.Length} file(s) from {dir}, one at a time ({(byUser ? "settings button" : everything ? "startup" : "automatic check")})");
                int ok = 0;

                foreach (string file in files)
                {
                    string name = Path.GetFileName(file);
                    string target = "imported";

                    try
                    {
                        Program.Log($"import: {name}");
                        await Import(file).ConfigureAwait(false);
                        ok++;
                    }
                    catch (Exception e)
                    {
                        string trace = e.ToString();
                        Program.Log($"import FAILED for {name}: {(trace.Length > 3000 ? trace[..3000] : trace)}");
                        notify($"Import of {name} failed (moved to the import folder's failed/): {e.Message}", true);
                        target = "failed";
                    }

                    try
                    {
                        if (File.Exists(file))  // osu! deleted it already if it was a beatmap archive
                        {
                            Directory.CreateDirectory(Path.Combine(dir, target));
                            File.Move(file, Path.Combine(dir, target, name), true);
                        }
                    }
                    catch (Exception e)
                    {
                        Program.Log($"import: can't move {name} to {target}/: {e.Message}");
                    }
                }

                Program.Log($"import: done ({ok}/{files.Length})");
            }
            catch (Exception e)
            {
                Program.Log($"import FAILED: {e.Message}");
                notify($"Import from the import folder failed: {e.Message}", true);
            }
            finally
            {
                System.Threading.Interlocked.Exchange(ref importBusy, 0);
            }
        });
    }

    private void takeScreenshot()
    {
        int n = ++shots;
        Host.TakeScreenshotAsync().ContinueWith(t =>
        {
            try
            {
                string dir = Path.Combine(Program.DataDir, "screenshots-console");
                Directory.CreateDirectory(dir);
                string file = Path.Combine(dir, $"shot-{n * screenshotEvery:000}s.png");
                using var image = t.Result;
                // The swapchain is RGBA but the framework's readback assumes BGRA: swap back.
                image.ProcessPixelRows(rows =>
                {
                    for (int y = 0; y < rows.Height; y++)
                        foreach (ref Rgba32 p in rows.GetRowSpan(y))
                            (p.R, p.B) = (p.B, p.R);
                });
                image.SaveAsPng(file);
                Program.Log($"screenshot {file}");
            }
            catch (Exception e)
            {
                Program.Log("screenshot FAILED: " + e.Message);
            }
        });
    }
}
