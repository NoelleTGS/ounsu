using System;
using System.Threading.Tasks;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.SignalR;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using osu.Game.Online;
using osu.Game.Online.Metadata;
using osu.Game.Users;

// Local stand-in for osu!'s metadata hub (see ../README.md). Stock SignalR MessagePack protocol with osu!'s resolver.
var builder = WebApplication.CreateBuilder(args);
builder.Logging.SetMinimumLevel(LogLevel.Warning);
builder.Services.AddSignalR(o => o.EnableDetailedErrors = true)
       .AddMessagePackProtocol(o => o.SerializerOptions = SignalRUnionWorkaroundResolver.OPTIONS);

var app = builder.Build();
app.MapHub<HubProbe.MetadataHub>("/metadata");
app.MapHub<HubProbe.RejectHub>("/reject");
Console.WriteLine("hub-server: ready");
app.Run();

namespace HubProbe
{
    public class MetadataHub : Hub<IMetadataClient>, IMetadataServer
    {
        private static void log(string text) => Console.WriteLine($"hub-server: {DateTime.Now:HH:mm:ss.fff} {text}");

        public override async Task OnConnectedAsync()
        {
            var http = Context.GetHttpContext()!;
            log($"/metadata connected (authorization \"{http.Request.Headers.Authorization}\", version hash \"{http.Request.Headers[HubClientConnector.VERSION_HASH_HEADER]}\")");

            await Clients.Caller.FriendPresenceUpdated(1002, new UserPresence
            {
                Status = UserStatus.Online,
                Activity = new UserActivity.InSoloGame { BeatmapID = 75, BeatmapDisplayTitle = "artist - title [diff]", RulesetID = 0, RulesetPlayingVerb = "Clicking circles" }
            });
            await Clients.Caller.DailyChallengeUpdated(new DailyChallengeInfo { RoomID = 1234567 });
            await Clients.Caller.BeatmapSetsUpdated(new BeatmapUpdates(new[] { 1, 2, 3 }, 1000));
        }

        public override Task OnDisconnectedAsync(Exception? exception)
        {
            log($"/metadata disconnected ({exception?.Message ?? "clean"})");
            return Task.CompletedTask;
        }

        public Task<BeatmapUpdates> GetChangesSince(int queueId)
        {
            log($"GetChangesSince({queueId})");
            return Task.FromResult(new BeatmapUpdates(new[] { queueId, queueId + 1 }, queueId + 2));
        }

        public Task UpdateActivity(UserActivity? activity)
        {
            log($"UpdateActivity({activity?.GetType().Name ?? "null"}: {activity?.GetStatus()} / {activity?.GetDetails()})");
            return Clients.Caller.UserPresenceUpdated(42, new UserPresence { Activity = activity, Status = UserStatus.Online });
        }

        public Task UpdateStatus(UserStatus? status)
        {
            log($"UpdateStatus({status?.ToString() ?? "null"})");
            return Clients.Caller.UserPresenceUpdated(43, status == null ? null : new UserPresence { Status = status });
        }

        public async Task BeginWatchingUserPresence()
        {
            log("BeginWatchingUserPresence()");
            await Clients.Caller.UserPresenceUpdated(2001, new UserPresence { Status = UserStatus.Online, Activity = new UserActivity.ChoosingBeatmap() });
            await Clients.Caller.UserPresenceUpdated(2002, new UserPresence { Status = UserStatus.DoNotDisturb, Activity = new UserActivity.InLobby { RoomID = 5, RoomName = "room" } });
            await Clients.Caller.UserPresenceUpdated(2003, new UserPresence { Status = UserStatus.Online });
            await Clients.Caller.UserPresenceUpdated(2003, null);
        }

        public Task EndWatchingUserPresence()
        {
            log("EndWatchingUserPresence()");
            return Task.CompletedTask;
        }

        public async Task<MultiplayerPlaylistItemStats[]> BeginWatchingMultiplayerRoom(long id)
        {
            log($"BeginWatchingMultiplayerRoom({id})");
            await Clients.Caller.MultiplayerRoomScoreSet(new MultiplayerRoomScoreSetEvent { RoomID = id, PlaylistItemID = 2, ScoreID = 3, UserID = 4, TotalScore = 999999, NewRank = 1 });
            return new[] { new MultiplayerPlaylistItemStats { PlaylistItemID = id, CumulativeScore = 123456789012 } };
        }

        public Task EndWatchingMultiplayerRoom(long id)
        {
            log($"EndWatchingMultiplayerRoom({id})");
            return Task.CompletedTask;
        }

        public Task RefreshFriends()
        {
            log("RefreshFriends()");
            return Task.CompletedTask;
        }
    }

    /// <summary>Drops every connection right after accepting it: what a client must not answer with a reconnect storm.</summary>
    public class RejectHub : Hub
    {
        public override Task OnConnectedAsync()
        {
            Console.WriteLine($"hub-server: {DateTime.Now:HH:mm:ss.fff} /reject connected, aborting it");
            Context.Abort();
            return Task.CompletedTask;
        }
    }
}
