using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.IO.Pipes;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Windows.Forms;
using Microsoft.Win32.SafeHandles;
using System.Diagnostics;

namespace LocalifyDiscordConnector
{
    internal static class Program
    {
        [STAThread]
        private static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            ConnectorService service = new ConnectorService();
            ConnectorForm form = new ConnectorForm(service);
            service.Start();
            Application.Run(form);
            service.Dispose();
        }
    }

    internal sealed class PresenceData
    {
        public bool Playing;
        public bool Shuffle;
        public string Song = "";
        public string Artist = "";
        public string Album = "";
        public string CoverUrl = "";
        public string LocalCoverData = "";
        public string LocalCoverKey = "";
        public string Repeat = "off";
        public double Position;
        public double Duration;
        public double Volume = 0.9;
        public long Version;
        public PresenceData Copy()
        {
            return (PresenceData)MemberwiseClone();
        }
    }

    internal sealed class ServiceSnapshot
    {
        public PresenceData Presence;
        public bool BridgeOnline;
        public bool DiscordConnected;
        public bool Enabled;
        public bool KeepOnTop;
        public int DiscordState;
        public string DiscordStatus;
        public string BridgeStatus;
        public string ClientId;
    }

    internal sealed class ConnectorService : IDisposable
    {
        internal const int Port = 4219;
        internal const string DefaultClientId = "1550620411740561568";
        internal const string DefaultCover = "https://raw.githubusercontent.com/blibbbye/Localify/main/favicon.png";

        private readonly object gate = new object();
        private readonly JavaScriptSerializer json = NewSerializer();
        private readonly string defaultIniPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Localify Discord Player.ini");
        private string configPath;
        private string clientId = DefaultClientId;
        private bool enabled = true;
        private bool keepOnTop;
        private PresenceData presence = new PresenceData();
        private long version;
        private long controlSequence;
        private string controlAction = "";
        private double controlValue;
        private bool bridgeOnline;
        private string bridgeStatus = "Starting the Localify bridge…";
        private bool discordConnected;
        private int discordState;
        private string discordStatus = "Looking for Discord Desktop…";
        private volatile bool stopping;
        private int reconnectRequested;
        private TcpListener listener;
        private Thread bridgeThread;
        private Thread discordThread;
        private NamedPipeClientStream activePipe;
        private bool activityPublished;
        private string lastActivitySignature = "";
        private DateTime lastActivitySentUtc = DateTime.MinValue;
        private double lastActivityPosition;

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool PeekNamedPipe(SafePipeHandle pipe, IntPtr buffer, uint bufferSize, IntPtr bytesRead, out uint bytesAvailable, IntPtr bytesLeftThisMessage);

        public ConnectorService()
        {
            LoadSettings();
        }

        private static JavaScriptSerializer NewSerializer()
        {
            return new JavaScriptSerializer { MaxJsonLength = 16 * 1024 * 1024, RecursionLimit = 100 };
        }

        public void Start()
        {
            bridgeThread = new Thread(BridgeLoop) { IsBackground = true, Name = "Localify HTTP bridge" };
            discordThread = new Thread(DiscordLoop) { IsBackground = true, Name = "Localify Discord IPC" };
            bridgeThread.Start();
            discordThread.Start();
        }

        public ServiceSnapshot Snapshot()
        {
            lock (gate)
            {
                return new ServiceSnapshot
                {
                    Presence = presence.Copy(),
                    BridgeOnline = bridgeOnline,
                    DiscordConnected = discordConnected,
                    Enabled = enabled,
                    KeepOnTop = keepOnTop,
                    DiscordState = discordState,
                    DiscordStatus = discordStatus,
                    BridgeStatus = bridgeStatus,
                    ClientId = clientId
                };
            }
        }

        public void RequestReconnect()
        {
            Interlocked.Exchange(ref reconnectRequested, 1);
        }

        public void QueueControl(string action, double value)
        {
            lock (gate)
            {
                controlSequence++;
                controlAction = action;
                controlValue = value;
            }
        }

        public void SaveSettings(string newClientId, bool richPresenceEnabled, bool topMost)
        {
            string cleaned = (newClientId ?? "").Trim();
            if (!ValidClientId(cleaned))
                throw new ArgumentException("The Discord Application ID must be 17–20 digits.");

            lock (gate)
            {
                clientId = cleaned;
                enabled = richPresenceEnabled;
                keepOnTop = topMost;
                SaveSettingsFileLocked();
            }
            RequestReconnect();
        }

        public void SetEnabled(bool value)
        {
            lock (gate)
            {
                enabled = value;
                SaveSettingsFileLocked();
            }
            RequestReconnect();
        }

        private void LoadSettings()
        {
            string appDataPath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Localify", "Localify Discord Player.ini");
            configPath = File.Exists(defaultIniPath) ? defaultIniPath : appDataPath;
            try
            {
                if (!File.Exists(configPath)) return;
                bool inDiscord = false;
                foreach (string raw in File.ReadAllLines(configPath))
                {
                    string line = raw.Trim();
                    if (line.StartsWith("[") && line.EndsWith("]"))
                    {
                        inDiscord = string.Equals(line, "[Discord]", StringComparison.OrdinalIgnoreCase);
                        continue;
                    }
                    if (!inDiscord || line.Length == 0 || line.StartsWith(";") || line.StartsWith("#")) continue;
                    int eq = line.IndexOf('=');
                    if (eq <= 0) continue;
                    string key = line.Substring(0, eq).Trim();
                    string value = line.Substring(eq + 1).Trim();
                    if (key.Equals("ClientId", StringComparison.OrdinalIgnoreCase) && ValidClientId(value)) clientId = value;
                    else if (key.Equals("RichPresenceEnabled", StringComparison.OrdinalIgnoreCase)) enabled = !value.Equals("false", StringComparison.OrdinalIgnoreCase) && value != "0";
                    else if (key.Equals("KeepOnTop", StringComparison.OrdinalIgnoreCase)) keepOnTop = value.Equals("true", StringComparison.OrdinalIgnoreCase) || value == "1";
                }
            }
            catch { }
        }

        private void SaveSettingsFileLocked()
        {
            try
            {
                string dir = Path.GetDirectoryName(configPath);
                if (!Directory.Exists(dir)) Directory.CreateDirectory(dir);
                string text = "[Discord]\r\nClientId=" + clientId + "\r\nRichPresenceEnabled=" + (enabled ? "true" : "false") + "\r\nKeepOnTop=" + (keepOnTop ? "true" : "false") + "\r\n";
                File.WriteAllText(configPath, text, new UTF8Encoding(false));
            }
            catch
            {
                try
                {
                    configPath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Localify", "Localify Discord Player.ini");
                    Directory.CreateDirectory(Path.GetDirectoryName(configPath));
                    string text = "[Discord]\r\nClientId=" + clientId + "\r\nRichPresenceEnabled=" + (enabled ? "true" : "false") + "\r\nKeepOnTop=" + (keepOnTop ? "true" : "false") + "\r\n";
                    File.WriteAllText(configPath, text, new UTF8Encoding(false));
                }
                catch { }
            }
        }

        private static bool ValidClientId(string id)
        {
            if (id == null || id.Length < 17 || id.Length > 20) return false;
            foreach (char ch in id) if (ch < '0' || ch > '9') return false;
            return true;
        }

        private void SetBridgeStatus(bool online, string message)
        {
            lock (gate)
            {
                bridgeOnline = online;
                bridgeStatus = message;
            }
        }

        private void SetDiscordStatus(int state, string message, bool connected)
        {
            lock (gate)
            {
                discordState = state;
                discordStatus = message;
                discordConnected = connected;
            }
        }

        private void BridgeLoop()
        {
            TcpListener server = new TcpListener(IPAddress.Loopback, Port);
            listener = server;
            try
            {
                server.Start(16);
                SetBridgeStatus(true, "Web bridge ready on 127.0.0.1:" + Port);
            }
            catch (Exception ex)
            {
                SetBridgeStatus(false, ex is SocketException && ((SocketException)ex).SocketErrorCode == SocketError.AddressAlreadyInUse
                    ? "Port 4219 is already in use. Close older connector copies."
                    : "Could not start local bridge: " + ex.Message);
                return;
            }

            while (!stopping)
            {
                try
                {
                    TcpClient client = server.AcceptTcpClient();
                    client.NoDelay = true;
                    ThreadPool.QueueUserWorkItem(delegate { HandleHttpClient(client); });
                }
                catch (SocketException) { if (!stopping) Thread.Sleep(250); }
                catch (ObjectDisposedException) { break; }
                catch (Exception ex) { if (!stopping) SetBridgeStatus(false, "Bridge error: " + ex.Message); }
            }
            try { server.Stop(); } catch { }
            SetBridgeStatus(false, "Web bridge stopped");
        }

        private void HandleHttpClient(TcpClient client)
        {
            using (client)
            {
                try
                {
                    client.ReceiveTimeout = 5000;
                    client.SendTimeout = 5000;
                    NetworkStream stream = client.GetStream();
                    MemoryStream header = new MemoryStream();
                    int match = 0;
                    byte[] marker = new byte[] { 13, 10, 13, 10 };
                    while (header.Length < 65536)
                    {
                        int v = stream.ReadByte();
                        if (v < 0) return;
                        header.WriteByte((byte)v);
                        if ((byte)v == marker[match]) match++;
                        else match = ((byte)v == marker[0]) ? 1 : 0;
                        if (match == marker.Length) break;
                    }
                    byte[] headBytes = header.ToArray();
                    if (headBytes.Length < 4 || !(headBytes[headBytes.Length - 4] == 13 && headBytes[headBytes.Length - 3] == 10 && headBytes[headBytes.Length - 2] == 13 && headBytes[headBytes.Length - 1] == 10))
                    {
                        WriteResponse(stream, 400, "Bad Request", "text/plain", "Invalid HTTP headers");
                        return;
                    }
                    string head = Encoding.ASCII.GetString(headBytes);
                    string[] lines = head.Split(new string[] { "\r\n" }, StringSplitOptions.None);
                    if (lines.Length == 0) return;
                    string[] request = lines[0].Split(' ');
                    if (request.Length < 2) { WriteResponse(stream, 400, "Bad Request", "text/plain", "Invalid request"); return; }
                    string method = request[0].ToUpperInvariant();
                    string rawPath = request[1];
                    int contentLength = 0;
                    for (int i = 1; i < lines.Length; i++)
                    {
                        int sep = lines[i].IndexOf(':');
                        if (sep > 0 && lines[i].Substring(0, sep).Trim().Equals("Content-Length", StringComparison.OrdinalIgnoreCase))
                            int.TryParse(lines[i].Substring(sep + 1).Trim(), out contentLength);
                    }
                    if (contentLength < 0 || contentLength > 16 * 1024 * 1024)
                    {
                        WriteResponse(stream, 413, "Payload Too Large", "text/plain", "Presence data is too large");
                        return;
                    }
                    byte[] bodyBytes = new byte[contentLength];
                    int got = 0;
                    while (got < contentLength)
                    {
                        int n = stream.Read(bodyBytes, got, contentLength - got);
                        if (n <= 0) return;
                        got += n;
                    }

                    if (method == "OPTIONS")
                    {
                        WriteResponse(stream, 204, "No Content", "text/plain", "");
                        return;
                    }
                    string pathOnly = rawPath.Split('?')[0];
                    if (method == "GET" && pathOnly == "/health")
                    {
                        ServiceSnapshot s = Snapshot();
                        Dictionary<string, object> reply = new Dictionary<string, object>();
                        reply["ok"] = true;
                        reply["desktopPlayer"] = true;
                        reply["discordConnected"] = s.DiscordConnected;
                        reply["discordStatus"] = s.DiscordStatus;
                        reply["serverListening"] = s.BridgeOnline;
                        reply["port"] = Port;
                        reply["version"] = "6.0";
                        WriteResponse(stream, 200, "OK", "application/json; charset=utf-8", json.Serialize(reply));
                        return;
                    }
                    if (method == "GET" && pathOnly == "/control")
                    {
                        long since = 0;
                        int q = rawPath.IndexOf("since=", StringComparison.OrdinalIgnoreCase);
                        if (q >= 0)
                        {
                            string number = rawPath.Substring(q + 6).Split('&')[0];
                            long.TryParse(number, out since);
                        }
                        Dictionary<string, object> reply = new Dictionary<string, object>();
                        lock (gate)
                        {
                            if (controlSequence > since)
                            {
                                reply["seq"] = controlSequence;
                                reply["action"] = controlAction;
                                reply["value"] = controlValue;
                            }
                            else reply["seq"] = since;
                        }
                        WriteResponse(stream, 200, "OK", "application/json; charset=utf-8", json.Serialize(reply));
                        return;
                    }
                    if (method == "POST" && pathOnly == "/presence")
                    {
                        string body = Encoding.UTF8.GetString(bodyBytes);
                        Dictionary<string, object> payload = json.DeserializeObject(body) as Dictionary<string, object>;
                        if (payload == null) { WriteResponse(stream, 400, "Bad Request", "text/plain", "Invalid JSON"); return; }
                        PresenceData next = new PresenceData();
                        string kind = GetString(payload, "kind", "");
                        next.Playing = kind == "clear" ? false : GetBool(payload, "playing", false);
                        next.Song = Clean(GetString(payload, "song", ""), 128);
                        next.Artist = Clean(GetString(payload, "artist", ""), 128);
                        next.Album = Clean(GetString(payload, "album", ""), 128);
                        next.CoverUrl = GetString(payload, "coverUrl", "");
                        next.LocalCoverData = GetString(payload, "localCoverData", "");
                        next.LocalCoverKey = GetString(payload, "localCoverKey", "");
                        next.Position = Math.Max(0, GetNumber(payload, "currentTime", 0));
                        next.Duration = Math.Max(0, GetNumber(payload, "duration", 0));
                        next.Volume = Math.Max(0, Math.Min(1, GetNumber(payload, "volume", 0.9)));
                        next.Shuffle = GetBool(payload, "shuffle", false);
                        next.Repeat = GetString(payload, "repeat", "off");
                        if (next.Repeat != "all" && next.Repeat != "one") next.Repeat = "off";
                        lock (gate)
                        {
                            next.Version = ++version;
                            if (String.IsNullOrEmpty(next.LocalCoverData) && next.LocalCoverKey == presence.LocalCoverKey)
                            {
                                next.LocalCoverData = presence.LocalCoverData;
                            }
                            presence = next;
                        }
                        WriteResponse(stream, 204, "No Content", "text/plain", "");
                        return;
                    }
                    WriteResponse(stream, 404, "Not Found", "text/plain", "Not found");
                }
                catch (IOException) { }
                catch (Exception) { }
            }
        }

        private static void WriteResponse(NetworkStream stream, int code, string reason, string type, string body)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(body ?? "");
            StringBuilder head = new StringBuilder();
            head.Append("HTTP/1.1 ").Append(code).Append(' ').Append(reason).Append("\r\n");
            head.Append("Content-Type: ").Append(type).Append("\r\n");
            head.Append("Content-Length: ").Append(bytes.Length).Append("\r\n");
            head.Append("Cache-Control: no-store\r\n");
            head.Append("Access-Control-Allow-Origin: *\r\n");
            head.Append("Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n");
            head.Append("Access-Control-Allow-Headers: Content-Type\r\n");
            head.Append("Access-Control-Allow-Private-Network: true\r\n");
            head.Append("Connection: close\r\n\r\n");
            byte[] headers = Encoding.ASCII.GetBytes(head.ToString());
            stream.Write(headers, 0, headers.Length);
            if (bytes.Length > 0) stream.Write(bytes, 0, bytes.Length);
        }

        private static string Clean(string value, int limit)
        {
            if (value == null) return "";
            StringBuilder s = new StringBuilder();
            foreach (char ch in value)
            {
                if (ch == '\r' || ch == '\n' || ch == '\t' || ch < 32) s.Append(' ');
                else s.Append(ch);
            }
            string result = System.Text.RegularExpressions.Regex.Replace(s.ToString(), @"\s+", " ").Trim();
            return result.Length > limit ? result.Substring(0, limit) : result;
        }

        private static string GetString(Dictionary<string, object> d, string key, string fallback)
        {
            object v;
            return d != null && d.TryGetValue(key, out v) && v != null ? Convert.ToString(v, System.Globalization.CultureInfo.InvariantCulture) : fallback;
        }

        private static double GetNumber(Dictionary<string, object> d, string key, double fallback)
        {
            object v;
            if (d == null || !d.TryGetValue(key, out v) || v == null) return fallback;
            double n;
            return Double.TryParse(Convert.ToString(v, System.Globalization.CultureInfo.InvariantCulture), System.Globalization.NumberStyles.Any, System.Globalization.CultureInfo.InvariantCulture, out n) && !Double.IsNaN(n) && !Double.IsInfinity(n) ? n : fallback;
        }

        private static bool GetBool(Dictionary<string, object> d, string key, bool fallback)
        {
            object v;
            if (d == null || !d.TryGetValue(key, out v) || v == null) return fallback;
            if (v is bool) return (bool)v;
            bool b;
            if (Boolean.TryParse(Convert.ToString(v), out b)) return b;
            return fallback;
        }

        private void DiscordLoop()
        {
            string lastError = "";
            while (!stopping)
            {
                try
                {
                    if (Interlocked.Exchange(ref reconnectRequested, 0) != 0)
                    {
                        ClearAndDisposePipe();
                        lastActivitySignature = "";
                        SetDiscordStatus(0, "Reconnecting to Discord…", false);
                    }

                    bool localEnabled;
                    string localId;
                    lock (gate) { localEnabled = enabled; localId = clientId; }
                    if (!localEnabled)
                    {
                        if (activePipe != null) ClearAndDisposePipe();
                        SetDiscordStatus(7, "Rich Presence is paused in connector settings", false);
                        SleepResponsive(300);
                        continue;
                    }

                    if (activePipe == null || !activePipe.IsConnected)
                    {
                        if (!TryConnectDiscord(localId, out lastError))
                        {
                            SetDiscordStatus(lastError.StartsWith("Application ID rejected", StringComparison.OrdinalIgnoreCase) ? 4 : 3, lastError, false);
                            SleepResponsive(1800);
                            continue;
                        }
                        lastActivitySignature = "";
                        activityPublished = false;
                    }

                    PresenceData p;
                    lock (gate) p = presence.Copy();
                    DateTime now = DateTime.UtcNow;
                    if (p.Playing)
                    {
                        string signature = ActivitySignature(p);
                        double predictedPosition = lastActivityPosition;
                        if (lastActivitySentUtc != DateTime.MinValue)
                            predictedPosition += (now - lastActivitySentUtc).TotalSeconds;
                        bool seeked = lastActivitySentUtc != DateTime.MinValue && Math.Abs(p.Position - predictedPosition) > 5.0;
                        bool due = signature != lastActivitySignature || lastActivitySentUtc == DateTime.MinValue ||
                                   (now - lastActivitySentUtc).TotalSeconds >= 20 || seeked;
                        if (due)
                        {
                            string publishError;
                            bool ok = PublishActivity(p, out publishError);
                            if (ok)
                            {
                                lastActivitySignature = signature;
                                lastActivitySentUtc = DateTime.UtcNow;
                                lastActivityPosition = p.Position;
                                activityPublished = true;
                                SetDiscordStatus(1, String.IsNullOrEmpty(publishError) ? "Connected to Discord" : publishError, true);
                            }
                            else
                            {
                                if (publishError.IndexOf("closed", StringComparison.OrdinalIgnoreCase) >= 0 ||
                                    publishError.IndexOf("pipe", StringComparison.OrdinalIgnoreCase) >= 0 ||
                                    publishError.IndexOf("timed out", StringComparison.OrdinalIgnoreCase) >= 0)
                                    throw new IOException(publishError);
                                SetDiscordStatus(5, publishError, true);
                                lastActivitySignature = signature;
                                lastActivitySentUtc = DateTime.UtcNow;
                                lastActivityPosition = p.Position;
                            }
                        }
                    }
                    else if (activityPublished)
                    {
                        string clearError;
                        SendActivityClear(out clearError);
                        activityPublished = false;
                        lastActivitySignature = "";
                    }

                    int opcode;
                    string packet;
                    if (TryReadPacket(activePipe, 180, out opcode, out packet))
                    {
                        if (opcode == 3) SendPacket(activePipe, 4, packet);
                        else if (opcode == 2) throw new IOException("Discord closed the IPC connection: " + DescribeRpcError(packet));
                        else if (opcode == 1 && GetString(ParseJson(packet), "evt", "") == "ERROR")
                        {
                            string msg = DescribeRpcError(packet);
                            SetDiscordStatus(5, msg, true);
                        }
                    }
                }
                catch (Exception ex)
                {
                    string msg = ex.Message;
                    if (String.IsNullOrWhiteSpace(msg)) msg = "Discord IPC connection ended. Reconnecting…";
                    SetDiscordStatus(3, msg, false);
                    ClearAndDisposePipe();
                    activityPublished = false;
                    lastActivitySignature = "";
                    SleepResponsive(1400);
                }
            }
            ClearAndDisposePipe();
            SetDiscordStatus(0, "Connector stopped", false);
        }

        private void SleepResponsive(int ms)
        {
            int waited = 0;
            while (!stopping && waited < ms)
            {
                if (Interlocked.CompareExchange(ref reconnectRequested, 0, 0) != 0) return;
                int part = Math.Min(100, ms - waited);
                Thread.Sleep(part);
                waited += part;
            }
        }

        private bool TryConnectDiscord(string id, out string error)
        {
            error = "Discord Desktop not found. Open the Discord desktop app, then reconnect.";
            if (!ValidClientId(id))
            {
                error = "Application ID rejected: enter a 17–20 digit Discord Application ID in Settings.";
                return false;
            }
            bool foundPipe = false;
            string lastReason = "";
            for (int i = 0; i < 10 && !stopping; i++)
            {
                NamedPipeClientStream pipe = null;
                try
                {
                    SetDiscordStatus(0, "Searching for Discord Desktop…", false);
                    pipe = new NamedPipeClientStream(".", "discord-ipc-" + i, PipeDirection.InOut, PipeOptions.None);
                    try { pipe.Connect(240); }
                    catch (TimeoutException) { pipe.Dispose(); continue; }
                    catch (IOException) { pipe.Dispose(); continue; }
                    foundPipe = true;
                    SetDiscordStatus(0, "Discord IPC found; checking handshake…", false);
                    SendPacket(pipe, 0, "{\"v\":1,\"client_id\":\"" + id + "\"}");

                    DateTime deadline = DateTime.UtcNow.AddMilliseconds(1700);
                    while (!stopping && DateTime.UtcNow < deadline)
                    {
                        int opcode;
                        string payload;
                        if (!TryReadPacket(pipe, 180, out opcode, out payload)) continue;
                        if (opcode == 3) { SendPacket(pipe, 4, payload); continue; }
                        if (opcode == 2)
                        {
                            lastReason = DescribeRpcError(payload);
                            if (IsInvalidClientId(payload, true))
                            {
                                error = "Application ID rejected by Discord. Open Settings and paste the Application ID from your Discord Developer Portal.";
                                pipe.Dispose();
                                return false;
                            }
                            break;
                        }
                        if (opcode != 1) continue;
                        Dictionary<string, object> d = ParseJson(payload);
                        string evt = GetString(d, "evt", "");
                        if (evt == "READY")
                        {
                            activePipe = pipe;
                            SetDiscordStatus(1, "Connected to Discord", true);
                            error = "";
                            return true;
                        }
                        if (evt == "ERROR")
                        {
                            lastReason = DescribeRpcError(payload);
                            if (IsInvalidClientId(payload, false))
                            {
                                error = "Application ID rejected by Discord. Open Settings and paste the Application ID from your Discord Developer Portal.";
                                pipe.Dispose();
                                return false;
                            }
                            break;
                        }
                    }
                    pipe.Dispose();
                }
                catch (Exception ex)
                {
                    lastReason = ex.Message;
                    try { if (pipe != null) pipe.Dispose(); } catch { }
                }
            }
            error = foundPipe
                ? "Discord's IPC pipe opened but no READY reply arrived. " + (String.IsNullOrWhiteSpace(lastReason) ? "Verify the Application ID in Settings and make sure Discord Desktop is running." : lastReason)
                : "Discord Desktop not found. Open the Discord desktop app (not just the website), then click Reconnect.";
            return false;
        }

        private bool PublishActivity(PresenceData p, out string error)
        {
            string cover = ResolveDiscordCover(p);
            if (SendActivityVariant(p, cover, true, out error)) return true;

            // If a client rejects a custom/external artwork URL, retry with the
            // Localify fallback image and finally with the standard text-only payload.
            if (!String.IsNullOrEmpty(cover) && cover != DefaultCover &&
                error.IndexOf("rejected", StringComparison.OrdinalIgnoreCase) >= 0)
            {
                string retry;
                if (SendActivityVariant(p, DefaultCover, true, out retry))
                {
                    error = "Connected to Discord — using fallback artwork";
                    return true;
                }
                if (retry.IndexOf("rejected", StringComparison.OrdinalIgnoreCase) >= 0)
                {
                    if (SendActivityVariant(p, "", false, out retry))
                    {
                        error = "Connected to Discord — this Discord client rejected custom artwork URLs";
                        return true;
                    }
                }
                error = retry;
            }
            return false;
        }

        private bool SendActivityVariant(PresenceData p, string cover, bool includeCover, out string error)
        {
            Dictionary<string, object> activity = new Dictionary<string, object>();
            activity["type"] = 2;
            activity["details"] = String.IsNullOrWhiteSpace(p.Song) ? "Playing music" : p.Song;
            activity["state"] = String.IsNullOrWhiteSpace(p.Artist) ? "Unknown Artist" : p.Artist;
            activity["instance"] = false;
            if (includeCover && !String.IsNullOrWhiteSpace(cover))
            {
                activity["assets"] = new Dictionary<string, object>
                {
                    { "large_image", cover },
                    { "large_text", String.IsNullOrWhiteSpace(p.Album) ? "Localify artwork" : Clean(p.Album, 128) }
                };
            }
            long now = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
            long start = now - (long)Math.Max(0, p.Position);
            if (p.Duration > 0)
                activity["timestamps"] = new Dictionary<string, object> { { "start", start }, { "end", start + (long)p.Duration } };
            Dictionary<string, object> args = new Dictionary<string, object>
            {
                { "pid", Process.GetCurrentProcess().Id },
                { "activity", activity }
            };
            string nonce = Guid.NewGuid().ToString("N");
            Dictionary<string, object> command = new Dictionary<string, object>
            {
                { "cmd", "SET_ACTIVITY" },
                { "args", args },
                { "nonce", nonce }
            };
            return SendRpcCommand(json.Serialize(command), nonce, out error);
        }

        private bool SendActivityClear(out string error)
        {
            string nonce = Guid.NewGuid().ToString("N");
            Dictionary<string, object> command = new Dictionary<string, object>
            {
                { "cmd", "SET_ACTIVITY" },
                { "args", new Dictionary<string, object> { { "pid", Process.GetCurrentProcess().Id }, { "activity", null } } },
                { "nonce", nonce }
            };
            return SendRpcCommand(json.Serialize(command), nonce, out error);
        }

        private bool SendRpcCommand(string command, string nonce, out string error)
        {
            error = "";
            try
            {
                SendPacket(activePipe, 1, command);
                DateTime deadline = DateTime.UtcNow.AddMilliseconds(1800);
                while (!stopping && DateTime.UtcNow < deadline)
                {
                    int opcode;
                    string payload;
                    if (!TryReadPacket(activePipe, 220, out opcode, out payload)) continue;
                    if (opcode == 3) { SendPacket(activePipe, 4, payload); continue; }
                    if (opcode == 2)
                    {
                        error = "Discord closed the IPC connection: " + DescribeRpcError(payload);
                        ClearAndDisposePipe();
                        return false;
                    }
                    if (opcode != 1) continue;
                    Dictionary<string, object> d = ParseJson(payload);
                    string responseNonce = GetString(d, "nonce", "");
                    string evt = GetString(d, "evt", "");
                    if (evt == "ERROR" && (String.IsNullOrEmpty(responseNonce) || responseNonce == nonce))
                    {
                        error = "Discord rejected the activity: " + DescribeRpcError(payload);
                        return false;
                    }
                    if (responseNonce == nonce) return true;
                }
                error = "Discord did not confirm the activity update before the timeout.";
                return false;
            }
            catch (Exception ex)
            {
                error = "Discord IPC pipe error: " + ex.Message;
                return false;
            }
        }

        private static string ResolveDiscordCover(PresenceData p)
        {
            string url = p == null ? "" : (p.CoverUrl ?? "").Trim();
            Uri uri;
            if (url.Length <= 300 && Uri.TryCreate(url, UriKind.Absolute, out uri) &&
                uri.Scheme.Equals(Uri.UriSchemeHttps, StringComparison.OrdinalIgnoreCase))
                return url;
            return DefaultCover;
        }

        private static string ActivitySignature(PresenceData p)
        {
            return String.Join("\u001f", new string[]
            {
                p.Song ?? "", p.Artist ?? "", p.Album ?? "", p.CoverUrl ?? "",
                p.LocalCoverKey ?? "", p.Playing ? "1" : "0", p.Shuffle ? "1" : "0", p.Repeat ?? "off"
            });
        }

        private void ClearAndDisposePipe()
        {
            NamedPipeClientStream pipe = activePipe;
            activePipe = null;
            if (pipe != null)
            {
                try { if (pipe.IsConnected && activityPublished) SendPacket(pipe, 1, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + Process.GetCurrentProcess().Id + ",\"activity\":null},\"nonce\":\"shutdown\"}"); } catch { }
                try { pipe.Dispose(); } catch { }
            }
            lock (gate) discordConnected = false;
            activityPublished = false;
        }

        private static void SendPacket(NamedPipeClientStream pipe, int opcode, string payload)
        {
            if (pipe == null || !pipe.IsConnected) throw new IOException("Discord IPC pipe is not connected.");
            byte[] body = Encoding.UTF8.GetBytes(payload ?? "");
            byte[] header = new byte[8];
            Buffer.BlockCopy(BitConverter.GetBytes(opcode), 0, header, 0, 4);
            Buffer.BlockCopy(BitConverter.GetBytes(body.Length), 0, header, 4, 4);
            pipe.Write(header, 0, header.Length);
            if (body.Length > 0) pipe.Write(body, 0, body.Length);
            pipe.Flush();
        }

        private static bool TryReadPacket(NamedPipeClientStream pipe, int timeoutMs, out int opcode, out string payload)
        {
            opcode = 0;
            payload = "";
            if (pipe == null || !pipe.IsConnected) throw new IOException("Discord IPC pipe is disconnected.");
            DateTime deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
            uint available = 0;
            while (DateTime.UtcNow < deadline)
            {
                if (!PeekNamedPipe(pipe.SafePipeHandle, IntPtr.Zero, 0, IntPtr.Zero, out available, IntPtr.Zero))
                    throw new IOException("Discord IPC disconnected (Win32 " + Marshal.GetLastWin32Error() + ").");
                if (available >= 8) break;
                Thread.Sleep(12);
            }
            if (available < 8) return false;

            byte[] header = ReadExact(pipe, 8);
            opcode = BitConverter.ToInt32(header, 0);
            int length = BitConverter.ToInt32(header, 4);
            if (length < 0 || length > 2 * 1024 * 1024) throw new IOException("Discord sent an invalid IPC packet length (" + length + ").");
            if (length == 0) { payload = ""; return true; }

            deadline = DateTime.UtcNow.AddMilliseconds(Math.Max(timeoutMs, 1400));
            while (DateTime.UtcNow < deadline)
            {
                if (!PeekNamedPipe(pipe.SafePipeHandle, IntPtr.Zero, 0, IntPtr.Zero, out available, IntPtr.Zero))
                    throw new IOException("Discord IPC disconnected while reading a packet (Win32 " + Marshal.GetLastWin32Error() + ").");
                if (available >= (uint)length) break;
                Thread.Sleep(8);
            }
            if (available < (uint)length) throw new IOException("Discord IPC packet timed out while reading its payload.");
            payload = Encoding.UTF8.GetString(ReadExact(pipe, length));
            return true;
        }

        private static byte[] ReadExact(NamedPipeClientStream pipe, int length)
        {
            byte[] data = new byte[length];
            int offset = 0;
            while (offset < length)
            {
                int n = pipe.Read(data, offset, length - offset);
                if (n <= 0) throw new IOException("Discord closed the IPC pipe.");
                offset += n;
            }
            return data;
        }

        private static Dictionary<string, object> ParseJson(string source)
        {
            try { return NewSerializer().DeserializeObject(source) as Dictionary<string, object> ?? new Dictionary<string, object>(); }
            catch { return new Dictionary<string, object>(); }
        }

        private static string DescribeRpcError(string payload)
        {
            Dictionary<string, object> d = ParseJson(payload);
            object codeObj;
            object messageObj;
            string code = "";
            string message = "";
            if (d.TryGetValue("code", out codeObj) && codeObj != null) code = Convert.ToString(codeObj);
            if (d.TryGetValue("message", out messageObj) && messageObj != null) message = Convert.ToString(messageObj);
            object dataObj;
            Dictionary<string, object> data = d.TryGetValue("data", out dataObj) ? dataObj as Dictionary<string, object> : null;
            if (data != null)
            {
                if (String.IsNullOrEmpty(code) && data.TryGetValue("code", out codeObj) && codeObj != null) code = Convert.ToString(codeObj);
                if (String.IsNullOrEmpty(message) && data.TryGetValue("message", out messageObj) && messageObj != null) message = Convert.ToString(messageObj);
            }
            if (!String.IsNullOrEmpty(code) && !String.IsNullOrEmpty(message)) return "Discord RPC " + code + ": " + message;
            if (!String.IsNullOrEmpty(code)) return "Discord RPC error " + code;
            if (!String.IsNullOrEmpty(message)) return message;
            return "no response detail was provided";
        }

        private static bool IsInvalidClientId(string payload, bool isClose)
        {
            string lower = (payload ?? "").ToLowerInvariant();
            string detail = DescribeRpcError(payload).ToLowerInvariant();
            // CLOSE 4000 historically identifies a bad client ID; ERROR 4000 is generally an invalid payload.
            if (isClose && (lower.Contains("\"code\":4000") || lower.Contains("\"code\": 4000"))) return true;
            if (lower.Contains("\"code\":4007") || lower.Contains("\"code\": 4007")) return true;
            return detail.Contains("invalid client id") || detail.Contains("invalid client_id");
        }

        public string Diagnostics()
        {
            ServiceSnapshot s = Snapshot();
            return "Localify Discord Connector 6.0\r\n\r\nDiscord: " + s.DiscordStatus +
                   "\r\nIPC: " + (s.DiscordConnected ? "connected" : "disconnected") +
                   "\r\nApplication ID: " + s.ClientId +
                   "\r\nWeb bridge: " + s.BridgeStatus +
                   "\r\nBridge endpoint: http://127.0.0.1:" + Port +
                   "\r\nSettings file: " + configPath +
                   "\r\n\r\nIf Discord reports an invalid Application ID, open Settings and paste the ID from Developer Portal > General Information.";
        }

        public void Dispose()
        {
            stopping = true;
            try { if (listener != null) listener.Stop(); } catch { }
            try { if (discordThread != null && discordThread.IsAlive) discordThread.Join(1500); } catch { }
            try { if (bridgeThread != null && bridgeThread.IsAlive) bridgeThread.Join(1000); } catch { }
        }
    }

    internal sealed class ConnectorForm : Form
    {
        private static readonly Color Bg = Color.FromArgb(7, 10, 14);
        private static readonly Color Panel = Color.FromArgb(15, 20, 27);
        private static readonly Color Panel2 = Color.FromArgb(20, 27, 36);
        private static readonly Color Line = Color.FromArgb(39, 49, 62);
        private static readonly Color Muted = Color.FromArgb(139, 151, 169);
        private static readonly Color Accent = Color.FromArgb(94, 174, 247);
        private readonly ConnectorService service;
        private readonly PictureBox art = new PictureBox();
        private readonly Label title = new Label();
        private readonly Label artist = new Label();
        private readonly Label album = new Label();
        private readonly Label playback = new Label();
        private readonly Label elapsed = new Label();
        private readonly Label duration = new Label();
        private readonly Label discordStatus = new Label();
        private readonly Label bridgeStatus = new Label();
        private readonly Label headerStatus = new Label();
        private readonly TrackBar seek = new TrackBar();
        private readonly TrackBar volume = new TrackBar();
        private readonly Button play = new Button();
        private readonly Button shuffle = new Button();
        private readonly Button repeat = new Button();
        private readonly System.Windows.Forms.Timer refreshTimer = new System.Windows.Forms.Timer();
        private string artworkKey = "";
        private bool changingControls;
        private bool seekDragging;
        private Bitmap fallbackArt;

        public ConnectorForm(ConnectorService service)
        {
            this.service = service;
            Text = "Localify Discord Connector";
            StartPosition = FormStartPosition.CenterScreen;
            Size = new Size(1000, 690);
            MinimumSize = new Size(850, 600);
            BackColor = Bg;
            ForeColor = Color.White;
            Font = new Font("Segoe UI", 9F, FontStyle.Regular);
            DoubleBuffered = true;
            BuildUi();
            fallbackArt = MakeFallbackArt();
            art.Image = fallbackArt;
            refreshTimer.Interval = 350;
            refreshTimer.Tick += delegate { RefreshFromService(); };
            refreshTimer.Start();
            RefreshFromService();
        }

        private void BuildUi()
        {
            TableLayoutPanel root = new TableLayoutPanel();
            root.Dock = DockStyle.Fill;
            root.Margin = Padding.Empty;
            root.Padding = new Padding(16);
            root.BackColor = Bg;
            root.ColumnCount = 1;
            root.RowCount = 3;
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 72));
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 112));
            Controls.Add(root);

            Panel header = new Panel { Dock = DockStyle.Fill, BackColor = Bg, Margin = new Padding(0, 0, 0, 10) };
            root.Controls.Add(header, 0, 0);
            Panel logo = new Panel { Location = new Point(4, 12), Size = new Size(44, 44), BackColor = Accent };
            logo.Paint += delegate(object sender, PaintEventArgs e)
            {
                e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
                using (SolidBrush b = new SolidBrush(Accent)) e.Graphics.FillEllipse(b, 0, 0, 43, 43);
                using (Font f = new Font("Segoe UI Symbol", 20, FontStyle.Bold))
                using (SolidBrush b = new SolidBrush(Color.FromArgb(7, 18, 30)))
                    e.Graphics.DrawString("♫", f, b, new PointF(7, 4));
            };
            header.Controls.Add(logo);
            Label brand = MakeLabel("Localify Discord Connector", 59, 10, 400, 27, 20, Color.White, true);
            header.Controls.Add(brand);
            Label sub = MakeLabel("A simple companion for your Localify music player", 61, 39, 420, 19, 9, Muted, false);
            header.Controls.Add(sub);
            headerStatus = MakeLabel("CONNECTING", 0, 23, 220, 24, 10, Color.FromArgb(245, 197, 111), true);
            headerStatus.TextAlign = ContentAlignment.MiddleRight;
            headerStatus.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            header.Controls.Add(headerStatus);
            header.Resize += delegate { headerStatus.Left = header.ClientSize.Width - headerStatus.Width - 8; };

            TableLayoutPanel main = new TableLayoutPanel();
            main.Dock = DockStyle.Fill;
            main.BackColor = Panel;
            main.Padding = new Padding(18);
            main.Margin = new Padding(0, 0, 0, 12);
            main.ColumnCount = 2;
            main.RowCount = 1;
            main.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 270));
            main.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            main.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            main.Paint += delegate(object sender, PaintEventArgs e)
            {
                using (Pen pen = new Pen(Line)) e.Graphics.DrawRectangle(pen, 0, 0, main.ClientSize.Width - 1, main.ClientSize.Height - 1);
            };
            root.Controls.Add(main, 0, 1);

            Panel coverColumn = new Panel { Dock = DockStyle.Fill, BackColor = Panel, Margin = new Padding(0, 0, 16, 0) };
            main.Controls.Add(coverColumn, 0, 0);
            art.Location = new Point(0, 0);
            art.Size = new Size(244, 244);
            art.SizeMode = PictureBoxSizeMode.Zoom;
            art.BackColor = Panel2;
            coverColumn.Controls.Add(art);
            Label artCaption = MakeLabel("ALBUM ARTWORK", 2, 254, 235, 18, 8, Muted, true);
            coverColumn.Controls.Add(artCaption);
            Label artSub = MakeLabel("Artwork follows the current Localify track", 2, 276, 238, 35, 8, Muted, false);
            artSub.MaximumSize = new Size(238, 0);
            coverColumn.Controls.Add(artSub);

            Panel info = new Panel { Dock = DockStyle.Fill, BackColor = Panel, Margin = Padding.Empty };
            main.Controls.Add(info, 1, 0);
            Label kicker = MakeLabel("NOW PLAYING", 0, 1, 280, 16, 9, Accent, true);
            info.Controls.Add(kicker);
            title = MakeLabel("Nothing playing", 0, 26, 600, 40, 24, Color.White, true);
            title.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            info.Controls.Add(title);
            artist = MakeLabel("Play a song in Localify to see it here", 1, 70, 600, 25, 12, Color.FromArgb(200, 210, 223), false);
            artist.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            info.Controls.Add(artist);
            album = MakeLabel("Local music library", 1, 96, 600, 20, 10, Muted, false);
            album.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            info.Controls.Add(album);

            playback = MakeLabel("●  WAITING FOR LOCALIFY", 0, 128, 240, 24, 9, Muted, true);
            playback.TextAlign = ContentAlignment.MiddleLeft;
            info.Controls.Add(playback);

            elapsed = MakeLabel("0:00", 0, 163, 48, 18, 8, Muted, false);
            info.Controls.Add(elapsed);
            duration = MakeLabel("0:00", 0, 163, 55, 18, 8, Muted, false);
            duration.TextAlign = ContentAlignment.TopRight;
            duration.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            info.Controls.Add(duration);
            seek.Minimum = 0;
            seek.Maximum = 1000;
            seek.TickStyle = TickStyle.None;
            seek.Height = 32;
            seek.Location = new Point(0, 177);
            seek.Width = 500;
            seek.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            seek.BackColor = Panel;
            seek.Scroll += delegate
            {
                if (changingControls) return;
                PresenceData p = service.Snapshot().Presence;
                if (p.Duration > 0) service.QueueControl("seek", (seek.Value / 1000.0) * p.Duration);
            };
            seek.MouseDown += delegate { seekDragging = true; };
            seek.MouseUp += delegate { seekDragging = false; };
            info.Controls.Add(seek);

            FlowLayoutPanel transport = new FlowLayoutPanel { Location = new Point(0, 220), Height = 43, Width = 430, Anchor = AnchorStyles.Top | AnchorStyles.Left, BackColor = Panel, WrapContents = false, Margin = Padding.Empty, Padding = Padding.Empty };
            transport.Controls.Add(MakeButton("PREVIOUS", 82, false, delegate { service.QueueControl("prev", 0); }));
            play = MakeButton("▶  PLAY", 108, true, delegate { service.QueueControl("toggle", 0); });
            transport.Controls.Add(play);
            transport.Controls.Add(MakeButton("NEXT", 82, false, delegate { service.QueueControl("next", 0); }));
            info.Controls.Add(transport);

            FlowLayoutPanel tools = new FlowLayoutPanel { Location = new Point(0, 278), Height = 62, Width = 620, Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right, BackColor = Panel, WrapContents = false, Margin = Padding.Empty, Padding = Padding.Empty };
            shuffle = MakeButton("SHUFFLE", 90, false, delegate { service.QueueControl("shuffle", 0); });
            repeat = MakeButton("REPEAT OFF", 105, false, delegate { service.QueueControl("repeat", 0); });
            tools.Controls.Add(shuffle);
            tools.Controls.Add(repeat);
            Label volLabel = MakeLabel("VOLUME", 8, 10, 57, 20, 8, Muted, true);
            tools.Controls.Add(volLabel);
            volume.Minimum = 0; volume.Maximum = 100; volume.Value = 90; volume.TickStyle = TickStyle.None;
            volume.Width = 150; volume.Height = 36; volume.BackColor = Panel; volume.Scroll += delegate
            {
                if (!changingControls) service.QueueControl("volume", volume.Value / 100.0);
            };
            tools.Controls.Add(volume);
            info.Controls.Add(tools);

            Panel footer = new Panel { Dock = DockStyle.Fill, BackColor = Panel, Margin = Padding.Empty, Padding = new Padding(14, 10, 12, 8) };
            footer.Paint += delegate(object sender, PaintEventArgs e)
            {
                using (Pen pen = new Pen(Line)) e.Graphics.DrawRectangle(pen, 0, 0, footer.ClientSize.Width - 1, footer.ClientSize.Height - 1);
            };
            root.Controls.Add(footer, 0, 2);
            Label connectionTitle = MakeLabel("CONNECTION", 14, 8, 125, 16, 8, Accent, true);
            footer.Controls.Add(connectionTitle);
            discordStatus = MakeLabel("Looking for Discord Desktop…", 14, 28, 400, 20, 10, Color.FromArgb(236, 207, 146), true);
            discordStatus.Anchor = AnchorStyles.Left | AnchorStyles.Top | AnchorStyles.Right;
            footer.Controls.Add(discordStatus);
            bridgeStatus = MakeLabel("Starting local web bridge…", 14, 51, 425, 20, 8, Muted, false);
            bridgeStatus.Anchor = AnchorStyles.Left | AnchorStyles.Top | AnchorStyles.Right;
            footer.Controls.Add(bridgeStatus);

            FlowLayoutPanel actions = new FlowLayoutPanel { AutoSize = false, Height = 42, Width = 570, WrapContents = false, FlowDirection = FlowDirection.LeftToRight, Anchor = AnchorStyles.Top | AnchorStyles.Right, BackColor = Panel, Location = new Point(0, 28), Margin = Padding.Empty };
            actions.Controls.Add(MakeButton("OPEN DISCORD", 112, false, OpenDiscord));
            actions.Controls.Add(MakeButton("RECONNECT", 104, true, delegate { service.RequestReconnect(); }));
            actions.Controls.Add(MakeButton("SETTINGS", 94, false, OpenSettings));
            actions.Controls.Add(MakeButton("COPY DETAILS", 112, false, CopyDetails));
            footer.Controls.Add(actions);
            footer.Resize += delegate { actions.Left = Math.Max(440, footer.ClientSize.Width - actions.Width - 10); };
            main.Resize += delegate
            {
                int infoWidth = info.ClientSize.Width;
                title.Width = Math.Max(250, infoWidth - 8);
                artist.Width = Math.Max(250, infoWidth - 8);
                album.Width = Math.Max(250, infoWidth - 8);
                seek.Width = Math.Max(260, infoWidth - 55);
                duration.Left = Math.Max(220, seek.Right - 55);
                tools.Width = Math.Max(400, infoWidth);
            };
        }

        private Label MakeLabel(string text, int x, int y, int width, int height, float size, Color color, bool bold)
        {
            return new Label
            {
                AutoSize = false,
                Text = text,
                Location = new Point(x, y),
                Size = new Size(width, height),
                ForeColor = color,
                BackColor = Color.Transparent,
                Font = new Font("Segoe UI", size, bold ? FontStyle.Bold : FontStyle.Regular),
                TextAlign = ContentAlignment.MiddleLeft,
                AutoEllipsis = true
            };
        }

        private Button MakeButton(string text, int width, bool primary, Action click)
        {
            Button button = new Button();
            button.Text = text;
            button.Width = width;
            button.Height = 36;
            button.Margin = new Padding(0, 0, 8, 0);
            button.FlatStyle = FlatStyle.Flat;
            button.FlatAppearance.BorderSize = 1;
            button.FlatAppearance.BorderColor = primary ? Accent : Line;
            button.BackColor = primary ? Accent : Panel2;
            button.ForeColor = primary ? Color.FromArgb(7, 16, 25) : Color.FromArgb(227, 233, 241);
            button.Font = new Font("Segoe UI", 8.2F, FontStyle.Bold);
            button.Cursor = Cursors.Hand;
            button.Click += delegate { click(); };
            button.MouseEnter += delegate { if (button.Enabled) button.BackColor = primary ? Color.FromArgb(131, 196, 252) : Color.FromArgb(30, 40, 52); };
            button.MouseLeave += delegate { button.BackColor = primary ? Accent : Panel2; };
            return button;
        }

        private Bitmap MakeFallbackArt()
        {
            Bitmap b = new Bitmap(300, 300);
            using (Graphics g = Graphics.FromImage(b))
            using (LinearGradientBrush grad = new LinearGradientBrush(new Rectangle(0, 0, 300, 300), Color.FromArgb(24, 48, 73), Color.FromArgb(9, 14, 22), 135f))
            using (Font f = new Font("Segoe UI", 58, FontStyle.Bold))
            using (SolidBrush white = new SolidBrush(Color.FromArgb(220, 230, 245)))
            using (SolidBrush glow = new SolidBrush(Color.FromArgb(32, 95, 171, 229)))
            using (StringFormat sf = new StringFormat { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center })
            {
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.FillRectangle(grad, 0, 0, 300, 300);
                g.FillEllipse(glow, 24, 20, 250, 250);
                g.DrawString("♫", f, white, new RectangleF(0, 18, 300, 240), sf);
                using (Font small = new Font("Segoe UI", 11, FontStyle.Bold))
                using (SolidBrush dim = new SolidBrush(Color.FromArgb(160, 190, 210, 232)))
                    g.DrawString("LOCALIFY", small, dim, new PointF(99, 258));
            }
            return b;
        }

        private void RefreshFromService()
        {
            if (IsDisposed) return;
            ServiceSnapshot s = service.Snapshot();
            PresenceData p = s.Presence;
            headerStatus.Text = s.DiscordConnected ? "●  DISCORD CONNECTED" : (s.DiscordState == 7 ? "●  PRESENCE PAUSED" : "●  DISCONNECTED");
            headerStatus.ForeColor = s.DiscordConnected ? Color.FromArgb(111, 225, 169) : (s.DiscordState == 4 || s.DiscordState == 5 ? Color.FromArgb(255, 133, 124) : Color.FromArgb(245, 197, 111));
            discordStatus.Text = s.DiscordStatus;
            discordStatus.ForeColor = s.DiscordConnected ? Color.FromArgb(133, 225, 176) : (s.DiscordState == 4 || s.DiscordState == 5 || s.DiscordState == 6 ? Color.FromArgb(255, 143, 134) : Color.FromArgb(240, 201, 135));
            bridgeStatus.Text = s.BridgeOnline ? "Localify bridge online  •  127.0.0.1:" + ConnectorService.Port : s.BridgeStatus;
            bridgeStatus.ForeColor = s.BridgeOnline ? Muted : Color.FromArgb(255, 157, 137);
            play.Text = p.Playing ? "Ⅱ  PAUSE" : "▶  PLAY";
            shuffle.BackColor = p.Shuffle ? Color.FromArgb(29, 56, 73) : Panel2;
            shuffle.FlatAppearance.BorderColor = p.Shuffle ? Accent : Line;
            repeat.Text = p.Repeat == "one" ? "REPEAT ONE" : (p.Repeat == "all" ? "REPEAT ALL" : "REPEAT OFF");
            repeat.BackColor = p.Repeat == "off" ? Panel2 : Color.FromArgb(29, 56, 73);
            repeat.FlatAppearance.BorderColor = p.Repeat == "off" ? Line : Accent;
            title.Text = String.IsNullOrWhiteSpace(p.Song) ? "Nothing playing" : p.Song;
            artist.Text = String.IsNullOrWhiteSpace(p.Artist) ? "Play a song in Localify to see it here" : p.Artist;
            album.Text = String.IsNullOrWhiteSpace(p.Album) ? "Local music library" : p.Album;
            playback.Text = p.Playing ? "●  PLAYING" : (String.IsNullOrWhiteSpace(p.Song) ? "●  WAITING FOR LOCALIFY" : "Ⅱ  PAUSED");
            playback.ForeColor = p.Playing ? Color.FromArgb(111, 225, 169) : Muted;

            changingControls = true;
            try
            {
                double ratio = p.Duration > 0 ? Math.Max(0, Math.Min(1, p.Position / p.Duration)) : 0;
                if (!seekDragging) seek.Value = (int)Math.Round(ratio * seek.Maximum);
                elapsed.Text = FormatTime(p.Position);
                duration.Text = FormatTime(p.Duration);
                int vol = (int)Math.Round(Math.Max(0, Math.Min(1, p.Volume)) * 100);
                if (vol >= volume.Minimum && vol <= volume.Maximum) volume.Value = vol;
            }
            finally { changingControls = false; }

            string coverKey = !String.IsNullOrEmpty(p.LocalCoverKey) ? p.LocalCoverKey : p.CoverUrl;
            if (!String.Equals(coverKey, artworkKey, StringComparison.Ordinal))
            {
                artworkKey = coverKey ?? "";
                LoadArtwork(p, artworkKey);
            }
            if (TopMost != s.KeepOnTop) TopMost = s.KeepOnTop;
        }

        private void LoadArtwork(PresenceData p, string key)
        {
            if (!String.IsNullOrWhiteSpace(p.LocalCoverData) && p.LocalCoverData.StartsWith("data:image/", StringComparison.OrdinalIgnoreCase))
            {
                try
                {
                    int comma = p.LocalCoverData.IndexOf(',');
                    if (comma > 0)
                    {
                        byte[] bytes = Convert.FromBase64String(p.LocalCoverData.Substring(comma + 1));
                        using (MemoryStream ms = new MemoryStream(bytes))
                        using (Image original = Image.FromStream(ms))
                        {
                            Image clone = new Bitmap(original);
                            ReplaceArtwork(clone);
                            return;
                        }
                    }
                }
                catch { }
            }
            Uri uri;
            if (!String.IsNullOrWhiteSpace(key) && Uri.TryCreate(key, UriKind.Absolute, out uri) &&
                (uri.Scheme == Uri.UriSchemeHttp || uri.Scheme == Uri.UriSchemeHttps))
            {
                ThreadPool.QueueUserWorkItem(delegate
                {
                    Image downloaded = null;
                    try
                    {
                        HttpWebRequest req = (HttpWebRequest)WebRequest.Create(uri);
                        req.Timeout = 6000;
                        req.ReadWriteTimeout = 6000;
                        req.UserAgent = "Localify Discord Connector/6.0";
                        using (HttpWebResponse resp = (HttpWebResponse)req.GetResponse())
                        using (Stream stream = resp.GetResponseStream())
                        using (MemoryStream ms = new MemoryStream())
                        {
                            byte[] buffer = new byte[8192];
                            int n;
                            while ((n = stream.Read(buffer, 0, buffer.Length)) > 0)
                            {
                                if (ms.Length + n > 8 * 1024 * 1024) break;
                                ms.Write(buffer, 0, n);
                            }
                            ms.Position = 0;
                            using (Image original = Image.FromStream(ms)) downloaded = new Bitmap(original);
                        }
                    }
                    catch { }
                    if (downloaded != null && !IsDisposed && IsHandleCreated)
                    {
                        try
                        {
                            BeginInvoke((MethodInvoker)delegate
                            {
                                if (String.Equals(artworkKey, key, StringComparison.Ordinal))
                                    ReplaceArtwork(downloaded);
                                else downloaded.Dispose();
                            });
                        }
                        catch { downloaded.Dispose(); }
                    }
                });
                return;
            }
            ReplaceArtwork(MakeFallbackArt());
        }

        private void ReplaceArtwork(Image image)
        {
            Image previous = art.Image;
            art.Image = image;
            if (previous != null && previous != fallbackArt) previous.Dispose();
        }

        private static string FormatTime(double seconds)
        {
            if (Double.IsNaN(seconds) || Double.IsInfinity(seconds) || seconds < 0) seconds = 0;
            int s = (int)Math.Floor(seconds);
            return s >= 3600 ? String.Format("{0}:{1:00}:{2:00}", s / 3600, (s / 60) % 60, s % 60) : String.Format("{0}:{1:00}", s / 60, s % 60);
        }

        private static void OpenDiscord()
        {
            try { Process.Start(new ProcessStartInfo("discord://") { UseShellExecute = true }); }
            catch
            {
                try { Process.Start(new ProcessStartInfo("https://discord.com/app") { UseShellExecute = true }); }
                catch { }
            }
        }

        private void OpenSettings()
        {
            using (SettingsForm form = new SettingsForm(service, OpenDiscord))
            {
                form.ShowDialog(this);
            }
            RefreshFromService();
        }

        private void CopyDetails()
        {
            try
            {
                Clipboard.SetText(service.Diagnostics());
                MessageBox.Show(this, "Connection details copied to the clipboard.", "Localify Discord Connector", MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
            catch { MessageBox.Show(this, service.Diagnostics(), "Connection details", MessageBoxButtons.OK, MessageBoxIcon.Information); }
        }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            refreshTimer.Stop();
            base.OnFormClosing(e);
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                refreshTimer.Dispose();
                if (art.Image != null && art.Image != fallbackArt) art.Image.Dispose();
                if (fallbackArt != null) fallbackArt.Dispose();
            }
            base.Dispose(disposing);
        }
    }

    internal sealed class SettingsForm : Form
    {
        private static readonly Color Bg = Color.FromArgb(9, 12, 17);
        private static readonly Color Panel = Color.FromArgb(18, 24, 32);
        private static readonly Color Line = Color.FromArgb(42, 52, 66);
        private static readonly Color Muted = Color.FromArgb(145, 158, 176);
        private static readonly Color Accent = Color.FromArgb(94, 174, 247);
        private readonly ConnectorService service;
        private readonly Action openDiscord;
        private TextBox appId;
        private CheckBox enabled;
        private CheckBox topMost;

        public SettingsForm(ConnectorService service, Action openDiscord)
        {
            this.service = service;
            this.openDiscord = openDiscord;
            Text = "Localify Connector Settings";
            StartPosition = FormStartPosition.CenterParent;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            ShowInTaskbar = false;
            Size = new Size(620, 435);
            BackColor = Bg;
            ForeColor = Color.White;
            Font = new Font("Segoe UI", 9F);
            BuildUi();
        }

        private void BuildUi()
        {
            Panel intro = new Panel { Location = new Point(18, 16), Size = new Size(570, 72), BackColor = Panel };
            Controls.Add(intro);
            Label heading = LabelAt("Account and connection", 16, 12, 420, 28, 15, Color.White, true);
            intro.Controls.Add(heading);
            Label note = LabelAt("The connector runs locally and talks to the Discord desktop app.", 16, 40, 520, 18, 9, Muted, false);
            intro.Controls.Add(note);

            Label appLabel = LabelAt("DISCORD APPLICATION ID", 22, 105, 300, 18, 8, Accent, true);
            Controls.Add(appLabel);
            appId = new TextBox { Location = new Point(22, 128), Size = new Size(560, 29), Text = service.Snapshot().ClientId, BackColor = Color.FromArgb(14, 19, 26), ForeColor = Color.White, BorderStyle = BorderStyle.FixedSingle, Font = new Font("Consolas", 11F) };
            Controls.Add(appId);
            Label explanation = LabelAt("Paste the numeric Application ID from Discord Developer Portal → General Information.", 22, 160, 555, 30, 9, Muted, false);
            Controls.Add(explanation);

            enabled = new CheckBox { Text = "Enable Discord Rich Presence", Location = new Point(22, 202), AutoSize = true, Checked = service.Snapshot().Enabled, ForeColor = Color.White, BackColor = Bg };
            Controls.Add(enabled);
            topMost = new CheckBox { Text = "Keep the connector above other windows", Location = new Point(22, 232), AutoSize = true, Checked = service.Snapshot().KeepOnTop, ForeColor = Color.White, BackColor = Bg };
            Controls.Add(topMost);

            Button open = ButtonAt("Open Discord", 22, 270, 125, false);
            open.Click += delegate { openDiscord(); };
            Controls.Add(open);
            Button developer = ButtonAt("Developer Portal", 156, 270, 136, false);
            developer.Click += delegate
            {
                try { Process.Start(new ProcessStartInfo("https://discord.com/developers/applications") { UseShellExecute = true }); }
                catch { }
            };
            Controls.Add(developer);
            Button diagnostics = ButtonAt("Copy diagnostics", 301, 270, 145, false);
            diagnostics.Click += delegate
            {
                try { Clipboard.SetText(service.Diagnostics()); MessageBox.Show(this, "Connection details copied.", "Localify", MessageBoxButtons.OK, MessageBoxIcon.Information); }
                catch { }
            };
            Controls.Add(diagnostics);

            Button defaultId = ButtonAt("Use built-in ID", 22, 337, 130, false);
            defaultId.Click += delegate { appId.Text = ConnectorService.DefaultClientId; };
            Controls.Add(defaultId);
            Button cancel = ButtonAt("Cancel", 365, 337, 92, false);
            cancel.Click += delegate { DialogResult = DialogResult.Cancel; Close(); };
            Controls.Add(cancel);
            Button save = ButtonAt("Save & reconnect", 464, 337, 118, true);
            save.Click += delegate
            {
                try
                {
                    service.SaveSettings(appId.Text, enabled.Checked, topMost.Checked);
                    DialogResult = DialogResult.OK;
                    Close();
                }
                catch (Exception ex)
                {
                    MessageBox.Show(this, ex.Message, "Check the Application ID", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                }
            };
            Controls.Add(save);
        }

        private Label LabelAt(string text, int x, int y, int w, int h, float size, Color color, bool bold)
        {
            return new Label { Text = text, Location = new Point(x, y), Size = new Size(w, h), ForeColor = color, BackColor = Color.Transparent, Font = new Font("Segoe UI", size, bold ? FontStyle.Bold : FontStyle.Regular), AutoEllipsis = true };
        }

        private Button ButtonAt(string text, int x, int y, int w, bool primary)
        {
            Button b = new Button { Text = text, Location = new Point(x, y), Size = new Size(w, 36), FlatStyle = FlatStyle.Flat, BackColor = primary ? Accent : Panel, ForeColor = primary ? Color.FromArgb(7, 16, 25) : Color.White, Font = new Font("Segoe UI", 8.2F, FontStyle.Bold), Cursor = Cursors.Hand };
            b.FlatAppearance.BorderSize = 1;
            b.FlatAppearance.BorderColor = primary ? Accent : Line;
            return b;
        }
    }
}
