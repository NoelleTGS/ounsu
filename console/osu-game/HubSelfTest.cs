using System;
using System.Buffers;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.CompilerServices;
using Microsoft.AspNetCore.SignalR;
using Microsoft.AspNetCore.SignalR.Client;
using osu.Framework.Logging;
using Microsoft.AspNetCore.SignalR.Protocol;
using osu.Game.Online;
using osu.Game.Online.Metadata;
using osu.Game.Users;

// OSU_HBXL_HUB_SELFTEST=<file>: checks osu.Game's HbxlMessagePackHubProtocol (the metadata hub's wire format without
// runtime code generation) offline. No network, nothing is sent anywhere.
//   - Where runtime code generation works (dotnet run / a JIT build): every test message is written with SignalR's stock
//     MessagePack protocol and with ours, the bytes must be equal, and the stock bytes are saved to <file>.
//   - Under NativeAOT (desktop publish or the console): our bytes must equal the ones in <file>, and the stock bytes are
//     parsed back with our protocol and written again, which must give the same bytes (so reading is right as well).
// New hub types: add them to HbxlHubTypes and a message here, regenerate the file with the JIT build.
static partial class Program
{
    private sealed class TestBinder : IInvocationBinder
    {
        private static readonly Dictionary<string, Type[]> parameters = new Dictionary<string, Type[]>
        {
            // client methods (IMetadataClient, IStatefulUserHubClient), as OnlineMetadataClient registers them
            { nameof(IMetadataClient.BeatmapSetsUpdated), new[] { typeof(BeatmapUpdates) } },
            { nameof(IMetadataClient.UserPresenceUpdated), new[] { typeof(int), typeof(UserPresence?) } },
            { nameof(IMetadataClient.FriendPresenceUpdated), new[] { typeof(int), typeof(UserPresence?) } },
            { nameof(IMetadataClient.DailyChallengeUpdated), new[] { typeof(DailyChallengeInfo?) } },
            { nameof(IMetadataClient.MultiplayerRoomScoreSet), new[] { typeof(MultiplayerRoomScoreSetEvent) } },
            { nameof(IStatefulUserHubClient.DisconnectRequested), Type.EmptyTypes },
            { nameof(IStatefulUserHubClient.ServerShuttingDown), Type.EmptyTypes },
            // server methods (IMetadataServer), so the messages the client sends can be read back too
            { nameof(IMetadataServer.GetChangesSince), new[] { typeof(int) } },
            { nameof(IMetadataServer.UpdateActivity), new[] { typeof(UserActivity) } },
            { nameof(IMetadataServer.UpdateStatus), new[] { typeof(UserStatus?) } },
            { nameof(IMetadataServer.BeginWatchingUserPresence), Type.EmptyTypes },
            { nameof(IMetadataServer.EndWatchingUserPresence), Type.EmptyTypes },
            { nameof(IMetadataServer.BeginWatchingMultiplayerRoom), new[] { typeof(long) } },
            { nameof(IMetadataServer.EndWatchingMultiplayerRoom), new[] { typeof(long) } },
            { nameof(IMetadataServer.RefreshFriends), Type.EmptyTypes },
        };

        // invocation id -> return type, for the completion messages below
        private static readonly Dictionary<string, Type> returns = new Dictionary<string, Type>
        {
            { "r-updates", typeof(BeatmapUpdates) },
            { "r-stats", typeof(MultiplayerPlaylistItemStats[]) },
        };

        public IReadOnlyList<Type> GetParameterTypes(string methodName) => parameters[methodName];
        public Type GetReturnType(string invocationId) => returns[invocationId];
        public Type GetStreamItemType(string streamId) => throw new NotSupportedException();
    }

    private static List<(string name, HubMessage message)> hubTestMessages()
    {
        var list = new List<(string, HubMessage)>();

        UserActivity[] activities =
        {
            new UserActivity.ChoosingBeatmap(),
            new UserActivity.InSoloGame { BeatmapID = 1695382, BeatmapDisplayTitle = "THE ORAL CIGARETTES - Mou Ii kai? [Rain]", RulesetID = 0, RulesetPlayingVerb = "Clicking circles" },
            new UserActivity.WatchingReplay { ScoreID = 3491548480, PlayerName = "someone", BeatmapID = 75, BeatmapDisplayTitle = "a - b [c]" },
            new UserActivity.WatchingReplay { ScoreID = 1, PlayerName = "no title", BeatmapID = 2 },
            new UserActivity.SpectatingUser { ScoreID = 5, PlayerName = "ユーザー", BeatmapID = 6, BeatmapDisplayTitle = "日本語 - タイトル [難しい]" },
            new UserActivity.SearchingForLobby(),
            new UserActivity.InLobby { RoomID = 123456789012, RoomName = "room" },
            new UserActivity.InMultiplayerGame { BeatmapID = 7, BeatmapDisplayTitle = "t", RulesetID = 3, RulesetPlayingVerb = "Smashing keys" },
            new UserActivity.SpectatingMultiplayerGame { BeatmapID = 8, BeatmapDisplayTitle = "t", RulesetID = 1, RulesetPlayingVerb = "Bashing drums" },
            new UserActivity.InPlaylistGame { BeatmapID = 9, BeatmapDisplayTitle = "t", RulesetID = 2, RulesetPlayingVerb = "Catching fruit" },
            new UserActivity.EditingBeatmap { BeatmapID = 10, BeatmapDisplayTitle = "edit" },
            new UserActivity.ModdingBeatmap { BeatmapID = 11, BeatmapDisplayTitle = "mod" },
            new UserActivity.TestingBeatmap { BeatmapID = 12, BeatmapDisplayTitle = "test" },
            new UserActivity.InDailyChallengeLobby(),
            new UserActivity.PlayingDailyChallenge { BeatmapID = 13, BeatmapDisplayTitle = "daily", RulesetID = 0, RulesetPlayingVerb = "Clicking circles" },
        };

        // client -> server, as HubConnection.InvokeAsync builds them
        int id = 0;
        foreach (var activity in activities)
            list.Add(($"UpdateActivity {activity.GetType().Name} #{id}", new InvocationMessage((++id).ToString(), nameof(IMetadataServer.UpdateActivity), new object?[] { activity })));
        list.Add(("UpdateActivity null", new InvocationMessage("100", nameof(IMetadataServer.UpdateActivity), new object?[] { null })));
        foreach (var status in new UserStatus?[] { UserStatus.Offline, UserStatus.DoNotDisturb, UserStatus.Online, null })
            list.Add(($"UpdateStatus {status?.ToString() ?? "null"}", new InvocationMessage("101", nameof(IMetadataServer.UpdateStatus), new object?[] { status })));
        list.Add(("GetChangesSince", new InvocationMessage("102", nameof(IMetadataServer.GetChangesSince), new object?[] { 123456 })));
        list.Add(("BeginWatchingUserPresence", new InvocationMessage("103", nameof(IMetadataServer.BeginWatchingUserPresence), Array.Empty<object?>())));
        list.Add(("EndWatchingUserPresence", new InvocationMessage("104", nameof(IMetadataServer.EndWatchingUserPresence), Array.Empty<object?>())));
        list.Add(("BeginWatchingMultiplayerRoom", new InvocationMessage("105", nameof(IMetadataServer.BeginWatchingMultiplayerRoom), new object?[] { 9876543210L })));
        list.Add(("EndWatchingMultiplayerRoom", new InvocationMessage("106", nameof(IMetadataServer.EndWatchingMultiplayerRoom), new object?[] { 5L })));
        list.Add(("RefreshFriends", new InvocationMessage("107", nameof(IMetadataServer.RefreshFriends), Array.Empty<object?>())));
        list.Add(("ping", PingMessage.Instance));
        list.Add(("close", new CloseMessage(null)));
        list.Add(("close with error", new CloseMessage("Server is shutting down.", true)));

        // server -> client
        id = 0;
        foreach (var activity in activities)
        {
            list.Add(($"UserPresenceUpdated {activity.GetType().Name} #{id++}", new InvocationMessage(nameof(IMetadataClient.UserPresenceUpdated),
                new object?[] { 1000 + id, new UserPresence { Activity = activity, Status = id % 2 == 0 ? UserStatus.Online : UserStatus.DoNotDisturb } })));
        }

        list.Add(("UserPresenceUpdated no activity", new InvocationMessage(nameof(IMetadataClient.UserPresenceUpdated), new object?[] { 2, new UserPresence { Status = UserStatus.Online } })));
        list.Add(("UserPresenceUpdated empty presence", new InvocationMessage(nameof(IMetadataClient.UserPresenceUpdated), new object?[] { 2, new UserPresence() })));
        list.Add(("UserPresenceUpdated null (went offline)", new InvocationMessage(nameof(IMetadataClient.UserPresenceUpdated), new object?[] { 34567890, null })));
        list.Add(("FriendPresenceUpdated", new InvocationMessage(nameof(IMetadataClient.FriendPresenceUpdated), new object?[] { 3, new UserPresence { Activity = activities[1], Status = UserStatus.Online } })));
        list.Add(("FriendPresenceUpdated null", new InvocationMessage(nameof(IMetadataClient.FriendPresenceUpdated), new object?[] { 3, null })));
        list.Add(("DailyChallengeUpdated", new InvocationMessage(nameof(IMetadataClient.DailyChallengeUpdated), new object?[] { new DailyChallengeInfo { RoomID = 1234567 } })));
        list.Add(("DailyChallengeUpdated null", new InvocationMessage(nameof(IMetadataClient.DailyChallengeUpdated), new object?[] { null })));
        list.Add(("BeatmapSetsUpdated", new InvocationMessage(nameof(IMetadataClient.BeatmapSetsUpdated), new object?[] { new BeatmapUpdates(new[] { 1, 2058976, 3 }, 7654321) })));
        list.Add(("BeatmapSetsUpdated empty", new InvocationMessage(nameof(IMetadataClient.BeatmapSetsUpdated), new object?[] { new BeatmapUpdates(Array.Empty<int>(), 0) })));
        list.Add(("MultiplayerRoomScoreSet", new InvocationMessage(nameof(IMetadataClient.MultiplayerRoomScoreSet),
            new object?[] { new MultiplayerRoomScoreSetEvent { RoomID = 1, PlaylistItemID = 2, ScoreID = 3, UserID = 4, TotalScore = 999999, NewRank = 5 } })));
        list.Add(("MultiplayerRoomScoreSet no rank", new InvocationMessage(nameof(IMetadataClient.MultiplayerRoomScoreSet),
            new object?[] { new MultiplayerRoomScoreSetEvent { RoomID = 1, PlaylistItemID = 2, ScoreID = 3, UserID = 4, TotalScore = 1 } })));
        list.Add(("DisconnectRequested", new InvocationMessage(nameof(IStatefulUserHubClient.DisconnectRequested), Array.Empty<object?>())));
        list.Add(("ServerShuttingDown", new InvocationMessage(nameof(IStatefulUserHubClient.ServerShuttingDown), Array.Empty<object?>())));
        list.Add(("completion void", CompletionMessage.Empty("1")));
        list.Add(("completion error", CompletionMessage.WithError("2", "An unexpected error occurred invoking 'UpdateStatus' on the server.")));
        list.Add(("completion BeatmapUpdates", CompletionMessage.WithResult("r-updates", new BeatmapUpdates(new[] { 5, 6 }, 99))));
        list.Add(("completion stats[]", CompletionMessage.WithResult("r-stats", new[]
        {
            new MultiplayerPlaylistItemStats { PlaylistItemID = 1, CumulativeScore = 123456789012, LastProcessedScoreID = 18446744073709551000 },
            new MultiplayerPlaylistItemStats { PlaylistItemID = 2, TotalScoreDistribution = new long[] { 1, 2, 3 } },
        })));
        list.Add(("completion empty stats[]", CompletionMessage.WithResult("r-stats", Array.Empty<MultiplayerPlaylistItemStats>())));

        return list;
    }

    static int HubSelfTest(string file)
    {
        var ours = new HbxlMessagePackHubProtocol();
        var messages = hubTestMessages();
        var binder = new TestBinder();
        int failures = 0;

        void fail(string text)
        {
            failures++;
            Log($"hub-selftest: FAIL {text}");
        }

        byte[]? write(string name, IHubProtocol protocol, HubMessage message, string which)
        {
            try
            {
                return protocol.GetMessageBytes(message).ToArray();
            }
            catch (Exception e)
            {
                fail($"{name}: {which} protocol could not write it: {describe(e)}");
                return null;
            }
        }

        if (RuntimeFeature.IsDynamicCodeSupported)
        {
            var stock = new MessagePackHubProtocol(Microsoft.Extensions.Options.Options.Create(new MessagePackHubProtocolOptions
            {
                SerializerOptions = SignalRUnionWorkaroundResolver.OPTIONS
            }));

            var lines = new List<string>();

            foreach (var (name, message) in messages)
            {
                byte[]? expected = write(name, stock, message, "stock");
                byte[]? actual = write(name, ours, message, "our");

                if (expected == null || actual == null)
                    continue;

                if (!expected.AsSpan().SequenceEqual(actual))
                    fail($"{name}: bytes differ\n  stock {Convert.ToHexString(expected)}\n  ours  {Convert.ToHexString(actual)}");
                else if (!roundTrip(name, ours, binder, expected, fail) || !roundTrip(name, stock, binder, expected, fail))
                    continue;
                else
                    Log($"hub-selftest: ok {name} ({expected.Length} bytes)");

                lines.Add($"{Convert.ToHexString(expected)} {name}");
            }

            File.WriteAllLines(file, lines);
            Log($"hub-selftest: wrote {lines.Count} stock message(s) to {file}");
        }
        else
        {
            var golden = File.ReadAllLines(file).Select(l => l.Split(' ', 2)).ToDictionary(p => p[1], p => Convert.FromHexString(p[0]));

            if (golden.Count != messages.Count)
                fail($"{file} has {golden.Count} message(s), this build has {messages.Count}: regenerate it with the JIT build");

            foreach (var (name, message) in messages)
            {
                if (!golden.TryGetValue(name, out byte[]? expected))
                {
                    fail($"{name}: not in {file}");
                    continue;
                }

                byte[]? actual = write(name, ours, message, "our");

                if (actual == null)
                    continue;

                if (!expected.AsSpan().SequenceEqual(actual))
                    fail($"{name}: bytes differ\n  stock {Convert.ToHexString(expected)}\n  ours  {Convert.ToHexString(actual)}");
                else if (roundTrip(name, ours, binder, expected, fail))
                    Log($"hub-selftest: ok {name} ({expected.Length} bytes)");
            }
        }

        // Two messages in one buffer and a message cut in half: the framing must hand out one at a time and wait for the rest.
        try
        {
            byte[] a = ours.GetMessageBytes(messages[1].message).ToArray();
            byte[] b = ours.GetMessageBytes(PingMessage.Instance).ToArray();
            var both = new ReadOnlySequence<byte>(a.Concat(b).ToArray());

            if (!ours.TryParseMessage(ref both, binder, out var first) || first is not InvocationMessage
                || !ours.TryParseMessage(ref both, binder, out var second) || second is not PingMessage || both.Length != 0)
                fail("framing: two messages in one buffer");

            var half = new ReadOnlySequence<byte>(a, 0, a.Length - 3);
            if (ours.TryParseMessage(ref half, binder, out _) || half.Length != a.Length - 3)
                fail("framing: an incomplete message was consumed");
            else
                Log("hub-selftest: ok framing");
        }
        catch (Exception e)
        {
            fail($"framing: {describe(e)}");
        }

        // A type that isn't listed has to fail as an exception (never a crash), and be counted.
        int before = HbxlHubTypes.Failures;

        try
        {
            ours.GetMessageBytes(new InvocationMessage("1", "Unknown", new object?[] { new Version(1, 2) }));
            fail("an unlisted type was written");
        }
        catch (NotSupportedException)
        {
            if (HbxlHubTypes.Failures == before + 1)
                Log("hub-selftest: ok unlisted type refused");
            else
                fail("unlisted type: failure not counted");
        }

        Log($"hub-selftest: done, {messages.Count} message(s), {failures} failure(s), dynamic code {(RuntimeFeature.IsDynamicCodeSupported ? "available (JIT: compared with the stock protocol)" : "unavailable (AOT: compared with the saved stock bytes)")}");
        return failures == 0 ? 0 : 1;
    }

    private static string describe(Exception e)
    {
        string text = $"{e.GetType().Name}: {e.Message}";
        for (var inner = e.InnerException; inner != null; inner = inner.InnerException)
            text += $" <- {inner.GetType().Name}: {inner.Message}";
        return text;
    }

    /// <summary>
    /// Parses <paramref name="bytes"/> with <paramref name="protocol"/> and writes the result again: must give the same bytes.
    /// </summary>
    private static bool roundTrip(string name, IHubProtocol protocol, IInvocationBinder binder, byte[] bytes, Action<string> fail)
    {
        try
        {
            var input = new ReadOnlySequence<byte>(bytes);

            if (!protocol.TryParseMessage(ref input, binder, out var parsed))
            {
                fail($"{name}: {protocol.GetType().Name} did not parse it");
                return false;
            }

            if (parsed is InvocationBindingFailureMessage bindingFailure)
            {
                fail($"{name}: {protocol.GetType().Name} could not bind the arguments: {describe(bindingFailure.BindingFailure.SourceException)}");
                return false;
            }

            if (parsed is CompletionMessage { Error: not null } completion && !name.Contains("error"))
            {
                fail($"{name}: {protocol.GetType().Name}: {completion.Error}");
                return false;
            }

            if (input.Length != 0)
            {
                fail($"{name}: {input.Length} byte(s) left over after parsing");
                return false;
            }

            byte[] again = protocol.GetMessageBytes(parsed).ToArray();

            if (!again.AsSpan().SequenceEqual(bytes))
            {
                fail($"{name}: {protocol.GetType().Name} read it, but wrote it back differently\n  in  {Convert.ToHexString(bytes)}\n  out {Convert.ToHexString(again)}");
                return false;
            }

            return true;
        }
        catch (Exception e)
        {
            fail($"{name}: {protocol.GetType().Name} threw while reading: {describe(e)}");
            return false;
        }
    }

    /// <summary>
    /// OSU_HBXL_HUB_LIVETEST=http://127.0.0.1:5123: the real connector (HubClientConnector, as OnlineMetadataClient uses it)
    /// against probes/hub-probe's local server. Never point this at anything but that server.
    /// </summary>
    static int HubLiveTest(string baseUrl)
    {
        if (!Uri.TryCreate(baseUrl, UriKind.Absolute, out var uri) || !uri.IsLoopback)
        {
            Log($"hub-livetest: FAIL {baseUrl} is not a loopback address; this test only talks to the local hub-probe server");
            return 1;
        }

        int failures = 0;
        var received = new System.Collections.Concurrent.ConcurrentQueue<string>();

        void fail(string text)
        {
            failures++;
            Log($"hub-livetest: FAIL {text}");
        }

        void expect(string what, int timeoutMs = 5000)
        {
            var watch = System.Diagnostics.Stopwatch.StartNew();

            while (watch.ElapsedMilliseconds < timeoutMs)
            {
                if (received.Contains(what))
                {
                    Log($"hub-livetest: ok received {what}");
                    return;
                }

                System.Threading.Thread.Sleep(20);
            }

            fail($"did not receive \"{what}\"; got: {string.Join(" | ", received)}");
        }

        static string show(UserPresence? p) => p == null ? "null" : $"{p.Value.Status?.ToString() ?? "-"}/{p.Value.Activity?.GetType().Name ?? "-"}/{p.Value.Activity?.GetStatus() ?? "-"}/{p.Value.Activity?.GetDetails() ?? "-"}";

        var api = new osu.Game.Online.API.DummyAPIAccess();

        // --- 1: every metadata hub method, both directions
        var connector = new HubClientConnector(nameof(OnlineMetadataClient), baseUrl + "/metadata", api, "selftest-hash");
        connector.ConfigureConnection = connection =>
        {
            connection.On<BeatmapUpdates>(nameof(IMetadataClient.BeatmapSetsUpdated), u => received.Enqueue($"BeatmapSetsUpdated [{string.Join(",", u.BeatmapSetIDs)}] {u.LastProcessedQueueID}"));
            connection.On<int, UserPresence?>(nameof(IMetadataClient.UserPresenceUpdated), (id, p) => received.Enqueue($"UserPresenceUpdated {id} {show(p)}"));
            connection.On<int, UserPresence?>(nameof(IMetadataClient.FriendPresenceUpdated), (id, p) => received.Enqueue($"FriendPresenceUpdated {id} {show(p)}"));
            connection.On<DailyChallengeInfo?>(nameof(IMetadataClient.DailyChallengeUpdated), i => received.Enqueue($"DailyChallengeUpdated {i?.RoomID.ToString() ?? "null"}"));
            connection.On<MultiplayerRoomScoreSetEvent>(nameof(IMetadataClient.MultiplayerRoomScoreSet), e => received.Enqueue($"MultiplayerRoomScoreSet {e.RoomID} {e.TotalScore} {e.NewRank}"));
        };

        try
        {
            var watch = System.Diagnostics.Stopwatch.StartNew();
            while (!connector.IsConnected.Value && watch.ElapsedMilliseconds < 10000)
                System.Threading.Thread.Sleep(20);

            if (!connector.IsConnected.Value)
            {
                fail("not connected after 10 s (is the hub-probe server running?)");
                return 1;
            }

            Log($"hub-livetest: connected in {watch.ElapsedMilliseconds} ms");
            var connection = connector.CurrentConnection!;

            expect("FriendPresenceUpdated 1002 Online/InSoloGame/Clicking circles/artist - title [diff]");
            expect("DailyChallengeUpdated 1234567");
            expect("BeatmapSetsUpdated [1,2,3] 1000");

            connection.InvokeAsync(nameof(IMetadataServer.UpdateActivity), new UserActivity.InSoloGame { BeatmapID = 9, BeatmapDisplayTitle = "a - b [c]", RulesetID = 0, RulesetPlayingVerb = "Clicking circles" }).GetAwaiter().GetResult();
            expect("UserPresenceUpdated 42 Online/InSoloGame/Clicking circles/a - b [c]");
            connection.InvokeAsync(nameof(IMetadataServer.UpdateActivity), new UserActivity.ChoosingBeatmap()).GetAwaiter().GetResult();
            expect("UserPresenceUpdated 42 Online/ChoosingBeatmap/Choosing a beatmap/-");
            connection.InvokeAsync(nameof(IMetadataServer.UpdateActivity), (UserActivity?)null).GetAwaiter().GetResult();
            expect("UserPresenceUpdated 42 Online/-/-/-");

            connection.InvokeAsync(nameof(IMetadataServer.UpdateStatus), (UserStatus?)UserStatus.DoNotDisturb).GetAwaiter().GetResult();
            expect("UserPresenceUpdated 43 DoNotDisturb/-/-/-");
            connection.InvokeAsync(nameof(IMetadataServer.UpdateStatus), (UserStatus?)null).GetAwaiter().GetResult();
            expect("UserPresenceUpdated 43 null");

            var updates = connection.InvokeAsync<BeatmapUpdates>(nameof(IMetadataServer.GetChangesSince), 500).GetAwaiter().GetResult();
            if (updates.LastProcessedQueueID == 502 && updates.BeatmapSetIDs.SequenceEqual(new[] { 500, 501 }))
                Log("hub-livetest: ok GetChangesSince result");
            else
                fail($"GetChangesSince returned [{string.Join(",", updates.BeatmapSetIDs)}] {updates.LastProcessedQueueID}");

            connection.InvokeAsync(nameof(IMetadataServer.BeginWatchingUserPresence)).GetAwaiter().GetResult();
            expect("UserPresenceUpdated 2001 Online/ChoosingBeatmap/Choosing a beatmap/-");
            expect("UserPresenceUpdated 2002 DoNotDisturb/InLobby/In a lobby/room");
            expect("UserPresenceUpdated 2003 Online/-/-/-");
            expect("UserPresenceUpdated 2003 null");
            connection.InvokeAsync(nameof(IMetadataServer.EndWatchingUserPresence)).GetAwaiter().GetResult();

            var stats = connection.InvokeAsync<MultiplayerPlaylistItemStats[]>(nameof(IMetadataServer.BeginWatchingMultiplayerRoom), 77L).GetAwaiter().GetResult();
            if (stats.Length == 1 && stats[0].PlaylistItemID == 77 && stats[0].CumulativeScore == 123456789012)
                Log("hub-livetest: ok BeginWatchingMultiplayerRoom result");
            else
                fail("BeginWatchingMultiplayerRoom result");
            expect("MultiplayerRoomScoreSet 77 999999 1");
            connection.InvokeAsync(nameof(IMetadataServer.EndWatchingMultiplayerRoom), 77L).GetAwaiter().GetResult();
            connection.InvokeAsync(nameof(IMetadataServer.RefreshFriends)).GetAwaiter().GetResult();
            Log("hub-livetest: ok all calls completed");

            // a method the server doesn't have: an error comes back, the connection stays
            try
            {
                connection.InvokeAsync("NoSuchMethod", 1).GetAwaiter().GetResult();
                fail("NoSuchMethod did not fail");
            }
            catch (HubException e)
            {
                Log($"hub-livetest: ok server error arrives as HubException ({e.Message})");
            }

            if (!connector.IsConnected.Value)
                fail("connection lost during the test");
        }
        catch (Exception e)
        {
            fail($"exception: {describe(e)}");
        }
        finally
        {
            api.SetState(osu.Game.Online.API.APIState.Offline);
            System.Threading.Thread.Sleep(500);
            connector.Dispose();
        }

        if (HbxlHubTypes.Failures != 0)
            fail($"{HbxlHubTypes.Failures} protocol failure(s) counted");

        // --- 2: a hub that drops every connection at once. Upstream would reconnect every ~3 s for ever; the brake
        // (PersistentEndpointClientConnector, HBXL_HUBS) allows the first connection, a second one after 3 s + 15 s,
        // and the third only after another minute. So: at most 2 connections in 45 s.
        int connects = 0;
        var started = System.Diagnostics.Stopwatch.StartNew();

        void count(LogEntry entry)
        {
            if (entry.Message.Contains("RejectHubTest connected!"))
                Log($"hub-livetest: /reject connection #{System.Threading.Interlocked.Increment(ref connects)} at {started.ElapsedMilliseconds / 1000.0:F1} s");
        }

        Logger.NewEntry += count;
        var api2 = new osu.Game.Online.API.DummyAPIAccess();
        var rejected = new HubClientConnector("RejectHubTest", baseUrl + "/reject", api2, "selftest-hash");
        System.Threading.Thread.Sleep(45000);
        api2.SetState(osu.Game.Online.API.APIState.Offline);
        System.Threading.Thread.Sleep(500);
        rejected.Dispose();
        Logger.NewEntry -= count;

        if (connects == 2)
            Log("hub-livetest: ok reconnect brake: 2 connections in 45 s to a hub that drops them at once");
        else
            fail($"reconnect brake: {connects} connection(s) in 45 s, expected 2");

        Log($"hub-livetest: done, {failures} failure(s)");
        return failures == 0 ? 0 : 1;
    }
}
