using System.Buffers;
using System.Text.Encodings.Web;
using System.Text.Json;

namespace Keysynth.Api.Relay;

/// <summary>Serialised server frames (UTF-8 JSON text). The inner <c>msg</c> is copied as-is (the hub adds nothing).</summary>
internal static class RelayFrames
{
    public static readonly ReadOnlyMemory<byte> Ping = """{"t":"ping"}"""u8.ToArray();
    public static readonly ReadOnlyMemory<byte> Pong = """{"t":"pong"}"""u8.ToArray();

    // Not embedded in HTML: non-ASCII names stay as UTF-8.
    private static readonly JsonWriterOptions WriterOptions = new() { Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping };

    private static byte[] Build(Action<Utf8JsonWriter> body)
    {
        var buffer = new ArrayBufferWriter<byte>(256);
        using (var w = new Utf8JsonWriter(buffer, WriterOptions))
        {
            w.WriteStartObject();
            body(w);
            w.WriteEndObject();
        }
        return buffer.WrittenSpan.ToArray();
    }

    /// <summary>server → controller <c>{t:"msg", host, msg}</c>.</summary>
    public static byte[] FromHost(string deviceId, JsonElement msg) => Build(w =>
    {
        w.WriteString("t", "msg");
        w.WriteString("host", deviceId);
        w.WritePropertyName("msg");
        msg.WriteTo(w);
    });

    /// <summary>server → host <c>{t:"msg", from, msg}</c>.</summary>
    public static byte[] FromController(string cid, JsonElement msg) => Build(w =>
    {
        w.WriteString("t", "msg");
        w.WriteString("from", cid);
        w.WritePropertyName("msg");
        msg.WriteTo(w);
    });

    /// <summary>server → host <c>{t:"controller", cid, joined}</c>.</summary>
    public static byte[] Controller(string cid, bool joined) => Build(w =>
    {
        w.WriteString("t", "controller");
        w.WriteString("cid", cid);
        w.WriteBoolean("joined", joined);
    });

    /// <summary>server → host <c>{t:"pause"|"resume", cid}</c>.</summary>
    public static byte[] Pause(string cid, bool paused) => Build(w =>
    {
        w.WriteString("t", paused ? "pause" : "resume");
        w.WriteString("cid", cid);
    });

    /// <summary>server → any <c>{t:"error", code, host?, id?}</c> (id = the inner request id, when there was one).</summary>
    public static byte[] Error(string code, string? host = null, JsonElement? id = null) => Build(w =>
    {
        w.WriteString("t", "error");
        w.WriteString("code", code);
        if (host is not null)
            w.WriteString("host", host);
        if (id is { } v)
        {
            w.WritePropertyName("id");
            v.WriteTo(w);
        }
    });

    public sealed record HostEntry(string DeviceId, string Name, string Version, bool Online);

    /// <summary>server → controller <c>{t:"hosts", hosts:[{deviceId,name,version,online}]}</c>.</summary>
    public static byte[] Hosts(IEnumerable<HostEntry> hosts) => Build(w =>
    {
        w.WriteString("t", "hosts");
        w.WriteStartArray("hosts");
        foreach (var h in hosts)
        {
            w.WriteStartObject();
            w.WriteString("deviceId", h.DeviceId);
            w.WriteString("name", h.Name);
            w.WriteString("version", h.Version);
            w.WriteBoolean("online", h.Online);
            w.WriteEndObject();
        }
        w.WriteEndArray();
    });
}
