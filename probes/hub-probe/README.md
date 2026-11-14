# hub-probe: a local stand-in for osu!'s metadata hub

Tests the port's SignalR path (HubClientConnector + HbxlMessagePackHubProtocol, see src/osu) against a real ASP.NET
Core SignalR server on this PC, with SignalR's stock MessagePack protocol on the server side. Nothing here talks to
ppy's servers. The server needs runtime code generation, so it is a normal (JIT) .NET program.

    sh run.sh            # builds the server (first time: downloads the ASP.NET runtime pack), runs it and the client test

The client is console/osu-game's desktop NativeAOT build (`obj/desktop-audio/osu-game`, see CLAUDE.md) started with
`OSU_HBXL_HUB_LIVETEST=http://127.0.0.1:5123`. It checks every metadata hub method in both directions, then connects
to `/reject` (a hub that drops every connection at once) and checks that the reconnect brake holds the client back.

The offline byte-for-byte comparison with the stock protocol is separate: `OSU_HBXL_HUB_SELFTEST=<file>` (HubSelfTest.cs).
