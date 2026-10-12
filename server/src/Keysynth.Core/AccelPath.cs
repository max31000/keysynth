using System.Text;

namespace Keysynth.Core;

/// <summary>
/// Builds the <c>X-Accel-Redirect</c> target for a sample file (cloud-api.md §3.2): <c>/keysynth/_files/&lt;lib&gt;/&lt;path&gt;</c>
/// with every segment percent-encoded (UTF-8, uppercase hex); only RFC 3986 unreserved characters
/// (<c>A-Z a-z 0-9 - . _ ~</c>) and the '/' between segments stay literal.
/// </summary>
public static class AccelPath
{
    public const string FilesPrefix = "/keysynth/_files/";

    /// <param name="lib">A valid library id.</param>
    /// <param name="path">A valid relative path (<see cref="SamplePaths.IsValidPath"/>).</param>
    public static string For(string lib, string path)
    {
        var sb = new StringBuilder(FilesPrefix.Length + lib.Length + path.Length * 3);
        sb.Append(FilesPrefix);
        AppendSegment(sb, lib);
        foreach (var segment in path.Split('/'))
        {
            sb.Append('/');
            AppendSegment(sb, segment);
        }
        return sb.ToString();
    }

    public static string EncodeSegment(string segment)
    {
        var sb = new StringBuilder(segment.Length * 3);
        AppendSegment(sb, segment);
        return sb.ToString();
    }

    private static void AppendSegment(StringBuilder sb, string segment)
    {
        const string hex = "0123456789ABCDEF";
        foreach (var b in Encoding.UTF8.GetBytes(segment))
        {
            var c = (char)b;
            if (c is >= 'A' and <= 'Z' or >= 'a' and <= 'z' or >= '0' and <= '9' or '-' or '.' or '_' or '~')
            {
                sb.Append(c);
            }
            else
            {
                sb.Append('%').Append(hex[b >> 4]).Append(hex[b & 0xF]);
            }
        }
    }
}
