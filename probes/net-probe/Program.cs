using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Security;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

// Networking probe for the lx host on kaiHEN (TESTING.md test 5). Every stage logs before it runs (raw write(2),
// no System.Console), so the last "stage N" line in the nxlink log says where it stopped.
//
// Load, concurrency and throughput all go to tools/net-probe-server.py on the PC.  The only request to osu!'s
// servers is the last stage: one HTTPS GET, 5 s after everything else (NETPROBE_SKIP_OSU=1 skips it).
static partial class Program
{
    [LibraryImport("libc", EntryPoint = "write")]
    private static unsafe partial nint write(int fd, byte* buf, nint count);

    private static void Log(string s)
    {
        var bytes = Encoding.UTF8.GetBytes("net-probe: " + s + "\n");
        unsafe
        {
            fixed (byte* p = bytes) write(2, p, bytes.Length);
        }
    }

    private static readonly string host = Environment.GetEnvironmentVariable("NETPROBE_HOST") ?? "192.168.1.206";
    // Desktop runs: NETPROBE_ROOT=<folder with net-probe/ca.pem> (on the console "/" is the island's root; the CA comes
    // from the .nro's romfs).
    private static readonly string root = Environment.GetEnvironmentVariable("NETPROBE_ROOT") ?? "";
    private const int http_port = 39510, https_port = 39511, ws_port = 39512;
    private static int passed, failed;

    private static byte Pattern(long i) => (byte)(i * 131 + (i >> 9));

    private static void Stage(int n, string name, int timeoutSeconds, Func<Task<string>> body)
    {
        Log($"stage {n}: {name}");
        var sw = Stopwatch.StartNew();
        try
        {
            var task = Task.Run(body);
            if (!task.Wait(TimeSpan.FromSeconds(timeoutSeconds)))
            {
                failed++;
                Log($"stage {n}: FAIL timeout after {timeoutSeconds} s (left running)");
                return;
            }
            passed++;
            Log($"stage {n}: ok ({sw.ElapsedMilliseconds} ms) {task.Result}");
        }
        catch (Exception e)
        {
            failed++;
            var inner = e is AggregateException a && a.InnerException != null ? a.InnerException : e;
            Log($"stage {n}: FAIL ({sw.ElapsedMilliseconds} ms) {inner.GetType().Name}: {inner.Message}");
            for (var x = inner.InnerException; x != null; x = x.InnerException)
                Log($"stage {n}:   caused by {x.GetType().Name}: {x.Message}");
            Log($"stage {n}:   at {inner.StackTrace?.Split('\n').FirstOrDefault()?.Trim()}");
        }
    }

    private static X509Certificate2? probeCa;

    /// <summary>Trust the probe server's own CA (fsroot/net-probe/ca.pem), with a real chain build.</summary>
    private static bool ValidateProbeServer(object sender, X509Certificate? cert, X509Chain? _, SslPolicyErrors errors)
    {
        if (cert == null || probeCa == null) return false;
        using var chain = new X509Chain();
        chain.ChainPolicy.TrustMode = X509ChainTrustMode.CustomRootTrust;
        chain.ChainPolicy.CustomTrustStore.Add(probeCa);
        chain.ChainPolicy.RevocationMode = X509RevocationMode.NoCheck;
        bool ok = chain.Build(new X509Certificate2(cert));
        if (!ok) Log("  chain: " + string.Join(", ", chain.ChainStatus.Select(s => s.Status.ToString())));
        return ok && (errors & SslPolicyErrors.RemoteCertificateNameMismatch) == 0;
    }

    private static HttpClient ProbeClient(TimeSpan? timeout = null)
    {
        var handler = new SocketsHttpHandler
        {
            SslOptions = new SslClientAuthenticationOptions { RemoteCertificateValidationCallback = ValidateProbeServer },
            PooledConnectionLifetime = TimeSpan.FromMinutes(1),
        };
        var client = new HttpClient(handler) { Timeout = timeout ?? TimeSpan.FromSeconds(30) };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("osu-kaihen-net-probe/1.0");
        return client;
    }

    private static async Task<long> VerifyPattern(Stream s, long expected, Stream? copyTo = null)
    {
        var buf = new byte[32768];
        long at = 0;
        int n;
        while ((n = await s.ReadAsync(buf)) > 0)
        {
            for (int i = 0; i < n; i++)
                if (buf[i] != Pattern(at + i))
                    throw new InvalidDataException($"byte {at + i} is {buf[i]}, want {Pattern(at + i)}");
            if (copyTo != null) await copyTo.WriteAsync(buf.AsMemory(0, n));
            at += n;
        }
        if (at != expected) throw new InvalidDataException($"got {at} bytes, want {expected}");
        return at;
    }

    private static int Main()
    {
        Log($"start: server {host}, ports {http_port}/{https_port}/{ws_port}");

        Stage(0, "csprng + wall clock", 20, () =>
        {
            var a = RandomNumberGenerator.GetBytes(32);
            var b = RandomNumberGenerator.GetBytes(32);
            if (a.SequenceEqual(b) || a.All(x => x == a[0])) throw new Exception("RNG output repeats");
            var now = DateTime.UtcNow;
            if (now.Year < 2026) throw new Exception($"clock says {now:o}: certificates would fail");
            return Task.FromResult($"utc {now:o}, rng {Convert.ToHexString(a, 0, 6)}..");
        });

        Stage(1, "raw blocking TCP (Socket.Connect/Send/Receive)", 30, () =>
        {
            using var s = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
            s.Connect(IPAddress.Parse(host), http_port);
            s.Send(Encoding.ASCII.GetBytes("GET /hello HTTP/1.0\r\nHost: probe\r\n\r\n"));
            var sb = new StringBuilder();
            var buf = new byte[4096];
            int n;
            while ((n = s.Receive(buf)) > 0) sb.Append(Encoding.ASCII.GetString(buf, 0, n));
            string text = sb.ToString();
            if (!text.Contains("hello from net-probe-server")) throw new Exception("unexpected reply: " + text[..Math.Min(80, text.Length)]);
            return Task.FromResult($"{text.Length} bytes, local {s.LocalEndPoint}");
        });

        Stage(2, "DNS (console resolver via the island's getaddrinfo)", 30, () =>
        {
            var osu = Dns.GetHostAddresses("osu.ppy.sh");
            var numeric = Dns.GetHostAddresses(host);
            if (osu.Length == 0) throw new Exception("no addresses for osu.ppy.sh");
            return Task.FromResult($"osu.ppy.sh -> {string.Join(" ", osu.Select(x => x.ToString()))}; {host} -> {numeric[0]}");
        });

        Stage(3, "async socket, 4 MiB (SocketAsyncEngine / epoll)", 60, async () =>
        {
            using var s = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
            await s.ConnectAsync(new IPEndPoint(IPAddress.Parse(host), http_port));
            await s.SendAsync(Encoding.ASCII.GetBytes("GET /raw/4194304 HTTP/1.0\r\n\r\n"));
            var sw = Stopwatch.StartNew();
            await using var ns = new NetworkStream(s, ownsSocket: false);
            long got = await VerifyPattern(ns, 4194304);
            return $"{got} bytes, {got / 1048576.0 / sw.Elapsed.TotalSeconds:F1} MiB/s";
        });

        Stage(4, "HttpClient plain HTTP", 30, async () =>
        {
            using var c = ProbeClient();
            string body = await c.GetStringAsync($"http://{host}:{http_port}/hello");
            return body.Trim();
        });

        Stage(5, "HttpClient HTTPS to the PC (TLS + chain build, probe CA)", 60, async () =>
        {
            string caPath = root + "/net-probe/ca.pem";
            probeCa = X509Certificate2.CreateFromPem(File.ReadAllText(caPath));
            using var c = ProbeClient();
            var sw = Stopwatch.StartNew();
            string body = await c.GetStringAsync($"https://{host}:{https_port}/hello");
            long first = sw.ElapsedMilliseconds;
            sw.Restart();
            await c.GetStringAsync($"https://{host}:{https_port}/hello");
            return $"{body.Trim()} (first {first} ms incl. handshake, pooled {sw.ElapsedMilliseconds} ms)";
        });

        Stage(6, "10 concurrent HTTPS requests to the PC", 90, async () =>
        {
            using var c = ProbeClient();
            var sw = Stopwatch.StartNew();
            var tasks = Enumerable.Range(0, 10).Select(async i =>
            {
                await using var s = await c.GetStreamAsync($"https://{host}:{https_port}/bytes/{65536 + i}");
                return await VerifyPattern(s, 65536 + i);
            }).ToArray();
            var sizes = await Task.WhenAll(tasks);
            return $"{sizes.Sum()} bytes in {sw.ElapsedMilliseconds} ms";
        });

        Stage(7, "downloads: 20 MiB HTTP to /tmp on the SD card, 4 MiB HTTPS", 240, async () =>
        {
            using var c = ProbeClient(TimeSpan.FromMinutes(3));
            var sw = Stopwatch.StartNew();
            Directory.CreateDirectory(root + "/tmp");
            long got;
            await using (var file = new FileStream(root + "/tmp/net-probe.bin", FileMode.Create, FileAccess.Write, FileShare.None, 65536))
            await using (var s = await c.GetStreamAsync($"http://{host}:{http_port}/bytes/20971520"))
                got = await VerifyPattern(s, 20971520, file);
            double http = got / 1048576.0 / sw.Elapsed.TotalSeconds;
            sw.Restart();
            await using (var s = await c.GetStreamAsync($"https://{host}:{https_port}/bytes/4194304"))
                got = await VerifyPattern(s, 4194304);
            double https = got / 1048576.0 / sw.Elapsed.TotalSeconds;
            File.Delete(root + "/tmp/net-probe.bin");
            return $"HTTP {http:F1} MiB/s (written to the SD card), HTTPS {https:F1} MiB/s";
        });

        Stage(8, "WebSocket echo (ClientWebSocket, SignalR's transport)", 60, async () =>
        {
            using var ws = new ClientWebSocket();
            await ws.ConnectAsync(new Uri($"ws://{host}:{ws_port}/echo"), CancellationToken.None);
            var sizes = new[] { 5, 1000, 100_000 };
            foreach (int size in sizes)
            {
                var msg = new byte[size];
                for (int i = 0; i < size; i++) msg[i] = Pattern(i);
                await ws.SendAsync(msg, WebSocketMessageType.Binary, true, CancellationToken.None);
                var back = new MemoryStream();
                var buf = new byte[16384];
                WebSocketReceiveResult r;
                do
                {
                    r = await ws.ReceiveAsync(buf, CancellationToken.None);
                    back.Write(buf, 0, r.Count);
                } while (!r.EndOfMessage);
                if (!back.ToArray().SequenceEqual(msg)) throw new InvalidDataException($"echo of {size} bytes differs");
            }
            await ws.CloseAsync(WebSocketCloseStatus.NormalClosure, "done", CancellationToken.None);
            return $"echoed {string.Join("/", sizes)} bytes, state {ws.State}";
        });

        Stage(9, "timeout + cancellation (server stalls 5 s, client gives up at 1 s)", 30, async () =>
        {
            using var c = ProbeClient(TimeSpan.FromSeconds(1));
            var sw = Stopwatch.StartNew();
            try
            {
                await c.GetStringAsync($"http://{host}:{http_port}/slow");
            }
            catch (TaskCanceledException)
            {
                if (sw.ElapsedMilliseconds > 3000) throw new Exception($"cancellation took {sw.ElapsedMilliseconds} ms");
                return $"cancelled after {sw.ElapsedMilliseconds} ms";
            }
            throw new Exception("request did not time out");
        });

        Stage(10, "connection refused is reported (closed port)", 30, async () =>
        {
            using var s = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
            try
            {
                await s.ConnectAsync(new IPEndPoint(IPAddress.Parse(host), 39519));
            }
            catch (SocketException e)
            {
                return $"{e.SocketErrorCode}";
            }
            throw new Exception("connect to a closed port succeeded");
        });

        Stage(11, "socket pool: open up to 40 connections to the PC at once", 60, async () =>
        {
            var open = new System.Collections.Generic.List<Socket>();
            string stop = "none (all 40 connected)";
            try
            {
                for (int i = 0; i < 40; i++)
                {
                    var s = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
                    try
                    {
                        await s.ConnectAsync(new IPEndPoint(IPAddress.Parse(host), http_port));
                    }
                    catch (SocketException e)
                    {
                        s.Dispose();
                        stop = $"{e.SocketErrorCode} at #{i + 1}";
                        break;
                    }
                    open.Add(s);
                }
            }
            finally
            {
                foreach (var s in open) s.Dispose();
            }
            // the file server must still answer after the pool was exhausted
            File.WriteAllText(root + "/tmp/net-probe-pool.txt", $"{open.Count}\n");
            File.Delete(root + "/tmp/net-probe-pool.txt");
            if (open.Count < 8) throw new Exception($"only {open.Count} sockets (limit: {stop}); osu-framework uses up to 8 per host");
            return $"{open.Count} sockets open at once, stopped by: {stop}";
        });

        if (Environment.GetEnvironmentVariable("NETPROBE_SKIP_OSU") == "1")
        {
            Log("stage 12: skipped (NETPROBE_SKIP_OSU=1)");
        }
        else
        {
            Thread.Sleep(5000);
            Stage(12, "one HTTPS GET to osu.ppy.sh (system CA bundle /usr/local/ssl/cert.pem)", 60, async () =>
            {
                using var handler = new SocketsHttpHandler();
                using var c = new HttpClient(handler) { Timeout = TimeSpan.FromSeconds(30) };
                c.DefaultRequestHeaders.UserAgent.ParseAdd("osu-HBLauncher-net-probe/1.0");
                using var r = await c.GetAsync("https://osu.ppy.sh/api/v2/seasonal-backgrounds");
                string body = await r.Content.ReadAsStringAsync();
                var skew = r.Headers.Date.HasValue ? (DateTimeOffset.UtcNow - r.Headers.Date.Value).TotalSeconds : double.NaN;
                return $"HTTP {(int)r.StatusCode}, {body.Length} chars, HTTP/{r.Version}, clock skew {skew:F0} s";
            });
        }

        Log($"done: {passed} passed, {failed} failed");
        return failed == 0 ? 0 : 1;
    }
}
