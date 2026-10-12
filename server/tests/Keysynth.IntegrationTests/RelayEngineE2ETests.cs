using System.Diagnostics;
using System.Net;
using FluentAssertions;
using Keysynth.IntegrationTests.Infrastructure;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Hosting.Server;
using Microsoft.AspNetCore.Hosting.Server.Features;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Xunit;
using Xunit.Abstractions;

namespace Keysynth.IntegrationTests;

/// <summary>
/// The test API (in-memory stores, stub JWKS) additionally listening on a real loopback port, so an external process
/// (the engine) can reach it. Pattern: build the TestServer host, then a second host from the same builder with Kestrel.
/// Both share the factory's store and clock; only the Kestrel one is used by the engine.
/// </summary>
public sealed class KestrelFactory : KeysynthFactory
{
    private IHost? _kestrel;

    public string Address { get; private set; } = "";

    protected override IHost CreateHost(IHostBuilder builder)
    {
        var testHost = builder.Build();
        builder.ConfigureWebHost(b => b.UseKestrel(k => k.Listen(IPAddress.Loopback, 0)));
        _kestrel = builder.Build();
        _kestrel.Start();
        Address = _kestrel.Services.GetRequiredService<IServer>().Features.Get<IServerAddressesFeature>()!.Addresses.First();
        testHost.Start();
        return testHost;
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _kestrel?.StopAsync().GetAwaiter().GetResult();
            _kestrel?.Dispose();
        }
        base.Dispose(disposing);
    }
}

/// <summary>
/// Engine ⇄ real relay hub: runs <c>scripts/dev_relay_e2e.py --server …</c> (engine --no-audio on alternate ports +
/// two cookie-authenticated controllers) against <see cref="KestrelFactory"/>. Needs a relay-capable engine build:
/// <c>KS_TESTS_ENGINE=&lt;path to keysynth-engine.exe&gt;</c> and Python with <c>websockets</c>; skipped otherwise.
/// </summary>
public sealed class RelayEngineE2ETests(ITestOutputHelper output)
{
    private static string? RepoRoot()
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
            if (File.Exists(Path.Combine(dir.FullName, "scripts", "dev_relay_e2e.py")))
                return dir.FullName;
        return null;
    }

    [SkippableFact]
    public async Task EngineHostsThroughTheRealHub()
    {
        var engine = Environment.GetEnvironmentVariable("KS_TESTS_ENGINE");
        Skip.If(string.IsNullOrEmpty(engine) || !File.Exists(engine), "KS_TESTS_ENGINE not set");
        var repo = RepoRoot();
        Skip.If(repo is null, "scripts/dev_relay_e2e.py not found");

        using var f = new KestrelFactory();
        f.WriteIndex("""{"format":1,"groups":[],"libraries":[]}""");
        _ = f.Server; // starts both hosts
        var tg = f.NewTelegramId();
        var cookie = $"{Mvv42TestAuth.CookieName}={Mvv42TestAuth.CreateToken(tg, lifetime: TimeSpan.FromMinutes(30))}";

        var psi = new ProcessStartInfo("python")
        {
            WorkingDirectory = repo!,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        foreach (var arg in new[]
                 {
                     "-I", Path.Combine(repo!, "scripts", "dev_relay_e2e.py"), "--engine", engine!, "--ws-port", "7357",
                     "--server", f.Address, "--cookie", cookie, "--origin", Mvv42TestAuth.AllowedOrigin,
                 })
            psi.ArgumentList.Add(arg);
        using var p = Process.Start(psi)!;
        var stdout = p.StandardOutput.ReadToEndAsync();
        var stderr = p.StandardError.ReadToEndAsync();
        using var cts = new CancellationTokenSource(TimeSpan.FromMinutes(3));
        try
        {
            await p.WaitForExitAsync(cts.Token);
        }
        catch (OperationCanceledException)
        {
            p.Kill(entireProcessTree: true);
            throw;
        }
        output.WriteLine(await stdout);
        output.WriteLine(await stderr);
        p.ExitCode.Should().Be(0, await stdout);
    }
}
