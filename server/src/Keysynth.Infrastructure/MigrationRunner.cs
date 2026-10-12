using System.Globalization;
using System.Reflection;
using System.Text.RegularExpressions;
using Dapper;
using Microsoft.Extensions.Logging;
using MySqlConnector;

namespace Keysynth.Infrastructure;

/// <summary>
/// Applies embedded <c>Migrations/NNN_*.sql</c> in version order on startup and records them in
/// <c>schema_migrations(version, applied_at)</c> (cloud-api.md §5.2). Pattern: bonds MigrationRunner.
/// MySQL DDL is not transactional: a failed migration must be fixed forward.
/// </summary>
public sealed partial class MigrationRunner(string connectionString, ILogger<MigrationRunner> logger)
{
    [GeneratedRegex(@"\.Migrations\.(\d{3})_[A-Za-z0-9_]+\.sql$", RegexOptions.CultureInvariant)]
    private static partial Regex ResourcePattern();

    public async Task RunAsync(CancellationToken ct = default)
    {
        await using var conn = new MySqlConnection(connectionString);
        await conn.OpenAsync(ct);

        await conn.ExecuteAsync("""
            CREATE TABLE IF NOT EXISTS schema_migrations (
              version     INT          NOT NULL PRIMARY KEY,
              applied_at  DATETIME(3)  NOT NULL
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
            """);

        var applied = (await conn.QueryAsync<int>("SELECT version FROM schema_migrations")).ToHashSet();

        foreach (var (version, resource) in Discover())
        {
            if (applied.Contains(version))
                continue;

            logger.LogInformation("Applying migration {Version} ({Resource})", version, resource);
            await using var stream = typeof(MigrationRunner).Assembly.GetManifestResourceStream(resource)
                ?? throw new InvalidOperationException($"migration resource {resource} missing");
            using var reader = new StreamReader(stream);
            var sql = await reader.ReadToEndAsync(ct);

            foreach (var statement in SplitSqlStatements(sql))
                await conn.ExecuteAsync(new CommandDefinition(statement, cancellationToken: ct));

            await conn.ExecuteAsync(
                "INSERT INTO schema_migrations (version, applied_at) VALUES (@version, UTC_TIMESTAMP(3))",
                new { version });
        }
    }

    internal static IReadOnlyList<(int Version, string Resource)> Discover()
    {
        var list = new List<(int, string)>();
        foreach (var name in typeof(MigrationRunner).Assembly.GetManifestResourceNames())
        {
            var m = ResourcePattern().Match(name);
            if (m.Success)
                list.Add((int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture), name));
        }
        list.Sort((a, b) => a.Item1.CompareTo(b.Item1));
        for (var i = 1; i < list.Count; i++)
            if (list[i].Item1 == list[i - 1].Item1)
                throw new InvalidOperationException($"duplicate migration version {list[i].Item1}");
        return list;
    }

    /// <summary>Splits on ';' after removing "--" line comments (migrations contain only DDL / simple DML).</summary>
    internal static List<string> SplitSqlStatements(string sql)
    {
        var withoutComments = string.Join('\n', sql.Split('\n').Select(line =>
        {
            var idx = line.IndexOf("--", StringComparison.Ordinal);
            return idx < 0 ? line : line[..idx];
        }));
        return withoutComments.Split(';')
            .Select(s => s.Trim())
            .Where(s => s.Length > 0)
            .ToList();
    }
}
