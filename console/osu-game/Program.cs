using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using osu.Framework;
using osu.Framework.Logging;
using osu.Framework.Platform;
using osu.Game;

// ounsu!: osu! (lazer) on Switch 2 (kaiHEN homebrew, Linux syscalls emulated by the host .nro), cut-down entry point.
// No Velopack/Discord/IPC/tournament (osu.Desktop's extras). Logs go through ounsu_log (the host's log: nxlink, and the
// SD card with LX_LOG_FILE), and osu!'s own logs land on the SD card under the data root's /home/.local/share/osu/logs.
static partial class Program
{
#if OUNSU
    [LibraryImport("ounsu", EntryPoint = "ounsu_log", StringMarshalling = StringMarshalling.Utf8)]
    private static partial void ounsu_log(string text);
#else
    private static void ounsu_log(string text) => Console.Error.WriteLine(text);
#endif

    internal static void Log(string s)
    {
        foreach (string line in ("osu-console: " + s).Split('\n'))
            ounsu_log(line.Length > 400 ? line[..400] : line);
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

#if OUNSU
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
    /// OUNSU_JSON_SELFTEST=1: deserialize canned API responses offline (no network) and exit. NativeAOT trimming broke
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
        // Mirror every entry synchronously through ounsu_log instead.
        Logger.NewEntry += mirrorLogEntry;
        if (Environment.GetEnvironmentVariable("OUNSU_JSON_SELFTEST") == "1")
            return JsonSelfTest();
        if (Environment.GetEnvironmentVariable("OUNSU_HUB_SELFTEST") is string hubGolden && hubGolden.Length > 0)
            return HubSelfTest(hubGolden);
        if (Environment.GetEnvironmentVariable("OUNSU_HUB_LIVETEST") is string hubUrl && hubUrl.Length > 0)
            return HubLiveTest(hubUrl);
#if OUNSU
        // Construct the shader-cache list types once so ILC compiles them completely (see the attributes above).
        GC.KeepAlive(new System.Collections.Generic.List<Veldrid.ResourceLayoutDescription>(0));
        GC.KeepAlive(new System.Collections.Generic.List<Veldrid.ResourceLayoutElementDescription>(0));
        GC.KeepAlive(new System.Collections.Generic.List<Veldrid.VertexElementDescription>(0));

        // The DynamicDependency attributes above keep only constructor metadata. A constructed generic that nothing else
        // creates still has no compiled type, so Activator.CreateInstance threw "PlayerSliderBar`1[Int32] is missing native
        // code or metadata" (ReplayAnalysisSettings, every replay; 2026-11-09). Construct each one in a branch that never
        // runs, so ILC compiles the types completely.
        if (Environment.GetEnvironmentVariable("OUNSU_AOT_NEVER_SET") == "construct-setting-controls")
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
            // The window's size per console mode and its frame rate, before the host makes the window.
            Switch2Config.ApplyDisplayEarly();
            Log("stage 1: creating host");
            host = Host.GetSuitableDesktopHost(@"osu", new HostOptions
            {
                IPCPort = null,
                FriendlyGameName = OsuGameBase.GAME_NAME,
            });
            Log($"stage 1: ok ({host.GetType().Name})");

            Log("stage 2: host.Run(OsuGame)");
            game = new ConsoleOsuGame();
            var hostForStatus = host;
            new System.Threading.Thread(() => statusLoop(hostForStatus)) { IsBackground = true, Name = "osu-console status" }.Start();
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

    static void mirrorLogEntry(LogEntry entry)
    {
        try
        {
            if (entry.Message?.Contains("That operation is not supported") == true)
                return; // SDL spam

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

    static void statusLoop(GameHost host)
    {
        for (int i = 1; ; i++)
        {
            System.Threading.Thread.Sleep(10000);
            try
            {
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
/// OsuGame with the console's extras: the "Switch 2" settings section, no GCs during gameplay and the import folder.
/// </summary>
public partial class ConsoleOsuGame : OsuGame
{
    public ConsoleOsuGame()
        : base(Array.Empty<string>())
    {
#if OUNSU_SETTINGS
        // The settings overlay asks for the section when it loads (after LoadComplete below has made the config).
        osu.Game.Overlays.SettingsOverlay.CreateExtraSections = () => switch2Config == null
            ? Array.Empty<osu.Game.Overlays.Settings.SettingsSection>()
            : new osu.Game.Overlays.Settings.SettingsSection[] { new Switch2Section(switch2Config, () => importFromFolder(everything: true, byUser: true)) };
#endif
    }

    // osu!'s high performance session (gameplay): no GCs while a map plays (GameplayGcSession.cs).
    // Cached by hand: this project doesn't run osu-framework's source generator for [Cached].
    private readonly GameplayGcSession highPerformanceSessionManager = new();

    protected override osu.Framework.Allocation.IReadOnlyDependencyContainer CreateChildDependencies(osu.Framework.Allocation.IReadOnlyDependencyContainer parent)
    {
        var dependencies = (osu.Framework.Allocation.DependencyContainer)base.CreateChildDependencies(parent);
        dependencies.CacheAs<osu.Game.Performance.IHighPerformanceSessionManager>(highPerformanceSessionManager);
        return dependencies;
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
            // The shims keep their lx-env values; the section is left out.
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

    protected override void Update()
    {
        base.Update();

        if (IsLoaded && Clock.CurrentTime >= nextImportTick && nextImportTick > 0)
        {
            nextImportTick = Clock.CurrentTime + 1000;
            importTick();
        }
    }

    protected override void LoadComplete()
    {
        // Before base.LoadComplete: it starts loading the settings overlay, which asks for the "Switch 2" section.
        loadSwitch2Config();
        base.LoadComplete();
        Program.Log("stage 3: OsuGame LoadComplete");
        importFromFolder(everything: true, byUser: false);
        nextImportTick = Clock.CurrentTime + 1000;
    }

    /// <summary>
    /// Beatmaps (.osz/.olz), skins (.osk) and replays (.osr) placed in the import folder (OUNSU_IMPORT_DIR, default
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
        string dir = Environment.GetEnvironmentVariable("OUNSU_IMPORT_DIR") ?? "/import";

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
}
